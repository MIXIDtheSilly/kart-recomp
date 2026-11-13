#include "upload.h"

#include <algorithm>
#include <map>
#include "../../par.h"
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
#include <random>
#include <span>
#include <thread>
#include <vector>

#include <windows.h>

#include "../abi.h"
#include "../nvn_int.h"
#include "../replay.h"
#include "hitch.h"
#include "../../cityhash.h"
#include "lz4.h"
#include "zstd.h"
#include "codecs.h"

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

// Big buffers (staging data, files read back) from a pool of blocks freed before, not zero-filled: a fresh 85 MB
// vector cost 15-50 ms in page faults and zeroing each time a texture was read back from the disk cache.
struct BlockPool {
    static constexpr size_t Big = 1u << 20, Keep = 128u << 20;   // pooled from this size; idle bytes kept at most
    std::mutex mu;
    std::multimap<size_t, void*> idle;           // by block size
    std::unordered_map<void*, size_t> size_of;   // every pooled block handed out
    size_t idle_bytes = 0;
    void* get(size_t n) {
        n = (n + Big - 1) & ~(Big - 1);
        {
            std::scoped_lock lk{mu};
            auto it = idle.lower_bound(n);
            if (it != idle.end() && it->first <= n + n / 4) {
                void* p = it->second;
                idle_bytes -= it->first;
                size_of[p] = it->first;
                idle.erase(it);
                return p;
            }
        }
        void* p = ::operator new(n);
        std::scoped_lock lk{mu};
        size_of[p] = n;
        return p;
    }
    void put(void* p) {
        std::unique_lock lk{mu};
        auto it = size_of.find(p);
        const size_t n = it->second;
        size_of.erase(it);
        if (idle_bytes + n <= Keep) {
            idle.emplace(n, p), idle_bytes += n;
            return;
        }
        lk.unlock();
        ::operator delete(p);
    }
};
inline BlockPool g_blocks;

template <typename T>
struct PoolAlloc {
    using value_type = T;
    PoolAlloc() = default;
    template <typename U> PoolAlloc(const PoolAlloc<U>&) {}
    T* allocate(size_t n) {
        const size_t b = n * sizeof(T);
        return static_cast<T*>(b >= BlockPool::Big ? g_blocks.get(b) : ::operator new(b));
    }
    void deallocate(T* p, size_t n) {
        if (n * sizeof(T) >= BlockPool::Big) g_blocks.put(p);
        else ::operator delete(p);
    }
    template <typename U> void construct(U* p) noexcept { ::new (static_cast<void*>(p)) U; }   // (not zeroed)
    template <typename U, typename... A> void construct(U* p, A&&... a) { ::new (static_cast<void*>(p)) U(std::forward<A>(a)...); }
    template <typename U> bool operator==(const PoolAlloc<U>&) const { return true; }
};
using Bytes = std::vector<u8, PoolAlloc<u8>>;

