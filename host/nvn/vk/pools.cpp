#include "pools.h"
#include "textures.h"

#include <map>
#include <shared_mutex>
#include <unordered_map>

#include "../../memory.h"

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
};
std::shared_mutex g_lock;
std::map<u64, Pool> g_pools;  // by guest start; pools may overlap (aliases of the same storage)
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

}  // namespace

void pool_add(u64 addr, u64 size) {
    bind_epoch_bump();
    Pool p;
    p.size = size;
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
    if (auto it = g_pools.find(addr); it != g_pools.end() && it->second.own) release_import(it->second.im);
    g_pools[addr] = p;
}

void pool_del(u64 addr) {
    bind_epoch_bump();
    std::unique_lock lk{g_lock};
    auto it = g_pools.find(addr);
    if (it == g_pools.end()) return;
    image_pool_gone(addr, addr + it->second.size);
    if (it->second.own) {
        Imported im = it->second.im;
        defer([im]() mutable { release_import(im); });
    }
    g_pools.erase(it);
}

bool pool_find(u64 addr, u64 len, BufferRef& out) {
    std::shared_lock lk{g_lock};
    auto it = g_pools.upper_bound(addr);
    // Overlapping pools: the nearest start at or below addr that holds the whole range, looking
    // back a few entries (pools nest only when the game aliases storage).
    for (int tries = 0; it != g_pools.begin() && tries < 8; tries++) {
        --it;
        const Pool& p = it->second;
        if (addr >= it->first && addr + len <= it->first + p.size) {
            out.buffer = p.buffer;
            out.offset = p.offset + (addr - it->first);
            out.size = p.size - (addr - it->first);
            out.gpu = p.gpu + (addr - it->first);
            return true;
        }
    }
    return false;
}

}  // namespace nvk
