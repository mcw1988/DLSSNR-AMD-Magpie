// amdgx_dlssnr.dll — P2：DLSSNR 槽位内跑 MochizukiNrRuntime 真网络
//
// EvaluateFeature 的每一帧：
//   PrepareFrame(color) -> RecordInputs(自有 list，立即执行) -> EnqueueHip(自有 queue)
//   -> [宿主 list] RecordOutputs + CopyResource(DLSSNR.Output <- private_output)
// 上一帧的 job 用 Poll 轮询到 NR_COMPLETE 再 Retire（"立即 Retire 致复用"是 mochinr 踩过的坑）。
//
// 任何运行时失败（加载/建会话/PrepareFrame）自动回落 P1 直通拷贝 —— 永不黑屏。
// 调参环境变量：MOCHI_SHIM_SCALE(默认1.0) / MOCHI_SHIM_PASSES(默认1) / MOCHI_SHIM_OFF=1 强制直通。
//
// 资源状态契约（宿主 DLSSNRFilter.cpp）：进入时 Color=NON_PIXEL_SHADER_RESOURCE(0x40)、
// Output=UNORDERED_ACCESS(0x8)；返回时须保持。private_output 在 RecordOutputs 后为
// NON_PIXEL_SHADER_RESOURCE（mochinr 直接把它喂 FFX 的既有事实）。
// 静态导入 GetModuleFileNameW（宿主 IAT 钩子要求）不变。

#include <windows.h>
#include <d3d12.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>

#include "MochiNrApi.h"

namespace {

std::mutex g_logMutex;
HANDLE g_logFile = INVALID_HANDLE_VALUE;
std::wstring g_dllDir;

int g_successCode = 1;
long g_evalCount = 0;
long g_copyFailures = 0;
long g_nrFrames = 0;
long g_nrFailures = 0;

void ResolveLogPath() {
	wchar_t dllPath[MAX_PATH]{};
	GetModuleFileNameW(GetModuleHandleW(L"amdgx_dlssnr.dll"), dllPath, MAX_PATH);
	std::wstring dir(dllPath);
	const size_t slash = dir.find_last_of(L"\\/");
	if (slash != std::wstring::npos) dir.resize(slash + 1);
	else dir.clear();
	const std::wstring full = dir + L"nvngx-shim.log";
	g_dllDir = dir;
	char pathA[MAX_PATH * 2]{};
	WideCharToMultiByte(CP_UTF8, 0, full.c_str(), -1, pathA, sizeof(pathA), nullptr, nullptr);
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
class PassthroughParameters {
public:
	PassthroughParameters() {}
	virtual ~PassthroughParameters() = default;

	void Store(unsigned slot, const char* name, void* a, void* b) {
		if (!name) return;
		std::lock_guard<std::mutex> guard(g_valuesMutex);
		auto& entry = g_values[std::string(name)];
		if (entry.first == nullptr && a != nullptr) {
			Log("P2: %s = %p (slot %u, first)", name, a, slot);
		}
		entry = { a, b };
	}
	void* Get(const char* name) {
		std::lock_guard<std::mutex> guard(g_valuesMutex);
		auto it = g_values.find(name);
		return it == g_values.end() ? nullptr : it->second.first;
	}

#define P2_SLOT(i) \
	virtual void Slot##i(const char* name, void* a, void* b) { Store(i, name, a, b); }

	P2_SLOT(0)  P2_SLOT(1)  P2_SLOT(2)  P2_SLOT(3)
	P2_SLOT(4)  P2_SLOT(5)  P2_SLOT(6)  P2_SLOT(7)
	P2_SLOT(8)  P2_SLOT(9)  P2_SLOT(10) P2_SLOT(11)
	P2_SLOT(12) P2_SLOT(13) P2_SLOT(14) P2_SLOT(15)
	P2_SLOT(16) P2_SLOT(17) P2_SLOT(18) P2_SLOT(19)
	P2_SLOT(20) P2_SLOT(21) P2_SLOT(22) P2_SLOT(23)
	P2_SLOT(24) P2_SLOT(25) P2_SLOT(26) P2_SLOT(27)
	P2_SLOT(28) P2_SLOT(29) P2_SLOT(30) P2_SLOT(31)

#undef P2_SLOT

	static constexpr unsigned kSlots = 32;

private:
	static std::mutex g_valuesMutex;
	static std::map<std::string, std::pair<void*, void*>> g_values;
};

std::mutex PassthroughParameters::g_valuesMutex;
std::map<std::string, std::pair<void*, void*>> PassthroughParameters::g_values;

struct PassthroughHandle {
	uint32_t magic = 0x4E525032;  // 'NRP2'
	int featureId = -1;
};

// ---------------------------------------------------------------- mochi runtime 桥
struct MochiBridge {
	HMODULE module = nullptr;
	MochiNrApi api{};
	void* session = nullptr;
	uint64_t frame = 0;
	bool failed = false;
	bool inited = false;

	// 自有提交通道（producer 立即执行；EnqueueHip 用同一 queue）
	ID3D12Device* device = nullptr;
	ID3D12CommandQueue* queue = nullptr;
	ID3D12CommandAllocator* allocator = nullptr;
	ID3D12GraphicsCommandList* list = nullptr;

	// 上一帧 job（延迟回收）
	void* prevJob = nullptr;
	uint32_t prevJobState = 0;
};
MochiBridge g_mochi;

float EnvFloat(const char* name, float def) {
	char buf[32]{};
	if (GetEnvironmentVariableA(name, buf, sizeof(buf)) > 0) {
		float v = (float)atof(buf);
		if (v > 0.0f) return v;
	}
	return def;
}

bool InitMochi(ID3D12Device* device) {
	if (g_mochi.inited) return !g_mochi.failed;
	g_mochi.inited = true;

	char off[8]{};
	if (GetEnvironmentVariableA("MOCHI_SHIM_OFF", off, sizeof(off)) > 0 && off[0] == '1') {
		Log("P2: MOCHI_SHIM_OFF=1 -> passthrough only");
		g_mochi.failed = true;
		return false;
	}

	g_mochi.module = LoadLibraryExW((g_dllDir + L"MochizukiNrRuntime.dll").c_str(), nullptr,
		LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
	if (!g_mochi.module) {
		Log("P2: LoadLibrary MochizukiNrRuntime.dll err=%lu -> passthrough", GetLastError());
		g_mochi.failed = true;
		return false;
	}
	auto getApi = reinterpret_cast<int32_t (*)(uint32_t, MochiNrApi*)>(
		GetProcAddress(g_mochi.module, "LmxxfNrGetApi") /* DLL 导出名：历史 ABI 名 */);
	if (!getApi) {
		Log("P2: LmxxfNrGetApi missing -> passthrough");
		g_mochi.failed = true;
		return false;
	}
	g_mochi.api.struct_size = sizeof g_mochi.api;
	if (getApi(MOCHI_NR_ABI_VERSION, &g_mochi.api) != MOCHI_NR_OK) {
		Log("P2: GetApi rejected -> passthrough");
		g_mochi.failed = true;
		return false;
	}

	// 自有 DIRECT queue（CreateInfo 与 EnqueueHip 用同一个，保证队列契约自洽）
	HRESULT hr = device->CreateCommandQueue(
		&D3D12_COMMAND_QUEUE_DESC{ D3D12_COMMAND_LIST_TYPE_DIRECT }, IID_PPV_ARGS(&g_mochi.queue));
	if (FAILED(hr)) {
		Log("P2: CreateCommandQueue hr=0x%08lX -> passthrough", (unsigned long)hr);
		g_mochi.failed = true;
		return false;
	}
	device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g_mochi.allocator));
	device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_mochi.allocator, nullptr,
		IID_PPV_ARGS(&g_mochi.list));
	if (g_mochi.list) g_mochi.list->Close();
	g_mochi.device = device;

