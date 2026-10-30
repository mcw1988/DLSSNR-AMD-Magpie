// nr-router: keeps exactly one neural-rendering add-on active, chosen by the Magpie effect group.
//
//   effect group contains XeSS\XeSS_SR   -> amd-nr.addon64      (mochizuki, Vulkan)
//   effect group contains FSR3\FSR3_SR   -> dlss5-amd.addon64   (lmxxf, HIP)
//   a group that contains both           -> refused, nothing touched
//   anything else                        -> left alone
//
// ReShade loads *.addon64 from its own directory at start, so "active" means "named .addon64";
// the other one is renamed to *.off. A change therefore applies the next time Magpie starts.
//
// Usage:
//   nr-router [--dry-run] [--watch] [--restart] [--dir <Magpie dir>]
//     --dry-run  report the decision, touch nothing
//     --watch    keep watching config.json and re-apply when the active group changes
//     --restart  after a change, restart Magpie.exe (kills it if running)
#include <windows.h>
#include <tlhelp32.h>

#include <fcntl.h>
#include <io.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwctype>
#include <string>
#include <vector>

namespace {

const wchar_t* kMochizukiAddon = L"amd-nr.addon64";
const wchar_t* kLmxxfAddon = L"dlss5-amd.addon64";
const wchar_t* kOffSuffix = L".off";

enum class Backend { None, Mochizuki, Lmxxf, Conflict };

struct Decision {
    Backend backend = Backend::None;
    std::wstring modeName;
    int modeIndex = -1;
    bool hasXess = false;
    bool hasFsr3 = false;
    bool byName = false;
    std::wstring note;
};

std::string ReadFileUtf8(const std::wstring& path)
{
    FILE* f = _wfopen(path.c_str(), L"rb");
    if (f == nullptr)
        return {};
    std::string data;
    char buffer[65536];
    size_t n;
    while ((n = fread(buffer, 1, sizeof buffer, f)) > 0)
        data.append(buffer, n);
    fclose(f);
    return data;
}

// Returns [begin, end) of the idx-th top-level object inside the array that starts at 'arrayOpen'.
bool NthObject(const std::string& s, size_t arrayOpen, int idx, size_t& begin, size_t& end)
{
    int depth = 0;
    bool inString = false, escape = false;
    int seen = 0;
    size_t start = std::string::npos;
    for (size_t i = arrayOpen; i < s.size(); ++i) {
        const char c = s[i];
        if (inString) {
            if (escape)
                escape = false;
            else if (c == '\\')
                escape = true;
            else if (c == '"')
                inString = false;
            continue;
        }
        if (c == '"') {
            inString = true;
        } else if (c == '{') {
            ++depth;
            if (depth == 1)
                start = i;
        } else if (c == '}') {
            --depth;
            if (depth == 0 && start != std::string::npos) {
                if (seen == idx) {
                    begin = start;
                    end = i + 1;
                    return true;
                }
                ++seen;
                start = std::string::npos;
            }
        } else if (c == ']' && depth == 0) {
            break;
        }
    }
    return false;
}

std::wstring Widen(const std::string& s)
{
    if (s.empty())
        return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

Decision Decide(const std::wstring& configPath)
{
    Decision d;
    const std::string s = ReadFileUtf8(configPath);
    if (s.empty()) {
        d.note = L"config.json 读不到";
        return d;
    }

    // profiles[0].scalingMode
    const size_t profiles = s.find("\"profiles\"");
    const size_t sm = s.find("\"scalingMode\"", profiles == std::string::npos ? 0 : profiles);
    if (sm == std::string::npos) {
        d.note = L"config.json 里找不到 scalingMode";
        return d;
    }
    const size_t colon = s.find(':', sm + std::strlen("\"scalingMode\""));
    d.modeIndex = colon == std::string::npos ? 0 : std::atoi(s.c_str() + colon + 1);

    const size_t modes = s.find("\"scalingModes\"");
    const size_t bracket = s.find('[', modes == std::string::npos ? 0 : modes);
    size_t begin = 0, end = 0;
    if (begin == 0 && !NthObject(s, bracket, d.modeIndex, begin, end)) {
        d.note = L"scalingModes 里找不到第 " + std::to_wstring(d.modeIndex) + L" 项";
        return d;
    }
    const std::string mode = s.substr(begin, end - begin);

    const size_t nameKey = mode.find("\"name\"");
    if (nameKey != std::string::npos) {
        const size_t q1 = mode.find('"', mode.find(':', nameKey));
        const size_t q2 = mode.find('"', q1 + 1);
        if (q1 != std::string::npos && q2 != std::string::npos)
            d.modeName = Widen(mode.substr(q1 + 1, q2 - q1 - 1));
    }

    // The names are stored with a doubled backslash, so match the escaped text.
    d.hasXess = mode.find("\"XeSS\\\\XeSS_SR\"") != std::string::npos;
    // lmxxf's group has been FSR3_SR in some packs and FSR4_SR in others; either one means "the
    // FFX super-resolution stage is on the chain", which is what lmxxf hooks.
    d.hasFsr3 = mode.find("\"FSR3\\\\FSR3_SR\"") != std::string::npos ||
                mode.find("\"FSR4\\\\FSR4_SR\"") != std::string::npos;

    // A group can name its backend even when the chain has no SR effect at all. The Bicubic
    // chains added for 4K keep NR as a ReShade add-on, so "contains XeSS_SR" is no longer a
    // reliable proxy: NR-mochi-fast has neither XeSS_SR nor FSR3_SR and would read as "no NR".
    std::wstring lowered = d.modeName;
    for (wchar_t& c : lowered)
        c = static_cast<wchar_t>(towlower(c));
    const bool nameMochi = lowered.find(L"mochi") != std::wstring::npos;
    const bool nameLmxxf = lowered.find(L"lmxxf") != std::wstring::npos ||
                           lowered.find(L"dlss5") != std::wstring::npos;

    if (nameMochi && !nameLmxxf) {
        d.backend = Backend::Mochizuki;
        d.byName = true;
        return d;
    }
    if (nameLmxxf && !nameMochi) {
        d.backend = Backend::Lmxxf;
        d.byName = true;
        return d;
    }
    if (nameMochi && nameLmxxf) {
        d.backend = Backend::Conflict;
        d.byName = true;
        return d;
    }

    if (d.hasXess && d.hasFsr3) {
        d.backend = Backend::Conflict;
    } else if (d.hasXess) {
        d.backend = Backend::Mochizuki;
    } else if (d.hasFsr3) {
        d.backend = Backend::Lmxxf;
    } else {
        d.backend = Backend::None;
    }
    return d;
}

bool FileExists(const std::wstring& path)
{
    return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

// Magpie 0.6.x keeps its live settings in config\v4e\config.json; config\config.json is the
// legacy file that only gets read during migration. Prefer whichever of the two actually has a
// scaling-mode list, so the router follows the group the user picked in the current build.
std::wstring ResolveConfig(const std::wstring& dir)
{
    const std::wstring v4e = dir + L"\\config\\v4e\\config.json";
    const std::wstring legacy = dir + L"\\config\\config.json";
    if (FileExists(v4e) && ReadFileUtf8(v4e).find("\"scalingModes\"") != std::string::npos)
        return v4e;
    return legacy;
}

// Makes sure 'name' exists as <dir>\<name> when wanted, and as <dir>\<name>.off when not.
// Returns true when the file system changed.
bool Sync(const std::wstring& dir, const wchar_t* name, bool wanted, bool dryRun, bool& problem)
{
    const std::wstring active = dir + L"\\" + name;
    const std::wstring off = active + kOffSuffix;
    const bool hasActive = FileExists(active);
    const bool hasOff = FileExists(off);

    if (wanted) {
        if (hasActive)
            return false;
        if (!hasOff) {
            std::wprintf(L"  !! %s 既不在也不是 .off，缺文件\n", name);
            problem = true;
            return false;
        }
        std::wprintf(L"  %s 启用\n", name);
        if (dryRun)
            return true;
        return MoveFileExW(off.c_str(), active.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
    }

    if (!hasActive)
        return false;
    std::wprintf(L"  %s 停用\n", name);
    if (dryRun)
        return true;
    return MoveFileExW(active.c_str(), off.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
}

const wchar_t* BackendName(Backend b)
{
    switch (b) {
    case Backend::Mochizuki: return L"mochizuki (Vulkan, 跑在 amd-nr.addon64 里)";
    case Backend::Lmxxf: return L"lmxxf (HIP, 跑在 dlss5-amd.addon64 里)";
    case Backend::Conflict: return L"冲突";
    default: return L"无（这个效果组不挂 NR）";
    }
}

// Returns: 0 = nothing to change, 1 = changed, 2 = refused/problem
int Apply(const std::wstring& dir, const Decision& d, bool dryRun)
{
    std::wprintf(L"缩放组 [%d] %s\n", d.modeIndex, d.modeName.c_str());
    if (d.byName)
        std::wprintf(L"  按组名判定（链里可以不挂 SR 效果）\n");
    else
        std::wprintf(L"  含 XeSS\\XeSS_SR: %s   含 FSR3/FSR4_SR: %s\n", d.hasXess ? L"是" : L"否",
                     d.hasFsr3 ? L"是" : L"否");
    if (!d.note.empty()) {
        std::wprintf(L"  %s\n", d.note.c_str());
        return 2;
    }

    if (d.backend == Backend::Conflict) {
        std::wprintf(L"  !! 这个效果组同时挂了 FSR3\\FSR3_SR 和 XeSS\\XeSS_SR，按规则不允许。\n"
                     L"     请把其中一个从组里删掉；本次不动任何文件。\n");
        return 2;
    }

    std::wprintf(L"  => 应该运行: %s\n", BackendName(d.backend));
    if (d.backend == Backend::None) {
        std::wprintf(L"     这个组和 NR 无关，保持现状。\n");
        return 0;
    }

    bool problem = false;
    bool changed = false;
    changed |= Sync(dir, kMochizukiAddon, d.backend == Backend::Mochizuki, dryRun, problem);
    changed |= Sync(dir, kLmxxfAddon, d.backend == Backend::Lmxxf, dryRun, problem);

    if (!changed)
        std::wprintf(L"  已经是这个状态，无需改动。\n");
    return problem ? 2 : (changed ? 1 : 0);
}

std::wstring ExeDir()
{
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (wchar_t* slash = wcsrchr(path, L'\\'))
        slash[1] = 0;
    return path;
}

DWORD FindProcess(const wchar_t* name)
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE)
        return 0;
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof entry;
    DWORD pid = 0;
    if (Process32FirstW(snap, &entry)) {
        do {
            if (_wcsicmp(entry.szExeFile, name) == 0) {
                pid = entry.th32ProcessID;
                break;
            }
        } while (Process32NextW(snap, &entry));
    }
    CloseHandle(snap);
    return pid;
}

struct WindowSearch {
    DWORD pid = 0;
    HWND found = nullptr;
};

BOOL CALLBACK FindWindowOfProcess(HWND hwnd, LPARAM param)
{
    auto* search = reinterpret_cast<WindowSearch*>(param);
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == search->pid && IsWindowVisible(hwnd) && GetWindow(hwnd, GW_OWNER) == nullptr) {
        search->found = hwnd;
        return FALSE;
    }
    return TRUE;
}

void RestartMagpie(const std::wstring& dir)
{
    const std::wstring exe = (dir.empty() || dir.back() == L'\\' || dir.back() == L'/')
                                 ? dir + L"Magpie.exe"
                                 : dir + L"\\Magpie.exe";
    if (!FileExists(exe)) {
        std::wprintf(L"  找不到 %s，跳过重启\n", exe.c_str());
        return;
    }
    const DWORD pid = FindProcess(L"Magpie.exe");
    if (pid != 0) {
        WindowSearch search{pid, nullptr};
        EnumWindows(FindWindowOfProcess, reinterpret_cast<LPARAM>(&search));
        if (search.found != nullptr)
            PostMessageW(search.found, WM_CLOSE, 0, 0);
        HANDLE proc = OpenProcess(SYNCHRONIZE | PROCESS_TERMINATE, FALSE, pid);
        if (proc != nullptr) {
            if (WaitForSingleObject(proc, 5000) == WAIT_TIMEOUT) {
                std::wprintf(L"  Magpie 没在 5 秒内退出，强制结束\n");
                TerminateProcess(proc, 0);
                WaitForSingleObject(proc, 3000);
            }
            CloseHandle(proc);
        }
    }
    STARTUPINFOW si{};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    std::wstring cmd = L"\"" + exe + L"\"";
    // Break away from the caller's job object when allowed: a launcher started from a console
    // or a script would otherwise take Magpie down with it when it exits.
    BOOL started = CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE,
                                  CREATE_BREAKAWAY_FROM_JOB, nullptr, dir.c_str(), &si, &pi);
    if (!started)
        started = CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, 0, nullptr,
                                 dir.c_str(), &si, &pi);
    if (started) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        std::wprintf(L"  已重新启动 Magpie\n");
    } else {
        std::wprintf(L"  重启 Magpie 失败 (%lu)\n", GetLastError());
    }
}

} // namespace

