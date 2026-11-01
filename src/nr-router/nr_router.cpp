// nr-router: keeps exactly one neural-rendering add-on active, chosen by the Magpie effect group.
//
// Three backends now exist, and they must never be loaded together: two of them hook the same
// ffxDispatch export, and the third is a second denoiser on a different stage of the chain.
//
//   amd-nr.addon64    mochizuki (Vulkan), ReShade present layer            -> XeSS\XeSS_SR chains
//   mochinr.addon64   mochizuki (Vulkan), inside the FFX upscale call      -> FSR chains, v0.4+
//   dlss5-amd.addon64 lmxxf (HIP),       inside the FFX upscale call      -> FSR chains
//
// Group name markers win over the effect list, because a chain can carry no SR effect at all:
//
//   name contains "mochinr" or "preupscale"      -> mochinr.addon64
//   name contains "lmxxf" or "dlss5"             -> dlss5-amd.addon64
//   name contains "mochi" (but not "mochinr")    -> amd-nr.addon64
//   more than one of the above                   -> refused, nothing touched
//
// Without a marker the effect list decides: XeSS_SR -> amd-nr, FSR3/FSR4_SR -> dlss5-amd.
// An unmarked FSR chain is genuinely ambiguous (both FSR backends hook the same call), so it keeps
// the older lmxxf routing and says so; name the group "mochinr"/"preupscale" to pick the other one.
//
// ReShade loads *.addon64 from its own directory at start, so "active" means "named .addon64";
// the others are renamed to *.off. A change therefore applies the next time Magpie starts.
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

const wchar_t* kMochizukiAddon = L"amd-nr.addon64";     // OpenNR, present layer
const wchar_t* kMochinrAddon = L"mochinr.addon64";      // v0.4, inside the FSR slot
const wchar_t* kLmxxfAddon = L"dlss5-amd.addon64";
const wchar_t* kOffSuffix = L".off";

enum class Backend { None, Mochizuki, Mochinr, Lmxxf, Conflict };

struct Decision {
    Backend backend = Backend::None;
    std::wstring modeName;
    int modeIndex = -1;
    bool hasXess = false;
    bool hasFsr = false;      // FSR3_SR or FSR4_SR: the stage mochinr and lmxxf both hook
    bool byName = false;
    std::wstring note;        // non-empty means "refused": Apply() returns 2
    std::wstring hint;        // informational only, never changes the exit code
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
    // FFX super-resolution stage is on the chain", which is what lmxxf and mochinr both hook.
    d.hasFsr = mode.find("\"FSR3\\\\FSR3_SR\"") != std::string::npos ||
               mode.find("\"FSR4\\\\FSR4_SR\"") != std::string::npos;

    // A group can name its backend even when the chain has no SR effect at all. The Bicubic
    // chains added for 4K keep NR as a ReShade add-on, so "contains XeSS_SR" is no longer a
    // reliable proxy: NR-mochi-fast has neither XeSS_SR nor FSR3_SR and would read as "no NR".
    std::wstring lowered = d.modeName;
    for (wchar_t& c : lowered)
        c = static_cast<wchar_t>(towlower(c));
    // "mochinr" contains "mochi", so the longer marker is tested first and then excluded from the
    // shorter one; "preupscale" is accepted as a synonym because it describes where mochinr sits.
    const bool nameMochinr = lowered.find(L"mochinr") != std::wstring::npos ||
                             lowered.find(L"preupscale") != std::wstring::npos ||
                             lowered.find(L"pre-upscale") != std::wstring::npos;
    // Strip the longer marker before looking for the shorter one, otherwise "NR-mochinr-mochizuki"
    // reads as a single marker instead of the contradiction it is.
    std::wstring withoutMochinr = lowered;
    for (size_t at = withoutMochinr.find(L"mochinr"); at != std::wstring::npos;
         at = withoutMochinr.find(L"mochinr"))
        withoutMochinr.erase(at, 7);
    const bool nameMochi = withoutMochinr.find(L"mochi") != std::wstring::npos;
    const bool nameLmxxf = lowered.find(L"lmxxf") != std::wstring::npos ||
                           lowered.find(L"dlss5") != std::wstring::npos;

