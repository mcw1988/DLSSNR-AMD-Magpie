// amdgx_dlssnr.dll — P2：DLSSNR 槽位内跑 MochizukiNrRuntime 真网络
//
// EvaluateFeature 的每一帧：
//   PrepareFrame(color) -> RecordInputs(自有 list，立即执行) -> EnqueueHip(自有 queue)
//   -> [宿主 list] RecordOutputs + CopyResource(DLSSNR.Output <- private_output)
// 上一帧的 job 用 Poll 轮询到 NR_COMPLETE 再 Retire（"立即 Retire 致复用"是 mochinr 踩过的坑）。
//
// 任何运行时失败（加载/建会话/PrepareFrame）自动回落 P1 直通拷贝 —— 永不黑屏。
// 调参环境变量：MOCHI_SHIM_SCALE(默认1.0) / MOCHI_SHIM_PASSES(默认1) / MOCHI_SHIM_OFF=1 强制直通。
// 渲染率热启（2026-11-04）：DLL 旁的 MochiShimScale.txt 写 0.25..1.0，**运行中改即重建网络**
//   （约 0.5–1 s 顿卡），不必重启效果器、不必重启 Magpie；文件优先于环境变量。
//
// v0.6 面板接线（2026-11-04）：宿主的 DLSSNR.* 效果参数 → MochizukiNrSetControls。
// 浮点键原来读不到（XMM 传参，32 个槽位统一声明成 (name, void*, void*) 只能读到 R8 残值），
// 现在按槽位给出真实类型签名（slot5=float / slot2=int / slot3=unsigned / slot0=void*），
// 于是 Strength / Local Tone / Local Structure / Skin / Style / AutoMask 全部可取。
// 逐 pass 生效：宿主每趟 pass 送的是那一趟自己的 settings，换 pass 时值会变，这里按值比对下发。
// 另：renodx=1 时运行时忽略模型组（fi.model_scale 也白给），渲染率走 reno_resolution_scale ——
// MOCHI_SHIM_SCALE 自本版起才真正改到网络分辨率（此前一直没生效）。
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
#include "MochizukiNrControls.h"

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
// 宿主按它自己那份 NGX 头文件的 vtable 顺序调 Set()，而这个对象是我们 new 出来交给它的。
//
// 权威声明来自 DLSS SDK 的 nvsdk_ngx_params.h（宿主 include 的是 $(DLSSSdkDir)\include）：
//   struct NVSDK_NGX_Parameter {                       // 接口本身**没有虚析构**
//     Set(const char*, unsigned long long); Set(, float); Set(, double); Set(, unsigned int);
//     Set(, int); Set(, ID3D11Resource*); Set(, ID3D12Resource*); Set(, void*);   // ← 声明顺序
//     Get(...) ×8;  Reset();
//   };
//
// 但 MSVC 会把**同名重载在 vtable 里按声明顺序反着摆**，实际 vtable 是：
//   [0] void*   [1] ID3D12Resource*  [2] ID3D11Resource*  [3] int
//   [4] unsigned[5] double           [6] float            [7] ull
// 我们的类有一个虚析构，占掉 vtable[0]，于是 SlotN 落在 vtable[N+1]：
//   vtable[0] void*           → ~PassthroughParameters（静默丢弃，见下）
//   vtable[1] ID3D12Resource* → Slot0      vtable[2] ID3D11Resource* → Slot1
//   vtable[3] int             → Slot2      vtable[4] unsigned        → Slot3
//   vtable[5] double          → Slot4      vtable[6] float           → Slot5
//   vtable[7] ull             → Slot6
//
// 这张表是 2026-11-04 用 abi_probe.cpp（**直接 include 上游那份头文件**，不改写接口）实测的，
// 与 P2 实机日志完全一致（资源 slot0 / int slot2 / uint slot3 / float slot5）。
// 两个推论：
//   · 宿主若调 Set(name, void*) 会落到虚析构上 —— 默认析构、类里没有成员，等于静默丢弃，不会崩；
//   · 宿主若调 Get(...) 会落到 Slot7 之后 —— DLSSNRFilter 当前两者都不用（已核 src/Magpie.Core）。
//
// P2 时代的坑：32 个槽位一律声明成 (const char*, void*, void*)，浮点值在 XMM 而 R8 是残值，
// 于是 DLSSNR.Intensity 只能读到 7FFFFFFFFFFFFFFC。整型/指针恰好在 GP 寄存器，所以一直是对的。
enum class ParamKind : uint8_t { Ptr, Int, Float };

struct ParamValue {
	ParamKind kind = ParamKind::Ptr;
	void* ptr = nullptr;
	int64_t i = 0;
	float f = 0.0f;
};

class PassthroughParameters {
public:
	PassthroughParameters() {}
	virtual ~PassthroughParameters() = default;

