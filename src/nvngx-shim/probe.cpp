// nvngx_dlssnr.dll 探针桩（P0）
//
// 目的：不实现任何降噪，只把 Magpie 的调用序列、参数字典、资源指针打进日志，
//       用来确认 ABI 与源码读码结论是否一致。风险最低的一步。
//
// 设计要点（都来自 docs/nvngx-slot.md）：
//   1. 参数对象由本 DLL 自己 new 并返回（NVSDK_NGX_D3D12_AllocateParameters），
//      所以 vtable 顺序完全由我们决定 —— 故意定义 32 个"什么都不懂"的槽位，
//      每个槽位只记录 (slot, name, arg)，再从日志反推真实布局。不需要 NVIDIA 头文件。
//   2. 必须静态导入 GetModuleFileNameW：Magpie 会去本 DLL 的 IAT 里找它并挂钩
//      （InstallSnippetCallerCompatibility），找不到就判定"签名片段不完整"直接放弃。
//   3. 成功码默认 1（NVSDK_NGX_Result_Success），可用环境变量 NGXSHIM_RESULT 覆盖，
//      避免因为枚举值猜错而要重新编译。
//
// 日志：<DLL 所在目录>\nvngx-shim.log，追加写，带毫秒时间戳与线程号。

#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>

namespace {

std::mutex g_logMutex;
HANDLE g_logFile = INVALID_HANDLE_VALUE;
char g_logPath[MAX_PATH * 2]{};

int g_successCode = 1;
long g_callSerial = 0;

void ResolveLogPath() {
	// DLL 自己的路径（顺带保证 GetModuleFileNameW 是一个真实导入：
	// Magpie 的调用方兼容钩子要求本模块的 IAT 里有这一项）。
	wchar_t dllPath[MAX_PATH]{};
	GetModuleFileNameW(GetModuleHandleW(L"nvngx_dlssnr.dll"), dllPath, MAX_PATH);
	std::wstring dir(dllPath);
	const size_t slash = dir.find_last_of(L"\\/");
	if (slash != std::wstring::npos) dir.resize(slash + 1);
	else dir.clear();
	const std::wstring full = dir + L"nvngx-shim.log";
	WideCharToMultiByte(CP_UTF8, 0, full.c_str(), -1, g_logPath, sizeof(g_logPath), nullptr, nullptr);

	g_logFile = CreateFileW(full.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
		nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);

	// 空文件先写 UTF-8 BOM：日志里混着中文，没 BOM 时记事本会按 ANSI 解，直接变乱码。
	if (g_logFile != INVALID_HANDLE_VALUE) {
		LARGE_INTEGER size{};
		if (GetFileSizeEx(g_logFile, &size) && size.QuadPart == 0) {
			DWORD written = 0;
			WriteFile(g_logFile, "\xEF\xBB\xBF", 3, &written, nullptr);
		}
	}
}

void Log(const char* fmt, ...) {
	if (g_logFile == INVALID_HANDLE_VALUE) return;
	SYSTEMTIME st{};
	GetLocalTime(&st);

	char body[1024]{};
	va_list args;
	va_start(args, fmt);
	_vsnprintf_s(body, sizeof(body) - 1, _TRUNCATE, fmt, args);
	va_end(args);

	char line[1200]{};
	const int n = _snprintf_s(line, sizeof(line), _TRUNCATE,
		"%04u-%02u-%02u %02u:%02u:%02u.%03u [tid %5lu] %s\r\n",
		st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
		GetCurrentThreadId(), body);

	std::lock_guard<std::mutex> guard(g_logMutex);
	if (n > 0) {
		DWORD written = 0;
		WriteFile(g_logFile, line, (DWORD)n, &written, nullptr);
	}
}

// ---------------------------------------------------------------- 参数对象
// 故意不实现任何语义：32 个槽位，签名统一成 (const char*, void*, void*)。
// 调用方按自己的 vtable 布局调用，落到哪个槽位就记哪个 —— 布局由日志反推。
class ProbeParameters {
public:
	ProbeParameters() {
		Log("PROBE: parameter object created at %p (%u slots)", (void*)this, kSlots);
	}
	virtual ~ProbeParameters() = default;

#define PROBE_SLOT(i) \
	virtual void Slot##i(const char* name, void* a, void* b) { \
		Log("PARAM: slot=%2d name=\"%s\" a=%p b=%p", i, name ? name : "(null)", a, b); \
	}

	PROBE_SLOT(0)  PROBE_SLOT(1)  PROBE_SLOT(2)  PROBE_SLOT(3)
	PROBE_SLOT(4)  PROBE_SLOT(5)  PROBE_SLOT(6)  PROBE_SLOT(7)
	PROBE_SLOT(8)  PROBE_SLOT(9)  PROBE_SLOT(10) PROBE_SLOT(11)
	PROBE_SLOT(12) PROBE_SLOT(13) PROBE_SLOT(14) PROBE_SLOT(15)
	PROBE_SLOT(16) PROBE_SLOT(17) PROBE_SLOT(18) PROBE_SLOT(19)
	PROBE_SLOT(20) PROBE_SLOT(21) PROBE_SLOT(22) PROBE_SLOT(23)
	PROBE_SLOT(24) PROBE_SLOT(25) PROBE_SLOT(26) PROBE_SLOT(27)
	PROBE_SLOT(28) PROBE_SLOT(29) PROBE_SLOT(30) PROBE_SLOT(31)

#undef PROBE_SLOT

	static constexpr unsigned kSlots = 32;
};