    const int markers = (nameMochinr ? 1 : 0) + (nameMochi ? 1 : 0) + (nameLmxxf ? 1 : 0);
    if (markers > 1) {
        d.backend = Backend::Conflict;
        d.byName = true;
        d.hint = L"组名里同时出现了两个后端的标记，无法判断，本次不动任何文件。";
        return d;
    }
    if (nameMochinr) {
        d.backend = Backend::Mochinr;
        d.byName = true;
        return d;
    }
    if (nameMochi) {
        d.backend = Backend::Mochizuki;
        d.byName = true;
        return d;
    }
    if (nameLmxxf) {
        d.backend = Backend::Lmxxf;
        d.byName = true;
        return d;
    }

    if (d.hasXess && d.hasFsr) {
        d.backend = Backend::Conflict;
        d.hint = L"这个组同时挂了 XeSS_SR 和 FSR 效果器，按规则不允许。";
    } else if (d.hasXess) {
        d.backend = Backend::Mochizuki;
    } else if (d.hasFsr) {
        // Both FSR backends hook ffxDispatch, so an unmarked chain cannot be told apart from the
        // effect list alone. Keep the older lmxxf routing and say how to ask for the other one.
        d.backend = Backend::Lmxxf;
        d.hint = L"组名没有后端标记，按 FSR 效果器默认判给 lmxxf；"
                 L"要跑 mochinr 请在组名里写 mochinr（或 preupscale）。";
    } else {
        d.backend = Backend::None;
    }
    return d;
}

bool FileExists(const std::wstring& path)
{
    return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

// Either name counts as installed: the router itself is the thing that renames them.
bool Available(const std::wstring& dir, const wchar_t* name)
{
    return FileExists(dir + L"\\" + name) || FileExists(dir + L"\\" + name + kOffSuffix);
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
    case Backend::Mochizuki: return L"mochizuki (Vulkan, amd-nr.addon64，present 层)";
    case Backend::Mochinr: return L"mochizuki (Vulkan, mochinr.addon64，放大之前)";
    case Backend::Lmxxf: return L"lmxxf (HIP, dlss5-amd.addon64，放大之前)";
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
                     d.hasFsr ? L"是" : L"否");
    if (!d.note.empty()) {
        std::wprintf(L"  %s\n", d.note.c_str());
        return 2;
    }

    if (d.backend == Backend::Conflict) {
        std::wprintf(L"  !! %s\n     本次不动任何文件。\n",
                     d.hint.empty() ? L"这个效果组同时挂了 FSR 和 XeSS_SR，按规则不允许。" : d.hint.c_str());
        return 2;
    }

    std::wprintf(L"  => 应该运行: %s\n", BackendName(d.backend));
    if (!d.hint.empty())
        std::wprintf(L"     注: %s\n", d.hint.c_str());
    if (d.backend == Backend::None) {
        std::wprintf(L"     这个组和 NR 无关，保持现状。\n");
        return 0;
    }
    if (d.backend == Backend::Mochinr && !d.hasFsr)
        std::wprintf(L"     注意: 这个组没有 FSR 效果器，mochinr 钩不到 ffxDispatch，实际不会降噪。\n");

    const wchar_t* wanted = d.backend == Backend::Mochinr      ? kMochinrAddon
                          : d.backend == Backend::Mochizuki    ? kMochizukiAddon
                                                               : kLmxxfAddon;
    if (!Available(dir, wanted)) {
        std::wprintf(L"  !! %s 既不在也不是 .off，缺文件\n"
                     L"     本次不动任何文件（否则会把唯一可用的后端也停掉）。\n", wanted);
        return 2;
    }

    // Exactly one backend: the two FSR add-ons would fight over the same export, and passing two
    // denoisers down the chain is wrong even where they would not collide.
    bool problem = false;
    bool changed = false;
    changed |= Sync(dir, kMochinrAddon, d.backend == Backend::Mochinr, dryRun, problem);
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
