// The game window on Linux, through SDL 3 (Wayland or X11, whichever SDL picks): created by init, its events handled by
// run_window_loop on the main thread, which also keeps SDL's keyboard and gamepad state current (hid.cpp and
// gamepads_sdl.cpp read it from their own threads). The renderer (nvn/vk) puts its swapchain on the surface it makes
// through SDL's Vulkan loader. The performance overlay (F3) is the window title here: Wayland has no windows layered
// over another program's.
#ifndef _WIN32
#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>

#include "os.h"
#include "services/gamepads.h"
#include "video.h"

namespace video {
namespace {

struct FrameStats {
    double fps = 0, frame_ms = 0, gpu_ms = 0;
    bool valid = false;
};
std::mutex g_stats_lock;
FrameStats g_stats;
SDL_Window* g_window = nullptr;
std::atomic<bool> g_focused{false};
const bool* g_keys = nullptr;   // SDL's keyboard state, by scancode
int g_nkeys = 0;
bool g_overlay_on = std::getenv("HOST_NO_OVERLAY") == nullptr;
std::atomic<u64> g_last_frame_ns{0};
std::atomic<u64> g_pixel_size{0};   // width << 32 | height, kept by the event loop

void note_pixel_size() {
    int w = 0, h = 0;
    if (!SDL_GetWindowSizeInPixels(g_window, &w, &h) || (SDL_GetWindowFlags(g_window) & SDL_WINDOW_MINIMIZED)) w = h = 0;
    g_pixel_size = static_cast<u64>(w) << 32 | static_cast<u32>(h);
}

// The process's memory with each guest page counted once: the guest view and the pool's backing view map the same pages,
// which the resident set counts twice. The proportional set size (smaps_rollup) splits a page between its mappings.
// Read every 2 s on a thread of its own (it walks every mapping).
std::atomic<u64> g_ram_bytes{0};
void start_ram_count() {
    std::thread([] {
        os::set_thread_name("overlay RAM count");
        os::set_thread_priority(os::Priority::Lowest);
        for (;; std::this_thread::sleep_for(std::chrono::seconds(2))) {
            if (!g_overlay_on) continue;
            std::ifstream f("/proc/self/smaps_rollup");
            for (std::string line; std::getline(f, line);)
                if (line.rfind("Pss:", 0) == 0) {
                    g_ram_bytes = std::strtoull(line.c_str() + 4, nullptr, 10) << 10;
                    break;
                }
        }
    }).detach();
}

const char* const BaseTitle = "Kart (native)";

void update_title() {
    if (!g_window) return;
    if (!g_overlay_on) {
        SDL_SetWindowTitle(g_window, BaseTitle);
        return;
    }
    FrameStats s;
    {
        std::scoped_lock lk{g_stats_lock};
        s = g_stats;
    }
    char title[160];
    if (s.valid)
        std::snprintf(title, sizeof title, "%s  |  %.1f FPS  %.1f ms  |  GPU wait %.1f ms  |  RAM %.1f GB", BaseTitle, s.fps, s.frame_ms,
                      s.gpu_ms, g_ram_bytes.load() / 1073741824.0);
    else
        std::snprintf(title, sizeof title, "%s  |  -- FPS  |  RAM %.1f GB", BaseTitle, g_ram_bytes.load() / 1073741824.0);
    SDL_SetWindowTitle(g_window, title);
}

}  // namespace

void init() {
    // The gamepads' events come through the same loop; SDL's own dialogs and screensaver handling stay out of the way.
    SDL_SetHint(SDL_HINT_VIDEO_ALLOW_SCREENSAVER, "0");
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");   // pads keep working while another window has the focus
    SDL_SetHint(SDL_HINT_APP_NAME, "Kart");
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) fatal("SDL could not start: %s", SDL_GetError());
    // Sound (audio_core/sink/sdl_sink.cpp): without an output device the game runs silent. Periods of 10 ms, as WASAPI's.
    SDL_SetHint(SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES, "480");
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) LOG("audio: SDL has no sound output (%s)", SDL_GetError());
    if (!SDL_Vulkan_LoadLibrary(nullptr)) fatal("no Vulkan loader (libvulkan.so.1): %s. Install your distribution's vulkan-loader package", SDL_GetError());
    const bool hidden = std::getenv("HOST_RENDER_HIDDEN") != nullptr;
    g_window = SDL_CreateWindow(BaseTitle, 1920, 1080, SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | (hidden ? SDL_WINDOW_HIDDEN : 0));
    if (!g_window) fatal("SDL could not create the window: %s", SDL_GetError());
    g_keys = SDL_GetKeyboardState(&g_nkeys);
    note_pixel_size();
    LOG("window: SDL %d.%d.%d, video driver %s", SDL_VERSIONNUM_MAJOR(SDL_GetVersion()), SDL_VERSIONNUM_MINOR(SDL_GetVersion()),
        SDL_VERSIONNUM_MICRO(SDL_GetVersion()), SDL_GetCurrentVideoDriver());
    start_ram_count();
}

