// Standalone smoke test for MochizukiNrRuntime.dll.
//
// Creates its own D3D12 device/queue, runs one frame through the Mochi API v1
// (QueryCapabilities -> Create -> PrepareSession -> PrepareFrame -> RecordInputs ->
// EnqueueHip -> RecordOutputs -> Poll -> Retire), and reports what came back.
// Usage: nrtest.exe [assets_dir]
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "MochiNrApi.h"

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")

static void Step(const char* what, int32_t rc, const MochiNrApi& api)
{
    char err[512]{};
    if (api.GetLastError)
        api.GetLastError(err, sizeof err);
    std::printf("[%s] rc=%d%s%s\n", what, rc, err[0] ? "  last_error: " : "", err);
    std::fflush(stdout);
}

int main(int argc, char** argv)
{
    std::string assets = argc > 1 ? argv[1] : ".";
    std::wstring assets_w(assets.begin(), assets.end());
    std::printf("assets: %s\n", assets.c_str());

    // ---- D3D12 device ----------------------------------------------------------------
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
        std::wprintf(L"adapter %u: %s (vendor %04x)\n", i, desc.Description, desc.VendorId);
        if (device == nullptr && SUCCEEDED(D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)))) {
            std::wprintf(L"  -> using this one\n");
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
            WaitForSingleObject(fevent, 30000);
        }
    };

    // ---- colour texture: 1920x1080 RGBA8 gradient -------------------------------------
    const UINT W = 1920, H = 1080;
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
    D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COPY_DEST;
    ID3D12Resource* colour = nullptr;
    if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &td, state, nullptr,
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
                uint8_t* q = px + y * rowPitch + x * 4;
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
        state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    }
    std::printf("colour %ux%u RGBA8 ready\n", W, H);

    // ---- runtime ---------------------------------------------------------------------
    std::wstring dll = assets_w + L"\\MochizukiNrRuntime.dll";
    HMODULE mod = LoadLibraryExW(dll.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (mod == nullptr) {
        std::printf("LoadLibraryEx(%ls) failed: %lu\n", dll.c_str(), GetLastError());
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
    std::printf("MochiNrGetApi -> %d, struct_size %u, abi %u\n", rc, api.struct_size, api.abi_version);
    if (rc != MOCHI_NR_OK) {
        return 1;
    }

    MochiNrCapabilities caps{};
    caps.struct_size = sizeof caps;
    Step("QueryCapabilities", api.QueryCapabilities(&caps), api);
    std::printf("  max %ux%u history %u graph %u hip %u gfx1201 %u\n", caps.max_input_width,
                caps.max_input_height, caps.history_supported, caps.graph_supported, caps.hip_ready,
                caps.gfx1201_target);

    MochiNrCreateInfo ci{};
    ci.struct_size = sizeof ci;
    ci.device = device;
    ci.queue = queue;
    ci.assets_directory = assets_w.c_str();
    ci.flags = MOCHI_NR_CREATE_FLAG_ZERO_OUTPUT_FALLBACK;
    void* session = nullptr;
    rc = api.Create(&ci, &session);
    Step("Create", rc, api);
    if (rc != MOCHI_NR_OK) {
        return 1;
    }

    // The network is compiled here on the first run: allow minutes.
    std::printf("PrepareSession (may compile pipelines, be patient)...\n");
    std::fflush(stdout);
    rc = api.PrepareSession(session);
    Step("PrepareSession", rc, api);

    MochiNrFrameInfo fi{};
    fi.struct_size = sizeof fi;
    fi.frame_id = 1;
    fi.command_list = list;
    fi.color_width = W;
    fi.color_height = H;
    fi.color = colour;
    fi.color_state = static_cast<uint32_t>(state);
    fi.flags = 0;
    fi.transfer_strength = 1.0f;
    fi.color_strength = 1.0f;
    fi.model_scale = 1.0f;
    fi.passes = 1;
    fi.reset = 1;

    MochiNrJob job{};
    job.struct_size = sizeof job;
    // The first frame refuses while the network compiles: keep asking, as the add-on does.
    const ULONGLONG t0 = GetTickCount64();
    for (;;) {
        job.struct_size = sizeof job;
        rc = api.PrepareFrame(session, &fi, &job);
        if (rc == MOCHI_NR_OK)
            break;
        char st[512]{};
        if (api.GetStatus)
            api.GetStatus(session, st, sizeof st);
        std::printf("  [%llus] PrepareFrame rc=%d  status: %s\n",
                    (GetTickCount64() - t0) / 1000, rc, st);
        std::fflush(stdout);
        if (GetTickCount64() - t0 > 900000 || (rc != MOCHI_NR_UNAVAILABLE && rc != MOCHI_NR_FAILED)) {
            char err[512]{};
            if (api.GetLastError)
                api.GetLastError(err, sizeof err);
            std::printf("PrepareFrame gave up: rc=%d %s\n", rc, err);
            return 1;
        }
        Sleep(3000);
    }
    Step("PrepareFrame", rc, api);
    std::printf("  private_output=%p after %.0f s\n", job.private_output,
                (GetTickCount64() - t0) / 1000.0);

    alloc->Reset();
    list->Reset(alloc, nullptr);
    rc = api.RecordInputs(session, job.handle, list);
    Step("RecordInputs", rc, api);
    list->Close();
    ID3D12CommandList* lists[] = {list};
    queue->ExecuteCommandLists(1, lists);
    WaitGpu();

    rc = api.EnqueueHip(session, job.handle, queue);
    Step("EnqueueHip", rc, api);

    uint32_t jobState = 0;
    for (int i = 0; i < 600; ++i) {
        rc = api.Poll(session, job.handle, &jobState);
        if (rc != MOCHI_NR_OK) {
            Step("Poll", rc, api);
            break;
        }
        if (jobState >= MOCHI_NR_JOB_NR_COMPLETE)
            break;
        Sleep(500);
    }
    std::printf("  jobState=%u after %d polls\n", jobState, 0);

    alloc->Reset();
    list->Reset(alloc, nullptr);
    rc = api.RecordOutputs(session, job.handle, list);
    Step("RecordOutputs", rc, api);
    list->Close();
    queue->ExecuteCommandLists(1, lists);
    WaitGpu();

    rc = api.Retire(session, job.handle);
    Step("Retire", rc, api);

    // ---- compare colour before/after (RecordOutputs may compose in place) -------------
    {
        D3D12_HEAP_PROPERTIES rb{};
        rb.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC rbd = bd;
        rbd.Width = slice;
        ID3D12Resource* readback = nullptr;
        device->CreateCommittedResource(&rb, D3D12_HEAP_FLAG_NONE, &rbd, D3D12_RESOURCE_STATE_COPY_DEST,
                                        nullptr, IID_PPV_ARGS(&readback));
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = colour;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        alloc->Reset();
        list->Reset(alloc, nullptr);
        list->ResourceBarrier(1, &b);
        D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
        dst.pResource = readback;
        dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint.Footprint = {DXGI_FORMAT_R8G8B8A8_UNORM, W, H, 1, rowPitch};
        src.pResource = colour;
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        list->Close();
        queue->ExecuteCommandLists(1, lists);
        WaitGpu();

        void* p = nullptr;
        readback->Map(0, nullptr, &p);
        auto* px = static_cast<uint8_t*>(p);
        double sum = 0, mind = 1e9, maxd = -1e9;
        for (UINT y = 0; y < H; y += 4)
            for (UINT x = 0; x < W; x += 4) {
                const uint8_t* q = px + y * rowPitch + x * 4;
                const double want = 128 + 60 * std::sin(x * 0.05f);
                const double d = std::fabs(q[2] - want);
                sum += d;
                mind = d < mind ? d : mind;
                maxd = d > maxd ? d : maxd;
            }
        std::printf("post-NR colour vs input |d|: mean %.3f min %.1f max %.1f\n",
                    sum / ((W / 4.0) * (H / 4.0)), mind, maxd);
        readback->Unmap(0, nullptr);
        readback->Release();
    }

    api.Drain(session);
    api.Destroy(session);
    std::printf("done\n");
    return 0;
}
