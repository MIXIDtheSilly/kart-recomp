// The launcher: the way from a game dump to a running kart.exe, one step per dot at the top.
//
//   1 Game          the dump folder (exefs\ and romfs\), its module ids checked, saved as KART_GAME in local.cfg
//   2 Dependencies  the tools the build needs (Visual Studio, Python, the .NET SDK); Set up installs the Python packages
//                   (tools\deps.py) and builds the shader translator from tools\xlate and its packages\ (the SPIR-V
//                   tools and everything else the host uses are in third_party\)
//   3 Build         nso.py, scan.py + funcs.py, build_all.py, gen_glue.py, the shader tools and host\build.bat, from
//                   the first stage whose output is missing; the tools' @@progress lines are drawn as a bar
//   4 Play          host\out\kart.exe on the dump
//
// The game version (buttons at the top right of every step) picks what each step reads and writes: 1.0.0 builds into
// the repository itself (kart.exe, KART_GAME), another version into ver\<version>\ with KART_VER=<version> set for the
// tools (kart-<version>.exe, KART_GAME_<version>). The choice is kept as LAUNCHER_VERSION in local.cfg, never as
// KART_VER, which there would switch every build.
//
// Settings are read as tools\paths.py reads them: the environment, else local.cfg, else the defaults.
// launcher\build.bat builds it into launcher\out\launcher.exe; Direct2D and DirectWrite draw everything, in Instrument
// Sans and DM Mono from launcher\fonts\ (SIL OFL, the licences beside them; Segoe UI and Consolas when they are missing).

#ifndef UNICODE
#define UNICODE
#endif
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <windowsx.h>
#include <d2d1.h>
#include <dwrite_3.h>
#include <dwmapi.h>
#include <shobjidl.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "shell32.lib")

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------------------------------------------
// Text

static std::wstring widen(const std::string& s, UINT cp = CP_UTF8) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(cp, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n > 0 ? n : 0, L'\0');
    if (n > 0) MultiByteToWideChar(cp, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}

static std::string narrow(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? n : 0, '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

// Tool output: UTF-8 when it is valid UTF-8 (Python), else the console code page (cl, link); colour codes dropped
static std::wstring decode_output(std::string s) {
    std::string clean;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '\x1b' && i + 1 < s.size() && s[i + 1] == '[') {
            i += 2;
            while (i < s.size() && !(s[i] >= '@' && s[i] <= '~')) i++;
            continue;
        }
        if (s[i] == '\r') continue;
        clean += s[i];
    }
    if (clean.empty()) return {};
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, clean.data(), (int)clean.size(), nullptr, 0) > 0) return widen(clean);
    return widen(clean, CP_OEMCP);
}

static std::wstring trim(const std::wstring& s) {
    const wchar_t* ws = L" \t\r\n\"\uFEFF";
    const size_t a = s.find_first_not_of(ws);
    if (a == std::wstring::npos) return {};
    return s.substr(a, s.find_last_not_of(ws) - a + 1);
}

static std::wstring lower(std::wstring s) {
    for (auto& c : s) c = (wchar_t)towlower(c);
    return s;
}

static std::wstring clock_text(ULONGLONG ms) {
    const ULONGLONG s = ms / 1000;
    wchar_t buf[32];
    if (s >= 3600) swprintf(buf, 32, L"%llu:%02llu:%02llu", s / 3600, s / 60 % 60, s % 60);
    else swprintf(buf, 32, L"%llu:%02llu", s / 60, s % 60);
    return buf;
}

// ---------------------------------------------------------------------------------------------------------------
// Settings (the rules of tools\paths.py)

static fs::path g_root;
static std::wstring g_vcvars_found;   // vswhere's answer, looked up once

static std::wstring env_var(const wchar_t* key) {
    std::wstring buf(32767, L'\0');
    const DWORD n = GetEnvironmentVariableW(key, buf.data(), (DWORD)buf.size());
    return n && n < buf.size() ? buf.substr(0, n) : L"";
}

static std::vector<std::string> read_lines(const fs::path& p) {
    std::vector<std::string> lines;
    std::ifstream f(p, std::ios::binary);
    for (std::string l; std::getline(f, l);) {
        if (!l.empty() && l.back() == '\r') l.pop_back();
        if (lines.empty() && l.rfind("\xEF\xBB\xBF", 0) == 0) l.erase(0, 3);
        lines.push_back(l);
    }
    return lines;
}

static std::map<std::wstring, std::wstring> local_cfg() {
    std::map<std::wstring, std::wstring> cfg;
    for (const auto& l : read_lines(g_root / "local.cfg")) {
        std::wstring w = widen(l);
        w = w.substr(0, w.find(L'#'));
        const size_t eq = w.find(L'=');
        if (eq != std::wstring::npos) cfg[trim(w.substr(0, eq))] = trim(w.substr(eq + 1));
    }
    return cfg;
}

static std::wstring setting(const std::wstring& key) {
    if (auto e = env_var(key.c_str()); !e.empty()) return e;
    const auto cfg = local_cfg();
    if (auto it = cfg.find(key); it != cfg.end() && !it->second.empty()) {
        fs::path v = it->second;
        if (key != L"VCVARS" && key != L"CLANG" && v.is_relative()) v = (g_root / v).lexically_normal();
        return v.wstring();
    }
    if (key == L"NVN_XLATE") return (g_root / "work" / "xlate").wstring();
    if (key == L"VCVARS") return g_vcvars_found;
    return L"";
}

// <key>=<value> in local.cfg, the other lines kept
static bool save_setting(const std::wstring& key, const std::wstring& value) {
    auto lines = read_lines(g_root / "local.cfg");
    bool found = false;
    for (auto& l : lines) {
        std::wstring w = widen(l);
        w = w.substr(0, w.find(L'#'));
        const size_t eq = w.find(L'=');
        if (eq != std::wstring::npos && trim(w.substr(0, eq)) == key) {
            l = narrow(key) + "=" + narrow(value);
            found = true;
        }
    }
    if (!found) lines.push_back(narrow(key) + "=" + narrow(value));
    std::ofstream o(g_root / "local.cfg", std::ios::binary | std::ios::trunc);
    for (const auto& l : lines) o << l << "\r\n";
    return o.good();
}

// kart.exe's own settings: the HOST_* lines of local.cfg, which kart.exe reads itself (host\localcfg.cpp; the
// environment wins). Shown on the Play step.
static std::vector<std::pair<std::wstring, std::wstring>> game_env() {
    std::vector<std::pair<std::wstring, std::wstring>> env;
    for (const auto& [k, v] : local_cfg())
        if (k.rfind(L"HOST_", 0) == 0 && env_var(k.c_str()).empty()) env.push_back({k, v});
    return env;
}

static bool path_exists(const fs::path& p) {
    std::error_code ec;
    return fs::exists(p, ec);
}

static bool any_file(const fs::path& dir, const wchar_t* pattern) {
    WIN32_FIND_DATAW fd;
    const HANDLE h = FindFirstFileExW((dir / pattern).c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, 0);
    if (h == INVALID_HANDLE_VALUE) return false;
    FindClose(h);
    return true;
}

static bool on_path(const wchar_t* exe) {
    wchar_t buf[MAX_PATH];
    return SearchPathW(nullptr, exe, nullptr, MAX_PATH, buf, nullptr) > 0;
}

// ---------------------------------------------------------------------------------------------------------------
// Processes

static HANDLE open_nul(SECURITY_ATTRIBUTES* sa) {
    return CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, sa, OPEN_EXISTING, 0, nullptr);
}

