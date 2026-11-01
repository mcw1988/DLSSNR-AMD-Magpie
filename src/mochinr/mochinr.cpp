// mochinr.addon64 — the mochizuki network inside the FFX upscale slot, before the upscaler.
//
// The submission contract is ported from lmxxf/dlss5-on-amd-9070xt-porting (MIT):
//
//   1. The ffxDispatch detour CAPTURES the descriptor and returns 0. Nothing runs on the GPU
//      there: at record time `color` still holds the previous frame, so touching it yet races.
//   2. A detour on ID3D12CommandQueue::ExecuteCommandLists (vtable slot 10) sees the batch the
//      host actually submits. When our armed list is in it, the batch is SPLIT: [0..at] first,
//      then the network, then [at+1..]. Queue order is the synchronisation, never a fence wait.
//   3. The network's lists are recorded on the HOST queue through our own allocator/list ring,
//      never on a queue of our own. A self-made queue cannot be ordered against the host's.
//   4. The original FFX dispatch is REPLAYED into our consumer list, so the upscaler reads the
//      denoised colour. The game's command list is never modified.
//
// Dispatch ABI (FFX 4.x x64, 432 bytes), cross-checked against the upstream add-on's struct:
//   0x000 type/flags      0x010 commandList
//   0x018 colour 0x048 depth 0x078 motion 0x0a8 exposure(1x1)
//   0x0d8 reactive 0x108 transparency 0x138 output
//   0x168 jitter(2f) 0x170 motionScale(2f) 0x178 render(2u) 0x180 upscale(2u)
//   0x188 sharpen/sharpness/frameTime/preExposure  0x198 reset/near/far/fov/meters  0x1ac flags
// ReShade 6 hands ImGui over as a function table: imgui.h must come FIRST so that reshade.hpp
// (which includes reshade_overlay.hpp) emits the inline definitions against that table.
#include <imgui.h>
#include <reshade.hpp>

#include <windows.h>
#include <d3d12.h>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <mutex>
#include <vector>
#include <deque>
#include <thread>

#include <MinHook.h>

#include "../../nrtest/LmxxfNrApi.h"
#include "MochizukiNrControls.h"   // the runtime's own exports beside LmxxfNrGetApi

using namespace reshade::api;

namespace {

// ---------------------------------------------------------------------------------------
// logging
// ---------------------------------------------------------------------------------------
FILE *g_log = nullptr;

void Log(const char *fmt, ...)
{
    if (!g_log) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fputc('\n', g_log);
    fflush(g_log);
}

std::wstring ModuleDir()
{
    wchar_t path[MAX_PATH]{};
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&ModuleDir), &self);
    GetModuleFileNameW(self, path, MAX_PATH);
    std::wstring s(path);
    const size_t slash = s.find_last_of(L'\\');
    return slash == std::wstring::npos ? L"." : s.substr(0, slash);
}

// ---------------------------------------------------------------------------------------
// settings
// ---------------------------------------------------------------------------------------
struct Settings
{
    bool enabled = true;
    int passes = 1;
    float modelScale = 1.0f;   // network render scale: 0.25..1.0 of the colour size
    float transfer = 1.0f;     // detail strength, 0..2
    float colour = 1.0f;       // colour strength, 0..4
    int resetEvery = 0;
    int debugView = 0;         // 0 normal, 2 = the network's own image
    int inflight = 3;          // jobs the runtime may keep alive

    // Flavour, ported from OpenNR's panel and mapped onto the RenoDX controls the runtime runs with.
    int style = 0;             // 0 standard, 1 natural, 2 cinematic
    float intensity = 1.0f;    // 0..2
    float localTone = 1.0f;    // 0..2
    float localStructure = 1.0f; // 0..2
    float skinStructure = -1.0f; // -1..2, -1 follows local structure
    int autoMask = 1;
    float highlightGuard = 2.0f; // 1..8
    float chromaClamp = 0.0f;    // 0..4 stops
    int detailStability = 0;
    int chainedHistory = 0;
    int followInputRes = 0;
    float pass2Transfer = 1.0f, pass2Colour = 1.0f;
    int pass2Follow = 1;
    float pass3Transfer = 1.0f, pass3Colour = 1.0f;
    int pass3Follow = 1;
};

Settings g_cfg;

void LoadSettings(const std::wstring &dir)
{
    const std::wstring path = dir + L"\\mochinr.ini";
    FILE *f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"r") != 0 || f == nullptr) return;
    char line[256];
    while (fgets(line, sizeof line, f)) {
        if (line[0] == '#' || line[0] == ';' || line[0] == '\n' || line[0] == '\r') continue;
        char key[64]{}, val[128]{};
        if (sscanf_s(line, "%63[^=]=%127s", key, (unsigned)sizeof key, val, (unsigned)sizeof val) != 2) continue;
        if (_stricmp(key, "enabled") == 0) g_cfg.enabled = atoi(val) != 0;
        else if (_stricmp(key, "passes") == 0) g_cfg.passes = atoi(val);
        else if (_stricmp(key, "model_scale") == 0) g_cfg.modelScale = (float)atof(val);
        else if (_stricmp(key, "strength") == 0) { g_cfg.transfer = (float)atof(val); g_cfg.colour = (float)atof(val); }
        else if (_stricmp(key, "transfer") == 0) g_cfg.transfer = (float)atof(val);
        else if (_stricmp(key, "colour") == 0) g_cfg.colour = (float)atof(val);
        else if (_stricmp(key, "reset_every") == 0) g_cfg.resetEvery = atoi(val);
        else if (_stricmp(key, "debug_view") == 0) g_cfg.debugView = atoi(val);
        else if (_stricmp(key, "inflight") == 0) g_cfg.inflight = atoi(val);
        else if (_stricmp(key, "style") == 0) g_cfg.style = atoi(val);
        else if (_stricmp(key, "intensity") == 0) g_cfg.intensity = (float)atof(val);
        else if (_stricmp(key, "local_tone") == 0) g_cfg.localTone = (float)atof(val);
        else if (_stricmp(key, "local_structure") == 0) g_cfg.localStructure = (float)atof(val);
        else if (_stricmp(key, "skin_structure") == 0) g_cfg.skinStructure = (float)atof(val);
        else if (_stricmp(key, "auto_mask") == 0) g_cfg.autoMask = atoi(val);
        else if (_stricmp(key, "highlight_guard") == 0) g_cfg.highlightGuard = (float)atof(val);
        else if (_stricmp(key, "chroma_clamp") == 0) g_cfg.chromaClamp = (float)atof(val);
        else if (_stricmp(key, "detail_stability") == 0) g_cfg.detailStability = atoi(val);
        else if (_stricmp(key, "chained_history") == 0) g_cfg.chainedHistory = atoi(val);
        else if (_stricmp(key, "follow_input_res") == 0) g_cfg.followInputRes = atoi(val);
    }
    fclose(f);
}

// ---------------------------------------------------------------------------------------
// FFX dispatch descriptor
// ---------------------------------------------------------------------------------------
struct FfxResource
{
    void *resource;
    uint32_t type, format, width, height, depth, mips, flags, usage, state, padding;
};
static_assert(sizeof(FfxResource) == 48, "FFX x64 resource layout");

struct DispatchDesc
{
    uint32_t type;      // 0x00010001 low 24 bits for an upscale
    uint32_t flags0;
    void *next;         // extension chain; upstream refuses to capture when it is set
    void *commandList;
    FfxResource res[7];              // colour, depth, motion, exposure, reactive, transparency, output
    float jitter[2], motionScale[2];
    uint32_t render[2], upscale[2];
    bool sharpen; char pad0[3];
    float sharpness, frameTime, preExposure;
    bool reset; char pad1[3];
    float nearPlane, farPlane, fov, meters;
    uint32_t flags;
};

