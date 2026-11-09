#include "memory.h"

#include <cstdlib>
#include <cstring>

#include "common/host_memory.h"
#include "core/device_memory.h"

extern "C" __declspec(dllimport) unsigned long __stdcall DiscardVirtualMemory(void* address, size_t size);

uintptr_t g_membase = 0;

namespace mem {
u64 g_phys_size = 0;
}

// The GPU (video_core) reads guest memory through Core::DeviceMemory's backing pool: it is ours.
namespace Core {
DeviceMemory::DeviceMemory() : buffer{mem::g_phys_size, mem::AddressSpaceSize} {}
DeviceMemory::~DeviceMemory() = default;
}  // namespace Core

namespace mem {
namespace {

struct Block {
    u64 size;
    u32 state, perm, attr;
    bool same(const Block& o) const { return state == o.state && perm == o.perm && attr == o.attr; }
};

// A mapped range of the fastmem view and the physical memory behind it.
struct Mapping {
    u64 size;
    u64 phys;
    bool owned;  // allocated by map(): freed on unmap
};

std::recursive_mutex g_lock;
std::map<u64, Block> g_blocks;      // covers [0, AddressSpaceSize) without gaps
std::map<u64, Mapping> g_mappings;  // guest address -> physical run (only mapped ranges)
std::map<u64, u64> g_phys_free;     // free physical runs: offset -> size
u64 g_heap_size = 0;
Core::DeviceMemory* g_device = nullptr;
Common::HostMemory* g_host = nullptr;

// Make `addr` the start of a block.
void split(u64 addr) {
    if (addr >= AddressSpaceSize) return;
    auto it = g_blocks.upper_bound(addr);
    --it;
    if (it->first == addr) return;
    Block tail = it->second;
    const u64 head = addr - it->first;
    tail.size -= head;
    it->second.size = head;
    g_blocks.emplace(addr, tail);
}

void merge_around(u64 lo, u64 hi) {
    auto it = g_blocks.upper_bound(lo);
    if (it != g_blocks.begin()) --it;
    if (it != g_blocks.begin()) --it;
    while (it != g_blocks.end() && it->first <= hi) {
        auto next = std::next(it);
        if (next != g_blocks.end() && it->second.same(next->second)) {
            it->second.size += next->second.size;
            g_blocks.erase(next);
            continue;
        }
        it = next;
    }
}

template <class F> void update(u64 addr, u64 size, F f) {
    split(addr);
    split(addr + size);
    for (auto it = g_blocks.find(addr); it != g_blocks.end() && it->first < addr + size; ++it) f(it->second);
    merge_around(addr, addr + size);
}

// Make `addr` the start of a mapping if a mapping covers it.
void split_mapping(u64 addr) {
    auto it = g_mappings.upper_bound(addr);
    if (it == g_mappings.begin()) return;
    --it;
    if (it->first == addr || addr >= it->first + it->second.size) return;
    const u64 head = addr - it->first;
    Mapping tail{it->second.size - head, it->second.phys + head, it->second.owned};
    it->second.size = head;
    g_mappings.emplace(addr, tail);
}

// Freed physical pages leave RAM: until the pool hands them out again only the backing view maps
// them, and first fit often places the next allocation elsewhere, so they stayed resident unused.
// Discarded section pages read zero again (scratchpad disc test), so they need no clearing when
// reused either: clearing a reused 4 MiB sparse block made all of it resident for a 256 KiB write.
// Returns false when they were kept (HOST_KEEP_FREED=1, or the discard failed): then they are
// marked freed and cleared on reuse. HOST_KEEP_FREED=1 restores that.
u64 g_discarded = 0;
bool discard_run(u64 phys, u64 size) {
    // Checked once on the pool's last page (the allocator hands it out last): written, discarded,
    // then it must read zero.
    static const bool keep = [] {
        if (const char* v = std::getenv("HOST_KEEP_FREED"); v && v[0] == '1') return true;
        u8* const page = g_host->BackingBasePointer() + g_phys_size - PageSize;
        std::memset(page, 0xAB, PageSize);
        bool zero = DiscardVirtualMemory(page, PageSize) == 0;
        for (u64 i = 0; zero && i < PageSize; ++i) zero = page[i] == 0;
        if (!zero) LOG("mem: discarded pages do not read zero; freed guest memory stays resident");
        return !zero;
    }();
    if (keep || DiscardVirtualMemory(g_host->BackingBasePointer() + phys, size) != 0) return false;
    const u64 before = g_discarded;
    g_discarded += size;
    if ((before >> 30) != (g_discarded >> 30)) LOG("mem: %llu MiB of freed guest memory returned to the system so far", g_discarded >> 20);
    return true;
}

void free_run(u64 phys, u64 size) {
    if (!discard_run(phys, size)) g_host->MarkBackingFreed(phys, size);
    auto next = g_phys_free.lower_bound(phys);
    if (next != g_phys_free.begin()) {
        auto prev = std::prev(next);
        if (prev->first + prev->second == phys) {
            prev->second += size;
            if (next != g_phys_free.end() && phys + size == next->first) {
                prev->second += next->second;
                g_phys_free.erase(next);
            }
            return;
        }
    }
    if (next != g_phys_free.end() && phys + size == next->first) {
        const u64 merged = size + next->second;
        g_phys_free.erase(next);
        g_phys_free.emplace(phys, merged);
        return;
    }
    g_phys_free.emplace(phys, size);
}

// Unmaps guest pages from the view and frees the physical memory map() gave them.
void unmap_view(u64 addr, u64 size) {
    split_mapping(addr);
    split_mapping(addr + size);
    for (auto it = g_mappings.lower_bound(addr); it != g_mappings.end() && it->first < addr + size;) {
        g_host->Unmap(it->first, it->second.size, false);
        if (it->second.owned) free_run(it->second.phys, it->second.size);
        it = g_mappings.erase(it);
    }
}

void map_view(u64 addr, const std::vector<PhysRun>& runs, bool owned) {
    unmap_view(addr, [&] { u64 s = 0; for (auto& r : runs) s += r.size; return s; }());
    for (const PhysRun& r : runs) {
        g_host->Map(addr, r.phys, r.size, Common::MemoryPermission::ReadWrite, false);
        g_mappings.emplace(addr, Mapping{r.size, r.phys, owned});
        addr += r.size;
    }
}

}  // namespace

Common::HostMemory& host_memory() { return *g_host; }
Core::DeviceMemory& device_memory() { return *g_device; }

void init() {
    // The pool is committed up front (a pagefile-backed section) but only touched pages use RAM.
    // 24 GiB: the game's memory (8.2 GB at boot) plus GPU-only regions (~2.8 GB) plus the sparse
    // regions' blocks, which are backed as the GPU first writes them and kept (~250 MB more per
    // course): ~15 GB allocated after a race, ~2 GB of it resident. Machines whose RAM + page file
    // can't take the commit (a 16 GB handheld) get the largest of 20, 16, 14, 12 GiB that fits;
    // past the pool's last GiB the GPU's sparse and GPU-only memory goes without backing.
    std::vector<u64> sizes{24, 20, 16, 14, 12};
    if (const char* v = std::getenv("HOST_PHYS_GIB")) sizes = {std::strtoull(v, nullptr, 0)};
    for (const u64 gib : sizes) {
        g_phys_size = gib << 30;
        g_device = new Core::DeviceMemory();
        if (g_device->buffer.VirtualBasePointer()) break;
        delete g_device;
        g_device = nullptr;
        LOG("guest memory pool: %llu GiB doesn't fit in RAM + page file", gib);
    }
    if (!g_device)
        fatal("cannot reserve guest memory: even %llu GiB doesn't fit in RAM + page file. Free memory or "
              "enlarge the page file (System > About > Advanced system settings > Performance > Virtual memory).",
              sizes.back());
    LOG("guest memory pool: %llu GiB", g_phys_size >> 30);
    g_host = &g_device->buffer;
    g_membase = reinterpret_cast<uintptr_t>(g_host->VirtualBasePointer());
    g_phys_free.emplace(0, g_phys_size);
    g_blocks.emplace(0, Block{AddressSpaceSize, Free, None, 0});
}

std::vector<PhysRun> phys_alloc(u64 size) {
    std::scoped_lock lk{g_lock};
    std::vector<PhysRun> runs;
    // First fit in one piece if possible, otherwise from the front in several.
    auto it = g_phys_free.begin();
    for (; it != g_phys_free.end() && it->second < size; ++it) {}
    if (it == g_phys_free.end()) it = g_phys_free.begin();
    u64 left = size;
    while (left && it != g_phys_free.end()) {
        const u64 take = std::min(left, it->second);
        runs.push_back({it->first, take});
        const u64 rest_off = it->first + take, rest = it->second - take;
        it = g_phys_free.erase(it);
        if (rest) it = g_phys_free.emplace(rest_off, rest).first;
        left -= take;
    }
    if (left) fatal("out of guest physical memory allocating %#llx bytes (raise HOST_PHYS_GIB)", size);
    for (const PhysRun& r : runs) g_host->ClearBackingRegion(r.phys, r.size, 0);
    return runs;
}

void phys_free(const std::vector<PhysRun>& runs) {
    std::scoped_lock lk{g_lock};
    for (const PhysRun& r : runs) free_run(r.phys, r.size);
}

u64 phys_free_bytes() {
    std::scoped_lock lk{g_lock};
    u64 total = 0;
    for (const auto& [off, size] : g_phys_free) total += size;
    return total;
}

u8* phys_ptr(u64 phys) { return g_host->BackingBasePointer() + phys; }
u64 phys_pool_bytes() { return g_phys_size; }

std::vector<RemapListener> g_remap_listeners;

void add_remap_listener(RemapListener listener) {
    std::scoped_lock lk{g_lock};
    g_remap_listeners.push_back(listener);
}

static void notify_remap(u64 addr, u64 size) {
    std::vector<RemapListener> listeners;
    {
        std::scoped_lock lk{g_lock};
        listeners = g_remap_listeners;
    }
    for (RemapListener l : listeners) l(addr, size);
}

void map(u64 addr, u64 size, u32 state, u32 perm) {
    {
        std::scoped_lock lk{g_lock};
        map_view(addr, phys_alloc(size), true);
        update(addr, size, [&](Block& b) { b.state = state; b.perm = perm; b.attr = 0; });
    }
    notify_remap(addr, size);
}

void map_phys(u64 addr, const std::vector<PhysRun>& runs, u32 state, u32 perm) {
    u64 size = 0;
    for (auto& r : runs) size += r.size;
    {
        std::scoped_lock lk{g_lock};
        map_view(addr, runs, false);
        update(addr, size, [&](Block& b) { b.state = state; b.perm = perm; b.attr = 0; });
    }
    notify_remap(addr, size);
}

void unmap(u64 addr, u64 size) {
    {
        std::scoped_lock lk{g_lock};
        unmap_view(addr, size);
        update(addr, size, [&](Block& b) { b.state = Free; b.perm = None; b.attr = 0; });
    }
    notify_remap(addr, size);
}

std::vector<PhysRun> phys_runs(u64 addr, u64 size) {
    std::scoped_lock lk{g_lock};
    std::vector<PhysRun> out;
    auto it = g_mappings.upper_bound(addr);
    if (it != g_mappings.begin()) --it;
    for (; it != g_mappings.end() && it->first < addr + size; ++it) {
        const u64 lo = std::max(addr, it->first), hi = std::min(addr + size, it->first + it->second.size);
        if (lo >= hi) continue;
        const u64 phys = it->second.phys + (lo - it->first);
        if (!out.empty() && out.back().phys + out.back().size == phys) out.back().size += hi - lo;
        else out.push_back({phys, hi - lo});
    }
    return out;
}

// Guest pages mapping the same physical page as `addr` (itself included); `others` gets up to
// four of the other guest addresses. A debug aid: the GPU's write tracking protects one guest
// address per physical page, so stores through an alias are not seen.
u32 alias_count(u64 addr, std::vector<u64>* others) {
    std::scoped_lock lk{g_lock};
    auto it = g_mappings.upper_bound(addr);
    if (it == g_mappings.begin()) return 0;
    --it;
    if (addr >= it->first + it->second.size) return 0;
    const u64 phys = (it->second.phys + (addr - it->first)) & ~(PageSize - 1);
    u32 count = 0;
    for (const auto& [va, m] : g_mappings) {
        if (phys < m.phys || phys >= m.phys + m.size) continue;
        ++count;
        const u64 alias = va + (phys - m.phys);
        if (others && alias != (addr & ~(PageSize - 1)) && others->size() < 4) others->push_back(alias);
    }
    return count;
}

u8* backing_ptr(u64 addr) {
    std::scoped_lock lk{g_lock};
    auto it = g_mappings.upper_bound(addr);
    if (it == g_mappings.begin()) return nullptr;
    --it;
    if (addr >= it->first + it->second.size) return nullptr;
    return phys_ptr(it->second.phys + (addr - it->first));
}

bool wait_mapped(u64 addr) {
    std::scoped_lock lk{g_lock};
    return backing_ptr(addr) != nullptr;
}

void protect_view(u64 addr, u64 size, bool read, bool write) {
    std::scoped_lock lk{g_lock};
    Common::MemoryPermission perm{};
    if (read) perm |= Common::MemoryPermission::Read;
    if (write) perm |= Common::MemoryPermission::Write;
    auto it = g_mappings.upper_bound(addr);
    if (it != g_mappings.begin()) --it;
    for (; it != g_mappings.end() && it->first < addr + size; ++it) {
        const u64 lo = std::max(addr, it->first), hi = std::min(addr + size, it->first + it->second.size);
        if (lo < hi) g_host->Protect(lo, hi - lo, perm);
    }
}

// Permissions are bookkeeping only: host code reads guest memory through the view whatever the
// guest's permission, and the view's host protection belongs to the GPU's write tracking.
void set_perm(u64 addr, u64 size, u32 perm) {
    std::scoped_lock lk{g_lock};
    update(addr, size, [&](Block& b) { b.perm = perm; });
}

void set_attr(u64 addr, u64 size, u32 mask, u32 attr) {
    std::scoped_lock lk{g_lock};
    update(addr, size, [&](Block& b) { b.attr = (b.attr & ~mask) | (attr & mask); });
}

void set_state(u64 addr, u64 size, u32 state, u32 perm) {
    std::scoped_lock lk{g_lock};
    update(addr, size, [&](Block& b) { b.state = state; b.perm = perm; });
}

Info query(u64 addr) {
    std::scoped_lock lk{g_lock};
    if (addr >= AddressSpaceSize) {
        // Beyond the address space the kernel reports one inaccessible block.
        return {AddressSpaceSize, 0ull - AddressSpaceSize, Inaccessible, 0, None};
    }
    auto it = g_blocks.upper_bound(addr);
    --it;
    return {it->first, it->second.size, it->second.state, it->second.attr, it->second.perm};
}

bool all_state(u64 addr, u64 size, u32 state) {
    std::scoped_lock lk{g_lock};
    if (addr + size > AddressSpaceSize || addr + size < addr) return false;
    auto it = g_blocks.upper_bound(addr);
    --it;
    for (; it != g_blocks.end() && it->first < addr + size; ++it)
        if (it->second.state != state) return false;
    return true;
}

bool is_free(u64 addr, u64 size) { return all_state(addr, size, Free); }

u64 find_free(u64 region, u64 region_size, u64 size, u64 align) {
    std::scoped_lock lk{g_lock};
    const u64 end = region + region_size;
    auto it = g_blocks.upper_bound(region);
    --it;
    for (; it != g_blocks.end() && it->first < end; ++it) {
        if (it->second.state != Free) continue;
        u64 start = (std::max(it->first, region) + align - 1) & ~(align - 1);
        const u64 block_end = std::min(it->first + it->second.size, end);
        if (start + size <= block_end) return start;
    }
    return 0;
}

u32 set_heap_size(u64 size, u64* out_addr) {
    if (size & 0x1FFFFF) return KResult::InvalidSize;
    if (size > HeapSize) return KResult::OutOfMemory;
    // Not under the memory lock: map() and unmap() notify the remap listeners after releasing it.
    static std::mutex heap_lock;
    std::scoped_lock lk{heap_lock};
    if (size > g_heap_size) {
        map(HeapBase + g_heap_size, size - g_heap_size, Normal, RW);
    } else if (size < g_heap_size) {
        unmap(HeapBase + size, g_heap_size - size);
    }
    g_heap_size = size;
    *out_addr = HeapBase;
    return ResultOk;
}

u64 heap_size() { return g_heap_size; }

}  // namespace mem