// A short command's whole output (the tool checks at startup)
static std::string capture(const std::wstring& cmdline, DWORD* exit_code = nullptr) {
    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
    HANDLE rd, wr;
    if (exit_code) *exit_code = (DWORD)-1;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return {};
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    const HANDLE nul = open_nul(&sa);
    STARTUPINFOW si{sizeof si};
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = nul;
    si.hStdOutput = si.hStdError = wr;
    PROCESS_INFORMATION pi{};
    std::wstring cl = cmdline;
    const BOOL ok = CreateProcessW(nullptr, cl.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(wr);
    CloseHandle(nul);
    std::string out;
    if (ok) {
        char buf[4096];
        DWORD n;
        while (ReadFile(rd, buf, sizeof buf, &n, nullptr) && n) out.append(buf, n);
        WaitForSingleObject(pi.hProcess, 10000);
        DWORD code = 1;
        GetExitCodeProcess(pi.hProcess, &code);
        if (exit_code) *exit_code = code;
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }
    CloseHandle(rd);
    return out;
}

struct Cmd {
    std::wstring line, cwd;
    std::wstring stage;     // the build stage it belongs to (shown while it runs), '' for none
    int stage_i = 0, stage_n = 0;
};

// What the running job is at: its stage, and the last "@@progress <done> <total> <what>" line a tool printed
// (tools\paths.py progress(), with KART_PROGRESS=1), drawn as a bar instead of logged. Under g_mx.
struct Progress {
    std::wstring stage, what;
    int stage_i = 0, stage_n = 0;
    long long done = 0, total = 0;
    ULONGLONG since = 0;    // when <what> started: for the time left
};
static Progress g_prog;

constexpr UINT WM_APP_LOG = WM_APP + 1, WM_APP_DONE = WM_APP + 2;

static HWND g_hwnd;
static std::mutex g_mx;
static std::deque<std::wstring> g_log;   // under g_mx
static size_t g_log_added = 0;           // under g_mx: lines since the UI last looked
static HANDLE g_job = nullptr;           // under g_mx: the running command's job object
static std::atomic<bool> g_running{false}, g_cancel{false}, g_log_posted{false};

static void log_line(std::wstring s) {
    if (s.rfind(L"@@progress ", 0) == 0) {
        long long done = 0, total = 0;
        int used = 0;
        if (swscanf_s(s.c_str() + 11, L"%lld %lld %n", &done, &total, &used) >= 2) {
            std::lock_guard lk(g_mx);
            std::wstring what = trim(s.substr(11 + used));
            if (what != g_prog.what || done < g_prog.done) g_prog.since = GetTickCount64();
            g_prog.what = std::move(what);
            g_prog.done = done;
            g_prog.total = total;
        }
        if (!g_log_posted.exchange(true)) PostMessageW(g_hwnd, WM_APP_LOG, 0, 0);
        return;
    }
    {
        std::lock_guard lk(g_mx);
        g_log.push_back(std::move(s));
        g_log_added++;
        while (g_log.size() > 5000) g_log.pop_front();
    }
    if (!g_log_posted.exchange(true)) PostMessageW(g_hwnd, WM_APP_LOG, 0, 0);
}

// One command, its output into the log; everything it starts is in a job object, so Stop ends the whole tree
static DWORD run_cmd(const Cmd& c) {
    log_line(L"> " + c.line);
    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
    HANDLE rd, wr;
    if (!CreatePipe(&rd, &wr, &sa, 1 << 16)) return 1;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    const HANDLE nul = open_nul(&sa);
    STARTUPINFOW si{sizeof si};
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = nul;
    si.hStdOutput = si.hStdError = wr;
    const HANDLE job = CreateJobObjectW(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION li{};
    li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    SetInformationJobObject(job, JobObjectExtendedLimitInformation, &li, sizeof li);
    PROCESS_INFORMATION pi{};
    std::wstring cl = c.line;
    const BOOL ok = CreateProcessW(nullptr, cl.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr,
                                   c.cwd.empty() ? nullptr : c.cwd.c_str(), &si, &pi);
    CloseHandle(wr);
    CloseHandle(nul);
    if (!ok) {
        const DWORD e = GetLastError();
        log_line(L"could not start it (Windows error " + std::to_wstring(e) + L")");
        CloseHandle(rd);
        CloseHandle(job);
        return e ? e : 1;
    }
    AssignProcessToJobObject(job, pi.hProcess);
    {
        std::lock_guard lk(g_mx);
        g_job = job;
    }
    if (g_cancel) TerminateJobObject(job, 1);
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);

    std::thread reader([rd] {
        std::string pending;
        char buf[8192];
        DWORD n;
        while (ReadFile(rd, buf, sizeof buf, &n, nullptr) && n) {
            pending.append(buf, n);
            for (size_t p; (p = pending.find('\n')) != std::string::npos;) {
                log_line(decode_output(pending.substr(0, p)));
                pending.erase(0, p + 1);
            }
        }
        if (!pending.empty()) log_line(decode_output(pending));
    });
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    // What it left running (mspdbsrv, build servers) would hold the pipe open: they end with it
    TerminateJobObject(job, 0);
    reader.join();
    CloseHandle(rd);
    {
        std::lock_guard lk(g_mx);
        g_job = nullptr;
    }
    CloseHandle(job);
    return code;
}

// ---------------------------------------------------------------------------------------------------------------
// What is there

enum { ST_MISSING, ST_OK, ST_BAD, ST_OPTIONAL };
struct Row {
    std::wstring name, detail;
    int st;
    std::wstring code;   // a build id or a version, set in the mono face before the detail
};

struct Tools {
    std::wstring python;      // version, '' when there is no working python
    std::wstring py_missing;  // the required Python packages it lacks (tools\deps.py check), space-separated
    bool dotnet = false, vp9 = false;
};
static Tools g_tools;

// The game versions this is made for: each module's build id (the NSO header's module id), in lifting order
struct Module {
    const wchar_t* name;
    const char* id;
};
struct Version {
    const wchar_t* name;
    std::vector<Module> modules;
};
static const Version VERSIONS[] = {
    {L"1.0.0",
     {{L"rtld", "35b31ae27e4726024edd493321d26bea1a2de891000000000000000000000000"},
      {L"main", "9e244438c3d8ef1c703a6a85ef99c4f000000000000000000000000000000000"},
      {L"subsdk0", "d39a7a3d44530bb61b35690e5b0383d51f86ff05000000000000000000000000"},
      {L"sdk", "90ebba182862b9ecdc21fcb87b99901224fb8f72000000000000000000000000"}}},
    {L"1.8.0",
     {{L"rtld", "8f204349ed459e84bf56bbc32bd4c9c30b431aaa000000000000000000000000"},
      {L"main", "d00e4f399e411d0613aa8d26b1c02fef00000000000000000000000000000000"},
      {L"subsdk0", "746302f49f095cfca3d2188423c2bd77f9c7bd71000000000000000000000000"},
      {L"subsdk1", "8dec9c0b1bed821364e77b10a07b9a287e3dab0a000000000000000000000000"},
      {L"sdk", "7a68b13d88183767fb9c7babffe89f3d368752e2000000000000000000000000"}}},
};
constexpr int VERSION_COUNT = (int)(sizeof VERSIONS / sizeof VERSIONS[0]);
static int g_ver = 0;   // the chosen one; the first builds into the repository itself

static const Version& ver() { return VERSIONS[g_ver]; }
static std::wstring ver_suffix() { return g_ver ? L"-" + std::wstring(ver().name) : L""; }
// Where the tools write the modules (out\, build\, gen\): tools\paths.py art()
static fs::path art() { return g_ver ? g_root / "ver" / ver().name : g_root; }
static std::wstring game_key() { return g_ver ? L"KART_GAME_" + std::wstring(ver().name) : L"KART_GAME"; }
static std::wstring exe_name() { return L"kart" + ver_suffix() + L".exe"; }
static fs::path shader_dir() { return g_root / "work" / (L"nvn_shaders" + ver_suffix()); }
// The tools see the chosen version through KART_VER (they inherit the launcher's environment)
static void apply_version() { SetEnvironmentVariableW(L"KART_VER", g_ver ? ver().name : nullptr); }

static std::string module_id(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    char h[0x60];
    if (!f.read(h, sizeof h) || std::memcmp(h, "NSO0", 4)) return {};
    static const char* hex = "0123456789abcdef";
    std::string s;
    for (int i = 0x40; i < 0x60; i++) {
        s += hex[(uint8_t)h[i] >> 4];
        s += hex[(uint8_t)h[i] & 15];
    }
    return s;
}

// The version of the dump in <dir>, by main's build id; -1 when it is none of them
static int dump_version(const fs::path& dir) {
    const std::string id = module_id(dir / "exefs" / "main");
    for (int v = 0; v < VERSION_COUNT; v++)
        for (const auto& m : VERSIONS[v].modules)
            if (std::wstring(m.name) == L"main" && id == m.id) return v;
    return -1;
}

// Python and its packages: "python <version>" and "missing <package> ..." from tools\deps.py check
static void check_python() {
    g_tools.python.clear();
    g_tools.py_missing.clear();
    DWORD code;
    const std::string out = capture(L"python \"" + (g_root / "tools" / "deps.py").wstring() + L"\" check", &code);
    if (code != 0) return;
    const std::wstring w = widen(out);
    size_t a = 0;
    for (size_t b; a < w.size(); a = b + 1) {
        b = w.find(L'\n', a);
        if (b == std::wstring::npos) b = w.size();
        const std::wstring l = trim(w.substr(a, b - a));
        if (l.rfind(L"python ", 0) == 0) g_tools.python = l.substr(7);
        else if (l.rfind(L"missing", 0) == 0) g_tools.py_missing = trim(l.substr(7));
    }
}

// The SPIR-V tools the shader stage runs (tools\paths.py VULKAN_BIN): the repository's copy, else the Vulkan SDK's
static bool spirv_tools() {
    if (const auto v = setting(L"VULKAN_BIN"); !v.empty()) return path_exists(fs::path(v) / "spirv-val.exe");
    if (path_exists(g_root / "third_party" / "spirv" / "bin" / "spirv-val.exe")) return true;
    if (const auto sdk = env_var(L"VULKAN_SDK"); !sdk.empty()) return path_exists(fs::path(sdk) / "Bin" / "spirv-val.exe");
    return false;
}

static void check_tools() {
    const fs::path vswhere = fs::path(env_var(L"ProgramFiles(x86)")) / "Microsoft Visual Studio" / "Installer" / "vswhere.exe";
    if (path_exists(vswhere)) {
        const std::string out = capture(L"\"" + vswhere.wstring() + L"\" -latest -products * -requires "
                                        L"Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath");
        size_t a = 0;
        for (size_t b; a < out.size(); a = b + 1) {
            b = out.find('\n', a);
            if (b == std::string::npos) b = out.size();
            const fs::path bat = fs::path(trim(widen(out.substr(a, b - a)))) / "VC" / "Auxiliary" / "Build" / "vcvars64.bat";
            if (g_vcvars_found.empty() && path_exists(bat)) g_vcvars_found = bat.wstring();
        }
    }
    // Microsoft's VP9 Video Extensions (a Store package): Media Foundation decodes the movies with it
    g_tools.vp9 = !trim(widen(capture(L"powershell -NoProfile -Command \"(Get-AppxPackage Microsoft.VP9VideoExtensions).Version\""))).empty();
    if (on_path(L"dotnet.exe")) {
        const std::string sdks = capture(L"dotnet --list-sdks");
        g_tools.dotnet = sdks.rfind("10.", 0) == 0 || sdks.find("\n10.") != std::string::npos;
    }
    check_python();
}

struct Stage {
    std::wstring name;
    std::function<bool()> done;
    std::function<std::vector<Cmd>()> cmds;
};

static std::wstring g_game;   // KART_GAME (KART_GAME_<version> for another version)

// For the chosen version: the tools write to art() and shader_dir() through KART_VER (apply_version)
static std::vector<Stage> stages() {
    const std::wstring root = g_root.wstring();
    const fs::path a = art(), sh = shader_dir(), exe = g_root / "host" / "out" / exe_name();
    const std::wstring out = L"\"" + (a / "out").wstring() + L"\"";
    const std::vector<Module> mods = ver().modules;
    const auto all = [mods](const fs::path& dir, const wchar_t* ext) {
        for (const auto& m : mods)
            if (!path_exists(dir / (std::wstring(m.name) + ext))) return false;
        return true;
    };
    return {
        {L"Unpack the modules", [=] { return all(a / "out", L".json"); },
         [=] { return std::vector<Cmd>{{L"python tools\\nso.py \"" + g_game + L"\\exefs\" " + out, root}}; }},
        {L"Find the functions", [=] { return all(a / "out", L".funcs.json"); },
         [=] {
             std::vector<Cmd> v;
             for (const auto& m : mods) {
                 v.push_back({L"python tools\\scan.py " + out + L" " + m.name, root});
                 v.push_back({L"python tools\\funcs.py " + out + L" " + m.name, root});
             }
             return v;
         }},
        {L"Lift and compile", [=] { return all(a / "build", L".lib"); },
         [=] { return std::vector<Cmd>{{L"python tools\\build_all.py", root}}; }},
        {L"Module glue", [=] { return path_exists(a / "gen" / "aot_modules.cpp"); },
         [=] { return std::vector<Cmd>{{L"python tools\\gen_glue.py", root}}; }},
        // The translation, then the renderer's reflection table (nothing is drawn without it), the mesh shaders'
        // per-vertex variants, the shaders the NVIDIA driver miscompiles as they are, and the compute shaders whose
        // barriers hang Intel GPUs (.bar.spv)
        {L"Translate the shaders", [=] { return path_exists(sh / "refl.tsv") && any_file(sh, L"*.fix.spv") && any_file(sh, L"*.bar.spv"); },
         [=] {
             return std::vector<Cmd>{{L"python tools\\nvn_shaders.py", root}, {L"python tools\\nvn_refl.py", root},
                                     {L"python tools\\nvn_mesh.py --flat", root}, {L"python tools\\nvn_spvfix.py --all", root},
                                     {L"python tools\\nvn_barfix.py", root}};
         }},
        {L"Link " + exe_name(), [=] { return path_exists(exe); },
         [=] { return std::vector<Cmd>{{L"cmd /c build.bat", (g_root / "host").wstring()}}; }},
    };
}

static std::vector<Row> g_rows[4];
static bool g_done[4];
static bool g_can_setup, g_can_build;

static void refresh() {
    g_game = setting(game_key());

    auto& game = g_rows[0];
    game.clear();
    bool game_ok = !g_game.empty();
    const int dumped = g_game.empty() ? -1 : dump_version(g_game);
    for (const auto& m : ver().modules) {
        Row r{m.name, L"", ST_MISSING};
        if (!g_game.empty()) {
            const fs::path p = fs::path(g_game) / "exefs" / m.name;
            const std::string id = path_exists(p) ? module_id(p) : "";
            if (!path_exists(p)) r = {m.name, L"not found in exefs\\", ST_BAD};
            else if (id.empty()) r = {m.name, L"not a module (NSO) file", ST_BAD};
            else if (id != m.id && dumped >= 0 && dumped != g_ver)
                r = {m.name, L"this dump is version " + std::wstring(VERSIONS[dumped].name) + L": pick it at the top right", ST_BAD};
            else if (id != m.id) r = {m.name, L"another version", ST_BAD, widen(id.substr(0, 16))};
            else r = {m.name, L"matches", ST_OK, widen(id.substr(0, 16))};
        }
        game_ok &= r.st == ST_OK;
        game.push_back(r);
    }
    if (g_game.empty()) game.push_back({L"romfs", L"", ST_MISSING});
    else if (path_exists(fs::path(g_game) / "romfs")) game.push_back({L"romfs", L"found", ST_OK});
    else {
        game.push_back({L"romfs", L"not found next to exefs\\", ST_BAD});
        game_ok = false;
    }
    g_done[0] = game_ok;

    auto& deps = g_rows[1];
    deps.clear();
    const bool vs = !setting(L"VCVARS").empty() && path_exists(setting(L"VCVARS"));
    deps.push_back({L"Visual Studio", vs ? L"C++ tools found" : L"install it with \"Desktop development with C++\"", vs ? ST_OK : ST_BAD});
    const bool py = !g_tools.python.empty(), pkgs = py && g_tools.py_missing.empty();
    if (!py) deps.push_back({L"Python", L"install Python 3.13 from python.org, with \"Add to PATH\"", ST_BAD});
    else deps.push_back({L"Python", L"", ST_OK, g_tools.python});
    deps.push_back({L"Python packages", !py ? L"" : pkgs ? L"installed" : L"Set up installs " + g_tools.py_missing,
                    pkgs ? ST_OK : ST_MISSING});
    deps.push_back({L".NET 10 SDK", g_tools.dotnet ? L"found" : L"install it from dot.net (winget install Microsoft.DotNet.SDK.10)",
                    g_tools.dotnet ? ST_OK : ST_BAD});
    const bool spirv = spirv_tools();
    deps.push_back({L"SPIR-V tools", spirv ? L"included (third_party\\spirv)" : L"missing: third_party\\spirv\\bin or the Vulkan SDK",
                    spirv ? ST_OK : ST_BAD});
    deps.push_back({L"VP9 Video Extensions", g_tools.vp9 ? L"found" : L"optional: without it the movies are skipped",
                    g_tools.vp9 ? ST_OK : ST_OPTIONAL});
    const bool xlate = path_exists(fs::path(setting(L"NVN_XLATE")) / "dmsh_xlate.exe");
    deps.push_back({L"Shader translator", xlate ? L"built" : L"Set up builds it", xlate ? ST_OK : ST_MISSING});
    g_done[1] = vs && pkgs && g_tools.dotnet && spirv && xlate;
    g_can_setup = vs && py && g_tools.dotnet;

    auto& build = g_rows[2];
    build.clear();
    bool all = true;
    for (const auto& s : stages()) {
        const bool d = s.done();
        build.push_back({s.name, d ? L"done" : L"to do", d ? ST_OK : ST_MISSING});
        all &= d;
    }
    g_done[2] = all;
    g_can_build = g_done[0] && g_done[1];

    auto& play = g_rows[3];
    play.clear();
    const bool kart = path_exists(g_root / "host" / "out" / exe_name());
    play.push_back({exe_name(), kart ? L"built" : L"not built yet", kart ? ST_OK : ST_MISSING});
    play.push_back({L"Game dump", g_done[0] ? L"checked" : L"not set", g_done[0] ? ST_OK : ST_MISSING});
    fs::path texdir = g_root / "work" / "nvn_texcache";
    if (const auto t = env_var(L"HOST_NVN_TEXCACHE"); !t.empty()) texdir = t;
    for (const auto& [k, v] : game_env())
        if (k == L"HOST_NVN_TEXCACHE") texdir = v;
    const bool tex = any_file(texdir, L"*.bc");
    play.push_back({L"Texture cache", tex ? L"filled" : L"fills as you play, or bake it", tex ? ST_OK : ST_OPTIONAL});
    const bool pipes = path_exists(g_root / "host" / "out" / (L"nvn_pipelines" + ver_suffix() + L".bin"));
    play.push_back({L"Pipeline cache", pipes ? L"there" : L"made over the first runs", pipes ? ST_OK : ST_OPTIONAL});
    g_done[3] = kart && g_done[0];
}

// ---------------------------------------------------------------------------------------------------------------
// Drawing

// Dark grey and white: neutral greys, white for what is done and the main action, so red stays the one colour of what
// is wrong. One radius scale: the panel 12, the log 8, buttons and fields 6.
namespace col {
constexpr uint32_t bg = 0x161718, panel = 0x1F2022, inset = 0x161718, line = 0x333538, line_strong = 0x45484C;
constexpr uint32_t text = 0xEDEDED, muted = 0x95989C, dim = 0x5E6165, accent = 0xF2F2F2, accent_hover = 0xD6D6D7;
constexpr uint32_t accent_ink = 0x161718, red = 0xEB6F62, log = 0xB4B6B9, btn_hover = 0x26282A, btn_off = 0x1C1D1F;
constexpr uint32_t dot_hover = 0x2A2C2F;
}
constexpr float R_PANEL = 12, R_LOG = 8, R_CTRL = 6;

static ID2D1Factory* g_d2d;
static IDWriteFactory* g_dw;
static IDWriteFontCollection* g_fonts;   // launcher\fonts\, or null when it is missing (the system's fonts)
static ID2D1HwndRenderTarget* g_rt;
static ID2D1SolidColorBrush* g_brush;
static ID2D1StrokeStyle* g_round;
static float g_dpi = 96;
static float g_alpha = 1;                // what is drawn now fades in with the step it belongs to

enum { F_TITLE, F_BODY, F_SMALL, F_LABEL, F_MONO, F_DOT, F_BUTTON, F_COUNT };
static IDWriteTextFormat* g_fmt[F_COUNT];
static IDWriteInlineObject* g_ellipsis[F_COUNT];
static float g_track[F_COUNT];           // letter spacing in DIPs (the title is set a little tight)

enum { T_LEFT = 0, T_CENTER = 1, T_RIGHT = 2, T_VCENTER = 4, T_WRAP = 8 };

static uint32_t mix(uint32_t a, uint32_t b, float t) {
    const auto ch = [&](int s) {
        const float x = (float)((a >> s) & 255), y = (float)((b >> s) & 255);
        return (uint32_t)(x + (y - x) * t + 0.5f) << s;
    };
    return ch(16) | ch(8) | ch(0);
}

static ID2D1SolidColorBrush* brush(uint32_t rgb) {
    g_brush->SetColor(D2D1::ColorF(rgb, g_alpha));
    return g_brush;
}

static void load_fonts() {
    IDWriteFactory5* f5 = nullptr;
    if (FAILED(g_dw->QueryInterface(__uuidof(IDWriteFactory5), (void**)&f5))) return;
    IDWriteFontSetBuilder1* b = nullptr;
    if (SUCCEEDED(f5->CreateFontSetBuilder(&b))) {
        bool any = false;
        std::error_code ec;
        for (fs::directory_iterator it(g_root / "launcher" / "fonts", ec), end; !ec && it != end; it.increment(ec)) {
            if (lower(it->path().extension().wstring()) != L".ttf") continue;
            IDWriteFontFile* ff = nullptr;
            if (SUCCEEDED(f5->CreateFontFileReference(it->path().c_str(), nullptr, &ff))) {
                any |= SUCCEEDED(b->AddFontFile(ff));   // a variable font adds its named instances
                ff->Release();
            }
        }
        IDWriteFontSet* set = nullptr;
        if (any && SUCCEEDED(b->CreateFontSet(&set))) {
            IDWriteFontCollection1* c = nullptr;
            if (SUCCEEDED(f5->CreateFontCollectionFromFontSet(set, &c))) g_fonts = c;
            set->Release();
        }
        b->Release();
    }
    f5->Release();
}

static bool has_family(const wchar_t* name) {
    UINT32 i;
    BOOL found = FALSE;
    return g_fonts && SUCCEEDED(g_fonts->FindFamilyName(name, &i, &found)) && found;
}

static void set_format(int f, int flags) {
    IDWriteTextFormat* fm = g_fmt[f];
    fm->SetTextAlignment(flags & T_CENTER ? DWRITE_TEXT_ALIGNMENT_CENTER : flags & T_RIGHT ? DWRITE_TEXT_ALIGNMENT_TRAILING : DWRITE_TEXT_ALIGNMENT_LEADING);
    fm->SetParagraphAlignment(flags & T_VCENTER ? DWRITE_PARAGRAPH_ALIGNMENT_CENTER : DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    fm->SetWordWrapping(flags & T_WRAP ? DWRITE_WORD_WRAPPING_WRAP : DWRITE_WORD_WRAPPING_NO_WRAP);
    DWRITE_TRIMMING tr{flags & T_WRAP ? DWRITE_TRIMMING_GRANULARITY_NONE : DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
    fm->SetTrimming(&tr, flags & T_WRAP ? nullptr : g_ellipsis[f]);
}

// A layout of <s> with the format's letter spacing (only a format with spacing needs one)
static IDWriteTextLayout* make_layout(const std::wstring& s, int f, float w, float h, int flags) {
    set_format(f, flags);
    IDWriteTextLayout* tl = nullptr;
    if (FAILED(g_dw->CreateTextLayout(s.c_str(), (UINT32)s.size(), g_fmt[f], w, h, &tl))) return nullptr;
    if (g_track[f]) {
        IDWriteTextLayout1* t1 = nullptr;
        if (SUCCEEDED(tl->QueryInterface(__uuidof(IDWriteTextLayout1), (void**)&t1))) {
            t1->SetCharacterSpacing(g_track[f] / 2, g_track[f] / 2, 0, {0, (UINT32)s.size()});
            t1->Release();
        }
    }
    return tl;
}

static void text(const std::wstring& s, int f, D2D1_RECT_F r, uint32_t color, int flags = 0) {
    if (s.empty() || r.right <= r.left) return;
    if (!g_track[f]) {
        set_format(f, flags);
        g_rt->DrawText(s.c_str(), (UINT32)s.size(), g_fmt[f], r, brush(color), D2D1_DRAW_TEXT_OPTIONS_CLIP);
        return;
    }
    if (IDWriteTextLayout* tl = make_layout(s, f, r.right - r.left, r.bottom - r.top, flags)) {
        g_rt->DrawTextLayout({r.left, r.top}, tl, brush(color), D2D1_DRAW_TEXT_OPTIONS_CLIP);
        tl->Release();
    }
}

static D2D1_SIZE_F measure(const std::wstring& s, int f, float maxw, int flags = 0) {
    D2D1_SIZE_F sz{0, 0};
    if (IDWriteTextLayout* tl = make_layout(s, f, maxw, 10000, flags)) {
        DWRITE_TEXT_METRICS m;
        tl->GetMetrics(&m);
        sz = {m.widthIncludingTrailingWhitespace, m.height};
        tl->Release();
    }
    return sz;
}

static void fill_round(D2D1_RECT_F r, float radius, uint32_t color) {
    g_rt->FillRoundedRectangle(D2D1::RoundedRect(r, radius, radius), brush(color));
}

// A 1px line just inside <r>
static void stroke_round(D2D1_RECT_F r, float radius, uint32_t color) {
    const D2D1_RECT_F in{r.left + 0.5f, r.top + 0.5f, r.right - 0.5f, r.bottom - 0.5f};
    g_rt->DrawRoundedRectangle(D2D1::RoundedRect(in, radius - 0.5f, radius - 0.5f), brush(color), 1);
}

static void fill_circle(float x, float y, float r, uint32_t color) {
    g_rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(x, y), r, r), brush(color));
}

static void stroke_circle(float x, float y, float r, uint32_t color, float width) {
    g_rt->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(x, y), r, r), brush(color), width);
}

