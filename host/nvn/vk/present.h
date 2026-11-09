// The window's swapchain: the game's presented texture is scaled into it.
#pragma once
#include "vkctx.h"

#include <atomic>

namespace nvk {

extern std::atomic<u64> g_presents;   // presents so far: the capture number of the frame being recorded

// Creates the surface and swapchain on the game window (video::main_window), once.
void present_init();

// Shows `image` (VK_IMAGE_LAYOUT_GENERAL, colour, w x h; crop = x, y, w, h of it, or null for all)
// once the GPU timeline (timeline()) reaches `ready`. Letterboxed to the window's aspect. Returns
// without waiting for the GPU; blocks while two presents are already in flight.
void present(VkImage image, VkFormat format, u32 w, u32 h, const int* crop, u64 ready);

// The renderer's timeline semaphore: every submission signals the next value.
VkSemaphore timeline();

}  // namespace nvk
