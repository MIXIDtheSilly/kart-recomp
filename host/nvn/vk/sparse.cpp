#include "sparse.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <execution>
#include <memory>
#include <mutex>
#include <span>
#include <unordered_map>
#include <vector>

#include "../abi.h"
#include "../nvn_tex.h"
#include "../replay.h"
#include "draw.h"
#include "hitch.h"
#include "vkq.h"
#include "codecs.h"

namespace nvn {
extern std::atomic<uint64_t> g_frame;   // presents so far (window.cpp)
}

namespace nvk {

struct SparseImage {
    struct Level { u64 off; u32 w, h, tiles_x, tiles_y, first; };  // offset in a layer, size in texels, its tiles (first: index in a layer)
    struct Tile { VmaAllocation mem = VK_NULL_HANDLE; bool dirty = false; };
    Image* img = nullptr;
    int refs = 1;
    const nvn::pool_t* pool = nullptr;
    u64 gpu = 0, layer_size = 0, size = 0;  // storage
    nvn_texdesc d{}, tile_d{};              // the texture; one 64 KB tile of it as a texture of its own
    u32 bpb = 0, bw = 1, bh = 1;
    bool astc = false;                      // decoded to BC3 (bc1: to BC1)
    bool bc1 = false;
    u32 tw = 0, th = 0;                     // a tile in texels
    u32 tiled = 0;                          // levels bound a tile at a time; the rest (the tail) a layer at a time
    u64 tail_off = 0;                       // where the tail starts in a layer
    std::vector<Level> lv;
    u32 per_layer = 0;                      // tiles in a layer
    std::vector<Tile> tiles;
    std::vector<u32> queue;                 // dirty tiles
    std::vector<u8> tail_dirty;             // per layer
    bool tail_any = false;
    std::vector<VmaAllocation> fixed;       // memory of the tail, bound for good
    VkExtent3D gran{};
    u64 block = 0;                          // bytes of a sparse block
    u32 memtypes = 0;
    u64 bound = 0, uploads = 0, unbinds = 0, ms_total = 0, last_log = 0;
    std::once_flag zero_once;
    std::vector<u8> zero_out;  // an all-zero tile converted
    // Moved out to host memory while not used (sparse_tick): the bound tiles' texels copied to buf, then (once the copy
    // is done) the tiles unbound; copied back before the next use.
    struct Evicted {
        VkBuffer buf = VK_NULL_HANDLE;
        VmaAllocation alloc = VK_NULL_HANDLE;
        std::vector<u32> tiles;
        std::vector<VkBufferImageCopy> regions;   // of tiles[k], in buf
        std::shared_ptr<std::atomic<bool>> copied;
        bool unbound = false;
        u64 bytes = 0;
    };
    std::unique_ptr<Evicted> ev;
};

namespace {

constexpr u64 Tile64 = 0x10000;

std::mutex g_mu;  // g_list, g_dirty, g_binds, g_opaque
std::vector<SparseImage*> g_list;
std::vector<std::pair<u64, u64>> g_dirty;
std::unordered_map<VkImage, std::vector<VkSparseImageMemoryBind>> g_binds;  // of the submission being recorded
std::unordered_map<VkImage, std::vector<VkSparseMemoryBind>> g_opaque;
VkSemaphore g_sem = VK_NULL_HANDLE;
u64 g_sem_value = 0;
u64 g_commits = 0;

std::atomic<u64> g_bytes{0};   // memory bound to sparse images (tiles and tails)

VmaAllocation alloc_blocks(const SparseImage* s, u64 bytes, VmaAllocationInfo* info) {
    VkMemoryRequirements mr{bytes, s->block, s->memtypes};
    VmaAllocationCreateInfo ai{};
    ai.requiredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    VmaAllocation a = VK_NULL_HANDLE;
    if (vmaAllocateMemory(ctx().vma, &mr, &ai, &a, info) != VK_SUCCESS) return VK_NULL_HANDLE;
    g_bytes += info->size;
    return a;
}

void free_blocks(VmaAllocation a) {
    VmaAllocationInfo info{};
    vmaGetAllocationInfo(ctx().vma, a, &info);
    g_bytes -= info.size;
    vmaFreeMemory(ctx().vma, a);
}

u64 region_bytes(const SparseImage* s, VkExtent3D e) {
    return static_cast<u64>((e.width + s->gran.width - 1) / s->gran.width) * ((e.height + s->gran.height - 1) / s->gran.height) * s->block;
}

// Binds new memory at a region of one level and layer (caller does not hold g_mu).
VmaAllocation bind_region(SparseImage* s, u32 level, u32 layer, VkOffset3D o, VkExtent3D e) {
    VmaAllocationInfo info{};
    const VmaAllocation a = alloc_blocks(s, region_bytes(s, e), &info);
    if (!a) return VK_NULL_HANDLE;
    VkSparseImageMemoryBind b{};
    b.subresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, layer};
    b.offset = o, b.extent = e;
    b.memory = info.deviceMemory, b.memoryOffset = info.offset;
    std::scoped_lock lk{g_mu};
    g_binds[s->img->image].push_back(b);
    return a;
}

void unbind_region(SparseImage* s, u32 level, u32 layer, VkOffset3D o, VkExtent3D e) {
    VkSparseImageMemoryBind b{};
    b.subresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, layer};
    b.offset = o, b.extent = e;
    std::scoped_lock lk{g_mu};
    g_binds[s->img->image].push_back(b);
}