// ---------------------------------------------------------------------------------------------------------------
// UI state

enum {
    B_NONE = -1, B_BROWSE = 1, B_CONTINUE, B_SETUP, B_BUILD, B_REBUILD, B_PLAY, B_BAKE, B_STOP, B_NUP, B_VER = 50, B_DOT = 100,
};
struct Hit {
    int id;
    D2D1_RECT_F r;
};
static std::vector<Hit> g_hits;
static int g_step = 0, g_hover = B_NONE, g_pressed = B_NONE;
static D2D1_RECT_F g_log_rect{};
static int g_log_scroll = 0;   // lines up from the end (0: following the output)
static std::wstring g_job_name, g_status;
static uint32_t g_status_color = col::muted;
static ULONGLONG g_job_start = 0;
static std::wstring g_nup_dest;    // the folder a .nup is being unpacked into (saved as the dump once that finishes)
static bool g_unpack_log = false;  // the Game step shows the unpacking's status and output

static const wchar_t* STEP_NAMES[4] = {L"Game", L"Dependencies", L"Build", L"Play"};
static const wchar_t* STEP_TITLES[4] = {L"Your game dump", L"Dependencies", L"Build", L"Play"};
static const wchar_t* STEP_TEXT[4] = {
    L"Pick the folder of your own dump of the game, the one holding exefs\\ and romfs\\, or a .nup file of it, which "
    L"is unpacked into a folder you choose. Versions 1.0.0 and 1.8.0 work, each built and played on its own (the "
    L"version buttons above).",
    L"Visual Studio, Python and the .NET SDK you install yourself. Set up does the rest: it installs the Python packages "
    L"(pip) and builds the shader translator from the packages in the repository, in a minute or two. The SPIR-V tools "
    L"and the third-party code the host uses come with the repository (third_party\\).",
    L"The game's modules are unpacked, lifted to C++ and compiled, its shaders translated to SPIR-V, and kart.exe "
    L"linked. The first build takes about an hour (the game is close to 1 GB of C++; the bar below shows how far it is); "
    L"later ones redo only what is missing.",
    L"Starts kart.exe on your dump. Pipelines are made ahead of time over the first runs; baking decodes every "
    L"texture into the cache ahead of time, so the first races do not hitch.",
};