	void StorePtr(unsigned slot, const char* name, void* a) {
		if (!name) return;
		std::lock_guard<std::mutex> guard(g_valuesMutex);
		auto& entry = g_values[std::string(name)];
		if (entry.kind != ParamKind::Ptr || entry.ptr == nullptr) {
			if (a != nullptr) Log("P2: %s = %p (slot %u, ptr)", name, a, slot);
		}
		entry.kind = ParamKind::Ptr;
		entry.ptr = a;
	}
	void StoreInt(unsigned slot, const char* name, int64_t v) {
		if (!name) return;
		std::lock_guard<std::mutex> guard(g_valuesMutex);
		auto& entry = g_values[std::string(name)];
		if (entry.kind != ParamKind::Int)
			Log("P2: %s = %lld (slot %u, int)", name, (long long)v, slot);
		entry.kind = ParamKind::Int;
		entry.i = v;
	}
	void StoreFloat(unsigned slot, const char* name, float v) {
		if (!name) return;
		std::lock_guard<std::mutex> guard(g_valuesMutex);
		auto& entry = g_values[std::string(name)];
		if (entry.kind != ParamKind::Float)
			Log("P2: %s = %g (slot %u, float)", name, (double)v, slot);
		entry.kind = ParamKind::Float;
		entry.f = v;
	}

	void* Get(const char* name) {
		std::lock_guard<std::mutex> guard(g_valuesMutex);
		auto it = g_values.find(name);
		return it == g_values.end() ? nullptr : it->second.ptr;
	}
	bool GetInt(const char* name, int32_t* out) {
		std::lock_guard<std::mutex> guard(g_valuesMutex);
		auto it = g_values.find(name);
		if (it == g_values.end() || it->second.kind != ParamKind::Int) return false;
		if (out) *out = (int32_t)it->second.i;
		return true;
	}
	bool GetFloat(const char* name, float* out) {
		std::lock_guard<std::mutex> guard(g_valuesMutex);
		auto it = g_values.find(name);
		if (it == g_values.end() || it->second.kind != ParamKind::Float) return false;
		if (out) *out = it->second.f;
		return true;
	}

	// 指针/整型槽位：签名按上面的槽位表声明。
	virtual void Slot0(const char* name, void* a) { StorePtr(0, name, a); }
	virtual void Slot1(const char* name, void* a, void* b) { (void)b; StorePtr(1, name, a); }
	virtual void Slot2(const char* name, int32_t v) { StoreInt(2, name, v); }
	virtual void Slot3(const char* name, uint32_t v) { StoreInt(3, name, (int64_t)v); }
	virtual void Slot4(const char* name, double v) { StoreFloat(4, name, (float)v); }
	virtual void Slot5(const char* name, float v) { StoreFloat(5, name, v); }

#define P2_SLOT(i) \
	virtual void Slot##i(const char* name, void* a, void* b) { (void)b; StorePtr(i, name, a); }

	P2_SLOT(6)  P2_SLOT(7)  P2_SLOT(8)  P2_SLOT(9)  P2_SLOT(10) P2_SLOT(11)
	P2_SLOT(12) P2_SLOT(13) P2_SLOT(14) P2_SLOT(15)
	P2_SLOT(16) P2_SLOT(17) P2_SLOT(18) P2_SLOT(19)
	P2_SLOT(20) P2_SLOT(21) P2_SLOT(22) P2_SLOT(23)
	P2_SLOT(24) P2_SLOT(25) P2_SLOT(26) P2_SLOT(27)
	P2_SLOT(28) P2_SLOT(29) P2_SLOT(30) P2_SLOT(31)

#undef P2_SLOT

	static constexpr unsigned kSlots = 32;

private:
	static std::mutex g_valuesMutex;
	static std::map<std::string, ParamValue> g_values;
};

std::mutex PassthroughParameters::g_valuesMutex;
std::map<std::string, ParamValue> PassthroughParameters::g_values;