// Tile i: its level, layer, texel rectangle and storage offset.
struct TileAt { u32 level, layer; VkOffset3D o; VkExtent3D e; u64 off; };
TileAt tile_at(const SparseImage* s, u32 i) {
    TileAt t{};
    t.layer = i / s->per_layer;
    const u32 r = i % s->per_layer;
    u32 L = 0;
    while (L + 1 < s->tiled && r >= s->lv[L + 1].first) L++;
    const SparseImage::Level& l = s->lv[L];
    const u32 k = r - l.first, tx = k % l.tiles_x, ty = k / l.tiles_x;
    t.level = L;
    t.o = {static_cast<int>(tx * s->tw), static_cast<int>(ty * s->th), 0};
    t.e = {std::min(s->tw, l.w - tx * s->tw), std::min(s->th, l.h - ty * s->th), 1};
    t.off = t.layer * s->layer_size + l.off + static_cast<u64>(k) * Tile64;
    return t;
}

// Storage offset `o` of s shows other memory: flag its tile (or its layer's tail).
void mark(SparseImage* s, u64 o) {
    const u64 layer = o / s->layer_size, in = o % s->layer_size;
    if (layer >= s->img->layers) return;
    if (in >= s->tail_off) {
        if (s->tiled < s->img->levels) s->tail_dirty[layer] = 1, s->tail_any = true;
        return;
    }
    for (u32 L = 0; L < s->tiled; L++) {
        const SparseImage::Level& l = s->lv[L];
        const u64 n = static_cast<u64>(l.tiles_x) * l.tiles_y;
        if (in >= l.off + n * Tile64) continue;
        const u32 i = static_cast<u32>(layer * s->per_layer + l.first + (in - l.off) / Tile64);
        if (!s->tiles[i].dirty) s->tiles[i].dirty = true, s->queue.push_back(i);
        return;
    }
}

// The mappings noted since into the images' dirty tiles.
void drain() {
    std::scoped_lock lk{g_mu};
    for (const auto& [a, n] : g_dirty)
        for (SparseImage* s : g_list) {
            const u64 lo = std::max(a, s->gpu), hi = std::min(a + n, s->gpu + s->size);
            for (u64 o = (lo - s->gpu) & ~(Tile64 - 1); lo < hi && o < hi - s->gpu; o += Tile64) mark(s, o);
        }
    g_dirty.clear();
}

// Bytes of a region of extent e of s's image (as created: ASTC as BC3 or BC1).
u64 texel_bytes(const SparseImage* s, VkExtent3D e) {
    if (s->astc) return static_cast<u64>((e.width + 3) / 4) * ((e.height + 3) / 4) * (s->bc1 ? 8 : 16);
    return static_cast<u64>((e.width + s->bw - 1) / s->bw) * ((e.height + s->bh - 1) / s->bh) * s->bpb;
}

// The copy out is not wanted (any more): its buffer goes.
void evict_drop(SparseImage* s) {
    const std::unique_ptr<SparseImage::Evicted> ev = std::move(s->ev);
    const VkBuffer b = ev->buf;
    const VmaAllocation a = ev->alloc;
    defer([b, a] { vmaDestroyBuffer(ctx().vma, b, a); });
}

// Moving s out, first step: its bound tiles copied to a host buffer in pre (the tiles go once that is done, sparse_tick).
void evict_start(SparseImage* s, VkCommandBuffer pre) {
    auto ev = std::make_unique<SparseImage::Evicted>();
    for (u32 i = 0; i < s->tiles.size(); i++) {
        if (!s->tiles[i].mem) continue;
        const TileAt at = tile_at(s, i);
        VkBufferImageCopy r{};
        r.bufferOffset = ev->bytes;
        r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, at.level, at.layer, 1};
        r.imageOffset = at.o, r.imageExtent = at.e;
        ev->bytes += (texel_bytes(s, at.e) + 15) & ~u64{15};
        ev->tiles.push_back(i), ev->regions.push_back(r);
    }
    if (ev->tiles.empty()) return;
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = ev->bytes;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    VmaAllocationCreateInfo ai{};
    ai.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST;
    if (vmaCreateBuffer(ctx().vma, &bci, &ai, &ev->buf, &ev->alloc, nullptr) != VK_SUCCESS) return;
    VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    mb.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(pre, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr);
    vkCmdCopyImageToBuffer(pre, s->img->image, VK_IMAGE_LAYOUT_GENERAL, ev->buf, static_cast<u32>(ev->regions.size()), ev->regions.data());
    ev->copied = std::make_shared<std::atomic<bool>>(false);
    defer([f = ev->copied] { f->store(true); });
    s->ev = std::move(ev);
}

