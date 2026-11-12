// The launcher: the way from a game dump to a running kart.exe, one step per dot at the top.
//
//   1 Game          the dump folder (exefs\ and romfs\), its module ids checked, saved as KART_GAME in local.cfg
//   2 Dependencies  tools\setup_deps.py: Eglantine and plumber-racer-2 at their pinned commits, patched and built
//   3 Build         nso.py, scan.py + funcs.py, build_all.py, gen_glue.py, the shader tools and host\build.bat, from
//                   the first stage whose output is missing
//   4 Play          host\out\kart.exe on the dump
//
// The game version (buttons at the top right of every step) picks what each step reads and writes: 1.0.0 builds into
// the repository itself (kart.exe, KART_GAME), another version into ver\<version>\ with KART_VER=<version> set for the
// tools (kart-<version>.exe, KART_GAME_<version>). The choice is kept as LAUNCHER_VERSION in local.cfg, never as
// KART_VER, which there would switch every build.
//
// Settings are read as tools\paths.py reads them: the environment, else local.cfg, else the deps\ defaults.
// launcher\build.bat builds it into launcher\out\launcher.exe; Direct2D and DirectWrite draw everything.

#ifndef UNICODE
#define UNICODE
#endif
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <windowsx.h>
#include <d2d1.h>
#include <dwrite.h>
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
    if (key == L"EGL") return (g_root / "deps" / "Eglantine").wstring();
    if (key == L"EGL_BUILD") return (g_root / "deps" / "egl-build").wstring();
    if (key == L"NVN_REF") return (g_root / "deps" / "plumber-racer-2").wstring();
    if (key == L"NVN_XLATE")
        return (fs::path(setting(L"NVN_REF")) / "dm" / "gfx" / "tools" / "dmsh_xlate" / "bin" / "Release" / "net10.0").wstring();
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
};

constexpr UINT WM_APP_LOG = WM_APP + 1, WM_APP_DONE = WM_APP + 2;

static HWND g_hwnd;
static std::mutex g_mx;
static std::deque<std::wstring> g_log;   // under g_mx
static size_t g_log_added = 0;           // under g_mx: lines since the UI last looked
static HANDLE g_job = nullptr;           // under g_mx: the running command's job object
static std::atomic<bool> g_running{false}, g_cancel{false}, g_log_posted{false};

static void log_line(std::wstring s) {
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
};

