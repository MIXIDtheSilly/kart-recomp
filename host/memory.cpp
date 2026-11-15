#include "memory.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <fstream>
#include <string>
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <deque>
#include <cstdlib>
#include <cstring>
#include <memory>

#ifdef _WIN32
#ifndef MEM_RESERVE_PLACEHOLDER
#define MEM_RESERVE_PLACEHOLDER 0x00040000
#endif
#ifndef MEM_REPLACE_PLACEHOLDER
#define MEM_REPLACE_PLACEHOLDER 0x00004000
#endif
#ifndef MEM_COALESCE_PLACEHOLDERS
#define MEM_COALESCE_PLACEHOLDERS 0x00000001
#endif
#ifndef MEM_PRESERVE_PLACEHOLDER
#define MEM_PRESERVE_PLACEHOLDER 0x00000002
#endif
#endif

uintptr_t g_membase = 0;

namespace mem {
namespace {

// ------------------------------------------------------------------ the pool
// All guest physical memory is one pagefile-backed section, committed up front (only touched pages
// use RAM). It is mapped whole at g_backing (the backing view: host code and the renderer read
// guest memory there, never faulting on the guest view's protections) and piece by piece into the
// guest view at g_membase, where recompiled code addresses guest memory. The guest view is one
// placeholder reservation (VirtualAlloc2): mapping a range splits a placeholder for it off the free
// space around it and replaces it with a view of the section; unmapping puts the placeholder back
// and merges it with the free space on both sides. Every entry of g_mappings is one such view, so
// the free space between two views is always one placeholder.
// Linux: the pool is a memfd, the backing view one shared mapping of it, and the guest view a PROT_NONE reservation
// in which each mapping is a MAP_FIXED shared mapping of the memfd (replacing that part of the reservation, which an
// unmap puts back). Linux unmaps and remaps parts of a mapping in place, so there are no placeholders to split or merge.

constexpr u64 ViewAlign = 2ull << 20;  // guest address 0 starts a 2 MiB block of the host's
u64 g_phys_size = 0;
u8* g_backing = nullptr;
u8* g_reserve = nullptr;  // the guest view's reservation: the address space plus ViewAlign
u8* g_reserve_end = nullptr;
// One bit per pool page that has held guest data (freed without being discarded): cleared on reuse.
std::unique_ptr<std::atomic<u64>[]> g_freed;

u8* view_ptr(u64 addr) { return reinterpret_cast<u8*>(g_membase) + addr; }

#ifdef _WIN32
using VirtualAlloc2Fn = void*(WINAPI*)(HANDLE, void*, SIZE_T, ULONG, ULONG, void*, ULONG);
using MapViewOfFile3Fn = void*(WINAPI*)(HANDLE, HANDLE, void*, ULONG64, SIZE_T, ULONG, ULONG, void*, ULONG);
using UnmapViewOfFile2Fn = BOOL(WINAPI*)(HANDLE, void*, ULONG);
VirtualAlloc2Fn g_VirtualAlloc2 = nullptr;
MapViewOfFile3Fn g_MapViewOfFile3 = nullptr;
UnmapViewOfFile2Fn g_UnmapViewOfFile2 = nullptr;

HANDLE g_section = nullptr;

void load_memory_api() {
    const HMODULE kernelbase = GetModuleHandleW(L"kernelbase.dll");
    if (kernelbase) {
        g_VirtualAlloc2 = reinterpret_cast<VirtualAlloc2Fn>(GetProcAddress(kernelbase, "VirtualAlloc2"));
        g_MapViewOfFile3 = reinterpret_cast<MapViewOfFile3Fn>(GetProcAddress(kernelbase, "MapViewOfFile3"));
        g_UnmapViewOfFile2 = reinterpret_cast<UnmapViewOfFile2Fn>(GetProcAddress(kernelbase, "UnmapViewOfFile2"));
    }
    if (!g_VirtualAlloc2 || !g_MapViewOfFile3 || !g_UnmapViewOfFile2)
        fatal("guest memory needs Windows 10 version 1803 or later (VirtualAlloc2, MapViewOfFile3)");
}

// The section and its backing view. False when the commit doesn't fit in RAM + page file.
bool create_pool(u64 size) {
    g_section = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE | SEC_COMMIT, static_cast<DWORD>(size >> 32),
                                   static_cast<DWORD>(size), nullptr);
    if (!g_section) return false;
    g_backing = static_cast<u8*>(MapViewOfFile(g_section, FILE_MAP_ALL_ACCESS, 0, 0, size));
    if (g_backing) return true;
    CloseHandle(g_section);
    g_section = nullptr;
    return false;
}

void reserve_view() {
    const u64 size = AddressSpaceSize + ViewAlign;
    g_reserve = static_cast<u8*>(
        g_VirtualAlloc2(GetCurrentProcess(), nullptr, size, MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, PAGE_NOACCESS, nullptr, 0));
    if (!g_reserve) fatal("cannot reserve %llu GiB of address space for guest memory (error %lu)", size >> 30, GetLastError());
    g_reserve_end = g_reserve + size;
    g_membase = (reinterpret_cast<uintptr_t>(g_reserve) + ViewAlign - 1) & ~(ViewAlign - 1);
}

void place_view(u64 addr, u64 phys, u64 size) {
    if (!g_MapViewOfFile3(g_section, GetCurrentProcess(), view_ptr(addr), phys, size, MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE,
                          nullptr, 0))
        fatal("mem: mapping guest %#llx (+%#llx) failed (error %lu)", addr, size, GetLastError());
}

// [at, at + size) becomes a placeholder of its own, split off the one containing it.
void split_placeholder(u8* at, u64 size) {
    if (!VirtualFree(at, size, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER))
        fatal("mem: splitting a placeholder at %p (+%#llx) failed (error %lu)", at, size, GetLastError());
}

void unplace_view(u64 addr) {
    if (!g_UnmapViewOfFile2(GetCurrentProcess(), view_ptr(addr), MEM_PRESERVE_PLACEHOLDER))
        fatal("mem: unmapping guest %#llx failed (error %lu)", addr, GetLastError());
}

#else  // Linux

int g_pool_fd = -1;

bool create_pool(u64 size) {
    g_pool_fd = memfd_create("kart guest memory", MFD_CLOEXEC);
    if (g_pool_fd < 0) fatal("cannot create the guest memory pool (memfd_create: %s)", std::strerror(errno));
    if (ftruncate(g_pool_fd, static_cast<off_t>(size)) == 0) {
        void* p = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_NORESERVE, g_pool_fd, 0);
        if (p != MAP_FAILED) {
            g_backing = static_cast<u8*>(p);
            return true;
        }
    }
    close(g_pool_fd);
    g_pool_fd = -1;
    return false;
}

