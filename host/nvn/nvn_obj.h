// Native NVN: host-side object layouts and the command record format, shared by the interface layer (nvn/*.cpp,
// which writes them) and the renderer behind replay.h (which reads them). C++ port of nxnative's nvn_obj.h (MIT).
//
// Objects live in place in the NVN object memory the game allocated (host pointer = g_membase + guest address);
// each is checked against the size the game reserves for it in nvn.h (FITS). The game also writes a few of these
// bytes itself, through code inlined from the SDK headers: cmd_t +0x10, buf_t +0x18 / +0x20, the handle word of
// OP_BIND_TEX1 records. Those offsets are asserted below and must not be reused.
//
// Address conventions (this port, not nxnative's: there a GPU address is a native pointer, here it is not):
//   - GPU address == guest virtual address. Every NVNbufferAddress / NVNtextureAddress the game sees or passes,
//     and every address stored in a command record (`addr`, `src`, `dst`, `ind`, `cnt`, `ibuf`), is a GUEST
//     address; the host pointer is hp(addr) (nvn::gp<T>). A virtual (sparse) pool's address range is reserved
//     in the guest address space but not mapped: resolve it with nvn::gpu_ptr / the sparse_* queries (replay.h).
//   - Pointers to NVN objects (pool, texture, program, sync, device ...), to object snapshots, and to storage
//     inside a command record are HOST pointers. `mem` fields (pool_t::mem, tex_t::mem) are host pointers into
//     guest memory; ga(mem) is the GPU address.
//   - NVNcommandHandle = ga(first record of the recording). Host-lent command memory segments (cmdbuf.cpp) are mapped in
//     guest memory too, so a handle is always a real guest address. OP_CALL stores handles as the game holds them
//     (guest values); OP_JUMP stores the HOST pointer of the next segment.
//   - Callbacks the game gave us (cmd_t::cb, dev_t_::dbg_cb) are raw GUEST addresses of guest functions.
#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "nvn.h"
#include "nvn_tex.h"