	MochiNrCapabilities caps{};
	caps.struct_size = sizeof caps;
	if (g_mochi.api.QueryCapabilities) g_mochi.api.QueryCapabilities(&caps);
	Log("P2: caps max %ux%u hip_ready=%u aco_target=%u",
		caps.max_input_width, caps.max_input_height, caps.hip_ready, caps.gfx1201_target);

	MochiNrCreateInfo ci{};
	ci.struct_size = sizeof ci;
	ci.device = device;
	ci.queue = g_mochi.queue;
	ci.assets_directory = g_dllDir.c_str();
	ci.flags = MOCHI_NR_CREATE_FLAG_ZERO_OUTPUT_FALLBACK | MOCHI_NR_CREATE_FLAG_ACO;

	int32_t rc = g_mochi.api.Create(&ci, &g_mochi.session);
	if (rc != MOCHI_NR_OK) {
		char err[512]{};
		if (g_mochi.api.GetLastError) g_mochi.api.GetLastError(err, sizeof err);
		Log("P2: Create rc=%d %s -> passthrough", rc, err);
		g_mochi.failed = true;
		return false;
	}
	Log("P2: session created, PrepareSession (编译管线，一次性 hitch)...");
	rc = g_mochi.api.PrepareSession(g_mochi.session);
	if (rc != MOCHI_NR_OK) {
		char err[512]{};
		if (g_mochi.api.GetLastError) g_mochi.api.GetLastError(err, sizeof err);
		Log("P2: PrepareSession rc=%d %s -> passthrough", rc, err);
		g_mochi.failed = true;
		return false;
	}
	Log("P2: session READY (weights=%ls)", g_dllDir.c_str());
	return true;
}