void reserve_view() {
    const u64 size = AddressSpaceSize + ViewAlign;
    void* p = mmap(nullptr, size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (p == MAP_FAILED) fatal("cannot reserve %llu GiB of address space for guest memory (%s)", size >> 30, std::strerror(errno));
    g_reserve = static_cast<u8*>(p);
    g_reserve_end = g_reserve + size;
    g_membase = (reinterpret_cast<uintptr_t>(g_reserve) + ViewAlign - 1) & ~(ViewAlign - 1);
}

void place_view(u64 addr, u64 phys, u64 size) {
    if (mmap(view_ptr(addr), size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, g_pool_fd, static_cast<off_t>(phys)) == MAP_FAILED)
        fatal("mem: mapping guest %#llx (+%#llx) failed (%s%s)", addr, size, std::strerror(errno),
              errno == ENOMEM ? "; raise vm.max_map_count" : "");
}

// [addr, addr + size) back to the reservation.
void unplace_range(u64 addr, u64 size) {
    if (mmap(view_ptr(addr), size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED, -1, 0) == MAP_FAILED)
        fatal("mem: unmapping guest %#llx (+%#llx) failed (%s)", addr, size, std::strerror(errno));
}
#endif

// Imports of guest memory by the GPU (VK_EXT_external_memory_host), counted per 64 KiB of the pool (under g_lock). On
// Linux the driver pins the pages an import found: a hole punched under it drops them from the memfd, the CPU then faults
// in new zero pages while the GPU goes on reading the old ones, so reused memory (vertices, indices, constants) read
// stale on the GPU: stretched and broken models. Freed memory there is kept until the last import over it goes.
constexpr u64 PinShift = 16;
std::unique_ptr<u16[]> g_pins;

void mark_freed(u64 phys, u64 size) {
    for (u64 page = phys / PageSize, last = (phys + size) / PageSize; page < last; ++page)
        g_freed[page / 64].fetch_or(u64{1} << (page % 64), std::memory_order_relaxed);
}
void unmark_freed(u64 phys, u64 size) {
    for (u64 page = phys / PageSize, last = (phys + size) / PageSize; page < last; ++page)
        g_freed[page / 64].fetch_and(~(u64{1} << (page % 64)), std::memory_order_relaxed);
}

// Zeroes the pages of a run that held guest data; the others were never written or were discarded
// (they read zero), and writing them would make RAM resident for memory the guest may never use.
void clear_freed(u64 phys, u64 size) {
    const u64 first = phys / PageSize, last = (phys + size) / PageSize;
    u64 run = last;
    const auto flush = [&](u64 end) {
        if (run < end) std::memset(g_backing + run * PageSize, 0, (end - run) * PageSize);
        run = last;
    };
    for (u64 page = first; page < last; ++page) {
        if ((g_freed[page / 64].load(std::memory_order_relaxed) >> (page % 64)) & 1) {
            if (run == last) run = page;
        } else {
            flush(page);
        }
    }
    flush(last);
}

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
u64 g_discarded = 0;
std::map<u64, u64> g_phys_free;     // free physical runs: offset -> size
u64 g_phys_used = 0, g_phys_top = 0;  // bytes handed out now; the highest offset ever handed out (statistics)
// Every 30 s at most, from the allocator: how much of the pool the guest holds (HOST_MEM_STATS=1).
void phys_stats() {
    static const bool on = [] {
        const char* v = std::getenv("HOST_MEM_STATS");
        return v && v[0] == '1';
    }();
    if (!on) return;
    static auto at = std::chrono::steady_clock::now();
    const auto now = std::chrono::steady_clock::now();
    if (now - at < std::chrono::seconds(30)) return;
    at = now;
    LOG("mem: guest physical memory in use %llu MB, highest offset %llu MB, %llu MB discarded so far", g_phys_used >> 20,
        g_phys_top >> 20, g_discarded >> 20);
}
u64 g_heap_size = 0;

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

// Make `addr` the start of a mapping if a mapping covers it. A view can't be split in place: it is
// unmapped, its placeholder split, and both halves mapped again. Another thread touching it meanwhile
// faults and retries once the memory lock is free (wait_mapped).
void split_mapping(u64 addr) {
    auto it = g_mappings.upper_bound(addr);
    if (it == g_mappings.begin()) return;
    --it;
    if (it->first == addr || addr >= it->first + it->second.size) return;
    const u64 head = addr - it->first;
    Mapping tail{it->second.size - head, it->second.phys + head, it->second.owned};
#ifdef _WIN32
    unplace_view(it->first);
    split_placeholder(view_ptr(it->first), head);
    place_view(it->first, it->second.phys, head);
    place_view(addr, tail.phys, tail.size);
#endif
    it->second.size = head;
    g_mappings.emplace(addr, tail);
}

// The free placeholder a view at [addr, addr + size) is or would be in: from the end of the view
// before it (or the reservation's start) to the start of the view after it (or the reservation's
// end). `next` is the first mapping after it.
std::pair<u8*, u8*> free_space(std::map<u64, Mapping>::iterator next, std::map<u64, Mapping>::iterator prev_end) {
    u8* lo = g_reserve;
    if (prev_end != g_mappings.begin()) {
        const auto prev = std::prev(prev_end);
        lo = view_ptr(prev->first + prev->second.size);
    }
    return {lo, next == g_mappings.end() ? g_reserve_end : view_ptr(next->first)};
}

// Maps a view of [phys, phys + size) at guest `addr` (nothing mapped there).
void map_one(u64 addr, u64 phys, u64 size) {
#ifdef _WIN32
    const auto next = g_mappings.lower_bound(addr);
    const auto [lo, hi] = free_space(next, next);
    if (lo != view_ptr(addr) || hi != view_ptr(addr + size)) split_placeholder(view_ptr(addr), size);
#endif
    place_view(addr, phys, size);
}

// Unmaps one view and merges its placeholder with the free space around it.
void unmap_one(std::map<u64, Mapping>::iterator it) {
#ifdef _WIN32
    unplace_view(it->first);
    const auto [lo, hi] = free_space(std::next(it), it);
    if (lo == view_ptr(it->first) && hi == view_ptr(it->first + it->second.size)) return;
    if (!VirtualFree(lo, hi - lo, MEM_RELEASE | MEM_COALESCE_PLACEHOLDERS))
        fatal("mem: merging placeholders at %p (+%#llx) failed (error %lu)", lo, static_cast<u64>(hi - lo), GetLastError());
#else
    unplace_range(it->first, it->second.size);
#endif
}

// Freed physical pages leave RAM: until the pool hands them out again only the backing view maps
// them, and first fit often places the next allocation elsewhere, so they stayed resident unused.
// Discarded section pages read zero again (scratchpad disc test), so they need no clearing when
// reused either: clearing a reused 4 MiB sparse block made all of it resident for a 256 KiB write.
// Returns false when they were kept (HOST_KEEP_FREED=1, or the discard failed): then they are
// marked freed and cleared on reuse. HOST_KEEP_FREED=1 restores that.
// Linux: the pool's pages go with a hole punched in the memfd, and read zero again. Not under the GPU's imports (pinned,
// see pin): free_run keeps those until unpin.
bool discard_pages(u64 phys, u64 size) {
#ifdef _WIN32
    return DiscardVirtualMemory(g_backing + phys, size) == 0;
#else
    return fallocate(g_pool_fd, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE, static_cast<off_t>(phys), static_cast<off_t>(size)) == 0;
#endif
}
bool discard_run(u64 phys, u64 size) {
    // Checked once on the pool's last page (the allocator hands it out last): written, discarded,
    // then it must read zero.
    static const bool keep = [] {
        if (const char* v = std::getenv("HOST_KEEP_FREED"); v && v[0] == '1') return true;
        u8* const page = g_backing + g_phys_size - PageSize;
        std::memset(page, 0xAB, PageSize);
        bool zero = discard_pages(g_phys_size - PageSize, PageSize);
        for (u64 i = 0; zero && i < PageSize; ++i) zero = page[i] == 0;
        if (!zero) std::memset(page, 0, PageSize);   // (the allocator expects never-used pages to read zero)
        if (!zero) LOG("mem: discarded pages do not read zero; freed guest memory stays resident");
        return !zero;
    }();
    if (keep || !discard_pages(phys, size)) return false;
    const u64 before = g_discarded;
    g_discarded += size;
    if ((before >> 30) != (g_discarded >> 30)) LOG("mem: %llu MiB of freed guest memory returned to the system so far", g_discarded >> 20);
    return true;
}

#ifndef _WIN32
// Linux: freed memory stays in RAM, cleared on reuse, unless HOST_DISCARD_FREED=1. Then it leaves RAM a while after the
// guest frees it (HOST_DISCARD_DELAY_MS, 3000), not at once: the renderer runs behind the game and still reads some
// memory the game has just freed (which, not found yet). Windows' DiscardVirtualMemory leaves the old content until RAM
// runs short, but a hole punched at once read zero there, and the GPU hung within minutes in the title screen's demo
// (NVRM Xid 109, CTX SWITCH TIMEOUT). Pages under the GPU's imports wait for the last import over them to go (unpin),
// as the GPU would go on reading the pinned old pages. It saved next to nothing (the title demo: 7.4 GB of guest memory
// after 10 minutes either way): the game keeps ~10 GB allocated and reuses what it frees.
bool discard_on() {
    static const bool on = [] {
        const char* v = std::getenv("HOST_DISCARD_FREED");
        return v && v[0] == '1';
    }();
    return on;
}
struct DueDiscard {
    u64 lo, hi;
    std::chrono::steady_clock::time_point at;
};
std::deque<DueDiscard> g_due;   // (under g_lock)

// The free, unpinned parts of [lo, hi) out of RAM: they read zero afterwards, so need no clearing on reuse.
void discard_free_in(u64 lo, u64 hi) {
    auto it = g_phys_free.upper_bound(lo);
    if (it != g_phys_free.begin()) --it;
    for (; it != g_phys_free.end() && it->first < hi; ++it) {
        for (u64 a = std::max(lo, it->first), b = std::min(hi, it->first + it->second); a < b;) {
            const bool pinned = g_pins[a >> PinShift] != 0;
            u64 n = std::min(b, ((a >> PinShift) + 1) << PinShift);
            while (n < b && (g_pins[n >> PinShift] != 0) == pinned) n = std::min(b, n + (u64{1} << PinShift));
            if (!pinned && discard_run(a, n - a)) unmark_freed(a, n - a);
            a = n;
        }
    }
}

void discard_later(u64 lo, u64 hi) {
    if (discard_on()) g_due.push_back({lo, hi, std::chrono::steady_clock::now()});
}

// The discards that are due (from the allocator, which the game calls often).
void discard_due() {
    static const auto delay = std::chrono::milliseconds([] {
        const char* v = std::getenv("HOST_DISCARD_DELAY_MS");
        return v && *v ? std::strtoll(v, nullptr, 0) : 3000;
    }());
    const auto now = std::chrono::steady_clock::now();
    while (!g_due.empty() && now - g_due.front().at >= delay) {
        const DueDiscard d = g_due.front();
        g_due.pop_front();
        discard_free_in(d.lo, d.hi);
    }
}
#endif

void free_run(u64 phys, u64 size) {
    g_phys_used -= size;
#ifdef _WIN32
    if (!discard_run(phys, size)) mark_freed(phys, size);
#else
    mark_freed(phys, size);   // (cleared on reuse, unless discarded first)
    discard_due();
    discard_later(phys, phys + size);   // (discard_free_in checks it is still free then)
#endif
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
        unmap_one(it);
        if (it->second.owned) free_run(it->second.phys, it->second.size);
        it = g_mappings.erase(it);
    }
}

