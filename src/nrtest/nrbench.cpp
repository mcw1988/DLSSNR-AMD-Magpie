// Frame-cost benchmark for MochizukiNrRuntime.dll.
//
// Runs a steady stream of frames at a chosen raster size and reports the wall time
// of every stage (prepare / producer copy / network / consumer copy). This isolates
// the NR stage from Magpie's effects so the two can be compared directly.
//
// Usage: nrbench.exe <assets_dir> <width> <height> <frames> [aco]
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "MochiNrApi.h"

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")

static double NowMs()
{
    static LARGE_INTEGER freq{};
    if (freq.QuadPart == 0)
        QueryPerformanceFrequency(&freq);
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return static_cast<double>(c.QuadPart) * 1000.0 / static_cast<double>(freq.QuadPart);
}

static void Step(const char* what, int32_t rc, const MochiNrApi& api)
{
    char err[512]{};
    if (api.GetLastError)
        api.GetLastError(err, sizeof err);
    std::printf("[%s] rc=%d%s%s\n", what, rc, err[0] ? "  last_error: " : "", err);
    std::fflush(stdout);
}

int wmain(int argc, wchar_t** argv)
{
    if (argc < 5) {
        std::printf("usage: nrbench.exe <assets_dir> <width> <height> <frames> [aco]\n");
        return 2;
    }
    const std::wstring assets_w = argv[1];
    const UINT W = static_cast<UINT>(_wtoi(argv[2]));
    const UINT H = static_cast<UINT>(_wtoi(argv[3]));
    const int frames = _wtoi(argv[4]);
    const bool use_aco = argc > 5 && _wcsicmp(argv[5], L"aco") == 0;
    const float model_scale = argc > 6 ? static_cast<float>(_wtof(argv[6])) : 0.6f;

    IDXGIFactory4* factory = nullptr;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
        std::printf("CreateDXGIFactory1 failed\n");
        return 1;
    }
    IDXGIAdapter1* adapter = nullptr;
    ID3D12Device* device = nullptr;
    for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_ADAPTER_DESC1 desc{};
        adapter->GetDesc1(&desc);
        if (device == nullptr && SUCCEEDED(D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)))) {
            std::wprintf(L"adapter: %s\n", desc.Description);
            adapter->Release();
            adapter = nullptr;
            continue;
        }
        adapter->Release();
        adapter = nullptr;
    }
    factory->Release();
    if (device == nullptr) {
        std::printf("D3D12CreateDevice failed\n");
        return 1;
    }

    ID3D12CommandQueue* queue = nullptr;
    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (FAILED(device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue)))) {
        std::printf("CreateCommandQueue failed\n");
        return 1;
    }
    ID3D12CommandAllocator* alloc = nullptr;
    ID3D12GraphicsCommandList* list = nullptr;
    device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc));
    device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, nullptr, IID_PPV_ARGS(&list));
    list->Close();

    ID3D12Fence* fence = nullptr;
    device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
    HANDLE fevent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    UINT64 fenceValue = 0;
    auto WaitGpu = [&]() {
        queue->Signal(fence, ++fenceValue);
        if (fence->GetCompletedValue() < fenceValue) {
            fence->SetEventOnCompletion(fenceValue, fevent);
            WaitForSingleObject(fevent, 60000);
        }
    };

    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC td{};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = W;
    td.Height = H;
    td.DepthOrArraySize = 1;
    td.MipLevels = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    ID3D12Resource* colour = nullptr;
    if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &td,
                                               D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                               IID_PPV_ARGS(&colour)))) {
        std::printf("CreateCommittedResource(colour) failed\n");
        return 1;
    }

    const UINT rowPitch = W * 4, slice = rowPitch * H;
    D3D12_HEAP_PROPERTIES up{};
    up.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC bd{};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = slice;
    bd.Height = 1;
    bd.DepthOrArraySize = 1;
    bd.MipLevels = 1;
    bd.Format = DXGI_FORMAT_UNKNOWN;
    bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ID3D12Resource* upload = nullptr;
    device->CreateCommittedResource(&up, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ,
                                    nullptr, IID_PPV_ARGS(&upload));
    {
        void* p = nullptr;
        upload->Map(0, nullptr, &p);
        auto* px = static_cast<uint8_t*>(p);
        for (UINT y = 0; y < H; ++y)
            for (UINT x = 0; x < W; ++x) {
                uint8_t* q = px + static_cast<size_t>(y) * rowPitch + static_cast<size_t>(x) * 4;
                q[0] = static_cast<uint8_t>(x * 255 / (W - 1));
                q[1] = static_cast<uint8_t>(y * 255 / (H - 1));
                q[2] = static_cast<uint8_t>(128 + 60 * std::sin(x * 0.05f));
                q[3] = 255;
            }
        upload->Unmap(0, nullptr);
    }
    {
        D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
        dst.pResource = colour;
        dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src.pResource = upload;
        src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        src.PlacedFootprint.Footprint = {DXGI_FORMAT_R8G8B8A8_UNORM, W, H, 1, rowPitch};
        alloc->Reset();
        list->Reset(alloc, nullptr);
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = colour;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        list->ResourceBarrier(1, &b);
        list->Close();
        ID3D12CommandList* lists[] = {list};
        queue->ExecuteCommandLists(1, lists);
        WaitGpu();
    }
    std::printf("raster %ux%u, %d frames%s, model scale %.2f\n", W, H, frames, use_aco ? ", ACO" : "", model_scale);

    const std::wstring dll = assets_w + L"\\MochizukiNrRuntime.dll";
    HMODULE mod = LoadLibraryExW(dll.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (mod == nullptr) {
        std::printf("LoadLibraryEx failed: %lu\n", GetLastError());
        return 1;
    }
    auto getApi = reinterpret_cast<int32_t (*)(uint32_t, MochiNrApi*)>(GetProcAddress(mod, "LmxxfNrGetApi") /* 历史导出名 */);
    if (getApi == nullptr) {
        std::printf("MochiNrGetApi missing\n");
        return 1;
    }
    MochiNrApi api{};
    api.struct_size = sizeof api;
    int32_t rc = getApi(MOCHI_NR_ABI_VERSION, &api);
    if (rc != MOCHI_NR_OK)
        return 1;

    MochiNrCreateInfo ci{};
    ci.struct_size = sizeof ci;
    ci.device = device;
    ci.queue = queue;
    ci.assets_directory = assets_w.c_str();
    ci.flags = MOCHI_NR_CREATE_FLAG_ZERO_OUTPUT_FALLBACK;
    if (use_aco)
        ci.flags |= MOCHI_NR_CREATE_FLAG_ACO;
    void* session = nullptr;
    rc = api.Create(&ci, &session);
    Step("Create", rc, api);
    if (rc != MOCHI_NR_OK)
        return 1;
    rc = api.PrepareSession(session);
    Step("PrepareSession", rc, api);

    std::vector<double> t_prep, t_in, t_nr, t_out, t_total;
    t_prep.reserve(frames);
    t_in.reserve(frames);
    t_nr.reserve(frames);
    t_out.reserve(frames);
    t_total.reserve(frames);

    for (int f = 0; f < frames; ++f) {
        const double t0 = NowMs();
        MochiNrFrameInfo fi{};
        fi.struct_size = sizeof fi;
        fi.frame_id = static_cast<uint64_t>(f) + 1;
        fi.command_list = list;
        fi.color_width = W;
        fi.color_height = H;
        fi.color = colour;
        fi.color_state = static_cast<uint32_t>(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        fi.flags = 0;
        fi.transfer_strength = 1.0f;
        fi.color_strength = 1.0f;
        fi.model_scale = model_scale;
        fi.passes = 1;
        fi.reset = f == 0 ? 1u : 0u;

        MochiNrJob job{};
        job.struct_size = sizeof job;
        const double a0 = NowMs();
        for (int attempt = 0; attempt < 600; ++attempt) {
            rc = api.PrepareFrame(session, &fi, &job);
            if (rc == MOCHI_NR_OK)
                break;
            Sleep(100);
        }
        const double a1 = NowMs();
        if (rc != MOCHI_NR_OK) {
            Step("PrepareFrame", rc, api);
            return 1;
        }

        alloc->Reset();
        list->Reset(alloc, nullptr);
        rc = api.RecordInputs(session, job.handle, list);
        if (rc != MOCHI_NR_OK) {
            Step("RecordInputs", rc, api);
            return 1;
        }
        list->Close();
        ID3D12CommandList* lists[] = {list};
        queue->ExecuteCommandLists(1, lists);
        const double b1 = NowMs();

        rc = api.EnqueueHip(session, job.handle, queue);
        if (rc != MOCHI_NR_OK) {
            Step("EnqueueHip", rc, api);
            return 1;
        }
        uint32_t jobState = 0;
        for (int i = 0; i < 600000; ++i) {
            rc = api.Poll(session, job.handle, &jobState);
            if (rc != MOCHI_NR_OK || jobState >= MOCHI_NR_JOB_NR_COMPLETE)
                break;
            Sleep(0);
        }
        const double c1 = NowMs();

        alloc->Reset();
        list->Reset(alloc, nullptr);
        rc = api.RecordOutputs(session, job.handle, list);
        if (rc != MOCHI_NR_OK) {
            Step("RecordOutputs", rc, api);
            return 1;
        }
        list->Close();
        queue->ExecuteCommandLists(1, lists);
        WaitGpu();
        const double d1 = NowMs();

        rc = api.Retire(session, job.handle);
        if (rc != MOCHI_NR_OK) {
            Step("Retire", rc, api);
            return 1;
        }
        const double t1 = NowMs();

        t_prep.push_back(a1 - a0);
        t_in.push_back(b1 - a1);
        t_nr.push_back(c1 - b1);
        t_out.push_back(d1 - c1);
        t_total.push_back(t1 - t0);
        if (f == 0 || f == frames - 1)
            std::printf("  frame %d: prep %.2f  in %.2f  nr %.2f  out %.2f  total %.2f ms\n", f,
                        a1 - a0, b1 - a1, c1 - b1, d1 - c1, t1 - t0);
        std::fflush(stdout);
    }

    auto report = [](const char* name, std::vector<double>& v) {
        if (v.empty())
            return;
        std::vector<double> s = v;
        std::sort(s.begin(), s.end());
        double sum = 0;
        for (double x : v)
            sum += x;
        const double avg = sum / static_cast<double>(v.size());
        const double p50 = s[s.size() / 2];
        const double p95 = s[static_cast<size_t>(s.size() * 0.95) < s.size() ? static_cast<size_t>(s.size() * 0.95) : s.size() - 1];
        std::printf("%-8s avg %7.2f  p50 %7.2f  p95 %7.2f  min %7.2f  max %7.2f ms\n", name, avg, p50, p95,
                    s.front(), s.back());
    };
    std::printf("\n--- %ux%u ---\n", W, H);
    report("prepare", t_prep);
    report("producer", t_in);
    report("network", t_nr);
    report("consumer", t_out);
    report("total", t_total);
    if (!t_total.empty()) {
        double sum = 0;
        for (double x : t_total)
            sum += x;
        std::printf("implied rate: %.1f fps (network-only budget: %.1f fps)\n",
                    1000.0 / (sum / static_cast<double>(t_total.size())),
                    1000.0 / std::max(0.001, t_nr.empty() ? 0.001 : [&] {
                        double s = 0;
                        for (double x : t_nr)
                            s += x;
                        return s / static_cast<double>(t_nr.size());
                    }()));
    }

    api.Drain(session);
    api.Destroy(session);
    return 0;
}