struct Prepared {
    Bytes staging;
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
    Bytes& staging = out.staging;
    std::vector<u8> rgba;
    for (const Untiled::Part& pt : u.parts) {
        const std::span<const u8> blocks(u.blocks.data() + pt.at, pt.bytes);
        const size_t at = staging.size();
        if (astc) {
            rgba.resize(static_cast<size_t>(pt.w) * pt.h * pt.depth * 4);
            codec::astc_decode(blocks, pt.w, pt.h, pt.depth, bw, bh, std::span<u8>(rgba));
            const size_t bytes = static_cast<size_t>((pt.w + 3) / 4) * ((pt.h + 3) / 4) * pt.depth * 16;
            staging.resize(at + bytes);
            codec::bc3_encode(std::span<const u8>(rgba), pt.w, pt.h, pt.depth, std::span<u8>(staging.data() + at, bytes));
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
    u64 h = CityHash64(reinterpret_cast<const char*>(u.blocks.data()), u.blocks.size());
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
        return e ? std::strtoull(e, nullptr, 10) : 256ull;   // (the disk cache is fast now)
    }();
    return mb << 20;
}

u64 cache_key(const Geom& g, const nvn_texdesc& d, const std::vector<u8>& src) {
    u64 h = CityHash64(reinterpret_cast<const char*>(src.data()), src.size());
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
// up to HOST_NVN_TEXCACHE_MB (default 65536); delete the folder to start over.
struct DiskHead {
    u32 magic, version;
    u64 key;
    u32 nregions, _pad;
    u64 staging, packed;
};
// Version 3: the staging data packed in pieces of DiskChunk bytes (u32 count, then each piece's packed size, then the
// pieces), unpacked on several threads: one 85 MB block took 25-185 ms to read back on one thread, and the renderer
// waits for that when the game copies a texture the moment it streams it in. Version 2 (one block) is still read,
// and written again as version 3.
constexpr u32 DiskMagic = 0x4354564E, DiskVersion = 3, DiskVersionOne = 2;   // "NVTC"
constexpr u64 DiskChunk = 4ull << 20;

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
        return e ? std::strtoull(e, nullptr, 10) : 65536ull;
    }();
    return mb << 20;
}

std::string disk_file(u64 key) {
    char b[32];
    std::snprintf(b, sizeof b, "\\%016llx.bc", static_cast<unsigned long long>(key));
    return disk_dir() + b;
}

