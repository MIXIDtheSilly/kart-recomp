// Guest memory: physical pages from one backing pool (a pagefile-backed section, also mapped whole
// as the backing view), mapped into the 512 GiB fastmem view at g_membase. Also the kernel's view
// of the address space (memory state, permission and attribute per block) for svcQueryMemory and
// friends.
#pragma once
#include <map>
#include <mutex>
#include <vector>

#include "common.h"

namespace mem {

constexpr u64 PageSize = 0x1000;
constexpr u64 AddressSpaceSize = 1ull << 39;

enum State : u32 {
    Free = 0x00, Io = 0x01, Static = 0x02, Code = 0x03, CodeData = 0x04, Normal = 0x05, Shared = 0x06,
    Alias = 0x07, AliasCode = 0x08, AliasCodeData = 0x09, Ipc = 0x0A, Stack = 0x0B, ThreadLocal = 0x0C,
    Transferred = 0x0D, SharedTransferred = 0x0E, SharedCode = 0x0F, Inaccessible = 0x10,
};
enum Perm : u32 { None = 0, R = 1, W = 2, X = 4, RW = 3, RX = 5 };
enum Attr : u32 { Locked = 1, IpcLocked = 2, DeviceShared = 4, Uncached = 8, PermissionLocked = 0x10, GpuAccessible = 0x20 };

// Address-space layout reported through svcGetInfo (values the console kernel reports for this title).
constexpr u64 AslrBase = 0x8000000, AslrSize = 0x7ff8000000;
constexpr u64 AliasBase = 0x1105400000, AliasSize = 0x1000000000;
constexpr u64 HeapBase = 0x2105400000, HeapSize = 0x400000000;
constexpr u64 StackBase = 0x1085400000, StackSize = 0x80000000;
constexpr u64 Nx2Base = 0x1905400000, Nx2Size = 0x800000000;

struct Info {
    u64 addr, size;
    u32 state, attr, perm;
};

// A run of physical memory: an offset into the backing pool.
struct PhysRun {
    u64 phys, size;
};

void init();

// Map fresh zeroed memory.
void map(u64 addr, u64 size, u32 state, u32 perm);
// Map physical memory the caller owns (shared memory, an alias of other guest memory).
void map_phys(u64 addr, const std::vector<PhysRun>& runs, u32 state, u32 perm);
// Unmap; physical memory that map() allocated for the range is freed.
void unmap(u64 addr, u64 size);
// Called after map(), map_phys() and unmap() change what backs [addr, addr + size), outside the
// memory lock: the renderer's mappings of guest memory follow the CPU's (GPU address == CPU address).
using RemapListener = void (*)(u64 addr, u64 size);
void add_remap_listener(RemapListener listener);

// Physical memory behind [addr, addr + size), in address order (unmapped pages are skipped).
std::vector<PhysRun> phys_runs(u64 addr, u64 size);
// Guest pages mapping the same physical page as `addr` (itself included); `others` gets up to four.
u32 alias_count(u64 addr, std::vector<u64>* others);
// Host pointer to the backing of a guest page (the backing view), or nullptr if unmapped.
u8* backing_ptr(u64 addr);
// Whether a guest page is mapped, after any map or unmap in progress on another thread has finished
// (partly unmapping a view briefly unmaps all of it on Windows: a fault there is retried).
bool wait_mapped(u64 addr);
// Host protection of the mapped parts of a range in the view (the debug write watch, watch.cpp).
void protect_view(u64 addr, u64 size, bool read, bool write);

// Physical pages that outlive any one mapping (shared memory).
std::vector<PhysRun> phys_alloc(u64 size);
void phys_free(const std::vector<PhysRun>& runs);
u64 phys_free_bytes();  // what is left of the pool
u8* phys_ptr(u64 phys);
u64 phys_pool_bytes();  // size of the whole pool

void set_perm(u64 addr, u64 size, u32 perm);
void set_attr(u64 addr, u64 size, u32 mask, u32 attr);
void set_state(u64 addr, u64 size, u32 state, u32 perm);
Info query(u64 addr);
bool is_free(u64 addr, u64 size);
bool all_state(u64 addr, u64 size, u32 state);
// Free range of `size` inside [region, region + region_size), aligned.
u64 find_free(u64 region, u64 region_size, u64 size, u64 align = PageSize);

// svcSetHeapSize
u32 set_heap_size(u64 size, u64* out_addr);
u64 heap_size();

}  // namespace mem