static_assert(sizeof(DispatchDesc) == 432, "FFX dispatch ABI");
static_assert(offsetof(DispatchDesc, commandList) == 16, "desc layout");
static_assert(offsetof(DispatchDesc, res) == 24, "desc layout");
static_assert(offsetof(DispatchDesc, jitter) == 360, "desc layout");
static_assert(offsetof(DispatchDesc, render) == 376, "desc layout");
static_assert(offsetof(DispatchDesc, reset) == 408, "desc layout");
static_assert(offsetof(DispatchDesc, flags) == 428, "desc layout");

enum { kColor = 0, kDepth = 1, kMotion = 2, kExposure = 3, kReactive = 4, kTransparency = 5, kOutput = 6 };

// FFX 4.x FfxApiResourceState bits -> D3D12 states (same table as the upstream add-on).
bool FfxStateToD3D12(uint32_t s, D3D12_RESOURCE_STATES &out)
{
    switch (s) {
    case 1: out = D3D12_RESOURCE_STATE_COMMON; return true;
    case 2: out = D3D12_RESOURCE_STATE_UNORDERED_ACCESS; return true;
    case 4: out = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE; return true;
    case 8: out = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE; return true;
    case 12: out = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE; return true;
    case 16: out = D3D12_RESOURCE_STATE_COPY_SOURCE; return true;
    case 32: out = D3D12_RESOURCE_STATE_COPY_DEST; return true;
    case 20: out = D3D12_RESOURCE_STATE_GENERIC_READ; return true;
    case 128: out = D3D12_RESOURCE_STATE_PRESENT; return true;
    case 256: out = D3D12_RESOURCE_STATE_RENDER_TARGET; return true;
    default: return false;
    }
}

// ReShade wraps the list it hands the effect chain; a wrapped list must be unwrapped before it
// is compared with what the native queue actually submits.
constexpr GUID kUnwrappedList = {0x7f2c9a11, 0x3b4e, 0x4d6a, {0x81, 0x2f, 0x5e, 0x9c, 0xd3, 0x7a, 0x1b, 0x42}};

void *UnwrapList(void *list)
{
    auto *unknown = static_cast<IUnknown *>(list);
    if (unknown == nullptr) return nullptr;
    ID3D12GraphicsCommandList *native = nullptr;
    if (SUCCEEDED(unknown->QueryInterface(kUnwrappedList, reinterpret_cast<void **>(&native))) && native != nullptr) {
        native->Release();
        return native;
    }
    return list;
}

// ---------------------------------------------------------------------------------------
// host-queue submitter: our own allocator/list ring on the HOST queue
// ---------------------------------------------------------------------------------------
bool g_netRan = false;   // set by RunJob: the network composed this frame
unsigned g_lastWidth = 0, g_lastHeight = 0;   // the colour size of the last composed frame

struct Stats
{
    LARGE_INTEGER freq{};
    double prepare = 0, recordIn = 0, enqueue = 0, submit = 0;   // enqueue = the Vulkan fence round trip
    double total = 0, detour = 0;
    unsigned detours = 0, splits = 0, batches = 0;
    unsigned frames = 0, waits = 0;
    double waitMs = 0;
};

Stats g_stats;

double NowMs()
{
    static LARGE_INTEGER f = [] { LARGE_INTEGER v{}; QueryPerformanceFrequency(&v); return v; }();
    LARGE_INTEGER c{};
    QueryPerformanceCounter(&c);
    return 1000.0 * double(c.QuadPart) / double(f.QuadPart);
}

// A job must stay alive until the GPU has finished with the frame: Retire() releases the runtime's
// private buffers, and retiring immediately lets the next PrepareFrame reuse them under the GPU.
std::deque<std::pair<UINT64, void *>> g_retire;

constexpr UINT kRing = 32;

struct Submitter
{
    ID3D12Device *device = nullptr;        // device the FFX context records on
    ID3D12Device *queueDevice = nullptr;   // device that owns the queue (fence lives here)
    ID3D12CommandQueue *queue = nullptr;
    ID3D12Fence *fence = nullptr;
    HANDLE event = nullptr;
    UINT64 value = 0;
    ID3D12CommandAllocator *alloc[kRing]{};
    ID3D12GraphicsCommandList *list[kRing]{};
    UINT64 slotValue[kRing]{};
    bool poisoned = false;
    std::mutex mutex;

    // FFX creates its context on the device ReShade hands the effect chain. A command list from a
    // different device makes the provider fault while recording, so the list must be created on
    // the same device the captured list came from, and unwrapped when it reaches the native queue.
    void Reset()
    {
        std::lock_guard<std::mutex> guard(mutex);
        for (UINT i = 0; i < kRing; ++i) {
            if (list[i]) { list[i]->Release(); list[i] = nullptr; }
            if (alloc[i]) { alloc[i]->Release(); alloc[i] = nullptr; }
            slotValue[i] = 0;
        }
        if (fence) { fence->Release(); fence = nullptr; }
        if (event) { CloseHandle(event); event = nullptr; }
        if (device) { device->Release(); device = nullptr; }
        if (queueDevice) { queueDevice->Release(); queueDevice = nullptr; }
        if (queue) { queue->Release(); queue = nullptr; }
        value = 0;
        poisoned = false;
    }

    bool Create(ID3D12CommandQueue *q, ID3D12Device *recordDevice)
    {
        if (queue != nullptr) return queue == q;   // created once; never rebuilt under live submits
        if (q == nullptr) return false;
        if (FAILED(q->GetDevice(IID_PPV_ARGS(&queueDevice))) || queueDevice == nullptr) return false;
        if (recordDevice != nullptr) {
            device = recordDevice;
            device->AddRef();
        } else {
            device = queueDevice;
            device->AddRef();
        }
        queue = q;
        queue->AddRef();
        for (UINT i = 0; i < kRing; ++i) {
            if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc[i])))) return false;
            if (FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc[i], nullptr,
                                                 IID_PPV_ARGS(&list[i])))) return false;
            list[i]->Close();
        }
        if (FAILED(queueDevice->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)))) return false;
        event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        return event != nullptr;
    }

    // A list created through the ReShade device is wrapped; the native queue only takes the real one.
    void ExecuteOwnedList(ID3D12GraphicsCommandList *owns)
    {
        ID3D12GraphicsCommandList *native = nullptr;
        if (SUCCEEDED(static_cast<IUnknown *>(owns)->QueryInterface(kUnwrappedList, reinterpret_cast<void **>(&native))) &&
            native != nullptr) {
            ID3D12CommandList *items[] = {native};
            queue->ExecuteCommandLists(1, items);   // re-enters our detour: guarded by the caller
            native->Release();
            return;
        }
        ID3D12CommandList *items[] = {owns};
        queue->ExecuteCommandLists(1, items);
    }

    void WaitSlot(UINT64 target)
    {
        if (fence == nullptr) return;
        if (fence->GetCompletedValue() >= target) return;
        if (FAILED(fence->SetEventOnCompletion(target, event))) return;
        const double t0 = NowMs();
        WaitForSingleObject(event, 4000);
        ++g_stats.waits;
        g_stats.waitMs += NowMs() - t0;
    }

    void Fail(const char *stage, HRESULT hr)
    {
        poisoned = true;
        unsigned long removed = 0;
        if (queueDevice != nullptr) removed = (unsigned long)queueDevice->GetDeviceRemovedReason();
        Log("!! submit failed at %s hr=0x%08lx slot=%u network=%d removed=0x%08lx", stage, (unsigned long)hr,
            UINT(value % kRing), g_netRan ? 1 : 0, removed);
    }

    template <class Record>
    bool Submit(Record record)
    {
        std::lock_guard<std::mutex> guard(mutex);
        if (poisoned) return false;
        if (queue == nullptr) return false;
        const UINT slot = UINT(value % kRing);
        if (slotValue[slot] != 0) WaitSlot(slotValue[slot]);
        HRESULT hr = alloc[slot]->Reset();
        if (FAILED(hr)) { Fail("allocator reset", hr); return false; }
        hr = list[slot]->Reset(alloc[slot], nullptr);
        if (FAILED(hr)) { Fail("list reset", hr); return false; }
        record(list[slot]);
        hr = list[slot]->Close();
        if (FAILED(hr)) {
            // The runtime's Record* may close the list it was handed. Closing twice is E_INVALIDARG;
            // treat that as "already closed" and submit, rather than poisoning the frame.
            static int said = 0;
            if (said < 3) { ++said; Log("!! close rejected hr=0x%08lx slot=%u (assuming already closed)", (unsigned long)hr, slot); }
            if (hr != E_INVALIDARG) { Fail("list close", hr); return false; }
        }
        ++value;
        ExecuteOwnedList(list[slot]);
        hr = queue->Signal(fence, value);
        if (FAILED(hr)) { Fail("signal", hr); return false; }
        slotValue[slot] = value;
        return true;
    }
};