// Moving s back before a use: its tiles bound again and their texels copied back in pre.
void restore(SparseImage* s, VkCommandBuffer pre) {
    const u64 t0 = host_ns();
    SparseImage::Evicted& ev = *s->ev;
    std::vector<VkBufferImageCopy> regions;
    for (size_t k = 0; k < ev.tiles.size(); k++) {
        SparseImage::Tile& t = s->tiles[ev.tiles[k]];
        if (t.mem) continue;
        const TileAt at = tile_at(s, ev.tiles[k]);
        t.mem = bind_region(s, at.level, at.layer, at.o, at.e);
        if (!t.mem) {
            static bool once = false;
            if (!once) once = true, LOG("[RED] nvk: sparse image %llu: out of memory moving it back", static_cast<unsigned long long>(s->img->serial));
            continue;
        }
        s->bound++;
        regions.push_back(ev.regions[k]);
    }
    if (!regions.empty()) {
        vkCmdCopyBufferToImage(pre, ev.buf, s->img->image, VK_IMAGE_LAYOUT_GENERAL, static_cast<u32>(regions.size()), regions.data());
        VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};   // before tile uploads over the same texels
        mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        mb.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT;
        vkCmdPipelineBarrier(pre, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr);
    }
    bind_epoch_bump(false);   // (content: kept descriptor sets stay)
    LOG("nvk: sparse image %llu: %llu MB moved back from host memory (%llu ms)", static_cast<unsigned long long>(s->img->serial),
        static_cast<unsigned long long>(ev.bytes >> 20), static_cast<unsigned long long>((host_ns() - t0) / 1'000'000));
    evict_drop(s);
}

// One upload: texels of one level and layer, from NVN storage (as `desc` level `dlevel` at `base`) to the image's format.
struct Job {
    const nvn_texdesc* desc;
    u32 dlevel, fw, fh;  // the source level and its size in texels
    std::shared_ptr<std::vector<u8>> src;
    u64 src_off;         // of the source's level 0 in src (may be "negative": a tail starts past level 0)
    VkBufferImageCopy r{};
    std::vector<u8> out;
};

void convert_now(const SparseImage* s, Job& j);

void convert(SparseImage* s, Job& j) {
    // Unwritten storage (most tiles at first) converts the same every time
    const bool tile = j.desc == &s->tile_d;
    const bool zero = tile && std::all_of(j.src->begin(), j.src->end(), [](u8 b) { return b == 0; });
    if (zero) {
        std::call_once(s->zero_once, [&] { Job z = j; z.out.clear(); convert_now(s, z); s->zero_out = std::move(z.out); });
        j.out = s->zero_out;
        return;
    }
    convert_now(s, j);
}

void convert_now(const SparseImage* s, Job& j) {
    const u32 bx = (j.fw + s->bw - 1) / s->bw, by = (j.fh + s->bh - 1) / s->bh;
    const size_t pitch = static_cast<size_t>(bx) * s->bpb;
    std::vector<u8> lin(pitch * by);
    ::nvn_deswizzle(j.desc, j.dlevel, 0, j.src->data() - j.src_off, lin.data(), pitch);
    if (!s->astc) {
        j.out = std::move(lin);
        return;
    }
    std::vector<u8> rgba(static_cast<size_t>(j.fw) * j.fh * 4);
    codec::astc_decode(std::span<const u8>(lin), j.fw, j.fh, 1, s->bw, s->bh, std::span<u8>(rgba));
    j.out.resize(static_cast<size_t>((j.fw + 3) / 4) * ((j.fh + 3) / 4) * (s->bc1 ? 8 : 16));
    if (s->bc1) codec::bc1_encode(std::span<const u8>(rgba), j.fw, j.fh, 1, std::span<u8>(j.out));
    else codec::bc3_encode(std::span<const u8>(rgba), j.fw, j.fh, 1, std::span<u8>(j.out));
}

void set_region(const SparseImage* s, Job& j, u32 level, u32 layer, VkOffset3D o, VkExtent3D e) {
    j.r.bufferRowLength = s->astc ? (j.fw + 3) & ~3u : (j.fw + s->bw - 1) / s->bw * s->bw;
    j.r.bufferImageHeight = s->astc ? (j.fh + 3) & ~3u : (j.fh + s->bh - 1) / s->bh * s->bh;
    j.r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, layer, 1};
    j.r.imageOffset = o;
    j.r.imageExtent = e;
}