// The prepared upload of key from the folder; *old: it is in the one-block format (to be written again).
bool disk_get(u64 key, Prepared& out, bool* old) {
    if (disk_dir().empty()) return false;
    // The whole file in one read, by the Win32 calls (not the C runtime's streams)
    const u64 t0 = host_ns();
    const HANDLE fh = CreateFileA(disk_file(key).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (fh == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER fsize{};
    Bytes file;
    bool ok = GetFileSizeEx(fh, &fsize) && fsize.QuadPart >= static_cast<LONGLONG>(sizeof(DiskHead)) && fsize.QuadPart < (1ll << 31);
    const u64 t1 = host_ns();
    if (ok) {
        file.resize(static_cast<size_t>(fsize.QuadPart));
        DWORD got = 0;
        ok = ReadFile(fh, file.data(), static_cast<DWORD>(file.size()), &got, nullptr) && got == file.size();
    }
    CloseHandle(fh);
    const u64 t2 = host_ns();
    DiskHead h{};
    if (ok) std::memcpy(&h, file.data(), sizeof h);
    ok = ok && h.magic == DiskMagic && (h.version == DiskVersion || h.version == DiskVersionOne) && h.key == key && h.nregions &&
         h.nregions < 4096 && h.packed < (1ull << 31) && h.staging < (1ull << 31);
    *old = h.version == DiskVersionOne;
    size_t at = sizeof h;
    const auto take = [&](void* dst, size_t n) {
        if (!ok || file.size() - at < n) return ok = false;
        std::memcpy(dst, file.data() + at, n);
        at += n;
        return true;
    };
    std::vector<u32> sizes;
    if (ok) {
        out.regions.resize(h.nregions);
        take(out.regions.data(), h.nregions * sizeof(VkBufferImageCopy));
        if (!*old) {
            u32 n = 0;
            take(&n, sizeof n);
            ok = ok && n == (h.staging + DiskChunk - 1) / DiskChunk;
            if (ok) sizes.resize(n), take(sizes.data(), n * sizeof(u32));
        } else {
            sizes.push_back(static_cast<u32>(h.packed));
        }
        ok = ok && file.size() - at == h.packed;
    }
    u64 t3 = t2;
    if (ok) {
        const u8* packed = file.data() + at;
        std::vector<u64> off(sizes.size() + 1, 0);
        for (size_t i = 0; i < sizes.size(); i++) off[i + 1] = off[i] + sizes[i];
        ok = off.back() == h.packed;
        if (ok) {
            out.staging.resize(h.staging);
            t3 = host_ns();
            const u64 piece = *old ? h.staging : DiskChunk;
            std::atomic<bool> good{true};
            const auto unpack = [&](size_t i) {
                const u64 o = i * piece, n = std::min(piece, h.staging - o);
                if (LZ4_decompress_safe(reinterpret_cast<const char*>(packed + off[i]), reinterpret_cast<char*>(out.staging.data() + o),
                                        static_cast<int>(sizes[i]), static_cast<int>(n)) != static_cast<int>(n))
                    good.store(false);
            };
            if (sizes.size() == 1) {
                unpack(0);   // (not through the shared thread pool for one piece)
            } else {
                std::vector<size_t> idx(sizes.size());
                for (size_t i = 0; i < idx.size(); i++) idx[i] = i;
                par_for_each(idx.begin(), idx.end(), unpack);
            }
            ok = good.load();
        }
    }
    if (host_ns() - t0 > 20'000'000ull)
        LOG("nvk: disk cache %llu KB: open %llu ms, read %llu, unpacked alloc %llu, unpack %llu", static_cast<unsigned long long>(h.staging >> 10),
            static_cast<unsigned long long>((t1 - t0) / 1'000'000), static_cast<unsigned long long>((t2 - t1) / 1'000'000),
            static_cast<unsigned long long>((t3 - t2) / 1'000'000), static_cast<unsigned long long>((host_ns() - t3) / 1'000'000));
    if (!ok) out = Prepared{};
    return ok;
}

// One LZ4 block; empty if it could not be compressed.
std::vector<u8> lz4_compress(const u8* src, u64 size) {
    std::vector<u8> out(static_cast<size_t>(LZ4_compressBound(static_cast<int>(size))));
    const int n = LZ4_compress_default(reinterpret_cast<const char*>(src), reinterpret_cast<char*>(out.data()), static_cast<int>(size),
                                       static_cast<int>(out.size()));
    out.resize(n > 0 ? static_cast<size_t>(n) : 0);
    return out;
}

// One prepared upload into the folder; the bytes it takes there, 0 when it could not be written.
u64 disk_write(u64 key, const Prepared& p) {
    std::vector<std::vector<u8>> chunks;
    std::vector<u32> sizes;
    u64 total = 0;
    for (u64 o = 0; o < p.staging.size(); o += DiskChunk) {
        chunks.push_back(lz4_compress(p.staging.data() + o, std::min<u64>(DiskChunk, p.staging.size() - o)));
        if (chunks.back().empty()) return 0;
        sizes.push_back(static_cast<u32>(chunks.back().size()));
        total += chunks.back().size();
    }
    const u32 n = static_cast<u32>(chunks.size());
    const DiskHead h{DiskMagic, DiskVersion, key, static_cast<u32>(p.regions.size()), 0, p.staging.size(), total};
    const std::string file = disk_file(key), tmp = file + ".tmp";
    FILE* f = std::fopen(tmp.c_str(), "wb");
    if (!f) return 0;
    bool ok = std::fwrite(&h, sizeof h, 1, f) == 1 &&
              std::fwrite(p.regions.data(), sizeof(VkBufferImageCopy), p.regions.size(), f) == p.regions.size() &&
              std::fwrite(&n, sizeof n, 1, f) == 1 && std::fwrite(sizes.data(), sizeof(u32), n, f) == n;
    for (const std::vector<u8>& c : chunks) ok = ok && std::fwrite(c.data(), 1, c.size(), f) == c.size();
    ok = std::fclose(f) == 0 && ok;
    std::error_code ec;
    if (ok) std::filesystem::rename(tmp, file, ec);
    if (!ok || ec) {
        std::filesystem::remove(tmp, ec);
        return 0;
    }
    return sizeof h + p.regions.size() * sizeof(VkBufferImageCopy) + 4 + n * 4ull + total;
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
        const u64 bytes = disk_write(w.first, p);
        std::scoped_lock lk{g_disk_mu};
        g_disk_queued -= p.staging.size();
        if (bytes) g_disk_bytes += bytes, g_n_disk_put++;
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
        const u64 t1 = host_ns();
        const u64 dk = j.ok && !disk_dir().empty() ? content_key(j.g, j.d, u) : 0;
        const u64 t2 = host_ns();
        bool old = false;
        const bool disk = dk && disk_get(dk, *p, &old);
        if (j.ok && !disk) convert(j.g, j.d, u, *p);
        j.took = host_ns() - t0;
        if (j.took > 30'000'000ull)   // a big one: where its time went
            LOG("nvk: upload %ux%u x%u, %u levels, %llu MB: %llu ms (untile %llu, key %llu, %s %llu)", j.g.width, j.g.height, j.g.layers,
                j.g.levels, static_cast<unsigned long long>(j.size >> 20), static_cast<unsigned long long>(j.took / 1'000'000),
                static_cast<unsigned long long>((t1 - t0) / 1'000'000), static_cast<unsigned long long>((t2 - t1) / 1'000'000),
                disk ? "disk read" : "decode", static_cast<unsigned long long>((host_ns() - t2) / 1'000'000));
        if (disk) {
            if (old) disk_put(dk, p);   // again in pieces
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
            // A few threads (HOST_NVN_UPLOAD_WORKERS, default 4): an upload from the disk cache (untile, hash, read,
            // unpack) runs on one thread only, and the game streams hundreds at once in free roam; with one worker they
            // queued for 250 ms on average (1 s worst) and the renderer stood still for the ones a copy needed. A
            // decode (cache miss) still spreads over the transcode threads (codec::parallel_for).
            const char* e = getenv("HOST_NVN_UPLOAD_WORKERS");
            const int n = std::clamp(e ? std::atoi(e) : 4, 1, 16);
            // At normal priority: below it, a worker waited ~30 ms (two scheduler ticks) for a CPU after each file read
            // while the game's threads ran, and the renderer often waits for the upload a worker has
            // (HOST_NVN_UPLOAD_PRIORITY=<Windows thread priority>, e.g. -1 for below normal).
            const char* pe = getenv("HOST_NVN_UPLOAD_PRIORITY");
            const int prio = pe ? std::atoi(pe) : THREAD_PRIORITY_NORMAL;
            for (int i = 0; i < n; i++)
                std::thread([prio] {
                    SetThreadPriority(GetCurrentThread(), prio);
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

// ------------------------------------------------------------------ the decoded texture cache, made ahead of time

namespace {

struct BakeStats {
    std::atomic<u64> files{0}, astc{0}, have{0}, made{0}, failed{0}, bytes{0};
};

template <typename T>
bool read_at(const std::vector<u8>& b, u64 at, T& v) {
    if (at > b.size() || b.size() - at < sizeof v) return false;
    std::memcpy(&v, b.data() + at, sizeof v);
    return true;
}

// The NVN format of a BNTX format when it is ASTC, else 0: BNTX and NVN list the block sizes in the same order.
int astc_format_of(u32 bntx) {
    const u32 type = bntx >> 8, sub = bntx & 0xff;
    if (type < 0x2d || type > 0x3a || (sub != 1 && sub != 6)) return 0;
    return (sub == 6 ? 0x87 : 0x79) + static_cast<int>(type - 0x2d);
}

// The texture target of a BRTI's image dimension, or -1 for one the game's ASTC textures do not use.
int target_of(u32 dim) {
    switch (dim) {
    case 1: return NVN_TEXTURE_TARGET_2D;
    case 3: return NVN_TEXTURE_TARGET_CUBEMAP;
    case 5: return NVN_TEXTURE_TARGET_2D_ARRAY;
    case 8: return NVN_TEXTURE_TARGET_CUBEMAP_ARRAY;
    default: return -1;
    }
}

// One zstd frame that records its size (the game's .zs files); empty if it does not decode.
std::vector<u8> zstd_decompress(const std::vector<u8>& packed) {
    const unsigned long long size = ZSTD_getFrameContentSize(packed.data(), packed.size());
    if (size == ZSTD_CONTENTSIZE_ERROR || size == ZSTD_CONTENTSIZE_UNKNOWN) return {};
    std::vector<u8> out(static_cast<size_t>(size));
    const size_t n = ZSTD_decompress(out.data(), out.size(), packed.data(), packed.size());
    if (ZSTD_isError(n) || n != out.size()) return {};
    return out;
}

// The ASTC textures of one .bntx.zs, each as the game would hand it to upload_async_start. write false: only counted.
void bake_file(const std::filesystem::path& path, bool write, BakeStats& st) {
    std::vector<u8> packed;
    if (FILE* f = _wfopen(path.c_str(), L"rb")) {
        std::error_code ec;
        packed.resize(std::filesystem::file_size(path, ec));
        const bool ok = std::fread(packed.data(), 1, packed.size(), f) == packed.size();
        std::fclose(f);
        if (!ok) packed.clear();
    }
    const std::vector<u8> b = packed.empty() ? std::vector<u8>{} : zstd_decompress(packed);
    st.files++;
    u32 count = 0;
    u64 infos = 0;
    if (b.size() < 0x30 || std::memcmp(b.data(), "BNTX", 4) != 0 || !read_at(b, 0x24, count) || !read_at(b, 0x28, infos)) {
        st.failed++;
        return;
    }
    for (u32 i = 0; i < count; i++) {
        u64 at = 0, mips_at = 0, first = 0;
        u32 fmt = 0, w = 0, h = 0, arr = 0, layout = 0, dim = 0;
        u16 mips = 0;
        if (!read_at(b, infos + 8ull * i, at) || at + 0x78 > b.size() || std::memcmp(b.data() + at, "BRTI", 4) != 0) {
            st.failed++;
            continue;
        }
        read_at(b, at + 0x16, mips), read_at(b, at + 0x1c, fmt), read_at(b, at + 0x24, w), read_at(b, at + 0x28, h);
        read_at(b, at + 0x30, arr), read_at(b, at + 0x34, layout), read_at(b, at + 0x5c, dim), read_at(b, at + 0x70, mips_at);
        const int format = astc_format_of(fmt), target = target_of(dim);
        if (!format || target < 0) continue;
        st.astc++;
        nvn_texdesc d{};
        d.target = target, d.format = format, d.samples = 1;
        d.width = w, d.height = h, d.depth = target == NVN_TEXTURE_TARGET_2D ? 1 : arr;
        d.levels = mips, d.pkg_block_h = (layout & 0xff) + 1;
        nvn_texlayout L;
        nvn_tex_layout(&d, &L);
        if (!L.size || !read_at(b, mips_at, first) || first > b.size() || b.size() - first < L.size) {
            st.failed++;
            continue;
        }
        const Geom g{VK_IMAGE_TYPE_2D, VK_IMAGE_ASPECT_COLOR_BIT, w, h, 1, std::max(L.layers, 1u), std::max<u32>(mips, 1)};
        Untiled u;
        if (!untile(g, d, b.data() + first, L.size, u)) {
            st.failed++;
            continue;
        }
        const u64 key = content_key(g, d, u);
        std::error_code ec;
        if (std::filesystem::exists(disk_file(key), ec)) {
            st.have++;
            continue;
        }
        if (!write) continue;
        Prepared p;
        convert(g, d, u, p);
        const u64 bytes = disk_write(key, p);
        if (bytes) st.made++, st.bytes += bytes;
        else st.failed++;
    }
}

// The folder's files still in the one-block format written again in pieces (version 3), on `threads` threads.
void repack_disk(u32 threads) {
    std::vector<u64> keys;
    std::error_code ec;
    for (const auto& f : std::filesystem::directory_iterator(disk_dir(), ec))
        if (f.path().extension() == ".bc") keys.push_back(std::strtoull(f.path().stem().string().c_str(), nullptr, 16));
    const u64 t0 = host_ns();
    std::atomic<size_t> next{0}, repacked{0}, failed{0};
    std::vector<std::thread> pool;
    for (u32 i = 0; i < threads; i++)
        pool.emplace_back([&] {
            for (size_t k; (k = next++) < keys.size();) {
                FILE* f = std::fopen(disk_file(keys[k]).c_str(), "rb");
                if (!f) continue;
                DiskHead h{};
                const bool one = std::fread(&h, sizeof h, 1, f) == 1 && h.magic == DiskMagic && h.version == DiskVersionOne;
                std::fclose(f);
                if (!one) continue;
                Prepared p;
                bool old = false;
                if (disk_get(keys[k], p, &old) && disk_write(keys[k], p)) repacked++;
                else failed++;
            }
        });
    for (std::thread& t : pool) t.join();
    LOG("bake: %zu cache files written again in pieces, %zu failed, of %zu (%llu s)", repacked.load(), failed.load(), keys.size(),
        static_cast<unsigned long long>((host_ns() - t0) / 1'000'000'000));
}

}  // namespace

int bake_textures(const std::filesystem::path& game, size_t check) {
    SetPriorityClass(GetCurrentProcess(), BELOW_NORMAL_PRIORITY_CLASS);
    nvn_gob_switch2 = 1;
    const u32 half = std::max(std::thread::hardware_concurrency() / 2, 2u);
    if (!getenv("HOST_TRANSCODE_THREADS")) _putenv_s("HOST_TRANSCODE_THREADS", std::to_string(half).c_str());
    if (disk_dir().empty()) {
        LOG("[RED] bake: the decoded texture cache is turned off (HOST_NVN_TEXCACHE=0)");
        return 1;
    }
    if (!check) repack_disk(half);
    std::vector<std::filesystem::path> files;
    std::error_code ec;
    for (const auto& f : std::filesystem::recursive_directory_iterator(game / "romfs", ec))
        if (f.path().native().ends_with(L".bntx.zs")) files.push_back(f.path());
    std::shuffle(files.begin(), files.end(), std::mt19937{1});   // even progress: the big textures are not all at the end
    if (check) files.resize(std::min(check, files.size()));
    LOG("bake: %zu texture files, %s", files.size(), check ? "counting only" : "decoding what the cache does not have");

    BakeStats st;
    std::atomic<size_t> next{0};
    std::vector<std::thread> threads;
    for (u32 i = 0; i < std::max(half / 3, 2u); i++)
        threads.emplace_back([&] {
            for (size_t k; (k = next++) < files.size();) bake_file(files[k], !check, st);
        });
    const u64 t0 = host_ns();
    u64 said = 0;
    const auto report = [&](const char* what, u64 secs) {
        LOG("bake: %s %llu of %zu files, %llu s: %llu ASTC textures, %llu already there, %llu decoded (%llu MB), %llu failed", what,
            static_cast<unsigned long long>(st.files.load()), files.size(), static_cast<unsigned long long>(secs),
            static_cast<unsigned long long>(st.astc.load()), static_cast<unsigned long long>(st.have.load()),
            static_cast<unsigned long long>(st.made.load()), static_cast<unsigned long long>(st.bytes.load() >> 20),
            static_cast<unsigned long long>(st.failed.load()));
    };
    while (st.files.load() < files.size()) {
        Sleep(200);
        const u64 secs = (host_ns() - t0) / 1'000'000'000;
        if (secs >= said + 10) said = secs, report("at", secs);
    }
    for (std::thread& t : threads) t.join();
    report("done:", (host_ns() - t0) / 1'000'000'000);
    return 0;
}

}  // namespace nvk