Submitter g_sub;

// The real ffxDispatch export, one per module we hook. Sharing a single pointer between two
// hooked modules makes one forward into the other and recurse (crash inside the upscaler DLL).
using DispatchFn = uint32_t (*)(void **, const DispatchDesc *);

thread_local int t_dispatchDepth = 0;

// Nesting guard. The upscaler's export forwards into the loader's, and both are hooked; whatever
// the outer call decided, the inner one must simply run. (The upstream add-on keeps shim_depth
// for the same reason.) Also used around the replay, which re-enters the other hook.
struct DepthGuard
{
    DepthGuard() { ++t_dispatchDepth; }
    ~DepthGuard() { --t_dispatchDepth; }
};


// ---------------------------------------------------------------------------------------
// armed job
// ---------------------------------------------------------------------------------------
struct Job
{
    bool armed = false;
    uint64_t frame = 0;
    uint32_t missed = 0;           // submissions the armed list was not part of
    void *list = nullptr;          // native list the host will submit
    void *ctx = nullptr;           // the FFX context value (not the caller's void**)
    DispatchDesc desc{};
    ID3D12Resource *res[7]{};
    D3D12_RESOURCE_STATES state[7]{};
    bool stateKnown = false;
    bool reset = false;
    uint32_t width = 0, height = 0;
    DispatchFn original = nullptr;  // the export this capture came through
    ID3D12Device *recordDevice = nullptr;  // the device the FFX context records on
};

Job g_pending;
std::mutex g_jobMutex;

std::vector<std::pair<UINT64, ID3D12Resource *>> g_deferred;
std::mutex g_deferredMutex;   // RunJob and ReleaseDeferred can run on different threads

void ReleaseDeferred()
{
    if (g_sub.fence == nullptr) return;
    const UINT64 done = g_sub.fence->GetCompletedValue();
    std::lock_guard<std::mutex> guard(g_deferredMutex);
    for (size_t i = 0; i < g_deferred.size();) {
        if (g_deferred[i].first <= done) {
            g_deferred[i].second->Release();
            g_deferred.erase(g_deferred.begin() + i);
        } else {
            ++i;
        }
    }
}

void ReleaseJob(Job &j)
{
    for (auto *r : j.res) if (r != nullptr) r->Release();
    if (j.recordDevice != nullptr) j.recordDevice->Release();
    j = Job{};
}

// ---------------------------------------------------------------------------------------
// network session
// ---------------------------------------------------------------------------------------
ID3D12CommandQueue *g_queueRef = nullptr;   // the host's DIRECT queue, seen at present
ID3D12Device *g_devRef = nullptr;           // that queue's device (what the runtime binds to)

struct Nr
{
    HMODULE module = nullptr;
    LmxxfNrApi api{};
    void *session = nullptr;
    volatile long state = 0;   // 0 idle, 1 creating, 2 ready, 3 failed
    uint64_t frame = 0, ran = 0, skipped = 0;
    bool loggedDims = false;

    // The runtime's own exports: controls take effect on the next PrepareFrame, info answers "what is it doing".
    PFN_MochizukiNrSetControls setControls = nullptr;
    PFN_MochizukiNrGetInfo getInfo = nullptr;
    MochizukiNrControls controls{};
    bool controlsReady = false;
    uint32_t infoStructSize = 0;
    MochizukiNrInfo info{};
    ULONGLONG infoAt = 0;
};

Nr g_nr;