void log_stats(SparseImage* s, const char* why) {
    u64 tiles = 0, tail = 0;
    VmaAllocationInfo info{};
    for (const SparseImage::Tile& t : s->tiles)
        if (t.mem) vmaGetAllocationInfo(ctx().vma, t.mem, &info), tiles += info.size;
    for (VmaAllocation a : s->fixed) vmaGetAllocationInfo(ctx().vma, a, &info), tail += info.size;
    LOG("nvk: sparse image %llu: %llu MB of memory, %llu MB of it tiles and %llu MB its tail (%zu allocations, bound for good)",
        static_cast<unsigned long long>(s->img->serial), static_cast<unsigned long long>((tiles + tail) >> 20),
        static_cast<unsigned long long>(tiles >> 20), static_cast<unsigned long long>(tail >> 20), s->fixed.size());
    LOG("nvk: sparse image %llu (%ux%u x%u, NVN format %#x)%s: %llu tiles bound (%llu MB), %llu uploads, %llu unbinds, %llu ms of tile work",
        static_cast<unsigned long long>(s->img->serial), s->img->width, s->img->height, s->img->layers, s->img->nvn_format, why,
        static_cast<unsigned long long>(s->bound), static_cast<unsigned long long>(s->bound * region_bytes(s, {s->tw, s->th, 1}) >> 20),
        static_cast<unsigned long long>(s->uploads), static_cast<unsigned long long>(s->unbinds), static_cast<unsigned long long>(s->ms_total));
}

}  // namespace