// 面板快照：只装"面板真的能改"的那几项，用于和上一次下发值比对（逐 pass 会变）。
struct PanelSnapshot {
	uint32_t renodx = 1;
	uint32_t style = 0;
	float intensity = 1.0f;
	float localTone = 1.0f;
	float localStructure = 1.0f;
	float skinStructure = -1.0f;
	uint32_t autoMask = 1;
	float modelScale = 0.0f;  // 0 = 不发（保持运行时默认）
	uint32_t passes = 1;      // 网络层数（运行时内部多趟，不是宿主 Multi Pass）
};

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

	// 上一帧的 job（延迟回收）。**必须是环**：Multi Pass > 1 时每帧有 2–3 个 job，
	// 只记一个会导致每帧漏 Retire（旧版就是这样，多层时资源/槽位泄漏）。
	static constexpr int kJobRing = 8;
	void* jobRing[kJobRing]{};
	int jobRingCount = 0;

	// 控制项通道（清单第 1 项）：宿主的 DLSSNR.* 参数 → MochizukiNrSetControls
	PFN_MochizukiNrSetControls setControls = nullptr;
	PFN_MochizukiNrGetControlDefaults controlDefaults = nullptr;
	PFN_MochizukiNrGetInfo getInfo = nullptr;
	MochizukiNrControls defaults{};   // 运行时自己的默认值（建会话时抓一次）
	PanelSnapshot sent{};             // 上一次真的下发的那一份
	bool controlsReady = false;
	bool controlsSent = false;
	uint32_t panelCalls = 0;          // 已下发次数（逐 pass 不同值时每帧会发多次）
	unsigned long long panelLogAt = 0; // 上一条面板日志的时间（按时间限流，保证"改了就能看到"）
	bool motionLogged = false;         // "motion: 已接上 …" 只打一次
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

// 文件式开关：DLL 旁的 MochiShim<Tag>.txt，内容为 0/1；文件不存在时回落环境变量。
// 用文件而不是环境变量，是因为 Magpie 通常从资源管理器启动，setx 之后拿不到新环境的概率很高。
int ReadFlagFile(const wchar_t* fileName, int fallback) {
	const std::wstring path = g_dllDir + fileName;
	WIN32_FILE_ATTRIBUTE_DATA fad{};
	if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad)) return fallback;
	char buf[16]{};
	FILE* f = nullptr;
	if (_wfopen_s(&f, path.c_str(), L"rb") == 0 && f != nullptr) {
		const size_t n = fread(buf, 1, sizeof(buf) - 1, f);
		buf[n] = '\0';
		fclose(f);
	}
	return atoi(buf) != 0 ? 1 : 0;
}

// 叠层（网络多趟）：读 DLL 旁的 MochiShimPasses.txt（1..3），最多每秒看一次。
// 借鉴旧 ReShade 路线（mochinr.cpp:719 `fi.passes`）与上游 OpenNR（mochizuki.inc:357）：
// **叠层是运行时在**一个 job 内部**跑 N 趟**（每趟吃上一趟的输出），不是宿主的多 pass。
// 宿主 Multi Pass 会走 laterPasses，那是跨队列时序，实测会全屏闪烁（见归档 §4.2）。
uint32_t ReadPassesFile() {
	static unsigned long long lastCheck = 0;
	static unsigned long long lastWrite = 0;
	static uint32_t value = 0;  // 0 = 不发（沿用环境变量/默认 1）
	const unsigned long long now = GetTickCount64();
	if (now - lastCheck < 1000) return value;
	lastCheck = now;

	const std::wstring path = g_dllDir + L"MochiShimPasses.txt";
	WIN32_FILE_ATTRIBUTE_DATA fad{};
	if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad)) {
		lastWrite = 0;
		value = 0;
		return value;
	}
	const unsigned long long wt =
		((unsigned long long)fad.ftLastWriteTime.dwHighDateTime << 32) | fad.ftLastWriteTime.dwLowDateTime;
	if (wt == lastWrite) return value;
	lastWrite = wt;

	char buf[32]{};
	FILE* f = nullptr;
	if (_wfopen_s(&f, path.c_str(), L"rb") == 0 && f != nullptr) {
		const size_t n = fread(buf, 1, sizeof(buf) - 1, f);
		buf[n] = '\0';
		fclose(f);
	}
	const int p = atoi(buf);
	if (p >= 1 && p <= 3) {
		value = (uint32_t)p;
		Log("passes: file = %d -> fi.passes / reno_passes（网络将重建）", p);
	} else {
		value = 0;
		Log("passes: file 值无效(%s)，忽略（范围 1..3）", buf);
	}
	return value;
}

// 最终生效的层数：文件 > 环境变量 > 1
uint32_t EffectivePasses() {
	const uint32_t fromFile = ReadPassesFile();
	if (fromFile > 0) return fromFile;
	char buf[8]{};
	if (GetEnvironmentVariableA("MOCHI_SHIM_PASSES", buf, sizeof(buf)) > 0) {
		const int p = atoi(buf);
		if (p >= 1 && p <= 3) return (uint32_t)p;
	}
	return 1;
}