void InitSession(std::wstring dir)
{
    Log("nr: loading runtime from %ls", dir.c_str());
    g_nr.module = LoadLibraryExW((dir + L"\\MochizukiNrRuntime.dll").c_str(), nullptr,
                                 LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (g_nr.module == nullptr) { Log("nr: LoadLibrary err=%lu", GetLastError()); g_nr.state = 3; return; }
    auto getApi = reinterpret_cast<int32_t (*)(uint32_t, LmxxfNrApi *)>(GetProcAddress(g_nr.module, "LmxxfNrGetApi"));
    if (getApi == nullptr) { Log("nr: LmxxfNrGetApi missing"); g_nr.state = 3; return; }
    g_nr.api.struct_size = sizeof g_nr.api;
    if (getApi(LMXXF_NR_ABI_VERSION, &g_nr.api) != LMXXF_NR_OK) { Log("nr: GetApi rejected"); g_nr.state = 3; return; }

    g_nr.setControls = reinterpret_cast<PFN_MochizukiNrSetControls>(GetProcAddress(g_nr.module, "MochizukiNrSetControls"));
    g_nr.getInfo = reinterpret_cast<PFN_MochizukiNrGetInfo>(GetProcAddress(g_nr.module, "MochizukiNrGetInfo"));
    g_nr.infoStructSize = sizeof(MochizukiNrInfo);
    if (auto defaults = reinterpret_cast<PFN_MochizukiNrGetControlDefaults>(
            GetProcAddress(g_nr.module, "MochizukiNrGetControlDefaults"))) {
        g_nr.controls = MochizukiNrControls{};
        g_nr.controls.struct_size = sizeof g_nr.controls;
        defaults(&g_nr.controls);
        g_nr.controls.struct_size = sizeof g_nr.controls;
        g_nr.controlsReady = true;
    }
    Log("nr: controls %s, info %s", g_nr.setControls ? "yes" : "no", g_nr.getInfo ? "yes" : "no");

    LmxxfNrCapabilities caps{};
    caps.struct_size = sizeof caps;
    if (g_nr.api.QueryCapabilities && g_nr.api.QueryCapabilities(&caps) == LMXXF_NR_OK)
        Log("nr: caps max %ux%u hip %u", caps.max_input_width, caps.max_input_height, caps.hip_ready);

    LmxxfNrCreateInfo ci{};
    ci.struct_size = sizeof ci;
    ci.device = g_devRef;
    ci.queue = g_queueRef;
    ci.assets_directory = dir.c_str();
    ci.flags = LMXXF_NR_CREATE_FLAG_ZERO_OUTPUT_FALLBACK | LMXXF_NR_CREATE_FLAG_ACO;

    void *session = nullptr;
    int32_t rc = g_nr.api.Create(&ci, &session);
    if (rc != LMXXF_NR_OK) {
        char err[512]{};
        if (g_nr.api.GetLastError) g_nr.api.GetLastError(err, sizeof err);
        Log("nr: Create rc=%d %s", rc, err);
        g_nr.state = 3;
        return;
    }
    Log("nr: created, compiling pipelines");
    rc = g_nr.api.PrepareSession(session);
    if (rc != LMXXF_NR_OK) {
        char err[512]{};
        if (g_nr.api.GetLastError) g_nr.api.GetLastError(err, sizeof err);
        Log("nr: PrepareSession rc=%d %s", rc, err);
        g_nr.state = 3;
        return;
    }
    g_nr.session = session;
    g_nr.state = 2;
    Log("nr: session ready");
}

// ---------------------------------------------------------------------------------------
// per-frame work, executed at the split point of a host submission
// ---------------------------------------------------------------------------------------
// POD-only so SEH is legal here: a fault inside the SDK must be a log line, not a dead process.
int ReplayFilter(EXCEPTION_POINTERS *info)
{
    static int said = 0;
    if (said < 3) {
        ++said;
        const EXCEPTION_RECORD *r = info->ExceptionRecord;
        unsigned long long kind = r->NumberParameters > 0 ? r->ExceptionInformation[0] : 0;
        unsigned long long addr = r->NumberParameters > 1 ? r->ExceptionInformation[1] : 0;
        Log("!! replay fault code=0x%08lx at=%p %s addr=0x%llx", (unsigned long)r->ExceptionCode, r->ExceptionAddress,
            kind == 0 ? "read" : kind == 1 ? "write" : kind == 8 ? "execute" : "?", addr);
    }
    return EXCEPTION_EXECUTE_HANDLER;
}

bool ReplayGuarded(void **ctx, DispatchDesc *d, DispatchFn original)
{
    __try {
        original(ctx, d);
        return true;
    } __except (ReplayFilter(GetExceptionInformation())) {
        return false;
    }
}

// The RenoDX stage is what the runtime runs by default (renodx = 1), and in that stage the model
// controls -- passes, detail/colour strength, model scale -- are documented as unused: the reno_*
// fields take over. That is why a panel writing only LmxxfNrFrameInfo changed nothing.
void SendControls(bool force)
{
    if (g_nr.setControls == nullptr || !g_nr.controlsReady || g_nr.session == nullptr) return;
    static Settings sent{};
    static bool once = false;
    if (!force && once && sent.passes == g_cfg.passes && sent.transfer == g_cfg.transfer &&
        sent.colour == g_cfg.colour && sent.modelScale == g_cfg.modelScale && sent.style == g_cfg.style &&
        sent.intensity == g_cfg.intensity && sent.localTone == g_cfg.localTone &&
        sent.localStructure == g_cfg.localStructure && sent.skinStructure == g_cfg.skinStructure &&
        sent.autoMask == g_cfg.autoMask && sent.highlightGuard == g_cfg.highlightGuard &&
        sent.chromaClamp == g_cfg.chromaClamp && sent.detailStability == g_cfg.detailStability &&
        sent.chainedHistory == g_cfg.chainedHistory && sent.followInputRes == g_cfg.followInputRes &&
        sent.pass2Transfer == g_cfg.pass2Transfer && sent.pass2Colour == g_cfg.pass2Colour &&
        sent.pass2Follow == g_cfg.pass2Follow && sent.pass3Transfer == g_cfg.pass3Transfer &&
        sent.pass3Colour == g_cfg.pass3Colour && sent.pass3Follow == g_cfg.pass3Follow)
        return;

    MochizukiNrControls c = g_nr.controls;   // the runtime's own defaults, captured at load
    c.struct_size = sizeof c;
    c.renodx = 1;
    c.reno_nr_enabled = 1;
    c.reno_passes = (uint32_t)(g_cfg.passes < 1 ? 1 : g_cfg.passes > 3 ? 3 : g_cfg.passes);
    c.reno_unlock_passes = g_cfg.passes > 1 ? 1u : 0u;
    c.reno_transfer_strength = g_cfg.transfer;
    c.reno_color_strength = g_cfg.colour;
    c.reno_resolution_scale = g_cfg.modelScale;

    // Flavour, in the order OpenNR's own panel offers it.
    c.reno_style = (uint32_t)(g_cfg.style < 0 ? 0 : g_cfg.style > 2 ? 2 : g_cfg.style);
    c.reno_intensity = g_cfg.intensity;
    c.reno_local_tone = g_cfg.localTone;
    c.reno_local_structure = g_cfg.localStructure;
    c.reno_skin_structure = g_cfg.skinStructure;
    c.reno_auto_mask = g_cfg.autoMask ? 1u : 0u;
    c.reno_style_highlight_guard = g_cfg.highlightGuard;
    c.reno_chroma_clamp_stops = g_cfg.chromaClamp;
    c.reno_detail_stability = g_cfg.detailStability ? 1u : 0u;
    c.reno_chained_history = g_cfg.chainedHistory ? 1u : 0u;
    c.reno_follow_input_res = g_cfg.followInputRes ? 1u : 0u;
    // A later pass either follows pass 1 or carries its own transfer/colour.
    c.reno_pass_follow_0 = g_cfg.pass2Follow ? 1u : 0u;
    c.reno_pass_transfer_0 = g_cfg.pass2Transfer;
    c.reno_pass_color_0 = g_cfg.pass2Colour;
    c.reno_pass_follow_1 = g_cfg.pass3Follow ? 1u : 0u;
    c.reno_pass_transfer_1 = g_cfg.pass3Transfer;
    c.reno_pass_color_1 = g_cfg.pass3Colour;

    const int32_t rc = g_nr.setControls(g_nr.session, &c);
    Log("nr: controls rc=%d passes=%u scale=%.2f transfer=%.2f colour=%.2f style=%u tone=%.2f struct=%.2f "
        "skin=%.2f guard=%.1f", rc, c.reno_passes, c.reno_resolution_scale, c.reno_transfer_strength,
        c.reno_color_strength, c.reno_style, c.reno_local_tone, c.reno_local_structure, c.reno_skin_structure,
        c.reno_style_highlight_guard);
    sent = g_cfg;
    once = true;
}

void RefreshInfo()
{
    if (g_nr.getInfo == nullptr || g_nr.session == nullptr) return;
    const ULONGLONG now = GetTickCount64();
    if (now - g_nr.infoAt < 400) return;
    g_nr.infoAt = now;
    MochizukiNrInfo info{};
    info.struct_size = g_nr.infoStructSize;
    if (g_nr.getInfo(g_nr.session, &info) == LMXXF_NR_OK) g_nr.info = info;
}

std::mutex g_runMutex;   // a game may submit from more than one thread; one job at a time here

bool RunJob(Job &j)
{
    std::lock_guard<std::mutex> serial(g_runMutex);
    const double tFrameStart = NowMs();
    RefreshInfo();
    auto *color = j.res[kColor];
    const D3D12_RESOURCE_STATES colorState = j.stateKnown ? j.state[kColor] : D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

    bool ran = false;
    void *jobHandle = nullptr;
    ID3D12Resource *privateOutput = nullptr;

    if (g_cfg.enabled && g_nr.state == 2 && color != nullptr) {
        LmxxfNrFrameInfo fi{};
        fi.struct_size = sizeof fi;
        fi.frame_id = ++g_nr.frame;
        fi.color = color;
        fi.color_width = j.width;
        fi.color_height = j.height;
        fi.color_state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        fi.flags = LMXXF_NR_FRAME_FLAG_STRENGTH;
        if (g_cfg.debugView != 0) {
            fi.flags |= LMXXF_NR_FRAME_FLAG_DEBUG_VIEW;
            fi.debug_view = (uint32_t)g_cfg.debugView;
        }
        fi.transfer_strength = g_cfg.transfer;
        fi.color_strength = g_cfg.colour;
        fi.model_scale = g_cfg.modelScale;
        fi.passes = (uint32_t)g_cfg.passes;
        fi.reset = j.reset ? 1u : 0u;

        const bool talk = g_nr.frame <= 5 || g_nr.frame % 300 == 0;
        LmxxfNrJob nrJob{};
        nrJob.struct_size = sizeof nrJob;
        SendControls(false);
        const double tPrep = NowMs();
        int32_t rc = g_nr.api.PrepareFrame(g_nr.session, &fi, &nrJob);
        g_stats.prepare += NowMs() - tPrep;
        if (rc != LMXXF_NR_OK) {
            if (talk) {
                char st[256]{};
                if (g_nr.api.GetStatus) g_nr.api.GetStatus(g_nr.session, st, sizeof st);
                Log("nr: PrepareFrame rc=%d f=%llu %s", rc, (unsigned long long)g_nr.frame, st);
            }
            ++g_nr.skipped;
        } else {
            privateOutput = static_cast<ID3D12Resource *>(nrJob.private_output);
            bool sizeOk = false;
            if (privateOutput != nullptr) {
                const D3D12_RESOURCE_DESC pd = privateOutput->GetDesc();
                const D3D12_RESOURCE_DESC cd = color->GetDesc();
                sizeOk = pd.Width == cd.Width && pd.Height == cd.Height;
                static bool saidFmt = false;
                if (!saidFmt) {
                    saidFmt = true;
                    Log("formats: colour=%u private=%u %llux%u vs %llux%u", (unsigned)cd.Format, (unsigned)pd.Format,
                        (unsigned long long)cd.Width, cd.Height, (unsigned long long)pd.Width, pd.Height);
                    Log("buffers: colour=%p private=%p same=%d debug_view=%d", (void *)color, (void *)privateOutput,
                        color == privateOutput ? 1 : 0, g_cfg.debugView);
                }
                if (!sizeOk && !g_nr.loggedDims) {
                    g_nr.loggedDims = true;
                    Log("nr: private output %llux%u != colour %llux%u", (unsigned long long)pd.Width, pd.Height,
                        (unsigned long long)cd.Width, cd.Height);
                }
            }
            if (!sizeOk) {
                if (privateOutput == nullptr && talk) Log("nr: no private output");
                if (privateOutput != nullptr) g_nr.api.CancelUnsubmitted(g_nr.session, nrJob.handle);
                ++g_nr.skipped;
            } else {
                bool recorded = true;
                const bool submitted = g_sub.Submit([&](ID3D12GraphicsCommandList *c) {
                    // A transition from a state to itself is an invalid call in D3D12 and latches
                    // DXGI_ERROR_INVALID_CALL on the device, which then fails every later submit.
                    if (colorState != D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE) {
                        D3D12_RESOURCE_BARRIER b{};
                        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                        b.Transition.pResource = color;
                        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                        b.Transition.StateBefore = colorState;
                        b.Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
                        c->ResourceBarrier(1, &b);
                    }
                    const double t0 = NowMs();
                    if (g_nr.api.RecordInputs(g_nr.session, nrJob.handle, c) != LMXXF_NR_OK) recorded = false;
                    g_stats.recordIn += NowMs() - t0;
                });
                if (!submitted || !recorded) {
                    if (talk) Log("nr: producer failed (submitted=%d recorded=%d)", submitted ? 1 : 0, recorded ? 1 : 0);
                    g_nr.api.CancelUnsubmitted(g_nr.session, nrJob.handle);
                    ++g_nr.skipped;
                } else if ([&] {
                               const double t0 = NowMs();
                               const int32_t hipRc = g_nr.api.EnqueueHip(g_nr.session, nrJob.handle, g_sub.queue);
                               g_stats.enqueue += NowMs() - t0;
                               if (hipRc == LMXXF_NR_OK) return false;
                               return true;
                           }()) {
                    char err[256]{};
                    if (g_nr.api.GetLastError) g_nr.api.GetLastError(err, sizeof err);
                    Log("nr: EnqueueHip failed %s", err);
                    g_nr.api.CancelUnsubmitted(g_nr.session, nrJob.handle);
                    ++g_nr.skipped;
                } else {
                    ran = true;
                    jobHandle = nrJob.handle;
                }
            }
        }
    }

    // Consumer: compose the network output back into colour, then replay the upscaler on it.
    g_netRan = ran;
    g_lastWidth = j.width;
    g_lastHeight = j.height;
    const UINT64 mark = g_sub.value + 1;

    // The runtime's Record* may close the list it is handed, so each stage gets its own list:
    // (a) the network's output, (b) compose back into colour, (c) the upscaler replay.
    bool replayOk = true;
    const double tSub = NowMs();
    const bool submitted = g_sub.Submit([&](ID3D12GraphicsCommandList *c) {
        if (ran && privateOutput != nullptr) {
            const int32_t outRc = g_nr.api.RecordOutputs(g_nr.session, jobHandle, c);
            if (outRc != LMXXF_NR_OK) Log("!! RecordOutputs rc=%d", outRc);
        }
        // No copy back into colour: the upscaler is simply pointed at the network's output. That is
        // what the upstream add-on does (fed = neural_oneshot.Delivered()), and it removes every
        // state assumption about a texture we do not own -- the copy was what eventually hung the
        // driver after the network rebuilt for a new resolution.
        DispatchDesc d = j.desc;
        d.commandList = c;
        if (ran && privateOutput != nullptr) {
            d.res[kColor].resource = privateOutput;
            d.res[kColor].width = j.width;
            d.res[kColor].height = j.height;
            d.res[kColor].state = 4;   // FFX NON_PIXEL_SHADER_RESOURCE, what the provider expects
        }
        if (j.ctx != nullptr && j.original != nullptr) {
            DepthGuard depth;   // the replay re-enters the other hooked export
            replayOk = ReplayGuarded(&j.ctx, &d, j.original);
        }
    });

    auto drainRetire = [] {
        if (g_sub.fence == nullptr) return;
        const UINT64 done = g_sub.fence->GetCompletedValue();
        while (!g_retire.empty() && g_retire.front().first <= done) {
            if (g_nr.api.Retire) g_nr.api.Retire(g_nr.session, g_retire.front().second);
            g_retire.pop_front();
        }
    };
    drainRetire();
    if (ran && jobHandle != nullptr && submitted) g_retire.emplace_back(g_sub.value, jobHandle);
    else if (ran && jobHandle != nullptr) g_nr.api.Retire(g_nr.session, jobHandle);

    // Keep only a few frames of GPU work queued: a deep queue makes the present side drop frames
    // even though the GPU is idle. The runtime's own guidance is 2-3 frames in flight.
    const size_t limit = (size_t)(g_cfg.inflight < 2 ? 2 : g_cfg.inflight);
    if (g_retire.size() > limit && g_sub.fence != nullptr) {
        const UINT64 target = g_retire.front().first;
        if (g_sub.fence->GetCompletedValue() < target) {
            static HANDLE backpressure = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            if (backpressure != nullptr) {
                const double t0 = NowMs();
                g_sub.fence->SetEventOnCompletion(target, backpressure);
                WaitForSingleObject(backpressure, 200);
                ++g_stats.waits;
                g_stats.waitMs += NowMs() - t0;
            }
        }
        drainRetire();
    }

    g_stats.submit += NowMs() - tSub;
    g_stats.frames++;
    g_stats.total += NowMs() - tFrameStart;
    if (g_stats.frames % 300 == 0) {
        const double n = double(g_stats.frames);
        Log("perf: frames=%u cpu/frame=%.3f ms (prepare %.3f, recIn %.3f, hip %.3f, submit %.3f) waits=%u %.2f ms",
            g_stats.frames, g_stats.total / n, g_stats.prepare / n, g_stats.recordIn / n, g_stats.enqueue / n,
            g_stats.submit / n, g_stats.waits, g_stats.waitMs);
        Log("detour: calls=%u splits=%u batches=%u avg=%.3f ms/frame",
            g_stats.detours, g_stats.splits, g_stats.batches,
            g_stats.detours ? g_stats.detour / double(g_stats.detours) : 0.0);
        g_stats = Stats{};
    }

    if (ran) {
        ++g_nr.ran;
        if (g_nr.ran == 1) Log("first composing frame f=%llu %ux%u", (unsigned long long)g_nr.frame, j.width, j.height);
        if (g_nr.ran <= 5 || g_nr.ran % 300 == 0)
            Log("nr: ran f=%llu ok=%llu skip=%llu %ux%u", (unsigned long long)g_nr.frame,
                (unsigned long long)g_nr.ran, (unsigned long long)g_nr.skipped, j.width, j.height);
    }

    static bool saidReplay = false;
    if (!replayOk && !saidReplay) { saidReplay = true; Log("!! replay faulted inside the SDK for ctx=%p", j.ctx); }
    if (!submitted) Log("!! consumer submit failed: the upscaler was swallowed this frame");
    {
        std::lock_guard<std::mutex> guard(g_deferredMutex);
        for (auto *r : j.res) if (r != nullptr) g_deferred.emplace_back(mark, r);
    }
    if (j.recordDevice != nullptr) { j.recordDevice->Release(); }   // was leaking one ref per frame
    j = Job{};
    return submitted;
}

// ---------------------------------------------------------------------------------------
// hooks
// ---------------------------------------------------------------------------------------

// A capture that a submission never claimed must still be honoured: replay it right away rather
// than let a swallowed upscaler call go missing (that alone can take the SDK down).
void ReplayStale(Job &stale)
{
    if (stale.ctx != nullptr && stale.original != nullptr && g_sub.queue != nullptr) {
        const UINT64 mark = g_sub.value + 1;
        g_sub.Submit([&](ID3D12GraphicsCommandList *c) {
            DispatchDesc d = stale.desc;
            d.commandList = c;
            DepthGuard depth;
            ReplayGuarded(&stale.ctx, &d, stale.original);
        });
        {
            std::lock_guard<std::mutex> guard(g_deferredMutex);
            for (auto *r : stale.res) if (r != nullptr) g_deferred.emplace_back(mark, r);
        }
    } else {
        for (auto *r : stale.res) if (r != nullptr) r->Release();
    }
    stale = Job{};
}

uint32_t DispatchCore(void **context, const DispatchDesc *desc, DispatchFn original)
{
    if (original == nullptr) return 0;
    if (t_dispatchDepth > 0) return original(context, desc);
    if (desc == nullptr || (desc->type & 0x00ffffffu) != 0x00010001u) return original(context, desc);

    DepthGuard depth;

    static uint64_t seen = 0;
    ++seen;

    const bool canRun = g_cfg.enabled && g_nr.state == 2 && g_queueRef != nullptr;
    if (!canRun || desc->res[kColor].resource == nullptr || desc->res[kOutput].resource == nullptr)
        return original(context, desc);

    Job job;
    if (desc->next != nullptr) {
        static bool saidNext = false;
        if (!saidNext) { saidNext = true; Log("capture refused: descriptor has an extension chain (next=%p)", desc->next); }
        return original(context, desc);
    }
    job.armed = true;
    job.frame = seen;
    job.list = UnwrapList(desc->commandList);
    if (context != nullptr) job.ctx = *context;   // dereference now; the caller's slot may not outlive the call
    if (desc->commandList != nullptr) {
        // The device must come from the list FFX itself was given. GetDevice() is a vtable method
        // and works through ReShade's wrapper; QueryInterface(IID_ID3D12Device) does not.
        ID3D12Device *recDev = nullptr;
        if (SUCCEEDED(static_cast<ID3D12GraphicsCommandList *>(desc->commandList)->GetDevice(IID_PPV_ARGS(&recDev))) &&
            recDev != nullptr)
            job.recordDevice = recDev;   // borrowed reference, released with the job
    }
    job.desc = *desc;
    job.original = original;
    job.width = desc->res[kColor].width;
    job.height = desc->res[kColor].height;
    job.reset = g_nr.frame == 0 || (g_cfg.resetEvery > 0 && g_nr.frame % g_cfg.resetEvery == 0);

    bool known = true;
    for (int i = 0; i < 7; ++i) {
        job.res[i] = static_cast<ID3D12Resource *>(job.desc.res[i].resource);
        if (job.res[i] != nullptr && !FfxStateToD3D12(job.desc.res[i].state, job.state[i])) known = false;
    }
    job.stateKnown = known;

    Job stale;
    {
        std::lock_guard<std::mutex> guard(g_jobMutex);
        if (g_pending.armed) { stale = g_pending; g_pending = Job{}; }
        for (auto *r : job.res) if (r != nullptr) r->AddRef();
        g_pending = job;
    }
    if (stale.armed) ReplayStale(stale);

    if (seen <= 5 || seen % 300 == 0)
        Log("cap #%llu ctx=%p list=%p %ux%u known=%d", (unsigned long long)seen, context, job.list,
            job.width, job.height, known ? 1 : 0);
    return 0;   // replayed at the split point
}

void StartSessionOnce();

using ExecuteFn = void(STDMETHODCALLTYPE *)(ID3D12CommandQueue *, UINT, ID3D12CommandList *const *);
ExecuteFn g_originalExecute = nullptr;
thread_local bool t_ownSubmit = false;

void STDMETHODCALLTYPE ExecuteDetour(ID3D12CommandQueue *queue, UINT count, ID3D12CommandList *const *lists)
{
    struct DetourTimer
    {
        double t0 = NowMs();
        ~DetourTimer()
        {
            g_stats.detour += NowMs() - t0;
            ++g_stats.detours;
        }
    } timer;
    ++g_stats.batches;

    if (t_ownSubmit || queue == nullptr || lists == nullptr || count == 0) {
        g_originalExecute(queue, count, lists);
        return;
    }

    Job job;
    UINT at = count;
    {
        std::lock_guard<std::mutex> guard(g_jobMutex);
        if (!g_pending.armed) {
            g_originalExecute(queue, count, lists);
            return;
        }
        for (UINT i = 0; i < count && i < 64; ++i) {
            if (reinterpret_cast<void *>(lists[i]) == g_pending.list) { at = i; break; }
        }
        if (at == count) {
            // Not part of this batch (another thread, or a later submission): keep it armed.
            if (++g_pending.missed > 240) {
                Log("drop #%llu: never submitted (list %p)", (unsigned long long)g_pending.frame, g_pending.list);
                ReleaseJob(g_pending);
            }
            g_originalExecute(queue, count, lists);
            return;
        }
        job = g_pending;
        g_pending = Job{};
    }

    // Built once, on the device the FFX context records on, before anything of ours is in flight.
    if (g_sub.poisoned) {
        Log("!! rebuilding the submitter after a failed submit");
        g_sub.Reset();
    }
    if (g_sub.queue == nullptr && !g_sub.Create(queue, job.recordDevice)) Log("!! submitter create failed");
    if (g_sub.queue != nullptr && g_sub.value == 0 && g_sub.device != nullptr)
        Log("devices: queue=%p ffx=%p -> using %p%s", g_sub.queueDevice, job.recordDevice, g_sub.device,
            g_sub.device == g_sub.queueDevice ? " (same as queue: FFX device unknown)" : " (FFX device)");

    ++g_stats.splits;

    // 1) everything the host recorded up to and including the upscaler's list
    g_originalExecute(queue, at + 1, lists);
    // 2) the network, then the upscaler replay, in our own lists on this queue
    t_ownSubmit = true;
    RunJob(job);
    t_ownSubmit = false;
    // 3) whatever followed it
    if (at + 1 < count) g_originalExecute(queue, count - at - 1, lists + at + 1);
}

// One detour per hooked module: each keeps its OWN original, otherwise the loader's original
// ends up standing in for the upscaler's and the two forward into each other.
constexpr int kMaxHookSlots = 3;
DispatchFn g_originalDispatch[kMaxHookSlots]{};
void *g_hookedTarget[kMaxHookSlots]{};
int g_hookSlots = 0;

uint32_t DispatchDetour0(void **c, const DispatchDesc *d) { return DispatchCore(c, d, g_originalDispatch[0]); }
uint32_t DispatchDetour1(void **c, const DispatchDesc *d) { return DispatchCore(c, d, g_originalDispatch[1]); }
uint32_t DispatchDetour2(void **c, const DispatchDesc *d) { return DispatchCore(c, d, g_originalDispatch[2]); }
LPVOID const g_detours[kMaxHookSlots] = {reinterpret_cast<LPVOID>(&DispatchDetour0),
                                         reinterpret_cast<LPVOID>(&DispatchDetour1),
                                         reinterpret_cast<LPVOID>(&DispatchDetour2)};

bool g_dispatchInstalled = false;

void InstallDispatch()
{
    if (g_dispatchInstalled) return;
    for (const wchar_t *name : {L"amd_fidelityfx_upscaler_dx12.dll", L"amd_fidelityfx_loader_dx12.dll",
                                L"amd_fidelityfx_dx12.dll"}) {
        if (g_hookSlots >= kMaxHookSlots) break;
        HMODULE mod = GetModuleHandleW(name);
        if (mod == nullptr) continue;
        void *target = reinterpret_cast<void *>(GetProcAddress(mod, "ffxDispatch"));
        if (target == nullptr) continue;
        bool already = false;
        for (int i = 0; i < g_hookSlots; ++i) if (g_hookedTarget[i] == target) already = true;
        if (already) continue;
        const int slot = g_hookSlots;
        if (MH_CreateHook(target, g_detours[slot],
                          reinterpret_cast<LPVOID *>(&g_originalDispatch[slot])) != MH_OK) {
            Log("MH_CreateHook(ffxDispatch) failed on %ls", name);
            continue;
        }
        if (MH_EnableHook(target) != MH_OK) { Log("MH_EnableHook failed %ls", name); continue; }
        g_hookedTarget[slot] = target;
        ++g_hookSlots;
        Log("hooked ffxDispatch @%p (%ls)", target, name);
    }
    g_dispatchInstalled = g_hookSlots > 0;
}

void InstallExecute(ID3D12CommandQueue *queue)
{
    if (queue == nullptr || g_originalExecute != nullptr) return;
    if (queue->GetDesc().Type != D3D12_COMMAND_LIST_TYPE_DIRECT) return;
    void **vtable = *reinterpret_cast<void ***>(queue);
    if (vtable == nullptr) return;
    void *target = vtable[10];   // ID3D12CommandQueue::ExecuteCommandLists
    if (target == nullptr) return;
    if (MH_CreateHook(target, reinterpret_cast<LPVOID>(&ExecuteDetour),
                      reinterpret_cast<LPVOID *>(&g_originalExecute)) != MH_OK) {
        Log("MH_CreateHook(ExecuteCommandLists) failed");
        return;
    }
    if (MH_EnableHook(target) != MH_OK) { Log("MH_EnableHook(Execute) failed"); return; }
    Log("hooked ExecuteCommandLists @%p", target);
}

void StartSessionOnce()
{
    static bool started = false;
    if (started || g_queueRef == nullptr || g_devRef == nullptr) return;
    started = true;
    std::thread([] { InitSession(ModuleDir()); }).detach();
}

void EnsureHooks(ID3D12CommandQueue *queue)
{
    InstallDispatch();
    if (queue != nullptr) {
        if (g_originalExecute == nullptr) InstallExecute(queue);
        if (g_queueRef == nullptr && queue->GetDesc().Type == D3D12_COMMAND_LIST_TYPE_DIRECT) {
            g_queueRef = queue;
            g_queueRef->AddRef();
            if (SUCCEEDED(queue->GetDevice(IID_PPV_ARGS(&g_devRef))) && g_devRef != nullptr)
                Log("host queue %p device %p", queue, g_devRef);
            else
                Log("host queue %p (no device)", queue);
        }
    }
    StartSessionOnce();
}

void OnPresent(command_queue *queue, swapchain *, const rect *, const rect *, uint32_t, const rect *)
{
    EnsureHooks(queue != nullptr ? reinterpret_cast<ID3D12CommandQueue *>(queue->get_native()) : nullptr);
}

void OnExecute(command_queue *queue, command_list *)
{
    EnsureHooks(queue != nullptr ? reinterpret_cast<ID3D12CommandQueue *>(queue->get_native()) : nullptr);
    ReleaseDeferred();
}

// ---------------------------------------------------------------------------------------
// ReShade panel (Home tab): every runtime parameter, live
// ---------------------------------------------------------------------------------------
void SaveSettings()
{
    const std::wstring path = ModuleDir() + L"\\mochinr.ini";
    FILE *f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"w") != 0 || f == nullptr) return;
    fprintf(f, "# mochinr - rewritten by the ReShade panel\n");
    fprintf(f, "enabled=%d\npasses=%d\nmodel_scale=%.3f\ntransfer=%.3f\ncolour=%.3f\nreset_every=%d\n"
               "debug_view=%d\ninflight=%d\n",
            g_cfg.enabled ? 1 : 0, g_cfg.passes, g_cfg.modelScale, g_cfg.transfer, g_cfg.colour, g_cfg.resetEvery,
            g_cfg.debugView, g_cfg.inflight);
    fprintf(f, "style=%d\nintensity=%.3f\nlocal_tone=%.3f\nlocal_structure=%.3f\nskin_structure=%.3f\n"
               "auto_mask=%d\nhighlight_guard=%.2f\nchroma_clamp=%.2f\ndetail_stability=%d\n"
               "chained_history=%d\nfollow_input_res=%d\n",
            g_cfg.style, g_cfg.intensity, g_cfg.localTone, g_cfg.localStructure, g_cfg.skinStructure, g_cfg.autoMask,
            g_cfg.highlightGuard, g_cfg.chromaClamp, g_cfg.detailStability, g_cfg.chainedHistory,
            g_cfg.followInputRes);
    fclose(f);
    Log("settings saved to mochinr.ini");
}

