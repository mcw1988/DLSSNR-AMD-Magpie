// Minimal host that reproduces Magpie's input situation, to test the ReShade overlay key.
//
// Top level window: WS_EX_LAYERED | WS_EX_NOACTIVATE, answers MA_NOACTIVATE to WM_MOUSEACTIVATE.
// Child window (owns the swapchain, so this is the window ReShade subclasses):
// WS_EX_LAYERED | WS_EX_TRANSPARENT.
// Both therefore never receive a key or a mouse message, exactly like Magpie's scaling window.
//
// Run it with ReShade's dxgi.dll next to the exe. It saves before.bmp, sends a real Home key with
// SendInput, then saves after.bmp. With overlay-input.addon64 present the two differ (the overlay
// is up); without it they are identical.
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_3.h>

#include <cstdio>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

static ID3D11Device* g_device = nullptr;
static ID3D11DeviceContext* g_context = nullptr;
static IDXGISwapChain* g_swapchain = nullptr;
static ID3D11RenderTargetView* g_rtv = nullptr;
static HWND g_top = nullptr, g_child = nullptr;
static int g_frame = 0;
static bool g_sent = false, g_done = false, g_clicked = false;

static void SaveBmp(const wchar_t* path)
{
    ID3D11Texture2D* back = nullptr;
    if (FAILED(g_swapchain->GetBuffer(0, IID_PPV_ARGS(&back))) || back == nullptr)
        return;
    D3D11_TEXTURE2D_DESC desc{};
    back->GetDesc(&desc);
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    desc.MiscFlags = 0;
    ID3D11Texture2D* staging = nullptr;
    if (SUCCEEDED(g_device->CreateTexture2D(&desc, nullptr, &staging))) {
        g_context->CopyResource(staging, back);
        D3D11_MAPPED_SUBRESOURCE map{};
        if (SUCCEEDED(g_context->Map(staging, 0, D3D11_MAP_READ, 0, &map))) {
            const DWORD rowBytes = desc.Width * 4;
            const DWORD imageBytes = rowBytes * desc.Height;
            BITMAPFILEHEADER fh{};
            BITMAPINFOHEADER ih{};
            fh.bfType = 0x4D42;
            fh.bfOffBits = sizeof fh + sizeof ih;
            fh.bfSize = fh.bfOffBits + imageBytes;
            ih.biSize = sizeof ih;
            ih.biWidth = static_cast<LONG>(desc.Width);
            ih.biHeight = -static_cast<LONG>(desc.Height);
            ih.biPlanes = 1;
            ih.biBitCount = 32;
            ih.biCompression = BI_RGB;
            ih.biSizeImage = imageBytes;
            if (FILE* f = _wfopen(path, L"wb")) {
                std::fwrite(&fh, sizeof fh, 1, f);
                std::fwrite(&ih, sizeof ih, 1, f);
                for (UINT y = 0; y < desc.Height; ++y)
                    std::fwrite(static_cast<const BYTE*>(map.pData) + y * map.RowPitch, rowBytes, 1, f);
                std::fclose(f);
            }
            g_context->Unmap(staging, 0);
        }
        staging->Release();
    }
    back->Release();
}

static void SendHome()
{
    INPUT in[2]{};
    in[0].type = INPUT_KEYBOARD;
    in[0].ki.wVk = VK_HOME;
    in[0].ki.dwFlags = 0;
    in[1].type = INPUT_KEYBOARD;
    in[1].ki.wVk = VK_HOME;
    in[1].ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(2, in, sizeof(INPUT));
    OutputDebugStringW(L"harness: sent Home\n");
}

