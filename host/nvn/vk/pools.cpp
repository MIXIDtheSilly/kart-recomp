#include "pools.h"
#include "draw.h"
#include "replay_async.h"
#include "textures.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "../../memory.h"

namespace nvn {
extern std::atomic<uint64_t> g_frame;   // presents so far (window.cpp)
}

namespace nvk {
namespace {

// The driver takes ~75 ms per VK_EXT_external_memory_host import whatever its size (measured on the
// guest view and on the backing pool's mapping alike; the game makes thousands of pools). So the
// backing pool is imported in windows: window k covers physical [k * Step, k * Step + 2 * Step), and a
// pool of at most Step bytes that is one physical run lies whole in the window of its first page.
// Larger pools, and pools whose pages are not one run, get an import of their own.
constexpr u64 Step = 128ull << 20;

struct Pool {
    u64 size = 0;
    VkBuffer buffer = VK_NULL_HANDLE;
    u64 offset = 0;          // of the pool's first byte in buffer
    VkDeviceAddress gpu = 0; // of the pool's first byte
    bool own = false;        // im is the pool's own import
    Imported im;
    // A copy in device memory, read instead of the import: see mirror_make.
    u32 flags = 0;
    bool mirror_tried = false;
    VkBuffer mirror = VK_NULL_HANDLE;
    VmaAllocation mirror_alloc = VK_NULL_HANDLE;
    VkDeviceAddress mirror_gpu = 0;
    u64 flush_frame = 0;
    u32 flush_frames = 0;      // frames in which the CPU published some of it
    u64 flush_bytes = 0;       // how much, in those frames
    bool refresh = false;        // the copy is made again every part of a submission that uses the pool
    u64 used_sub = 0;            // the submission that last used it (refresh)
    u64 copied = 0;              // its 4 KB pages copied in that submission
    bool gpu_written = false;
    bool owned = false;          // the copy is the GPU's own: what the GPU wrote is only there
    bool pinned = false;         // the renderer writes it on the CPU: system memory for good
    bool promote_queued = false;
    bool shared = false;         // was the GPU's own until the CPU published or asked for some of it: not again
    u64 quiet_from = 0;          // the frame of the first GPU write, or the last time the CPU published or asked for some of it
    const char* where = "system memory";   // (for the statistics: why it has no copy)
};
std::shared_mutex g_lock;
std::map<u64, Pool> g_pools;  // by guest start; pools may overlap (aliases of the same storage)
std::atomic<u64> g_pools_gen{1};  // steps at every change of g_pools
std::atomic<u64> g_bumps[4];      // bind_epoch_bump by cause: copy made, copy dropped, pool added, pool gone (statistics)

// A change of what an address resolves to steps g_bind_objects (the kept descriptor sets' keys: every one is made again,
// ~3,000) only when a Vulkan object goes, whose handle could come back as another one: an own import or a device copy.
// Otherwise (a pool over a shared import window, a copy made) the kept sets stay, and keep_find sees which resolve
// otherwise now. Pools come and go 10-15 times a second in a race. HOST_NVN_FAST=0: always.
bool objects_only_when_freed() {
    static const bool on = [] {
        const char* e = getenv("HOST_NVN_FAST");
        return !(e && *e == '0');
    }();
    return on;
}
std::mutex g_window_lock;
std::unordered_map<u64, Imported> g_windows;  // by k

// The window holding physical [phys, phys + size); imports it the first time.
const Imported* window(u64 phys, u64 size) {
    const u64 k = phys / Step;
    if (phys + size > (k + 2) * Step) return nullptr;
    std::scoped_lock lk{g_window_lock};
    if (auto it = g_windows.find(k); it != g_windows.end()) return &it->second;
    const u64 lo = k * Step, hi = std::min(lo + 2 * Step, mem::phys_pool_bytes());
    Imported im;
    const u64 t0 = host_ns();
    if (!import_host(mem::phys_ptr(lo), hi - lo, im)) return nullptr;
    im.addr = lo;
    LOG("nvk: imported physical window %#llx+%#llx in %llu ms", lo, hi - lo,
        static_cast<unsigned long long>((host_ns() - t0) / 1000000));
    return &g_windows.emplace(k, im).first->second;
}

// The GPU reads the vertices, indices and constants of the game's models straight from system memory otherwise, over PCIe, and
// that is most of a frame's GPU time. A pool whose content the renderer can follow gets a copy in device memory that the GPU uses
// in place of the import, made at the first use, once the game has filled it. HOST_NVN_POOL_MIRROR=0: none.
//  - GPU-cached and either not CPU-accessible or CPU-cached (flags 0x21, 0x24): the CPU's writes are announced by FlushMappedRange,
//    and those ranges are copied again. One the CPU publishes a quarter of, frame after frame, goes back to system memory.
//  - Small and CPU-uncached (the per-frame uniform blocks nearly every draw reads; writes are not announced): the 4 KB pages a
//    submission uses are copied once for it. The game wrote what a submission reads before submitting it (submissions are replayed
//    after that), and the copy runs on the GPU before the draws of the part that first uses it.
//  - Written by the GPU (storage a shader writes, buffer copies): back to system memory at first, the import being what the CPU
//    sees. A pool the CPU cannot access, or a CPU-cached one the CPU has neither published nor asked for (InvalidateMappedRange,
//    which the game calls before it reads what the GPU wrote) in QuietFrames frames of such writes, then becomes the GPU's own:
//    reads and writes go to the copy and system memory is left as it was. The first publish or ask takes it back for good
//    (the copy is written out first, and waited for).
//  - Written by the renderer on the CPU (counter reports, copies it could not do on the GPU): system memory for good.
constexpr u64 MirrorMaxPool = 64ull << 20, MirrorMaxAll = 1500ull << 20, RefreshMaxPool = 256ull << 10;
constexpr u32 MirrorMaxFlushFrames = 32;
constexpr u64 QuietFrames = 180;
u64 g_mirror_bytes = 0, g_mirrors = 0;
std::vector<std::pair<u64, u64>> g_flushed;   // (renderer thread, as everything below but g_owned)
u64 g_sub_id = 1;                             // the submission being recorded
u64 g_refresh_bytes = 0, g_refresh_n = 0;     // copies of the pools copied each submission (statistics)
std::vector<u64> g_promote;                   // pools to become the GPU's own at the next part
struct Demote {
    u64 addr;
    std::vector<std::pair<u64, u64>> keep;    // ranges of system memory newer than the copy
};
std::vector<Demote> g_demote;                 // pools to go back to system memory at the next part
u64 g_taken_back = 0;
// The GPU's own pools, for the game's threads (pool_gpu_owned)
std::mutex g_owned_mu;
std::vector<std::pair<u64, u64>> g_owned;
std::atomic<u32> g_owned_n{0};

bool mirror_on() {
    static const bool on = [] {
        // Off on unified memory: the GPU reads system memory as fast as its own, and the copies only cost bandwidth and
        // memory (HOST_NVN_POOL_MIRROR=1 has them anyway).
        const char* e = getenv("HOST_NVN_POOL_MIRROR");
        const bool want = e && *e ? std::strcmp(e, "0") != 0 : !ctx().feat.uma;
        return nvn::async_replay() && want;   // (everything here runs on the renderer thread)
    }();
    return on;
}

bool memory_to_spare() {
    u64 used = 0, budget = 0;
    device_budget(used, budget);
    return used * 100 < budget * 85;
}

// The device memory for a copy of p, its content not yet there. False (and p.where says why) when it gets none.
bool mirror_alloc(u64 addr, Pool& p, u64 max_size) {
    if (p.size > max_size) { p.where = "system memory (large)"; return false; }
    if (g_mirror_bytes + p.size > MirrorMaxAll) { p.where = "system memory (enough copies)"; return false; }
    // (an alias of the same storage would not hear of writes through the other)
    auto it = g_pools.find(addr);
    p.where = "system memory (aliased)";
    if (auto next = std::next(it); next != g_pools.end() && next->first < addr + p.size) return false;
    if (it != g_pools.begin() && std::prev(it)->first + std::prev(it)->second.size > addr) return false;
    p.where = "system memory (device memory short)";
    if (!memory_to_spare()) return false;
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = p.size;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT |
                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
                VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT |
                VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
                VK_BUFFER_USAGE_CONDITIONAL_RENDERING_BIT_EXT;
    VmaAllocationCreateInfo ai{};
    ai.requiredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    if (vmaCreateBuffer(ctx().vma, &bci, &ai, &p.mirror, &p.mirror_alloc, nullptr) != VK_SUCCESS) {
        p.mirror = VK_NULL_HANDLE;
        return false;
    }
    VkBufferDeviceAddressInfo dai{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
    dai.buffer = p.mirror;
    p.mirror_gpu = vkGetBufferDeviceAddress(ctx().dev, &dai);
    g_mirror_bytes += p.size;
    if (g_mirrors++ % 256 == 0)
        LOG("nvk: pools with a copy in device memory: %llu, %llu MB", static_cast<unsigned long long>(g_mirrors), static_cast<unsigned long long>(g_mirror_bytes >> 20));
    g_pools_gen.fetch_add(1, std::memory_order_release);
    bind_epoch_bump(!objects_only_when_freed());   // (nothing goes: the import stays)
    g_bumps[0]++;
    return true;
}

void mirror_fill(const Pool& p) {
    const VkBufferCopy r{p.offset, 0, p.size};
    vkCmdCopyBuffer(draw_pre_cb(), p.buffer, p.mirror, 1, &r);
}

// The 4 KB pages of [addr, addr + use) (0: to the pool's end) of a pool copied each submission, those not yet copied in this one.
void refresh_touch(u64 start, Pool& p, u64 addr, u64 use) {
    if (p.used_sub != g_sub_id) p.used_sub = g_sub_id, p.copied = 0;
    const u64 pages = (p.size + 0xfff) >> 12, lo = (addr - start) >> 12;
    const u64 hi = use ? std::min(pages, (addr - start + use + 0xfff) >> 12) : pages;
    if (lo >= hi) return;
    const u64 mask = (hi - lo == 64 ? ~0ull : (1ull << (hi - lo)) - 1) << lo;
    u64 need = mask & ~p.copied;
    if (!need) return;
    p.copied |= need;
    VkBufferCopy r[32];
    u32 n = 0;
    while (need) {
        const u64 i = static_cast<u64>(std::countr_zero(need)), run = static_cast<u64>(std::countr_one(need >> i));
        const u64 bytes = std::min(run << 12, p.size - (i << 12));
        r[n++] = {p.offset + (i << 12), i << 12, bytes};
        g_refresh_bytes += bytes;
        need = run + i >= 64 ? 0 : need & ~(((1ull << run) - 1) << i);
    }
    g_refresh_n++;
    vkCmdCopyBuffer(draw_pre_cb(), p.buffer, p.mirror, n, r);
}

void mirror_make(u64 addr, Pool& p) {
    if (p.mirror_tried) return;
    p.mirror_tried = true;
    if (!mirror_on() || p.pinned) return;
    const bool refresh = (p.flags & 0x2) && !(p.flags & (0x8 | 0x40 | 0x200)) && p.size <= RefreshMaxPool;
    if (!refresh && p.flags != 0x21 && p.flags != 0x24) { p.where = "system memory (the CPU writes it unannounced)"; return; }
    if (!mirror_alloc(addr, p, MirrorMaxPool)) return;
    p.where = "device memory";
    if (refresh) p.refresh = true, p.used_sub = 0, p.where = "device memory (copied each submission)";   // (refresh_touch fills it)
    else mirror_fill(p);
}

void owned_forget(u64 addr) {
    std::scoped_lock lk{g_owned_mu};
    std::erase_if(g_owned, [addr](const auto& r) { return r.first == addr; });
    g_owned_n.store(static_cast<u32>(g_owned.size()), std::memory_order_release);
}

void mirror_drop(u64 addr, Pool& p) {
    if (p.owned) owned_forget(addr);
    const VkBuffer b = p.mirror;
    const VmaAllocation a = p.mirror_alloc;
    defer([b, a] { vmaDestroyBuffer(ctx().vma, b, a); });
    g_mirror_bytes -= p.size;
    p.mirror = VK_NULL_HANDLE, p.mirror_alloc = VK_NULL_HANDLE, p.mirror_gpu = 0;
    p.owned = p.refresh = false;
    g_pools_gen.fetch_add(1, std::memory_order_release);
    bind_epoch_bump();
    g_bumps[1]++;
}

// The copy of an own pool written out to system memory, but for the ranges in keep, and the pool back on its import.
void take_back(VkCommandBuffer cb, u64 addr, Pool& p, std::vector<std::pair<u64, u64>> keep) {
    std::sort(keep.begin(), keep.end());
    u64 at = 0;
    const auto out = [&](u64 lo, u64 hi) {
        if (lo >= hi) return;
        const VkBufferCopy r{lo, p.offset + lo, hi - lo};
        vkCmdCopyBuffer(cb, p.mirror, p.buffer, 1, &r);
    };
    for (const auto& [a, n] : keep) {
        const u64 lo = a > addr ? std::min(a - addr, p.size) : 0, hi = a + n > addr ? std::min(a + n - addr, p.size) : 0;
        out(at, lo);
        at = std::max(at, hi);
    }
    out(at, p.size);
    g_taken_back++;
}

void took_back(u64 addr, Pool& p) {
    mirror_drop(addr, p);
    p.shared = true;
    if (!p.pinned) p.where = "system memory (the GPU writes it, the CPU reads or writes it)";
    static u64 at = 0;
    if (const u64 now = host_ns(); now - at > 10'000'000'000ull) {
        at = now;
        LOG("nvk: pools the GPU had to itself taken back to system memory (the CPU reads or writes them after all): %llu so far",
            static_cast<unsigned long long>(g_taken_back));
    }
}

// Between submissions (nothing recorded is waiting): at once, and waited for.
void take_back_now(u64 addr, Pool& p, u64 keep_addr, u64 keep_n) {
    VkCommandBuffer cb = begin_oneoff();
    take_back(cb, addr, p, {{keep_addr, keep_n}});
    end_oneoff(cb);
    took_back(addr, p);
}

template <class F> void each_pool(u64 addr, u64 n, F f) {
    auto it = g_pools.upper_bound(addr);
    while (it != g_pools.begin() && std::prev(it)->first + std::prev(it)->second.size > addr) --it;   // (pools nest only where the game aliases storage)
    for (; it != g_pools.end() && it->first < addr + n; ++it)
        if (it->first + it->second.size > addr) f(it->first, it->second);
}

}  // namespace

void pool_gpu_write(u64 addr, u64 n, bool on_cpu) {
    if (!mirror_on()) return;
    const u64 frame = nvn::g_frame.load(std::memory_order_relaxed);
    std::shared_lock lk{g_lock};
    each_pool(addr, n, [&](u64 start, Pool& p) {
        p.mirror_tried = true;
        if (on_cpu) {
            p.pinned = true;
            p.where = "system memory (the renderer writes it on the CPU)";
            if (p.owned) {
                // (what this part recorded so far uses the copy: out with it when the part is over)
                auto it = std::find_if(g_demote.begin(), g_demote.end(), [&](const Demote& d) { return d.addr == start; });
                if (it == g_demote.end()) it = g_demote.insert(g_demote.end(), Demote{start, {}});
                it->keep.emplace_back(addr, n);
            } else if (p.mirror) {
                mirror_drop(start, p);
            }
            return;
        }
        if (p.owned) return;
        if (p.mirror) mirror_drop(start, p);
        if (!p.gpu_written) p.gpu_written = true, p.quiet_from = frame;
        if (p.pinned || p.shared) return;
        p.where = "system memory (the GPU writes it)";
        if (!p.promote_queued && (p.flags == 0x21 || (p.flags == 0x24 && frame - p.quiet_from >= QuietFrames))) {
            p.promote_queued = true;
            g_promote.push_back(start);
        }
    });
}

bool pool_write_through(VkCommandBuffer cb, u64 addr, u64 n) {
    if (!g_owned_n.load(std::memory_order_relaxed)) return false;
    std::shared_lock lk{g_lock};
    bool any = false;
    each_pool(addr, n, [&](u64 start, Pool& p) {
        if (!p.owned || addr < start || addr + n > start + p.size) return;
        const VkBufferCopy r{addr - start, p.offset + (addr - start), n};
        vkCmdCopyBuffer(cb, p.mirror, p.buffer, 1, &r);
        any = true;
    });
    return any;
}

bool pool_gpu_owned(u64 addr, u64 n) {
    if (!g_owned_n.load(std::memory_order_acquire)) return false;
    std::scoped_lock lk{g_owned_mu};
    for (const auto& [a, size] : g_owned)
        if (a < addr + n && addr < a + size) return true;
    return false;
}

void pool_flushed(u64 addr, u64 n) {
    if (pool_stats_on()) pool_note(addr, n, 7);
    if (!mirror_on()) return;
    const u64 frame = nvn::g_frame.load(std::memory_order_relaxed);
    {
        std::shared_lock lk{g_lock};
        each_pool(addr, n, [&](u64 start, Pool& p) {
            p.quiet_from = frame;
            if (p.owned) take_back_now(start, p, addr, n);
        });
    }
    g_flushed.emplace_back(addr, n);
}

void pool_invalidated(u64 addr, u64 n) {
    if (pool_stats_on()) pool_note(addr, n, 8);
    if (!mirror_on()) return;
    const u64 frame = nvn::g_frame.load(std::memory_order_relaxed);
    std::shared_lock lk{g_lock};
    each_pool(addr, n, [&](u64 start, Pool& p) {
        p.quiet_from = frame;
        if (p.owned) take_back_now(start, p, 0, 0);
    });
}

void pool_part_begin(bool submission) {
    if (!mirror_on()) return;
    std::shared_lock lk{g_lock};
    if (submission) g_sub_id++;
    for (const Demote& d : g_demote) {
        auto it = g_pools.find(d.addr);
        if (it == g_pools.end() || !it->second.owned) continue;
        take_back(draw_pre_cb(), d.addr, it->second, d.keep);
        took_back(d.addr, it->second);
    }
    g_demote.clear();
    const u64 frame = nvn::g_frame.load(std::memory_order_relaxed);
    for (const u64 addr : g_promote) {
        auto it = g_pools.find(addr);
        if (it == g_pools.end()) continue;
        Pool& p = it->second;
        p.promote_queued = false;
        if (p.pinned || p.shared || p.mirror || !p.gpu_written || (p.flags != 0x21 && frame - p.quiet_from < QuietFrames)) continue;
        if (!mirror_alloc(addr, p, MirrorMaxPool)) {
            p.quiet_from = frame;   // (asked again in a while)
            continue;
        }
        mirror_fill(p);
        p.owned = true;
        p.where = "device memory (the GPU's own)";
        std::scoped_lock ol{g_owned_mu};
        g_owned.emplace_back(addr, p.size);
        g_owned_n.store(static_cast<u32>(g_owned.size()), std::memory_order_release);
    }
    g_promote.clear();
    if (!submission || g_flushed.empty()) return;
    for (const auto& [addr, n] : g_flushed) {
        auto it = g_pools.upper_bound(addr);
        if (it == g_pools.begin()) continue;
        --it;
        Pool& p = it->second;
        if (!p.mirror || p.owned || p.refresh || addr >= it->first + p.size) continue;
        const u64 at = addr - it->first;
        const VkBufferCopy r{p.offset + at, at, std::min(n, p.size - at)};
        p.flush_bytes += r.size;
        if (p.flush_frame != frame) {
            p.flush_frame = frame;
            if (++p.flush_frames > MirrorMaxFlushFrames) {
                // (a little of a pool published every frame costs less copied than read in place)
                if (p.flush_bytes > p.size * MirrorMaxFlushFrames / 4) {
                    p.mirror_tried = true;
                    p.where = "system memory (the CPU publishes much of it every frame)";
                    mirror_drop(it->first, p);
                    continue;
                }
                p.flush_frames = 0, p.flush_bytes = 0;
            }
        }
        if (r.size) vkCmdCopyBuffer(draw_pre_cb(), p.buffer, p.mirror, 1, &r);
    }
    g_flushed.clear();
}

void pool_add(u64 addr, u64 size, u32 flags) {
    bind_epoch_bump(!objects_only_when_freed());
    g_bumps[2]++;
    Pool p;
    p.size = size;
    p.flags = flags;
    const u64 lo = addr & ~0xfffull, hi = (addr + size + 0xfff) & ~0xfffull;
    const auto runs = mem::phys_runs(lo, hi - lo);
    const bool one_run = runs.size() == 1 && runs[0].size == hi - lo;
    const Imported* w = one_run && hi - lo <= Step ? window(runs[0].phys, hi - lo) : nullptr;
    if (w) {
        p.buffer = w->buffer;
        p.offset = runs[0].phys - w->addr + (addr - lo);
        p.gpu = w->gpu + p.offset;
    } else {
        const u64 t0 = host_ns();
        const char* why = nullptr;
        const bool ok = one_run ? import_host(mem::phys_ptr(runs[0].phys), hi - lo, p.im, &why)
                                : import_guest(lo, hi - lo, p.im, &why);
        if (!ok) {
            LOG("[RED] nvk: memory pool %#llx+%#llx could not be imported for the GPU (%s)", addr, size, why ? why : "?");
            return;
        }
        LOG("nvk: memory pool %#llx+%#llx imported on its own (%s) in %llu ms", addr, size,
            one_run ? "large" : "several physical runs", static_cast<unsigned long long>((host_ns() - t0) / 1000000));
        p.own = true;
        p.buffer = p.im.buffer;
        p.offset = addr - lo;
        p.gpu = p.im.gpu + p.offset;
    }
    std::unique_lock lk{g_lock};
    if (auto it = g_pools.find(addr); it != g_pools.end()) {
        if (it->second.mirror) mirror_drop(addr, it->second);
        if (it->second.own) release_import(it->second.im), bind_epoch_bump();
    }
    g_pools[addr] = p;
    g_pools_gen.fetch_add(1, std::memory_order_release);
}

void pool_del(u64 addr) {
    bind_epoch_bump(!objects_only_when_freed());
    g_bumps[3]++;
    std::unique_lock lk{g_lock};
    auto it = g_pools.find(addr);
    if (it == g_pools.end()) return;
    image_pool_gone(addr, addr + it->second.size);
    if (it->second.mirror) mirror_drop(addr, it->second);
    if (it->second.own) {
        Imported im = it->second.im;
        defer([im]() mutable { release_import(im); });
        bind_epoch_bump();
    }
    g_pools.erase(it);
    g_pools_gen.fetch_add(1, std::memory_order_release);
}

bool pool_stats_on() {
    static const bool on = getenv("HOST_NVN_POOL_STATS") != nullptr;
    return on;
}

void pool_note(u64 addr, u64 bytes, int kind) {
    constexpr int K = 9;
    static const char* const names[K] = {"vertices", "indices", "uniform", "storage read", "indirect", "copied to", "storage written", "published", "asked for"};
    struct Stat { u64 n[K] = {}; };
    static std::map<u64, Stat> by;
    static u64 at = 0, f0 = 0;
    {
        std::shared_lock lk{g_lock};
        auto it = g_pools.upper_bound(addr);
        by[it == g_pools.begin() ? 0 : std::prev(it)->first].n[kind] += bytes;
    }
    const u64 now = host_ns();
    if (now - at < 10'000'000'000ull) return;
    const u64 fr = nvn::g_frame.load();
    if (at && fr > f0) {
        const double per = 1.0 / 1048576.0 / static_cast<double>(fr - f0);
        const auto line = [&](const Stat& st) {
            std::string o;
            for (int i = 0; i < K; i++) {
                if (!st.n[i]) continue;
                char t[64];
                std::snprintf(t, sizeof t, "%s%s %.2f", o.empty() ? "" : ", ", names[i], st.n[i] * per);
                o += t;
            }
            return o;
        };
        std::shared_lock lk{g_lock};
        std::map<std::string, Stat> groups;
        std::vector<std::pair<u64, u64>> v;
        Stat all;
        for (auto& [k, st] : by) {
            u64 sum = 0;
            for (int i = 0; i < K; i++) sum += st.n[i], all.n[i] += st.n[i];
            v.push_back({sum, k});
            auto it = g_pools.find(k);
            char g[96];
            std::snprintf(g, sizeof g, "flags %#x, %s", it == g_pools.end() ? 0u : it->second.flags, it == g_pools.end() ? "gone" : it->second.where);
            Stat& gs = groups[g];
            for (int i = 0; i < K; i++) gs.n[i] += st.n[i];
        }
        LOG("nvk: pool use, MB a frame: %s; copies in device memory %llu MB; copied each submission: %.1f pools, %.2f MB a frame", line(all).c_str(),
            static_cast<unsigned long long>(g_mirror_bytes >> 20), static_cast<double>(g_refresh_n) / static_cast<double>(fr - f0), g_refresh_bytes * per);
        g_refresh_n = g_refresh_bytes = 0;
        for (auto& [g, st] : groups) LOG("nvk:   %s: %s", g.c_str(), line(st).c_str());
        std::sort(v.rbegin(), v.rend());
        for (size_t i = 0; i < v.size() && i < 16; i++) {
            auto it = g_pools.find(v[i].second);
            LOG("nvk:   pool %#llx +%#llx flags %#x, %s: %s", static_cast<unsigned long long>(v[i].second),
                it == g_pools.end() ? 0ull : static_cast<unsigned long long>(it->second.size), it == g_pools.end() ? 0u : it->second.flags,
                it == g_pools.end() ? "gone" : it->second.where, line(by[v[i].second]).c_str());
        }
    }
    by.clear(), at = now, f0 = fr;
}

u64 pool_gen() { return g_pools_gen.load(std::memory_order_acquire); }

void pool_bumps(u64 out[4]) {
    for (int i = 0; i < 4; i++) out[i] = g_bumps[i].load(std::memory_order_relaxed);
}

bool pool_find(u64 addr, u64 len, BufferRef& out, u64 use) {
    // Every buffer of every draw comes through here: what was found last for this address's page, per thread. An entry
    // answers for the addresses from its pool's start up to the next pool's, where the search below finds that pool first.
    struct Last {
        u64 gen = 0, start = 0, next = 0, end = 0, offset = 0;
        Pool* refresh = nullptr;   // the pool, if it is copied each submission
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceAddress gpu = 0;
    };
    constexpr u64 NLast = 1 << 12;
    thread_local std::unique_ptr<Last[]> last{new Last[NLast]};   // (few threads get here)
    Last& l = last[(addr >> 12) & (NLast - 1)];
    const u64 gen = g_pools_gen.load(std::memory_order_acquire);
    if (l.gen == gen && addr >= l.start && addr < l.next && addr + len <= l.end) {
        if (l.refresh) refresh_touch(l.start, *l.refresh, addr, use);
        out.buffer = l.buffer;
        out.offset = l.offset + (addr - l.start);
        out.size = l.end - addr;
        out.gpu = l.gpu + (addr - l.start);
        return true;
    }
    std::shared_lock lk{g_lock};
    auto it = g_pools.upper_bound(addr);
    const u64 next = it == g_pools.end() ? UINT64_MAX : it->first;
    // Overlapping pools: the nearest start at or below addr that holds the whole range, looking
    // back a few entries (pools nest only when the game aliases storage).
    for (int tries = 0; it != g_pools.begin() && tries < 8; tries++) {
        --it;
        Pool& p = it->second;
        if (addr >= it->first && addr + len <= it->first + p.size) {
            mirror_make(it->first, p);
            if (p.refresh) refresh_touch(it->first, p, addr, use);
            const VkBuffer buffer = p.mirror ? p.mirror : p.buffer;
            const u64 offset = p.mirror ? 0 : p.offset;
            const VkDeviceAddress gpu = p.mirror ? p.mirror_gpu : p.gpu;
            out.buffer = buffer;
            out.offset = offset + (addr - it->first);
            out.size = p.size - (addr - it->first);
            out.gpu = gpu + (addr - it->first);
            if (tries == 0) l = {g_pools_gen.load(std::memory_order_acquire), it->first, next, it->first + p.size, offset, p.refresh ? &p : nullptr, buffer, gpu};
            return true;
        }
    }
    return false;
}

}  // namespace nvk
