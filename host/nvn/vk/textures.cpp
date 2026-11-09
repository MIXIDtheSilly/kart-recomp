#include "textures.h"

#include "sparse.h"
#include "upload.h"
#include "hitch.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <string>
#include <mutex>

#include "../abi.h"

namespace nvn {
extern std::atomic<uint64_t> g_frame;   // presents so far (window.cpp)
}

namespace nvk {
namespace {

std::mutex g_lock;
std::unordered_map<u64, Image*> g_images;  // by texture serial
std::unordered_map<u64, bool> g_failed;    // serials whose image could not be made (not retried)
// The game makes new texture objects over the same memory all the time (its render targets every frame, a sampled view
// of a target as a texture of its own): on the console they are the same texels. Images by storage address and shape;
// an image whose texture objects are all gone stays here a while ("parked") if the GPU made its content, so the next
// frame's texture object over that memory finds what was rendered there.
std::unordered_map<u64, Image*> g_shared;  // by shape_key
std::vector<Image*> g_parked;
constexpr u64 ParkNs = 2'000'000'000ull;
u64 g_shared_hits = 0, g_revived = 0, g_created = 0, g_stats_at = 0;
// Live image memory: ASTC textures are recompressed to BC3 here, 1 byte a texel (the console has 1 for 4x4 ASTC, 0.25 for 8x8).
std::atomic<u64> g_bytes_astc{0}, g_bytes_other{0};

std::atomic<u64>& bytes_of_kind(const Image* img) { return format(img->nvn_format).kind == FmtKind::Astc ? g_bytes_astc : g_bytes_other; }
u64 alloc_size(VmaAllocation a) {
    VmaAllocationInfo i{};
    vmaGetAllocationInfo(ctx().vma, a, &i);
    return i.size;
}

u64 shape_key(const nvn::tex_t* t) {
    static const bool off = getenv("HOST_NVN_NOSHARE") != nullptr;   // one image per texture object (to compare)
    if (!t->mem || off) return 0;
    const nvn_texdesc& d = t->d;
    u64 h = nvn::ga(t->mem) * 0x9E3779B97F4A7C15ull;
    for (const u64 v : {static_cast<u64>(d.format), static_cast<u64>(d.width), static_cast<u64>(d.height), static_cast<u64>(d.depth),
                        static_cast<u64>(d.levels), static_cast<u64>(d.target), static_cast<u64>(d.samples),
                        static_cast<u64>(t->L.layers), static_cast<u64>(d.flags)})
        h = (h ^ v) * 0xC2B2AE3D27D4EB4Full + 0x165667B19E3779F9ull;
    return h ? h : 1;
}

void destroy(Image* img) {
    upload_async_cancel(img);
    bytes_of_kind(img) -= alloc_size(img->alloc);
    // The last submissions using it may still run.
    defer([img] {
        Device& c = ctx();
        for (auto& [k, v] : img->views)
            if (v) vkDestroyImageView(c.dev, v, nullptr);
        vmaDestroyImage(c.vma, img->image, img->alloc);
        delete img;
    });
}

// Parked images past their time go (caller holds g_lock).
void sweep(u64 now) {
    for (size_t i = 0; i < g_parked.size();) {
        Image* img = g_parked[i];
        if (now - img->parked < ParkNs) { i++; continue; }
        g_shared.erase(img->shape);
        destroy(img);
        g_parked[i] = g_parked.back();
        g_parked.pop_back();
    }
}

VkFormatFeatureFlags features(VkFormat f) {
    static std::mutex m;
    static std::unordered_map<int, VkFormatFeatureFlags> cache;
    std::scoped_lock lk{m};
    if (auto it = cache.find(f); it != cache.end()) return it->second;
    VkFormatProperties p{};
    vkGetPhysicalDeviceFormatProperties(ctx().phys, f, &p);
    return cache[f] = p.optimalTilingFeatures;
}

VkImageAspectFlags aspect_of(const Format& f) {
    if (f.depth || f.stencil)
        return (f.depth ? VK_IMAGE_ASPECT_DEPTH_BIT : 0) | (f.stencil ? VK_IMAGE_ASPECT_STENCIL_BIT : 0);
    return VK_IMAGE_ASPECT_COLOR_BIT;
}

VkComponentSwizzle swz(u32 nvn_swizzle) {
    return nvn_swizzle <= NVN_TEXTURE_SWIZZLE_A ? static_cast<VkComponentSwizzle>(nvn_swizzle + 1) : VK_COMPONENT_SWIZZLE_IDENTITY;
}

}  // namespace

VkImageSubresourceRange full_range(const Image* img) {
    return {img->aspect, 0, img->levels, 0, VK_REMAINING_ARRAY_LAYERS};
}

VkFormat view_format(const Image* img, const nvn::view_t* v) {
    const u32 fmt = v && v->set_format && v->format ? v->format : img->nvn_format;
    const Format& f = format(fmt);
    return f.kind == FmtKind::None || format(img->nvn_format).kind == FmtKind::Astc ? img->format : f.vk;
}

Image* image_find(u64 serial) {
    std::scoped_lock lk{g_lock};
    auto it = g_images.find(serial);
    return it == g_images.end() ? nullptr : it->second;
}

Image* image_of(const nvn::tex_t* t, VkCommandBuffer cb) {
    if (!t || !t->serial) return nullptr;
    {
        std::scoped_lock lk{g_lock};
        if (auto it = g_images.find(t->serial); it != g_images.end()) return it->second;
        if (g_failed.count(t->serial)) return nullptr;
        const u64 now = host_ns();
        sweep(now);
        if (now - g_stats_at > 10'000'000'000ull) {
            if (g_stats_at)
            {
                VmaBudget b[VK_MAX_MEMORY_HEAPS]{};
                vmaGetHeapBudgets(ctx().vma, b);
                const VkPhysicalDeviceMemoryProperties* mp = nullptr;
                vmaGetMemoryProperties(ctx().vma, &mp);
                u64 local = 0, budget = 0, blocks = 0, used = 0;
                for (u32 h = 0; h < mp->memoryHeapCount; h++)
                    if (mp->memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)
                        local += b[h].usage, budget += b[h].budget, blocks += b[h].statistics.blockBytes, used += b[h].statistics.allocationBytes;
                LOG("nvk: images: %llu made, %llu shared with a live texture over the same memory, %llu taken over from a released one; %zu parked; "
                    "%llu MB live (ASTC decoded %llu MB, other %llu MB); sparse images %llu MB; VRAM %llu of %llu MB (VMA: %llu MB in blocks, %llu MB of it used)",
                    static_cast<unsigned long long>(g_created), static_cast<unsigned long long>(g_shared_hits),
                    static_cast<unsigned long long>(g_revived), g_parked.size(),
                    static_cast<unsigned long long>((g_bytes_astc.load() + g_bytes_other.load()) >> 20), static_cast<unsigned long long>(g_bytes_astc.load() >> 20),
                    static_cast<unsigned long long>(g_bytes_other.load() >> 20), static_cast<unsigned long long>(sparse_bytes() >> 20),
                    static_cast<unsigned long long>(local >> 20),
                    static_cast<unsigned long long>(budget >> 20), static_cast<unsigned long long>(blocks >> 20),
                    static_cast<unsigned long long>(used >> 20));
            }
            // HOST_NVN_IMAGE_REPORT=1: the live images by shape, the biggest groups
            {   // memory held by images not viewed for 30 s (1800 frames) or more
                const u64 frame = nvn::g_frame.load(), cold_at = frame > 1800 ? frame - 1800 : 0;
                std::unordered_map<const Image*, int> seen;
                u64 n = 0, bytes = 0, sparse = 0;
                const Image* big[3] = {};
                u64 big_bytes[3] = {};
                auto add = [&](const Image* img) {
                    if (seen[img]++ || img->last_use >= cold_at) return;
                    const u64 b = img->sparse ? sparse_image_bytes(img) : alloc_size(img->alloc);
                    n++, (img->sparse ? sparse : bytes) += b;
                    for (int i = 0; i < 3; i++)
                        if (b > big_bytes[i]) {
                            for (int k = 2; k > i; k--) big[k] = big[k - 1], big_bytes[k] = big_bytes[k - 1];
                            big[i] = img, big_bytes[i] = b;
                            break;
                        }
                };
                for (auto& [serial, img] : g_images) add(img);
                for (const Image* img : g_parked) add(img);
                std::string top;
                for (int i = 0; i < 3 && big[i]; i++) {
                    char b[128];
                    std::snprintf(b, sizeof b, "%s%llu MB image %llu %ux%u x%u fmt %#x%s", i ? ", " : "", static_cast<unsigned long long>(big_bytes[i] >> 20),
                                  static_cast<unsigned long long>(big[i]->serial), big[i]->width, big[i]->height, big[i]->layers, big[i]->nvn_format,
                                  big[i]->sparse ? " sparse" : "");
                    top += b;
                }
                LOG("nvk: images not used for 30 s: %llu, %llu MB (%llu MB of them sparse); biggest: %s", static_cast<unsigned long long>(n),
                    static_cast<unsigned long long>((bytes + sparse) >> 20), static_cast<unsigned long long>(sparse >> 20), top.c_str());
            }
            static const bool report = getenv("HOST_NVN_IMAGE_REPORT") != nullptr;
            if (report) {
                struct Group { u64 bytes = 0; u32 n = 0, gpu = 0, shared = 0; const Image* any = nullptr; };
                std::unordered_map<u64, Group> groups;
                std::unordered_map<const Image*, int> seen;
                auto add = [&](const Image* img) {
                    if (img->sparse || seen[img]++) return;
                    const u64 k = static_cast<u64>(img->nvn_format) | static_cast<u64>(img->width) << 8 | static_cast<u64>(img->height) << 24 |
                                  static_cast<u64>(img->layers * img->depth) << 40 | static_cast<u64>(img->levels) << 56;
                    Group& g = groups[k];
                    g.bytes += alloc_size(img->alloc), g.n++, g.gpu += img->gpu_written, g.shared += img->refs > 1, g.any = img;
                };
                for (auto& [serial, img] : g_images) add(img);
                for (const Image* img : g_parked) add(img);
                std::vector<std::pair<u64, Group>> v(groups.begin(), groups.end());
                std::sort(v.begin(), v.end(), [](auto& a, auto& b) { return a.second.bytes > b.second.bytes; });
                for (size_t i = 0; i < v.size() && i < 24; i++) {
                    const Group& g = v[i].second;
                    LOG("nvk:   %5llu MB: %u x %ux%u x%u, %u levels, NVN format %#x (%u GPU-made, %u shared)", static_cast<unsigned long long>(g.bytes >> 20), g.n,
                        g.any->width, g.any->height, g.any->layers * g.any->depth, g.any->levels, g.any->nvn_format, g.gpu, g.shared);
                }
            }
            g_stats_at = now, g_created = g_shared_hits = g_revived = 0;
        }
        if (const u64 key = shape_key(t))
            if (auto it = g_shared.find(key); it != g_shared.end()) {
                Image* img = it->second;
                if (img->parked) {
                    // (only GPU-made images are parked)
                    g_parked.erase(std::find(g_parked.begin(), g_parked.end(), img));
                    img->parked = 0, img->refs = 0;
                    g_revived++;
                } else {
                    g_shared_hits++;
                }
                img->refs++;
                g_images[t->serial] = img;
                return img;
            }
    }
    const nvn_texdesc& d = t->d;
    const Format& f = format(static_cast<u32>(d.format));
    if (f.kind == FmtKind::None || d.target == NVN_TEXTURE_TARGET_BUFFER) {
        static std::mutex m;
        static std::unordered_map<int, bool> seen;
        std::scoped_lock lk{m};
        if (!seen[d.format * 16 + d.target])
            LOG("[RED] nvk: texture format %#x target %d has no Vulkan image yet", d.format, d.target), seen[d.format * 16 + d.target] = true;
        return nullptr;
    }
    auto* img = new Image;
    img->serial = t->serial;
    img->desc = d;
    img->nvn_format = static_cast<u32>(d.format);
    img->format = f.vk;
    img->aspect = aspect_of(f);
    img->width = std::max(d.width, 1u);
    img->height = std::max(d.height, 1u);
    img->levels = std::max(d.levels, 1u);
    img->layers = std::max(t->L.layers, 1u);
    VkImageCreateFlags flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT | VK_IMAGE_CREATE_EXTENDED_USAGE_BIT;
    switch (d.target) {
    case NVN_TEXTURE_TARGET_1D:
    case NVN_TEXTURE_TARGET_1D_ARRAY: img->type = VK_IMAGE_TYPE_1D; img->height = 1; break;
    case NVN_TEXTURE_TARGET_3D:
        img->type = VK_IMAGE_TYPE_3D;
        img->depth = std::max(d.depth, 1u);
        img->layers = 1;
        flags |= VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT;
        break;
    default: img->type = VK_IMAGE_TYPE_2D; break;
    }
    if (d.target == NVN_TEXTURE_TARGET_2D_MULTISAMPLE || d.target == NVN_TEXTURE_TARGET_2D_MULTISAMPLE_ARRAY)
        img->samples = static_cast<VkSampleCountFlagBits>(std::max(d.samples, 1));
    img->cube_ok = img->type == VK_IMAGE_TYPE_2D && img->width == img->height && img->layers >= 6 &&
                   img->samples == VK_SAMPLE_COUNT_1_BIT;
    if (img->cube_ok) flags |= VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
    if (f.block_w > 1 && f.kind != FmtKind::Astc) flags |= VK_IMAGE_CREATE_BLOCK_TEXEL_VIEW_COMPATIBLE_BIT;

    const VkFormatFeatureFlags ff = features(img->format);
    VkImageUsageFlags usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (ff & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT) usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    if (ff & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) usage |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    if ((ff & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) && img->samples == VK_SAMPLE_COUNT_1_BIT) {
        usage |= VK_IMAGE_USAGE_STORAGE_BIT;
        img->storage_ok = true;
    }
    // Size as created (ASTC as BC3, 1 byte a texel): the game's sparse texture arrays (8192x8192x68 ASTC)
    // are mostly unmapped on the console and do not fit as plain images here.
    {
        const u64 bpt = f.kind == FmtKind::Astc ? 1 : std::max<u64>(1, f.bytes / std::max(1, f.block_w * f.block_h));
        const u64 bytes = static_cast<u64>(img->width) * img->height * img->depth * img->layers * bpt * img->samples * 4 / 3;
        // A big texture in a sparse pool: a sparse-residency image with memory where the game mapped some. Below 1 GiB only
        // when it is mostly unmapped (the game's 16384x16384 and 8192x8192 ones, 341 MB each as plain images, have 5 to 25 MB
        // mapped; it renders to a half-mapped 6144x6144 one).
        if (bytes >= (64ull << 20))
            if (Image* sp = sparse_image_of(t, cb, bytes <= (1ull << 30))) {
                delete img;
                std::scoped_lock lk{g_lock};
                auto [it, fresh] = g_images.emplace(t->serial, sp);
                if (!fresh) sparse_image_release(sp);
                return it->second;
            }
        if (bytes > (1ull << 30)) {
            LOG("[RED] nvk: image %ux%ux%u x%u layers, format NVN %#x would take %llu MB: not created (sampled as empty)", img->width,
                img->height, img->depth, img->layers, d.format, static_cast<unsigned long long>(bytes >> 20));
            std::scoped_lock lk{g_lock};
            g_failed[t->serial] = true;
            delete img;
            return nullptr;
        }
    }
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.flags = flags;
    ci.imageType = img->type;
    ci.format = img->format;
    ci.extent = {img->width, img->height, img->depth};
    ci.mipLevels = img->levels;
    ci.arrayLayers = img->layers;
    ci.samples = img->samples;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = usage;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VmaAllocationCreateInfo ai{};
    ai.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    HitchTimer ht{HI_IMAGE};
    if (vmaCreateImage(ctx().vma, &ci, &ai, &img->image, &img->alloc, nullptr) != VK_SUCCESS) {
        LOG("[RED] nvk: image %ux%ux%u x%u layers, %u levels, format %d (NVN %#x) could not be created", img->width,
            img->height, img->depth, img->layers, img->levels, img->format, d.format);
        std::scoped_lock lk{g_lock};
        g_failed[t->serial] = true;
        delete img;
        return nullptr;
    }
    // UNDEFINED -> GENERAL, before anything in cb uses it.
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask = 0;
    b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img->image;
    b.subresourceRange = full_range(img);
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
    std::scoped_lock lk{g_lock};
    auto [it, fresh] = g_images.emplace(t->serial, img);
    if (!fresh) {  // another thread made it first
        vmaDestroyImage(ctx().vma, img->image, img->alloc);
        delete img;
        return it->second;
    }
    g_created++;
    bytes_of_kind(img) += alloc_size(img->alloc);
    img->mem = nvn::ga(t->mem);
    if (const u64 key = shape_key(t); key && g_shared.emplace(key, img).second) img->shape = key;
    return img;
}

void image_release(u64 serial) {
    Image* img;
    {
        std::scoped_lock lk{g_lock};
        auto it = g_images.find(serial);
        if (it == g_images.end()) return;
        img = it->second;
        g_images.erase(it);
        g_failed.erase(serial);
    }
    if (img->sparse) {
        sparse_image_release(img);
        return;
    }
    {
        std::scoped_lock lk{g_lock};
        if (--img->refs > 0) return;
        if (img->shape) {
            if (img->gpu_written) {
                img->parked = std::max<u64>(host_ns(), 1);
                g_parked.push_back(img);
                return;
            }
            g_shared.erase(img->shape);
        }
    }
    destroy(img);
}

void image_pool_gone(u64 lo, u64 hi) {
    std::scoped_lock lk{g_lock};
    for (Image* img : g_parked)
        if (img->mem >= lo && img->mem < hi) img->parked = 1;  // past its time: the sweep destroys it
    sweep(host_ns());
}

VkImageView view_of(Image* img, const nvn::view_t* v, bool attachment, VkImageViewType want) {
    img->last_use = nvn::g_frame.load(std::memory_order_relaxed);
    u32 base_level = 0, nlevels = img->levels, base_layer = 0, nlayers = img->layers;
    int target = img->desc.target;
    u32 fmt = img->nvn_format;
    u8 sw[4] = {NVN_TEXTURE_SWIZZLE_R, NVN_TEXTURE_SWIZZLE_G, NVN_TEXTURE_SWIZZLE_B, NVN_TEXTURE_SWIZZLE_A};
    bool stencil = false;
    if (v) {
        if (v->set_levels) base_level = v->base_level, nlevels = v->nlevels ? v->nlevels : img->levels - v->base_level;
        if (v->set_layers) base_layer = v->min_layer, nlayers = v->nlayers ? v->nlayers : img->layers - v->min_layer;
        if (v->set_target) target = v->target;
        if (v->set_format && v->format) fmt = v->format;
        if (v->set_swizzle) std::memcpy(sw, v->swz, 4);
        if (v->set_dsmode) stencil = v->dsmode == 1;
    }
    if (attachment) {
        nlevels = 1;
        std::memset(sw, 0, 4);
    }
    base_level = std::min(base_level, img->levels - 1);
    nlevels = std::max(1u, std::min(nlevels, img->levels - base_level));
    if (img->type != VK_IMAGE_TYPE_3D) {
        base_layer = std::min(base_layer, img->layers - 1);
        nlayers = std::max(1u, std::min(nlayers, img->layers - base_layer));
    }
    const Format& f = format(fmt);
    const VkFormat vkf = f.kind == FmtKind::None ? img->format : (format(img->nvn_format).kind == FmtKind::Astc ? img->format : f.vk);
    const u64 key = static_cast<u64>(base_level) | static_cast<u64>(nlevels) << 5 | static_cast<u64>(base_layer) << 10 |
                    static_cast<u64>(nlayers) << 23 | static_cast<u64>(target & 15) << 36 | static_cast<u64>(vkf) << 40 |
                    static_cast<u64>(attachment) << 63 | static_cast<u64>(stencil) << 62 |
                    static_cast<u64>(sw[0] | sw[1] << 3 | sw[2] << 6 | sw[3] << 9) * 0x9E3779B97F4A7C15ull ^
                    (static_cast<u64>(want) + 1) * 0xC2B2AE3D27D4EB4Full;
    if (auto it = img->views.find(key); it != img->views.end()) return it->second;

    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = img->image;
    vi.format = vkf;
    if (attachment) {
        vi.viewType = img->type == VK_IMAGE_TYPE_3D || nlayers > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
        if (img->type == VK_IMAGE_TYPE_1D) vi.viewType = nlayers > 1 ? VK_IMAGE_VIEW_TYPE_1D_ARRAY : VK_IMAGE_VIEW_TYPE_1D;
        if (img->type == VK_IMAGE_TYPE_3D) {
            // A 3D level rendered as its slices (2D_ARRAY_COMPATIBLE): the view's layers are slices (the game
            // renders a LUT one slice at a time this way), else all of them.
            const u32 d = std::max(1u, img->depth >> base_level);
            if (v && v->set_layers) {
                base_layer = std::min(base_layer, d - 1);
                nlayers = std::max(1u, std::min(v->nlayers ? static_cast<u32>(v->nlayers) : d - base_layer, d - base_layer));
            } else {
                base_layer = 0;
                nlayers = d;
            }
            vi.viewType = nlayers > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
        }
    } else {
        switch (target) {
        case NVN_TEXTURE_TARGET_1D: vi.viewType = VK_IMAGE_VIEW_TYPE_1D; break;
        case NVN_TEXTURE_TARGET_1D_ARRAY: vi.viewType = VK_IMAGE_VIEW_TYPE_1D_ARRAY; break;
        case NVN_TEXTURE_TARGET_3D: vi.viewType = VK_IMAGE_VIEW_TYPE_3D; break;
        case NVN_TEXTURE_TARGET_2D_ARRAY:
        case NVN_TEXTURE_TARGET_2D_MULTISAMPLE_ARRAY: vi.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY; break;
        case NVN_TEXTURE_TARGET_CUBEMAP: vi.viewType = img->cube_ok ? VK_IMAGE_VIEW_TYPE_CUBE : VK_IMAGE_VIEW_TYPE_2D_ARRAY; break;
        case NVN_TEXTURE_TARGET_CUBEMAP_ARRAY: vi.viewType = img->cube_ok ? VK_IMAGE_VIEW_TYPE_CUBE_ARRAY : VK_IMAGE_VIEW_TYPE_2D_ARRAY; break;
        default: vi.viewType = VK_IMAGE_VIEW_TYPE_2D; break;
        }
        if (want != VK_IMAGE_VIEW_TYPE_MAX_ENUM && img->type != VK_IMAGE_TYPE_3D && want != VK_IMAGE_VIEW_TYPE_3D) {
            const bool cube = want == VK_IMAGE_VIEW_TYPE_CUBE || want == VK_IMAGE_VIEW_TYPE_CUBE_ARRAY;
            const bool one_d = want == VK_IMAGE_VIEW_TYPE_1D || want == VK_IMAGE_VIEW_TYPE_1D_ARRAY;
            if ((!cube || (img->cube_ok && nlayers >= 6)) && one_d == (img->type == VK_IMAGE_TYPE_1D)) vi.viewType = want;
        }
        if (img->type == VK_IMAGE_TYPE_3D) vi.viewType = VK_IMAGE_VIEW_TYPE_3D, base_layer = 0, nlayers = 1;
        if ((vi.viewType == VK_IMAGE_VIEW_TYPE_2D || vi.viewType == VK_IMAGE_VIEW_TYPE_1D) && nlayers > 1) nlayers = 1;
        if (vi.viewType == VK_IMAGE_VIEW_TYPE_CUBE) nlayers = 6;
        if (vi.viewType == VK_IMAGE_VIEW_TYPE_CUBE_ARRAY) nlayers -= nlayers % 6;
        const bool noalpha = format(img->nvn_format).noalpha || f.noalpha;
        VkComponentSwizzle c[4];
        for (int i = 0; i < 4; i++) {
            c[i] = swz(sw[i]);
            if (noalpha && sw[i] == NVN_TEXTURE_SWIZZLE_A) c[i] = VK_COMPONENT_SWIZZLE_ONE;
        }
        vi.components = {c[0], c[1], c[2], c[3]};
    }
    VkImageAspectFlags aspect = img->aspect;
    if (!attachment && (aspect & VK_IMAGE_ASPECT_DEPTH_BIT)) aspect = stencil ? VK_IMAGE_ASPECT_STENCIL_BIT : VK_IMAGE_ASPECT_DEPTH_BIT;
    if (!attachment && aspect == (VK_IMAGE_ASPECT_STENCIL_BIT)) vi.components = {};
    vi.subresourceRange = {aspect, base_level, nlevels, base_layer, nlayers};
    // A view's format may lack the storage feature the image's own format has (sRGB views of RGBA8).
    VkImageViewUsageCreateInfo usage{VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO};
    usage.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    const VkFormatFeatureFlags vff = features(vkf);
    if (attachment && (vff & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT)) usage.usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    if (attachment && (vff & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)) usage.usage |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    if (img->storage_ok && (vff & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT)) usage.usage |= VK_IMAGE_USAGE_STORAGE_BIT;
    vi.pNext = &usage;
    VkImageView view = VK_NULL_HANDLE;
    if (vkCreateImageView(ctx().dev, &vi, nullptr, &view) != VK_SUCCESS) {
        LOG("[RED] nvk: view of image %llu (format %d, type %d, levels %u+%u, layers %u+%u) failed",
            static_cast<unsigned long long>(img->serial), vkf, vi.viewType, base_level, nlevels, base_layer, nlayers);
        view = VK_NULL_HANDLE;
    }
    img->views.emplace(key, view);
    return view;
}

}  // namespace nvk
