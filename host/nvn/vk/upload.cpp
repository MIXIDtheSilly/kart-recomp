#include "upload.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <list>
#include <memory>
#include <mutex>
#include <span>
#include <thread>
#include <vector>

#include <windows.h>

#include "../abi.h"
#include "../nvn_int.h"
#include "../replay.h"
#include "hitch.h"
#include "common/cityhash.h"
#include "common/lz4_compression.h"
#include "video_core/textures/astc.h"
#include "video_core/textures/bcn.h"

namespace nvk {
namespace {

void red_once(const char* what, int key) {
    static std::mutex m;
    static std::unordered_map<std::string, bool> seen;
    std::scoped_lock lk{m};
    const std::string k = std::string(what) + std::to_string(key);
    if (seen.emplace(k, true).second) LOG("[RED] nvk: %s (%d): texture content not uploaded", what, key);
}

}  // namespace

namespace {

// What the CPU part of an upload needs of the image (fixed at creation): a copy, so a worker never touches an Image
// destroyed meanwhile.
struct Geom {
    VkImageType type;
    VkImageAspectFlags aspect;
    u32 width, height, depth, layers, levels;
};

Geom geom_of(const Image* img) { return {img->type, img->aspect, img->width, img->height, img->depth, img->layers, img->levels}; }

struct Prepared {
    std::vector<u8> staging;
    std::vector<VkBufferImageCopy> regions;
};

// The storage bytes (a sparse pool's pages are elsewhere: copied out), or null when there is nothing to read.
const u8* storage_of(const nvn::tex_t* t, std::vector<u8>& copy, bool always_copy) {
    if (!t->mem) return nullptr;
    const u64 size = t->L.size;
    if (t->pool && nvn::sparse_pool(t->pool)) {
        copy.resize(size);
        nvn::sparse_copy(t->pool, copy.data(), nvn::ga(t->mem), size);
        return copy.data();
    }
    if (!always_copy) return t->mem;
    copy.assign(t->mem, t->mem + size);
    return copy.data();
}

// Whether img can take content from storage at all (logged once when not).
bool uploadable(const Image* img, const nvn_texdesc& d) {
    if (img->samples != VK_SAMPLE_COUNT_1_BIT) return false;
    int bw = 1, bh = 1;
    if (!::nvn_fmt_bpb(d.format, &bw, &bh)) {
        red_once("unknown bytes per block for format", d.format);
        return false;
    }
    if (img->aspect == (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) {
        // Packed depth-stencil storage cannot be copied as one aspect: game data of this kind is rare.
        red_once("depth-stencil texture content from memory, format", d.format);
        return false;
    }
    return true;
}

// Every level and layer of the storage untiled: the texture's blocks only (tiling padding, which may hold anything,
// left out).
struct Untiled {
    struct Part {
        u32 level, layer, w, h, depth, bx, by;
        size_t at, bytes;   // in blocks
    };
    std::vector<Part> parts;
    std::vector<u8> blocks;
};

// The CPU part, first half (any thread).
bool untile(const Geom& g, const nvn_texdesc& d, const u8* storage, u64 storage_size, Untiled& u) {
    int bw = 1, bh = 1;
    const int bpb = ::nvn_fmt_bpb(d.format, &bw, &bh);
    const bool linear = (d.flags & (NVN_TEXTURE_FLAGS_LINEAR | NVN_TEXTURE_FLAGS_LINEAR_RENDER_TARGET)) != 0;
    const u32 layers = g.type == VK_IMAGE_TYPE_3D ? 1 : g.layers;
    for (u32 level = 0; level < g.levels; level++) {
        const u32 w = std::max(g.width >> level, 1u), h = std::max(g.height >> level, 1u);
        const u32 depth = g.type == VK_IMAGE_TYPE_3D ? std::max(g.depth >> level, 1u) : 1;
        const u32 bx = (w + bw - 1) / bw, by = (h + bh - 1) / bh;
        const size_t pitch = static_cast<size_t>(bx) * bpb;
        for (u32 layer = 0; layer < layers; layer++) {
            // LINEAR: rows of `stride` bytes, one level and layer (render targets mostly).
            if (linear && (level || layer)) continue;
            const size_t at = u.blocks.size(), bytes = pitch * by * depth;
            u.blocks.resize(at + bytes);
            u8* out = u.blocks.data() + at;
            if (linear) {
                const size_t stride = d.stride ? d.stride : pitch;
                for (u32 y = 0; y < by; y++)
                    if ((y + 1) * stride <= storage_size) std::memcpy(out + y * pitch, storage + y * stride, std::min(pitch, stride));
                    else std::memset(out + y * pitch, 0, pitch);
            } else if (::nvn_deswizzle(&d, level, layer, storage, out, pitch) != 0) {
                red_once("block-linear layout not handled for target", d.target);
                return false;
            }
            u.parts.push_back({level, layer, w, h, depth, bx, by, at, bytes});
        }
    }
    return !u.parts.empty();
}

// The CPU part, second half: staging data (ASTC decoded and recompressed to BC3) and copy regions (any thread).
void convert(const Geom& g, const nvn_texdesc& d, const Untiled& u, Prepared& out) {
    int bw = 1, bh = 1;
    ::nvn_fmt_bpb(d.format, &bw, &bh);
    const bool astc = format(static_cast<u32>(d.format)).kind == FmtKind::Astc;
    std::vector<u8>& staging = out.staging;
    std::vector<u8> rgba;
    for (const Untiled::Part& pt : u.parts) {
        const std::span<const u8> blocks(u.blocks.data() + pt.at, pt.bytes);
        const size_t at = staging.size();
        if (astc) {
            rgba.resize(static_cast<size_t>(pt.w) * pt.h * pt.depth * 4);
            Tegra::Texture::ASTC::Decompress(blocks, pt.w, pt.h, pt.depth, bw, bh, std::span<u8>(rgba));
            const size_t bytes = static_cast<size_t>((pt.w + 3) / 4) * ((pt.h + 3) / 4) * pt.depth * 16;
            staging.resize(at + bytes);
            Tegra::Texture::BCN::CompressBC3(std::span<const u8>(rgba), pt.w, pt.h, pt.depth, std::span<u8>(staging.data() + at, bytes));
        } else {
            staging.insert(staging.end(), blocks.begin(), blocks.end());
        }
        staging.resize((staging.size() + 15) & ~size_t{15});
        VkBufferImageCopy r{};
        r.bufferOffset = at;
        r.bufferRowLength = astc ? (pt.w + 3) & ~3u : pt.bx * bw;
        r.bufferImageHeight = astc ? (pt.h + 3) & ~3u : pt.by * bh;
        r.imageSubresource = {g.aspect & ~VK_IMAGE_ASPECT_STENCIL_BIT, pt.level, pt.layer, 1};
        if (g.aspect == VK_IMAGE_ASPECT_STENCIL_BIT) r.imageSubresource.aspectMask = VK_IMAGE_ASPECT_STENCIL_BIT;
        r.imageExtent = {pt.w, pt.h, pt.depth};
        out.regions.push_back(r);
    }
}

// The whole CPU part: every level and layer into staging data and copy regions (any thread).
bool prepare(const Geom& g, const nvn_texdesc& d, const u8* storage, u64 storage_size, Prepared& out) {
    Untiled u;
    if (!untile(g, d, storage, storage_size, u)) return false;
    convert(g, d, u, out);
    return true;
}

// What the prepared data of untiled u depends on (the disk cache's key: the same in every run, as the storage's
// tiling padding is not in it).
u64 content_key(const Geom& g, const nvn_texdesc& d, const Untiled& u) {
    u64 h = Common::CityHash64(reinterpret_cast<const char*>(u.blocks.data()), u.blocks.size());
    for (const u64 v : {static_cast<u64>(g.type), static_cast<u64>(g.aspect), static_cast<u64>(g.width), static_cast<u64>(g.height),
                        static_cast<u64>(g.depth), static_cast<u64>(g.layers), static_cast<u64>(g.levels), static_cast<u64>(d.format)})
        h = (h ^ v) * 0xC2B2AE3D27D4EB4Full + 0x165667B19E3779F9ull;
    return h ? h : 1;
}

// The GPU part: a staging buffer with the prepared data, copied into img in cb.
bool record(Image* img, const Prepared& p, VkCommandBuffer cb) {
    Device& c = ctx();
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = p.staging.size();
    bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    VmaAllocationCreateInfo ai{};
    ai.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST;
    ai.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
    VkBuffer buf;
    VmaAllocation alloc;
    VmaAllocationInfo info;
    if (vmaCreateBuffer(c.vma, &bci, &ai, &buf, &alloc, &info) != VK_SUCCESS) {
        red_once("staging buffer allocation failed, bytes", static_cast<int>(p.staging.size()));
        return false;
    }
    std::memcpy(info.pMappedData, p.staging.data(), p.staging.size());
    vmaFlushAllocation(c.vma, alloc, 0, VK_WHOLE_SIZE);
    vkCmdCopyBufferToImage(cb, buf, img->image, VK_IMAGE_LAYOUT_GENERAL, static_cast<u32>(p.regions.size()), p.regions.data());
    defer([buf, alloc] { vmaDestroyBuffer(ctx().vma, buf, alloc); });
    img->has_content = true;
    return true;
}

}  // namespace

bool upload_from_storage(Image* img, const nvn::tex_t* t, VkCommandBuffer cb) {
    if (!uploadable(img, t->d)) return false;
    HitchTimer ht{HI_UPLOAD};
    std::vector<u8> copy;
    const u8* storage = storage_of(t, copy, false);
    if (!storage) return false;
    Prepared p;
    if (!prepare(geom_of(img), t->d, storage, t->L.size, p)) return false;
    return record(img, p, cb);
}

// ------------------------------------------------------------------ uploads on a worker thread

struct AsyncUpload {
    std::atomic<int> state{0};   // 0 queued, 1 being prepared, 2 done
    std::atomic<bool> cancel{false};
    bool ok = false;
    Geom g{};
    nvn_texdesc d{};
    u64 size = 0;
    std::vector<u8> src;         // the storage as it was when the upload was asked for
    u64 key = 0;                 // its content key (cache_key)
    std::shared_ptr<const Prepared> out;
    u64 queued = 0, took = 0;    // host ns
    bool missed = false;         // a draw needed it before it was ready
};

namespace {

std::mutex g_q_mu;
std::condition_variable g_q_cv, g_done_cv;
std::deque<std::shared_ptr<AsyncUpload>> g_q;
// statistics (under g_q_mu), logged every 10 s
u64 g_n_done = 0, g_ms_work = 0, g_ms_wait = 0, g_n_sync = 0, g_worst_wait = 0, g_stats_at = 0, g_n_hit = 0;
// waits for one: ms the renderer stood still, how many it prepared itself (not started yet), the longest
u64 g_ms_sync = 0, g_n_sync_here = 0, g_sync_worst_ms = 0;
char g_sync_worst[96];

// The game streams the same textures in again and again (the track's as the kart goes round, each race's): prepared
// uploads by content, the least recently used dropped past HOST_NVN_UPLOAD_CACHE_MB (default 1024; 0 = none).
struct CacheEntry {
    std::shared_ptr<const Prepared> p;
    std::list<u64>::iterator at;
};
std::mutex g_c_mu;
std::unordered_map<u64, CacheEntry> g_cache;
std::list<u64> g_lru;   // most recent first
u64 g_cache_bytes = 0;

u64 cache_max() {
    static const u64 mb = [] {
        const char* e = getenv("HOST_NVN_UPLOAD_CACHE_MB");
        return e ? std::strtoull(e, nullptr, 10) : 1024ull;
    }();
    return mb << 20;
}

u64 cache_key(const Geom& g, const nvn_texdesc& d, const std::vector<u8>& src) {
    u64 h = Common::CityHash64(reinterpret_cast<const char*>(src.data()), src.size());
    for (const u64 v : {static_cast<u64>(g.type), static_cast<u64>(g.width), static_cast<u64>(g.height), static_cast<u64>(g.depth),
                        static_cast<u64>(g.layers), static_cast<u64>(g.levels), static_cast<u64>(d.format), static_cast<u64>(d.flags),
                        static_cast<u64>(d.target), static_cast<u64>(d.stride)})
        h = (h ^ v) * 0xC2B2AE3D27D4EB4Full + 0x165667B19E3779F9ull;
    return h;
}

std::shared_ptr<const Prepared> cache_get(u64 key) {
    std::scoped_lock lk{g_c_mu};
    auto it = g_cache.find(key);
    if (it == g_cache.end()) return nullptr;
    g_lru.splice(g_lru.begin(), g_lru, it->second.at);
    return it->second.p;
}

void cache_put(u64 key, std::shared_ptr<const Prepared> p) {
    const u64 bytes = p->staging.size();
    if (bytes > cache_max() / 4) return;
    std::scoped_lock lk{g_c_mu};
    if (g_cache.count(key)) return;
    g_lru.push_front(key);
    g_cache.emplace(key, CacheEntry{std::move(p), g_lru.begin()});
    g_cache_bytes += bytes;
    while (g_cache_bytes > cache_max() && !g_lru.empty()) {
        auto it = g_cache.find(g_lru.back());
        g_cache_bytes -= it->second.p->staging.size();
        g_cache.erase(it);
        g_lru.pop_back();
    }
}

// Prepared uploads on disk, by content key (content_key), so an ASTC texture is decoded once and not again in later runs:
// work\nvn_texcache\<key>.bc (HOST_NVN_TEXCACHE=<dir>, or 0 for none), the staging data LZ4-packed. Files are added
// up to HOST_NVN_TEXCACHE_MB (default 16384); delete the folder to start over.
struct DiskHead {
    u32 magic, version;
    u64 key;
    u32 nregions, _pad;
    u64 staging, packed;
};
constexpr u32 DiskMagic = 0x4354564E, DiskVersion = 2;   // "NVTC"

std::mutex g_disk_mu;
std::condition_variable g_disk_cv;
std::deque<std::pair<u64, std::shared_ptr<const Prepared>>> g_disk_q;   // to write
u64 g_disk_bytes = 0, g_disk_queued = 0;    // in the folder; staging bytes waiting to be written
u64 g_n_missed = 0;   // statistics (under g_q_mu): uploads drawn before they were ready, the texture showing the placeholder
u64 g_n_disk = 0, g_ms_disk = 0, g_n_disk_put = 0;    // statistics (under g_q_mu): read back, ms reading, written

const std::string& disk_dir() {
    static const std::string dir = [] {
        std::string d;
        if (const char* e = getenv("HOST_NVN_TEXCACHE")) {
            if (std::strcmp(e, "0") != 0) d = e;
        } else {
            char exe[MAX_PATH];
            const DWORD n = GetModuleFileNameA(nullptr, exe, MAX_PATH);
            std::filesystem::path p(std::string(exe, n));
            d = (p.parent_path().parent_path().parent_path() / "work" / "nvn_texcache").string();   // ...\recomp\work
        }
        if (d.empty()) return d;
        std::error_code ec;
        std::filesystem::create_directories(d, ec);
        u64 files = 0;
        for (const auto& f : std::filesystem::directory_iterator(d, ec))
            if (f.path().extension() == ".bc") g_disk_bytes += f.file_size(ec), files++;
        LOG("nvk: decoded texture cache %s: %llu files, %llu MB", d.c_str(), static_cast<unsigned long long>(files),
            static_cast<unsigned long long>(g_disk_bytes >> 20));
        return d;
    }();
    return dir;
}

u64 disk_max() {
    static const u64 mb = [] {
        const char* e = getenv("HOST_NVN_TEXCACHE_MB");
        return e ? std::strtoull(e, nullptr, 10) : 16384ull;
    }();
    return mb << 20;
}

std::string disk_file(u64 key) {
    char b[32];
    std::snprintf(b, sizeof b, "\\%016llx.bc", static_cast<unsigned long long>(key));
    return disk_dir() + b;
}

bool disk_get(u64 key, Prepared& out) {
    if (disk_dir().empty()) return false;
    FILE* f = std::fopen(disk_file(key).c_str(), "rb");
    if (!f) return false;
    DiskHead h{};
    std::vector<u8> packed;
    bool ok = std::fread(&h, sizeof h, 1, f) == 1 && h.magic == DiskMagic && h.version == DiskVersion && h.key == key &&
              h.nregions && h.nregions < 4096 && h.packed < (1ull << 31) && h.staging < (1ull << 31);
    if (ok) {
        out.regions.resize(h.nregions);
        packed.resize(h.packed);
        ok = std::fread(out.regions.data(), sizeof(VkBufferImageCopy), h.nregions, f) == h.nregions &&
             std::fread(packed.data(), 1, h.packed, f) == h.packed;
    }
    std::fclose(f);
    if (ok) {
        out.staging.resize(h.staging);
        ok = Common::Compression::DecompressDataLZ4(out.staging.data(), out.staging.size(), packed.data(), packed.size()) ==
             static_cast<int>(h.staging);
    }
    if (!ok) out = Prepared{};
    return ok;
}

void disk_writer() {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_LOWEST);
    for (;;) {
        std::pair<u64, std::shared_ptr<const Prepared>> w;
        {
            std::unique_lock lk{g_disk_mu};
            g_disk_cv.wait(lk, [] { return !g_disk_q.empty(); });
            w = std::move(g_disk_q.front());
            g_disk_q.pop_front();
        }
        const Prepared& p = *w.second;
        const std::vector<u8> packed = Common::Compression::CompressDataLZ4(p.staging.data(), p.staging.size());
        const DiskHead h{DiskMagic, DiskVersion, w.first, static_cast<u32>(p.regions.size()), 0, p.staging.size(), packed.size()};
        const std::string file = disk_file(w.first), tmp = file + ".tmp";
        bool ok = !packed.empty();
        if (FILE* f = ok ? std::fopen(tmp.c_str(), "wb") : nullptr) {
            ok = std::fwrite(&h, sizeof h, 1, f) == 1 &&
                 std::fwrite(p.regions.data(), sizeof(VkBufferImageCopy), p.regions.size(), f) == p.regions.size() &&
                 std::fwrite(packed.data(), 1, packed.size(), f) == packed.size();
            ok = std::fclose(f) == 0 && ok;
            std::error_code ec;
            if (ok) std::filesystem::rename(tmp, file, ec);
            if (!ok || ec) std::filesystem::remove(tmp, ec), ok = false;
        } else {
            ok = false;
        }
        std::scoped_lock lk{g_disk_mu};
        g_disk_queued -= p.staging.size();
        if (ok) g_disk_bytes += sizeof h + p.regions.size() * sizeof(VkBufferImageCopy) + packed.size(), g_n_disk_put++;
    }
}

void disk_put(u64 key, std::shared_ptr<const Prepared> p) {
    if (disk_dir().empty()) return;
    std::scoped_lock lk{g_disk_mu};
    if (g_disk_bytes >= disk_max()) {
        static bool once = false;
        if (!once) once = true, LOG("nvk: decoded texture cache full (HOST_NVN_TEXCACHE_MB): no more files added");
        return;
    }
    if (g_disk_queued > (1ull << 30)) return;   // the writer is far behind: not this one
    static bool started = false;
    if (!started) started = true, std::thread(disk_writer).detach();
    g_disk_queued += p->staging.size();
    g_disk_q.emplace_back(key, std::move(p));
    g_disk_cv.notify_one();
}

void run(AsyncUpload& j) {
    if (!j.cancel.load()) {
        const u64 t0 = host_ns();
        auto p = std::make_shared<Prepared>();
        Untiled u;
        j.ok = untile(j.g, j.d, j.src.data(), j.size, u);
        const u64 dk = j.ok && !disk_dir().empty() ? content_key(j.g, j.d, u) : 0;
        const bool disk = dk && disk_get(dk, *p);
        if (j.ok && !disk) convert(j.g, j.d, u, *p);
        j.took = host_ns() - t0;
        if (disk) {
            std::scoped_lock lk{g_q_mu};
            g_n_disk++, g_ms_disk += j.took / 1'000'000;
        } else if (dk) {
            disk_put(dk, p);
        }
        if (j.ok && cache_max()) cache_put(j.key, p);
        j.out = std::move(p);
    }
    std::vector<u8>().swap(j.src);
    {
        std::scoped_lock lk{g_q_mu};
        j.state.store(2);
        const u64 wait = (host_ns() - j.queued) / 1'000'000;
        g_n_done++, g_ms_work += j.took / 1'000'000, g_ms_wait += wait;
        g_worst_wait = std::max(g_worst_wait, wait);
    }
    g_done_cv.notify_all();
}

void worker() {
    for (;;) {
        std::shared_ptr<AsyncUpload> j;
        {
            std::unique_lock lk{g_q_mu};
            g_q_cv.wait(lk, [] { return !g_q.empty(); });
            j = std::move(g_q.front());
            g_q.pop_front();
        }
        int queued = 0;
        if (j->state.compare_exchange_strong(queued, 1)) run(*j);
    }
}

// (g_q_mu held)
void log_stats() {
    const u64 now = host_ns();
    if (now - g_stats_at < 10'000'000'000ull) return;
    if (g_stats_at)
        LOG("nvk: uploads on the worker: %llu done (%llu ms of work, %llu ms average from asked to done, worst %llu ms), %llu waited for, "
            "%zu queued, %llu drawn before ready; %llu from the cache (%llu MB in it), %llu from disk (%llu ms), %llu written",
            static_cast<unsigned long long>(g_n_done), static_cast<unsigned long long>(g_ms_work),
            static_cast<unsigned long long>(g_n_done ? g_ms_wait / g_n_done : 0), static_cast<unsigned long long>(g_worst_wait),
            static_cast<unsigned long long>(g_n_sync), g_q.size(), static_cast<unsigned long long>(g_n_missed), static_cast<unsigned long long>(g_n_hit),
            static_cast<unsigned long long>(g_cache_bytes >> 20), static_cast<unsigned long long>(g_n_disk),
            static_cast<unsigned long long>(g_ms_disk), static_cast<unsigned long long>(g_n_disk_put));
    if (g_stats_at && g_n_sync)
        LOG("nvk: uploads waited for: %llu ms in all (%llu prepared on the renderer thread), longest %llu ms: %s",
            static_cast<unsigned long long>(g_ms_sync), static_cast<unsigned long long>(g_n_sync_here),
            static_cast<unsigned long long>(g_sync_worst_ms), g_sync_worst);
    g_stats_at = now, g_n_done = g_ms_work = g_ms_wait = g_n_sync = g_worst_wait = g_n_hit = 0;
    g_ms_sync = g_n_sync_here = g_sync_worst_ms = g_n_missed = g_n_disk = g_ms_disk = g_n_disk_put = 0;
}

}  // namespace

bool upload_async_start(Image* img, const nvn::tex_t* t) {
    static const bool off = getenv("HOST_NVN_SYNC_UPLOADS") != nullptr;
    if (off || format(static_cast<u32>(t->d.format)).kind != FmtKind::Astc) return false;
    if (!t->mem || !uploadable(img, t->d)) return true;   // nothing to upload (as on the synchronous path)
    auto j = std::make_shared<AsyncUpload>();
    j->g = geom_of(img);
    j->d = t->d;
    j->size = t->L.size;
    storage_of(t, j->src, true);
    j->queued = host_ns();
    img->pending = j;
    if (cache_max() || !disk_dir().empty()) {
        j->key = cache_key(j->g, j->d, j->src);
        if (cache_max() && (j->out = cache_get(j->key))) {
            j->ok = true;
            j->state.store(2);
            std::vector<u8>().swap(j->src);
            std::scoped_lock lk{g_q_mu};
            g_n_hit++;
            log_stats();
            return true;
        }
    }
    {
        std::scoped_lock lk{g_q_mu};
        static bool started = false;
        if (!started) {
            started = true;
            // One thread: the decode inside spreads over the ASTC worker pool (video_core/textures/workers.cpp).
            std::thread([] {
                SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
                worker();
            }).detach();
        }
        g_q.push_back(std::move(j));
        log_stats();
    }
    g_q_cv.notify_one();
    return true;
}

namespace {

// A texture drawn before its upload is ready shows the placeholder (black) for a frame or more. Most are ready within
// milliseconds, so a frame may spend this long on the uploads its draws need (HOST_NVN_UPLOAD_WAIT_MS, default 8; 0: never).
// Not during a burst (a level loading): waiting then costs frames and gets few of the textures.
u64 g_wait_frame = ~0ull, g_wait_left = 0;   // ns (renderer thread)

u64 wait_left() {
    {
        std::scoped_lock lk{g_q_mu};
        if (g_q.size() > 16) return 0;
    }
    static const u64 per_frame = [] {
        const char* s = getenv("HOST_NVN_UPLOAD_WAIT_MS");
        return (s ? std::strtoull(s, nullptr, 10) : 8) * 1'000'000;
    }();
    const u64 frame = nvn::g_frame.load(std::memory_order_relaxed);
    if (frame != g_wait_frame) g_wait_frame = frame, g_wait_left = per_frame;
    return g_wait_left;
}

// About what preparing an upload from the disk cache costs (136 ms for an 8192x8192 texture of 89 MB).
u64 estimate_ns(const AsyncUpload& j) { return j.size * 3 / 2; }

bool not_ready(const Image* img, AsyncUpload& j) {
    if (j.missed || img->has_content) return false;
    j.missed = true;
    std::scoped_lock lk{g_q_mu};
    g_n_missed++;
    return false;
}

}  // namespace

bool upload_async_poll(Image* img, VkCommandBuffer cb, bool wait) {
    std::shared_ptr<AsyncUpload> j = img->pending;
    if (!j) return true;
    if (j->state.load() != 2) {
        const u64 left = wait ? 0 : wait_left();
        // Not started and more than is left of this frame: waiting could not get it.
        if (!wait && (!left || (j->state.load() == 0 && estimate_ns(*j) > left))) return not_ready(img, *j);
        const u64 t0 = host_ns();
        HitchTimer ht{HI_UPLOAD_WAIT};
        int queued = 0;
        const bool here = j->state.compare_exchange_strong(queued, 1);
        if (here) {
            run(*j);   // not started yet: here
        } else {
            std::unique_lock lk{g_q_mu};
            const auto done = [&] { return j->state.load() == 2; };
            if (wait) {
                g_done_cv.wait(lk, done);
            } else if (!g_done_cv.wait_for(lk, std::chrono::nanoseconds(left), done)) {
                lk.unlock();
                g_wait_left = 0;
                return not_ready(img, *j);
            }
        }
        if (!wait) g_wait_left -= std::min(g_wait_left, host_ns() - t0);
        const u64 ms = (host_ns() - t0) / 1'000'000;
        std::scoped_lock lk{g_q_mu};
        if (!g_n_sync || ms > g_sync_worst_ms) {
            g_sync_worst_ms = ms;
            std::snprintf(g_sync_worst, sizeof g_sync_worst, "image %llu %ux%u x%u, %u levels, NVN format %#x%s",
                          static_cast<unsigned long long>(img->serial), img->width, img->height, img->layers, img->levels, img->nvn_format,
                          here ? " (prepared here)" : "");
        }
        g_n_sync++, g_ms_sync += ms, g_n_sync_here += here;
    }
    img->pending.reset();
    if (j->ok && j->out && !img->gpu_written) record(img, *j->out, cb);
    return true;
}

void upload_async_cancel(Image* img) {
    if (img->pending) img->pending->cancel.store(true);
    img->pending.reset();
}

}  // namespace nvk