// 抗光晕（ANTI_HALO）：MochiShimHalo.txt（0 关 / 1 开），节流 1 秒，热生效。
// 文件不存在时**跟随层数**：≥2 层自动开 —— 与旧 ReShade 路线一致（mochinr.cpp:712 在 passes>1 时开）。
// 作用：限制"第二趟把像素推出第一趟局部范围"的幅度，压住叠层带来的边缘光晕/拖影。
// 幅度参数 anti_halo_margin 在本结构里传不了（FrameInfo 在 depth 前就结束），运行时按默认 0.05 处理。
bool ReadHaloFlag(uint32_t passes) {
	static unsigned long long lastCheck = 0;
	static int value = -1;  // -1 = 无文件（跟随层数）
	const unsigned long long now = GetTickCount64();
	if (now - lastCheck >= 1000) {
		lastCheck = now;
		const std::wstring path = g_dllDir + L"MochiShimHalo.txt";
		WIN32_FILE_ATTRIBUTE_DATA fad{};
		if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad)) {
			if (value != -1) Log("halo: 跟随层数（文件不存在）");
			value = -1;
		} else {
			char buf[16]{};
			FILE* f = nullptr;
			if (_wfopen_s(&f, path.c_str(), L"rb") == 0 && f != nullptr) {
				const size_t n = fread(buf, 1, sizeof(buf) - 1, f);
				buf[n] = '\0';
				fclose(f);
			}
			const int next = (buf[0] == '0') ? 0 : 1;
			if (next != value) {
				Log("halo: 抗光晕 %s（anti_halo_margin = 运行时默认 0.05）", next ? "开" : "关");
			}
			value = next;
		}
	}
	return value < 0 ? (passes > 1) : (value != 0);
}
// 运动矢量转接开关：MochiShimMotion.txt=0 或 MOCHI_SHIM_MOTION=0 关掉（默认开）。
bool MotionEnabled() {
	static int cached = -1;
	if (cached < 0) {
		char env[16]{};
		const int envVal = (GetEnvironmentVariableA("MOCHI_SHIM_MOTION", env, sizeof(env)) > 0
			&& env[0] == '0') ? 0 : 1;
		cached = ReadFlagFile(L"MochiShimMotion.txt", envVal);
	}
	return cached != 0;
}

// 时域开关：MochiShimTemporal.txt=1 或 MOCHI_SHIM_TEMPORAL=1 打开 TEMPORAL 帧标志（默认关）。
// 用于分辨"运行时是不收 motion，还是收了但没开时域"。
bool TemporalEnabled() {
	static int cached = -1;
	if (cached < 0) {
		char env[16]{};
		const int envVal = (GetEnvironmentVariableA("MOCHI_SHIM_TEMPORAL", env, sizeof(env)) > 0
			&& env[0] == '1') ? 1 : 0;
		cached = ReadFlagFile(L"MochiShimTemporal.txt", envVal);
	}
	return cached != 0;
}

