// local.cfg at the repository root (host\out\kart.exe -> ..\..\local.cfg): its KART_GAME and HOST_* lines become
// environment variables of this process, unless the environment already has them, as the build tools read it
// (tools\paths.py). Done before every other static initializer (init_seg(lib)), since some read HOST_* settings
// while the program starts.
// Another game version's exe, kart-<version>.exe (tools\paths.py KART_VER), takes its dump from KART_GAME_<version>.
#ifdef _WIN32
#include <windows.h>
#endif

#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <type_traits>

#include "os.h"

#ifdef _WIN32
#pragma warning(disable : 4073)   // initializers in the library segment: intended
#pragma init_seg(lib)
#endif

namespace {

std::string trim(const std::string& s) {
    const size_t a = s.find_first_not_of(" \t\r\""), b = s.find_last_not_of(" \t\r\"");
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

// "1.8.0" for kart-1.8.0.exe (Linux: kart-1.8.0), "" for kart.exe.
std::string variant_of(const std::filesystem::path& exe) {
    std::string stem = exe.filename().string();   // (not stem(): on Linux there is no .exe, and stem would take the ".0")
    if (stem.size() > 4 && stem.compare(stem.size() - 4, 4, ".exe") == 0) stem.resize(stem.size() - 4);
    const size_t dash = stem.find('-');
    return dash == std::string::npos ? std::string() : stem.substr(dash + 1);
}

// On Linux local.linux.cfg comes first, and local.cfg values that are Windows paths (C:\...) are skipped: one checkout
// can serve both systems (tools/paths.py reads them the same way).
bool windows_path(const std::string& v) {
    return v.size() >= 3 && std::isalpha(static_cast<unsigned char>(v[0])) && v[1] == ':' && (v[2] == '\\' || v[2] == '/');
}

struct LocalCfg {
    LocalCfg() {
        namespace fs = std::filesystem;
        const fs::path exe = os::exe_path();
        const fs::path root = exe.parent_path().parent_path().parent_path();
        const std::string game_key = variant_of(exe).empty() ? "KART_GAME" : "KART_GAME_" + variant_of(exe);
#ifdef _WIN32
        const char* const files[] = {"local.cfg"};
#else
        const char* const files[] = {"local.linux.cfg", "local.cfg"};
#endif
        for (const char* name : files) {
            const bool skip_windows_paths = !std::is_same_v<fs::path::value_type, wchar_t> && std::string(name) == "local.cfg";
            std::ifstream cfg(root / name);
            bool first = true;
            for (std::string line; std::getline(cfg, line); first = false) {
                if (first && line.rfind("\xEF\xBB\xBF", 0) == 0) line.erase(0, 3);
                line = line.substr(0, line.find('#'));
                const size_t eq = line.find('=');
                if (eq == std::string::npos) continue;
                std::string key = trim(line.substr(0, eq));
                if (key.rfind("KART_GAME", 0) == 0) {
                    if (key != game_key) continue;
                    key = "KART_GAME";
                }
                std::string value = trim(line.substr(eq + 1));
                if (key != "KART_GAME" && key.rfind("HOST_", 0) != 0) continue;
                if (skip_windows_paths && windows_path(value)) continue;
                if (const char* have = std::getenv(key.c_str()); have && *have) continue;
                if (key == "KART_GAME" && !value.empty() && fs::u8path(value).is_relative()) value = (root / fs::u8path(value)).string();
                os::set_env(key.c_str(), value.c_str());
            }
        }
    }
};
#ifdef _WIN32
LocalCfg g_local_cfg;
#else
LocalCfg g_local_cfg __attribute__((init_priority(101)));   // (before the other static initializers, as init_seg(lib) does above)
#endif

}  // namespace

// The exe's game version, "" for the default build (main.cpp: user data of another version goes to user-<version>).
std::string host_exe_variant() { return variant_of(os::exe_path()); }