void map_view(u64 addr, const std::vector<PhysRun>& runs, bool owned) {
    unmap_view(addr, [&] { u64 s = 0; for (auto& r : runs) s += r.size; return s; }());
    for (const PhysRun& r : runs) {
        map_one(addr, r.phys, r.size);
        g_mappings.emplace(addr, Mapping{r.size, r.phys, owned});
        addr += r.size;
    }
}

}  // namespace

void init() {
    // The pool is committed up front (a pagefile-backed section) but only touched pages use RAM.
    // 24 GiB: the game's memory (8.2 GB at boot) plus GPU-only regions (~2.8 GB) plus the sparse
    // regions' blocks, which are backed as the GPU first writes them and kept (~250 MB more per
    // course): ~15 GB allocated after a race, ~2 GB of it resident. Machines whose RAM + page file
    // can't take the commit (a 16 GB handheld) get the largest of 20, 16, 14, 12 GiB that fits;
    // past the pool's last GiB the GPU's sparse and GPU-only memory goes without backing.
    // A size that fits but leaves the rest of the process too little commit is skipped too: the GPU driver
    // allocates from the same RAM + page file, and on an integrated GPU so do its images and pipelines (an
    // Intel handheld took 24 GiB and then every image of the first frame failed, out of host memory, and the
    // device was lost). HOST_PHYS_HEADROOM_GIB sets that reserve (8).
    std::vector<u64> sizes{24, 20, 16, 14, 12};
#ifdef _WIN32
    MEMORYSTATUSEX ms{sizeof ms};
    GlobalMemoryStatusEx(&ms);
#else
    // Linux commits nothing up front (the pool's pages are taken as they are touched): what bounds it is RAM + swap.
    struct {
        u64 ullTotalPhys = 0, ullTotalPageFile = 0, ullAvailPageFile = 0;
    } ms;
    {
        std::ifstream mi("/proc/meminfo");
        u64 swap = 0, avail = 0, swap_free = 0;
        for (std::string key; mi >> key;) {
            u64 kb = 0;
            mi >> kb;
            if (key == "MemTotal:") ms.ullTotalPhys = kb << 10;
            else if (key == "SwapTotal:") swap = kb << 10;
            else if (key == "MemAvailable:") avail = kb << 10;
            else if (key == "SwapFree:") swap_free = kb << 10;
            mi.ignore(64, '\n');
        }
        ms.ullTotalPageFile = ms.ullTotalPhys + swap;
        ms.ullAvailPageFile = ms.ullTotalPageFile;   // (not what is free now: the pool is only used as it is touched)
        (void)avail, (void)swap_free;
    }
#endif
    u64 headroom = 8;
    if (const char* v = std::getenv("HOST_PHYS_HEADROOM_GIB")) headroom = std::strtoull(v, nullptr, 0);
    LOG("memory: %llu MB RAM, %llu MB of %llu MB commit (RAM + page file) free; %llu GiB kept for the rest", ms.ullTotalPhys >> 20,
        ms.ullAvailPageFile >> 20, ms.ullTotalPageFile >> 20, headroom);
    if (const char* v = std::getenv("HOST_PHYS_GIB")) sizes = {std::strtoull(v, nullptr, 0)};
    else
        while (sizes.size() > 1 && ((sizes.front() + headroom) << 30) > ms.ullAvailPageFile) {
            LOG("guest memory pool: %llu GiB would leave less than %llu GiB of commit", sizes.front(), headroom);
            sizes.erase(sizes.begin());
        }
#ifdef _WIN32
    load_memory_api();
#endif
    for (const u64 gib : sizes) {
        g_phys_size = gib << 30;
        if (create_pool(g_phys_size)) break;
        g_phys_size = 0;
        LOG("guest memory pool: %llu GiB doesn't fit in RAM + page file", gib);
    }
#ifdef _WIN32
    if (!g_phys_size)
        fatal("cannot reserve guest memory: even %llu GiB doesn't fit in RAM + page file. Free memory or "
              "enlarge the page file (System > About > Advanced system settings > Performance > Virtual memory).",
              sizes.back());
#else
    if (!g_phys_size) fatal("cannot create guest memory: even %llu GiB failed (%s)", sizes.back(), std::strerror(errno));
#endif
    LOG("guest memory pool: %llu GiB", g_phys_size >> 30);
#ifndef _WIN32
    LOG("mem: freed guest memory %s", discard_on() ? "leaves RAM 3 s after it is freed (HOST_DISCARD_FREED=1)" : "stays in RAM, cleared on reuse");
#endif
    reserve_view();
    g_freed = std::make_unique<std::atomic<u64>[]>(g_phys_size / PageSize / 64);
    g_pins = std::make_unique<u16[]>((g_phys_size >> PinShift) + 1);
    g_phys_free.emplace(0, g_phys_size);
    g_blocks.emplace(0, Block{AddressSpaceSize, Free, None, 0});
}

