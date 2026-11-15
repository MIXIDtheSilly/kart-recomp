// The launcher: the way from a game dump to a running kart.exe, one step per dot at the top.
//
//   1 Game          the dump folder (exefs\ and romfs\), its module ids checked, saved as KART_GAME in local.cfg
//   2 Dependencies  the tools the build needs; Set up installs Python and the .NET SDK into tools\ when the machine
//                   has none (tools\bootstrap.ps1), downloads the C++ compiler (llvm-mingw,
//                   tools\toolchain.py: no Visual Studio needed), installs the Python packages (tools\deps.py) and builds
//                   the shader translator from tools\xlate and its packages\ (the SPIR-V tools and everything else
//                   the host uses are in third_party\). Its C++ compiler switch writes BUILD_CC to local.cfg: mingw, or
//                   cl for Visual Studio (clang, clang-cl with Visual Studio, counts as Visual Studio there).
//   3 Build         nso.py, scan.py + funcs.py, build_all.py, gen_glue.py, the shader tools and host\build.bat: the
//                   stages whose output is missing or whose tools changed since they last ran (Stage::stamp), and
//                   the ones after them; the tools' @@progress lines are drawn as a bar. A new release unzipped beside
//                   an older one takes that one's build over on the Game step (bring_over), so only what changed builds
//   4 Play          host\out\kart.exe on the dump
//
// The game version (buttons at the top right of every step) picks what each step reads and writes: 1.0.0 builds into
// the repository itself (kart.exe, KART_GAME), another version into ver\<version>\ with KART_VER=<version> set for the
// tools (kart-<version>.exe, KART_GAME_<version>). The choice is kept as LAUNCHER_VERSION in local.cfg, never as
// KART_VER, which there would switch every build.
//
// A release (launcher\out\release.txt beside the prebuilt launcher) asks GitHub for a newer one at startup and offers
// to update: tools\update.py puts it over this folder and the launcher starts again (see Updates below).
//
// Settings are read as tools\paths.py reads them: the environment, else local.cfg, else the defaults.
// launcher\build.bat builds it into launcher\out\launcher.exe; Direct2D and DirectWrite draw everything, in Instrument
// Sans and DM Mono from launcher\fonts\ (SIL OFL, the licences beside them; Segoe UI and Consolas when they are missing).
//
// Linux: the same launcher, built by ./kart.sh (tools/linux.py launcher) into launcher/out/launcher. An SDL 3 window
// (the library the game uses too), drawn with cairo and laid out with Pango (the fonts above, through fontconfig); the
// questions are drawn in the window, the file dialogs are the desktop's (SDL's, through its portal). Settings are written
// to local.linux.cfg, which tools/paths.py reads before local.cfg (whose Windows paths Linux skips); the steps run what
// ./kart.sh runs: Set up is tools/linux.py setup <part>, the build the same tools with build_host.py linking host/out/kart.
// The commands run through /bin/sh in a process group of their own, so Stop ends the whole tree.

#ifdef _WIN32
#ifndef UNICODE
#define UNICODE
#endif
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <regex>
#include <windows.h>
#include <windowsx.h>
#include <d2d1.h>
#include <dwrite_3.h>
#include <dwmapi.h>
#include <shobjidl.h>
#include <shellapi.h>
#else
#include <regex>
#include <SDL3/SDL.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <poll.h>
#include <signal.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <cmath>
#include <cwchar>
#include <cwctype>
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
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

#ifdef _WIN32
#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "shell32.lib")
#define DS L"\\"             // the folder separator, in what the launcher shows and the commands it runs
#define CFG_NAME L"local.cfg"  // where the launcher writes its settings
#else
// The few Windows names the shared code uses, and Direct2D's geometry types (the drawing below is cairo's)
using DWORD = uint32_t;
using UINT = unsigned;
using ULONGLONG = unsigned long long;
struct D2D1_RECT_F {
    float left, top, right, bottom;
};
struct D2D1_POINT_2F {
    float x, y;
};
struct D2D1_SIZE_F {
    float width, height;
};
static ULONGLONG GetTickCount64() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (ULONGLONG)ts.tv_sec * 1000 + (ULONGLONG)ts.tv_nsec / 1000000;
}
#define swscanf_s swscanf
#define _wcstoui64 wcstoull
#define DS L"/"
#define CFG_NAME L"local.linux.cfg"
#endif

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------------------------------------------
// Text

#ifdef _WIN32
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
#else
// UTF-8 to UTF-32 (wchar_t is 32 bits here); a byte that is not UTF-8 stands for itself (Latin-1), and *valid says
// whether there was one
static std::wstring widen(const std::string& s, bool* valid = nullptr) {
    std::wstring w;
    w.reserve(s.size());
    bool ok = true;
    for (size_t i = 0; i < s.size();) {
        const unsigned char c = (unsigned char)s[i];
        const int n = c < 0x80 ? 0 : (c >> 5) == 6 ? 1 : (c >> 4) == 14 ? 2 : (c >> 3) == 30 ? 3 : -1;
        uint32_t cp = n == 0 ? c : n == 1 ? c & 31 : n == 2 ? c & 15 : c & 7;
        bool good = n >= 0 && i + n < s.size();
        for (int k = 1; good && k <= n; k++) {
            const unsigned char d = (unsigned char)s[i + k];
            good = (d >> 6) == 2;
            cp = cp << 6 | (d & 63);
        }
        if (!good) {
            ok = false;
            w += (wchar_t)c;
            i++;
            continue;
        }
        w += (wchar_t)cp;
        i += n + 1;
    }
    if (valid) *valid = ok;
    return w;
}

static std::string narrow(const std::wstring& w) {
    std::string s;
    s.reserve(w.size());
    for (const wchar_t wc : w) {
        const uint32_t c = (uint32_t)wc;
        if (c < 0x80) s += (char)c;
        else if (c < 0x800) s += (char)(0xC0 | c >> 6), s += (char)(0x80 | (c & 63));
        else if (c < 0x10000) s += (char)(0xE0 | c >> 12), s += (char)(0x80 | (c >> 6 & 63)), s += (char)(0x80 | (c & 63));
        else s += (char)(0xF0 | c >> 18), s += (char)(0x80 | (c >> 12 & 63)), s += (char)(0x80 | (c >> 6 & 63)), s += (char)(0x80 | (c & 63));
    }
    return s;
}
#endif

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
#ifdef _WIN32
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, clean.data(), (int)clean.size(), nullptr, 0) > 0) return widen(clean);
    return widen(clean, CP_OEMCP);
#else
    return widen(clean);
#endif
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

#ifdef _WIN32
static std::wstring env_var(const wchar_t* key) {
    std::wstring buf(32767, L'\0');
    const DWORD n = GetEnvironmentVariableW(key, buf.data(), (DWORD)buf.size());
    return n && n < buf.size() ? buf.substr(0, n) : L"";
}
#else
static std::wstring env_var(const wchar_t* key) {
    const char* v = getenv(narrow(key).c_str());
    return v ? widen(v) : L"";
}
static void SetEnvironmentVariableW(const wchar_t* key, const wchar_t* value) {
    if (value) setenv(narrow(key).c_str(), narrow(value).c_str(), 1);
    else unsetenv(narrow(key).c_str());
}
#endif

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

// Linux: local.cfg, less its Windows paths (C:\...), then local.linux.cfg over it (tools\paths.py _local)
static std::map<std::wstring, std::wstring> local_cfg() {
    std::map<std::wstring, std::wstring> cfg;
#ifdef _WIN32
    for (const wchar_t* name : {L"local.cfg"}) {
#else
    for (const wchar_t* name : {L"local.cfg", L"local.linux.cfg"}) {
#endif
        for (const auto& l : read_lines(g_root / name)) {
            std::wstring w = widen(l);
            w = w.substr(0, w.find(L'#'));
            const size_t eq = w.find(L'=');
            if (eq == std::wstring::npos) continue;
            const std::wstring v = trim(w.substr(eq + 1));
#ifndef _WIN32
            if (name == std::wstring(L"local.cfg") && v.size() >= 3 && iswalpha(v[0]) && v[1] == L':' && (v[2] == L'\\' || v[2] == L'/'))
                continue;
#endif
            cfg[trim(w.substr(0, eq))] = v;
        }
    }
    return cfg;
}

static std::wstring setting(const std::wstring& key) {
    if (auto e = env_var(key.c_str()); !e.empty()) return e;
    const auto cfg = local_cfg();
    if (auto it = cfg.find(key); it != cfg.end() && !it->second.empty()) {
        fs::path v = it->second;
        if (key != L"VCVARS" && key != L"CLANG" && key != L"BUILD_CC" && key != L"KART_VER" && v.is_relative())
            v = (g_root / v).lexically_normal();
        return v.wstring();
    }
    if (key == L"NVN_XLATE") return (g_root / "work" / "xlate").wstring();
    if (key == L"MINGW") return (g_root / "tools" / "llvm-mingw").wstring();
    if (key == L"LLVM") {   // tools/llvm, else the system's clang when there is one
        const fs::path own = g_root / "tools" / "llvm";
        return (fs::exists(own / "bin" / "clang++") || !fs::exists("/usr/bin/clang++") ? own : fs::path("/usr")).wstring();
    }
    if (key == L"VCVARS") return g_vcvars_found;
    return L"";
}

// <key>=<value> in local.cfg (Linux: local.linux.cfg), the other lines kept
static bool save_setting(const std::wstring& key, const std::wstring& value) {
    auto lines = read_lines(g_root / CFG_NAME);
#ifndef _WIN32
    if (lines.empty()) lines.push_back("# Linux settings (tools/paths.py): they come before local.cfg");
#endif
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
    std::ofstream o(g_root / CFG_NAME, std::ios::binary | std::ios::trunc);
#ifdef _WIN32
    for (const auto& l : lines) o << l << "\r\n";
#else
    for (const auto& l : lines) o << l << "\n";
#endif
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

#ifdef _WIN32
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
#else
// The files in <dir> whose names match <pattern> (* and ?)
static std::vector<fs::path> files_matching(const fs::path& dir, const wchar_t* pattern, size_t most = SIZE_MAX) {
    std::vector<fs::path> found;
    const std::string pat = narrow(pattern);
    std::error_code ec;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end && found.size() < most; it.increment(ec))
        if (!it->is_directory(ec) && fnmatch(pat.c_str(), it->path().filename().c_str(), 0) == 0) found.push_back(it->path());
    return found;
}

static bool any_file(const fs::path& dir, const wchar_t* pattern) {
    return !files_matching(dir, pattern, 1).empty();
}

// Where <exe> is on PATH; '' when it is not
static fs::path which(const wchar_t* exe) {
    const char* path = getenv("PATH");
    std::string rest = path ? path : "";
    for (size_t a = 0; a <= rest.size();) {
        size_t b = rest.find(':', a);
        if (b == std::string::npos) b = rest.size();
        const std::string d = rest.substr(a, b - a);
        if (!d.empty() && access((d + "/" + narrow(exe)).c_str(), X_OK) == 0) return fs::path(d) / narrow(exe);
        a = b + 1;
    }
    return {};
}

static bool on_path(const wchar_t* exe) { return !which(exe).empty(); }
#endif

// ---------------------------------------------------------------------------------------------------------------
// Processes

#ifdef _WIN32
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
#else
// A command line through /bin/sh, its output into <fd> (stdout and stderr), stdin empty; in a process group of its own
// when <group> (Stop and the end of the command kill the group), in <cwd> unless that is empty. Only what is safe
// between fork and exec in a program with threads.
static pid_t spawn_sh(const std::string& line, const std::string& cwd, int fd, bool group) {
    const pid_t pid = fork();
    if (pid == 0) {
        if (group) {
            setpgid(0, 0);
            prctl(PR_SET_PDEATHSIG, SIGKILL);   // the launcher gone: so is the command
        }
        const int nul = open("/dev/null", O_RDONLY);
        dup2(nul, 0);
        dup2(fd, 1);
        dup2(fd, 2);
        if (!cwd.empty() && chdir(cwd.c_str())) _exit(126);
        execl("/bin/sh", "sh", "-c", line.c_str(), (char*)nullptr);
        _exit(127);
    }
    if (pid > 0 && group) setpgid(pid, pid);
    return pid;
}

static DWORD wait_code(pid_t pid) {
    int st = 0;
    while (waitpid(pid, &st, 0) < 0)
        if (errno != EINTR) return 1;
    return WIFEXITED(st) ? (DWORD)WEXITSTATUS(st) : 128 + (DWORD)WTERMSIG(st);
}

// A short command's whole output (the tool checks at startup)
static std::string capture(const std::wstring& cmdline, DWORD* exit_code = nullptr) {
    if (exit_code) *exit_code = (DWORD)-1;
    int fd[2];
    if (pipe2(fd, O_CLOEXEC)) return {};
    const pid_t pid = spawn_sh(narrow(cmdline), "", fd[1], false);
    close(fd[1]);
    std::string out;
    if (pid > 0) {
        char buf[4096];
        for (ssize_t n; (n = read(fd[0], buf, sizeof buf)) != 0;) {
            if (n > 0) out.append(buf, (size_t)n);
            else if (errno != EINTR) break;
        }
        const DWORD code = wait_code(pid);
        if (exit_code) *exit_code = code;
    }
    close(fd[0]);
    return out;
}
#endif

struct Cmd {
    std::wstring line, cwd;
    std::wstring stage;     // the build stage it belongs to (shown while it runs), '' for none
    int stage_i = 0, stage_n = 0;
    std::function<DWORD()> fn;       // instead of a process: done on the job's thread (line is only logged)
    std::function<void()> after;     // when it succeeded (a build stage's last command: the stage's stamp)
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

#ifdef _WIN32
constexpr UINT WM_APP_LOG = WM_APP + 1, WM_APP_DONE = WM_APP + 2, WM_APP_UPDATE = WM_APP + 3;

static HWND g_hwnd;
static void post_log() { PostMessageW(g_hwnd, WM_APP_LOG, 0, 0); }
#else
// The window, and the events the job's threads post to it (SDL_PushEvent is safe from any thread)
static SDL_Window* g_window;
static Uint32 g_ev_log, g_ev_done, g_ev_update;
static void post_event(Uint32 type, int code = 0, bool flag = false) {
    SDL_Event e{};
    e.type = type;
    e.user.code = code;
    e.user.data1 = (void*)(intptr_t)flag;
    SDL_PushEvent(&e);
}
static void post_log() { post_event(g_ev_log); }
#endif
static std::mutex g_mx;
static std::deque<std::wstring> g_log;   // under g_mx
static size_t g_log_added = 0;           // under g_mx: lines since the UI last looked
#ifdef _WIN32
static HANDLE g_job = nullptr;           // under g_mx: the running command's job object
#else
static pid_t g_job = 0;                  // under g_mx: the running command's process group
#endif
static std::atomic<bool> g_running{false}, g_cancel{false}, g_log_posted{false};
static FILE* g_log_file = nullptr;       // under g_mx: --log <file>, every output line also goes there

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
        if (!g_log_posted.exchange(true)) post_log();
        return;
    }
    {
        std::lock_guard lk(g_mx);
        if (g_log_file) {
            fputs((narrow(s) + "\n").c_str(), g_log_file);
            fflush(g_log_file);
        }
        g_log.push_back(std::move(s));
        g_log_added++;
        while (g_log.size() > 5000) g_log.pop_front();
    }
    if (!g_log_posted.exchange(true)) post_log();
}