static void redraw() {
    InvalidateRect(g_hwnd, nullptr, FALSE);
}

// ---------------------------------------------------------------------------------------------------------------
// Motion: every control answers the pointer. Hover: the fill steps and a button rises 1px (150ms in, 120ms out);
// press: it sinks to 1px below rest and scales to .985 (90ms). A new step's content rises 6px into place as it fades
// in (200ms). Windows' "Animation effects" off: every state is reached at once.

struct Anim {
    float hov = 0, press = 0;   // 0..1, linear in time; ease() shapes them
};
static std::map<int, Anim> g_anim;
static bool g_motion = true, g_moving = false;
static ULONGLONG g_frame_ms = 0, g_step_ms = 0;
static int g_shown_step = -1;
constexpr UINT_PTR TIMER_JOB = 1, TIMER_ANIM = 2;

static float ease(float p) {   // an ease-out close to cubic-bezier(.2, .8, .2, 1)
    p = std::clamp(p, 0.f, 1.f);
    return 1 - (1 - p) * (1 - p) * (1 - p);
}

static Anim anim(int id) {
    const auto it = g_anim.find(id);
    return it == g_anim.end() ? Anim{} : it->second;
}

// Moves every control toward its state; true while something is still on its way
static bool step_anims() {
    const ULONGLONG now = GetTickCount64();
    const float dt = g_moving ? (float)std::min<ULONGLONG>(now - g_frame_ms, 50) : 16.f;
    g_frame_ms = now;
    if (g_hover != B_NONE) g_anim[g_hover];
    if (g_pressed != B_NONE) g_anim[g_pressed];
    bool moving = false;
    const auto go = [&](float& v, float to, float in_ms, float out_ms) {
        if (!g_motion) v = to;
        else if (to > v) v = std::min(to, v + dt / in_ms);
        else v = std::max(to, v - dt / out_ms);
        moving |= v != to;
    };
    for (auto it = g_anim.begin(); it != g_anim.end();) {
        const int id = it->first;
        go(it->second.hov, id == g_hover ? 1.f : 0.f, 150, 120);
        go(it->second.press, id == g_hover && id == g_pressed ? 1.f : 0.f, 90, 90);
        if (it->second.hov == 0 && it->second.press == 0 && id != g_hover) it = g_anim.erase(it);
        else ++it;
    }
    if (g_step != g_shown_step) {
        g_step_ms = g_shown_step < 0 ? 0 : now;   // the window's first paint does not fade
        g_shown_step = g_step;
    }
    moving |= g_motion && g_step_ms && now - g_step_ms < 200;
    return moving;
}

// Draws with a control's hover rise and press sink around <r>'s centre
struct Lift {
    D2D1_MATRIX_3X2_F old;
    Lift(D2D1_RECT_F r, const Anim& a, float rise = 1) {
        g_rt->GetTransform(&old);
        const float h = ease(a.hov), p = ease(a.press), s = 1 - 0.015f * p;
        const float dy = -rise * h * (1 - p) + p;
        g_rt->SetTransform(D2D1::Matrix3x2F::Scale(s, s, {(r.left + r.right) / 2, (r.top + r.bottom) / 2}) *
                           D2D1::Matrix3x2F::Translation(0, dy) * old);
    }
    ~Lift() { g_rt->SetTransform(old); }
};

