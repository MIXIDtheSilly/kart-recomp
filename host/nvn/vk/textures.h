// NVN textures as Vulkan images: one image per texture serial (tex_t::serial), created at its first
// GPU use, always in VK_IMAGE_LAYOUT_GENERAL; views by NVN view.
#pragma once
#include <atomic>
#include <memory>
#include <unordered_map>

#include "../nvn_obj.h"
#include "format.h"
#include "vkctx.h"

namespace nvk {

struct Image {
    u64 serial = 0;
    VkImage image = VK_NULL_HANDLE;
    VmaAllocation alloc = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkImageAspectFlags aspect = 0;
    VkImageType type = VK_IMAGE_TYPE_2D;
    u32 width = 1, height = 1, depth = 1, layers = 1, levels = 1;
    VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
    bool cube_ok = false;
    bool storage_ok = false;
    bool mutable_ok = true;     // views may take other formats (false: the driver refused MUTABLE_FORMAT for it)
    nvn_texdesc desc{};
    u32 nvn_format = 0;
    bool gpu_written = false;   // rendered to / written by shaders or copies: guest storage is no longer its content
    bool uploaded = false;      // content taken from guest storage once
    u32 upload_count = 0;       // uploads from storage so far (statistics)
    bool sysmem = false;        // its memory is in system memory (VRAM was full when it was made)
    bool has_content = false;   // an upload from storage was recorded (its content is defined)
    std::shared_ptr<struct AsyncUpload> pending;  // an upload being prepared on the worker thread (upload.h)
    // Texture objects over the same storage and shape share the image (textures.cpp): refs counts them. parked: when the
    // last one went (host ns), a GPU-made image is kept a while for the next texture object over its storage (0 = in use).
    int refs = 1;
    u64 parked = 0;
    u64 shape = 0;              // its key in the shared table (0 = not shared)
    u64 mem = 0;                // storage (guest address)
    std::unordered_map<u64, VkImageView> views;
    struct SparseImage* sparse = nullptr;  // a sparse-residency image (sparse.h), shared by the textures over its storage
    u64 last_use = 0;           // frame (nvn::g_frame) it was last viewed for a draw, copy or target
    std::shared_ptr<struct CpuWatch> cpu_watch;  // storage the CPU writes unannounced (a linear texture): see cpu_writes_poll
};

// The image of texture t, created when first asked for; `cb` gets the layout transition of a new image
// (the command buffer it is used in next). Null for textures the renderer cannot make (logged once).
Image* image_of(const nvn::tex_t* t, VkCommandBuffer cb);
Image* image_find(u64 serial);
void image_release(u64 serial);
// Once a submission (after it began, renderer thread): when device memory is short, the textures not used for
// HOST_NVN_EVICT_IMAGES_S seconds (default 30, 0 = never) go; their next use makes them again from guest storage.
void image_evict_cold();
// A memory pool [lo, hi) is gone: parked images over it go.
void image_pool_gone(u64 lo, u64 hi);
// Linear textures in CPU-accessible memory are written in place by the CPU, announced by nothing (the game's font
// cache: an R8 1024x8192 linear texture in CPU-uncached memory, glyphs added as text needs them; uploaded once, the
// text lacked every glyph added after that). A thread hashes the storage of those drawn lately, once a frame at most;
// this, once a submission (renderer thread), has the ones it found changed uploaded again. True if any.
bool cpu_writes_poll();

// Changes whenever a guest address or texture content may resolve to something else than before (textures written by
// the CPU, pools made or gone, sparse tiles remapped): draw.cpp keeps descriptor sets made under one value. (Texture and
// sampler handles have a generation each, draw.cpp.)
inline std::atomic<u64> g_bind_epoch{0};
// Changes only when something a descriptor may hold goes or is replaced (pools, their device copies, images), not for
// content (`objects` false: textures written by the CPU, sparse tiles remapped): descriptor sets kept across
// submissions resolve their buffers and views again before use (draw.cpp keep_find), but a freed handle may come back
// as another object. (Every bump is a full rebuild of the kept sets: ~3,000 a frame.)
inline std::atomic<u64> g_bind_objects{0};
inline void bind_epoch_bump(bool objects = true) {
    g_bind_epoch.fetch_add(1, std::memory_order_relaxed);
    if (objects) g_bind_objects.fetch_add(1, std::memory_order_relaxed);
}

// A view of img as NVN view v (null = the whole texture as created). `attachment`: a single level for
// rendering (all of v's layers). `want`: the view type a shader declares (sampled and storage views), when
// the image can be seen that way.
VkImageView view_of(Image* img, const nvn::view_t* v, bool attachment, VkImageViewType want = VK_IMAGE_VIEW_TYPE_MAX_ENUM);

VkImageSubresourceRange full_range(const Image* img);
// The Vulkan format a view of img as NVN view v (null = whole texture) has.
VkFormat view_format(const Image* img, const nvn::view_t* v);

}  // namespace nvk
