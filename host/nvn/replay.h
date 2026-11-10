// Native NVN: the interface between the NVN interface layer (nvn/*.cpp: objects, command recording, queue, sync,
// window, device queries) and the renderer behind it (replay_null.cpp: no GPU; a Vulkan replay later).
// The rp_* functions are what nxnative's interface layer asks of its Metal replay (the mtl_* functions of
// nvn_mtl.m, renamed), restricted to the semantics the interface layer needs. Statistics, telemetry and Apple-specific
// calls are gone.
//
// The model, as in nxnative:
//   - The interface layer records NVN commands into the game's command memory as records (nvn_obj.h) and does
//     nothing else with them until nvnQueueSubmitCommands. A submission is
//         rp_prepare(); rp_queue_activate(q); rp_sub_begin(); for each handle { rp_exec_rest(rest); rp_exec(handle); } rp_exec_rest(null, 0); fence = rp_sub_commit();
//     with the submission lock held: rp_prepare .. rp_sub_commit never run concurrently, whatever the queues.
//   - rp_exec walks a recording (following JUMP and CALL) and executes it. Everything with a CPU-visible result
//     (event signals, counter reports, buffer copies / fills / inline uniform updates) is the renderer's to
//     perform, in command order, through the rp_sub_* operations it implements for itself.
//   - THE COMPLETION RULE: every visible effect of GPU completion (the value written by a SignalEvent or
//     ReportValue, a counter report, the bytes a buffer copy / fill / uniform update writes into guest memory
//     the CPU can read, a sync object counting as signalled, a texture read back) must happen only after the
//     GPU work that produces it has completed, and before rp_wait(fence) reports that fence complete or
//     rp_finish() returns. The game polls these values and orders its own reuse of memory by them; one that
//     appears early lets it reuse memory the GPU still reads. A renderer that completes in order may apply
//     them at any point before the fence completes (the null renderer does it at exec time).
//   - Fences are 64-bit and increase by one per commit (first commit: 1); fence 0 is always complete.
//   - Which thread: every function is called from a guest thread (a host thread running recompiled code).
//     Functions that can block for GPU progress (rp_wait*, rp_finish, rp_drain, rp_present_wait) are called by
//     the interface layer inside nvn::HostWait, so the guest thread's emulated core is free meanwhile; the
//     renderer must not wrap them itself. The renderer may keep its own threads.
//   - GPU address == guest virtual address; addresses below are GUEST addresses (u64) unless "host" is said.
//     See nvn_obj.h for the conventions of the objects and records passed in.
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "../common.h"

#include "nvn_obj.h"