// ---------------------------------------------------------------------------------------------------------------
// Drawing

static float button_width(const std::wstring& label) {
    return std::max(112.f, measure(label, F_BUTTON, 1000).width + 40);
}

static float button(int id, const std::wstring& label, float x, float y, bool primary, bool enabled) {
    const float w = button_width(label);
    const D2D1_RECT_F r{x, y, x + w, y + 40};
    if (enabled) g_hits.push_back({id, r});
    const Anim a = enabled ? anim(id) : Anim{};
    const float h = ease(a.hov);
    Lift lift(r, a);
    if (!enabled) {
        fill_round(r, R_CTRL, col::btn_off);
        text(label, F_BUTTON, r, col::dim, T_CENTER | T_VCENTER);
    } else if (primary) {
        fill_round(r, R_CTRL, mix(col::accent, col::accent_hover, h));
        text(label, F_BUTTON, r, col::accent_ink, T_CENTER | T_VCENTER);
    } else {
        fill_round(r, R_CTRL, mix(col::inset, col::btn_hover, h));
        stroke_round(r, R_CTRL, mix(col::line_strong, col::muted, h * 0.35f));
        text(label, F_BUTTON, r, col::text, T_CENTER | T_VCENTER);
    }
    return x + w + 12;
}

static void marker(float x, float y, int st) {
    if (st == ST_OK) fill_circle(x, y, 4.5f, col::accent);
    else if (st == ST_BAD) fill_circle(x, y, 4.5f, col::red);
    else stroke_circle(x, y, 4, st == ST_OPTIONAL ? col::line_strong : col::muted, 1.25f);
}

static float draw_rows(const std::vector<Row>& rows, int cols, float x0, float x1, float y) {
    const int per = (int)((rows.size() + cols - 1) / cols);
    const float gap = 32, colw = (x1 - x0 - gap * (cols - 1)) / cols, rh = 28;
    const float namew = std::min(170.f, colw * 0.46f);
    for (size_t i = 0; i < rows.size(); i++) {
        const auto& r = rows[i];
        const float x = x0 + (i / per) * (colw + gap), yy = y + (i % per) * rh;
        marker(x + 5, yy + rh / 2, r.st);
        text(r.name, F_BODY, {x + 20, yy, x + 20 + namew, yy + rh}, r.st == ST_OPTIONAL ? col::muted : col::text, T_VCENTER);
        float dx = x + 20 + namew + 8;
        if (!r.code.empty()) {
            text(r.code, F_MONO, {dx, yy, x + colw, yy + rh}, r.st == ST_BAD ? col::red : col::text, T_VCENTER);
            dx += measure(r.code, F_MONO, 1000).width + 10;
        }
        text(r.detail, F_SMALL, {dx, yy, x + colw, yy + rh}, r.st == ST_BAD ? col::red : col::muted, T_VCENTER);
    }
    return y + per * rh;
}

static void draw_status(float x, float x1, float y) {
    if (g_running) {
        fill_circle(x + 5, y + 20, 4.5f, col::accent);
        std::wstring s = g_job_name;
        {
            std::lock_guard lk(g_mx);
            if (!g_prog.stage.empty()) {
                s += L": " + g_prog.stage;
                if (g_prog.stage_n > 1) s += L" (" + std::to_wstring(g_prog.stage_i) + L" of " + std::to_wstring(g_prog.stage_n) + L")";
            }
        }
        text(s + L"   " + clock_text(GetTickCount64() - g_job_start), F_BODY, {x + 18, y, x1, y + 40}, col::text, T_VCENTER);
    } else {
        text(g_status, F_BODY, {x, y, x1, y + 40}, g_status_color, T_VCENTER);
    }
}

// The running tool's progress: a bar, what it is doing, n of total and the time left; returns the y below it
static float draw_progress(float x0, float x1, float y) {
    Progress p;
    {
        std::lock_guard lk(g_mx);
        p = g_prog;
    }
    if (!g_running || p.total <= 0) return y;
    const float f = std::clamp((float)p.done / (float)p.total, 0.f, 1.f);
    std::wstring s = (p.what.empty() ? std::wstring(L"Working") : p.what) + L"   " + std::to_wstring(p.done) + L" of " +
                     std::to_wstring(p.total) + L"   " + std::to_wstring((int)(f * 100)) + L"%";
    const ULONGLONG el = GetTickCount64() - p.since;
    if (p.done > 0 && p.done < p.total && el > 10000)
        s += L"   about " + clock_text((ULONGLONG)((double)el * (p.total - p.done) / p.done)) + L" left";
    text(s, F_SMALL, {x0, y, x1, y + 20}, col::muted, T_VCENTER);
    const D2D1_RECT_F bar{x0, y + 26, x1, y + 32};
    fill_round(bar, 3, col::inset);
    if (f > 0) fill_round({x0, bar.top, x0 + std::max(6.f, (x1 - x0) * f), bar.bottom}, 3, col::accent);
    return y + 46;
}

static void draw_log(D2D1_RECT_F r) {
    g_log_rect = r;
    fill_round(r, R_LOG, col::inset);
    stroke_round(r, R_LOG, col::line);
    const float pad = 16, lh = 19;
    const D2D1_RECT_F in{r.left + pad, r.top + pad - 2, r.right - pad, r.bottom - pad + 2};
    std::lock_guard lk(g_mx);
    const int vis = std::max(1, (int)((in.bottom - in.top) / lh));
    const int total = (int)g_log.size();
    g_log_scroll = std::clamp(g_log_scroll, 0, std::max(0, total - vis));
    if (!total) {
        text(L"Output shows up here.", F_MONO, in, col::dim);
        return;
    }
    g_rt->PushAxisAlignedClip(in, D2D1_ANTIALIAS_MODE_ALIASED);
    const int first = std::max(0, total - vis - g_log_scroll);
    for (int i = first, k = 0; i < total && k < vis; i++, k++) {
        const std::wstring& l = g_log[i];
        const std::wstring lo = lower(l);
        uint32_t c = col::log;
        if (l.rfind(L"> ", 0) == 0) c = col::accent;
        else if (lo.find(L"error") != std::wstring::npos || lo.find(L"failed") != std::wstring::npos) c = col::red;
        text(l, F_MONO, {in.left, in.top + k * lh, in.right, in.top + (k + 1) * lh}, c);
    }
    g_rt->PopAxisAlignedClip();
}

static void draw_check(float x, float y, uint32_t color) {
    auto* b = brush(color);
    g_rt->DrawLine({x - 6.5f, y + 0.5f}, {x - 2.2f, y + 4.8f}, b, 2.4f, g_round);
    g_rt->DrawLine({x - 2.2f, y + 4.8f}, {x + 7, y - 5}, b, 2.4f, g_round);
}

static void draw_play_icon(float x, float y, uint32_t color) {
    ID2D1PathGeometry* g = nullptr;
    ID2D1GeometrySink* s = nullptr;
    if (FAILED(g_d2d->CreatePathGeometry(&g))) return;
    g->Open(&s);
    s->BeginFigure({x - 5, y - 8}, D2D1_FIGURE_BEGIN_FILLED);
    s->AddLine({x + 8.5f, y});
    s->AddLine({x - 5, y + 8});
    s->EndFigure(D2D1_FIGURE_END_CLOSED);
    s->Close();
    s->Release();
    g_rt->FillGeometry(g, brush(color));
    g->Release();
}

// The game version: one segmented control, right-aligned in the title row; returns where it starts
static float draw_versions(float x1, float y, bool enabled) {
    float seg[VERSION_COUNT], total = 6;
    for (int i = 0; i < VERSION_COUNT; i++) total += seg[i] = measure(VERSIONS[i].name, F_LABEL, 200).width + 26;
    const D2D1_RECT_F box{x1 - total, y + 1, x1, y + 33};
    fill_round(box, R_CTRL, col::inset);
    stroke_round(box, R_CTRL, col::line);
    float x = box.left + 3;
    for (int i = 0; i < VERSION_COUNT; i++) {
        const D2D1_RECT_F r{x, box.top + 3, x + seg[i], box.bottom - 3};
        const bool sel = i == g_ver;
        if (sel) {
            fill_round(r, R_CTRL - 2, col::accent);
            text(VERSIONS[i].name, F_LABEL, r, col::accent_ink, T_CENTER | T_VCENTER);
        } else {
            const float h = enabled ? ease(anim(B_VER + i).hov) : 0;
            if (h > 0) fill_round(r, R_CTRL - 2, mix(col::inset, col::btn_hover, h));
            text(VERSIONS[i].name, F_LABEL, r, enabled ? mix(col::muted, col::text, h) : col::dim, T_CENTER | T_VCENTER);
            if (enabled) g_hits.push_back({B_VER + i, r});
        }
        x += seg[i];
    }
    const float lw = measure(L"Game version", F_SMALL, 200).width;
    text(L"Game version", F_SMALL, {box.left - lw - 12, box.top, box.left - 10, box.bottom}, col::muted, T_RIGHT | T_VCENTER);
    return box.left - lw - 24;
}