// 每 300 帧打一次运行时自报的状态：history 用没用、motion 是否被拒、GPU 中位耗时。
// 光流测试的客观依据（画面之外唯一能说明"光流真的进去了"的东西）。
void LogRuntimeInfoPeriodic() {
	if (!g_mochi.getInfo || !g_mochi.session) return;
	if (g_mochi.frame == 0 || (g_mochi.frame % 300) != 0) return;
	MochizukiNrInfo info{};
	info.struct_size = sizeof info;
	if (g_mochi.getInfo(g_mochi.session, &info) != MOCHI_NR_OK) return;
	// 两条"体感"诊断：
	//   inflight = 还在飞的 job 数 → GPU 落后几帧（延迟的来源，环深 8）
	//   fps      = shim 侧墙钟吞吐 → 与 Magpie 面板显示的帧率对照
	static unsigned long long lastAt = 0;
	static unsigned long long lastFrames = 0;
	const unsigned long long now = GetTickCount64();
	double fps = 0.0;
	if (lastAt != 0 && now > lastAt && info.frames > lastFrames) {
		fps = (double)(info.frames - lastFrames) * 1000.0 / (double)(now - lastAt);
	}
	lastAt = now;
	lastFrames = info.frames;
	Log("info: frames=%llu history=%u%% motion_refused_dxgi=%u gpu_ms=%.2f(p95 %.2f) "
		"inflight=%d fps=%.1f preprocess_ev=%.2f white=%.3f renodx=%u",
		(unsigned long long)info.frames, info.history_consumed_pct, info.motion_refused_dxgi,
		(double)info.gpu_ms_median, (double)info.gpu_ms_p95,
		g_mochi.jobRingCount, fps,
		(double)info.preprocess_ev, (double)info.white_point, info.renodx_effective);
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

	// 控制项导出（清单第 1 项）。老运行时没有这些：不致命，只是面板旋钮不生效，日志里说清楚。
	g_mochi.setControls = reinterpret_cast<PFN_MochizukiNrSetControls>(
		GetProcAddress(g_mochi.module, "MochizukiNrSetControls"));
	g_mochi.controlDefaults = reinterpret_cast<PFN_MochizukiNrGetControlDefaults>(
		GetProcAddress(g_mochi.module, "MochizukiNrGetControlDefaults"));
	g_mochi.getInfo = reinterpret_cast<PFN_MochizukiNrGetInfo>(
		GetProcAddress(g_mochi.module, "MochizukiNrGetInfo"));
	g_mochi.defaults.struct_size = sizeof(MochizukiNrControls);
	if (g_mochi.setControls && g_mochi.controlDefaults &&
		g_mochi.controlDefaults(&g_mochi.defaults) == MOCHI_NR_OK) {
		g_mochi.controlsReady = true;
		const MochizukiNrControls& d = g_mochi.defaults;
		// 这行是"面板到底该写哪一组字段"的唯一事实来源：renodx=1 时模型组（intensity/style/
		// local_*）被运行时忽略，真正生效的是 reno_* 组（见 MochizukiNrControls.h 注释）。
		Log("panel: controls READY struct_size=%u", d.struct_size);
		Log("panel: defaults reno{renodx=%u passes=%u scale=%.2f style=%u intensity=%.2f tone=%.2f "
			"struct=%.2f skin=%.2f mask=%u chained_history=%u stability=%u follow_input=%u} "
			"model{intensity=%.2f style=%u tone=%.2f struct=%.2f skin=%.2f mask=%u}",
			d.renodx, d.reno_passes, (double)d.reno_resolution_scale, d.reno_style,
			(double)d.reno_intensity, (double)d.reno_local_tone, (double)d.reno_local_structure,
			(double)d.reno_skin_structure, d.reno_auto_mask,
			d.reno_chained_history, d.reno_detail_stability, d.reno_follow_input_res,
			(double)d.intensity, d.style, (double)d.local_tone, (double)d.local_structure,
			(double)d.skin_structure, d.automatic_mask);
	} else {
		Log("panel: controls UNAVAILABLE (SetControls=%p Defaults=%p) -> 面板旋钮不生效",
			(void*)g_mochi.setControls, (void*)g_mochi.controlDefaults);
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
	// 这几个能力位决定"运动矢量/历史"有没有意义：history_supported=0 时，
	// 无论宿主算不算光流，运行时都不会用它 —— 直接决定光流这条线值不值得测。
	Log("P2: caps max %ux%u hip_ready=%u aco_target=%u | history=%u overlap=%u graph=%u",
		caps.max_input_width, caps.max_input_height, caps.hip_ready, caps.gfx1201_target,
		caps.history_supported, caps.overlap_supported, caps.graph_supported);

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

// 回收所有已完成的 job。**环里可能同时存在多帧、多个 pass 的 job**（Multi Pass > 1）。
void RetireDoneJobs() {
	if (!g_mochi.api.Poll || !g_mochi.api.Retire) return;
	int kept = 0;
	for (int i = 0; i < g_mochi.jobRingCount; ++i) {
		void* h = g_mochi.jobRing[i];
		uint32_t st = 0;
		if (g_mochi.api.Poll(g_mochi.session, h, &st) == MOCHI_NR_OK &&
			st >= MOCHI_NR_JOB_NR_COMPLETE) {
			g_mochi.api.Retire(g_mochi.session, h);
		} else {
			g_mochi.jobRing[kept++] = h;
		}
	}
	g_mochi.jobRingCount = kept;
}

void TrackJob(void* handle) {
	if (g_mochi.jobRingCount < MochiBridge::kJobRing) {
		g_mochi.jobRing[g_mochi.jobRingCount++] = handle;
		return;
	}
	// 环满说明 GPU 落后 4 帧以上（8 槽 / 每帧 2 个）。记一次，别静默丢：
	// 丢掉的是"回收记录"，job 本体由运行时自己管，不会崩，但会推迟释放。
	static int warnCount = 0;
	if (warnCount < 5) {
		++warnCount;
		Log("P2: job ring full (%d) —— 有 job 未被跟踪回收", MochiBridge::kJobRing);
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

// ---------------------------------------------------------------- 面板 → 控制项（清单第 1 项）
// 宿主每帧通过 NGX 参数对象送来的 DLSSNR.* 键（DLSSNRFilter::SetEvaluateParametersUnsafe）
// 在这里翻译成 MochizukiNrSetControls。要点：
//   1. 这些键是**逐 pass** 的：宿主对 pass2/pass3 送的是那一趟自己的 settings
//      （DLSSNRFilter::ApplyLiveParameters → _passSettings[i]），所以换 pass 时值会变，
//      必须用"和上次真的下发的那份比"来判断，不能用"只在下一次改动时下发"的脏标记。
//   2. SetControls 在**下一次 PrepareFrame** 生效，而每趟 pass 就是一次 PrepareFrame ⇒ 逐 pass 天然对齐。
//   3. renodx=1（运行时默认）时模型组字段被忽略，真正生效的是 reno_* 组。
//      P2 期间 fi.model_scale 一直被忽略就是这个原因 —— MOCHI_SHIM_SCALE 从未真正改过渲染率。
namespace {

float ClampF(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
int32_t ClampI(int32_t v, int32_t lo, int32_t hi) { return v < lo ? lo : (v > hi ? hi : v); }

// 渲染率热启：读 DLL 旁边的 MochiShimScale.txt（0.25..1.0），最多每秒看一次。
// 值变了就把新的 reno_resolution_scale 下发，运行时自己重建网络（约 0.5–1 s 顿卡），
// **不重启效果器、不重启 Magpie**。面板化要等宿主补丁 0004（见设计文档 §8）。
float ReadScaleFile() {
	static unsigned long long lastCheck = 0;
	static unsigned long long lastWrite = 0;
	static float value = 0.0f;
	const unsigned long long now = GetTickCount64();
	if (now - lastCheck < 1000) return value;
	lastCheck = now;

	const std::wstring path = g_dllDir + L"MochiShimScale.txt";
	WIN32_FILE_ATTRIBUTE_DATA fad{};
	if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad)) {
		if (lastWrite) Log("panel: scale file removed -> 保持当前渲染率");
		lastWrite = 0;
		value = 0.0f;
		return value;
	}
	const unsigned long long wt =
		((unsigned long long)fad.ftLastWriteTime.dwHighDateTime << 32) | fad.ftLastWriteTime.dwLowDateTime;
	if (wt == lastWrite) return value;
	lastWrite = wt;

	char buf[64]{};
	FILE* f = nullptr;
	if (_wfopen_s(&f, path.c_str(), L"rb") == 0 && f != nullptr) {
		const size_t n = fread(buf, 1, sizeof(buf) - 1, f);
		buf[n] = '\0';
		fclose(f);
	}
	const double v = atof(buf);
	if (v >= 0.25 && v <= 1.0) {
		value = (float)v;
		Log("panel: scale file = %.2f -> SetControls (网络将重建)", v);
	} else {
		value = 0.0f;
		Log("panel: scale file 值无效(%s)，忽略（范围 0.25..1.0）", buf);
	}
	return value;
}

bool SamePanel(const PanelSnapshot& a, const PanelSnapshot& b) {
	return a.renodx == b.renodx && a.style == b.style && a.intensity == b.intensity &&
		a.localTone == b.localTone && a.localStructure == b.localStructure &&
		a.skinStructure == b.skinStructure && a.autoMask == b.autoMask &&
		a.modelScale == b.modelScale && a.passes == b.passes;
}

// 返回 true 表示这一帧要不要 Reset 历史（真值语义，不是"键存在"）
bool ReadPanel(PassthroughParameters* pp, PanelSnapshot* out, uint32_t* resetOut) {
	if (resetOut) {
		int32_t r = 0;
		*resetOut = (pp->GetInt("DLSSNR.Reset", &r) && r != 0) ? 1u : 0u;
	}
	if (!g_mochi.controlsReady) return false;

	const MochizukiNrControls& d = g_mochi.defaults;
	PanelSnapshot s{};
	s.renodx = d.renodx;
	s.style = d.reno_style;
	s.intensity = d.reno_intensity;
	s.localTone = d.reno_local_tone;
	s.localStructure = d.reno_local_structure;
	s.skinStructure = d.reno_skin_structure;
	s.autoMask = d.reno_auto_mask;

	int32_t i32 = 0;
	float f32 = 0.0f;
	if (pp->GetInt("DLSSNR.Style", &i32)) s.style = (uint32_t)ClampI(i32, 0, 2);
	if (pp->GetFloat("DLSSNR.Intensity", &f32)) s.intensity = ClampF(f32, 0.0f, 2.0f);
	if (pp->GetFloat("DLSSNR.LocalToneStrength", &f32)) s.localTone = ClampF(f32, 0.0f, 2.0f);
	if (pp->GetFloat("DLSSNR.LocalStructureStrength", &f32))
		s.localStructure = ClampF(f32, 0.0f, 2.0f);
	// 面板的 Skin 只有 0..2，表达不了运行时的 -1（跟随 Local Structure）。
	// 所以 0 视为"没表态"，保持运行时默认，避免把默认的 -1 悄悄改成 0。
	if (pp->GetFloat("DLSSNR.SkinStructureStrength", &f32) && f32 > 0.0f)
		s.skinStructure = ClampF(f32, 0.0f, 2.0f);
	if (pp->GetInt("DLSSNR.UseAutoMask", &i32)) s.autoMask = i32 ? 1u : 0u;

	// 渲染率 s 与 renodx 组暂由外部接管：面板还没有这两个键（要宿主补丁 0004，见设计文档 §8）。
	// 优先级：MochiShimScale.txt（可运行中改，热重建网络）> 环境变量 MOCHI_SHIM_SCALE（需重启 Magpie）。
	const float fileScale = ReadScaleFile();
	if (fileScale > 0.0f) {
		s.modelScale = fileScale;
	} else {
		char env[32]{};
		if (GetEnvironmentVariableA("MOCHI_SHIM_SCALE", env, sizeof(env)) > 0) {
			const double v = atof(env);
			if (v >= 0.25 && v <= 1.0) s.modelScale = (float)v;
		}
	}
	{
		char env[32]{};
		if (GetEnvironmentVariableA("MOCHI_SHIM_RENODX", env, sizeof(env)) > 0) {
			s.renodx = atoi(env) ? 1u : 0u;
		}
	}
	// 网络层数（运行时内部多趟）
	s.passes = EffectivePasses();

	*out = s;
	return true;
}

void ApplyPanelControls(PassthroughParameters* pp) {
	if (!g_mochi.controlsReady || !g_mochi.setControls || !g_mochi.session) return;
	PanelSnapshot s{};
	if (!ReadPanel(pp, &s, nullptr)) return;
	if (g_mochi.controlsSent && SamePanel(s, g_mochi.sent)) return;

	// 从运行时默认值起手：只覆盖面板真的能改的字段，别的（reno_passes 等）一个都不碰。
	MochizukiNrControls c = g_mochi.defaults;
	c.struct_size = sizeof c;
	c.renodx = s.renodx;
	// renodx 阶段真正生效的一组
	c.reno_style = s.style;
	c.reno_intensity = s.intensity;
	c.reno_local_tone = s.localTone;
	c.reno_local_structure = s.localStructure;
	c.reno_skin_structure = s.skinStructure;
	c.reno_auto_mask = s.autoMask;
	// 叠层：运行时内部多趟（旧 ReShade 路线与 OpenNR 都是这么做的）。
	// `reno_unlock_passes` 是旧路线为了让面板能超过 1 层额外打开的（mochinr.cpp:620）。
	c.reno_passes = s.passes;
	c.reno_unlock_passes = s.passes > 1 ? 1u : 0u;
	if (s.modelScale > 0.0f) c.reno_resolution_scale = s.modelScale;
	// 模型组也写一份：把 MOCHI_SHIM_RENODX=0 时的 A/B 也做成零改动可测
	c.style = s.style;
	c.intensity = s.intensity;
	c.local_tone = s.localTone;
	c.local_structure = s.localStructure;
	c.skin_structure = s.skinStructure;
	c.automatic_mask = s.autoMask;

	const int32_t rc = g_mochi.setControls(g_mochi.session, &c);
	++g_mochi.panelCalls;
	// 按**时间**限流（≥500 ms 一条），而不是按次数：这样"拧一下旋钮"必定在 0.5 s 内落一行，
	// 逐 pass 不同控制导致的每帧多次下发也不会灌爆日志。
	const unsigned long long now = GetTickCount64();
	const bool talk = g_mochi.panelCalls <= 30 || (now - g_mochi.panelLogAt >= 500);
	if (talk) {
		g_mochi.panelLogAt = now;
		Log("panel: setcontrols rc=%d (f=%llu n=%u) renodx=%u style=%u intensity=%.2f tone=%.2f "
			"struct=%.2f skin=%.2f mask=%u scale=%.2f passes=%u(unlock=%u)",
			rc, (unsigned long long)g_mochi.frame, g_mochi.panelCalls, s.renodx, s.style,
			(double)s.intensity, (double)s.localTone, (double)s.localStructure,
			(double)s.skinStructure, s.autoMask, (double)c.reno_resolution_scale, c.reno_passes,
			c.reno_unlock_passes);
	}
	g_mochi.sent = s;
	g_mochi.controlsSent = true;
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
		RetireDoneJobs();

		MochiNrFrameInfo fi{};
		fi.struct_size = sizeof fi;
		fi.frame_id = ++g_mochi.frame;

		// 面板 → MochizukiNrSetControls。必须排在 PrepareFrame 之前（下次 PrepareFrame 生效）。
		ApplyPanelControls(pp);
		fi.color = color;
		fi.color_width = (uint32_t)color->GetDesc().Width;
		fi.color_height = color->GetDesc().Height;
		fi.color_state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;  // 宿主进入态
		fi.flags = MOCHI_NR_FRAME_FLAG_STRENGTH;
		// 时域：唯一实测有正面收益的开关（闪烁抑制，代价约 -1.7 fps）。
		// 放在这里（而不是后面）的原因：下面的 motion 日志要打最终 flags，放后面会打出 OR 之前的值。
		if (TemporalEnabled()) fi.flags |= MOCHI_NR_FRAME_FLAG_TEMPORAL;
		// 抗光晕：叠层时压边缘光晕/拖影（≥2 层自动开，或由 MochiShimHalo.txt 显式指定）。
		if (ReadHaloFlag(EffectivePasses())) fi.flags |= MOCHI_NR_FRAME_FLAG_ANTI_HALO;
		fi.transfer_strength = 1.0f;
		fi.color_strength = 1.0f;
		// 模型组的 model_scale 在 renodx=1 时被运行时忽略（渲染率走 reno_resolution_scale），
		// 这里保留赋值只为 renodx=0 的 A/B 可用。
		fi.model_scale = EnvFloat("MOCHI_SHIM_SCALE", 1.0f);
		// 层数：文件 > 环境变量 > 1。运行时在一个 job 内跑 N 趟（每趟吃上一趟输出），
		// 与宿主 Multi Pass（laterPasses）是两套机制，后者在本架构下有跨队列时序问题。
		fi.passes = EffectivePasses();
		// Reset 是整型键（slot 2）：现在读的是**值**而不是"键在不在"。
		// 宿主每帧都会 Set 一次（1 = 丢历史），所以这里必须按值判断。
		{
			int32_t resetValue = 0;
			fi.reset = (pp->GetInt("DLSSNR.Reset", &resetValue) && resetValue != 0) ? 1u : 0u;
		}

		// 运动矢量 / 深度：宿主在 DLSSNR.MVec / DLSSNR.Depth 里送，**尺寸与 Color 同域**
		// （实测 MVecSubrectWidth == ColorSubrectWidth）。以前这里一行都没有，
		// 所以运行时的 `motion 0x0 DXGI 0` 与宿主的「光流方法」设置无关 —— 2026-11-04 补上。
		// 开关：MOCHI_SHIM_MOTION=0 可关掉（做 A/B 用），默认开。
		if (MotionEnabled()) {
			auto* mvec = static_cast<ID3D12Resource*>(pp->Get("DLSSNR.MVec"));
			if (mvec) {
				fi.motion = mvec;
				fi.motion_state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
				int32_t mw = 0, mh = 0;
				pp->GetInt("DLSSNR.MVecSubrectWidth", &mw);
				pp->GetInt("DLSSNR.MVecSubrectHeight", &mh);
				fi.motion_width = (uint32_t)(mw > 0 ? mw : 0);
				fi.motion_height = (uint32_t)(mh > 0 ? mh : 0);
				float sx = 1.0f, sy = 1.0f;
				pp->GetFloat("DLSSNR.MVecScaleX", &sx);
				pp->GetFloat("DLSSNR.MVecScaleY", &sy);
				fi.motion_scale_x = sx;
				fi.motion_scale_y = sy;
			}
			auto* depthRes = static_cast<ID3D12Resource*>(pp->Get("DLSSNR.Depth"));
			if (depthRes) {
				fi.depth = depthRes;
				fi.depth_state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
				int32_t inverted = 0;
				pp->GetInt("DLSSNR.DepthInverted", &inverted);
				fi.depth_inverted = inverted ? 1u : 0u;
			}
			if (!g_mochi.motionLogged && (mvec || depthRes)) {
				g_mochi.motionLogged = true;
				uint32_t mfmt = 0, mw2 = 0, mh2 = 0, dfmt = 0, dw2 = 0, dh2 = 0;
				if (mvec) {
					const D3D12_RESOURCE_DESC md = mvec->GetDesc();
					mfmt = (uint32_t)md.Format;
					mw2 = (uint32_t)md.Width;
					mh2 = md.Height;
				}
				if (depthRes) {
					const D3D12_RESOURCE_DESC dd = depthRes->GetDesc();
					dfmt = (uint32_t)dd.Format;
					dw2 = (uint32_t)dd.Width;
					dh2 = dd.Height;
				}
				Log("motion: 已接上 motion=%p(%ux%u DXGI %u, subrect %ux%u, scale=%.3f/%.3f) "
					"depth=%p(%ux%u DXGI %u) inv=%u | flags=0x%X temporal=%u",
					(void*)mvec, mw2, mh2, mfmt, fi.motion_width, fi.motion_height,
					(double)fi.motion_scale_x, (double)fi.motion_scale_y,
					(void*)depthRes, dw2, dh2, dfmt,
					fi.depth_inverted, fi.flags, TemporalEnabled() ? 1u : 0u);
			}
		}

		// 时域（默认关）：只在诊断"运行时到底收不收 motion"时打开。
		// 注：TEMPORAL 已在上面和 flags 一起拼好，这里不再重复 OR。

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
					TrackJob(job.handle);
					LogRuntimeInfoPeriodic();
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
	Log("CALL ReleaseFeature (nrFrames=%ld nrFails=%ld passthrough=%ld panelCalls=%u)",
		g_nrFrames, g_nrFailures, g_copyFailures, g_mochi.panelCalls);
	if (g_mochi.api.Retire) {
		for (int i = 0; i < g_mochi.jobRingCount; ++i) {
			g_mochi.api.Retire(g_mochi.session, g_mochi.jobRing[i]);
		}
		g_mochi.jobRingCount = 0;
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