namespace nvn {

#define NVN_MAGIC(a, b, c, d) ((uint32_t)(a) | (uint32_t)(b) << 8 | (uint32_t)(c) << 16 | (uint32_t)(d) << 24)
#define NVN_FITS(T, H) static_assert(sizeof(T) <= sizeof(H), #T " exceeds the reserved size of " #H)

// MSVC and clang lay out bit-fields of mixed types differently, so every record below that has them is pinned by a size assertion.
struct dev_builder_t { uint32_t flags; };                                                   NVN_FITS(dev_builder_t, NVNdeviceBuilder);
struct dev_t_ { uint32_t magic, flags, origin, depth_mode; uint64_t dbg_cb, dbg_data; };    NVN_FITS(dev_t_, NVNdevice);   // dbg_cb: guest function address

struct queue_builder_t { NVNdevice* dev; uint32_t flags; uint64_t cmd_sz, comp_sz, ctl_sz, flush; void* mem; uint64_t mem_sz; };
NVN_FITS(queue_builder_t, NVNqueueBuilder);
struct queue_t { uint32_t magic; NVNdevice* dev; uint64_t submits; int origin_ll; uint64_t serial; };   NVN_FITS(queue_t, NVNqueue);

struct pool_builder_t { NVNdevice* dev; void* mem; uint64_t size; uint32_t flags; };       NVN_FITS(pool_builder_t, NVNmemoryPoolBuilder);
struct vpool;
// A virtual pool (flags 0x200) keeps its sparse state in vp (struct vpool in memory.cpp, nullptr for an aliased one);
// vmode: 0 not mapped yet, 1 aliased onto one physical pool (mem = that pool's memory), 2 sparse (mem = the reserved, unmapped address range).
struct pool_t { uint32_t magic, flags; uint8_t* mem; uint64_t size; vpool* vp; int vmode; };   NVN_FITS(pool_t, NVNmemoryPool);

struct dpool_t { const pool_t* pool; int64_t off; int32_t n; };                             // Texture / sampler descriptor pool
NVN_FITS(dpool_t, NVNtexturePool); NVN_FITS(dpool_t, NVNsamplerPool);

struct tex_builder_t {
    NVNdevice* dev; nvn_texdesc d; pool_t* pool; int64_t off;
    uint8_t has_layout, layout[8];
    const void* pkg_data;                // TextureBuilderSetPackagedTextureData (host pointer): copied into the storage at TextureInitialize (13 Sentinels)
    uint8_t set_swz : 1, stencil : 1; uint16_t swz;   // TextureBuilderSetSwizzle: r | g << 3 | b << 6 | a << 9 (NVN and Vulkan-style channel enums: 0 zero 1 one 2 R 3 G 4 B 5 A);
                                         // stencil: TextureBuilderSetDepthStencilMode STENCIL (the texture reads the stencil of its depth-stencil storage, as a stencil-mode view does)
};                                                                                          NVN_FITS(tex_builder_t, NVNtextureBuilder);
// Field order packed to 0x80: newer SDKs (SMBW, NVN 55) allocate NVNtexture as 0x80 bytes; a larger record overwrites the adjacent heap block's free list.
struct tex_t {
    uint32_t magic; nvn_texdesc d; uint8_t has_layout, set_swz : 1, stencil : 1; uint16_t swz; nvn_texlayout L; uint8_t* mem; pool_t* pool; int64_t off; uint8_t layout[8];
    uint64_t serial;                     // Initialization serial: the renderer finds its texture in its own table by it (the game may copy the object by value, so object pointers cannot be stored)
};                                                                                          NVN_FITS(tex_t, NVNtexture);
static_assert(sizeof(tex_t) <= 0x80, "tex_t exceeds the newer SDKs' NVNtexture (0x80)");
// The newer SDK's NVNtextureView is only 16 bytes: the game keeps it in a 16-byte stack slot. Levels / layers / format / target + swizzle and depth-stencil mode.
struct view_t {
    uint8_t set_levels : 1, set_layers : 1, set_format : 1, set_target : 1, set_swizzle : 1, set_dsmode : 1, dsmode : 2;
    uint8_t target, base_level, nlevels; uint16_t format, min_layer, nlayers; uint8_t swz[4]; uint16_t _rsv;
};                                                                                          NVN_FITS(view_t, NVNtextureView);
static_assert(sizeof(view_t) <= 16, "view_t exceeds the newer SDKs' NVNtextureView (16 bytes)");

// Sampler parameters (one copy shared by builder and object)
struct samp_par { uint8_t minf, magf, ws, wt, wr, cmp_mode, cmp_func, reduction; float lod_min, lod_max, lod_bias, aniso, border[4]; };   // Enum values are all < 256; reduction takes the padding byte
struct samp_builder_t { NVNdevice* dev; samp_par p; };                                      NVN_FITS(samp_builder_t, NVNsamplerBuilder);
struct samp_t { uint32_t magic; samp_par p; };                                              NVN_FITS(samp_t, NVNsampler);

// The per-stage shader keys are not kept in prog_t: the game's real NVNprogram may be smaller than the header's 0xC0 (growing prog_t corrupted the adjacent object in Mario); see program.cpp.
// sd[].data / sd[].control are GUEST addresses (NVNshaderData as the game filled it in).
struct prog_t { uint32_t magic, nstage; NVNshaderData sd[6]; uint64_t serial; };           NVN_FITS(prog_t, NVNprogram);   // serial: table key for the renderer's resolved result
static_assert(sizeof(prog_t) == 112, "prog_t is 112 bytes (Mario's NVNprogram is smaller than the header's)");

struct sync_t { uint32_t magic, fenced; uint64_t fence; };   /* fenced 2: by WindowAcquireTexture */ NVN_FITS(sync_t, NVNsync);
struct ev_builder_t { const pool_t* pool; int64_t off; };                                   NVN_FITS(ev_builder_t, NVNeventBuilder);
struct ev_t { uint32_t magic; volatile uint32_t* val; };                                    NVN_FITS(ev_t, NVNevent);   // val: host pointer into the event's pool

struct win_builder_t { NVNdevice* dev; void* native; int32_t ntex, interval; NVNtexture* tex[4]; };   // tex: host pointers
NVN_FITS(win_builder_t, NVNwindowBuilder);
// avail[i]: time at which swapchain image i can be acquired again (ns, host clock) = its display time + one present period; pace: target time of this window's present pacer
struct win_t { uint32_t magic; void* native; int32_t ntex, interval, next, crop[4]; NVNtexture* tex[4]; uint64_t presents, avail[4], pace, ticket[4]; };   // ticket[i]: ticket of the most recent present of image i
NVN_FITS(win_t, NVNwindow);

// Command buffer: commands are written into the command memory given by the game (AddCommandMemory), handle = segment start address; when it runs out, the game is called back for more memory
struct cmd_t {
    uint32_t magic, recording;
    uint8_t* cur;                        // Write pointer of the current command memory segment (host pointer)
    uint64_t nvn_flag10;
                                         // +0x10 is left to the game: its inlined NVN header code writes byte 1 here after AddCommandMemory and before BeginRecording / CallCommands (an internal flag of real NVN).
                                         // It used to be the low byte of `end`: rewritten to 01 it cut the segment end by up to 254 bytes, and SMBW's material setup handles (512-byte blocks at minimum) kept overflowing into host segments
    uint8_t* end;                        // End of the current command memory segment
    uint8_t* begin;                      // First record of this recording (its handle); recording 2 = not written yet
    uint64_t cb; uint64_t cb_data;       // cb: guest address of the memory callback; cb_data: the pointer the game gave SetMemoryCallbackData, as the adapter converted it (give it back with ga())
    NVNdevice* dev;
    uint64_t used;                       // Bytes written since the game last added command memory, across recordings and host segments, including segment-switch JUMPs (returned by GetCommandMemoryUsed)
    int32_t tex_base, samp_base;         // Host-table bases of the texture / sampler pool this command buffer last bound (memory.cpp dpool_add; 0 = the first pool of its kind)
    size_t ctl_size;                     // Bytes in the last control memory block supplied by the game (the host records no control commands)
};                                                                                          NVN_FITS(cmd_t, NVNcommandBuffer);
static_assert(offsetof(cmd_t, nvn_flag10) == 0x10, "the game writes NVNcommandBuffer +0x10");

// ---------- State objects (defaults follow the NVN / GL defaults) ----------
struct color_st { uint32_t blend_en : 8, logic_op : 4, alpha_func : 4; };                    NVN_FITS(color_st, NVNcolorState);
struct chmask_st { uint32_t mask; };            /* 4 bits per target: r=1 g=2 b=4 a=8, bit set = write */ NVN_FITS(chmask_st, NVNchannelMaskState);
// adv: NVN advanced blend mode (0 = none); adv_npm: the source is not premultiplied (SetAdvancedPremultipliedSrc(false)); adv_ovl: overlap mode (0 = uncorrelated, the default)
struct blend_st { uint64_t target : 3, src_rgb : 7, dst_rgb : 7, src_a : 7, dst_a : 7, eq_rgb : 3, eq_a : 3, adv : 6, adv_npm : 1, adv_ovl : 2; };
NVN_FITS(blend_st, NVNblendState);
struct ms_st { uint8_t enable, samples, a2c, a2c_dither, raster_samples, cov_mod, cov2color_en, cov2color_out, sl_en, sl_grid; };
NVN_FITS(ms_st, NVNmultisampleState);
struct poly_st { uint32_t cull : 2, front : 1, mode : 2, offset_en : 3; };                   NVN_FITS(poly_st, NVNpolygonState);
struct ds_st {
    uint64_t dt : 1, dw : 1, df : 4, st : 1, sf_f : 4, sf_b : 4,
             fail_f : 4, zfail_f : 4, zpass_f : 4, fail_b : 4, zfail_b : 4, zpass_b : 4;
};                                                                                          NVN_FITS(ds_st, NVNdepthStencilState);
struct va_st { uint32_t fmt : 8, offset : 19, stream : 5; };                                 NVN_FITS(va_st, NVNvertexAttribState);
struct vs_st { uint32_t stride, divisor; };                                                 NVN_FITS(vs_st, NVNvertexStreamState);
static_assert(sizeof(color_st) == 4 && sizeof(blend_st) == 8 && sizeof(ds_st) == 8 && sizeof(poly_st) == 4 && sizeof(va_st) == 4 && sizeof(ms_st) == 10,
              "state objects must have the record sizes of the ARM64 build (the game sizes command memory by them)");

// ---------- Command records: {op, size (including header, aligned to 8)} + payload ----------
// Record sizes are part of the contract: games size their command heaps and prerecorded-list slots by GetCommandMemoryUsed
// (see cmdbuf.cpp), so a record must not grow. Each p_* below is the payload that follows the 8-byte op_hdr.
struct op_hdr { uint32_t op, size; };
enum {
    OP_END, OP_JUMP, OP_BEGIN, OP_CALL,                                             // OP_BEGIN is no longer recorded (BeginRecording)
    OP_SIGNAL_EVENT, OP_REPORT_COUNTER, OP_COPY_B2B, OP_UPDATE_UB,                 // CPU-visible side effects
    OP_SET_RT, OP_CLEAR_COLOR, OP_CLEAR_DS, OP_VIEWPORTS, OP_SCISSOR, OP_DEPTH_RANGE,
    OP_BIND_PROGRAM, OP_BIND_STATE, OP_BIND_VATTR, OP_BIND_VSTREAM, OP_BIND_VBUF,
    OP_BIND_UBO, OP_BIND_SSBO, OP_BIND_TEX, OP_BIND_SAMP,
    OP_STENCIL, OP_POLY_OFFSET, OP_DRAW, OP_DRAW_IDX, OP_DISPATCH, OP_BARRIER,
    OP_SET_TEX_POOL, OP_SET_SAMP_POOL, OP_COPY_B2T, OP_COPY_STRIDE, OP_BLEND_COLOR, OP_COPY_T2B, OP_BIND_IMG, OP_COPY_T2T, OP_RESET_COUNTER, OP_CLEAR_TEX, OP_CLEAR_BUF, OP_DEPTH_CLAMP, OP_RENDER_COND, OP_FENCE_SYNC, OP_TF_BIND, OP_TF_STATE, OP_RAST_DISCARD, OP_VP_SWIZZLE, OP_BIND_TEX1, OP_PRIM_RESTART, OP_BIND_UBO_SMALL, OP_BIND_VBUF1, OP_DRAW_TEX, OP_ALPHA_REF, OP_PUSH_CONST, OP_DLSS, OP_COUNT_
};
// Payload (immediately follows op_hdr)
// OP_JUMP: {const uint8_t* target (HOST pointer)}; OP_CALL: {uint64_t handle (guest value)}
// OP_SIGNAL_EVENT: {volatile uint32_t* val (HOST pointer), uint32_t mode (NVNeventSignalMode), uint32_t value}
// OP_REPORT_COUNTER: {uint64_t NVNcounterType, uint64_t GPU address of the 16-byte report}; OP_RESET_COUNTER: {uint64_t NVNcounterType}
// OP_COPY_B2B: {src, dst, n (GPU addresses, bytes)}; OP_UPDATE_UB: {dst GPU address, n, then n bytes of data}; OP_FENCE_SYNC: {sync_t* (HOST pointer)}
// OP_BARRIER: {int32 bits}
struct p_set_rt { int32_t n; const tex_t* color[8]; view_t cview[8]; uint8_t has_cview[8]; const tex_t* depth; view_t dview; uint8_t has_dview; };   // texture pointers: snapshots after the record, or the live object (cmdbuf.cpp tex_snap)
struct p_clear_color { int32_t index, mask; float rgba[4]; };
struct p_draw_tex { uint64_t h; NVNdrawTextureRegion d, s; };   // nvnCommandBufferDrawTexture: texture handle, destination (pixels) and source (texels) regions
struct p_render_cond { int32_t mode, pad; uint64_t addr; };   // mode 0 / 1: SetRenderEnableConditional (render if the 64-bit counters at addr and addr + 16 are equal / not equal), 2 / 3: SetRenderEnable(true / false)
struct p_clear_ds { float depth; int32_t depth_mask, stencil, stencil_mask; };
struct p_viewports { int32_t first, n; float v[16][6]; };          // x, y, w, h, near, far (near and far are overridden by DEPTH_RANGE)
struct p_scissor { int32_t x, y, w, h; };
struct p_depth_range { float n, f; };
struct p_prim_restart { uint32_t on, index; };
struct p_depth_clamp { int32_t on; };   // SetDepthClamp: depth outside [near, far] is clamped instead of clipped
struct p_vp_swizzle { int32_t first, n; uint8_t sw[16][4]; };   // SetViewportSwizzles: per viewport the source of clip x, y, z, w (NVNviewportSwizzle) XOR the identity (0, 2, 4, 6), so 0 = unchanged
struct p_bind_program { const prog_t* prog; int32_t stages; };
// kind: 0 blend 1 color 2 chmask 3 ms 4 poly 5 ds. Recorded with the state object's own bytes only: 16 bytes with the header for color, channel mask and polygon state,
// 24 for blend, multisample and depth-stencil, instead of 40 for all. Readers take that kind's size from bytes. Games size display-list heaps by GetCommandMemoryUsed: Super Mario Odyssey's
// "DisplayList" GPU heap (62 MB, a bump allocator) filled while a kingdom loaded with 40-byte state binds (39% of its bytes), and every display list after that got no memory
struct p_bind_state { int32_t kind; uint8_t bytes[0x18]; };
struct p_bind_vattr { int32_t first, n; uint32_t st[16]; };
struct p_bind_vstream { int32_t first, n; vs_st st[16]; };
struct p_bind_vbuf { int32_t first, n; NVNbufferRange r[16]; };
// Games measure scratch recordings to size resource heaps. Keep a single bounded binding at 24 bytes including its header (the native vertex-buffer command is 24 bytes).
// Larger ranges retain the full record; readers expand compact records to the same state, with the full 64-bit GPU address.
struct p_bind_vbuf1 { uint32_t index, size; uint64_t addr; };
inline const p_bind_vbuf* vbuf_rec(uint32_t op, const void* a, p_bind_vbuf* t) {
    if (op != OP_BIND_VBUF1) return static_cast<const p_bind_vbuf*>(a);
    const p_bind_vbuf1* b = static_cast<const p_bind_vbuf1*>(a);
    t->first = (int32_t)b->index; t->n = 1; t->r[0] = NVNbufferRange{b->addr, b->size}; return t;
}
struct p_bind_buf { int32_t stage, index; uint64_t addr, size; };  // UBO / SSBO
// Uniform bindings likewise take 24 bytes instead of 32 (the native inline command takes 20).
struct p_bind_ubo_small { uint16_t stage, index; uint32_t size; uint64_t addr; };
inline const p_bind_buf* bind_buf_rec(uint32_t op, const void* a, p_bind_buf* t) {
    if (op != OP_BIND_UBO_SMALL) return static_cast<const p_bind_buf*>(a);
    const p_bind_ubo_small* b = static_cast<const p_bind_ubo_small*>(a);
    *t = p_bind_buf{b->stage, b->index, b->addr, b->size}; return t;
}
// defer / src: the handles of BindTexturesDeferred are read from src (a HOST pointer into guest memory) when the record is executed (h at recording time is only a fallback);
// the renderer refreshes them with bh_fresh before reading the record
struct p_bind_handles { int32_t stage, first, n, defer; uint64_t src; uint64_t h[32]; }; // Separate texture / sampler handles
// nvnCommandBufferBindTexture (one combined handle) is laid out as the hardware's command: the handle 16 bytes from the start of the record, where an engine that patches its recorded commands
// in place writes a new one. Luigi's Mansion 3 takes the command memory base + GetCommandMemoryUsed before each such bind and stores a handle there + 0x10 every frame;
// in a p_bind_handles record that word is the count. h is the handle as the game gives it (32 bits: texture ID | sampler ID << 20, all a patch writes); tbase / sbase are the numbering bases of the
// descriptor pools the command buffer had bound (cmdbuf.cpp), added when it is read.
// With both bases 0 (the first pools, nearly always) the record ends after h, 24 bytes with the header, and stage carries BIND_TEX1_NOBASE.
// Read it with bind_rec, which gives a p_bind_handles
struct p_bind_tex1 { int32_t stage, slot; uint64_t h; int32_t tbase, sbase; };
#define BIND_TEX1_NOBASE 0x100
inline const p_bind_handles* bind_rec(uint32_t op, const void* a, p_bind_handles* t) {
    if (op != OP_BIND_TEX1) return static_cast<const p_bind_handles*>(a);
    const p_bind_tex1* b = static_cast<const p_bind_tex1*>(a); uint64_t tid = b->h & 0xfffff, sid = (uint32_t)b->h >> 20;
    int nobase = b->stage & BIND_TEX1_NOBASE; int32_t tb = nobase ? 0 : b->tbase, sb = nobase ? 0 : b->sbase;
    t->stage = b->stage & ~BIND_TEX1_NOBASE; t->first = b->slot; t->n = 1; t->defer = 0; t->src = 0;
    t->h[0] = (tid ? tid + (uint64_t)tb : 0) | (sid ? sid + (uint64_t)sb : 0) << 20; return t;
}
// Texture slots of a stage: 0-127 nvnCommandBufferBindTexture's (combined handles), 128-255 the separate texture binds. The driver keeps the two in tables of their own,
// which the shaders' reflection names as these slots, so a game may bind both kinds in the same frame
#define NVN_SEPTEX_SLOT0 128
#define NVN_PUSH_SIZE 0x100   // bytes of a push-constant block (graphics and compute each)
struct p_stencil { int32_t face, value_mask, mask, ref, which; };  // which: 0 value mask 1 write mask 2 reference value
struct p_poly_offset { float factor, units, clamp; };
#define NVN_PRIM_MESH (1 << 16)   // p_draw.prim of an NVN2 mesh draw (nvnCommandBufferDrawMeshTasks): first and count are task / mesh workgroups, no vertex stage
struct p_draw { int32_t prim, first, count, instances, base_instance; uint64_t ind; uint64_t mdi_cnt; int32_t mdi_i, pad; };   // instances 0 counts as 1; ind != 0: DrawArraysIndirect, the arguments are read by the GPU from that GPU address; mdi_cnt != 0: draw mdi_i of an indirect-count multi-draw (drawn only while mdi_i < the u32 at mdi_cnt)
struct p_draw_idx { int32_t prim, itype, count, base_vertex, base_instance, instances; uint64_t ibuf, ind; uint64_t mdi_cnt; int32_t mdi_i, pad; };   // ind != 0: DrawElementsIndirect, {count, instances, first index, base vertex, base instance} read by the GPU from that address; mdi_cnt: as in p_draw
// A draw record stops before ind unless it is an indirect draw or a multi-draw (cmdbuf.cpp rec_draw); read it with draw_rec / draw_idx_rec, which give the missing fields as 0.
// Games size command memory by what nvnCommandBufferGetCommandMemoryUsed reports, so a plain draw must not take more of it than the real driver's commands do. Breath of the Wild
// records each mesh's draw into a scratch command buffer and reserves that many bytes per mesh for the model's prerecorded draws: at 0x50 bytes per recorded draw three models needed one 4 KB page more than their table entry
inline const p_draw* draw_rec(const void* a, uint32_t size, p_draw* t) { if (size - 8 >= sizeof *t) return static_cast<const p_draw*>(a); memset(t, 0, sizeof *t); memcpy(t, a, size - 8); return t; }
inline const p_draw_idx* draw_idx_rec(const void* a, uint32_t size, p_draw_idx* t) { if (size - 8 >= sizeof *t) return static_cast<const p_draw_idx*>(a); memset(t, 0, sizeof *t); memcpy(t, a, size - 8); return t; }
struct p_dispatch { int32_t x, y, z; uint64_t caller, ind; };   // caller: guest return address of the call site (to locate the dispatcher); ind != 0: DispatchComputeIndirect, the group counts are read by the GPU from that GPU address
struct p_set_pool { const dpool_t* pool; };
struct p_copy_b2t { uint64_t src; const tex_t* tex; view_t view; uint8_t has_view; NVNcopyRegion r; int32_t flags; };   // src: GPU address (the destination for OP_COPY_T2B)
// A host-only bit in flags: NVN_COPY_RESOLVE = this is a Downsample (multisample resolve), recorded as a texture copy so everything else treats it as one (reads the source, writes the destination)
#define NVN_COPY_RESOLVE 0x40000000
struct p_copy_t2t { const tex_t* src, *dst; view_t sv, dv; uint8_t has_sv, has_dv; NVNcopyRegion sr, dr; int32_t flags; };
struct p_clear_tex { const tex_t* tex; view_t view; uint8_t has_view, is_uint; NVNcopyRegion r; int32_t mask; union { float f[4]; uint32_t u[4]; } c; };
struct p_copy_stride { int64_t stride; int image; };   // image: SetCopyImageStride (bytes per layer / depth slice) instead of SetCopyRowStride
// NVN2's DLSS evaluation: the parameters at the time of the call, each texture snapped after the record
enum { DLSS_COLOR, DLSS_DEPTH, DLSS_MOTION, DLSS_OUTPUT, DLSS_BIAS, DLSS_EXPOSURE, DLSS_TEXTURES };
struct p_dlss { const tex_t* tex[DLSS_TEXTURES]; uint64_t net; int32_t iw, ih, ow, oh, base[DLSS_TEXTURES][2]; float mv[2], jitter[2], pre_exposure; uint32_t flags, reset; };
struct p_clear_buf { uint64_t addr, size; uint32_t value, _pad; };   // buffer (GPU address) filled with a 32-bit value (ClearBuffer)
struct p_blend_color { float rgba[4]; };
struct p_alpha_ref { float ref; };   // SetAlphaRef: the reference of the alpha test (ColorStateSetAlphaTest)
#pragma warning(push)
#pragma warning(disable : 4200)   // zero-length array: the record's trailing payload
struct p_push_const { uint16_t compute, offset; uint32_t size; uint8_t data[]; };
#pragma warning(pop)
// ^ NVN2 push constants: the bytes at offset in the graphics or compute block (NVN_PUSH_SIZE)
struct p_tf_bind { int32_t index, pad; uint64_t addr, size; };   // BindTransformFeedbackBuffer(s): one record per buffer (index 0-3)
struct p_tf_state { int32_t what, pad; uint64_t ctl; };          // what: 0 begin, 1 end, 2 pause, 3 resume; ctl = the game's 32-byte control buffer (GPU address)
struct p_rast_discard { int32_t on; };

// The effect of a SignalEvent / ReportValue on the event word: WRITE (NVN_EVENT_SIGNAL_MODE_WRITE = 0) stores v, ADD (1) adds v. The game polls the word from other threads.
inline void event_apply(volatile uint32_t* val, uint32_t mode, uint32_t v) {
    std::atomic_ref<uint32_t> a(*const_cast<uint32_t*>(val));
    if (mode == NVN_EVENT_SIGNAL_MODE_ADD) a.fetch_add(v, std::memory_order_acq_rel); else a.store(v, std::memory_order_release);
}

// New serial (> 0, globally increasing): textures, programs and queues get one at initialization so the renderer can key its tables by it
uint64_t new_serial();

}  // namespace nvn
