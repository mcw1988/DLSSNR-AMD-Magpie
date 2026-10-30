// overlay-input: routes keyboard and mouse into the window ReShade renders to.
//
// Magpie creates its scaling window with WS_EX_NOACTIVATE (and returns MA_NOACTIVATE from
// WM_MOUSEACTIVATE); the renderer child that owns the swapchain is WS_EX_TRANSPARENT. So the
// window that ReShade subclasses never receives a key or a mouse message, and its overlay key
// does nothing. ReShade's input is purely message driven (source/input_windows.cpp,
// handle_window_message), so posting the messages by hand is enough.
//
// Press the toggle key (Home by default) to open the overlay: this add-on posts the key into the
// swapchain window and from then on forwards every key and mouse event there, swallowing the
// originals so the game underneath does not act on them too. Press it again to close.
#include <reshade.hpp>

#include <windows.h>

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <string>
#include <thread>

using namespace reshade::api;

namespace {

HWND g_hwnd = nullptr;
std::atomic<bool> g_open{false};
std::atomic<bool> g_ready{false};
HMODULE g_module = nullptr;
HHOOK g_kb_hook = nullptr;
HHOOK g_mouse_hook = nullptr;
UINT g_key = VK_HOME;
bool g_swallow = true;
FILE* g_log = nullptr;
DWORD g_thread_id = 0;

void Log(const char* fmt, ...)
{
    if (g_log == nullptr)
        return;
    va_list args;
    va_start(args, fmt);
    std::vfprintf(g_log, fmt, args);
    va_end(args);
    std::fputc('\n', g_log);
    std::fflush(g_log);
}

void OpenLog()
{
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (wchar_t* slash = wcsrchr(path, L'\\'))
        slash[1] = 0;
    std::wstring file = std::wstring(path) + L"overlay-input.log";
    g_log = _wfopen(file.c_str(), L"w");
}

void ReadIni()
{
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (wchar_t* slash = wcsrchr(path, L'\\'))
        slash[1] = 0;
    const std::wstring ini = std::wstring(path) + L"overlay-input.ini";
    if (GetFileAttributesW(ini.c_str()) == INVALID_FILE_ATTRIBUTES) {
        if (FILE* f = _wfopen(ini.c_str(), L"w")) {
            std::fputs("; overlay-input.ini\r\n"
                       "; Key: Windows virtual-key code of the overlay toggle. 36 = Home, 35 = End.\r\n"
                       "; Swallow: 1 = the game does not see the keys and clicks while the overlay is open.\r\n"
                       "Key=36\r\nSwallow=1\r\n", f);
            std::fclose(f);
        }
    }
    g_key = static_cast<UINT>(GetPrivateProfileIntW(L"overlay-input", L"Key", VK_HOME, ini.c_str()));
    g_swallow = GetPrivateProfileIntW(L"overlay-input", L"Swallow", 1, ini.c_str()) != 0;
}

void PostKey(WPARAM vk, bool down)
{
    if (g_hwnd == nullptr)
        return;
    const LPARAM scan = static_cast<LPARAM>(MapVirtualKeyW(static_cast<UINT>(vk), MAPVK_VK_TO_VSC)) << 16;
    PostMessageW(g_hwnd, down ? WM_KEYDOWN : WM_KEYUP, vk, scan | (down ? 0x00000001 : 0xC0000001));
}

void PostChar(WPARAM vk)
{
    if (g_hwnd == nullptr)
        return;
    BYTE state[256]{};
    GetKeyboardState(state);
    WCHAR buf[8]{};
    const UINT scan = MapVirtualKeyW(static_cast<UINT>(vk), MAPVK_VK_TO_VSC);
    const int n = ToUnicode(static_cast<UINT>(vk), scan, state, buf, 8, 0);
    for (int i = 0; i < n; ++i)
        PostMessageW(g_hwnd, WM_CHAR, static_cast<WPARAM>(buf[i]), 1);
}

void PostMouseMove(const POINT* screen = nullptr)
{
    if (g_hwnd == nullptr)
        return;
    POINT p = screen != nullptr ? *screen : POINT{};
    if (screen == nullptr)
        GetCursorPos(&p);
    ScreenToClient(g_hwnd, &p);
    PostMessageW(g_hwnd, WM_MOUSEMOVE, 0, MAKELPARAM(p.x, p.y));
}

void PostButton(int index, bool down, const POINT* screen = nullptr)
{
    if (g_hwnd == nullptr)
        return;
    POINT p = screen != nullptr ? *screen : POINT{};
    if (screen == nullptr)
        GetCursorPos(&p);
    ScreenToClient(g_hwnd, &p);
    UINT msg = WM_LBUTTONDOWN;
    WPARAM wp = 0;
    switch (index) {
    case 0: msg = down ? WM_LBUTTONDOWN : WM_LBUTTONUP; wp = down ? MK_LBUTTON : 0; break;
    case 1: msg = down ? WM_RBUTTONDOWN : WM_RBUTTONUP; wp = down ? MK_RBUTTON : 0; break;
    default: msg = down ? WM_MBUTTONDOWN : WM_MBUTTONUP; wp = down ? MK_MBUTTON : 0; break;
    }
    PostMessageW(g_hwnd, msg, wp, MAKELPARAM(p.x, p.y));
}

void ToggleOnWorkerThread()
{
    PostKey(g_key, true);
    Sleep(90); // a frame has to pass between down and up, or ReShade sees no press
    PostKey(g_key, false);
    if (g_open.load())
        PostMouseMove();
}

void Toggle()
{
    const bool open = !g_open.load();
    g_open.store(open);
    Log("overlay input %s", open ? "ON" : "OFF");
    if (g_log)
        std::fflush(g_log);
    std::thread(ToggleOnWorkerThread).detach();
}

LRESULT CALLBACK KeyboardProc(int code, WPARAM wparam, LPARAM lparam)
{
    if (code == HC_ACTION) {
        const auto* info = reinterpret_cast<KBDLLHOOKSTRUCT*>(lparam);
        const bool down = wparam == WM_KEYDOWN || wparam == WM_SYSKEYDOWN;
        if (static_cast<UINT>(info->vkCode) == g_key) {
            if (down)
                Toggle();
            return 1;
        }
        if (g_open.load()) {
            PostKey(info->vkCode, down);
            if (down)
                PostChar(info->vkCode);
            if (g_swallow)
                return 1;
        }
    }
    return CallNextHookEx(nullptr, code, wparam, lparam);
}

LRESULT CALLBACK MouseProc(int code, WPARAM wparam, LPARAM lparam)
{
    if (code == HC_ACTION) {
        const auto* info = reinterpret_cast<MSLLHOOKSTRUCT*>(lparam);
        static int seen = 0;
        if (g_open.load() && seen < 24) {
            POINT c = info->pt;
            ScreenToClient(g_hwnd, &c);
            Log("mouse msg=%04X screen=%ld,%ld client=%ld,%ld", (unsigned)wparam, info->pt.x,
                info->pt.y, c.x, c.y);
            ++seen;
        }
    }
    if (code == HC_ACTION && g_open.load()) {
        const auto* info = reinterpret_cast<MSLLHOOKSTRUCT*>(lparam);
        switch (wparam) {
        case WM_MOUSEWHEEL: {
            if (g_hwnd == nullptr)
                break;
            const int delta = GET_WHEEL_DELTA_WPARAM(info->mouseData);
            PostMessageW(g_hwnd, WM_MOUSEWHEEL, MAKEWPARAM(0, delta),
                         MAKELPARAM(info->pt.x, info->pt.y));
            break;
        }
        default:
            return CallNextHookEx(nullptr, code, wparam, lparam);
        }
        if (g_swallow)
            return 1;
    }
    return CallNextHookEx(nullptr, code, wparam, lparam);
}

DWORD WINAPI PollThread(LPVOID)
{
    bool last[3]{};
    POINT lastPt{};
    bool havePt = false;
    for (;;) {
        Sleep(15);
        if (!g_open.load()) {
            havePt = false;
            last[0] = last[1] = last[2] = false;
            continue;
        }
        if (g_hwnd == nullptr)
            continue;
        POINT pt{};
        GetCursorPos(&pt);
        if (!havePt || pt.x != lastPt.x || pt.y != lastPt.y) {
            PostMouseMove(&pt);
            lastPt = pt;
            havePt = true;
        }
        const int keys[3] = {VK_LBUTTON, VK_RBUTTON, VK_MBUTTON};
        for (int i = 0; i < 3; ++i) {
            const bool down = (GetAsyncKeyState(keys[i]) & 0x8000) != 0;
            if (down != last[i]) {
                PostButton(i, down, &pt);
                last[i] = down;
            }
        }
    }
    return 0;
}

DWORD WINAPI HookThread(LPVOID)
{
    g_thread_id = GetCurrentThreadId();
    g_kb_hook = SetWindowsHookExW(WH_KEYBOARD_LL, KeyboardProc, g_module, 0);
    g_mouse_hook = SetWindowsHookExW(WH_MOUSE_LL, MouseProc, g_module, 0);
    g_ready.store(g_kb_hook != nullptr && g_mouse_hook != nullptr);
    Log("hooks keyboard=%p mouse=%p key=0x%02X swallow=%d", g_kb_hook, g_mouse_hook, g_key,
        g_swallow ? 1 : 0);
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}

void OnPresent(command_queue*, swapchain* swapchain, const rect*, const rect*, uint32_t, const rect*)
{
    if (swapchain == nullptr)
        return;
    const auto hwnd = static_cast<HWND>(reinterpret_cast<void*>(swapchain->get_hwnd()));
    if (hwnd != nullptr && hwnd != g_hwnd) {
        g_hwnd = hwnd;
        Log("swapchain window 0x%p", hwnd);
    }
    static bool logged = false;
    if (!logged && g_ready.load()) {
        logged = true;
        Log("ready: press the toggle key to open the ReShade overlay");
    }
}

} // namespace

extern "C" __declspec(dllexport) const char* NAME = "overlay-input";
extern "C" __declspec(dllexport) const char* DESCRIPTION =
    "Routes keyboard and mouse into the render window so the ReShade overlay can be used inside "
    "Magpie, whose window never takes focus.";

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)
{
    switch (reason) {
    case DLL_PROCESS_ATTACH:
        DisableThreadLibraryCalls(module);
        g_module = module;
        OpenLog();
        ReadIni();
        if (!reshade::register_addon(module)) {
            Log("register_addon failed");
            return FALSE;
        }
        reshade::register_event<reshade::addon_event::present>(OnPresent);
        CloseHandle(CreateThread(nullptr, 0, HookThread, nullptr, 0, nullptr));
        CloseHandle(CreateThread(nullptr, 0, PollThread, nullptr, 0, nullptr));
        Log("loaded");
        break;
    case DLL_PROCESS_DETACH:
        g_open.store(false);
        if (g_kb_hook)
            UnhookWindowsHookEx(g_kb_hook);
        if (g_mouse_hook)
            UnhookWindowsHookEx(g_mouse_hook);
        reshade::unregister_addon(module);
        break;
    }
    return TRUE;
}