static void paint_steps(D2D1_RECT_F p) {
    const float x0 = p.left + 36, x1 = p.right - 36;
    float y = p.top + 30;
    const float tx1 = draw_versions(x1, y, !g_running);
    text(STEP_TITLES[g_step], F_TITLE, {x0, y - 1, tx1, y + 35}, col::text);
    y += 42;
    const float dh = measure(STEP_TEXT[g_step], F_BODY, x1 - x0, T_WRAP).height;
    text(STEP_TEXT[g_step], F_BODY, {x0, y, x1, y + dh + 4}, col::muted, T_WRAP);
    y += dh + 26;

    const bool busy = g_running;
    switch (g_step) {
    case 0: {
        text(L"Game folder", F_LABEL, {x0, y, x1, y + 18}, col::muted);
        y += 24;
        const float bw = button_width(L"Browse…"), nw = button_width(L"Open .nup…");
        const D2D1_RECT_F box{x0, y, x1 - bw - nw - 24, y + 40};
        fill_round(box, R_CTRL, col::inset);
        stroke_round(box, R_CTRL, col::line);
        const std::wstring shown = busy && !g_nup_dest.empty() ? g_nup_dest : g_game;
        text(shown.empty() ? L"Not chosen yet" : shown, F_BODY, {box.left + 14, box.top, box.right - 14, box.bottom},
             shown.empty() ? col::dim : col::text, T_VCENTER);
        button(B_NUP, L"Open .nup…", x1 - bw - nw - 12, y, false, !busy);
        button(B_BROWSE, L"Browse…", x1 - bw, y, g_game.empty(), !busy);
        y += 60;
        y = draw_rows(g_rows[0], 1, x0, x1, y);
        if (!env_var(game_key().c_str()).empty()) {
            text(game_key() + L" is set in the environment: it is used instead of local.cfg.", F_SMALL, {x0, y + 8, x1, y + 28}, col::muted);
            y += 28;
        }
        if (g_unpack_log || busy) {   // a .nup being unpacked (or just done): its status and output
            y += 18;
            float x = x0;
            if (busy) x = button(B_STOP, L"Stop", x, y, false, true);
            else if (g_done[0]) x = button(B_CONTINUE, L"Continue", x, y, true, true);
            draw_status(x + 4, x1, y);
            y += 56;
            draw_log({x0, y, x1, p.bottom - 30});
        } else if (g_done[0]) button(B_CONTINUE, L"Continue", x0, y + 18, true, true);
        break;
    }
    case 1: {
        y = draw_rows(g_rows[1], 2, x0, x1, y) + 18;
        float x = x0;
        if (busy) x = button(B_STOP, L"Stop", x, y, false, true);
        else x = button(B_SETUP, g_done[1] ? L"Set up again" : L"Set up", x, y, !g_done[1], g_can_setup);
        if (!busy && g_done[1]) x = button(B_CONTINUE, L"Continue", x, y, true, true);
        if (!busy && !g_can_setup && g_status.empty())
            text(L"Install what is marked red first.", F_BODY, {x + 4, y, x1, y + 40}, col::muted, T_VCENTER);
        else draw_status(x + 4, x1, y);
        y = draw_progress(x0, x1, y + 56);
        draw_log({x0, y, x1, p.bottom - 30});
        break;
    }
    case 2: {
        auto rows = g_rows[2];
        if (busy) {   // the stage running now
            std::lock_guard lk(g_mx);
            for (auto& r : rows)
                if (r.name == g_prog.stage) r.detail = L"running…";
        }
        y = draw_rows(rows, 2, x0, x1, y);
        const std::wstring mp = env_var(L"BUILD_MP");
        text(L"Up to " + mp + L" compiler processes at once (BUILD_MP in local.cfg or the environment)", F_SMALL,
             {x0, y + 6, x1, y + 26}, col::muted);
        y += 40;
        float x = x0;
        if (busy) x = button(B_STOP, L"Stop", x, y, false, true);
        else {
            x = button(B_BUILD, L"Build", x, y, !g_done[2], g_can_build);
            x = button(B_REBUILD, L"Rebuild everything", x, y, false, g_can_build);
            if (g_done[2]) x = button(B_CONTINUE, L"Continue", x, y, true, true);
        }
        if (!busy && !g_can_build && g_status.empty()) {
            text(!g_done[0] ? L"Choose the game dump first." : L"Set up the dependencies first.", F_BODY, {x + 4, y, x1, y + 40},
                 col::muted, T_VCENTER);
        } else draw_status(x + 4, x1, y);
        y = draw_progress(x0, x1, y + 56);
        draw_log({x0, y, x1, p.bottom - 30});
        break;
    }
    case 3: {
        y = draw_rows(g_rows[3], 2, x0, x1, y);
        std::wstring settings;
        for (const auto& [k, v] : game_env()) settings += (settings.empty() ? L"" : L"   ") + k + L"=" + v;
        text(L"Settings: " + (settings.empty() ? std::wstring(L"the defaults") : settings) + L"   (HOST_* lines in local.cfg)", F_SMALL,
             {x0, y + 6, x1, y + 26}, col::muted);
        y += 40;
        float x = x0;
        if (busy) x = button(B_STOP, L"Stop", x, y, false, true);
        else {
            x = button(B_PLAY, L"Play", x, y, true, g_done[3]);
            x = button(B_BAKE, L"Bake textures", x, y, false, g_done[3]);
        }
        draw_status(x + 4, x1, y);
        y = draw_progress(x0, x1, y + 56);
        draw_log({x0, y, x1, p.bottom - 30});
        break;
    }
    }
}

// The panel, its content rising into place when the step has just changed
static void paint_panel(D2D1_RECT_F p) {
    fill_round(p, R_PANEL, col::panel);
    stroke_round(p, R_PANEL, col::line);
    const float t = g_motion && g_step_ms ? ease((GetTickCount64() - g_step_ms) / 200.f) : 1;
    D2D1_MATRIX_3X2_F old;
    g_rt->GetTransform(&old);
    g_rt->SetTransform(D2D1::Matrix3x2F::Translation(0, (1 - t) * 6) * old);
    g_alpha = t;
    paint_steps(p);
    g_alpha = 1;
    g_rt->SetTransform(old);
}

static void paint() {
    if (!g_rt) {
        RECT rc;
        GetClientRect(g_hwnd, &rc);
        const auto props = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT, D2D1::PixelFormat(), g_dpi, g_dpi);
        if (FAILED(g_d2d->CreateHwndRenderTarget(props, D2D1::HwndRenderTargetProperties(g_hwnd, D2D1::SizeU(rc.right, rc.bottom)), &g_rt)))
            return;
        g_rt->CreateSolidColorBrush(D2D1::ColorF(0), &g_brush);
    }
    const bool moving = step_anims();
    g_rt->BeginDraw();
    g_rt->Clear(D2D1::ColorF(col::bg));
    g_hits.clear();
    g_log_rect = {};
    const D2D1_SIZE_F sz = g_rt->GetSize();
    const float m = 32, r = 18, cy = 58;

    // The steps: dots on a line, the finished ones white
    float cx[4];
    for (int i = 0; i < 4; i++) cx[i] = m + 46 + (sz.width - 2 * m - 92) * i / 3.f;
    for (int i = 0; i < 3; i++)
        g_rt->FillRectangle({cx[i] + r, cy - 1, cx[i + 1] - r, cy + 1}, brush(g_done[i] && g_done[i + 1] ? col::accent : col::line));
    for (int i = 0; i < 4; i++) {
        const D2D1_RECT_F hit{cx[i] - r - 8, cy - r - 8, cx[i] + r + 8, cy + r + 30};
        g_hits.push_back({B_DOT + i, hit});
        const bool sel = i == g_step;
        const Anim a = anim(B_DOT + i);
        const float h = ease(a.hov);
        if (sel) stroke_circle(cx[i], cy, r + 5, col::accent, 1.5f);
        {
            Lift lift({cx[i] - r, cy - r, cx[i] + r, cy + r}, a, 0);
            if (g_done[i] || sel) {
                fill_circle(cx[i], cy, r, mix(col::accent, col::accent_hover, h));
                if (g_done[i] && i == 3) draw_play_icon(cx[i] + 1, cy, col::accent_ink);
                else if (g_done[i]) draw_check(cx[i], cy, col::accent_ink);
                else text(std::to_wstring(i + 1), F_DOT, {cx[i] - r, cy - r, cx[i] + r, cy + r}, col::accent_ink, T_CENTER | T_VCENTER);
            } else {
                fill_circle(cx[i], cy, r, mix(col::panel, col::dot_hover, h));
                stroke_circle(cx[i], cy, r - 0.5f, col::line_strong, 1);
                text(std::to_wstring(i + 1), F_DOT, {cx[i] - r, cy - r, cx[i] + r, cy + r}, mix(col::muted, col::text, h), T_CENTER | T_VCENTER);
            }
        }
        text(STEP_NAMES[i], F_LABEL, {cx[i] - 70, cy + r + 12, cx[i] + 70, cy + r + 32}, sel ? col::text : mix(col::muted, col::text, h), T_CENTER);
    }

    paint_panel({m, cy + r + 48, sz.width - m, sz.height - m});

    if (g_rt->EndDraw() == D2DERR_RECREATE_TARGET) {
        g_brush->Release();
        g_rt->Release();
        g_brush = nullptr;
        g_rt = nullptr;
        redraw();
    }
    // The next frame while something moves; the timer stops once everything rests
    g_moving = moving;
    if (moving) SetTimer(g_hwnd, TIMER_ANIM, 15, nullptr);
    else KillTimer(g_hwnd, TIMER_ANIM);
}

// ---------------------------------------------------------------------------------------------------------------
// Actions

static void start_job(const std::wstring& name, std::vector<Cmd> cmds) {
    if (g_running || cmds.empty()) return;
    {
        std::lock_guard lk(g_mx);
        g_log.clear();
        g_log_added = 0;
    }
    g_log_scroll = 0;
    g_running = true;
    g_cancel = false;
    g_job_name = name;
    g_job_start = GetTickCount64();
    g_status.clear();
    {
        std::lock_guard lk(g_mx);
        g_prog = {};
    }
    SetTimer(g_hwnd, TIMER_JOB, 500, nullptr);
    std::thread([cmds = std::move(cmds)] {
        DWORD code = 0;
        for (const auto& c : cmds) {
            {
                std::lock_guard lk(g_mx);
                g_prog = {c.stage, L"", c.stage_i, c.stage_n};   // each command reports its own progress
            }
            code = run_cmd(c);
            if (code || g_cancel) break;
        }
        PostMessageW(g_hwnd, WM_APP_DONE, code, g_cancel ? 1 : 0);
    }).detach();
    redraw();
}