std::vector<PhysRun> phys_alloc(u64 size) {
    std::scoped_lock lk{g_lock};
    std::vector<PhysRun> runs;
#ifndef _WIN32
    discard_due();
#endif
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
    for (const PhysRun& r : runs) {
        clear_freed(r.phys, r.size);
        g_phys_used += r.size, g_phys_top = std::max(g_phys_top, r.phys + r.size);
    }
    phys_stats();
    return runs;
}

void phys_free(const std::vector<PhysRun>& runs) {
    std::scoped_lock lk{g_lock};
    for (const PhysRun& r : runs) free_run(r.phys, r.size);
    phys_stats();
}

u64 phys_free_bytes() {
    std::scoped_lock lk{g_lock};
    u64 total = 0;
    for (const auto& [off, size] : g_phys_free) total += size;
    return total;
}

std::vector<PhysRun> pin(const void* host, u64 size) {
    std::scoped_lock lk{g_lock};
    const u8* const p = static_cast<const u8*>(host);
    std::vector<PhysRun> runs;
    if (p >= g_backing && p < g_backing + g_phys_size) runs.push_back({static_cast<u64>(p - g_backing), std::min<u64>(size, g_backing + g_phys_size - p)});
    else runs = phys_runs(static_cast<u64>(p - reinterpret_cast<const u8*>(g_membase)), size);
    for (const PhysRun& r : runs)
        for (u64 g = r.phys >> PinShift, last = (r.phys + r.size - 1) >> PinShift; g <= last; ++g) ++g_pins[g];
    return runs;
}