// The output of a command, line by line into the log
struct LineSplitter {
    std::string pending;
    void add(const char* p, size_t n) {
        pending.append(p, n);
        for (size_t at; (at = pending.find('\n')) != std::string::npos;) {
            log_line(decode_output(pending.substr(0, at)));
            pending.erase(0, at + 1);
        }
    }
    ~LineSplitter() {
        if (!pending.empty()) log_line(decode_output(pending));
    }
};

// One command, its output into the log; everything it starts is in a job object (Linux: a process group), so Stop
// ends the whole tree
static DWORD run_cmd(const Cmd& c) {
    log_line(L"> " + c.line);
    if (c.fn) return c.fn();
#ifndef _WIN32
    int fd[2];
    if (pipe2(fd, O_CLOEXEC)) return 1;
    const pid_t pid = spawn_sh(narrow(c.line), narrow(c.cwd), fd[1], true);
    close(fd[1]);
    if (pid < 0) {
        log_line(L"could not start it (" + widen(strerror(errno)) + L")");
        close(fd[0]);
        return 1;
    }
    {
        std::lock_guard lk(g_mx);
        g_job = pid;
    }
    if (g_cancel) kill(-pid, SIGKILL);
    std::atomic<bool> exited{false};
    std::thread reader([&] {
        LineSplitter lines;
        char buf[8192];
        for (;;) {
            pollfd p{fd[0], POLLIN, 0};
            const int r = poll(&p, 1, 200);
            if (r == 0) {
                if (exited) break;   // what it left running outside its group still holds the pipe: not waited for
                continue;
            }
            const ssize_t n = r < 0 ? -1 : read(fd[0], buf, sizeof buf);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) break;
            lines.add(buf, (size_t)n);
        }
    });
    const DWORD code = wait_code(pid);
    // What it left running in its group (build servers) would hold the pipe open: they end with it
    kill(-pid, SIGKILL);
    exited = true;
    reader.join();
    close(fd[0]);
    {
        std::lock_guard lk(g_mx);
        g_job = 0;
    }
    return code;
#else
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
#endif
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
    bool dotnet = false, vp9 = false;   // vp9: Linux: libvpx
    // Linux: what the system has to have (Set up cannot install it): the Vulkan loader, the C++ library headers, and
    // the python3 Set up makes tools/venv from (its version)
    bool vulkan = false, cxx_headers = false;
    std::wstring sys_python;
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
#ifdef _WIN32
static std::wstring exe_name() { return L"kart" + ver_suffix() + L".exe"; }
#else
static std::wstring exe_name() { return L"kart" + ver_suffix(); }
#endif
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

// The C++ compiler, as tools\paths.py picks it: BUILD_CC when it is set (mingw, cl, clang), else mingw (llvm-mingw,
// which Set up downloads into MINGW)
static bool mingw_ok() { return path_exists(fs::path(setting(L"MINGW")) / "bin" / "clang++.exe"); }
static bool build_cc_set() { return !setting(L"BUILD_CC").empty(); }
static std::wstring build_cc() {
    std::wstring cc = setting(L"BUILD_CC");
    for (auto& ch : cc) ch = (wchar_t)towlower(ch);
    if (cc.empty()) return L"mingw";
    return cc == L"clang-cl" ? L"clang" : cc;
}
// Where build_lib.py puts the module libraries for that compiler, and their extension
#ifdef _WIN32
static fs::path lib_dir(const fs::path& build) {
    const std::wstring cc = build_cc();
    return cc == L"mingw" ? build / "mingw" : cc == L"clang" ? build / "clang" : build;
}
static const wchar_t* lib_ext() { return build_cc() == L"mingw" ? L".a" : L".lib"; }
#else
static fs::path lib_dir(const fs::path& build) { return build / "linux"; }
static const wchar_t* lib_ext() { return L".a"; }
// Linux: clang from LLVM (tools\paths.py LLVM: tools/llvm, which Set up downloads, else the system's)
static bool llvm_ok() { return path_exists(fs::path(setting(L"LLVM")) / "bin" / "clang++"); }
#endif
// A tool the build runs, as a command line from the repository (Python: tools\python, Linux: tools/venv, first on PATH)
static std::wstring py(const std::wstring& tool) { return L"python tools" DS + tool; }

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
#ifdef _WIN32
static bool spirv_tools() {
    if (const auto v = setting(L"VULKAN_BIN"); !v.empty()) return path_exists(fs::path(v) / "spirv-val.exe");
    if (path_exists(g_root / "third_party" / "spirv" / "bin" / "spirv-val.exe")) return true;
    if (const auto sdk = env_var(L"VULKAN_SDK"); !sdk.empty()) return path_exists(fs::path(sdk) / "Bin" / "spirv-val.exe");
    return false;
}
#else
// Linux: tools/spirv (Set up takes them from the Vulkan SDK), else the Vulkan SDK's, else where spirv-val is on PATH;
// all five of them (distributions often lack spirv-cross)
static fs::path spirv_dir() {
    if (const auto v = setting(L"VULKAN_BIN"); !v.empty()) return v;
    if (path_exists(g_root / "tools" / "spirv" / "spirv-val")) return g_root / "tools" / "spirv";
    if (const auto sdk = env_var(L"VULKAN_SDK"); !sdk.empty()) return fs::path(sdk) / "bin";
    const fs::path found = which(L"spirv-val");
    return found.empty() ? fs::path("/usr/bin") : found.parent_path();
}
static bool spirv_tools() {
    const fs::path d = spirv_dir();
    for (const char* t : {"spirv-val", "spirv-opt", "spirv-dis", "spirv-cross", "glslangValidator"})
        if (!path_exists(d / t)) return false;
    return true;
}
#endif

// Python and the .NET SDK that Set up installs into the repository when the machine has none (tools\bootstrap.ps1):
// first on PATH for everything the launcher runs, so they take over from a missing or too old one
#ifdef _WIN32
static fs::path local_python() { return g_root / "tools" / "python"; }
static fs::path local_dotnet() { return g_root / "tools" / "dotnet"; }
#else
// Linux: Set up makes a virtual environment from the system's python3 (distributions keep pip away from it) and puts
// the .NET SDK into tools/dotnet when the system has none
static fs::path local_python() { return g_root / "tools" / "venv"; }
static fs::path local_dotnet() { return g_root / "tools" / "dotnet"; }
static void use_local_tools() {
    static bool on_path_done = false;
    if (!on_path_done) {
        on_path_done = true;
        SetEnvironmentVariableW(L"PATH", ((local_python() / "bin").wstring() + L":" + local_dotnet().wstring() + L":" + env_var(L"PATH")).c_str());
    }
    if (path_exists(local_dotnet() / "dotnet")) {
        SetEnvironmentVariableW(L"DOTNET_ROOT", local_dotnet().wstring().c_str());
        SetEnvironmentVariableW(L"DOTNET_CLI_TELEMETRY_OPTOUT", L"1");
        SetEnvironmentVariableW(L"DOTNET_NOLOGO", L"1");
    }
}
static void check_dotnet() {
    use_local_tools();
    g_tools.dotnet = false;
    if (on_path(L"dotnet")) {
        const std::string sdks = capture(L"dotnet --list-sdks");
        g_tools.dotnet = sdks.rfind("10.", 0) == 0 || sdks.find("\n10.") != std::string::npos;
    }
}

static bool have_lib(const char* name) {
    void* h = dlopen(name, RTLD_LAZY | RTLD_LOCAL);
    if (h) dlclose(h);
    return h != nullptr;
}

static void check_tools() {
    use_local_tools();
    g_tools.vulkan = have_lib("libvulkan.so.1");
    g_tools.vp9 = false;   // libvpx, which the game loads for the movies (host\services\vdec_vpx.cpp)
    for (int n = 12; n >= 6 && !g_tools.vp9; n--) g_tools.vp9 = have_lib(("libvpx.so." + std::to_string(n)).c_str());
    std::error_code ec;
    g_tools.cxx_headers = false;
    for (fs::directory_iterator it("/usr/include/c++", ec), end; !ec && it != end; it.increment(ec))
        g_tools.cxx_headers |= path_exists(it->path() / "vector");
    g_tools.sys_python = trim(widen(capture(L"python3 -c \"import sys; print(sys.version.split()[0])\"")));
    check_dotnet();
    check_python();
}
#endif
#ifdef _WIN32
static void use_local_tools() {
    static bool on_path_done = false;
    if (!on_path_done) {
        on_path_done = true;
        SetEnvironmentVariableW(L"PATH", (local_python().wstring() + L";" + (local_python() / "Scripts").wstring() + L";" +
                                          local_dotnet().wstring() + L";" + env_var(L"PATH")).c_str());
    }
    // A dotnet outside Program Files: the translator it builds finds the runtime through DOTNET_ROOT
    if (path_exists(local_dotnet() / "dotnet.exe")) {
        SetEnvironmentVariableW(L"DOTNET_ROOT", local_dotnet().c_str());
        SetEnvironmentVariableW(L"DOTNET_MULTILEVEL_LOOKUP", L"0");
        SetEnvironmentVariableW(L"DOTNET_CLI_TELEMETRY_OPTOUT", L"1");
        SetEnvironmentVariableW(L"DOTNET_NOLOGO", L"1");
    }
}
static void check_dotnet() {
    use_local_tools();
    g_tools.dotnet = false;
    if (on_path(L"dotnet.exe")) {
        const std::string sdks = capture(L"dotnet --list-sdks");
        g_tools.dotnet = sdks.rfind("10.", 0) == 0 || sdks.find("\n10.") != std::string::npos;
    }
}

static void check_tools() {
    use_local_tools();
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
    check_dotnet();
    check_python();
}
#endif

// What a build stage is made from, as a hash: the tools it runs (their contents), or for the exe its sources' names,
// sizes and times. A stage stamps the hash into <art>\build\stages\<id>.txt when it finishes, and Build runs it again
// only when the hash changed (or its output is missing): after an update (a new release unzipped over the old one, or
// an older install brought over) only what the changed tools make is built again, and each tool itself only redoes
// what changed (lift.py writes only the chunks that differ, build_lib.py and build_host.py compile only those).
struct Hasher {
    uint64_t h = 1469598103934665603ull;
    void add(const void* p, size_t n) {
        for (size_t i = 0; i < n; i++) h = (h ^ ((const uint8_t*)p)[i]) * 1099511628211ull;
    }
    void add(const std::wstring& s) { add(s.data(), (s.size() + 1) * sizeof(wchar_t)); }
};

// <dir>\<file>, <dir>\*.<ext> (that folder) or <dir>\** (everything under it, but build output: bin, obj, out,
// __pycache__); names relative to the repository, so a moved install keeps its hashes
static void hash_inputs(Hasher& hs, const fs::path& spec, bool contents) {
    std::vector<fs::path> files;
    std::error_code ec;
    const std::wstring leaf = spec.filename().wstring();
    if (leaf == L"**") {
        for (auto it = fs::recursive_directory_iterator(spec.parent_path(), ec); !ec && it != fs::recursive_directory_iterator();
             it.increment(ec)) {
            const std::wstring n = lower(it->path().filename().wstring());
            if (it->is_directory(ec)) {
                if (n == L"bin" || n == L"obj" || n == L"out" || n == L"__pycache__" || n == L".vs") it.disable_recursion_pending();
            } else files.push_back(it->path());
        }
    } else if (leaf.find(L'*') != std::wstring::npos) {
#ifndef _WIN32
        for (auto& f : files_matching(spec.parent_path(), leaf.c_str())) files.push_back(std::move(f));
#else
        WIN32_FIND_DATAW fd;
        const HANDLE h = FindFirstFileExW(spec.c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, 0);
        if (h != INVALID_HANDLE_VALUE) {
            do
                if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) files.push_back(spec.parent_path() / fd.cFileName);
            while (FindNextFileW(h, &fd));
            FindClose(h);
        }
#endif
    } else files.push_back(spec);
    std::sort(files.begin(), files.end());
    for (const auto& f : files) {
        hs.add(lower(f.lexically_relative(g_root).wstring()));
        if (contents) {
            std::ifstream in(f, std::ios::binary);
            if (!in) {
                hs.add(L"(missing)");
                continue;
            }
            char buf[1 << 16];
            while (in.read(buf, sizeof buf) || in.gcount()) hs.add(buf, (size_t)in.gcount());
        } else {
#ifdef _WIN32
            WIN32_FILE_ATTRIBUTE_DATA fa{};
            if (!GetFileAttributesExW(f.c_str(), GetFileExInfoStandard, &fa)) fa = {};
            hs.add(&fa.ftLastWriteTime, sizeof fa.ftLastWriteTime);
            hs.add(&fa.nFileSizeLow, sizeof fa.nFileSizeLow);
            hs.add(&fa.nFileSizeHigh, sizeof fa.nFileSizeHigh);
#else
            std::error_code e1, e2;
            const long long t = fs::last_write_time(f, e1).time_since_epoch().count(), n = (long long)fs::file_size(f, e2);
            hs.add(&t, sizeof t);
            hs.add(&n, sizeof n);
#endif
        }
    }
}

enum StageState { S_TODO, S_DONE, S_CHANGED, S_UNSTAMPED };

struct Stage {
    std::wstring name, id;
    int chain;                          // a stage that runs makes the later ones of its chain run too
    std::vector<fs::path> inputs;
    bool by_time;                       // inputs by name, size and time instead of contents
    std::function<bool()> built;        // its output is there
    std::function<std::vector<Cmd>()> cmds;

#ifdef _WIN32
    fs::path stamp_file() const { return art() / "build" / "stages" / (id + L".txt"); }
#else
    // Apart from Windows' (a checkout both systems build): its names hash with \ and its own module libraries
    fs::path stamp_file() const { return art() / "build" / "stages-linux" / (id + L".txt"); }
#endif
    std::wstring hash() const {
        Hasher hs;
        for (const auto& p : inputs) hash_inputs(hs, p, !by_time);
        wchar_t b[17];
        swprintf(b, 17, L"%016llx", (unsigned long long)hs.h);
        return b;
    }
    StageState state() const {
        if (!built()) return S_TODO;
        const auto l = read_lines(stamp_file());
        if (l.empty()) return S_UNSTAMPED;
        return widen(l[0]) == hash() ? S_DONE : S_CHANGED;
    }
    bool done() const { return state() == S_DONE; }
    void stamp() const {
        std::error_code ec;
        fs::create_directories(stamp_file().parent_path(), ec);
        std::ofstream(stamp_file(), std::ios::binary | std::ios::trunc) << narrow(hash()) << "\r\n";
    }
};