Image* sparse_image_of(const nvn::tex_t* t, VkCommandBuffer cb, bool mostly_unmapped) {
    Device& c = ctx();
    const nvn_texdesc& d = t->d;
    static const bool off = getenv("HOST_NVN_NOSPARSE") != nullptr;   // sampled as empty, as before (to compare)
    if (off || !c.feat.sparse || !t->mem || !t->pool || !nvn::sparse_pool(t->pool)) return nullptr;
    if (d.target != NVN_TEXTURE_TARGET_2D && d.target != NVN_TEXTURE_TARGET_2D_ARRAY) return nullptr;
    const u64 gpu = nvn::ga(t->mem);
    const u32 layers = std::max(t->L.layers, 1u), levels = std::max(d.levels, 1u);
    if (mostly_unmapped) {
        u64 pages = 0, mapped = 0;
        for (u64 o = 0; o < t->L.size; o += Tile64, pages++) mapped += nvn::sparse_mapped(t->pool, gpu + o, std::min<u64>(Tile64, t->L.size - o));
        if (mapped * 4 >= pages) return nullptr;
    }
    {
        std::scoped_lock lk{g_mu};
        for (SparseImage* s : g_list)
            if (s->gpu == gpu && s->d.format == d.format && s->d.width == d.width && s->d.height == d.height &&
                s->img->layers == layers && s->img->levels == levels) {
                s->refs++;
                return s->img;
            }
    }
    auto fail = [&](const char* why) -> Image* {
        LOG("[RED] nvk: sparse image %ux%u x%u layers, NVN format %#x: %s", d.width, d.height, layers, d.format, why);
        return nullptr;
    };
    const Format& f = format(static_cast<u32>(d.format));
    auto s = std::make_unique<SparseImage>();
    s->pool = t->pool, s->gpu = gpu, s->d = d;
    s->layer_size = t->L.layer_size, s->size = t->L.size;
    int bw = 1, bh = 1;
    s->bpb = static_cast<u32>(::nvn_fmt_bpb(d.format, &bw, &bh));
    s->bw = bw, s->bh = bh;
    s->astc = f.kind == FmtKind::Astc;
    // The terrain caches (ASTC 8x8 arrays filled by copies from opaque textures; their unwritten tiles have alpha 0, hence BC1 RGBA): BC1 holds the same colours as BC3 in half the
    // memory, and these take gigabytes (at BC3, four times what they take on the console; a 10 GB card ran out).
    static const bool keep_bc3 = getenv("HOST_NVN_TERRAIN_BC3") != nullptr;
    s->bc1 = !keep_bc3 && d.format == 0x8e && d.width == 8192 && d.height == 8192 && layers == 68;
    u32 tw, th, td;
    const int tiled = ::nvn_tex_sparse_tiles(&d, &tw, &th, &td);
    if (tiled <= 0 || !s->bpb || !tw || !th || td != 1) return fail("no 64 KB tiles");
    s->tw = tw, s->th = th;

    // One tile as a texture of its own: one block row of the tile's block height
    s->tile_d = d;
    s->tile_d.target = NVN_TEXTURE_TARGET_2D, s->tile_d.flags = 0, s->tile_d.samples = 0, s->tile_d.stride = 0;
    s->tile_d.width = tw, s->tile_d.height = th, s->tile_d.depth = 1, s->tile_d.levels = 1;
    unsigned sh = 0;
    while ((8u << sh) < th / s->bh) sh++;
    s->tile_d.pkg_block_h = static_cast<int>(sh) + 1;
    {
        nvn_texlayout tl;
        ::nvn_tex_layout(&s->tile_d, &tl);
        if (tl.size != Tile64) return fail("a tile is not 64 KB laid out on its own");
    }

    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.flags = VK_IMAGE_CREATE_SPARSE_BINDING_BIT | VK_IMAGE_CREATE_SPARSE_RESIDENCY_BIT | VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT |
               VK_IMAGE_CREATE_EXTENDED_USAGE_BIT;
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = s->bc1 ? VK_FORMAT_BC1_RGBA_SRGB_BLOCK : f.vk;
    ci.extent = {std::max(d.width, 1u), std::max(d.height, 1u), 1};
    ci.mipLevels = levels;
    ci.arrayLayers = layers;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    {
        u32 n = 0;
        vkGetPhysicalDeviceSparseImageFormatProperties(c.phys, ci.format, ci.imageType, ci.samples, ci.usage, ci.tiling, &n, nullptr);
        if (!n && s->bc1) {
            s->bc1 = false, ci.format = f.vk;
            vkGetPhysicalDeviceSparseImageFormatProperties(c.phys, ci.format, ci.imageType, ci.samples, ci.usage, ci.tiling, &n, nullptr);
        }
        std::vector<VkSparseImageFormatProperties> fp(n);
        vkGetPhysicalDeviceSparseImageFormatProperties(c.phys, ci.format, ci.imageType, ci.samples, ci.usage, ci.tiling, &n, fp.data());
        if (!n) return fail("the format cannot be sparse");
    }
    auto* img = new Image;
    img->serial = t->serial;
    img->desc = d;
    img->nvn_format = static_cast<u32>(d.format);
    img->format = ci.format;
    img->aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    img->width = ci.extent.width, img->height = ci.extent.height;
    img->levels = levels, img->layers = layers;
    img->uploaded = true;  // content comes through sparse_image_use
    img->sparse = s.get();
    s->img = img;
    if (vkCreateImage(c.dev, &ci, nullptr, &img->image) != VK_SUCCESS) {
        delete img;
        return fail("vkCreateImage failed");
    }
    auto drop = [&](const char* why) {
        {
            std::scoped_lock lk{g_mu};
            g_binds.erase(img->image);
            g_opaque.erase(img->image);
        }
        for (VmaAllocation a : s->fixed) free_blocks(a);
        vkDestroyImage(c.dev, img->image, nullptr);
        delete img;
        return fail(why);
    };
    VkMemoryRequirements mr;
    vkGetImageMemoryRequirements(c.dev, img->image, &mr);
    s->block = mr.alignment, s->memtypes = mr.memoryTypeBits;
    u32 nreq = 0;
    vkGetImageSparseMemoryRequirements(c.dev, img->image, &nreq, nullptr);
    std::vector<VkSparseImageMemoryRequirements> reqs(nreq);
    vkGetImageSparseMemoryRequirements(c.dev, img->image, &nreq, reqs.data());
    const VkSparseImageMemoryRequirements* rq = nullptr;
    for (const auto& r : reqs) {
        if (r.formatProperties.aspectMask & VK_IMAGE_ASPECT_METADATA_BIT) return drop("needs metadata memory");
        if (r.formatProperties.aspectMask & VK_IMAGE_ASPECT_COLOR_BIT) rq = &r;
    }
    if (!rq) return drop("no colour sparse requirements");
    s->gran = rq->formatProperties.imageGranularity;
    if (tw % s->gran.width || th % s->gran.height) return drop("its tiles are not whole sparse blocks");

    // Levels: the game's tiled ones that Vulkan also tiles go a tile at a time
    s->tiled = std::min<u32>(static_cast<u32>(tiled), std::min(levels, rq->imageMipTailFirstLod));
    for (u32 L = 0; L <= levels; L++) {
        SparseImage::Level l{};
        l.off = L < levels ? ::nvn_tex_level_offset(&d, L) : s->layer_size;
        l.w = std::max(d.width >> L, 1u), l.h = std::max(d.height >> L, 1u);
        l.tiles_x = (l.w + tw - 1) / tw, l.tiles_y = (l.h + th - 1) / th;
        l.first = s->per_layer;
        if (L < s->tiled) s->per_layer += l.tiles_x * l.tiles_y;
        s->lv.push_back(l);
    }
    for (u32 L = 0; L < s->tiled; L++)
        if (s->lv[L + 1].off - s->lv[L].off < static_cast<u64>(s->lv[L].tiles_x) * s->lv[L].tiles_y * Tile64)
            return drop("its levels are not whole tiles");
    s->tail_off = s->lv[s->tiled].off;
    s->tiles.resize(static_cast<size_t>(s->per_layer) * layers);
    s->tail_dirty.assign(layers, 0);

    // The tail is bound for good: whole levels Vulkan tiles but the game does not, and Vulkan's mip tail
    for (u32 L = s->tiled; L < std::min(levels, rq->imageMipTailFirstLod); L++)
        for (u32 layer = 0; layer < layers; layer++) {
            const VkExtent3D e{s->lv[L].w, s->lv[L].h, 1};
            if (VmaAllocation a = bind_region(s.get(), L, layer, {0, 0, 0}, e)) s->fixed.push_back(a);
            else return drop("no memory for its tail");
        }
    if (rq->imageMipTailFirstLod < levels) {
        const bool single = rq->formatProperties.flags & VK_SPARSE_IMAGE_FORMAT_SINGLE_MIPTAIL_BIT;
        for (u32 layer = 0; layer < (single ? 1 : layers); layer++) {
            VmaAllocationInfo info{};
            const VmaAllocation a = alloc_blocks(s.get(), rq->imageMipTailSize, &info);
            if (!a) return drop("no memory for its mip tail");
            s->fixed.push_back(a);
            VkSparseMemoryBind b{};
            b.resourceOffset = rq->imageMipTailOffset + layer * rq->imageMipTailStride;
            b.size = rq->imageMipTailSize;
            b.memory = info.deviceMemory, b.memoryOffset = info.offset;
            std::scoped_lock lk{g_mu};
            g_opaque[img->image].push_back(b);
        }
    }

    // UNDEFINED -> GENERAL, after this submission's binds
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img->image;
    b.subresourceRange = full_range(img);
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);

    // What the game mapped before the image existed
    for (u32 layer = 0; layer < layers; layer++) {
        for (u32 i = 0; i < s->per_layer; i++) {
            const u32 k = layer * s->per_layer + i;
            if (nvn::sparse_changed_since(s->pool, gpu + tile_at(s.get(), k).off, Tile64, 0)) s->tiles[k].dirty = true, s->queue.push_back(k);
        }
        if (s->tiled < levels &&
            nvn::sparse_changed_since(s->pool, gpu + layer * s->layer_size + s->tail_off, s->layer_size - s->tail_off, 0))
            s->tail_dirty[layer] = 1, s->tail_any = true;
    }
    std::reverse(s->queue.begin(), s->queue.end());  // taken from the back: layer 0 first
    LOG("nvk: sparse image %llu: %ux%u x%u layers, %u levels (%u tiled, Vulkan mip tail from %u), NVN format %#x as %d, flags %#x; tiles %ux%u texels "
        "(%llu KB bound each), %u a layer, %zu mapped so far; strict %d",
        static_cast<unsigned long long>(img->serial), img->width, img->height, layers, levels, s->tiled, rq->imageMipTailFirstLod,
        d.format, ci.format, d.flags, tw, th, static_cast<unsigned long long>(region_bytes(s.get(), {tw, th, 1}) >> 10), s->per_layer,
        s->queue.size(), c.feat.sparse_strict);
    std::scoped_lock lk{g_mu};
    g_list.push_back(s.release());
    return img;
}

