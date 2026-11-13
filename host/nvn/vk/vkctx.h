// The native NVN renderer's Vulkan device: instance, the GPU, one queue, the window's swapchain, and
// guest memory imported as GPU buffers.
#pragma once
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include "../../common.h"
#include "vkfn.h"
#include "vma.h"

namespace nvk {

[[noreturn]] void vk_fail(VkResult r, const char* what);
inline void check(VkResult r, const char* what) {
    if (r != VK_SUCCESS) vk_fail(r, what);
}

struct Features {
    bool mesh_shader = false;
    bool eds3 = false;              // VK_EXT_extended_dynamic_state3 (polygon mode, logic op, blend state...)
    bool vertex_input_dynamic = false;
    bool conditional_rendering = false;
    bool host_import = false;       // VK_EXT_external_memory_host
    bool index_uint8 = false;
    bool clip_control = false;      // dynamic negative-one-to-one depth (GL clip space)
    bool clip_enable = false;       // dynamic depth clip enable
    bool sparse = false;            // sparse residency 2D images, bound on the queue (vkQueueBindSparse)
    bool sparse_strict = false;     // unbound parts of a sparse image read as 0
    bool device_fault = false;      // VK_EXT_device_fault: what the GPU tripped over, logged at device loss
    bool null_descriptor = false;   // robustness2 nullDescriptor: a texture with no image may be bound as a null view
    bool checkpoints = false;       // HOST_NVN_CHECKPOINTS: VK_NV_device_diagnostic_checkpoints markers per draw
    // Unified memory (an integrated GPU; HOST_NVN_UMA=0/1 overrides): system memory is the GPU's own, so guest memory is
    // used in place (no device copies of pools), and memory pressure counts every heap, the device-local one being only
    // the firmware's carve-out.
    bool uma = false;
    u64 import_alignment = 4096;
};

struct Device {
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice phys = VK_NULL_HANDLE;
    VkPhysicalDeviceProperties props{};
    VkPhysicalDeviceMemoryProperties mem_props{};
    VkDevice dev = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    u32 family = 0;
    std::mutex queue_lock;  // vkQueueSubmit / vkQueuePresentKHR
    VmaAllocator vma = VK_NULL_HANDLE;
    Features feat;
};
Device& ctx();
// Device memory in use and the budget for it (VK_EXT_memory_budget): the device-local heaps, or every heap on unified
// memory. `spare`: free space inside VMA's blocks (what is released stays in them for the next allocation).
void device_budget(u64& used, u64& budget, u64* spare = nullptr);
// The memory property a GPU-only allocation requires: device-local, or nothing on unified memory (where the
// device-local heap is the small carve-out and system memory is as fast).
VkMemoryPropertyFlags gpu_only_flags();

// Creates the instance and device (once). Logs the GPU and the features found.
void init();
// The draw a checkpoint marker stands for (draw.cpp).
std::string checkpoint_desc(u64 marker);
std::string checkpoint_between(u64 lo, u64 hi);

// ------------------------------------------------------------------ guest memory as GPU buffers

// [addr, addr + size) of guest memory, imported (VK_EXT_external_memory_host) as one buffer that the
// GPU reads and writes in place: CPU and GPU see the same bytes, as on the console. Over PCIe: for
// memory the GPU touches lightly. The range is widened to the import alignment.
struct Imported {
    u64 addr = 0, size = 0;  // the widened guest range
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceAddress gpu = 0;
};
bool import_guest(u64 addr, u64 size, Imported& out, const char** why = nullptr);
// Host memory [host, host + size) (aligned to the import alignment) as one buffer; out.addr is left 0.
bool import_host(void* host, u64 size, Imported& out, const char** why = nullptr);
void release_import(Imported& im);
// Bytes imported for the GPU now (on unified memory they count against the GPU's heap).
u64 imported_bytes();

// ------------------------------------------------------------------ deferred destruction

// Runs fn (on the completion thread) once the GPU has finished everything submitted so far: for objects
// the game released that a submission in flight may still use (replay_vk.cpp).
void defer(std::function<void()> fn);
// defer, for images and views (`bytes` of memory): also not before the descriptor sets kept across submissions that may
// hold them are gone (draw.cpp, keep_find), so a view's handle is never reused while one may still be found.
void defer_past_kept(std::function<void()> fn, u64 bytes);

// ------------------------------------------------------------------ one-off work

// A command buffer for work outside the game's submissions (uploads at texture creation...), run
// and waited for at once.
VkCommandBuffer begin_oneoff();
void end_oneoff(VkCommandBuffer cb);

}  // namespace nvk