static std::wstring g_game;   // KART_GAME (KART_GAME_<version> for another version)

// For the chosen version: the tools write to art() and shader_dir() through KART_VER (apply_version). Chains: 0 the
// code (each stage reads what the one before wrote), 1 the shaders (from the dump alone), 2 the exe (runs every Build:
// it compiles only what changed and links).
static std::vector<Stage> stages() {
    const std::wstring root = g_root.wstring();
    const fs::path a = art(), sh = shader_dir(), exe = g_root / "host" / "out" / exe_name(), t = g_root / "tools";
    const std::wstring out = L"\"" + (a / "out").wstring() + L"\"";
    const std::vector<Module> mods = ver().modules;
    const auto all = [mods](const fs::path& dir, const wchar_t* ext) {
        for (const auto& m : mods)
            if (!path_exists(dir / (std::wstring(m.name) + ext))) return false;
        return true;
    };
    return {
        {L"Unpack the modules", L"unpack", 0, {t / "nso.py"}, false, [=] { return all(a / "out", L".json"); },
         [=] { return std::vector<Cmd>{{py(L"nso.py") + L" \"" + (fs::path(g_game) / "exefs").wstring() + L"\" " + out, root}}; }},
        {L"Find the functions", L"funcs", 0, {t / "scan.py", t / "funcs.py"}, false, [=] { return all(a / "out", L".funcs.json"); },
         [=] {
             std::vector<Cmd> v;
             for (const auto& m : mods) {
                 v.push_back({py(L"scan.py") + L" " + out + L" " + m.name, root});
                 v.push_back({py(L"funcs.py") + L" " + out + L" " + m.name, root});
             }
             return v;
         }},
        // runtime\*.h: build_lib.py compiles everything again when they changed
        {L"Lift and compile", L"lift", 0,
         {t / "build_all.py", t / "lift.py", t / "a64.py", t / "neon.py", t / "patches.py", t / "build_lib.py", t / "hot_*.txt",
          g_root / "runtime" / "*.h"},
         false, [=] { return all(lib_dir(a / "build"), lib_ext()); },
         [=] { return std::vector<Cmd>{{py(L"build_all.py"), root}}; }},
        {L"Module glue", L"glue", 0, {t / "gen_glue.py"}, false, [=] { return path_exists(a / "gen" / "aot_modules.cpp"); },
         [=] { return std::vector<Cmd>{{py(L"gen_glue.py"), root}}; }},
        // The translation (nvn_shaders.py builds the translator again when tools\xlate changed), then the renderer's
        // reflection table (nothing is drawn without it), the mesh shaders' per-vertex variants, the shaders the
        // NVIDIA driver miscompiles as they are, and the compute shaders whose barriers hang Intel GPUs (.bar.spv)
        {L"Translate the shaders", L"shaders", 1,
         {t / "nvn_shaders.py", t / "nvn_refl.py", t / "nvn_mesh.py", t / "nvn_spvfix.py", t / "nvn_barfix.py",
          t / "shader_split" / "*.py", t / "xlate" / "**"},
         false, [=] { return path_exists(sh / "refl.tsv") && any_file(sh, L"*.fix.spv") && any_file(sh, L"*.bar.spv"); },
         [=] {
             return std::vector<Cmd>{{py(L"nvn_shaders.py"), root}, {py(L"nvn_refl.py"), root},
                                     {py(L"nvn_mesh.py") + L" --flat", root}, {py(L"nvn_spvfix.py") + L" --all", root},
                                     {py(L"nvn_barfix.py"), root}};
         }},
        {L"Link " + exe_name(), L"link-kart" + ver_suffix(), 2,
         {g_root / "host" / "**", g_root / "runtime" / "*.h", g_root / "third_party" / "**", t / "build_host.py",
          lib_dir(a / "build") / (std::wstring(L"*") + lib_ext()), a / "gen" / "aot_modules.cpp"},
         true, [=] { return path_exists(exe); },
         // By its full path: with NoDefaultCurrentDirectoryInExePath set (some security setups) cmd does not look in
         // the current folder
#ifdef _WIN32
         [=] { return std::vector<Cmd>{{L"cmd /c \"\"" + (g_root / "host" / "build.bat").wstring() + L"\"\"", (g_root / "host").wstring()}}; }},
#else
         [=] { return std::vector<Cmd>{{py(L"build_host.py"), root}}; }},
#endif
    };
}

// What an older install hands over to a new release beside it (all of it gitignored, none of it in the release zip):
// the game unpacked, lifted, compiled and its shaders translated (out, gen, build, ver, work), kart.exe with its
// objects, saves and pipeline cache (host\out), and what Set up installed
#ifdef _WIN32
static const wchar_t* BRING_OVER[] = {L"out", L"gen", L"build", L"ver", L"work", L"host\\out",
                                      L"tools\\llvm-mingw", L"tools\\python", L"tools\\dotnet"};
#else
static const wchar_t* BRING_OVER[] = {L"out", L"gen", L"build", L"ver", L"work", L"host/out",
                                      L"tools/llvm", L"tools/venv", L"tools/dotnet", L"tools/spirv"};
#endif
static bool fresh_install() {
    for (const auto* d : {L"out", L"build", L"ver", L"host" DS L"out"})
        if (path_exists(g_root / d)) return false;
    return true;
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
            if (!path_exists(p)) r = {m.name, L"not found in exefs" DS, ST_BAD};
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
        game.push_back({L"romfs", L"not found next to exefs" DS, ST_BAD});
        game_ok = false;
    }
    g_done[0] = game_ok;

    auto& deps = g_rows[1];
    deps.clear();
#ifndef _WIN32
    // Linux: what Set up installs into the repository (clang, the Python packages, .NET, the SPIR-V tools, the
    // translator), and what only the system's package manager can (red: the C++ headers, the Vulkan loader, python3)
    const bool cc = llvm_ok(), hdr = g_tools.cxx_headers;
    const fs::path llvm = setting(L"LLVM");
    deps.push_back({L"C++ compiler", !cc ? L"Set up installs LLVM's clang (1.2 GB)"
                                     : llvm == g_root / "tools" / "llvm" ? L"clang (tools/llvm)" : L"clang (" + llvm.wstring() + L")",
                    cc ? ST_OK : ST_MISSING});
    deps.push_back({L"C++ headers", hdr ? L"found" : L"install g++ (Fedora: gcc-c++, Debian/Ubuntu: g++)", hdr ? ST_OK : ST_BAD});
    const bool py = !g_tools.python.empty(), pkgs = py && g_tools.py_missing.empty();
    if (!py && g_tools.sys_python.empty()) deps.push_back({L"Python", L"install python3 (3.13 or newer)", ST_BAD});
    else if (!py) deps.push_back({L"Python", L"Set up makes tools/venv from it", ST_MISSING, g_tools.sys_python});
    else deps.push_back({L"Python", path_exists(local_python() / "bin" / "python") ? L"tools/venv" : L"", ST_OK, g_tools.python});
    deps.push_back({L"Python packages", !py ? L"Set up installs them" : pkgs ? L"installed" : L"Set up installs " + g_tools.py_missing,
                    pkgs ? ST_OK : ST_MISSING});
    deps.push_back({L".NET 10 SDK", !g_tools.dotnet ? L"Set up installs it (220 MB)"
                                    : path_exists(local_dotnet() / "dotnet") ? L"tools/dotnet" : L"found",
                    g_tools.dotnet ? ST_OK : ST_MISSING});
    const bool spirv = spirv_tools();
    deps.push_back({L"SPIR-V tools", !spirv ? L"Set up takes them from the Vulkan SDK (370 MB)"
                                     : spirv_dir() == g_root / "tools" / "spirv" ? L"tools/spirv" : spirv_dir().wstring(),
                    spirv ? ST_OK : ST_MISSING});
    deps.push_back({L"Vulkan", g_tools.vulkan ? L"loader found" : L"install the Vulkan loader (vulkan-loader, libvulkan1)",
                    g_tools.vulkan ? ST_OK : ST_BAD});
    deps.push_back({L"libvpx", g_tools.vp9 ? L"found" : L"optional: without it the movies show black", g_tools.vp9 ? ST_OK : ST_OPTIONAL});
    const bool xlate = path_exists(fs::path(setting(L"NVN_XLATE")) / "dmsh_xlate");
    deps.push_back({L"Shader translator", xlate ? L"built" : L"Set up builds it", xlate ? ST_OK : ST_MISSING});
    g_done[1] = cc && hdr && pkgs && g_tools.dotnet && spirv && g_tools.vulkan && xlate;
    g_can_setup = hdr && g_tools.vulkan && (py || !g_tools.sys_python.empty());
#else
    // The C++ compiler: llvm-mingw, which Set up downloads, unless BUILD_CC asks for Visual Studio
    const bool ms = build_cc_set() && build_cc() != L"mingw";
    const bool vs = ms && !setting(L"VCVARS").empty() && path_exists(setting(L"VCVARS"));
    const bool cc = ms ? vs : mingw_ok();
    if (ms) deps.push_back({L"Visual Studio", vs ? (build_cc() == L"clang" ? L"C++ tools found (clang-cl)" : L"C++ tools found") :
                            L"install it with \"Desktop development with C++\", or remove BUILD_CC from local.cfg", vs ? ST_OK : ST_BAD});
    else deps.push_back({L"C++ compiler", cc ? L"LLVM MinGW" : L"Set up installs it (190 MB)",
                         cc ? ST_OK : ST_MISSING});
    const bool py = !g_tools.python.empty(), pkgs = py && g_tools.py_missing.empty();
    if (!py) deps.push_back({L"Python", L"Set up installs it (15 MB)", ST_MISSING});
    else deps.push_back({L"Python", path_exists(local_python() / "python.exe") ? L"tools\\python" : L"", ST_OK, g_tools.python});
    deps.push_back({L"Python packages", !py ? L"Set up installs them" : pkgs ? L"installed" : L"Set up installs " + g_tools.py_missing,
                    pkgs ? ST_OK : ST_MISSING});
    deps.push_back({L".NET 10 SDK", !g_tools.dotnet ? L"Set up installs it (220 MB)"
                                    : path_exists(local_dotnet() / "dotnet.exe") ? L"tools\\dotnet" : L"found",
                    g_tools.dotnet ? ST_OK : ST_MISSING});
    const bool spirv = spirv_tools();
    deps.push_back({L"SPIR-V tools", spirv ? L"included (third_party\\spirv)" : L"missing: third_party\\spirv\\bin or the Vulkan SDK",
                    spirv ? ST_OK : ST_BAD});
    deps.push_back({L"VP9 Video Extensions", g_tools.vp9 ? L"found" : L"optional: without it the movies are skipped",
                    g_tools.vp9 ? ST_OK : ST_OPTIONAL});
    const bool xlate = path_exists(fs::path(setting(L"NVN_XLATE")) / "dmsh_xlate.exe");
    deps.push_back({L"Shader translator", xlate ? L"built" : L"Set up builds it", xlate ? ST_OK : ST_MISSING});
    g_done[1] = cc && pkgs && g_tools.dotnet && spirv && xlate;
    g_can_setup = cc || !ms;   // everything else Set up installs itself
