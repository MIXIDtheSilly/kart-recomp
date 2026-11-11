// Memory pools as the GPU sees them: a GPU address (= guest address) -> the Vulkan buffer holding it.
#pragma once
#include "vkctx.h"

namespace nvk {

struct BufferRef {
    VkBuffer buffer = VK_NULL_HANDLE;
    u64 offset = 0;          // of the address in buffer
    u64 size = 0;            // bytes from offset to the end of the buffer
    VkDeviceAddress gpu = 0; // device address of the guest address
};

// A pool's storage [addr, addr + size) (NVN pool Initialize / Finalize). Physical pools only.
void pool_add(u64 addr, u64 size, u32 flags);
// The GPU is about to write [addr, addr + n) (before the pool_find for that write), or with on_cpu the renderer will, on the CPU.
void pool_gpu_write(u64 addr, u64 n, bool on_cpu = false);
// After a GPU write of [addr, addr + n) that the CPU reads without asking (a linear render target's pixels): where the pool is
// the GPU's own, a copy of the range out to system memory is recorded. True if so (the caller ends it with a barrier).
bool pool_write_through(VkCommandBuffer cb, u64 addr, u64 n);
// The CPU published [addr, addr + n) / is about to read what the GPU wrote there. Between submissions.
void pool_flushed(u64 addr, u64 n);
void pool_invalidated(u64 addr, u64 n);
// Whether some of [addr, addr + n) is in a pool only the GPU's copy is up to date for. Any thread.
bool pool_gpu_owned(u64 addr, u64 n);
// A part of a submission begins (the first with submission): the device copies are brought up to date in its first commands.
void pool_part_begin(bool submission);
void pool_del(u64 addr);
// HOST_NVN_POOL_STATS=1: every 10 s, which pools the GPU read and wrote, how much a frame, and where each one's memory is.
// kind: 0 vertices, 1 indices, 2 uniform, 3 storage a shader only reads, 4 indirect arguments, 5 written by copies, 6 storage a
// shader writes, 7 published by the CPU, 8 asked for by the CPU. The buffer kinds count the sizes bound, not what was read.
bool pool_stats_on();
void pool_note(u64 addr, u64 bytes, int kind);

// The buffer behind [addr, addr + len); false if no pool holds all of it. use: how much from addr on the GPU will read
// (0: up to the pool's end), for the pools whose copy is brought up to date only where a submission reads it.
bool pool_find(u64 addr, u64 len, BufferRef& out, u64 use = 0);
// Steps whenever what pool_find gives for some address may change (pools made or gone, device copies made or dropped).
u64 pool_gen();

}  // namespace nvk