// ImGui's checkbox takes a bool*; our settings are ints, and casting would write one byte into four.
bool Toggle(const char *label, int &value)
{
    bool on = value != 0;
    if (!ImGui::Checkbox(label, &on)) return false;
    value = on ? 1 : 0;
    return true;
}

void OnOverlay(effect_runtime *)
{
    const char *state = g_nr.state == 2 ? "就绪" : g_nr.state == 1 ? "构建中" : g_nr.state == 3 ? "失败" : "空闲";
    const double n = g_stats.frames ? double(g_stats.frames) : 0.0;

    // Tabbed like ReShade's own settings strip. (An add-on cannot add a page to that strip itself --
    // ReShadeRegisterOverlay only gives us a window -- so the closest thing is to draw the same tab
    // bar here and keep every control inside it.)
    if (ImGui::BeginTabBar("##mochinr_tabs", ImGuiTabBarFlags_None)) {
        if (ImGui::BeginTabItem("网络")) {
            ImGui::Spacing();
            ImGui::Checkbox("启用（关闭则效果器直通，不做降噪）", &g_cfg.enabled);
            ImGui::Spacing();

            // What the network is actually running at (MochizukiNrGetInfo), not what was asked for.
            const MochizukiNrInfo &ri = g_nr.info;
            if (ri.model_w != 0) {
                double ratio = 1.0;
                if (ri.frame_w != 0) ratio = double(ri.model_w) / double(ri.frame_w);
                ImGui::Text("渲染分辨率  %u x %u", ri.model_w, ri.model_h);
                ImGui::SameLine();
                ImGui::TextDisabled("(帧 %u x %u, %.2f 倍)", ri.frame_w, ri.frame_h, ratio);
            } else {
                ImGui::TextDisabled("渲染分辨率  等待网络就绪…");
            }
            ImGui::Text("网络 GPU     %.2f ms", ri.gpu_ms_median);
            ImGui::SameLine();
            ImGui::TextDisabled("(p95 %.2f, 层数上限 %u, RenoDX %s)", ri.gpu_ms_p95, ri.max_passes,
                                ri.renodx_effective ? "开" : "关");
            ImGui::Separator();
            ImGui::SetNextItemWidth(240.0f);
            ImGui::SliderInt("网络层数", &g_cfg.passes, 1, 3);
            ImGui::SetNextItemWidth(240.0f);
            ImGui::SliderFloat("网络渲染率", &g_cfg.modelScale, 0.25f, 1.0f, "%.2f");
            ImGui::SetNextItemWidth(240.0f);
            ImGui::SliderFloat("细节强度", &g_cfg.transfer, 0.0f, 2.0f, "%.2f");
            ImGui::SetNextItemWidth(240.0f);
            ImGui::SliderFloat("色彩强度", &g_cfg.colour, 0.0f, 4.0f, "%.2f");
            ImGui::Spacing();
            ImGui::SetNextItemWidth(240.0f);
            ImGui::SliderInt("每 N 帧重置时域历史（0 = 不重置）", &g_cfg.resetEvery, 0, 240);
            if (ImGui::Button("立即重置历史")) {
                if (g_nr.session != nullptr && g_nr.api.ResetHistory != nullptr) g_nr.api.ResetHistory(g_nr.session);
                Log("panel: history reset");
            }
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("风味")) {
            ImGui::Spacing();
            ImGui::SetNextItemWidth(240.0f);
            ImGui::Combo("风格", &g_cfg.style, "标准\0自然\0电影感\0");
            ImGui::SetNextItemWidth(240.0f);
            ImGui::SliderFloat("强度", &g_cfg.intensity, 0.0f, 2.0f, "%.2f");
            ImGui::SetNextItemWidth(240.0f);
            ImGui::SliderFloat("局部色调", &g_cfg.localTone, 0.0f, 2.0f, "%.2f");
            ImGui::SetNextItemWidth(240.0f);
            ImGui::SliderFloat("局部结构", &g_cfg.localStructure, 0.0f, 2.0f, "%.2f");
            ImGui::SetNextItemWidth(240.0f);
            ImGui::SliderFloat("皮肤结构", &g_cfg.skinStructure, -1.0f, 2.0f,
                               g_cfg.skinStructure < 0.0f ? "跟随局部结构" : "%.2f");
            Toggle("自动皮肤遮罩", g_cfg.autoMask);
            ImGui::Spacing();
            ImGui::SetNextItemWidth(240.0f);
            ImGui::SliderFloat("高光保护", &g_cfg.highlightGuard, 1.0f, 8.0f, "%.1f 倍");
            ImGui::SetNextItemWidth(240.0f);
            ImGui::SliderFloat("色度钳制", &g_cfg.chromaClamp, 0.0f, 4.0f,
                               g_cfg.chromaClamp <= 0.0f ? "关闭" : "%.2f 档");
            ImGui::Spacing();
            Toggle("细节稳定", g_cfg.detailStability);
            ImGui::SameLine();
            Toggle("链式历史", g_cfg.chainedHistory);
            ImGui::SameLine();
            Toggle("跟随输入分辨率", g_cfg.followInputRes);
            ImGui::Spacing();
            if (ImGui::TreeNode("多层的后续参数")) {
                Toggle("第 2 层跟随第 1 层", g_cfg.pass2Follow);
                ImGui::BeginDisabled(g_cfg.pass2Follow != 0);
                ImGui::SetNextItemWidth(200.0f);
                ImGui::SliderFloat("第 2 层细节", &g_cfg.pass2Transfer, 0.0f, 2.0f, "%.2f");
                ImGui::SetNextItemWidth(200.0f);
                ImGui::SliderFloat("第 2 层色彩", &g_cfg.pass2Colour, 0.0f, 1.0f, "%.2f");
                ImGui::EndDisabled();
                Toggle("第 3 层跟随第 1 层", g_cfg.pass3Follow);
                ImGui::BeginDisabled(g_cfg.pass3Follow != 0);
                ImGui::SetNextItemWidth(200.0f);
                ImGui::SliderFloat("第 3 层细节", &g_cfg.pass3Transfer, 0.0f, 2.0f, "%.2f");
                ImGui::SetNextItemWidth(200.0f);
                ImGui::SliderFloat("第 3 层色彩", &g_cfg.pass3Colour, 0.0f, 1.0f, "%.2f");
                ImGui::EndDisabled();
                ImGui::TreePop();
            }
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("计时")) {
            ImGui::Spacing();
            ImGui::Text("会话状态    %s", state);
            ImGui::Text("画面尺寸    %ux%u", g_lastWidth, g_lastHeight);
            ImGui::Text("已合成帧    %llu", (unsigned long long)g_nr.ran);
            ImGui::Text("已跳过帧    %llu", (unsigned long long)g_nr.skipped);
            if (g_nr.info.building)
                ImGui::Text("网络构建中  阶段 %u (%u/%u)", g_nr.info.build_stage, g_nr.info.build_done,
                            g_nr.info.build_total);
            else
                ImGui::Text("网络         %u x %u   %.2f ms", g_nr.info.model_w, g_nr.info.model_h,
                            g_nr.info.gpu_ms_median);
            ImGui::Separator();
            ImGui::Text("每帧 CPU 合计   %.3f ms", n ? g_stats.total / n : 0.0);
            ImGui::Text("  Vulkan 围栏    %.3f ms", n ? g_stats.enqueue / n : 0.0);
            ImGui::Text("  提交           %.3f ms", n ? g_stats.submit / n : 0.0);
            ImGui::Text("  录制           %.3f ms", n ? g_stats.recordIn / n : 0.0);
            ImGui::Text("  准备           %.3f ms", n ? g_stats.prepare / n : 0.0);
            ImGui::Separator();
            ImGui::SetNextItemWidth(240.0f);
            ImGui::SliderInt("在飞帧数", &g_cfg.inflight, 2, 16);
            ImGui::Text("当前队列深度    %d", (int)g_retire.size());
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("调试")) {
            ImGui::Spacing();
            ImGui::SetNextItemWidth(240.0f);
            ImGui::Combo("显示视图", &g_cfg.debugView, "正常\0代理\0纯网络\0差分 20 倍\0着色\0");
            ImGui::TextWrapped("「纯网络」显示网络自己的图像：若画面变成一层偏软的网络渲染，说明\n"
                               "「网络 → 效果器」这条链路是通的。");
            ImGui::Spacing();
            if (ImGui::Button("保存设置")) SaveSettings();
            ImGui::SameLine();
            ImGui::TextDisabled("下一帧生效，松手后自动写入 mochinr.ini");
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }

    // Autosave once the drag ends: the ini is rewritten whole, so not on every tick.
    static Settings saved = g_cfg;
    static bool loaded = false;
    if (!loaded) { loaded = true; saved = g_cfg; }
    const bool changed = saved.enabled != g_cfg.enabled || saved.passes != g_cfg.passes ||
                         saved.modelScale != g_cfg.modelScale || saved.transfer != g_cfg.transfer ||
                         saved.colour != g_cfg.colour || saved.resetEvery != g_cfg.resetEvery ||
                         saved.debugView != g_cfg.debugView || saved.inflight != g_cfg.inflight;
    if (changed && !ImGui::IsAnyItemActive()) {
        SaveSettings();
        saved = g_cfg;
    }
}

} // namespace