#endif

    auto& build = g_rows[2];
    build.clear();
    bool all = true;
    for (const auto& s : stages()) {
        const StageState st = s.state();
        build.push_back({s.name, st == S_DONE ? L"done" : st == S_CHANGED ? (s.by_time ? L"sources changed: compiled again" : L"its tools changed: built again")
                                 : st == S_UNSTAMPED ? L"built by an older version: checked again" : L"to do",
                         st == S_DONE ? ST_OK : ST_MISSING});
        all &= st == S_DONE;
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
#ifdef _WIN32
    const bool pipes = path_exists(g_root / "host" / "out" / (L"nvn_pipelines" + ver_suffix() + L".bin"));
#else
    const bool pipes = path_exists(g_root / "host" / "out" / (L"nvn_pipelines" + ver_suffix() + L"-linux.bin"));
#endif
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

static float g_alpha = 1;                // what is drawn now fades in with the step it belongs to

enum { F_TITLE, F_BODY, F_SMALL, F_LABEL, F_MONO, F_DOT, F_BUTTON, F_COUNT };
static float g_track[F_COUNT];           // letter spacing in DIPs (the title is set a little tight)

enum { T_LEFT = 0, T_CENTER = 1, T_RIGHT = 2, T_VCENTER = 4, T_WRAP = 8 };

static uint32_t mix(uint32_t a, uint32_t b, float t) {
    const auto ch = [&](int s) {
        const float x = (float)((a >> s) & 255), y = (float)((b >> s) & 255);
        return (uint32_t)(x + (y - x) * t + 0.5f) << s;
    };
    return ch(16) | ch(8) | ch(0);
}

static void redraw();

#ifdef _WIN32
static ID2D1Factory* g_d2d;
static IDWriteFactory* g_dw;
static IDWriteFontCollection* g_fonts;   // launcher\fonts\, or null when it is missing (the system's fonts)
static ID2D1HwndRenderTarget* g_rt;
static ID2D1SolidColorBrush* g_brush;
static ID2D1StrokeStyle* g_round;
static float g_dpi = 96;

static IDWriteTextFormat* g_fmt[F_COUNT];
static IDWriteInlineObject* g_ellipsis[F_COUNT];

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

static void fill_rect(D2D1_RECT_F r, uint32_t color) { g_rt->FillRectangle(r, brush(color)); }

static void push_clip(D2D1_RECT_F r) { g_rt->PushAxisAlignedClip(r, D2D1_ANTIALIAS_MODE_ALIASED); }
static void pop_clip() { g_rt->PopAxisAlignedClip(); }

// A line with round ends
static void line_round(float x0, float y0, float x1, float y1, float width, uint32_t color) {
    g_rt->DrawLine({x0, y0}, {x1, y1}, brush(color), width, g_round);
}

static void fill_triangle(D2D1_POINT_2F a, D2D1_POINT_2F b, D2D1_POINT_2F c, uint32_t color) {
    ID2D1PathGeometry* g = nullptr;
    ID2D1GeometrySink* s = nullptr;
    if (FAILED(g_d2d->CreatePathGeometry(&g))) return;
    g->Open(&s);
    s->BeginFigure(a, D2D1_FIGURE_BEGIN_FILLED);
    s->AddLine(b);
    s->AddLine(c);
    s->EndFigure(D2D1_FIGURE_END_CLOSED);
    s->Close();
    s->Release();
    g_rt->FillGeometry(g, brush(color));
    g->Release();
}

// What is drawn until pop_xform: scaled by <s> around (cx, cy), then moved down by <dy>
static std::vector<D2D1_MATRIX_3X2_F> g_xforms;
static void push_xform(float s, float cx, float cy, float dy) {
    D2D1_MATRIX_3X2_F old;
    g_rt->GetTransform(&old);
    g_xforms.push_back(old);
    g_rt->SetTransform(D2D1::Matrix3x2F::Scale(s, s, {cx, cy}) * D2D1::Matrix3x2F::Translation(0, dy) * old);
}
static void pop_xform() {
    g_rt->SetTransform(g_xforms.back());
    g_xforms.pop_back();
}

// A frame: the target made when there is none (or the device was lost), cleared to the background
static bool frame_begin() {
    if (!g_rt) {
        RECT rc;
        GetClientRect(g_hwnd, &rc);
        const auto props = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT, D2D1::PixelFormat(), g_dpi, g_dpi);
        if (FAILED(g_d2d->CreateHwndRenderTarget(props, D2D1::HwndRenderTargetProperties(g_hwnd, D2D1::SizeU(rc.right, rc.bottom)), &g_rt)))
            return false;
        g_rt->CreateSolidColorBrush(D2D1::ColorF(0), &g_brush);
    }
    g_rt->BeginDraw();
    g_rt->Clear(D2D1::ColorF(col::bg));
    return true;
}
static D2D1_SIZE_F frame_size() { return g_rt->GetSize(); }
static void frame_end() {
    if (g_rt->EndDraw() == D2DERR_RECREATE_TARGET) {
        g_brush->Release();
        g_rt->Release();
        g_brush = nullptr;
        g_rt = nullptr;
        redraw();
    }
}
#else
// cairo, Pango and fontconfig: the few calls the launcher makes, declared here so that only the libraries are needed
// (libcairo.so.2, libpango-1.0.so.0, libpangocairo-1.0.so.0, libgobject-2.0.so.0, libfontconfig.so.1: every desktop
// has them), not their -devel packages
extern "C" {
typedef struct _cairo cairo_t;
typedef struct _cairo_surface cairo_surface_t;
typedef struct _cairo_font_options cairo_font_options_t;
cairo_surface_t* cairo_image_surface_create(int format, int width, int height);
unsigned char* cairo_image_surface_get_data(cairo_surface_t* s);
int cairo_image_surface_get_stride(cairo_surface_t* s);
void cairo_surface_flush(cairo_surface_t* s);
void cairo_surface_destroy(cairo_surface_t* s);
cairo_t* cairo_create(cairo_surface_t* target);
void cairo_destroy(cairo_t* cr);
void cairo_save(cairo_t* cr);
void cairo_restore(cairo_t* cr);
void cairo_identity_matrix(cairo_t* cr);
void cairo_translate(cairo_t* cr, double tx, double ty);
void cairo_scale(cairo_t* cr, double sx, double sy);
void cairo_set_source_rgba(cairo_t* cr, double r, double g, double b, double a);
void cairo_paint(cairo_t* cr);
void cairo_fill(cairo_t* cr);
void cairo_stroke(cairo_t* cr);
void cairo_clip(cairo_t* cr);
void cairo_new_path(cairo_t* cr);
void cairo_new_sub_path(cairo_t* cr);
void cairo_close_path(cairo_t* cr);
void cairo_move_to(cairo_t* cr, double x, double y);
void cairo_line_to(cairo_t* cr, double x, double y);
void cairo_arc(cairo_t* cr, double xc, double yc, double radius, double angle1, double angle2);
void cairo_rectangle(cairo_t* cr, double x, double y, double width, double height);
void cairo_set_line_width(cairo_t* cr, double width);
void cairo_set_line_cap(cairo_t* cr, int cap);
void cairo_set_line_join(cairo_t* cr, int join);
cairo_font_options_t* cairo_font_options_create(void);
void cairo_font_options_destroy(cairo_font_options_t* o);
void cairo_font_options_set_antialias(cairo_font_options_t* o, int antialias);
void cairo_font_options_set_hint_style(cairo_font_options_t* o, int style);
void cairo_font_options_set_hint_metrics(cairo_font_options_t* o, int metrics);

typedef struct _PangoContext PangoContext;
typedef struct _PangoLayout PangoLayout;
typedef struct _PangoFontDescription PangoFontDescription;
typedef struct _PangoAttrList PangoAttrList;
typedef struct _PangoAttribute PangoAttribute;
struct PangoRectangle {
    int x, y, width, height;
};
PangoContext* pango_cairo_create_context(cairo_t* cr);
void pango_cairo_context_set_font_options(PangoContext* context, const cairo_font_options_t* options);
void pango_cairo_update_context(cairo_t* cr, PangoContext* context);
void pango_cairo_show_layout(cairo_t* cr, PangoLayout* layout);
PangoLayout* pango_layout_new(PangoContext* context);
void pango_layout_set_text(PangoLayout* layout, const char* text, int length);
void pango_layout_set_font_description(PangoLayout* layout, const PangoFontDescription* desc);
void pango_layout_set_width(PangoLayout* layout, int width);
void pango_layout_set_wrap(PangoLayout* layout, int wrap);
void pango_layout_set_ellipsize(PangoLayout* layout, int ellipsize);
void pango_layout_set_alignment(PangoLayout* layout, int alignment);
void pango_layout_set_attributes(PangoLayout* layout, PangoAttrList* attrs);
void pango_layout_get_extents(PangoLayout* layout, PangoRectangle* ink_rect, PangoRectangle* logical_rect);
PangoFontDescription* pango_font_description_new(void);
void pango_font_description_set_family(PangoFontDescription* desc, const char* family);
void pango_font_description_set_weight(PangoFontDescription* desc, int weight);
void pango_font_description_set_absolute_size(PangoFontDescription* desc, double size);
PangoAttrList* pango_attr_list_new(void);
void pango_attr_list_unref(PangoAttrList* list);
void pango_attr_list_insert(PangoAttrList* list, PangoAttribute* attr);
PangoAttribute* pango_attr_letter_spacing_new(int letter_spacing);
void g_object_unref(void* object);

typedef struct _FcConfig FcConfig;
FcConfig* FcConfigGetCurrent(void);
int FcConfigAppFontAddFile(FcConfig* config, const unsigned char* file);
}
enum { CAIRO_FORMAT_RGB24 = 1, CAIRO_LINE_CAP_ROUND = 1, CAIRO_LINE_JOIN_ROUND = 1, CAIRO_ANTIALIAS_GRAY = 2,
       CAIRO_HINT_STYLE_SLIGHT = 2, CAIRO_HINT_METRICS_OFF = 1 };
constexpr float PANGO_SCALE = 1024;
enum { PANGO_WRAP_WORD = 0, PANGO_ELLIPSIZE_END = 3, PANGO_ALIGN_LEFT = 0, PANGO_ALIGN_CENTER = 1,
       PANGO_ALIGN_RIGHT = 2 };

// The window's pixels: a cairo image, copied into an SDL texture each frame. g_scale: pixels per DIP (the display's
// scale: 1.25 on a desktop set to 125%), so the layout below is the same as on Windows at any scale.
static SDL_Renderer* g_ren;
static SDL_Texture* g_tex;
static int g_px_w, g_px_h;
static cairo_surface_t* g_surf;
static cairo_t* g_cr;
static PangoContext* g_pctx;
static float g_scale = 1;
static PangoFontDescription* g_font[F_COUNT];

static void source(uint32_t rgb) {
    cairo_set_source_rgba(g_cr, ((rgb >> 16) & 255) / 255.0, ((rgb >> 8) & 255) / 255.0, (rgb & 255) / 255.0, g_alpha);
}

// Fonts: Instrument Sans at 400 for text and 500 for headings, buttons and labels; DM Mono for output and build ids
// (launcher/fonts, given to fontconfig for this process only); the desktop's sans-serif and monospace without them
static void create_formats() {
    bool own_sans = false, own_mono = false;
    FcConfig* fc = FcConfigGetCurrent();
    std::error_code ec;
    for (fs::directory_iterator it(g_root / "launcher" / "fonts", ec), end; !ec && it != end; it.increment(ec)) {
        if (lower(it->path().extension().wstring()) != L".ttf") continue;
        if (!FcConfigAppFontAddFile(fc, (const unsigned char*)it->path().c_str())) continue;
        const std::wstring n = lower(it->path().filename().wstring());
        own_sans |= n.rfind(L"instrumentsans", 0) == 0;
        own_mono |= n.rfind(L"dmmono", 0) == 0;
    }
    const char* sans = own_sans ? "Instrument Sans" : "sans-serif";
    const char* mono = own_mono ? "DM Mono" : "monospace";
    const int head = own_sans ? 500 : 600;
    struct F {
        const char* family;
        int weight;
        float size, track;
    } fs_[F_COUNT] = {
        {sans, head, 26, own_sans ? -0.7f : 0}, {sans, 400, 14, 0}, {sans, 400, 12.5f, 0}, {sans, head, 12.5f, 0},
        {mono, 400, 12.5f, 0}, {sans, head, 15, 0}, {sans, head, 14, 0},
    };
    for (int i = 0; i < F_COUNT; i++) {
        g_font[i] = pango_font_description_new();
        pango_font_description_set_family(g_font[i], fs_[i].family);
        pango_font_description_set_weight(g_font[i], fs_[i].weight);
        pango_font_description_set_absolute_size(g_font[i], fs_[i].size * PANGO_SCALE);
        g_track[i] = fs_[i].track;
    }
    // Grey antialiasing and outlines at their true size (no hinted metrics), as DirectWrite lays text out
    cairo_surface_t* one = cairo_image_surface_create(CAIRO_FORMAT_RGB24, 1, 1);
    cairo_t* cr = cairo_create(one);
    g_pctx = pango_cairo_create_context(cr);
    cairo_font_options_t* fo = cairo_font_options_create();
    cairo_font_options_set_antialias(fo, CAIRO_ANTIALIAS_GRAY);
    cairo_font_options_set_hint_style(fo, CAIRO_HINT_STYLE_SLIGHT);
    cairo_font_options_set_hint_metrics(fo, CAIRO_HINT_METRICS_OFF);
    pango_cairo_context_set_font_options(g_pctx, fo);
    cairo_font_options_destroy(fo);
    cairo_destroy(cr);
    cairo_surface_destroy(one);
}

// <s> laid out <w> wide: one line ending in an ellipsis when it does not fit, or wrapped at words (T_WRAP)
static PangoLayout* make_layout(const std::wstring& s, int f, float w, int flags) {
    PangoLayout* l = pango_layout_new(g_pctx);
    const std::string u = narrow(s);
    pango_layout_set_text(l, u.c_str(), (int)u.size());
    pango_layout_set_font_description(l, g_font[f]);
    pango_layout_set_width(l, (int)(w * PANGO_SCALE));
    if (flags & T_WRAP) pango_layout_set_wrap(l, PANGO_WRAP_WORD);
    else pango_layout_set_ellipsize(l, PANGO_ELLIPSIZE_END);
    pango_layout_set_alignment(l, flags & T_CENTER ? PANGO_ALIGN_CENTER : flags & T_RIGHT ? PANGO_ALIGN_RIGHT : PANGO_ALIGN_LEFT);
    if (g_track[f]) {
        PangoAttrList* al = pango_attr_list_new();
        pango_attr_list_insert(al, pango_attr_letter_spacing_new((int)(g_track[f] * PANGO_SCALE)));
        pango_layout_set_attributes(l, al);
        pango_attr_list_unref(al);
    }
    return l;
}

static void text(const std::wstring& s, int f, D2D1_RECT_F r, uint32_t color, int flags = 0) {
    if (s.empty() || r.right <= r.left) return;
    PangoLayout* l = make_layout(s, f, r.right - r.left, flags);
    PangoRectangle ink, logical;
    pango_layout_get_extents(l, &ink, &logical);
    float y = r.top;
    if (flags & T_VCENTER) y += ((r.bottom - r.top) - (float)logical.height / PANGO_SCALE) / 2;
    cairo_save(g_cr);
    cairo_rectangle(g_cr, r.left, r.top, r.right - r.left, r.bottom - r.top);
    cairo_clip(g_cr);
    source(color);
    cairo_move_to(g_cr, r.left, y);
    pango_cairo_show_layout(g_cr, l);
    cairo_restore(g_cr);
    g_object_unref(l);
}

static D2D1_SIZE_F measure(const std::wstring& s, int f, float maxw, int flags = 0) {
    PangoLayout* l = make_layout(s, f, maxw, flags);
    PangoRectangle ink, logical;
    pango_layout_get_extents(l, &ink, &logical);
    g_object_unref(l);
    return {(float)logical.width / PANGO_SCALE, (float)logical.height / PANGO_SCALE};
}

static void round_path(D2D1_RECT_F r, float radius) {
    radius = std::min({radius, (r.right - r.left) / 2, (r.bottom - r.top) / 2});
    cairo_new_path(g_cr);
    cairo_new_sub_path(g_cr);
    cairo_arc(g_cr, r.right - radius, r.top + radius, radius, -M_PI / 2, 0);
    cairo_arc(g_cr, r.right - radius, r.bottom - radius, radius, 0, M_PI / 2);
    cairo_arc(g_cr, r.left + radius, r.bottom - radius, radius, M_PI / 2, M_PI);
    cairo_arc(g_cr, r.left + radius, r.top + radius, radius, M_PI, 3 * M_PI / 2);
    cairo_close_path(g_cr);
}

static void fill_round(D2D1_RECT_F r, float radius, uint32_t color) {
    round_path(r, radius);
    source(color);
    cairo_fill(g_cr);
}

// A 1px line just inside <r>
static void stroke_round(D2D1_RECT_F r, float radius, uint32_t color) {
    round_path({r.left + 0.5f, r.top + 0.5f, r.right - 0.5f, r.bottom - 0.5f}, radius - 0.5f);
    source(color);
    cairo_set_line_width(g_cr, 1);
    cairo_stroke(g_cr);
}

static void fill_circle(float x, float y, float r, uint32_t color) {
    cairo_new_path(g_cr);
    cairo_arc(g_cr, x, y, r, 0, 2 * M_PI);
    source(color);
    cairo_fill(g_cr);
}

static void stroke_circle(float x, float y, float r, uint32_t color, float width) {
    cairo_new_path(g_cr);
    cairo_arc(g_cr, x, y, r, 0, 2 * M_PI);
    source(color);
    cairo_set_line_width(g_cr, width);
    cairo_stroke(g_cr);
}

static void fill_rect(D2D1_RECT_F r, uint32_t color) {
    cairo_new_path(g_cr);
    cairo_rectangle(g_cr, r.left, r.top, r.right - r.left, r.bottom - r.top);
    source(color);
    cairo_fill(g_cr);
}

static void push_clip(D2D1_RECT_F r) {
    cairo_save(g_cr);
    cairo_new_path(g_cr);
    cairo_rectangle(g_cr, r.left, r.top, r.right - r.left, r.bottom - r.top);
    cairo_clip(g_cr);
}
static void pop_clip() { cairo_restore(g_cr); }

// A line with round ends
static void line_round(float x0, float y0, float x1, float y1, float width, uint32_t color) {
    cairo_new_path(g_cr);
    cairo_move_to(g_cr, x0, y0);
    cairo_line_to(g_cr, x1, y1);
    cairo_set_line_cap(g_cr, CAIRO_LINE_CAP_ROUND);
    cairo_set_line_join(g_cr, CAIRO_LINE_JOIN_ROUND);
    cairo_set_line_width(g_cr, width);
    source(color);
    cairo_stroke(g_cr);
}