void* vk_get_instance_proc_addr() { return reinterpret_cast<void*>(SDL_Vulkan_GetVkGetInstanceProcAddr()); }

std::vector<const char*> vk_instance_extensions() {
    Uint32 n = 0;
    const char* const* names = SDL_Vulkan_GetInstanceExtensions(&n);
    return names ? std::vector<const char*>(names, names + n) : std::vector<const char*>{};
}

void* create_vk_surface(void* instance, const char** why) {
    VkSurfaceKHR surface = 0;   // (SDL_vulkan.h declares the handle types without vulkan.h)
    if (!SDL_Vulkan_CreateSurface(g_window, static_cast<VkInstance>(instance), nullptr, &surface)) {
        if (why) *why = SDL_GetError();
        return nullptr;
    }
    return reinterpret_cast<void*>(surface);
}

void window_pixel_size(u32* w, u32* h) {
    const u64 s = g_pixel_size.load(std::memory_order_relaxed);
    *w = static_cast<u32>(s >> 32);
    *h = static_cast<u32>(s);
}

void error_box(const char* title, const char* text) { SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, title, text, g_window); }

bool focused() { return g_focused.load(std::memory_order_relaxed); }

bool key_down(int scancode) { return g_keys && scancode > 0 && scancode < g_nkeys && g_keys[scancode]; }

void report_frame_stats(double fps, double frame_ms, double gpu_wait_ms) {
    g_last_frame_ns.store(host_ns(), std::memory_order_relaxed);
    std::scoped_lock lk{g_stats_lock};
    g_stats = {fps, frame_ms, gpu_wait_ms, true};
}

unsigned long long last_frame_ns() { return g_last_frame_ns.load(std::memory_order_relaxed); }

void run_window_loop() {
    bool fullscreen = false;
    u64 title_at = 0;
    for (;;) {
        // A short wait: the loop also pumps SDL's gamepad state, which gamepads_sdl.cpp reads every 4 ms
        SDL_Event e;
        if (SDL_WaitEventTimeout(&e, 4)) {
            do {
                switch (e.type) {
                case SDL_EVENT_QUIT:
                case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
                    LOG("window closed");
                    gamepads::stop_vibration();
                    std::fflush(nullptr);
                    _exit(0);   // (not exit(): the guest threads and the Vulkan driver's would run destructors under them)
                case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
                case SDL_EVENT_WINDOW_MINIMIZED:
                case SDL_EVENT_WINDOW_RESTORED: note_pixel_size(); break;
                case SDL_EVENT_WINDOW_FOCUS_GAINED: g_focused = true; break;
                case SDL_EVENT_WINDOW_FOCUS_LOST: g_focused = false; break;
                case SDL_EVENT_KEY_DOWN:
                    if (e.key.repeat) break;
                    if (e.key.scancode == SDL_SCANCODE_F3) {
                        g_overlay_on = !g_overlay_on;
                        update_title();
                    } else if (e.key.scancode == SDL_SCANCODE_F11) {
                        // Borderless fullscreen on the window's display, and back (present.cpp recreates the swapchain)
                        fullscreen = !fullscreen;
                        SDL_SetWindowFullscreen(g_window, fullscreen);
                        LOG("window: %s", fullscreen ? "fullscreen" : "windowed");
                    }
                    break;
                default: break;
                }
                gamepads::on_sdl_event(&e);
            } while (SDL_PollEvent(&e));
        }
        const u64 now = host_ns();
        if (now - title_at >= 250'000'000ull) {
            title_at = now;
            update_title();
        }
    }
}

}  // namespace video
#endif