static void stop_job() {
    g_cancel = true;
    std::lock_guard lk(g_mx);
    if (g_job) TerminateJobObject(g_job, 1);
}

static void job_done(DWORD code, bool cancelled) {
    g_running = false;
    KillTimer(g_hwnd, TIMER_JOB);
    const std::wstring took = clock_text(GetTickCount64() - g_job_start);
    if (cancelled) {
        g_status = L"Stopped after " + took;
        g_status_color = col::muted;
    } else if (code == 0) {
        g_status = L"Finished in " + took;
        g_status_color = col::accent;
    } else {
        wchar_t c[32];
        swprintf(c, 32, code > 0xFFFF ? L"0x%08X" : L"%u", code);
        g_status = L"Failed (exit code " + std::wstring(c) + L") after " + took;
        g_status_color = col::red;
    }
    // A .nup unpacked: its folder is the dump now, of the version it holds (the output stays in view)
    if (const std::wstring dest = std::exchange(g_nup_dest, std::wstring()); !dest.empty() && !cancelled && code == 0) {
        if (const int v = dump_version(dest); v >= 0 && v != g_ver) {
            g_ver = v;
            apply_version();
            save_setting(L"LAUNCHER_VERSION", ver().name);
        }
        if (!save_setting(game_key(), dest)) MessageBoxW(g_hwnd, L"Could not write local.cfg.", L"Launcher", MB_ICONERROR);
        g_status = L"Unpacked in " + took + L" into " + dest;
    }
    const int step = g_step;
    if (step == 1) check_python();   // packages Set up just installed
    refresh();
    if (!cancelled && code == 0 && (step == 1 || step == 2) && g_done[step]) g_step = step + 1, g_status.clear();
    redraw();
}

static void select_version(int v) {
    g_ver = v;
    apply_version();
    save_setting(L"LAUNCHER_VERSION", ver().name);
    g_status.clear();
    {
        std::lock_guard lk(g_mx);
        g_log.clear();
        g_log_added = 0;
    }
    refresh();
}

static void browse() {
    IFileOpenDialog* d = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&d)))) return;
    DWORD o = 0;
    d->GetOptions(&o);
    d->SetOptions(o | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    d->SetTitle(L"The game dump: the folder holding exefs and romfs");
    if (!g_game.empty()) {
        IShellItem* si = nullptr;
        if (SUCCEEDED(SHCreateItemFromParsingName(g_game.c_str(), nullptr, IID_PPV_ARGS(&si)))) {
            d->SetFolder(si);
            si->Release();
        }
    }
    std::wstring picked;
    if (SUCCEEDED(d->Show(g_hwnd))) {
        IShellItem* it = nullptr;
        if (SUCCEEDED(d->GetResult(&it))) {
            PWSTR p = nullptr;
            if (SUCCEEDED(it->GetDisplayName(SIGDN_FILESYSPATH, &p))) {
                picked = p;
                CoTaskMemFree(p);
            }
            it->Release();
        }
    }
    d->Release();
    if (picked.empty()) return;
    // exefs\ or romfs\ itself picked: the dump is the folder above
    const fs::path pp = picked;
    const std::wstring leaf = lower(pp.filename().wstring());
    if ((leaf == L"exefs" || leaf == L"romfs") && !path_exists(pp / "exefs")) picked = pp.parent_path().wstring();
    // A dump of another known version switches to it, and is saved as that version's dump
    if (const int v = dump_version(picked); v >= 0 && v != g_ver) select_version(v);
    if (!save_setting(game_key(), picked)) MessageBoxW(g_hwnd, L"Could not write local.cfg.", L"Launcher", MB_ICONERROR);
    g_unpack_log = false;
    refresh();
    redraw();
}

// A file or folder dialog; '' when cancelled
static std::wstring pick(bool folder, const wchar_t* title, const std::wstring& start) {
    IFileOpenDialog* d = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&d)))) return {};
    DWORD o = 0;
    d->GetOptions(&o);
    d->SetOptions(o | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST | (folder ? FOS_PICKFOLDERS : FOS_FILEMUSTEXIST));
    d->SetTitle(title);
    if (!folder) {
        const COMDLG_FILTERSPEC types[] = {{L"NUP game container (*.nup)", L"*.nup"}, {L"All files", L"*.*"}};
        d->SetFileTypes(2, types);
    }
    if (!start.empty()) {
        IShellItem* si = nullptr;
        if (SUCCEEDED(SHCreateItemFromParsingName(start.c_str(), nullptr, IID_PPV_ARGS(&si)))) {
            d->SetFolder(si);
            si->Release();
        }
    }
    std::wstring picked;
    if (SUCCEEDED(d->Show(g_hwnd))) {
        IShellItem* it = nullptr;
        if (SUCCEEDED(d->GetResult(&it))) {
            PWSTR p = nullptr;
            if (SUCCEEDED(it->GetDisplayName(SIGDN_FILESYSPATH, &p))) {
                picked = p;
                CoTaskMemFree(p);
            }
            it->Release();
        }
    }
    d->Release();
    return picked;
}

// A .nup (one file holding the game, its updates and add-ons): tools\nup.py says which version it holds, and unpacks it
// (the newest update over the base, as Eglantine boots it) into a folder named after that version in a folder the user
// picks. Unpacking again into the same folder only writes what is missing.
static void open_nup() {
    const std::wstring file = pick(false, L"The game as a .nup file", fs::path(g_game).parent_path().wstring());
    if (file.empty()) return;
    const std::wstring nup_py = L"python \"" + (g_root / "tools" / "nup.py").wstring() + L"\"";
    DWORD code;
    const std::string out = capture(nup_py + L" info \"" + file + L"\"", &code);
    std::wstring version;
    unsigned long long bytes = 0;
    for (size_t a = 0, b; a < out.size(); a = b + 1) {
        b = out.find('\n', a);
        if (b == std::string::npos) b = out.size();
        const std::wstring l = trim(widen(out.substr(a, b - a)));
        if (l.rfind(L"version ", 0) == 0) version = l.substr(8);
        else if (l.rfind(L"bytes ", 0) == 0) bytes = _wcstoui64(l.c_str() + 6, nullptr, 10);
    }
    if (code != 0 || version.empty()) {
        std::wstring why = trim(decode_output(out));
        if (why.size() > 600) why = L"…" + why.substr(why.size() - 600);
        MessageBoxW(g_hwnd, (L"tools\\nup.py could not read this file.\n\n" + why).c_str(), L"Open .nup", MB_ICONERROR);
        return;
    }
    const bool known = std::any_of(std::begin(VERSIONS), std::end(VERSIONS), [&](const Version& v) { return version == v.name; });
    if (!known && MessageBoxW(g_hwnd, (L"This .nup holds version " + version + L", which is not one this recompilation is made "
                                       L"for (1.0.0 and 1.8.0). Unpack it anyway?").c_str(),
                              L"Open .nup", MB_YESNO | MB_ICONWARNING) != IDYES)
        return;
    const std::wstring title = L"Where to unpack version " + version + L" (a folder \"" + version + L"\" is made in it)";
    const std::wstring parent = pick(true, title.c_str(), fs::path(file).parent_path().wstring());
    if (parent.empty()) return;
    fs::path dest = parent;
    if (lower(dest.filename().wstring()) != lower(version)) dest /= version;
    // Room for it: the files still to write, less what an earlier unpacking there already wrote (roughly)
    ULARGE_INTEGER room{};
    if (GetDiskFreeSpaceExW(parent.c_str(), &room, nullptr, nullptr) && !path_exists(dest / "romfs") &&
        room.QuadPart < bytes + (1ull << 30)) {
        wchar_t msg[256];
        swprintf(msg, 256, L"Version %s needs %.1f GB, and only %.1f GB are free there. Unpack anyway?", version.c_str(),
                 bytes / 1e9, room.QuadPart / 1e9);
        if (MessageBoxW(g_hwnd, msg, L"Open .nup", MB_YESNO | MB_ICONWARNING) != IDYES) return;
    }
    g_nup_dest = dest.wstring();
    g_unpack_log = true;
    start_job(L"Unpacking version " + version, {{nup_py + L" extract \"" + file + L"\" \"" + g_nup_dest + L"\"", g_root.wstring()}});
}

static void click(int id) {
    const std::wstring root = g_root.wstring();
    if (id >= B_DOT) {
        g_step = id - B_DOT;
        refresh();
    } else if (id >= B_VER && id < B_VER + VERSION_COUNT) {
        select_version(id - B_VER);
    } else if (id == B_BROWSE) {
        browse();
    } else if (id == B_NUP) {
        open_nup();
    } else if (id == B_CONTINUE) {
        g_step = std::min(3, g_step + 1);
        g_status.clear();
        refresh();
    } else if (id == B_SETUP) {
        // The Python packages (pip, from PyPI), then the translator from the NuGet packages in tools\xlate\packages
        std::vector<Cmd> cmds;
        if (!g_tools.py_missing.empty()) cmds.push_back({L"python tools\\deps.py install", root, L"Python packages", 1, 2});
        cmds.push_back({L"dotnet build -c Release -o \"" + setting(L"NVN_XLATE") + L"\" tools\\xlate\\dmsh_xlate\\dmsh_xlate.csproj",
                        root, L"Shader translator", (int)cmds.size() + 1, (int)cmds.size() + 1});
        if (cmds.size() == 2) cmds[0].stage_n = 2;
        start_job(L"Setting up", cmds);
    } else if (id == B_BUILD || id == B_REBUILD) {
        if (id == B_REBUILD &&
            MessageBoxW(g_hwnd, L"Unpack, lift and compile everything again? It takes about an hour.", L"Rebuild everything",
                        MB_YESNO | MB_ICONQUESTION) != IDYES)
            return;
        const auto st = stages();
        size_t from = 0;
        if (id == B_BUILD) {
            // From the first stage whose output is missing (everything later depends on it); with nothing missing,
            // the host alone, which rebuilds only what changed
            while (from < st.size() && st[from].done()) from++;
            if (from == st.size()) from = st.size() - 1;
        }
        std::vector<Cmd> cmds;
        for (size_t i = from; i < st.size(); i++)
            for (auto& c : st[i].cmds()) {
                c.stage = st[i].name;
                c.stage_i = (int)(i - from) + 1;
                c.stage_n = (int)(st.size() - from);
                cmds.push_back(c);
            }
        start_job(id == B_BUILD ? L"Building" : L"Rebuilding everything", cmds);
    } else if (id == B_PLAY || id == B_BAKE) {
        const std::wstring exe = (g_root / "host" / "out" / exe_name()).wstring();
        start_job(id == B_PLAY ? L"Playing" : L"Baking textures",
                  {{L"\"" + exe + L"\" \"" + g_game + L"\"" + (id == B_BAKE ? L" --bake-textures" : L""), (g_root / "host" / "out").wstring()}});
    } else if (id == B_STOP) {
        stop_job();
    }
    redraw();
}