static void fill_triangle(D2D1_POINT_2F a, D2D1_POINT_2F b, D2D1_POINT_2F c, uint32_t color) {
    cairo_new_path(g_cr);
    cairo_move_to(g_cr, a.x, a.y);
    cairo_line_to(g_cr, b.x, b.y);
    cairo_line_to(g_cr, c.x, c.y);
    cairo_close_path(g_cr);
    source(color);
    cairo_fill(g_cr);
}

// What is drawn until pop_xform: scaled by <s> around (cx, cy), then moved down by <dy>
static void push_xform(float s, float cx, float cy, float dy) {
    cairo_save(g_cr);
    cairo_translate(g_cr, cx, cy + dy);
    cairo_scale(g_cr, s, s);
    cairo_translate(g_cr, -cx, -cy);
}
static void pop_xform() { cairo_restore(g_cr); }

// A frame: the image (and the texture it goes to) made again when the window's size in pixels changed
static bool frame_begin() {
    int pw = 0, ph = 0;
    SDL_GetWindowSizeInPixels(g_window, &pw, &ph);
    if (pw <= 0 || ph <= 0) return false;
    const float sc = SDL_GetWindowDisplayScale(g_window);
    g_scale = sc > 0 ? sc : 1;
    if (!g_surf || pw != g_px_w || ph != g_px_h) {
        if (g_cr) cairo_destroy(g_cr);
        if (g_surf) cairo_surface_destroy(g_surf);
        if (g_tex) SDL_DestroyTexture(g_tex);
        g_surf = cairo_image_surface_create(CAIRO_FORMAT_RGB24, pw, ph);
        g_cr = cairo_create(g_surf);
        g_tex = SDL_CreateTexture(g_ren, SDL_PIXELFORMAT_XRGB8888, SDL_TEXTUREACCESS_STREAMING, pw, ph);
        g_px_w = pw, g_px_h = ph;
    }
    if (!g_tex) return false;
    cairo_identity_matrix(g_cr);
    cairo_scale(g_cr, g_scale, g_scale);
    pango_cairo_update_context(g_cr, g_pctx);
    source(col::bg);
    cairo_paint(g_cr);
    return true;
}
static D2D1_SIZE_F frame_size() { return {g_px_w / g_scale, g_px_h / g_scale}; }
static void frame_end() {
    cairo_surface_flush(g_surf);
    SDL_UpdateTexture(g_tex, nullptr, cairo_image_surface_get_data(g_surf), cairo_image_surface_get_stride(g_surf));
    SDL_RenderTexture(g_ren, g_tex, nullptr, nullptr);
    SDL_RenderPresent(g_ren);
}
#endif

// ---------------------------------------------------------------------------------------------------------------
// UI state