BOOL WINAPI DllMain(HINSTANCE self, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(self);
        wchar_t path[MAX_PATH]{};
        GetModuleFileNameW(self, path, MAX_PATH);
        std::wstring logPath(path);
        logPath = logPath.substr(0, logPath.find_last_of(L'\\')) + L"\\mochinr.log";
        g_log = _wfsopen(logPath.c_str(), L"a", _SH_DENYWR);
        Log("=== loaded ===");
        LoadSettings(ModuleDir());
        Log("cfg enabled=%d passes=%d scale=%.2f transfer=%.2f colour=%.2f inflight=%d", g_cfg.enabled ? 1 : 0,
            g_cfg.passes, g_cfg.modelScale, g_cfg.transfer, g_cfg.colour, g_cfg.inflight);
        if (MH_Initialize() != MH_OK) Log("MH_Initialize failed");
        InstallDispatch();
        if (!reshade::register_addon(self)) {
            Log("register_addon failed");
        } else {
            reshade::register_event<reshade::addon_event::present>(OnPresent);
            reshade::register_event<reshade::addon_event::execute_command_list>(OnExecute);
            reshade::register_overlay("mochinr 降噪", OnOverlay);
            Log("registered (panel in the ReShade Home tab)");
        }
    } else if (reason == DLL_PROCESS_DETACH) {
        MH_Uninitialize();
        if (g_log) { fclose(g_log); g_log = nullptr; }
    }
    return TRUE;
}
