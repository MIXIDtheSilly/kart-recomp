// Shared between the Vulkan replay's record walker (replay_vk.cpp) and its draw path (draw.cpp).
#pragma once
#include <atomic>

#include "textures.h"

namespace nvk {

// The render targets of the submission being recorded (OP_SET_RT) and its open rendering scope.
struct Target {
    Image* img = nullptr;
    VkImageView view = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;  // of the view
    u32 layer = 0;                          // first layer (3D: slice) of the view
    u32 level = 0;
};
struct RenderState {
    Target color[8];
    int ncolor = 0;
    Target depth;
    bool active = false;  // vkCmdBeginRendering is open
    VkExtent2D extent{};
    u32 layers = 1;
    VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
};
extern RenderState R;

void begin_rendering(VkCommandBuffer cb);
void end_rendering(VkCommandBuffer cb);  // also a full barrier when a scope was open
void full_barrier(VkCommandBuffer cb);

// Device conventions (rp_set_origin_lower_left, rp_set_queue_origin_lower_left, rp_set_depth_zero).
extern std::atomic<int> g_origin_ll, g_queue_origin_ll, g_depth_zero;

}  // namespace nvk