enum {
    B_NONE = -1, B_BROWSE = 1, B_CONTINUE, B_SETUP, B_BUILD, B_REBUILD, B_PLAY, B_BAKE, B_STOP, B_NUP, B_IMPORT, B_UPDATE, B_VER = 50, B_CC = 60, B_DOT = 100,
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
static bool g_bringing = false;    // an older version being brought over (bring_over)

// Updates (the section after bring_over)
static std::wstring g_release;       // this folder's release (launcher/out/release.txt), '' in a checkout
static std::wstring g_update_ver;    // a newer release on GitHub, '' while none is known
static std::wstring g_update_notes;  // the start of its notes
static bool g_update_ask = false;    // found while a job ran or a question was open: asked once that is over
static bool g_updating = false;      // the newer release being downloaded and unpacked (start_update)
static int g_asking = 0;             // a message box or file dialog is open
struct Asking {
    Asking() { g_asking++; }
    ~Asking() { g_asking--; }
};
static std::vector<std::pair<std::wstring, std::wstring>> g_env0;   // what the launcher changes in its environment, as it was
static void maybe_offer_update();
static void update_done(bool ok);
#ifndef _WIN32
static bool g_quit_asked = false;   // the window's close button (or --quit, or an update), handled by the main loop
#endif

// A scripted run, launcher.exe [--bring-over <older folder>] [--setup] [--build] [--quit] [--log <file>]: what the buttons do,
// one action after the other, each once the one before finished well. --quit closes the launcher at the end, or at
// the first failure, with exit code 0 or 1 (--log keeps the output). --update installs the newest release over this
// folder without asking, and the launcher starts again.
static std::deque<std::pair<std::wstring, std::wstring>> g_script;
static bool g_scripting = false, g_script_quit = false;
static int g_exit_code = 0;
static void run_script();

static const wchar_t* STEP_NAMES[4] = {L"Game", L"Dependencies", L"Build", L"Play"};
static const wchar_t* STEP_TITLES[4] = {L"Your game dump", L"Dependencies", L"Build", L"Play"};
static const wchar_t* STEP_TEXT[4] = {
    L"Pick your own dump of the game (the folder with exefs" DS L" and romfs" DS L") or a .nup file of it.",
#ifdef _WIN32
    L"Set up installs what is missing into the repository. No admin rights or Visual Studio needed.",
#else
    L"Set up installs what is missing into the repository, no root needed. What is marked red comes from your distribution's packages.",
#endif
    L"Lifts the game to C++, compiles it and translates its shaders. The first build takes about half an hour; after an update only what changed is built again.",
    L"Starts the game. Bake textures once so the first races do not hitch.",
};

#ifdef _WIN32
static void redraw() {
    InvalidateRect(g_hwnd, nullptr, FALSE);
}
#else
static bool g_dirty = true;   // the window is drawn again once the events waiting are handled
static void redraw() { g_dirty = true; }
#endif

// A message, or a question answered Yes or No (M_ASK, M_WARN): true for Yes. Windows' message box; Linux: drawn in
// the window (modal_box)
enum { M_INFO, M_ERROR, M_ASK, M_WARN };
#ifndef _WIN32
static bool modal_box(const std::wstring& text, const wchar_t* title, int kind);
#endif
static bool msg_box(const std::wstring& text, const wchar_t* title, int kind) {
    Asking asking;
#ifdef _WIN32
    const UINT f = kind == M_INFO ? MB_ICONINFORMATION : kind == M_ERROR ? MB_ICONERROR : kind == M_ASK ? MB_YESNO | MB_ICONQUESTION
                                                                                                        : MB_YESNO | MB_ICONWARNING;
    const int r = MessageBoxW(g_hwnd, text.c_str(), title, f);
    return kind < M_ASK || r == IDYES;
#else
    return modal_box(text, title, kind);
#endif
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
#ifdef _WIN32
constexpr UINT_PTR TIMER_JOB = 1, TIMER_ANIM = 2;
#endif

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
    Lift(D2D1_RECT_F r, const Anim& a, float rise = 1) {
        const float h = ease(a.hov), p = ease(a.press), s = 1 - 0.015f * p;
        const float dy = -rise * h * (1 - p) + p;
        push_xform(s, (r.left + r.right) / 2, (r.top + r.bottom) / 2, dy);
    }
    ~Lift() { pop_xform(); }
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

// A log line that reports a failure (shown red): it says error or failed, but not only as a zero count ("0 Error(s)",
// "failed so far 0", "0 failed").
static bool bad_line(const std::wstring& lo) {
    static const std::wregex zero(L"(^|[^0-9.])0 +(errors?|error\\(s\\)|failed|failures?)|(errors?|failed|failures?)( so far)?:? +0($|[^0-9.])");
    const std::wstring rest = std::regex_replace(lo, zero, L" ");
    return rest.find(L"error") != std::wstring::npos || rest.find(L"failed") != std::wstring::npos;
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
    push_clip(in);
    const int first = std::max(0, total - vis - g_log_scroll);
    for (int i = first, k = 0; i < total && k < vis; i++, k++) {
        const std::wstring& l = g_log[i];
        const std::wstring lo = lower(l);
        uint32_t c = col::log;
        if (l.rfind(L"> ", 0) == 0) c = col::accent;
        else if (bad_line(lo)) c = col::red;
        text(l, F_MONO, {in.left, in.top + k * lh, in.right, in.top + (k + 1) * lh}, c);
    }
    pop_clip();
}

static void draw_check(float x, float y, uint32_t color) {
    line_round(x - 6.5f, y + 0.5f, x - 2.2f, y + 4.8f, 2.4f, color);
    line_round(x - 2.2f, y + 4.8f, x + 7, y - 5, 2.4f, color);
}

static void draw_play_icon(float x, float y, uint32_t color) {
    fill_triangle({x - 5, y - 8}, {x + 8.5f, y}, {x - 5, y + 8}, color);
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

// The C++ compiler: a segmented control at (x0, y), BUILD_CC=mingw or cl in local.cfg; returns the y below it.
// clang (clang-cl) counts as Visual Studio, whose tools it builds with.
#ifdef _WIN32
static const wchar_t* const CC_NAMES[] = {L"LLVM MinGW", L"Visual Studio"};
static float draw_compiler(float x0, float x1, float y, bool enabled) {
    const int cur = build_cc() == L"mingw" ? 0 : 1;
    text(L"C++ compiler", F_LABEL, {x0, y, x1, y + 18}, col::muted);
    y += 24;
    float seg[2], total = 6;
    for (int i = 0; i < 2; i++) total += seg[i] = measure(CC_NAMES[i], F_LABEL, 300).width + 30;
    const D2D1_RECT_F box{x0, y, x0 + total, y + 32};
    fill_round(box, R_CTRL, col::inset);
    stroke_round(box, R_CTRL, col::line);
    float x = box.left + 3;
    for (int i = 0; i < 2; i++) {
        const D2D1_RECT_F r{x, box.top + 3, x + seg[i], box.bottom - 3};
        if (i == cur) {
            fill_round(r, R_CTRL - 2, col::accent);
            text(CC_NAMES[i], F_LABEL, r, col::accent_ink, T_CENTER | T_VCENTER);
        } else {
            const float h = enabled ? ease(anim(B_CC + i).hov) : 0;
            if (h > 0) fill_round(r, R_CTRL - 2, mix(col::inset, col::btn_hover, h));
            text(CC_NAMES[i], F_LABEL, r, enabled ? mix(col::muted, col::text, h) : col::dim, T_CENTER | T_VCENTER);
            if (enabled) g_hits.push_back({B_CC + i, r});
        }
        x += seg[i];
    }
    const std::wstring note = !env_var(L"BUILD_CC").empty()
        ? L"BUILD_CC=" + env_var(L"BUILD_CC") + L" is set in the environment: it is used instead of local.cfg."
        : cur == 0 ? L"Default. About 50% faster."
                   : L"Needs Visual Studio. Slower game.";
    text(note, F_SMALL, {box.right + 14, box.top, x1, box.bottom}, col::muted, T_VCENTER | T_WRAP);
    return box.bottom + 22;
}
#endif

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
        } else {
            float x = x0;
            if (g_done[0]) x = button(B_CONTINUE, L"Continue", x, y + 18, true, true);
            // A new release unzipped beside an older one: that one's build, tools and settings come over, and Build
            // then does only what changed
            if (fresh_install()) {
                x = button(B_IMPORT, L"Bring over an older version…", x, y + 18, false, true);
                text(L"Built an older release? Pick its folder: only what changed gets built again.", F_SMALL,
                     {x + 4, y + 18, x1, y + 58}, col::muted, T_VCENTER | T_WRAP);
            }
        }
        break;
    }
    case 1: {
#ifdef _WIN32
        y = draw_compiler(x0, x1, y, !busy);
#endif
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
#ifdef _WIN32
        text(std::wstring(CC_NAMES[build_cc() == L"mingw" ? 0 : 1]) + L" (chosen in Dependencies), up to " + mp +
                 L" compiler processes at once (BUILD_MP)", F_SMALL,
             {x0, y + 6, x1, y + 26}, col::muted);
#else
        text(L"clang, up to " + mp + L" compiler processes at once (BUILD_MP)", F_SMALL, {x0, y + 6, x1, y + 26}, col::muted);
#endif
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
#ifdef _WIN32
        text(L"Settings: " + (settings.empty() ? std::wstring(L"the defaults") : settings) + L"   (HOST_* lines in local.cfg)", F_SMALL,
             {x0, y + 6, x1, y + 26}, col::muted);
#else
        text(L"Settings: " + (settings.empty() ? std::wstring(L"the defaults") : settings) + L"   (HOST_* lines in local.linux.cfg or local.cfg)",
             F_SMALL, {x0, y + 6, x1, y + 26}, col::muted);
#endif
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
    push_xform(1, 0, 0, (1 - t) * 6);
    g_alpha = t;
    paint_steps(p);
    g_alpha = 1;
    pop_xform();
}

#ifndef _WIN32
static void paint_modal(D2D1_SIZE_F sz);
#endif

static void paint() {
    const bool moving = step_anims();
    if (!frame_begin()) return;
    g_hits.clear();
    g_log_rect = {};
    const D2D1_SIZE_F sz = frame_size();
    const float m = 32, r = 18, cy = 58;

    // The steps: dots on a line, the finished ones white
    float cx[4];
    for (int i = 0; i < 4; i++) cx[i] = m + 46 + (sz.width - 2 * m - 92) * i / 3.f;
    for (int i = 0; i < 3; i++) fill_rect({cx[i] + r, cy - 1, cx[i + 1] - r, cy + 1}, g_done[i] && g_done[i + 1] ? col::accent : col::line);
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

    // A newer release: a small button at the top right asks again (it is asked once by itself, offer_update)
    if (!g_update_ver.empty() && !g_updating) {
        const std::wstring label = L"Update to " + g_update_ver;
        const float w = measure(label, F_SMALL, 400).width + 26;
        const D2D1_RECT_F b{sz.width - m - w, 8, sz.width - m, 33};
        const bool on = !g_running;
        if (on) g_hits.push_back({B_UPDATE, b});
        const float h = on ? ease(anim(B_UPDATE).hov) : 0;
        fill_round(b, R_CTRL, mix(col::inset, col::btn_hover, h));
        stroke_round(b, R_CTRL, mix(col::line_strong, col::muted, h * 0.35f));
        fill_circle(b.left + 11, (b.top + b.bottom) / 2, 3, on ? col::accent : col::dim);
        text(label, F_SMALL, {b.left + 18, b.top, b.right, b.bottom}, on ? col::text : col::dim, T_CENTER | T_VCENTER);
    }

    paint_panel({m, cy + r + 48, sz.width - m, sz.height - m});
#ifndef _WIN32
    paint_modal(sz);   // a question over it all
#endif

    frame_end();
    // The next frame while something moves; the timer stops once everything rests
    g_moving = moving;
#ifdef _WIN32
    if (moving) SetTimer(g_hwnd, TIMER_ANIM, 15, nullptr);
    else KillTimer(g_hwnd, TIMER_ANIM);
#endif
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
#ifdef _WIN32
    SetTimer(g_hwnd, TIMER_JOB, 500, nullptr);
#endif
    std::thread([cmds = std::move(cmds)] {
        DWORD code = 0;
        for (const auto& c : cmds) {
            {
                std::lock_guard lk(g_mx);
                g_prog = {c.stage, L"", c.stage_i, c.stage_n};   // each command reports its own progress
            }
            code = run_cmd(c);
            if (code || g_cancel) break;
            if (c.after) c.after();
        }
#ifdef _WIN32
        PostMessageW(g_hwnd, WM_APP_DONE, code, g_cancel ? 1 : 0);
#else
        post_event(g_ev_done, (int)code, g_cancel);
#endif
    }).detach();
    redraw();
}

static void stop_job() {
    g_cancel = true;
    std::lock_guard lk(g_mx);
#ifdef _WIN32
    if (g_job) TerminateJobObject(g_job, 1);
#else
    if (g_job) kill(-g_job, SIGKILL);
#endif
}

static void job_done(DWORD code, bool cancelled) {
    g_running = false;
#ifdef _WIN32
    KillTimer(g_hwnd, TIMER_JOB);
#endif
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
        if (!save_setting(game_key(), dest)) msg_box(L"Could not write " CFG_NAME L".", L"Launcher", M_ERROR);
        g_status = L"Unpacked in " + took + L" into " + dest;
    }
    // An older version brought over: its game version, Python and .NET (tools\python, tools\dotnet) are here now
    if (std::exchange(g_bringing, false) && !cancelled && code == 0) {
        const std::wstring want = local_cfg()[L"LAUNCHER_VERSION"];
        for (int v = 0; v < VERSION_COUNT; v++)
            if (want == VERSIONS[v].name && env_var(L"KART_VER").empty()) g_ver = v;
        apply_version();
        check_dotnet(), check_python();
        g_status = L"Brought over in " + took + L": Build now does only what changed";
    }
    const int step = g_step;
    if (step == 1) check_dotnet(), check_python();   // what Set up just installed
    refresh();
    if (!cancelled && code == 0 && (step == 1 || step == 2) && g_done[step]) g_step = step + 1, g_status.clear();
    redraw();
    if (std::exchange(g_updating, false)) update_done(!cancelled && code == 0);
    if (g_scripting) {
        if (cancelled || code) g_exit_code = 1;
        run_script();
    }
    maybe_offer_update();
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

static std::wstring pick(bool folder, const wchar_t* title, const std::wstring& start);
static void browse() {
    std::wstring picked = pick(true, L"The game dump: the folder holding exefs and romfs", g_game);
    if (picked.empty()) return;
    // exefs\ or romfs\ itself picked: the dump is the folder above
    const fs::path pp = picked;
    const std::wstring leaf = lower(pp.filename().wstring());
    if ((leaf == L"exefs" || leaf == L"romfs") && !path_exists(pp / "exefs")) picked = pp.parent_path().wstring();
    // A dump of another known version switches to it, and is saved as that version's dump
    if (const int v = dump_version(picked); v >= 0 && v != g_ver) select_version(v);
    if (!save_setting(game_key(), picked)) msg_box(L"Could not write " CFG_NAME L".", L"Launcher", M_ERROR);
    g_unpack_log = false;
    refresh();
    redraw();
}

// A file or folder dialog; '' when cancelled (Linux: in the window section, through SDL)
#ifdef _WIN32
static std::wstring pick(bool folder, const wchar_t* title, const std::wstring& start) {
    Asking asking;
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
#endif

// A .nup (one file holding the game, its updates and add-ons): tools\nup.py says which version it holds, and unpacks it
// (the newest update over the base, as Eglantine boots it) into a folder named after that version in a folder the user
// picks. Unpacking again into the same folder only writes what is missing.
static void open_nup() {
    const std::wstring file = pick(false, L"The game as a .nup file", fs::path(g_game).parent_path().wstring());
    if (file.empty()) return;
    // This comes before the Dependencies step, but unpacking needs Python and its zstd package
    check_python();
    if (g_tools.python.empty()) {
        msg_box(L"Unpacking a .nup needs Python, which is not installed yet. Run Set up in the Dependencies "
                L"step (2) first, then open the .nup again.", L"Open .nup", M_INFO);
        return;
    }
    const bool install_pkgs = !g_tools.py_missing.empty();
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
        msg_box(L"tools" DS L"nup.py could not read this file.\n\n" + why, L"Open .nup", M_ERROR);
        return;
    }
    const bool known = std::any_of(std::begin(VERSIONS), std::end(VERSIONS), [&](const Version& v) { return version == v.name; });
    if (!known && !msg_box(L"This .nup holds version " + version + L", which is not one this recompilation is made "
                                                                     L"for (1.0.0 and 1.8.0). Unpack it anyway?",
                           L"Open .nup", M_WARN))
        return;
    const std::wstring title = L"Where to unpack version " + version + L" (a folder \"" + version + L"\" is made in it)";
    const std::wstring parent = pick(true, title.c_str(), fs::path(file).parent_path().wstring());
    if (parent.empty()) return;
    fs::path dest = parent;
    if (lower(dest.filename().wstring()) != lower(version)) dest /= version;
    // Room for it: the files still to write, less what an earlier unpacking there already wrote (roughly)
#ifdef _WIN32
    ULARGE_INTEGER room{};
    const bool room_known = GetDiskFreeSpaceExW(parent.c_str(), &room, nullptr, nullptr);
    const unsigned long long free_bytes = room.QuadPart;
#else
    std::error_code sec;
    const fs::space_info room = fs::space(parent, sec);
    const bool room_known = !sec;
    const unsigned long long free_bytes = room.available;
#endif
    if (room_known && !path_exists(dest / "romfs") && free_bytes < bytes + (1ull << 30)) {
        wchar_t msg[256];
        swprintf(msg, 256, L"Version %ls needs %.1f GB, and only %.1f GB are free there. Unpack anyway?", version.c_str(),
                 bytes / 1e9, free_bytes / 1e9);
        if (!msg_box(msg, L"Open .nup", M_WARN)) return;
    }
    g_nup_dest = dest.wstring();
    g_unpack_log = true;
    std::vector<Cmd> cmds;
#ifdef _WIN32
    if (install_pkgs) cmds.push_back({L"python tools\\deps.py install", g_root.wstring(), L"Python packages"});
#else
    if (install_pkgs) cmds.push_back({L"python3 tools/linux.py setup python", g_root.wstring(), L"Python packages"});
#endif
    cmds.push_back({nup_py + L" extract \"" + file + L"\" \"" + g_nup_dest + L"\"", g_root.wstring(), L"Unpacking"});
    for (size_t i = 0; i < cmds.size(); i++) cmds[i].stage_i = (int)i + 1, cmds[i].stage_n = (int)cmds.size();
    start_job(L"Unpacking version " + version, cmds);
}

// An older release's folder: its build, tools and settings come over (moved on the same drive, which is instant;
// copied from another one), so Build only redoes the stages whose tools changed since. local.cfg: the keys this one
// lacks, with paths into the old folder pointed here.
// interactive: the questions and errors as message boxes; scripted (--bring-over): no questions, errors in the log.
// False when nothing started.
static bool bring_over_from(const fs::path& picked, bool interactive) {
    const auto fail = [&](const wchar_t* why) {
        if (interactive) msg_box(why, L"Bring over", M_INFO);
        else log_line(std::wstring(L"bring over: ") + why);
        return false;
    };
    fs::path old;
    for (fs::path d = picked;; d = d.parent_path()) {   // launcher\out or another folder inside it picked: the one above
        if (path_exists(d / "tools" / "paths.py")) {
            old = d;
            break;
        }
        if (d == d.parent_path()) return fail(L"That is not a kart-recomp folder (it has no tools" DS L"paths.py).");
    }
    std::error_code ec;
    if (fs::equivalent(old, g_root, ec)) return fail(L"That is this folder. Pick the older version's folder.");
    std::vector<std::wstring> items;
    for (const auto* d : BRING_OVER)
        if (path_exists(old / d) && !path_exists(g_root / d)) items.push_back(d);
    const bool cfg = path_exists(old / CFG_NAME);
    if (items.empty() && !cfg) return fail(L"Nothing to bring over: that folder has no build and no " CFG_NAME L".");
#ifdef _WIN32
    wchar_t va[MAX_PATH] = {}, vb[MAX_PATH] = {};
    GetVolumePathNameW(old.c_str(), va, MAX_PATH);
    GetVolumePathNameW(g_root.c_str(), vb, MAX_PATH);
    const bool same = lower(va) == lower(vb);
#else
    struct stat sa{}, sb{};
    const bool same = stat(old.c_str(), &sa) == 0 && stat(g_root.c_str(), &sb) == 0 && sa.st_dev == sb.st_dev;
#endif
    const std::wstring ask = std::wstring(same ? L"Move" : L"Copy") +
        L" the build, the tools Set up installed, your saves and settings from\n\n" + old.wstring() + L"\n\ninto this folder?\n\n" +
        (same ? L"Moving is instant (same drive). The older version will not run afterwards."
              : L"It is on another drive, so this copies (it can take a few minutes). The older version stays as it is.");
    if (interactive && !msg_box(ask, L"Bring over an older version", M_ASK)) return false;

    std::vector<Cmd> cmds;
    for (const auto& d : items) {
        const fs::path src = old / d, dst = g_root / d;
        Cmd c{std::wstring(same ? L"move " : L"copy ") + src.wstring(), g_root.wstring(), L"Bringing over " + d};
#ifndef _WIN32
        if (same) {
            c.fn = [src, dst]() -> DWORD {
                std::error_code e;
                fs::create_directories(dst.parent_path(), e);
                if (rename(src.c_str(), dst.c_str()) == 0) return 0;
                log_line(L"could not move it (" + widen(strerror(errno)) + L")");
                return 1;
            };
        } else {
            // cp -a keeps the files' times (build_lib.py and build_host.py go by them)
            c.fn = [src, dst]() -> DWORD {
                return run_cmd({L"mkdir -p \"" + dst.parent_path().wstring() + L"\" && cp -a \"" + src.wstring() + L"\" \"" +
                                    dst.wstring() + L"\"", L""});
            };
        }
#else
        if (same) {
            c.fn = [src, dst]() -> DWORD {
                std::error_code e;
                fs::create_directories(dst.parent_path(), e);
                // A file inside still open for a moment (a virus scanner, Explorer listing it): a few tries
                DWORD err = 0;
                for (int i = 0; i < 10; i++) {
                    if (MoveFileExW(src.c_str(), dst.c_str(), 0)) return 0;
                    err = GetLastError();
                    if (err != ERROR_ACCESS_DENIED && err != ERROR_SHARING_VIOLATION) break;
                    Sleep(1000);
                }
                log_line(L"could not move it (Windows error " + std::to_wstring(err) + L"): is a program using it?");
                return err ? err : 1;
            };
        } else {
            // robocopy keeps the files' times (build_lib.py and build_host.py go by them); below 8 is success
            c.fn = [src, dst]() -> DWORD {
                const DWORD code = run_cmd({L"robocopy \"" + src.wstring() + L"\" \"" + dst.wstring() +
                                                L"\" /E /COPY:DAT /DCOPY:T /R:1 /W:1 /NFL /NDL /NJH /NP", L""});
                return code < 8 ? 0 : code;
            };
        }
#endif
        cmds.push_back(c);
    }
    if (cfg) {
        Cmd c{L"settings from " + (old / CFG_NAME).wstring(), g_root.wstring(), L"Settings"};
        c.fn = [old]() -> DWORD {
            const auto have = local_cfg();
            const std::wstring from = lower(old.wstring());
            int n = 0;
            for (const auto& l : read_lines(old / CFG_NAME)) {
                std::wstring w = widen(l);
                w = w.substr(0, w.find(L'#'));
                const size_t eq = w.find(L'=');
                if (eq == std::wstring::npos) continue;
                const std::wstring k = trim(w.substr(0, eq));
                std::wstring v = trim(w.substr(eq + 1));
                if (k.empty() || have.count(k)) continue;
                if (lower(v).rfind(from, 0) == 0 && (v.size() == from.size() || v[from.size()] == L'\\' || v[from.size()] == L'/'))
                    v = g_root.wstring() + v.substr(from.size());   // into the old folder: here
                if (!save_setting(k, v)) return 1;
                n++;
            }
            log_line(std::to_wstring(n) + L" settings taken over");
            return 0;
        };
        cmds.push_back(c);
    }
    for (size_t i = 0; i < cmds.size(); i++) cmds[i].stage_i = (int)i + 1, cmds[i].stage_n = (int)cmds.size();
    g_unpack_log = true;
    g_bringing = true;
    start_job(L"Bringing over " + old.filename().wstring(), cmds);
    return true;
}
static void bring_over() {
    const std::wstring picked = pick(true, L"The older version's folder (the one holding launcher, tools and host)",
                                     g_root.parent_path().wstring());
    if (!picked.empty()) bring_over_from(picked, true);
}

// ---------------------------------------------------------------------------------------------------------------
// Updates: a release carries launcher/out/release.txt (its version; a checkout has none and is never offered one). At
// startup tools/update.py check asks GitHub for the newest release in the background; a newer one is asked about once
// (No: UPDATE_SKIP=<version> in the settings, and "Update to <version>" at the top right asks again). Yes: update.py
// install puts it over this folder (the changed files replaced, the ones it dropped deleted, by launcher/out/files.txt;
// the build, tools, saves and settings stay, so Build redoes only what changed) and the launcher starts again. A file
// in use (Windows: this launcher.exe) is renamed to <name>.old; the next start deletes those (clean_leftovers).
// UPDATE_CHECK=0 in the settings: no check.

#ifdef _WIN32
#define PYTHON3 L"python"
#define LAUNCHER_EXE L"launcher.exe"
#else
#define PYTHON3 L"python3"   // the system's: Set up's tools/venv need not exist yet
#define LAUNCHER_EXE L"launcher"
#endif

static std::wstring g_found_ver, g_found_notes;   // under g_mx: what the check found, for update_found

static void read_release() {
    for (const auto& l : read_lines(g_root / "launcher" / "out" / "release.txt"))
        if (!trim(widen(l)).empty()) {
            g_release = trim(widen(l));
            break;
        }
}

// tools/update.py check: a newer release's version and the start of its notes; no version when this one is the newest,
// or there was no answer (offline, no Python yet, GitHub's rate limit)
static std::pair<std::wstring, std::wstring> check_update() {
    DWORD code;
    const std::wstring out = widen(capture(PYTHON3 L" \"" + (g_root / "tools" / "update.py").wstring() + L"\" check", &code));
    if (code != 0) return {};
    std::wstring ver, notes;
    bool in_notes = false;
    for (size_t a = 0, b; a < out.size(); a = b + 1) {
        b = out.find(L'\n', a);
        if (b == std::wstring::npos) b = out.size();
        std::wstring l = out.substr(a, b - a);
        if (!l.empty() && l.back() == L'\r') l.pop_back();
        if (in_notes) notes += l + L"\n";
        else if (l.rfind(L"version ", 0) == 0) ver = trim(l.substr(8));
        else if (l == L"notes") in_notes = true;
    }
    return {ver, trim(notes)};
}

static void start_update_check() {
    std::wstring check = env_var(L"UPDATE_CHECK");
    if (check.empty()) check = local_cfg()[L"UPDATE_CHECK"];
    if (g_release.empty() || check == L"0") return;
    std::thread([] {
        auto [ver, notes] = check_update();
        if (ver.empty()) return;   // asked again next time
        {
            std::lock_guard lk(g_mx);
            g_found_ver = ver;
            g_found_notes = notes;
        }
#ifdef _WIN32
        PostMessageW(g_hwnd, WM_APP_UPDATE, 0, 0);
#else
        post_event(g_ev_update);
#endif
    }).detach();
}

static void start_update() {
    if (g_running || g_update_ver.empty()) return;
    Cmd c{PYTHON3 L" tools" DS L"update.py install " + g_update_ver, g_root.wstring(), L"Downloading"};
    c.stage_i = c.stage_n = 1;
    g_unpack_log = true;   // the Game step shows the output too
    start_job(L"Updating to " + g_update_ver, {c});
    g_updating = g_running;
}

static void offer_update() {
    if (g_running || g_update_ver.empty()) return;
    std::wstring t = L"Version " + g_update_ver + L" is out (you have " + g_release + L").";
    if (!g_update_notes.empty()) t += L"\n\n" + g_update_notes;
    t += L"\n\nUpdate now? Your build, saves and settings stay as they are.";
    if (msg_box(t, L"Update", M_ASK)) start_update();
    else save_setting(L"UPDATE_SKIP", g_update_ver);   // not asked by itself again for this one
    redraw();
}

static void update_found() {
    {
        std::lock_guard lk(g_mx);
        g_update_ver = g_found_ver;
        g_update_notes = g_found_notes;
    }
    g_update_ask = local_cfg()[L"UPDATE_SKIP"] != g_update_ver;
    redraw();
    maybe_offer_update();
}

// Asked once nothing else is going on: no job, no question open, no script
static void maybe_offer_update() {
    if (!g_update_ask || g_running || g_asking || g_scripting) return;
    g_update_ask = false;
    offer_update();
}

// This launcher again (the updated one), on its own: not a child that dies with this one
static bool restart() {
    for (const auto& [k, v] : g_env0) SetEnvironmentVariableW(k.c_str(), v.empty() ? nullptr : v.c_str());   // as it started
    const fs::path exe = g_root / "launcher" / "out" / LAUNCHER_EXE;
#ifdef _WIN32
    std::wstring cl = L"\"" + exe.wstring() + L"\"";
    STARTUPINFOW si{sizeof si};
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(exe.c_str(), cl.data(), nullptr, nullptr, FALSE, 0, nullptr, g_root.c_str(), &si, &pi)) return false;
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return true;
#else
    const std::string path = exe.string(), dir = g_root.string();
    int fd[2];
    if (pipe2(fd, O_CLOEXEC)) return false;
    const pid_t pid = fork();
    if (pid == 0) {
        setsid();
        const int nul = open("/dev/null", O_RDWR);
        dup2(nul, 0);
        dup2(nul, 1);
        dup2(nul, 2);
        if (chdir(dir.c_str()) == 0) execl(path.c_str(), path.c_str(), (char*)nullptr);
        const int err = errno;
        (void)!write(fd[1], &err, sizeof err);   // exec failed: the pipe says so (it closes unread on success)
        _exit(127);
    }
    close(fd[1]);
    int err = 0;
    ssize_t n = -1;
    if (pid > 0)
        while ((n = read(fd[0], &err, sizeof err)) < 0 && errno == EINTR) {}
    close(fd[0]);
    if (pid > 0) waitpid(pid, nullptr, WNOHANG);
    return pid > 0 && n == 0;
#endif
}