struct Tools {
    std::wstring python;   // version, '' when there is no working python
    bool zstd = false, git = false, dotnet = false;
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
    g_tools.git = on_path(L"git.exe");
    if (on_path(L"dotnet.exe")) {
        const std::string sdks = capture(L"dotnet --list-sdks");
        g_tools.dotnet = sdks.rfind("10.", 0) == 0 || sdks.find("\n10.") != std::string::npos;
    }
    DWORD code;
    const std::string out = capture(L"python -c \"import sys,importlib.util as u;print(sys.version.split()[0],"
                                    L"int(sys.version_info>=(3,14) or bool(u.find_spec('backports') and u.find_spec('backports.zstd'))))\"",
                                    &code);
    if (code == 0) {
        const std::wstring w = trim(widen(out));
        const size_t sp = w.find(L' ');
        g_tools.python = w.substr(0, sp);
        g_tools.zstd = sp != std::wstring::npos && w.substr(sp + 1, 1) == L"1";
    }
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
        // per-vertex variants and the shaders the NVIDIA driver miscompiles as they are
        {L"Translate the shaders", [=] { return path_exists(sh / "refl.tsv") && any_file(sh, L"*.fix.spv"); },
         [=] {
             return std::vector<Cmd>{{L"python tools\\nvn_shaders.py", root}, {L"python tools\\nvn_refl.py", root},
                                     {L"python tools\\nvn_mesh.py --flat", root}, {L"python tools\\nvn_spvfix.py --all", root}};
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
            else if (id != m.id) r = {m.name, L"another version: " + widen(id.substr(0, 16)), ST_BAD};
            else r = {m.name, widen(id.substr(0, 16)) + L"  matches", ST_OK};
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
    deps.push_back({L"Visual Studio", vs ? L"C++ tools found" : L"install \"Desktop development with C++\"", vs ? ST_OK : ST_BAD});
    deps.push_back({L"Git", g_tools.git ? L"found" : L"not on PATH", g_tools.git ? ST_OK : ST_BAD});
    deps.push_back({L"Python", g_tools.python.empty() ? L"not on PATH" : g_tools.python, g_tools.python.empty() ? ST_BAD : ST_OK});
    deps.push_back({L"backports.zstd", g_tools.zstd ? L"found" : L"pip install backports.zstd", g_tools.zstd ? ST_OK : ST_BAD});
    deps.push_back({L".NET 10 SDK", g_tools.dotnet ? L"found" : L"not found", g_tools.dotnet ? ST_OK : ST_BAD});
    const bool egl_src = path_exists(fs::path(setting(L"EGL")) / "src" / "video_core");
    const bool egl_lib = path_exists(fs::path(setting(L"EGL_BUILD")) / "src" / "video_core" / "video_core.lib");
    const bool ref = path_exists(fs::path(setting(L"NVN_REF")) / "dm" / "gfx" / "tools" / "dmsh_xlate");
    const bool xlate = path_exists(fs::path(setting(L"NVN_XLATE")) / "dmsh_xlate.exe");
    deps.push_back({L"Eglantine", egl_src ? L"cloned and patched" : L"not fetched", egl_src ? ST_OK : ST_MISSING});
    deps.push_back({L"Eglantine libraries", egl_lib ? L"built" : L"not built", egl_lib ? ST_OK : ST_MISSING});
    deps.push_back({L"plumber-racer-2", ref ? L"cloned and patched" : L"not fetched", ref ? ST_OK : ST_MISSING});
    deps.push_back({L"Shader translator", xlate ? L"built" : L"not built", xlate ? ST_OK : ST_MISSING});
    g_done[1] = egl_src && egl_lib && ref && xlate;
    g_can_setup = vs && g_tools.git && !g_tools.python.empty() && g_tools.dotnet;

    auto& build = g_rows[2];
    build.clear();
    bool all = true;
    for (const auto& s : stages()) {
        const bool d = s.done();
        build.push_back({s.name, d ? L"done" : L"to do", d ? ST_OK : ST_MISSING});
        all &= d;
    }
    g_done[2] = all;
    g_can_build = g_done[0] && g_done[1] && !g_tools.python.empty() && g_tools.zstd;

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

namespace col {
constexpr uint32_t bg = 0x16181C, panel = 0x22252B, raised = 0x1A1C20, line = 0x353A42, dot = 0x353A42, dot_hover = 0x434953;
constexpr uint32_t text = 0xE8EAED, muted = 0x8D949E, dim = 0x5C636D, blue = 0x3B82F6, blue_hover = 0x5A97F8, blue_down = 0x2F6ED6;
constexpr uint32_t blue_text = 0x7FB0FA, red = 0xEF5350, btn = 0x30343C, btn_hover = 0x3A3F48, btn_down = 0x2A2D34, btn_off = 0x272A30;
constexpr uint32_t white = 0xFFFFFF, log = 0xB9BFC7;
}

static ID2D1Factory* g_d2d;
static IDWriteFactory* g_dw;
static ID2D1HwndRenderTarget* g_rt;
static ID2D1SolidColorBrush* g_brush;
static ID2D1StrokeStyle* g_round;
static float g_dpi = 96;

enum { F_TITLE, F_BODY, F_SMALL, F_LABEL, F_MONO, F_DOT, F_BUTTON, F_COUNT };
static IDWriteTextFormat* g_fmt[F_COUNT];
static IDWriteInlineObject* g_ellipsis[F_COUNT];

enum { T_LEFT = 0, T_CENTER = 1, T_RIGHT = 2, T_VCENTER = 4, T_WRAP = 8 };

static ID2D1SolidColorBrush* brush(uint32_t rgb) {
    g_brush->SetColor(D2D1::ColorF(rgb));
    return g_brush;
}

static void set_format(int f, int flags) {
    IDWriteTextFormat* fm = g_fmt[f];
    fm->SetTextAlignment(flags & T_CENTER ? DWRITE_TEXT_ALIGNMENT_CENTER : flags & T_RIGHT ? DWRITE_TEXT_ALIGNMENT_TRAILING : DWRITE_TEXT_ALIGNMENT_LEADING);
    fm->SetParagraphAlignment(flags & T_VCENTER ? DWRITE_PARAGRAPH_ALIGNMENT_CENTER : DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    fm->SetWordWrapping(flags & T_WRAP ? DWRITE_WORD_WRAPPING_WRAP : DWRITE_WORD_WRAPPING_NO_WRAP);
    DWRITE_TRIMMING tr{flags & T_WRAP ? DWRITE_TRIMMING_GRANULARITY_NONE : DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
    fm->SetTrimming(&tr, flags & T_WRAP ? nullptr : g_ellipsis[f]);
}

static void text(const std::wstring& s, int f, D2D1_RECT_F r, uint32_t color, int flags = 0) {
    if (s.empty() || r.right <= r.left) return;
    set_format(f, flags);
    g_rt->DrawText(s.c_str(), (UINT32)s.size(), g_fmt[f], r, brush(color), D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

static D2D1_SIZE_F measure(const std::wstring& s, int f, float maxw, int flags = 0) {
    set_format(f, flags);
    IDWriteTextLayout* tl = nullptr;
    D2D1_SIZE_F sz{0, 0};
    if (SUCCEEDED(g_dw->CreateTextLayout(s.c_str(), (UINT32)s.size(), g_fmt[f], maxw, 10000, &tl))) {
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

static void fill_circle(float x, float y, float r, uint32_t color) {
    g_rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(x, y), r, r), brush(color));
}

// ---------------------------------------------------------------------------------------------------------------
// UI state

enum {
    B_NONE = -1, B_BROWSE = 1, B_CONTINUE, B_SETUP, B_BUILD, B_REBUILD, B_PLAY, B_BAKE, B_STOP, B_VER = 50, B_DOT = 100,
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

static const wchar_t* STEP_NAMES[4] = {L"Game", L"Dependencies", L"Build", L"Play"};
static const wchar_t* STEP_TITLES[4] = {L"Your game dump", L"Dependencies", L"Build", L"Play"};
static const wchar_t* STEP_TEXT[4] = {
    L"Pick the folder of your own dump of the game, the one holding exefs\\ and romfs\\. Versions 1.0.0 and 1.8.0 "
    L"work, each built and played on its own (the version buttons above); nothing of the dump is copied anywhere.",
    L"Eglantine and plumber-racer-2 are cloned at their pinned commits into deps\\, this repository's patches applied, "
    L"and built: Eglantine's GPU and audio libraries and the shader translator. An hour or more, mostly Eglantine.",
    L"The game's modules are unpacked, lifted to C++ and compiled, its shaders translated to SPIR-V, and kart.exe "
    L"linked. The first build takes half an hour or more; later ones redo only what is missing.",
    L"Starts kart.exe on your dump. Pipelines are made ahead of time over the first runs; baking decodes every "
    L"texture into the cache ahead of time, so the first races do not hitch.",
};

static void redraw() {
    InvalidateRect(g_hwnd, nullptr, FALSE);
}

static float button(int id, const std::wstring& label, float x, float y, bool primary, bool enabled) {
    const float w = std::max(120.f, measure(label, F_BUTTON, 1000).width + 44);
    const D2D1_RECT_F r{x, y, x + w, y + 40};
    const bool hov = g_hover == id, down = hov && g_pressed == id;
    uint32_t bg, fg = primary ? col::white : col::text;
    if (!enabled) bg = col::btn_off, fg = col::dim;
    else if (primary) bg = down ? col::blue_down : hov ? col::blue_hover : col::blue;
    else bg = down ? col::btn_down : hov ? col::btn_hover : col::btn;
    fill_round(r, 8, bg);
    text(label, F_BUTTON, r, fg, T_CENTER | T_VCENTER);
    if (enabled) g_hits.push_back({id, r});
    return x + w + 12;
}

static float draw_rows(const std::vector<Row>& rows, int cols, float x0, float x1, float y) {
    const int per = (int)((rows.size() + cols - 1) / cols);
    const float gap = 32, colw = (x1 - x0 - gap * (cols - 1)) / cols, rh = 28;
    const float namew = std::min(170.f, colw * 0.46f);
    for (size_t i = 0; i < rows.size(); i++) {
        const auto& r = rows[i];
        const float x = x0 + (i / per) * (colw + gap), yy = y + (i % per) * rh;
        const uint32_t dotc = r.st == ST_OK ? col::blue : r.st == ST_BAD ? col::red : col::line;
        fill_circle(x + 5, yy + rh / 2, 4.5f, dotc);
        text(r.name, F_BODY, {x + 20, yy, x + 20 + namew, yy + rh}, r.st == ST_OPTIONAL ? col::muted : col::text, T_VCENTER);
        text(r.detail, F_SMALL, {x + 20 + namew + 8, yy, x + colw, yy + rh},
             r.st == ST_BAD ? col::red : col::muted, T_VCENTER);
    }
    return y + per * rh;
}

static void draw_status(float x, float x1, float y) {
    if (g_running) {
        fill_circle(x + 5, y + 20, 4.5f, col::blue);
        text(g_job_name + L"   " + clock_text(GetTickCount64() - g_job_start), F_BODY, {x + 18, y, x1, y + 40}, col::text, T_VCENTER);
    } else {
        text(g_status, F_BODY, {x, y, x1, y + 40}, g_status_color, T_VCENTER);
    }
}

static void draw_log(D2D1_RECT_F r) {
    g_log_rect = r;
    fill_round(r, 10, col::raised);
    const float pad = 14, lh = 18;
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
        if (l.rfind(L"> ", 0) == 0) c = col::blue_text;
        else if (lo.find(L"error") != std::wstring::npos || lo.find(L"failed") != std::wstring::npos) c = col::red;
        text(l, F_MONO, {in.left, in.top + k * lh, in.right, in.top + (k + 1) * lh}, c);
    }
    g_rt->PopAxisAlignedClip();
}

static void draw_check(float x, float y) {
    auto* b = brush(col::white);
    g_rt->DrawLine({x - 7, y + 0.5f}, {x - 2.5f, y + 5}, b, 2.6f, g_round);
    g_rt->DrawLine({x - 2.5f, y + 5}, {x + 7.5f, y - 5.5f}, b, 2.6f, g_round);
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

// The game version: one button each, right-aligned in the title row; returns where they start
static float draw_versions(float x1, float y, bool enabled) {
    float x = x1;
    for (int i = VERSION_COUNT - 1; i >= 0; i--) {
        const std::wstring label = VERSIONS[i].name;
        const float w = measure(label, F_SMALL, 200).width + 28;
        x -= w;
        const D2D1_RECT_F r{x, y + 4, x + w, y + 30};
        const bool sel = i == g_ver, hov = !sel && enabled && g_hover == B_VER + i;
        fill_round(r, 13, sel ? col::blue : hov ? col::btn_hover : col::btn);
        text(label, F_SMALL, r, sel ? col::white : enabled ? col::text : col::dim, T_CENTER | T_VCENTER);
        if (enabled && !sel) g_hits.push_back({B_VER + i, r});
        x -= 8;
    }
    const float lw = measure(L"Game version", F_SMALL, 200).width;
    text(L"Game version", F_SMALL, {x - lw - 4, y + 4, x, y + 30}, col::muted, T_RIGHT | T_VCENTER);
    return x - lw - 16;
}

static void paint_panel(D2D1_RECT_F p) {
    fill_round(p, 22, col::panel);
    const float x0 = p.left + 36, x1 = p.right - 36;
    float y = p.top + 30;
    const float tx1 = draw_versions(x1, y, !g_running);
    text(STEP_TITLES[g_step], F_TITLE, {x0, y, tx1, y + 34}, col::text);
    y += 40;
    const float dh = measure(STEP_TEXT[g_step], F_BODY, x1 - x0, T_WRAP).height;
    text(STEP_TEXT[g_step], F_BODY, {x0, y, x1, y + dh + 4}, col::muted, T_WRAP);
    y += dh + 24;

    const bool busy = g_running;
    switch (g_step) {
    case 0: {
        text(L"Game folder", F_SMALL, {x0, y, x1, y + 18}, col::muted);
        y += 24;
        const D2D1_RECT_F box{x0, y, x1 - 136, y + 40};
        fill_round(box, 8, col::raised);
        text(g_game.empty() ? L"Not chosen yet" : g_game, F_BODY, {box.left + 14, box.top, box.right - 14, box.bottom},
             g_game.empty() ? col::dim : col::text, T_VCENTER);
        button(B_BROWSE, L"Browse\u2026", x1 - 124, y, g_game.empty(), !busy);
        y += 60;
        y = draw_rows(g_rows[0], 1, x0, x1, y);
        if (!env_var(game_key().c_str()).empty()) {
            text(game_key() + L" is set in the environment: it is used instead of local.cfg.", F_SMALL, {x0, y + 8, x1, y + 28}, col::muted);
            y += 28;
        }
        if (g_done[0]) button(B_CONTINUE, L"Continue", x0, y + 18, true, true);
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
        y += 56;
        draw_log({x0, y, x1, p.bottom - 30});
        break;
    }
    case 2: {
        y = draw_rows(g_rows[2], 2, x0, x1, y);
        const std::wstring mp = env_var(L"BUILD_MP");
        text(L"Up to " + mp + L" compiler processes at once (BUILD_MP in local.cfg or the environment)", F_SMALL,
             {x0, y + 6, x1, y + 26}, col::dim);
        y += 40;
        float x = x0;
        if (busy) x = button(B_STOP, L"Stop", x, y, false, true);
        else {
            x = button(B_BUILD, L"Build", x, y, !g_done[2], g_can_build);
            x = button(B_REBUILD, L"Rebuild everything", x, y, false, g_can_build);
            if (g_done[2]) x = button(B_CONTINUE, L"Continue", x, y, true, true);
        }
        if (!busy && !g_can_build && g_status.empty()) {
            const wchar_t* why = !g_done[0] ? L"Choose the game dump first." : !g_done[1] ? L"Set up the dependencies first."
                                                                                           : L"Python with backports.zstd is needed.";
            text(why, F_BODY, {x + 4, y, x1, y + 40}, col::muted, T_VCENTER);
        } else draw_status(x + 4, x1, y);
        y += 56;
        draw_log({x0, y, x1, p.bottom - 30});
        break;
    }
    case 3: {
        y = draw_rows(g_rows[3], 2, x0, x1, y);
        std::wstring settings;
        for (const auto& [k, v] : game_env()) settings += (settings.empty() ? L"" : L"   ") + k + L"=" + v;
        text(L"Settings: " + (settings.empty() ? std::wstring(L"the defaults") : settings) + L"   (HOST_* lines in local.cfg)", F_SMALL,
             {x0, y + 6, x1, y + 26}, col::dim);
        y += 40;
        float x = x0;
        if (busy) x = button(B_STOP, L"Stop", x, y, false, true);
        else {
            x = button(B_PLAY, L"Play", x, y, true, g_done[3]);
            x = button(B_BAKE, L"Bake textures", x, y, false, g_done[3]);
        }
        draw_status(x + 4, x1, y);
        y += 56;
        draw_log({x0, y, x1, p.bottom - 30});
        break;
    }
    }
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
    g_rt->BeginDraw();
    g_rt->Clear(D2D1::ColorF(col::bg));
    g_hits.clear();
    g_log_rect = {};
    const D2D1_SIZE_F sz = g_rt->GetSize();
    const float m = 32, r = 20, cy = 58;

    // The steps: dots on a line, the finished ones blue
    float cx[4];
    for (int i = 0; i < 4; i++) cx[i] = m + 46 + (sz.width - 2 * m - 92) * i / 3.f;
    for (int i = 0; i < 3; i++)
        g_rt->FillRectangle({cx[i], cy - 1.5f, cx[i + 1], cy + 1.5f}, brush(g_done[i] && g_done[i + 1] ? col::blue : col::line));
    for (int i = 0; i < 4; i++) {
        const bool sel = i == g_step, hov = g_hover == B_DOT + i;
        if (sel) {
            fill_circle(cx[i], cy, r + 9, col::bg);
            g_rt->DrawEllipse(D2D1::Ellipse({cx[i], cy}, r + 5, r + 5), brush(col::blue), 2);
        }
        fill_circle(cx[i], cy, r, g_done[i] || sel ? (hov ? col::blue_hover : col::blue) : (hov ? col::dot_hover : col::dot));
        if (g_done[i] && i == 3) draw_play_icon(cx[i] + 1, cy, col::white);
        else if (g_done[i]) draw_check(cx[i], cy);
        else text(std::to_wstring(i + 1), F_DOT, {cx[i] - r, cy - r, cx[i] + r, cy + r}, sel ? col::white : col::muted, T_CENTER | T_VCENTER);
        text(STEP_NAMES[i], F_LABEL, {cx[i] - 70, cy + r + 10, cx[i] + 70, cy + r + 30}, sel ? col::text : col::muted, T_CENTER);
        g_hits.push_back({B_DOT + i, {cx[i] - r - 8, cy - r - 8, cx[i] + r + 8, cy + r + 30}});
    }

    paint_panel({m, cy + r + 46, sz.width - m, sz.height - m});

    if (g_rt->EndDraw() == D2DERR_RECREATE_TARGET) {
        g_brush->Release();
        g_rt->Release();
        g_brush = nullptr;
        g_rt = nullptr;
        redraw();
    }
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
    SetTimer(g_hwnd, 1, 500, nullptr);
    std::thread([cmds = std::move(cmds)] {
        DWORD code = 0;
        for (const auto& c : cmds) {
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
    KillTimer(g_hwnd, 1);
    const std::wstring took = clock_text(GetTickCount64() - g_job_start);
    if (cancelled) {
        g_status = L"Stopped after " + took;
        g_status_color = col::muted;
    } else if (code == 0) {
        g_status = L"Finished in " + took;
        g_status_color = col::blue_text;
    } else {
        wchar_t c[32];
        swprintf(c, 32, code > 0xFFFF ? L"0x%08X" : L"%u", code);
        g_status = L"Failed (exit code " + std::wstring(c) + L") after " + took;
        g_status_color = col::red;
    }
    const int step = g_step;
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
    refresh();
    redraw();
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
    } else if (id == B_CONTINUE) {
        g_step = std::min(3, g_step + 1);
        g_status.clear();
        refresh();
    } else if (id == B_SETUP) {
        start_job(L"Setting up the dependencies", {{L"python tools\\setup_deps.py", root}});
    } else if (id == B_BUILD || id == B_REBUILD) {
        if (id == B_REBUILD &&
            MessageBoxW(g_hwnd, L"Unpack, lift and compile everything again? It takes half an hour or more.", L"Rebuild everything",
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
            for (auto& c : st[i].cmds()) cmds.push_back(c);
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
    struct F {
        const wchar_t* family;
        DWRITE_FONT_WEIGHT weight;
        float size;
    } fs_[F_COUNT] = {
        {L"Segoe UI", DWRITE_FONT_WEIGHT_SEMI_BOLD, 24}, {L"Segoe UI", DWRITE_FONT_WEIGHT_NORMAL, 14},
        {L"Segoe UI", DWRITE_FONT_WEIGHT_NORMAL, 12.5f}, {L"Segoe UI", DWRITE_FONT_WEIGHT_SEMI_BOLD, 12.5f},
        {L"Consolas", DWRITE_FONT_WEIGHT_NORMAL, 12.5f}, {L"Segoe UI", DWRITE_FONT_WEIGHT_SEMI_BOLD, 15},
        {L"Segoe UI", DWRITE_FONT_WEIGHT_SEMI_BOLD, 14},
    };
    for (int i = 0; i < F_COUNT; i++) {
        g_dw->CreateTextFormat(fs_[i].family, nullptr, fs_[i].weight, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, fs_[i].size,
                               L"en-us", &g_fmt[i]);
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