static LRESULT CALLBACK TopProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
    switch (msg) {
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

static LRESULT CALLBACK ChildProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
    if (msg == WM_ERASEBKGND)
        return 1;
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int)
{
    // Magpie is per-monitor DPI aware; without this the window is virtualised and every
    // coordinate the test uses would be scaled by the desktop DPI.
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    WNDCLASSEXW top{};
    top.cbSize = sizeof top;
    top.lpfnWndProc = TopProc;
    top.hInstance = instance;
    top.hCursor = LoadCursorW(nullptr, reinterpret_cast<LPCWSTR>(IDC_ARROW));
    top.lpszClassName = L"MagpieLikeHost";
    RegisterClassExW(&top);

    WNDCLASSEXW child = top;
    child.lpfnWndProc = ChildProc;
    child.lpszClassName = L"MagpieLikeHostChild";
    child.style = CS_OWNDC;
    RegisterClassExW(&child);

    const int w = 1280, h = 720;
    g_top = CreateWindowExW(WS_EX_LAYERED | WS_EX_NOACTIVATE | WS_EX_TOPMOST, top.lpszClassName,
                            L"Magpie-like host", WS_POPUP, 40, 40, w, h, nullptr, nullptr, instance,
                            nullptr);
    SetLastError(0);
    g_child = CreateWindowExW(WS_EX_TRANSPARENT | WS_EX_NOPARENTNOTIFY, child.lpszClassName, L"renderer",
                              WS_CHILD | WS_VISIBLE, 0, 0, w, h, g_top, nullptr, instance, nullptr);
    if (g_top == nullptr || g_child == nullptr) {
        wchar_t msg[256]{};
        swprintf_s(msg, L"window creation failed top=%p child=%p err=%lu", g_top, g_child, GetLastError());
        if (FILE* f = _wfopen(L"harness-error.txt", L"w")) { fwprintf(f, L"%s\n", msg); fclose(f); }
        MessageBoxW(nullptr, msg, L"harness", MB_ICONERROR);
        return 1;
    }
    ShowWindow(g_top, SW_SHOWNOACTIVATE);

    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 2;
    sd.BufferDesc.Width = w;
    sd.BufferDesc.Height = h;
    sd.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = g_child;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    D3D_FEATURE_LEVEL level{};
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    if (FAILED(D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, nullptr,
                                             0, D3D11_SDK_VERSION, &sd, &g_swapchain, &g_device,
                                             &level, &g_context))) {
        MessageBoxW(nullptr, L"D3D11 init failed", L"harness", MB_ICONERROR);
        return 1;
    }

    ID3D11Texture2D* back = nullptr;
    g_swapchain->GetBuffer(0, IID_PPV_ARGS(&back));
    g_device->CreateRenderTargetView(back, nullptr, &g_rtv);
    back->Release();

    bool reshade = GetModuleHandleW(L"dxgi.dll") != nullptr;

    for (;;) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        // Cycling colour so the frames are visibly different from each other.
        const float t = g_frame / 60.0f;
        const float colour[4] = {0.15f + 0.1f * (g_frame % 30) / 30.0f, 0.25f, 0.55f, 1.0f};
        (void)t;
        g_context->ClearRenderTargetView(g_rtv, colour);
        g_swapchain->Present(0, 0);
        ++g_frame;

        if (g_frame == 120)
            SaveBmp(L"before.bmp");
        if (g_frame == 150 && !g_sent) {
            g_sent = true;
            SendHome();
        }
        if (g_frame == 200 && g_sent && !g_clicked) {
            g_clicked = true;
            // Move onto the "Add-ons" tab of the overlay and click it.
            POINT p{};
            ClientToScreen(g_top, &p);
            const int sx = p.x + 56, sy = p.y + 12;
            SetCursorPos(sx, sy);
            Sleep(120);
            INPUT in[2]{};
            in[0].type = INPUT_MOUSE; in[0].mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
            in[1].type = INPUT_MOUSE; in[1].mi.dwFlags = MOUSEEVENTF_LEFTUP;
            SendInput(2, in, sizeof(INPUT));
        }
        if (g_frame == 260 && !g_done) {
            g_done = true;
            SaveBmp(L"after.bmp");
            if (FILE* f = _wfopen(L"harness.txt", L"w")) {
                fwprintf(f, L"frames=%d reshade_dll=%d\n", g_frame, reshade ? 1 : 0);
                std::fclose(f);
            }
            PostQuitMessage(0);
            break;
        }
        Sleep(16);
    }
    return 0;
}