static void update_done(bool ok) {
    if (!ok) return;   // the status and output say why; the button at the top right tries again
    if (!restart()) {
        msg_box(L"Updated, but the launcher did not start again: close it and open it again.", L"Update", M_INFO);
        return;
    }
#ifdef _WIN32
    PostMessageW(g_hwnd, WM_CLOSE, 0, 0);
#else
    g_quit_asked = true;
#endif
}

// The files an update set aside (launcher/out/update-leftovers.txt), deleted in the background: the launcher that
// updated may take a moment to close and let go of its .exe
static void clean_leftovers() {
    const fs::path list = g_root / "launcher" / "out" / "update-leftovers.txt";
    if (!path_exists(list)) return;
    std::thread([list] {
        std::vector<std::string> left = read_lines(list);
        for (int tries = 0; tries < 20 && !left.empty(); tries++) {
            if (tries) std::this_thread::sleep_for(std::chrono::milliseconds(500));
            std::vector<std::string> still;
            for (const auto& l : left) {
                if (l.empty() || l.find("..") != std::string::npos) continue;
                std::error_code ec;
                const fs::path p = g_root / fs::path(widen(l));
                fs::remove(p, ec);
                if (ec && fs::exists(p)) still.push_back(l);
            }
            left = std::move(still);
        }
        std::error_code ec;
        if (left.empty()) fs::remove(list, ec);
        else if (std::ofstream o(list, std::ios::binary | std::ios::trunc); o)
            for (const auto& l : left) o << l << "\n";
    }).detach();
}

static void click(int id);
static void run_script() {
    while (!g_running) {
        if (g_script.empty() || g_exit_code) {
            log_line(g_exit_code ? L"script: failed" : L"script: done");
#ifdef _WIN32
            if (g_script_quit) PostMessageW(g_hwnd, WM_CLOSE, 0, 0);
#else
            if (g_script_quit) {
                SDL_Event e{};
                e.type = SDL_EVENT_QUIT;
                SDL_PushEvent(&e);
            }
#endif
            g_scripting = false;
            return;
        }
        const auto [what, arg] = g_script.front();
        g_script.pop_front();
        log_line(L"script: " + what + (arg.empty() ? L"" : L" " + arg));
        if (what == L"bring-over") {
            if (!bring_over_from(arg, false)) g_exit_code = 1;
        } else if (what == L"update") {
            // The newest release, without asking: the launcher starts again once it is in place
            auto [ver, notes] = g_release.empty() ? std::pair<std::wstring, std::wstring>{} : check_update();
            if (ver.empty()) {
                log_line(g_release.empty() ? L"script: no update: this folder is not a release (no launcher" DS L"out" DS L"release.txt)"
                                           : L"script: no newer release (or no answer from GitHub)");
            } else {
                g_update_ver = ver, g_update_notes = notes;
                start_update();
                if (!g_running) g_exit_code = 1;
            }
        } else if (what == L"setup") {
            refresh();
            if (g_done[1]) {
                log_line(L"script: set up already");
            } else if (!g_can_setup) {
                log_line(L"script: cannot set up: what is marked red on the Dependencies step is missing");
                g_exit_code = 1;
            } else {
                g_step = 1;
                click(B_SETUP);
            }
        } else if (what == L"build") {
            refresh();
            if (!g_can_build) {
                log_line(!g_done[0] ? L"script: cannot build: the game dump is not set or does not match"
                                    : L"script: cannot build: the dependencies are not set up");
                g_exit_code = 1;
            } else {
                g_step = 2;
                click(B_BUILD);
            }
        }
    }
}

