// amdgx_dlssnr.dll — P1 直通 shim
//
// 在 P0 探针（probe.cpp）的 ABI 骨架上，把 EvaluateFeature 变成真正的 GPU 拷贝：
//   DLSSNR.Color  --CopyResource-->  DLSSNR.Output
// 目的：验证"DLSSNR 槽位成立"——画面正常、面板/多趟/抗闪烁都还能用。
//
// 资源状态契约（来自宿主 DLSSNRFilter.cpp 实测源码）：
//   EvaluateFeature 进入时 Color = NON_PIXEL_SHADER_RESOURCE(0x40)
//                            Output = UNORDERED_ACCESS(0x8)
//   返回时宿主期望状态不变（返回后宿主做 0x40→COMMON / 0x8→COMMON）。
//   所以 shim 内部：0x40→COPY_SOURCE、0x8→COPY_DEST、拷贝、再转回。
//
// 其余 ABI 行为与 P0 相同；日志降频（每 600 帧 1 条 + 事件行），避免 10 万行日志。
// 仍然静态导入 GetModuleFileNameW（宿主 IAT 钩子要求）。

#include <windows.h>
#include <d3d12.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <string>

namespace {

std::mutex g_logMutex;
HANDLE g_logFile = INVALID_HANDLE_VALUE;
char g_logPath[MAX_PATH * 2]{};

int g_successCode = 1;
long g_callSerial = 0;
long g_evalCount = 0;
long g_copyFailures = 0;

void ResolveLogPath() {
	wchar_t dllPath[MAX_PATH]{};
	GetModuleFileNameW(GetModuleHandleW(L"amdgx_dlssnr.dll"), dllPath, MAX_PATH);
	std::wstring dir(dllPath);
	const size_t slash = dir.find_last_of(L"\\/");
	if (slash != std::wstring::npos) dir.resize(slash + 1);
	else dir.clear();
	const std::wstring full = dir + L"nvngx-shim.log";
	WideCharToMultiByte(CP_UTF8, 0, full.c_str(), -1, g_logPath, sizeof(g_logPath), nullptr, nullptr);
	g_logFile = CreateFileW(full.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
		nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
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
// P0 已探明：宿主按 vtable 槽位 Set(name, value)，值在 a（b 为伴随值）。
// P1 按名字存值，EvaluateFeature 时取 Color/Output 指针。
class PassthroughParameters {
public:
	PassthroughParameters() {
		Log("P1: parameter object created at %p (%u slots)", (void*)this, kSlots);
	}
	virtual ~PassthroughParameters() = default;

	void Store(unsigned slot, const char* name, void* a, void* b) {
		if (!name) return;
		std::lock_guard<std::mutex> guard(g_valuesMutex);
		auto& entry = g_values[std::string(name)];
		if (entry.first == nullptr && a != nullptr) {
			Log("P1: %s = %p (slot %u, first)", name, a, slot);
		}
		entry = { a, b };
	}

	// Get(name) -> a value
	void* Get(const char* name) {
		std::lock_guard<std::mutex> guard(g_valuesMutex);
		auto it = g_values.find(name);
		return it == g_values.end() ? nullptr : it->second.first;
	}

#define P1_SLOT(i) \
	virtual void Slot##i(const char* name, void* a, void* b) { Store(i, name, a, b); }

	P1_SLOT(0)  P1_SLOT(1)  P1_SLOT(2)  P1_SLOT(3)
	P1_SLOT(4)  P1_SLOT(5)  P1_SLOT(6)  P1_SLOT(7)
	P1_SLOT(8)  P1_SLOT(9)  P1_SLOT(10) P1_SLOT(11)
	P1_SLOT(12) P1_SLOT(13) P1_SLOT(14) P1_SLOT(15)
	P1_SLOT(16) P1_SLOT(17) P1_SLOT(18) P1_SLOT(19)
	P1_SLOT(20) P1_SLOT(21) P1_SLOT(22) P1_SLOT(23)
	P1_SLOT(24) P1_SLOT(25) P1_SLOT(26) P1_SLOT(27)
	P1_SLOT(28) P1_SLOT(29) P1_SLOT(30) P1_SLOT(31)

#undef P1_SLOT

	static constexpr unsigned kSlots = 32;

private:
	static std::mutex g_valuesMutex;
	static std::map<std::string, std::pair<void*, void*>> g_values;
};

std::mutex PassthroughParameters::g_valuesMutex;
std::map<std::string, std::pair<void*, void*>> PassthroughParameters::g_values;

struct PassthroughHandle {
	uint32_t magic = 0x4E525031;  // 'NRP1'
	int featureId = -1;
};

const char* FeatureName(int feature) {
	switch (feature) {
	case 18: return "CoreFeature18(NR)";
	default: return "unknown";
	}
}

void Transition(ID3D12GraphicsCommandList* list, ID3D12Resource* res,
	D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
	D3D12_RESOURCE_BARRIER barrier{};
	barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	barrier.Transition = { res, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, before, after };
	list->ResourceBarrier(1, &barrier);
}

}  // namespace

extern "C" {

__declspec(dllexport) int NVSDK_NGX_D3D12_Init_Ext(
	unsigned long long appId, const wchar_t* path, void* device,
	unsigned int version, const void* params) {
	Log("CALL Init_Ext appId=0x%llX path=\"%ls\" device=%p version=0x%X",
		appId, path ? path : L"(null)", device, version);
	return g_successCode;
}

__declspec(dllexport) int NVSDK_NGX_D3D12_Init_with_ProjectID(
	const char* projectId, int engineType, const char* engineVersion, const wchar_t* path,
	void* device, const void* featureInfo, unsigned int version) {
	Log("CALL Init_with_ProjectID project=\"%s\" device=%p version=0x%X",
		projectId ? projectId : "(null)", device, version);
	return g_successCode;
}

__declspec(dllexport) int NVSDK_NGX_D3D12_AllocateParameters(void** out) {
	Log("CALL AllocateParameters out=%p", out);
	if (!out) return 0xBAD00000;
	*out = new PassthroughParameters();
	return g_successCode;
}

__declspec(dllexport) int NVSDK_NGX_D3D12_GetCapabilityParameters(void** out) {
	Log("CALL GetCapabilityParameters out=%p", out);
	if (!out) return 0xBAD00000;
	*out = new PassthroughParameters();
	return g_successCode;
}

__declspec(dllexport) int NVSDK_NGX_D3D12_DestroyParameters(void* params) {
	Log("CALL DestroyParameters params=%p", params);
	// 与 P0 同策略：不 delete，避免宿主仍在使用时悬空。
	return g_successCode;
}

__declspec(dllexport) int NVSDK_NGX_D3D12_CreateFeature(
	void* cmdList, int feature, void* params, void** handle) {
	const long serial = ++g_callSerial;
	Log("CALL CreateFeature #%ld cmdList=%p feature=%d(%s) params=%p",
		serial, cmdList, feature, FeatureName(feature), params);
	if (handle) {
		auto* h = new PassthroughHandle();
		h->featureId = feature;
		*handle = h;
	}
	return g_successCode;
}

__declspec(dllexport) int NVSDK_NGX_D3D12_EvaluateFeature(
	void* cmdList, const void* handle, const void* params, void* callback) {
	auto* pp = static_cast<PassthroughParameters*>(const_cast<void*>(params));
	ID3D12Resource* color = static_cast<ID3D12Resource*>(pp ? pp->Get("DLSSNR.Color") : nullptr);
	ID3D12Resource* output = static_cast<ID3D12Resource*>(pp ? pp->Get("DLSSNR.Output") : nullptr);
	auto* list = static_cast<ID3D12GraphicsCommandList*>(cmdList);

	++g_evalCount;
	const bool verbose = (g_evalCount <= 3) || (g_evalCount % 600 == 0);

	if (!list || !color || !output) {
		++g_copyFailures;
		if (verbose) Log("EVAL #%ld missing: list=%p color=%p output=%p",
			g_evalCount, list, color, output);
		return g_successCode;
	}

	// 状态契约：进 Color=NON_PIXEL_SHADER_RESOURCE(0x40)、Output=UNORDERED_ACCESS(0x8)
	Transition(list, color, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
		D3D12_RESOURCE_STATE_COPY_SOURCE);
	Transition(list, output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
		D3D12_RESOURCE_STATE_COPY_DEST);
	list->CopyResource(output, color);
	Transition(list, color, D3D12_RESOURCE_STATE_COPY_SOURCE,
		D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
	Transition(list, output, D3D12_RESOURCE_STATE_COPY_DEST,
		D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

	if (verbose) {
		D3D12_RESOURCE_DESC ci = color->GetDesc();
		D3D12_RESOURCE_DESC oi = output->GetDesc();
		Log("EVAL #%ld color=%ux%u fmt=%u output=%ux%u fmt=%u (mism=%d)",
			g_evalCount,
			(unsigned)ci.Width, ci.Height, (unsigned)ci.Format,
			(unsigned)oi.Width, oi.Height, (unsigned)oi.Format,
			(ci.Width != oi.Width || ci.Height != oi.Height || ci.Format != oi.Format));
	}
	return g_successCode;
}

__declspec(dllexport) int NVSDK_NGX_D3D12_ReleaseFeature(void* handle) {
	Log("CALL ReleaseFeature handle=%p (total evals=%ld copyFails=%ld)",
		handle, g_evalCount, g_copyFailures);
	delete static_cast<PassthroughHandle*>(handle);
	return g_successCode;
}

__declspec(dllexport) int NVSDK_NGX_D3D12_Shutdown(void* device) {
	Log("CALL Shutdown device=%p", device);
	return g_successCode;
}

__declspec(dllexport) int NVSDK_NGX_D3D12_Shutdown1(void* device) {
	Log("CALL Shutdown1 device=%p", device);
	return g_successCode;
}

__declspec(dllexport) int NVSDK_NGX_D3D12_GetFeatureRequirements(
	void* device, const void* desc, void* requirements) {
	Log("CALL GetFeatureRequirements device=%p", device);
	return g_successCode;
}

__declspec(dllexport) int NVSDK_NGX_D3D12_GetScratchBufferSize(
	int feature, const void* params, size_t* size) {
	Log("CALL GetScratchBufferSize feature=%d", feature);
	if (size) *size = 0;
	return g_successCode;
}

__declspec(dllexport) int NVSDK_NGX_D3D12_PopulateParameters_Impl(void* params) {
	Log("CALL PopulateParameters_Impl params=%p", params);
	return g_successCode;
}

}  // extern "C"

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
		Log("P1: loaded as \"%ls\" success=0x%X (passthrough Color->Output)", self, g_successCode);
	} else if (reason == DLL_PROCESS_DETACH) {
		Log("P1: unloaded (evals=%ld copyFails=%ld)", g_evalCount, g_copyFailures);
		if (g_logFile != INVALID_HANDLE_VALUE) {
			CloseHandle(g_logFile);
			g_logFile = INVALID_HANDLE_VALUE;
		}
	}
	return TRUE;
}