// ---------------------------------------------------------------------------------------------------------------
// Window

static D2D1_POINT_2F to_dip(LPARAM lp) {
    return {GET_X_LPARAM(lp) * 96.f / g_dpi, GET_Y_LPARAM(lp) * 96.f / g_dpi};
}

static int hit_test(D2D1_POINT_2F p) {
    for (const auto& h : g_hits)
        if (p.x >= h.r.left && p.x < h.r.right && p.y >= h.r.top && p.y < h.r.bottom) return h.id;
    return B_NONE;
}

static LRESULT CALLBACK wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        g_dpi = (float)GetDpiForWindow(hwnd);
        const BOOL dark = TRUE;
        DwmSetWindowAttribute(hwnd, 20 /*DWMWA_USE_IMMERSIVE_DARK_MODE*/, &dark, sizeof dark);
        [[fallthrough]];
    }
    case WM_SETTINGCHANGE: {
        BOOL anim = TRUE;
        SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &anim, 0);
        g_motion = anim != FALSE;
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(hwnd, &ps);
        paint();
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_SIZE:
        if (g_rt) g_rt->Resize(D2D1::SizeU(LOWORD(lp), HIWORD(lp)));
        redraw();
        return 0;
    case WM_DPICHANGED: {
        g_dpi = (float)HIWORD(wp);
        if (g_rt) g_rt->SetDpi(g_dpi, g_dpi);
        const RECT* r = (const RECT*)lp;
        SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }
    case WM_GETMINMAXINFO: {
        auto* mm = (MINMAXINFO*)lp;
        mm->ptMinTrackSize = {(LONG)(760 * g_dpi / 96), (LONG)(660 * g_dpi / 96)};
        return 0;
    }
    case WM_MOUSEMOVE: {
        const int h = hit_test(to_dip(lp));
        if (h != g_hover) {
            g_hover = h;
            redraw();
        }
        TRACKMOUSEEVENT t{sizeof t, TME_LEAVE, hwnd, 0};
        TrackMouseEvent(&t);
        return 0;
    }
    case WM_MOUSELEAVE:
        if (g_hover != B_NONE) g_hover = B_NONE, redraw();
        return 0;
    case WM_LBUTTONDOWN:
        g_pressed = hit_test(to_dip(lp));
        SetCapture(hwnd);
        redraw();
        return 0;
    case WM_LBUTTONUP: {
        const int h = hit_test(to_dip(lp)), p = g_pressed;
        g_pressed = B_NONE;
        ReleaseCapture();
        if (h != B_NONE && h == p) click(h);
        redraw();
        return 0;
    }
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT) {
            SetCursor(LoadCursorW(nullptr, g_hover != B_NONE ? IDC_HAND : IDC_ARROW));
            return TRUE;
        }
        break;
    case WM_MOUSEWHEEL: {
        POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        ScreenToClient(hwnd, &pt);
        const float x = pt.x * 96.f / g_dpi, y = pt.y * 96.f / g_dpi;
        if (x >= g_log_rect.left && x < g_log_rect.right && y >= g_log_rect.top && y < g_log_rect.bottom) {
            g_log_scroll = std::max(0, g_log_scroll + GET_WHEEL_DELTA_WPARAM(wp) / WHEEL_DELTA * 3);
            redraw();
        }
        return 0;
    }
    case WM_TIMER:
        redraw();
        return 0;
    case WM_APP_LOG: {
        g_log_posted = false;
        size_t added;
        {
            std::lock_guard lk(g_mx);
            added = g_log_added;
            g_log_added = 0;
        }
        if (g_log_scroll > 0) g_log_scroll += (int)added;   // scrolled back: the view stays where it is
        redraw();
        return 0;
    }
    case WM_APP_DONE:
        job_done((DWORD)wp, lp != 0);
        return 0;
    case WM_CLOSE:
        if (g_running && MessageBoxW(hwnd, (g_job_name + L" is still running. Stop it and quit?").c_str(), L"Launcher",
                                     MB_YESNO | MB_ICONQUESTION) != IDYES)
            return 0;
        stop_job();   // the job objects close with this process and take what runs in them along
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void create_formats() {
    // Instrument Sans at 400 for text and 500 for headings, buttons and labels; DM Mono for output and build ids
    load_fonts();
    const bool own_sans = has_family(L"Instrument Sans"), own_mono = has_family(L"DM Mono");
    const wchar_t* sans = own_sans ? L"Instrument Sans" : L"Segoe UI";
    const wchar_t* mono = own_mono ? L"DM Mono" : L"Consolas";
    const DWRITE_FONT_WEIGHT head = own_sans ? DWRITE_FONT_WEIGHT_MEDIUM : DWRITE_FONT_WEIGHT_SEMI_BOLD;
    struct F {
        const wchar_t* family;
        DWRITE_FONT_WEIGHT weight;
        float size, track;
    } fs_[F_COUNT] = {
        {sans, head, 26, own_sans ? -0.7f : 0}, {sans, DWRITE_FONT_WEIGHT_NORMAL, 14, 0},
        {sans, DWRITE_FONT_WEIGHT_NORMAL, 12.5f, 0}, {sans, head, 12.5f, 0},
        {mono, DWRITE_FONT_WEIGHT_NORMAL, 12.5f, 0}, {sans, head, 15, 0},
        {sans, head, 14, 0},
    };
    for (int i = 0; i < F_COUNT; i++) {
        const bool own = fs_[i].family == sans ? own_sans : own_mono;
        g_dw->CreateTextFormat(fs_[i].family, own ? g_fonts : nullptr, fs_[i].weight, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                               fs_[i].size, L"en-us", &g_fmt[i]);
        g_track[i] = fs_[i].track;
        g_dw->CreateEllipsisTrimmingSign(g_fmt[i], &g_ellipsis[i]);
    }
    const D2D1_STROKE_STYLE_PROPERTIES sp{D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_LINE_JOIN_ROUND};
    g_d2d->CreateStrokeStyle(sp, nullptr, 0, &g_round);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int show) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    // The repository: the folder above holding tools\paths.py (the exe is launcher\out\launcher.exe)
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    for (fs::path d = fs::path(exe).parent_path();; d = d.parent_path()) {
        if (path_exists(d / "tools" / "paths.py")) {
            g_root = d;
            break;
        }
        if (d == d.parent_path()) {
            MessageBoxW(nullptr, L"Run the launcher from inside the repository (launcher\\out\\launcher.exe).", L"Launcher", MB_ICONERROR);
            return 1;
        }
    }

    // For the tools it runs: Python output as it comes, in UTF-8; builds on about half the cores unless BUILD_MP says
    SetEnvironmentVariableW(L"PYTHONUNBUFFERED", L"1");
    SetEnvironmentVariableW(L"PYTHONIOENCODING", L"utf-8");
    SetEnvironmentVariableW(L"PYTHONUTF8", L"1");      // files in UTF-8 too, whatever the system code page (cp950: issue #7)
    SetEnvironmentVariableW(L"KART_PROGRESS", L"1");   // the tools' @@progress lines, drawn as a bar
    if (env_var(L"BUILD_MP").empty()) {
        std::wstring mp = local_cfg()[L"BUILD_MP"];
        if (mp.empty()) mp = std::to_wstring(std::max(1u, std::thread::hardware_concurrency() / 2));
        SetEnvironmentVariableW(L"BUILD_MP", mp.c_str());
    }

    // The game version: KART_VER in the environment, else the one chosen last time
    {
        std::wstring want = env_var(L"KART_VER");
        if (want.empty()) want = local_cfg()[L"LAUNCHER_VERSION"];
        for (int v = 0; v < VERSION_COUNT; v++)
            if (want == VERSIONS[v].name) g_ver = v;
        apply_version();
    }

    check_tools();
    refresh();
    g_step = 3;
    for (int i = 0; i < 4; i++)
        if (!g_done[i]) {
            g_step = i;
            break;
        }

    D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &g_d2d);
    DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), (IUnknown**)&g_dw);
    create_formats();

    WNDCLASSEXW wc{sizeof wc};
    wc.lpfnWndProc = wndproc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wc.lpszClassName = L"KartRecompLauncher";
    RegisterClassExW(&wc);

    // 900 x 780, or less on a small screen
    const float scale = GetDpiForSystem() / 96.f;
    RECT work;
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    const int w = std::min((int)(900 * scale), (int)((work.right - work.left) * 0.95f));
    const int h = std::min((int)(780 * scale), (int)((work.bottom - work.top) * 0.95f));
    g_hwnd = CreateWindowExW(0, wc.lpszClassName, L"kart-recomp", WS_OVERLAPPEDWINDOW, work.left + (work.right - work.left - w) / 2,
                             work.top + (work.bottom - work.top - h) / 2, w, h, nullptr, nullptr, inst, nullptr);
    ShowWindow(g_hwnd, show);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}
