// The game window (video.cpp): the renderer (nvn/vk) presents into it.
#pragma once
#include "common.h"

struct HWND__;

namespace video {

// Creates the game window.
void init();
// The game window (created by init).
::HWND__* main_window();
// Runs the window's message loop on the calling thread until the window closes.
void run_window_loop();

// Present statistics for the overlay in the window's corner (vi2 reports them twice a second):
// frames per second, mean time between presents, mean wait for the frame's GPU fence.
void report_frame_stats(double fps, double frame_ms, double gpu_wait_ms);
// When report_frame_stats was last called (host_ns; 0 = never): frames are being shown while it advances.
unsigned long long last_frame_ns();

}  // namespace video