int wmain(int argc, wchar_t** argv)
{
    _setmode(_fileno(stdout), _O_U8TEXT);
    bool dryRun = false, watch = false, restart = false;
    std::wstring dir;
    for (int i = 1; i < argc; ++i) {
        if (wcscmp(argv[i], L"--dry-run") == 0)
            dryRun = true;
        else if (wcscmp(argv[i], L"--watch") == 0)
            watch = true;
        else if (wcscmp(argv[i], L"--restart") == 0)
            restart = true;
        else if (wcscmp(argv[i], L"--dir") == 0 && i + 1 < argc)
            dir = argv[++i];
        else {
            std::wprintf(L"未知参数: %s\n", argv[i]);
            return 2;
        }
    }
    if (dir.empty())
        dir = ExeDir();
    while (!dir.empty() && (dir.back() == L'\\' || dir.back() == L'/'))
        dir.pop_back();

    const std::wstring config = ResolveConfig(dir);
    std::wprintf(L"目录: %s\n", dir.c_str());
    std::wprintf(L"配置: %s\n", config.c_str());

    Decision last{};
    int lastRc = 0;
    for (;;) {
        const Decision d = Decide(config);
        const bool same = d.backend == last.backend && d.modeIndex == last.modeIndex;
        if (!watch || !same) {
            const int rc = Apply(dir, d, dryRun);
            lastRc = rc;
            // Restarting is not only about the add-on file names: OpenNR shuts itself off for
            // good when Magpie destroys its D3D12 device (i.e. every time scaling stops), so a
            // group switch leaves NR dead even though the files are already correct. When the
            // caller asked for --restart, always restart rather than only on a file change.
            if (rc != 2 && !dryRun) {
                if (rc == 1)
                    std::wprintf(L"\n下一次启动 Magpie 生效（ReShade 只在启动时加载 add-on）。\n");
                if (restart) {
                    if (rc == 0)
                        std::wprintf(L"\n文件无需改动；仍重启 Magpie，让 OpenNR 在新设备上重新挂载。\n");
                    RestartMagpie(dir);
                }
            }
        }
        last = d;
        if (!watch)
            return lastRc == 2 ? 2 : 0;
        Sleep(2000);
    }
}
