
// The game window (video.cpp on Windows, video_sdl.cpp on Linux): the renderer (nvn/vk) presents into it.
#pragma once
#include <vector>

#include "common.h"

#ifdef _WIN32
struct HWND__;
#endif

namespace video {

// Creates the game window.
void init();
#ifdef _WIN32
// The game window (created by init).
::HWND__* main_window();
#else
// Linux (SDL 3): SDL's Vulkan loader (its vkGetInstanceProcAddr, nullptr when there is no libvulkan.so.1), the instance
// extensions a surface on the window needs, and that surface (a VkSurfaceKHR; nullptr when it fails, `why` says why).
void* vk_get_instance_proc_addr();
std::vector<const char*> vk_instance_extensions();
void* create_vk_surface(void* instance, const char** why);
// The keyboard while the window has the focus (hid.cpp): whether the key with that SDL scancode is down.
bool focused();
bool key_down(int scancode);
// The window's drawable size in pixels (0 x 0 while minimised). A Wayland surface leaves the swapchain's size to it.
void window_pixel_size(u32* w, u32* h);
// A message box for a fatal error (with or without the window).
void error_box(const char* title, const char* text);
#endif
// Runs the window's message loop on the calling thread until the window closes.
void run_window_loop();

// Present statistics for the overlay in the window's corner (vi2 reports them twice a second):
// frames per second, mean time between presents, mean wait for the frame's GPU fence.
void report_frame_stats(double fps, double frame_ms, double gpu_wait_ms);
// When report_frame_stats was last called (host_ns; 0 = never): frames are being shown while it advances.
unsigned long long last_frame_ns();

}  // namespace video