void unpin(const std::vector<PhysRun>& runs) {
    std::scoped_lock lk{g_lock};
    std::vector<std::pair<u64, u64>> open;   // granule spans no import holds now
    for (const PhysRun& r : runs)
        for (u64 g = r.phys >> PinShift, last = (r.phys + r.size - 1) >> PinShift; g <= last; ++g) {
            if (!g_pins[g] || --g_pins[g]) continue;
            if (!open.empty() && open.back().second == g) open.back().second = g + 1;
            else open.emplace_back(g, g + 1);
        }
#ifndef _WIN32
    // The free memory there can leave RAM now (also the never-used pages the import made resident), after the delay.
    for (const auto& [g0, g1] : open) discard_later(g0 << PinShift, std::min(g1 << PinShift, g_phys_size));
#endif
}

u8* phys_ptr(u64 phys) { return g_backing + phys; }
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
#ifndef _WIN32
    const int prot = write ? PROT_READ | PROT_WRITE : read ? PROT_READ : PROT_NONE;
    auto m = g_mappings.upper_bound(addr);
    if (m != g_mappings.begin()) --m;
    for (; m != g_mappings.end() && m->first < addr + size; ++m) {
        const u64 lo = std::max(addr, m->first), hi = std::min(addr + size, m->first + m->second.size);
        if (lo < hi && mprotect(view_ptr(lo), hi - lo, prot) != 0)
            LOG("mem: protecting guest %#llx (+%#llx) failed (%s)", lo, hi - lo, std::strerror(errno));
    }
    return;
#else
    const DWORD protection = write ? PAGE_READWRITE : read ? PAGE_READONLY : PAGE_NOACCESS;
    // One call per view: VirtualProtect doesn't cross views.
    auto it = g_mappings.upper_bound(addr);
    if (it != g_mappings.begin()) --it;
    for (; it != g_mappings.end() && it->first < addr + size; ++it) {
        const u64 lo = std::max(addr, it->first), hi = std::min(addr + size, it->first + it->second.size);
        DWORD old = 0;
        if (lo < hi && !VirtualProtect(view_ptr(lo), hi - lo, protection, &old))
            LOG("mem: protecting guest %#llx (+%#llx) failed (error %lu)", lo, hi - lo, GetLastError());
    }
#endif
}

// Permissions are bookkeeping only: host code reads guest memory through the view whatever the
// guest's permission, and the view's host protection belongs to the debug write watch (watch.cpp).
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