namespace nvn {

// ---------------------------------------------------------------- device state

// DeviceInitialize / SetWindowOriginMode / SetDepthMode / queue initialization. origin: the window origin is
// the lower left (NVN's default; viewports flip, winding reverses, present flips). Called from any thread.
void rp_set_origin_lower_left(int on);
// Depth range convention: on = near 0 (D3D / Vulkan), off = near -w (GL, NVN's default).
void rp_set_depth_zero(int on);
// The origin the queue captured at QueueInitialize (NVN emits the window origin into a queue's initial GPU state).
// Called with the submission lock held right before the queue's submission; the value stays until the next call.
void rp_set_queue_origin_lower_left(int on);

// ---------------------------------------------------------------- memory pools

// A memory pool (storage in guest memory, `gpu` = its guest address) exists / is gone. A sparse virtual pool does
// not call this; it reports through rp_vpool_*. Pools may be added and removed on any thread while submissions run.
// A removed range must not be touched by the renderer afterwards (the game frees the memory).
void rp_pool_add(u64 gpu, u64 size, u32 flags);
void rp_pool_invalidated(u64 gpu, u64 size);   // the CPU is about to read what the GPU wrote in [gpu, gpu + size) (InvalidateMappedRange of CPU-cached memory)
void rp_pool_flushed(u64 gpu, u64 size);   // the CPU published [gpu, gpu + size) (FlushMappedRange of CPU-cached memory)
void rp_pool_del(u64 gpu);
// A pool object is being finalized: forget every texture / descriptor the renderer derived from that object
// (storage-sharing tables keyed by the pool). Called before rp_pool_del and before the object's magic is cleared.
void rp_alias_drop_pool(const pool_t* pool);

// Sparse virtual pools (NVN virtual memory pools, MapVirtual). `base` is the guest address range reserved for the
// pool (not mapped in the guest address space: do not dereference it; use nvn::gpu_ptr / sparse_* below).
// rp_vpool_create: a sparse pool exists at base, size bytes. rp_vpool_map: bytes [voff, voff+size) of the pool
// now show guest memory at `phys` (a guest address of a physical pool's storage), or nothing when phys == 0.
// Offsets and sizes are 4 KB multiples. Later requests overwrite earlier ones. rp_vpool_destroy: the pool is gone.
// The interface layer keeps the 4 KB page table (nvn::sparse_*) in step before calling these, and calls them with its
// sparse-pool lock held (so mappings reach the renderer in the order of the page table): they must not call
// sparse_* / gpu_* / host_readable and must not wait for a thread that does.
void rp_vpool_create(u64 base, u64 size);
void rp_vpool_map(u64 base, u64 voff, u64 size, u64 phys);
void rp_vpool_destroy(u64 base);

// ---------------------------------------------------------------- descriptors (texture / sampler / image pools)

// All descriptor pools share one ID space per kind in the renderer: pool k's IDs start at a base the interface
// layer assigns (nvn_obj.h cmd_t::tex_base), and every id below is already base + the game's ID.
// The capacity of the texture table (IDs at or past it read as "empty slot"). Grows as pools are initialized.
void rp_set_tex_pool_n(int n);
// RegisterTexture: id now names texture t (as seen through view v, null = whole texture). The renderer snapshots
// what it needs: t is the game's object, which may be copied, finalized or reused later. Called on any thread.
void rp_reg_tex(int id, const tex_t* t, const view_t* v);
// RegisterImage: same table, declares shader-write use of the texture even if the builder omitted it.
void rp_reg_image(int id, const tex_t* t, const view_t* v);
void rp_reg_samp(int id, const samp_par* p);

// ---------------------------------------------------------------- textures

// TextureInitialize: t is a new live texture (t->serial > 0, unique); the renderer may create its image lazily.
void rp_tex_init(const tex_t* t);
// TextureFinalize / re-initialization of a live object: release everything of t->serial. t->magic is still valid.
void rp_tex_release(tex_t* t);
// The CPU filled the storage of t (WriteTexels, packaged data at Initialize): upload from guest memory before the next use.
void rp_tex_cpu_written(const tex_t* t);
// Whether the renderer's copy of the texture is GPU content (render target / shader-writable): never re-uploaded from guest memory.
int rp_tex_gpu_owned(uint64_t serial);
// ReadTexels: starting at (level, layer0) of t, write the region r into host memory dst with row / img byte strides
// (blocks of the texture's format). Must reflect GPU writes completed so far (called after the game synced); the
// null renderer writes zeros.
void rp_read_texels(const tex_t* t, unsigned level, unsigned layer0, const NVNcopyRegion* r, uint8_t* dst, size_t row, size_t img);

// ---------------------------------------------------------------- programs

// A program's shaders changed (SetShaders) or the program is finalized: drop what was derived from prog->serial.
void rp_prog_release(prog_t* p);
// A new program's shaders may be loaded in the background now (not at its first draw). Keys: nvn::prog_key (program.cpp).
void rp_prog_prefetch(const prog_t* p);
// Program finalize, submission lock held: drop it from every queue's bindings (NVN reads a program only when bound;
// a game may finalize one a queue still has bound and reuse its memory).
void rp_prog_forget(const prog_t* p);

// ---------------------------------------------------------------- queues / submissions

// Queue serial (queue_t::serial): the renderer keeps binding state per queue. activate: the next submission is
// this queue's (lock held). forget: the queue was finalized (lock held).
void rp_queue_activate(uint64_t serial);
void rp_queue_forget(uint64_t serial);
// One-time work before the first submission (shader prewarm, pipeline prebuild); runs under the submission lock
// but may take long; called before every submission, a no-op after the first.
void rp_prepare();
// Start a submission. Everything between sub_begin and sub_commit forms one GPU submission (the renderer may cut it internally).
void rp_sub_begin();
// Execute the recording `handle` (HOST pointer to its first record; the game's NVNcommandHandle is ga() of it).
// Returns at the recording's END. Follows OP_JUMP; OP_CALL executes the callee recursively and continues.
void rp_exec(const uint8_t* handle);
// The handles of this submission that still follow the one rp_exec is about to run (guest values as the game
// passed them), for lookahead across handles. n == 0 at the end.
void rp_exec_rest(const uint64_t* handles, int n);
// Commit the submission; returns its fence value (> every earlier one). Effects of the submission are visible
// once rp_wait(fence) is true. The renderer must set sync_t::fence for OP_FENCE_SYNC records executed in this
// submission to rp_last_fence() + 1 (the value this commit returns).
uint64_t rp_sub_commit();

// In-order operations of a submission with a CPU-visible effect, called by the renderer's own record walker
// (rp_exec) for the records of the same names; part of the contract so every renderer treats them alike.
// All addresses are GPU (guest) addresses; sparse ranges must be resolved with nvn::gpu_ptr.
void rp_sub_copy(u64 src, u64 dst, u64 n);                         // OP_COPY_B2B
void rp_sub_update(const void* data, u64 dst, u64 n);              // OP_UPDATE_UB: data is host memory (copy it now: command memory may be reused)
void rp_sub_fill(u64 dst, u64 n, uint32_t value);                  // OP_CLEAR_BUF
void rp_sub_event(volatile uint32_t* val, uint32_t mode, uint32_t v);   // OP_SIGNAL_EVENT: val is a host pointer; mode NVN_EVENT_SIGNAL_MODE_WRITE / ADD
// OP_REPORT_COUNTER: write the 16-byte NVNcounterData {count, timestamp} for counter `type` to addr. The timestamp
// is the GPU time in 614.4 MHz ticks on the guest clock (nvn::gpu_ticks). The count field of TIMESTAMP and
// TIMESTAMP_TOP reports is 0 (this game runs with render conditions: the GPU writes no count there);
// SAMPLES_PASSED counts the samples since the last reset; other counters: 0.
void rp_sub_counter(u64 addr, uint32_t type);

// ---------------------------------------------------------------- fences / waiting (called inside nvn::HostWait)

// The fence of the newest commit.
uint64_t rp_last_fence();
// Wait until `fence` is complete (all its CPU-visible effects done) or timeout_ns passed (0 = poll; UINT64_MAX = forever).
// rp_wait is the game's SyncWait on a fence from QueueFenceSync / CommandBufferFenceSync; rp_wait_acq on the sync
// WindowAcquireTexture hands out (same completion, kept apart so a renderer can account them differently).
bool rp_wait(uint64_t fence, uint64_t timeout_ns);
bool rp_wait_acq(uint64_t fence, uint64_t timeout_ns);
// nvnQueueFinish: wait for the GPU work submitted so far (not for the display).
void rp_finish();
// Wait for all GPU work and queued presents (shutdown / idle).
void rp_drain();

// ---------------------------------------------------------------- present

// nvnQueuePresentTexture: show texture t (a window texture) with crop {x, y, w, h} (w or h 0 = whole texture).
// Returns a ticket immediately; the display is expected to keep the image for period_s seconds (0 = unlimited).
// The interface layer paces the game (nvn::window.cpp); the renderer need not.
uint64_t rp_present(const tex_t* t, double period_s, const int crop[4]);
// Block until the present with this ticket has been processed (the image may be drawn into again). Inside HostWait.
void rp_present_wait(uint64_t ticket);
// A texture is a window (swapchain) texture: it never enters the renderer's content ledger.
void rp_window_tex(const tex_t* t);

// ---------------------------------------------------------------- services the interface layer offers the renderer (memory.cpp / cmdbuf.cpp)

// A guest address range as host memory. Normal pools: hp(addr). Sparse virtual pools: the page behind each 4 KB
// piece is somewhere else, so ranges that cross a mapping boundary have no host pointer: returns nullptr unless
// [addr, addr+n) is one contiguous run of guest memory (mapped pages of a sparse pool count only if their
// physical pages are contiguous).
uint8_t* gpu_ptr(u64 addr, u64 n);
// Copy n bytes out of / into GPU address space piecewise, through the sparse page table; unmapped pages read as
// zero (and miss is their byte count) and swallow writes. Return the number of unmapped bytes.
uint64_t gpu_read(u64 addr, void* dst, u64 n);
uint64_t gpu_write(u64 addr, const void* src, u64 n);
// Whether the host can read n bytes at a GPU address: guest memory is mapped there, and inside a sparse pool, a mapped run.
bool host_readable(u64 addr, u64 n);
// Sparse pool page table (4 KB granularity; each page remembers the mapping generation that set it).
bool sparse_pool(const pool_t* pool);                  // the pool is a sparse virtual pool (mapped or not)
uint64_t sparse_gen();                                 // bumped per mapping request
bool sparse_alias(u64 lo, u64 hi);                     // [lo, hi) overlaps a sparse pool's range
uint64_t sparse_copy(const pool_t* pool, void* dst, u64 gpu, u64 n);   // bytes of a texture in a sparse pool, unmapped pages as 0; returns the unmapped bytes
bool sparse_mapped(const pool_t* pool, u64 gpu, u64 n);              // every page of the range has memory behind it
// The physical memory (guest address) behind [gpu, gpu+n) when it is one contiguous run, else 0.
u64 sparse_phys(const pool_t* pool, u64 gpu, u64 n);
void sparse_alias_report();   // HOST_NVN_ALIAS_STATS=1: mapped vs distinct physical memory per sparse pool
bool sparse_stale(const pool_t* pool, u64 gpu, u64 n);   // mapped, but every mapped page's physical page was mapped again at another page since (a stale alias)
// A mapping in the range is newer than `gen` (gen == UINT64_MAX: do not look).
bool sparse_changed_since(const pool_t* pool, u64 gpu, u64 n, uint64_t gen);
// Whether the CPU flushed (nvnMemoryPoolFlushMappedRange) any of [addr, addr+n) of a pool: until a texture's first GPU
// use, flushed storage is its content (Breath of the Wild's sky tables; here the terrain tiles).
bool flushed(u64 addr, u64 n);
// GPU copies into a texture the renderer uploads from its storage (packaged layout, CPU-written): the hardware's copy
// writes the storage, so the host writes the copied bytes there as well, and an upload from the storage later carries them
// (Yoshi's Crafted World's texture streamer copies resident levels GPU-side and writes the finer ones from the CPU). Block-linear
// by address, no 3D. -1: cannot mirror (3D, format mismatch, region out of range): the GPU copy alone then stands.
int tex_mirror_b2t(const tex_t* t, unsigned level, unsigned layer0, const NVNcopyRegion* r, const uint8_t* src, size_t row, size_t img);
int tex_mirror_t2t(const tex_t* s, unsigned sl, unsigned ss, const NVNcopyRegion* sr, const tex_t* d, unsigned dl, unsigned ds, const NVNcopyRegion* dr);
// First index of the descriptor numbering base of a descriptor pool object (memory.cpp dpool_base).
int dpool_base(const void* pool, bool tex);
// BindTexturesDeferred records: the GPU reads the handles at execution, not at recording. Call before reading the
// record's h; it re-reads them from src (a host pointer into the game's memory). Raw words: the pool numbering
// bases are not added to deferred handles (cmdbuf.cpp logs once when a base is in force).
inline void bh_fresh(const p_bind_handles* b) {
    if (b->defer && b->src && b->n > 0 && b->n <= 32) memcpy(const_cast<uint64_t*>(b->h), reinterpret_cast<const void*>(static_cast<uintptr_t>(b->src)), 8 * static_cast<size_t>(b->n));
}
// Whole-submission program stage keys: first 8 bytes of sha256(control section prefix + code area) of a program stage
// (program.cpp); false = no such stage / not computable.
bool prog_key(const prog_t* p, unsigned stage, uint8_t key[8]);
// NVN counter timestamps: 614.4 MHz ticks (384 ticks per 625 ns) of the guest clock, the clock
// nvnDeviceGetCurrentTimestampInNanoseconds returns.
uint64_t gpu_ticks();
uint64_t guest_ns();

}  // namespace nvn