void RetirePrevIfDone() {
	if (!g_mochi.prevJob || !g_mochi.api.Poll || !g_mochi.api.Retire) return;
	uint32_t st = 0;
	if (g_mochi.api.Poll(g_mochi.session, g_mochi.prevJob, &st) == MOCHI_NR_OK) {
		g_mochi.prevJobState = st;
		if (st >= MOCHI_NR_JOB_NR_COMPLETE) {
			g_mochi.api.Retire(g_mochi.session, g_mochi.prevJob);
			g_mochi.prevJob = nullptr;
		}
	}
}

void Transition(ID3D12GraphicsCommandList* list, ID3D12Resource* res,
	D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
	D3D12_RESOURCE_BARRIER barrier{};
	barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	barrier.Transition = { res, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, before, after };
	list->ResourceBarrier(1, &barrier);
}

// P1 直通兜底：状态往返 + CopyResource
void PassthroughCopy(ID3D12GraphicsCommandList* list, ID3D12Resource* color, ID3D12Resource* output) {
	Transition(list, color, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
		D3D12_RESOURCE_STATE_COPY_SOURCE);
	Transition(list, output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
		D3D12_RESOURCE_STATE_COPY_DEST);
	list->CopyResource(output, color);
	Transition(list, color, D3D12_RESOURCE_STATE_COPY_SOURCE,
		D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
	Transition(list, output, D3D12_RESOURCE_STATE_COPY_DEST,
		D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
	++g_copyFailures;  // 复用计数器：直通帧数
}

}  // namespace

extern "C" {

__declspec(dllexport) int NVSDK_NGX_D3D12_Init_Ext(
	unsigned long long appId, const wchar_t* path, void* device,
	unsigned int version, const void* params) {
	Log("CALL Init_Ext appId=0x%llX device=%p version=0x%X",
		appId, device, version);
	// 宿主在此交付 D3D12 device —— mochi 会话在这里建（PrepareSession 编译管线，
	// 一次性 hitch 数秒，发生在缩放启动阶段，可接受）。
	if (device && !g_mochi.inited) InitMochi(static_cast<ID3D12Device*>(device));
	return g_successCode;
}

__declspec(dllexport) int NVSDK_NGX_D3D12_Init_with_ProjectID(
	const char* projectId, int engineType, const char* engineVersion, const wchar_t* path,
	void* device, const void* featureInfo, unsigned int version) {
	Log("CALL Init_with_ProjectID device=%p version=0x%X", device, version);
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
	return g_successCode;  // 不 delete：与 P0/P1 同策略，避免宿主悬空
}

__declspec(dllexport) int NVSDK_NGX_D3D12_CreateFeature(
	void* cmdList, int feature, void* params, void** handle) {
	Log("CALL CreateFeature feature=%d(%s) params=%p",
		feature, feature == 18 ? "CoreFeature18(NR)" : "unknown", params);
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
	const bool verbose = (g_evalCount <= 5) || (g_evalCount % 600 == 0);

	if (!list || !color || !output) {
		if (verbose) Log("EVAL #%ld missing: list=%p color=%p output=%p", g_evalCount, list, color, output);
		return g_successCode;
	}

	// ---- 真网络路径 ----
	if (!g_mochi.failed && g_mochi.session) {
		RetirePrevIfDone();

		MochiNrFrameInfo fi{};
		fi.struct_size = sizeof fi;
		fi.frame_id = ++g_mochi.frame;
		fi.color = color;
		fi.color_width = (uint32_t)color->GetDesc().Width;
		fi.color_height = color->GetDesc().Height;
		fi.color_state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;  // 宿主进入态
		fi.flags = MOCHI_NR_FRAME_FLAG_STRENGTH;
		fi.transfer_strength = 1.0f;
		fi.color_strength = 1.0f;
		fi.model_scale = EnvFloat("MOCHI_SHIM_SCALE", 1.0f);
		fi.passes = 1;
		{
			char buf[8]{};
			if (GetEnvironmentVariableA("MOCHI_SHIM_PASSES", buf, sizeof(buf)) > 0) {
				int p = atoi(buf);
				if (p >= 1 && p <= 3) fi.passes = (uint32_t)p;
			}
		}
		// Reset 是整型键（slot 2），a 可靠
		fi.reset = pp->Get("DLSSNR.Reset") != nullptr ? 1u : 0u;

		MochiNrJob job{};
		job.struct_size = sizeof job;
		int32_t rc = g_mochi.api.PrepareFrame(g_mochi.session, &fi, &job);
		if (rc == MOCHI_NR_OK && job.private_output) {
			// producer：自有 list 记录并立即执行（Record* 只记录不 Execute）
			bool producerOk = false;
			if (g_mochi.list && g_mochi.allocator) {
				if (SUCCEEDED(g_mochi.list->Reset(g_mochi.allocator, nullptr))) {
					if (g_mochi.api.RecordInputs(g_mochi.session, job.handle, g_mochi.list) == MOCHI_NR_OK
						&& SUCCEEDED(g_mochi.list->Close())) {
						ID3D12CommandList* lists[] = { g_mochi.list };
						g_mochi.queue->ExecuteCommandLists(1, lists);
						producerOk = true;
					}
				}
			}
			if (producerOk && g_mochi.api.EnqueueHip(g_mochi.session, job.handle, g_mochi.queue) == MOCHI_NR_OK) {
				// consumer：宿主 list 上记录 RecordOutputs + 拷回 Output
				int32_t outRc = g_mochi.api.RecordOutputs(g_mochi.session, job.handle, list);
				if (outRc == MOCHI_NR_OK) {
					auto* priv = static_cast<ID3D12Resource*>(job.private_output);
					Transition(list, priv, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
						D3D12_RESOURCE_STATE_COPY_SOURCE);
					Transition(list, output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
						D3D12_RESOURCE_STATE_COPY_DEST);
					list->CopyResource(output, priv);
					Transition(list, priv, D3D12_RESOURCE_STATE_COPY_SOURCE,
						D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
					Transition(list, output, D3D12_RESOURCE_STATE_COPY_DEST,
						D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
					++g_nrFrames;
					if (g_mochi.prevJob) {
						// 上一帧还没 Poll 到完成也先攒着，下一帧再试
					}
					g_mochi.prevJob = job.handle;
					if (verbose) {
						D3D12_RESOURCE_DESC pd = priv->GetDesc();
						Log("EVAL #%ld NR f=%llu private=%llux%u fmt=%u scale=%.2f passes=%u",
							g_evalCount, (unsigned long long)fi.frame_id,
							(unsigned long long)pd.Width, pd.Height, (unsigned)pd.Format,
							fi.model_scale, fi.passes);
					}
					return g_successCode;
				}
				Log("P2: RecordOutputs rc=%d f=%llu -> passthrough", outRc,
					(unsigned long long)fi.frame_id);
			} else if (producerOk) {
				char err[256]{};
				if (g_mochi.api.GetLastError) g_mochi.api.GetLastError(err, sizeof err);
				Log("P2: EnqueueHip failed f=%llu %s", (unsigned long long)fi.frame_id, err);
				g_mochi.api.CancelUnsubmitted(g_mochi.session, job.handle);
			} else {
				Log("P2: producer failed f=%llu", (unsigned long long)fi.frame_id);
				g_mochi.api.CancelUnsubmitted(g_mochi.session, job.handle);
			}
			++g_nrFailures;
		} else {
			char err[256]{};
			if (g_mochi.api.GetLastError) g_mochi.api.GetLastError(err, sizeof err);
			if (verbose || g_nrFailures < 10)
				Log("P2: PrepareFrame rc=%d f=%llu %s -> passthrough", rc,
					(unsigned long long)fi.frame_id, err);
			++g_nrFailures;
		}
	}  // end: 真网络路径（任何失败都落到下面的直通兜底）

	// ---- P1 直通兜底 ----
	PassthroughCopy(list, color, output);
	if (verbose)
		Log("EVAL #%ld passthrough color=%p output=%p", g_evalCount, (void*)color, (void*)output);
	return g_successCode;
}

__declspec(dllexport) int NVSDK_NGX_D3D12_ReleaseFeature(void* handle) {
	Log("CALL ReleaseFeature (nrFrames=%ld nrFails=%ld passthrough=%ld)",
		g_nrFrames, g_nrFailures, g_copyFailures);
	if (g_mochi.prevJob && g_mochi.api.Retire) {
		g_mochi.api.Retire(g_mochi.session, g_mochi.prevJob);
		g_mochi.prevJob = nullptr;
	}
	if (g_mochi.session && g_mochi.api.Drain) g_mochi.api.Drain(g_mochi.session);
	if (g_mochi.session && g_mochi.api.Destroy) {
		g_mochi.api.Destroy(g_mochi.session);
		g_mochi.session = nullptr;
	}
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
		GetModuleFileNameW(module, self, MAX_PATH);
		Log("P2: loaded \"%ls\" (mochizuki in DLSSNR slot, fallback=passthrough)", self);
	} else if (reason == DLL_PROCESS_DETACH) {
		Log("P2: unloaded (nr=%ld nrFails=%ld passthrough=%ld)", g_nrFrames, g_nrFailures, g_copyFailures);
		if (g_logFile != INVALID_HANDLE_VALUE) {
			CloseHandle(g_logFile);
			g_logFile = INVALID_HANDLE_VALUE;
		}
	}
	return TRUE;
}