static void click(int id) {
    const std::wstring root = g_root.wstring();
    if (id >= B_DOT) {
        g_step = id - B_DOT;
        refresh();
    } else if (id >= B_VER && id < B_VER + VERSION_COUNT) {
        select_version(id - B_VER);
    } else if (id == B_CC || id == B_CC + 1) {
        // Each compiler keeps its own module libraries (build\mingw\*.a, build\*.lib), so switching back costs
        // nothing; switching to one not built yet makes Build compile the game with it (the Build step shows that)
        if (!save_setting(L"BUILD_CC", id == B_CC ? L"mingw" : L"cl")) msg_box(L"Could not write " CFG_NAME L".", L"Launcher", M_ERROR);
        g_status.clear();
        refresh();
    } else if (id == B_BROWSE) {
        browse();
    } else if (id == B_NUP) {
        open_nup();
    } else if (id == B_IMPORT) {
        bring_over();
    } else if (id == B_UPDATE) {
        offer_update();
    } else if (id == B_CONTINUE) {
        g_step = std::min(3, g_step + 1);
        g_status.clear();
        refresh();
    } else if (id == B_SETUP) {
        // Python (python.org's build from NuGet, checked against its SHA-256) and the .NET SDK (dotnet-install.ps1)
        // when the machine has none, the C++ compiler (llvm-mingw, from GitHub, checked against its SHA-256), the
        // Python packages (pip, from PyPI), then the translator from the NuGet packages in tools\xlate\packages
        std::vector<Cmd> cmds;
#ifndef _WIN32
        // Linux: tools/linux.py setup, one part per step (clang from LLVM's release, a virtual environment with the
        // Python packages, .NET into tools/dotnet, the SPIR-V tools from the Vulkan SDK, the translator)
        const std::wstring setup = L"python3 tools/linux.py setup ";
        if (!llvm_ok()) cmds.push_back({setup + L"clang", root, L"C++ compiler"});
        if (g_tools.python.empty() || !g_tools.py_missing.empty()) cmds.push_back({setup + L"python", root, L"Python packages"});
        if (!g_tools.dotnet) cmds.push_back({setup + L"dotnet", root, L".NET SDK"});
        if (!spirv_tools()) cmds.push_back({setup + L"spirv", root, L"SPIR-V tools"});
        cmds.push_back({setup + L"xlate", root, L"Shader translator"});
#else
        const std::wstring bootstrap = L"powershell -NoProfile -ExecutionPolicy Bypass -File tools\\bootstrap.ps1 ";
        if (g_tools.python.empty()) cmds.push_back({bootstrap + L"python", root, L"Python"});
        if (!g_tools.dotnet) cmds.push_back({bootstrap + L"dotnet", root, L".NET SDK"});
        if (!mingw_ok() && !(build_cc_set() && build_cc() != L"mingw"))
            cmds.push_back({L"python tools\\toolchain.py install", root, L"C++ compiler"});
        if (g_tools.python.empty() || !g_tools.py_missing.empty()) cmds.push_back({L"python tools\\deps.py install", root, L"Python packages"});
        cmds.push_back({L"dotnet build -c Release -o \"" + setting(L"NVN_XLATE") + L"\" tools\\xlate\\dmsh_xlate\\dmsh_xlate.csproj",
                        root, L"Shader translator"});
#endif
        for (size_t i = 0; i < cmds.size(); i++) cmds[i].stage_i = (int)i + 1, cmds[i].stage_n = (int)cmds.size();
        start_job(L"Setting up", cmds);
    } else if (id == B_BUILD || id == B_REBUILD) {
        if (id == B_REBUILD && !msg_box(L"Unpack, lift and compile everything again? It takes about half an hour.", L"Rebuild everything", M_ASK))
            return;
        const auto st = stages();
        // Build: in each chain, from the first stage that is not done (its output missing, or its tools changed:
        // the later ones read what it writes), and the exe every time (it compiles only what changed). Rebuild
        // everything: every stage.
        std::vector<size_t> run;
        int started = -1;   // the chain that already has a stage to run
        for (size_t i = 0; i < st.size(); i++) {
            if (id == B_REBUILD || st[i].chain == 2 || started == st[i].chain || !st[i].done()) {
                run.push_back(i);
                started = st[i].chain;
            }
        }
        std::vector<Cmd> cmds;
        for (size_t k = 0; k < run.size(); k++) {
            auto cs = st[run[k]].cmds();
            for (auto& c : cs) {
                c.stage = st[run[k]].name;
                c.stage_i = (int)k + 1;
                c.stage_n = (int)run.size();
            }
            cs.back().after = [s = st[run[k]]] { s.stamp(); };
            cmds.insert(cmds.end(), cs.begin(), cs.end());
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
    maybe_offer_update();   // found while a question of this click was open
}

// ---------------------------------------------------------------------------------------------------------------
// Window

#ifdef _WIN32
static D2D1_POINT_2F to_dip(LPARAM lp) {
    return {GET_X_LPARAM(lp) * 96.f / g_dpi, GET_Y_LPARAM(lp) * 96.f / g_dpi};
}
#endif

static int hit_test(D2D1_POINT_2F p) {
    for (const auto& h : g_hits)
        if (p.x >= h.r.left && p.x < h.r.right && p.y >= h.r.top && p.y < h.r.bottom) return h.id;
    return B_NONE;
}

#ifdef _WIN32
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
    case WM_APP_UPDATE:
        update_found();
        return 0;
    case WM_CLOSE:
        if (g_running && MessageBoxW(hwnd, (g_job_name + L" is still running. Stop it and quit?").c_str(), L"Launcher",
                                     MB_YESNO | MB_ICONQUESTION) != IDYES)
            return 0;
        stop_job();   // the job objects close with this process and take what runs in them along
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(g_exit_code);
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

    for (const wchar_t* k : {L"PATH", L"KART_VER", L"BUILD_MP", L"DOTNET_ROOT", L"DOTNET_MULTILEVEL_LOOKUP"}) g_env0.push_back({k, env_var(k)});

    // A scripted run (g_script)
    {
        int argc = 0;
        LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        for (int i = 1; argv && i < argc; i++) {
            const std::wstring a = argv[i];
            if (a == L"--bring-over" && i + 1 < argc) g_script.push_back({L"bring-over", argv[++i]});
            else if (a == L"--setup") g_script.push_back({L"setup", L""});
            else if (a == L"--update") g_script.push_back({L"update", L""});
            else if (a == L"--build") g_script.push_back({L"build", L""});
            else if (a == L"--quit") g_script_quit = true;
            else if (a == L"--log" && i + 1 < argc) g_log_file = _wfopen(argv[++i], L"wb");
        }
        if (argv) LocalFree(argv);
        g_scripting = !g_script.empty() || g_script_quit;
    }

    // For the tools it runs: Python output as it comes, in UTF-8; builds on every core unless BUILD_MP says
    SetEnvironmentVariableW(L"PYTHONUNBUFFERED", L"1");
    SetEnvironmentVariableW(L"PYTHONIOENCODING", L"utf-8");
    SetEnvironmentVariableW(L"PYTHONUTF8", L"1");      // files in UTF-8 too, whatever the system code page (cp950: issue #7)
    SetEnvironmentVariableW(L"KART_PROGRESS", L"1");   // the tools' @@progress lines, drawn as a bar
    if (env_var(L"BUILD_MP").empty()) {
        std::wstring mp = local_cfg()[L"BUILD_MP"];
        if (mp.empty()) mp = std::to_wstring(std::max(1u, std::thread::hardware_concurrency()));
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

    read_release();
    clean_leftovers();
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
    if (g_scripting) run_script();
    else start_update_check();

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return (int)msg.wParam;
}
#else
// ---------------------------------------------------------------------------------------------------------------
// Linux: questions drawn over the window (Windows' message boxes), the desktop's file dialogs, the SDL window

struct Modal {
    std::wstring title, text;
    int kind;
    int answer = -1;   // 1 Yes (OK), 0 No; -1 while it is asked
};
static Modal* g_modal;                 // the question being asked
static bool g_waiting_dialog = false;  // a file dialog is open: the window only draws itself
static float g_wheel = 0;              // wheel steps not yet scrolled (touchpads send fractions)
static SDL_Cursor *g_cur_arrow, *g_cur_hand;
enum { B_YES = 40, B_NO = 41 };

// The question: the window dimmed, a panel with its title, text and buttons; only the buttons answer the pointer
static void paint_modal(D2D1_SIZE_F sz) {
    if (!g_modal && !g_waiting_dialog) return;
    g_hits.clear();
    g_alpha = 0.65f;
    fill_rect({0, 0, sz.width, sz.height}, col::bg);
    g_alpha = 1;
    if (!g_modal) return;
    const float pad = 28, w = std::min(540.f, sz.width - 64);
    const float th = measure(g_modal->text, F_BODY, w - 2 * pad, T_WRAP).height;
    const float h = pad + 26 + 12 + th + 28 + 40 + pad;
    const float x0 = (sz.width - w) / 2, y0 = std::max(24.f, (sz.height - h) / 2);
    const D2D1_RECT_F box{x0, y0, x0 + w, y0 + h};
    fill_round(box, R_PANEL, col::panel);
    stroke_round(box, R_PANEL, col::line_strong);
    float y = y0 + pad, tx = x0 + pad;
    if (g_modal->kind == M_ERROR || g_modal->kind == M_WARN) {
        fill_circle(tx + 5, y + 13, 4.5f, col::red);
        tx += 20;
    }
    text(g_modal->title, F_DOT, {tx, y, x0 + w - pad, y + 26}, col::text, T_VCENTER);
    y += 26 + 12;
    text(g_modal->text, F_BODY, {x0 + pad, y, x0 + w - pad, y + th + 4}, col::muted, T_WRAP);
    y += th + 28;
    const bool ask = g_modal->kind >= M_ASK;
    const std::wstring yes = ask ? L"Yes" : L"OK";
    const float bx = x0 + w - pad - button_width(yes);
    button(B_YES, yes, bx, y, true, true);
    if (ask) button(B_NO, L"No", bx - 12 - button_width(L"No"), y, false, true);
}

static D2D1_POINT_2F to_dip(float x, float y) {
    const float d = SDL_GetWindowPixelDensity(g_window);
    return {x * d / g_scale, y * d / g_scale};
}

static void set_cursor() { SDL_SetCursor(g_hover != B_NONE ? g_cur_hand : g_cur_arrow); }

static void handle_event(const SDL_Event& e) {
    if (e.type == g_ev_log) {
        g_log_posted = false;
        size_t added;
        {
            std::lock_guard lk(g_mx);
            added = g_log_added;
            g_log_added = 0;
        }
        if (g_log_scroll > 0) g_log_scroll += (int)added;   // scrolled back: the view stays where it is
        redraw();
        return;
    }
    if (e.type == g_ev_done) {
        job_done((DWORD)e.user.code, e.user.data1 != nullptr);
        return;
    }
    if (e.type == g_ev_update) {
        update_found();
        return;
    }
    switch (e.type) {
    case SDL_EVENT_QUIT:
    case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
        g_quit_asked = true;
        break;
    case SDL_EVENT_WINDOW_EXPOSED:
    case SDL_EVENT_WINDOW_RESIZED:
    case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
    case SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED:
        redraw();
        break;
    case SDL_EVENT_MOUSE_MOTION: {
        const int h = g_waiting_dialog ? B_NONE : hit_test(to_dip(e.motion.x, e.motion.y));
        if (h != g_hover) g_hover = h, redraw();
        set_cursor();
        break;
    }
    case SDL_EVENT_WINDOW_MOUSE_LEAVE:
        if (g_hover != B_NONE) g_hover = B_NONE, redraw();
        break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
        if (e.button.button == SDL_BUTTON_LEFT && !g_waiting_dialog) {
            g_pressed = hit_test(to_dip(e.button.x, e.button.y));
            SDL_CaptureMouse(true);
            redraw();
        }
        break;
    case SDL_EVENT_MOUSE_BUTTON_UP:
        if (e.button.button == SDL_BUTTON_LEFT) {
            const int h = g_waiting_dialog ? B_NONE : hit_test(to_dip(e.button.x, e.button.y)), p = g_pressed;
            g_pressed = B_NONE;
            SDL_CaptureMouse(false);
            if (h != B_NONE && h == p) {
                if (g_modal) g_modal->answer = h == B_YES ? 1 : 0;
                else click(h);
            }
            redraw();
        }
        break;
    case SDL_EVENT_MOUSE_WHEEL: {
        if (g_modal || g_waiting_dialog) break;
        const D2D1_POINT_2F p = to_dip(e.wheel.mouse_x, e.wheel.mouse_y);
        if (p.x >= g_log_rect.left && p.x < g_log_rect.right && p.y >= g_log_rect.top && p.y < g_log_rect.bottom) {
            g_wheel += e.wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -e.wheel.y : e.wheel.y;
            const int steps = (int)g_wheel;
            g_wheel -= (float)steps;
            g_log_scroll = std::max(0, g_log_scroll + steps * 3);
            redraw();
        }
        break;
    }
    case SDL_EVENT_KEY_DOWN:
        if (g_modal && (e.key.key == SDLK_RETURN || e.key.key == SDLK_KP_ENTER)) g_modal->answer = 1;
        else if (g_modal && e.key.key == SDLK_ESCAPE) g_modal->answer = g_modal->kind >= M_ASK ? 0 : 1;
        break;
    }
}

// Events handled and the window drawn until <done>: a question or a file dialog waits like this, as Windows' modal
// boxes do (a job's output keeps coming in meanwhile)
static void modal_loop(const std::function<bool()>& done) {
    while (!done()) {
        SDL_Event e;
        if (SDL_WaitEventTimeout(&e, g_moving ? 15 : 100))
            do handle_event(e);
            while (!done() && SDL_PollEvent(&e));
        if (g_quit_asked && g_modal) g_modal->answer = 0;   // closing the window answers No; the main loop then asks to quit
        if (g_running || g_moving) redraw();
        if (g_dirty) {
            g_dirty = false;
            paint();
        }
    }
}

static bool modal_box(const std::wstring& text, const wchar_t* title, int kind) {
    if (!g_window) {
        fprintf(stderr, "%s: %s\n", narrow(title).c_str(), narrow(text).c_str());
        return false;
    }
    Modal m{title, text, kind};
    Modal* outer = g_modal;
    g_modal = &m;
    g_hover = g_pressed = B_NONE;
    redraw();
    modal_loop([&] { return m.answer >= 0; });
    g_modal = outer;
    g_hover = B_NONE;
    redraw();
    return m.answer == 1;
}

// The desktop's file dialog (SDL's: the XDG portal, else zenity), waited for in modal_loop
struct Picked {
    std::mutex mx;
    bool done = false;
    std::string path, error;
};
static void SDLCALL picked(void* ud, const char* const* list, int) {
    auto* r = (Picked*)ud;
    {
        std::lock_guard lk(r->mx);
        if (!list) r->error = SDL_GetError();
        else if (list[0]) r->path = list[0];
        r->done = true;
    }
    post_log();   // wakes the loop
}

static std::wstring pick(bool folder, const wchar_t* title, const std::wstring& start) {
    Asking asking;
    static const SDL_DialogFileFilter nup[] = {{"NUP game container (*.nup)", "nup"}, {"All files", "*"}};
    const SDL_PropertiesID props = SDL_CreateProperties();
    const std::string t = narrow(title), loc = narrow(start) + "/";
    SDL_SetStringProperty(props, SDL_PROP_FILE_DIALOG_TITLE_STRING, t.c_str());
    SDL_SetPointerProperty(props, SDL_PROP_FILE_DIALOG_WINDOW_POINTER, g_window);
    if (!start.empty() && fs::is_directory(start)) SDL_SetStringProperty(props, SDL_PROP_FILE_DIALOG_LOCATION_STRING, loc.c_str());
    if (!folder) {
        SDL_SetPointerProperty(props, SDL_PROP_FILE_DIALOG_FILTERS_POINTER, (void*)nup);
        SDL_SetNumberProperty(props, SDL_PROP_FILE_DIALOG_NFILTERS_NUMBER, 2);
    }
    Picked r;
    g_waiting_dialog = true;
    g_hover = g_pressed = B_NONE;
    set_cursor();
    redraw();
    SDL_ShowFileDialogWithProperties(folder ? SDL_FILEDIALOG_OPENFOLDER : SDL_FILEDIALOG_OPENFILE, picked, &r, props);
    modal_loop([&] {
        std::lock_guard lk(r.mx);
        return r.done;
    });
    SDL_DestroyProperties(props);
    g_waiting_dialog = false;
    redraw();
    if (!r.error.empty())
        msg_box(L"No file dialog could open (" + widen(r.error) + L"). Install xdg-desktop-portal (with your desktop's portal) or "
                L"zenity, or set the folder in " CFG_NAME L" or with ./kart.sh game <folder>.", L"Launcher", M_ERROR);
    return widen(r.path);
}

int main(int argc, char** argv) {
    // The repository: the folder above holding tools/paths.py (the program is launcher/out/launcher)
    std::error_code ec;
    for (fs::path d = fs::read_symlink("/proc/self/exe", ec).parent_path();; d = d.parent_path()) {
        if (path_exists(d / "tools" / "paths.py")) {
            g_root = d;
            break;
        }
        if (d.empty() || d == d.parent_path()) {
            fprintf(stderr, "Run the launcher from inside the repository (./kart.sh, or launcher/out/launcher).\n");
            return 1;
        }
    }
    signal(SIGPIPE, SIG_IGN);

    for (const wchar_t* k : {L"PATH", L"KART_VER", L"BUILD_MP", L"DOTNET_ROOT", L"DOTNET_MULTILEVEL_LOOKUP"}) g_env0.push_back({k, env_var(k)});

    // A scripted run (g_script)
    for (int i = 1; i < argc; i++) {
        const std::string a = argv[i];
        if (a == "--bring-over" && i + 1 < argc) g_script.push_back({L"bring-over", widen(argv[++i])});
        else if (a == "--setup") g_script.push_back({L"setup", L""});
        else if (a == "--update") g_script.push_back({L"update", L""});
        else if (a == "--build") g_script.push_back({L"build", L""});
        else if (a == "--quit") g_script_quit = true;
        else if (a == "--log" && i + 1 < argc) g_log_file = fopen(argv[++i], "wb");
    }
    g_scripting = !g_script.empty() || g_script_quit;

    // For the tools it runs: Python output as it comes, in UTF-8; builds on every core unless BUILD_MP says
    setenv("PYTHONUNBUFFERED", "1", 1);
    setenv("PYTHONIOENCODING", "utf-8", 1);
    setenv("PYTHONUTF8", "1", 1);
    setenv("KART_PROGRESS", "1", 1);   // the tools' @@progress lines, drawn as a bar
    if (env_var(L"BUILD_MP").empty()) {
        std::wstring mp = local_cfg()[L"BUILD_MP"];
        if (mp.empty()) mp = std::to_wstring(std::max(1u, std::thread::hardware_concurrency()));
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

    read_release();
    clean_leftovers();
    check_tools();
    refresh();
    g_step = 3;
    for (int i = 0; i < 4; i++)
        if (!g_done[i]) {
            g_step = i;
            break;
        }
    // The desktop's "animations" switch (GNOME's; off: every state is reached at once)
    g_motion = trim(widen(capture(L"gsettings get org.gnome.desktop.interface enable-animations 2>/dev/null"))) != L"false";

    SDL_SetHint(SDL_HINT_APP_ID, "kart-recomp");
    SDL_SetHint(SDL_HINT_VIDEO_ALLOW_SCREENSAVER, "1");
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        fprintf(stderr, "launcher: no window (%s)\n", SDL_GetError());
        return 1;
    }
    g_ev_log = SDL_RegisterEvents(3);
    g_ev_done = g_ev_log + 1;
    g_ev_update = g_ev_log + 2;
    g_window = SDL_CreateWindow("kart-recomp", 900, 780, SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_HIDDEN);
    if (!g_window || !(g_ren = SDL_CreateRenderer(g_window, nullptr))) {
        fprintf(stderr, "launcher: no window (%s)\n", SDL_GetError());
        return 1;
    }
    SDL_SetRenderVSync(g_ren, 1);
    // 900 x 780 DIPs, or less on a small screen (window units are DIPs times the display's scale over the pixel density:
    // 1 on Wayland, the scale on X11)
    {
        const float k = SDL_GetWindowDisplayScale(g_window) / std::max(0.01f, SDL_GetWindowPixelDensity(g_window));
        int w = (int)(900 * k), h = (int)(780 * k);
        SDL_Rect work{};
        if (SDL_GetDisplayUsableBounds(SDL_GetDisplayForWindow(g_window), &work) && work.w > 0) {
            w = std::min(w, (int)(work.w * 0.95f));
            h = std::min(h, (int)(work.h * 0.95f));
        }
        SDL_SetWindowMinimumSize(g_window, std::min(w, (int)(760 * k)), std::min(h, (int)(660 * k)));
        SDL_SetWindowSize(g_window, w, h);
        SDL_SetWindowPosition(g_window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
    }
    create_formats();
    g_cur_arrow = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_DEFAULT);
    g_cur_hand = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_POINTER);
    SDL_ShowWindow(g_window);
    if (g_scripting) run_script();
    else start_update_check();

    // Events; a frame once they are handled when something changed, every 15 ms while something moves, and every
    // half second while a job runs (its clock)
    ULONGLONG tick = 0;
    for (;;) {
        SDL_Event e;
        if (SDL_WaitEventTimeout(&e, g_dirty ? 0 : g_moving ? 15 : g_running ? 500 : -1))
            do handle_event(e);
            while (SDL_PollEvent(&e));
        if (g_quit_asked) {
            g_quit_asked = false;
            if (!g_running || msg_box(g_job_name + L" is still running. Stop it and quit?", L"Launcher", M_ASK)) {
                stop_job();   // its process group, and with this process what it started
                break;
            }
        }
        if (g_moving) redraw();
        if (g_running && GetTickCount64() - tick >= 500) {
            tick = GetTickCount64();
            redraw();
        }
        if (g_dirty) {
            g_dirty = false;
            paint();
        }
    }
    {
        std::lock_guard lk(g_mx);
        if (g_log_file) fclose(g_log_file);
        g_log_file = nullptr;
    }
    SDL_Quit();
    _exit(g_exit_code);   // not through the static destructors: a stopped job's thread may still be finishing
}
#endif
