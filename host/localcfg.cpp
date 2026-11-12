// local.cfg at the repository root (host\out\kart.exe -> ..\..\local.cfg): its KART_GAME and HOST_* lines become
// environment variables of this process, unless the environment already has them, as the build tools read it
// (tools\paths.py). Done before every other static initializer (init_seg(lib)), since some read HOST_* settings
// while the program starts.
// Another game version's exe, kart-<version>.exe (tools\paths.py KART_VER), takes its dump from KART_GAME_<version>.
#include <windows.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#pragma warning(disable : 4073)   // initializers in the library segment: intended
#pragma init_seg(lib)

namespace {

std::string trim(const std::string& s) {
    const size_t a = s.find_first_not_of(" \t\r\""), b = s.find_last_not_of(" \t\r\"");
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

// "1.8.0" for kart-1.8.0.exe, "" for kart.exe.
std::string variant_of(const std::filesystem::path& exe) {
    const std::string stem = exe.stem().string();
    const size_t dash = stem.find('-');
    return dash == std::string::npos ? std::string() : stem.substr(dash + 1);
}

struct LocalCfg {
    LocalCfg() {
        namespace fs = std::filesystem;
        wchar_t exe[MAX_PATH];
        const DWORD n = GetModuleFileNameW(nullptr, exe, MAX_PATH);
        const fs::path root = fs::path(std::wstring(exe, n)).parent_path().parent_path().parent_path();
        const std::string game_key = variant_of(fs::path(std::wstring(exe, n))).empty()
                                         ? "KART_GAME" : "KART_GAME_" + variant_of(fs::path(std::wstring(exe, n)));
        std::ifstream cfg(root / "local.cfg");
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
            if (const char* have = std::getenv(key.c_str()); have && *have) continue;
            if (key == "KART_GAME" && !value.empty() && fs::u8path(value).is_relative()) value = (root / fs::u8path(value)).string();
            _putenv_s(key.c_str(), value.c_str());
        }
    }
} g_local_cfg;

}  // namespace

// The exe's game version, "" for the default build (main.cpp: user data of another version goes to user-<version>).
std::string host_exe_variant() {
    wchar_t exe[MAX_PATH];
    const DWORD n = GetModuleFileNameW(nullptr, exe, MAX_PATH);
    return variant_of(std::filesystem::path(std::wstring(exe, n)));
}