struct ProbeHandle {
	uint32_t magic = 0x4E525052;  // 'NRPR'
	int featureId = -1;
};

int Success() {
	return g_successCode;
}

const char* FeatureName(int feature) {
	switch (feature) {
	case 0: return "eDLSS";
	case 1: return "eDLFG";
	case 2: return "eNR?";
	case 18: return "CoreFeature18(NR)";
	default: return "unknown";
	}
}

}  // namespace

// ---------------------------------------------------------------- 导出
extern "C" {

__declspec(dllexport) int NVSDK_NGX_D3D12_Init_Ext(
	unsigned long long appId, const wchar_t* path, void* device,
	unsigned int version, const void* params) {
	Log("CALL Init_Ext appId=0x%llX path=\"%ls\" device=%p version=0x%X params=%p",
		appId, path ? path : L"(null)", device, version, params);
	return Success();
}

__declspec(dllexport) int NVSDK_NGX_D3D12_Init_with_ProjectID(
	const char* projectId, int engineType, const char* engineVersion, const wchar_t* path,
	void* device, const void* featureInfo, unsigned int version) {
	Log("CALL Init_with_ProjectID project=\"%s\" engineType=%d engineVersion=\"%s\" path=\"%ls\" device=%p version=0x%X",
		projectId ? projectId : "(null)", engineType, engineVersion ? engineVersion : "(null)",
		path ? path : L"(null)", device, version);
	return Success();
}

__declspec(dllexport) int NVSDK_NGX_D3D12_AllocateParameters(void** out) {
	Log("CALL AllocateParameters out=%p", out);
	if (!out) return 0xBAD00000;  // FAIL_InvalidParameter
	*out = new ProbeParameters();
	return Success();
}

__declspec(dllexport) int NVSDK_NGX_D3D12_GetCapabilityParameters(void** out) {
	Log("CALL GetCapabilityParameters out=%p", out);
	if (!out) return 0xBAD00000;
	*out = new ProbeParameters();
	return Success();
}

__declspec(dllexport) int NVSDK_NGX_D3D12_DestroyParameters(void* params) {
	Log("CALL DestroyParameters params=%p", params);
	// 探针不 delete：泄漏一个对象无所谓，反倒能避免"Magpie 还在用时被释放"的悬空。
	return Success();
}

__declspec(dllexport) int NVSDK_NGX_D3D12_CreateFeature(
	void* cmdList, int feature, void* params, void** handle) {
	const long serial = ++g_callSerial;
	Log("CALL CreateFeature #%ld cmdList=%p feature=%d(%s) params=%p handle=%p",
		serial, cmdList, feature, FeatureName(feature), params, handle);
	if (handle) {
		auto* h = new ProbeHandle();
		h->featureId = feature;
		*handle = h;
	}
	return Success();
}

__declspec(dllexport) int NVSDK_NGX_D3D12_EvaluateFeature(
	void* cmdList, const void* handle, const void* params, void* callback) {
	Log("CALL EvaluateFeature cmdList=%p handle=%p feature=%d params=%p callback=%p",
		cmdList, handle, handle ? static_cast<const ProbeHandle*>(handle)->featureId : -1,
		params, callback);
	return Success();
}

__declspec(dllexport) int NVSDK_NGX_D3D12_ReleaseFeature(void* handle) {
	Log("CALL ReleaseFeature handle=%p", handle);
	delete static_cast<ProbeHandle*>(handle);
	return Success();
}

__declspec(dllexport) int NVSDK_NGX_D3D12_Shutdown(void* device) {
	Log("CALL Shutdown device=%p", device);
	return Success();
}

__declspec(dllexport) int NVSDK_NGX_D3D12_Shutdown1(void* device) {
	Log("CALL Shutdown1 device=%p", device);
	return Success();
}

__declspec(dllexport) int NVSDK_NGX_D3D12_GetFeatureRequirements(
	void* device, const void* desc, void* requirements) {
	Log("CALL GetFeatureRequirements device=%p desc=%p requirements=%p", device, desc, requirements);
	return Success();
}

__declspec(dllexport) int NVSDK_NGX_D3D12_GetScratchBufferSize(
	int feature, const void* params, size_t* size) {
	Log("CALL GetScratchBufferSize feature=%d params=%p size=%p", feature, params, size);
	if (size) *size = 0;
	return Success();
}

__declspec(dllexport) int NVSDK_NGX_D3D12_PopulateParameters_Impl(void* params) {
	Log("CALL PopulateParameters_Impl params=%p", params);
	return Success();
}

}  // extern "C"

// ---------------------------------------------------------------- DllMain
BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
	if (reason == DLL_PROCESS_ATTACH) {
		DisableThreadLibraryCalls(module);
		ResolveLogPath();

		char env[32]{};
		if (GetEnvironmentVariableA("NGXSHIM_RESULT", env, sizeof(env)) > 0) {
			g_successCode = (int)strtol(env, nullptr, 0);
		}

		wchar_t self[MAX_PATH]{};
		GetModuleFileNameW(module, self, MAX_PATH);  // 兼作 GetModuleFileNameW 导入锚点
		Log("PROBE: loaded as \"%ls\" success=0x%X", self, g_successCode);
		Log("PROBE: 这是探针桩，不做任何降噪；日志只用来核对调用序列与参数键");
	} else if (reason == DLL_PROCESS_DETACH) {
		Log("PROBE: unloaded");
		if (g_logFile != INVALID_HANDLE_VALUE) {
			CloseHandle(g_logFile);
			g_logFile = INVALID_HANDLE_VALUE;
		}
	}
	return TRUE;
}