void sparse_image_release(Image* img) {
    SparseImage* s = img->sparse;
    {
        std::scoped_lock lk{g_mu};
        if (--s->refs > 0) return;
        g_list.erase(std::remove(g_list.begin(), g_list.end(), s), g_list.end());
        g_binds.erase(img->image);
        g_opaque.erase(img->image);
    }
    log_stats(s, " released");
    const u64 bytes = s->size;
    defer_past_kept([s, img] {
        Device& c = ctx();
        for (auto& [k, v] : img->views)
            if (v) vkDestroyImageView(c.dev, v, nullptr);
        vkDestroyImage(c.dev, img->image, nullptr);
        for (auto& t : s->tiles)
            if (t.mem) free_blocks(t.mem);
        for (VmaAllocation a : s->fixed) free_blocks(a);
        if (s->ev) vmaDestroyBuffer(c.vma, s->ev->buf, s->ev->alloc);
        delete img;
        delete s;
    }, bytes);
}

u64 sparse_bytes() { return g_bytes.load(); }

void sparse_tick(VkCommandBuffer (*pre)()) {
    static const u64 after_frames = [] {
        const char* e = getenv("HOST_NVN_EVICT_S");
        return (e ? std::strtoull(e, nullptr, 10) : 60ull) * 60;
    }();
    if (!after_frames) return;
    std::vector<SparseImage*> list;
    {
        std::scoped_lock lk{g_mu};
        bool any = false;
        for (SparseImage* s : g_list) any |= s->ev && !s->ev->unbound && s->ev->copied->load();
        static u64 at = 0;
        const u64 now = host_ns();
        if (!any && now - at < 5'000'000'000ull) return;
        if (now - at >= 5'000'000'000ull) at = now;
        list = g_list;
    }
    static const bool alias_stats = getenv("HOST_NVN_ALIAS_STATS") != nullptr;
    if (static u64 alias_at = 0; alias_stats && host_ns() - alias_at >= 30'000'000'000ull) alias_at = host_ns(), nvn::sparse_alias_report();
    // Copies done: the tiles go
    for (SparseImage* s : list)
        if (s->ev && !s->ev->unbound && s->ev->copied->load()) {
            for (const u32 i : s->ev->tiles) {
                SparseImage::Tile& t = s->tiles[i];
                if (!t.mem) continue;
                const TileAt at = tile_at(s, i);
                unbind_region(s, at.level, at.layer, at.o, at.e);
                const VmaAllocation a = t.mem;
                defer([a] { free_blocks(a); });
                t.mem = VK_NULL_HANDLE;
                s->bound--;
            }
            s->ev->unbound = true;
            bind_epoch_bump(false);   // (content: kept descriptor sets stay)
            LOG("nvk: sparse image %llu not used for %llu s: %llu MB moved out to host memory", static_cast<unsigned long long>(s->img->serial),
                static_cast<unsigned long long>(after_frames / 60), static_cast<unsigned long long>(s->ev->bytes >> 20));
        }
    // Only when the device memory is short (the driver pages it out otherwise, and making room makes allocations slow)
    VmaBudget b[VK_MAX_MEMORY_HEAPS]{};
    vmaGetHeapBudgets(ctx().vma, b);
    const VkPhysicalDeviceMemoryProperties* mp = nullptr;
    vmaGetMemoryProperties(ctx().vma, &mp);
    u64 used = 0, budget = 0;
    for (u32 h = 0; h < mp->memoryHeapCount; h++)
        if (mp->memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) used += b[h].usage, budget += b[h].budget;
    if (used * 5 < budget * 4) return;
    const u64 frame = nvn::g_frame.load();
    for (SparseImage* s : list)
        if (!s->ev && s->queue.empty() && !s->tail_any && s->bound * region_bytes(s, {s->tw, s->th, 1}) >= (64ull << 20) &&
            s->img->last_use + after_frames < frame)
            evict_start(s, pre());
}

u64 sparse_image_bytes(const Image* img) {
    const SparseImage* s = img->sparse;
    u64 n = 0;
    VmaAllocationInfo info{};
    for (const SparseImage::Tile& t : s->tiles)
        if (t.mem) vmaGetAllocationInfo(ctx().vma, t.mem, &info), n += info.size;
    for (VmaAllocation a : s->fixed) vmaGetAllocationInfo(ctx().vma, a, &info), n += info.size;
    return n;
}

void sparse_note_map(u64 gpu, u64 size) {
    draw_sparse_changed();
    std::scoped_lock lk{g_mu};
    if (g_list.empty()) return;  // a new image looks at the page table itself
    g_dirty.emplace_back(gpu, size);
}

void sparse_image_use(Image* img, VkCommandBuffer pre) {
    SparseImage* s = img->sparse;
    img->last_use = nvn::g_frame.load(std::memory_order_relaxed);
    if (s->ev) {
        if (s->ev->unbound) restore(s, pre);
        else evict_drop(s);   // used before its tiles went: they stay
    }
    drain();
    if (s->queue.empty() && !s->tail_any) return;
    const u64 t0 = host_ns();
    HitchTimer ht{HI_SPARSE};
    std::vector<Job> jobs;
    const size_t batch = s->astc ? 32 : 256;
    while (!s->queue.empty() || s->tail_any) {
        jobs.clear();
        if (s->tail_any) {  // the tails (small) first
            s->tail_any = false;
            for (u32 layer = 0; layer < img->layers; layer++) {
                if (!s->tail_dirty[layer]) continue;
                s->tail_dirty[layer] = 0;
                auto src = std::make_shared<std::vector<u8>>(s->layer_size - s->tail_off);
                nvn::sparse_copy(s->pool, src->data(), s->gpu + layer * s->layer_size + s->tail_off, src->size());
                for (u32 L = s->tiled; L < img->levels; L++) {
                    Job j{&s->d, L, s->lv[L].w, s->lv[L].h, src, s->tail_off};
                    set_region(s, j, L, layer, {0, 0, 0}, {s->lv[L].w, s->lv[L].h, 1});
                    jobs.push_back(std::move(j));
                }
            }
        }
        while (!s->queue.empty() && jobs.size() < batch) {
            const u32 i = s->queue.back();
            s->queue.pop_back();
            SparseImage::Tile& tile = s->tiles[i];
            tile.dirty = false;
            const TileAt at = tile_at(s, i);
            auto src = std::make_shared<std::vector<u8>>(Tile64);
            // A stale alias (its physical pages mapped again at another tile since, the terrain caches' way of recycling
            // them) goes as an unmapped tile: the game no longer samples it, and kept it took a tile of memory each, without
            // end (+1 GB in 2 minutes of racing, then device lost). HOST_NVN_KEEP_ALIASES=1 keeps them.
            static const bool keep_aliases = getenv("HOST_NVN_KEEP_ALIASES") != nullptr;
            if (nvn::sparse_copy(s->pool, src->data(), s->gpu + at.off, Tile64) == Tile64 ||
                (!keep_aliases && nvn::sparse_stale(s->pool, s->gpu + at.off, Tile64))) {  // unmapped
                if (tile.mem) {
                    unbind_region(s, at.level, at.layer, at.o, at.e);
                    const VmaAllocation a = tile.mem;
                    defer([a] { free_blocks(a); });
                    tile.mem = VK_NULL_HANDLE;
                    s->bound--, s->unbinds++;
                }
                continue;
            }
            if (!tile.mem) {
                tile.mem = bind_region(s, at.level, at.layer, at.o, at.e);
                if (!tile.mem) {
                    static bool once = false;
                    if (!once) once = true, LOG("[RED] nvk: sparse image %llu: out of memory for tiles", static_cast<unsigned long long>(img->serial));
                    continue;
                }
                s->bound++;
            }
            Job j{&s->tile_d, 0, s->tw, s->th, src, 0};
            set_region(s, j, at.level, at.layer, at.o, at.e);
            jobs.push_back(std::move(j));
        }
        if (!jobs.empty()) {
            std::for_each(std::execution::par, jobs.begin(), jobs.end(), [s](Job& j) { convert(s, j); });
            size_t total = 0;
            for (Job& j : jobs) j.r.bufferOffset = total, total += (j.out.size() + 15) & ~size_t{15};
            Device& c = ctx();
            VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
            bci.size = total;
            bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
            VmaAllocationCreateInfo ai{};
            ai.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST;
            ai.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
            VkBuffer buf;
            VmaAllocation alloc;
            VmaAllocationInfo info;
            if (vmaCreateBuffer(c.vma, &bci, &ai, &buf, &alloc, &info) == VK_SUCCESS) {
                std::vector<VkBufferImageCopy> regions;
                for (Job& j : jobs) {
                    std::memcpy(static_cast<u8*>(info.pMappedData) + j.r.bufferOffset, j.out.data(), j.out.size());
                    regions.push_back(j.r);
                }
                vmaFlushAllocation(c.vma, alloc, 0, VK_WHOLE_SIZE);
                vkCmdCopyBufferToImage(pre, buf, img->image, VK_IMAGE_LAYOUT_GENERAL, static_cast<u32>(regions.size()), regions.data());
                defer([buf, alloc] { vmaDestroyBuffer(ctx().vma, buf, alloc); });
                s->uploads += jobs.size();
            }
        }
    }
    s->ms_total += (host_ns() - t0) / 1000000;
    if (const u64 now = host_ns(); now - s->last_log > 10'000'000'000ull) s->last_log = now, log_stats(s, "");
}

u64 sparse_commit(VkSemaphore* sem) {
    std::unordered_map<VkImage, std::vector<VkSparseImageMemoryBind>> binds;
    std::unordered_map<VkImage, std::vector<VkSparseMemoryBind>> opaque;
    {
        std::scoped_lock lk{g_mu};
        g_commits++;
        binds.swap(g_binds);
        opaque.swap(g_opaque);
    }
    if (binds.empty() && opaque.empty()) return 0;
    Device& c = ctx();
    if (!g_sem) {
        VkSemaphoreTypeCreateInfo ti{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
        ti.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
        VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        si.pNext = &ti;
        check(vkCreateSemaphore(c.dev, &si, nullptr, &g_sem), "sparse bind semaphore");
    }
    const u64 value = ++g_sem_value;
    // (on the driver thread when it is on, in its place among the submits: the closure owns the binds)
    vkq::run([binds = std::move(binds), opaque = std::move(opaque), value, bsem = g_sem]() mutable {
        Device& c = ctx();
        std::vector<VkSparseImageMemoryBindInfo> ib;
        std::vector<VkSparseImageOpaqueMemoryBindInfo> ob;
        for (auto& [im, v] : binds) ib.push_back({im, static_cast<u32>(v.size()), v.data()});
        for (auto& [im, v] : opaque) ob.push_back({im, static_cast<u32>(v.size()), v.data()});
        VkTimelineSemaphoreSubmitInfo ts{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
        ts.signalSemaphoreValueCount = 1;
        ts.pSignalSemaphoreValues = &value;
        VkBindSparseInfo bi{VK_STRUCTURE_TYPE_BIND_SPARSE_INFO};
        bi.pNext = &ts;
        bi.imageBindCount = static_cast<u32>(ib.size());
        bi.pImageBinds = ib.data();
        bi.imageOpaqueBindCount = static_cast<u32>(ob.size());
        bi.pImageOpaqueBinds = ob.data();
        bi.signalSemaphoreCount = 1;
        bi.pSignalSemaphores = &bsem;
        std::scoped_lock lk{c.queue_lock};
        check(vkQueueBindSparse(c.queue, 1, &bi, VK_NULL_HANDLE), "vkQueueBindSparse");
    });
    *sem = g_sem;
    return value;
}

}  // namespace nvk
