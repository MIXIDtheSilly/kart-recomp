// Native NVN: command buffers. Commands are recorded into the command memory the game gave (AddCommandMemory) as records (nvn_obj.h) and are executed by the renderer at
// submit (replay.h rp_exec). Nothing here touches the GPU. The record sizes and the "used" accounting are observable by the game and must not change:
// games size their command heaps, prerecorded-list slots and ring allocators by nvnCommandBufferGetCommandMemoryUsed (see the notes at each piece).
#include "nvn_int.h"

namespace nvn {

#define CB(c) ((cmd_t*)(c))

// Room kept at the segment end after every record but the END: one JUMP. The END needs none (nothing follows it), and every other record is at least 16 bytes, so a JUMP
// only ever stands where a record was to go: linking never writes past the bytes the recording takes in one piece, which is what GetCommandMemoryUsed reports and games size
// command memory by. Tears of the Kingdom (nxnative issue #61) gives each model's prerecorded lists fixed slots of the size a recording of that list measured, slot k at
// base + size x k, and passes AddCommandMemory max(size, 512). With 24 bytes kept for "a JUMP or an END", a 496-byte list's END did not fit in 512,
// went to a host segment, and the 16-byte JUMP at 488 reached 8 bytes into the next slot; recording that slot wrote its first record over the JUMP's target, and the
// replay jumped to 0x1000000003 (that CALL record's header) after 8 to 30 minutes of play
#define OP_RESERVE 16

NVN(nvnCommandBufferInitialize, NVNboolean, NVNcommandBuffer* c, NVNdevice* d) {
    memset(c, 0, sizeof(cmd_t)); CB(c)->magic = NVN_MAGIC('C', 'M', 'D', 'B'); CB(c)->dev = d; return 1;
}
NVN(nvnCommandBufferFinalize, void, NVNcommandBuffer* c) { CB(c)->magic = 0; }
NVN(nvnCommandBufferSetMemoryCallback, void, NVNcommandBuffer* c, PFNNVNCOMMANDBUFFERMEMORYCALLBACKPROC cb) { CB(c)->cb = (uint64_t)(uintptr_t)cb; }   // a guest function address
NVN(nvnCommandBufferSetMemoryCallbackData, void, NVNcommandBuffer* c, void* d) { CB(c)->cb_data = ga(d); }                                          // handed back unchanged
static uint8_t* align8(uint8_t* p) { return (uint8_t*)(((uintptr_t)p + 7) & ~(uintptr_t)7); }
static void put_hdr(uint8_t* p, uint32_t op, uint32_t size) { op_hdr h = {op, size}; memcpy(p, &h, sizeof h); }

// New segment: if the recording in progress has records, write a JUMP at the end of the old segment to link it. A recording with none yet starts in the new segment
// (its first record is its handle), and the bytes at the old write position are not its own: after an END that took the last bytes there is no room there at all
static void cmd_new_segment(cmd_t* o, uint8_t* mem, size_t n) {
    uint8_t *start = align8(mem), *end = mem + n;
    if (o->recording == 1 && o->cur) {
        put_hdr(o->cur, OP_JUMP, 16); memcpy(o->cur + 8, &start, 8);
        o->used += 16;   // the JUMP also occupies guest memory of this recording: count it in used, so the game packing the next recording right behind by used does not overwrite it
    }
    o->cur = start; o->end = end;
}

// Host-supplied segment when the game's own command memory runs out: when the game's out-of-memory callback cannot allocate from its command memory pool, it calls AddCommandMemory with an empty pool (p = NULL, n = 0)
// (the original implementation segfaulted reading NULL+8). The records are larger than real-device NVN commands, so the budget the game reserved for real-device usage is not necessarily enough,
// so this is not an error state of the game but something we must catch: provide a segment of host memory to keep recording. The segment is replayed in place at submit.
// The segments are mapped in guest memory (HsRegion), so a command handle that starts in one is still a real guest address (the game may patch recorded commands in place, nvn_obj.h OP_BIND_TEX1).
// Reclaim follows the game's command memory chunks, not a frame count: the game caches recorded command handles and CALLs them every frame until it re-records them itself (SMBW's object setup handles);
// once a handle was recorded into a host segment, reclaiming "8 frames after lending" rewrote the segment while the game still CALLed it.
// A segment hangs off one recording through the JUMP at the end of its guest part: that recording occupies [BEGIN, end of JUMP) in guest memory, and the game only gives those bytes out again once they are dead
// (when it re-records that material in place; packing the next recording right behind by used never touches them, since used includes the JUMP). So a segment remembers that guest range and is reclaimed when the game gives out memory
// overlapping it; segments overflowing from a host segment keep the same range.
// The segment pool grows on demand (SMBW opening course: many small command buffers each overflow once per frame); abort only beyond HOSTSEG_CAP in total.
// A segment is HOSTSEG_SZ, with two exceptions (hostseg_size): an empty pool with a length of at most HOSTSEG_SMALL outside a recording gets that much rounded up to HOSTSEG_SMALL
// (Super Mario Odyssey hands out many such 512-byte grants before recording), and a record larger than HOSTSEG_SZ gets a segment that holds it. A free segment is taken again only for a request of its own size; the cap counts every segment's bytes.
// An empty-pool grant outside a recording means the game's own allocator for command memory came back empty. On hardware a null pool cannot be written to, so a game only gets here
// when the host's records outgrew what it measured for them. Those segments have no guest range to be reclaimed by, so they are not reclaimed; the cap's abort names the state, and the fix belongs in the record sizes
namespace {
constexpr uint32_t HOSTSEG_SZ = 256u << 10, HOSTSEG_SMALL = 4u << 10;
constexpr uint64_t HOSTSEG_CAP = 1ull << 30;
constexpr u64 HsRegionBase = 0x7000000000ull, HsRegionSize = 0x400000000ull;   // guest addresses the segments are mapped at
struct hostseg_t { uint8_t* mem; size_t n; uint64_t frame; bool busy; uintptr_t org_lo, org_hi; };   // org: the guest range of the recording it hangs off (0 = unknown, never reclaimed)
std::vector<hostseg_t> g_hostseg;
uint64_t g_hostseg_bytes;
std::mutex g_hostseg_mu;

size_t hostseg_size(const cmd_t* o, bool empty_pool, size_t want) {
    if (empty_pool) return want && want <= HOSTSEG_SMALL && !o->recording ? HOSTSEG_SMALL : HOSTSEG_SZ;
    return want + 16 > HOSTSEG_SZ ? (want + 16 + 0xfff) & ~(size_t)0xfff : HOSTSEG_SZ;   // 16: room for the next JUMP (want holds OP_RESERVE already, this is slack for the alignment)
}
// Take a segment of n bytes: origin = the current recording's guest bytes [BEGIN, end of JUMP); if the write pointer is already in a host segment (overflowing again), keep that segment's origin
uint8_t* hostseg_take(const cmd_t* o, size_t n) {
    static unsigned total; const uint64_t f = g_frame.load(); uint8_t* m = nullptr;
    uintptr_t lo = 0, hi = 0; const uintptr_t cur = o ? (uintptr_t)o->cur : 0;
    std::unique_lock lk{g_hostseg_mu};
    bool in_seg = false;   // by the segment's end too: a small segment can end where another one starts
    for (const hostseg_t& s : g_hostseg) if (cur && cur >= (uintptr_t)s.mem && o->end == s.mem + s.n) { lo = s.org_lo; hi = s.org_hi; in_seg = true; break; }
    if (!in_seg && o && o->recording && o->cur) { lo = o->recording == 1 && o->begin && (uintptr_t)o->begin <= cur ? (uintptr_t)o->begin : cur; hi = cur + 16; }
    for (hostseg_t& s : g_hostseg) if (!s.busy && s.n == n) { s = hostseg_t{s.mem, n, f, true, lo, hi}; m = s.mem; break; }
    if (!m && g_hostseg_bytes + n <= HOSTSEG_CAP) {
        const u64 len = (n + 0xfff) & ~(size_t)0xfff;
        const u64 addr = mem::find_free(HsRegionBase, HsRegionSize, len, 0x1000);
        if (addr) { mem::map(addr, len, mem::Normal, mem::RW); m = static_cast<uint8_t*>(hp(addr)); g_hostseg.push_back(hostseg_t{m, n, f, true, lo, hi}); g_hostseg_bytes += n; }
    }
    const unsigned t = ++total; const size_t ns = g_hostseg.size(); const uint64_t bytes = g_hostseg_bytes;
    lk.unlock();
    if (!m) fatal("nvn: command memory: the game's pool is empty and host-lent segments reached %u MB without being reclaimed by the game's blocks", (unsigned)(HOSTSEG_CAP >> 20));
    if (t == 1 || t % 1024 == 0) LOG("nvn: the game's command memory ran short, the host lends a segment of %zu KB (%u times so far, frame %llu, segment pool %zu segments, %llu KB)", n >> 10, t, (unsigned long long)f, ns, (unsigned long long)(bytes >> 10));
    return m;
}
// The game gives out [lo, hi): segments whose recording's guest bytes overlap it are dead, reclaim them
void hostseg_release(uintptr_t lo, uintptr_t hi) {
    std::scoped_lock lk{g_hostseg_mu};
    for (hostseg_t& s : g_hostseg) if (s.busy && s.org_hi > lo && s.org_lo < hi) s.busy = false;
}
}  // namespace

NVN(nvnCommandBufferAddCommandMemory, void, NVNcommandBuffer* c, const NVNmemoryPool* p, ptrdiff_t off, size_t n) {
    const pool_t* pl = (const pool_t*)p;
    if (!pl || n < 64) { const size_t sz = hostseg_size(CB(c), !pl, n); cmd_new_segment(CB(c), hostseg_take(CB(c), sz), sz); return; }
    if (!pl->mem) { LOG("[RED] nvn: AddCommandMemory: pool has no CPU storage"); return; }
    hostseg_release((uintptr_t)(pl->mem + off), (uintptr_t)(pl->mem + off + n));
    cmd_new_segment(CB(c), pl->mem + off, n);
    CB(c)->used = 0;   // GetCommandMemoryUsed counts from the start of the memory the game last added, across recordings (see there)
}
NVN(nvnCommandBufferAddControlMemory, void, NVNcommandBuffer* c, void* m, size_t n) {
    (void)m;
    CB(c)->ctl_size = n;   // The host's recordings contain no control-memory entries; the entire last block remains free.
}
// Get space for one record and keep `keep` bytes after it (OP_RESERVE, 0 for the END); if not enough, call back into the game per the NVN convention to add command memory
static uint8_t* cmd_alloc_keep(cmd_t* o, uint32_t size, uint32_t keep) {
    size = (size + 7) & ~7u;
    for (int tries = 0; !o->cur || o->cur + size + keep > o->end; tries++) {
        // The game's callback cannot supply enough (newer-SDK games hand out small chunks sized for real-device commands, or disable the callback and manage memory themselves): continue recording in a host segment instead of aborting
        if (!o->cb || tries == 2) {
            ONCE_LOG("command memory: the game's callback did not add enough (cb=%#llx, need %u bytes), using a host-lent segment", (unsigned long long)o->cb, size + keep);
            const size_t sz = hostseg_size(o, false, size + keep); cmd_new_segment(o, hostseg_take(o, sz), sz); break;
        }
        // NVNcommandBufferMemoryCallback(cmdBuf, event, minSize, callbackData): the game answers by calling AddCommandMemory on this command buffer (re-entering this layer)
        call_guest(o->cb, {ga(o), (u64)NVN_COMMAND_BUFFER_MEMORY_EVENT_OUT_OF_COMMAND_MEMORY, (u64)(size + keep), o->cb_data});
    }
    uint8_t* p = o->cur; o->cur += size; o->used += size; put_hdr(p, 0, size);
    if (o->recording == 2) { o->begin = p; o->recording = 1; }   // The recording's first record is its handle
    return p;
}
static uint8_t* cmd_alloc(cmd_t* o, uint32_t size) { return cmd_alloc_keep(o, size, OP_RESERVE); }
// A recording starts with its first command, as in NVN: no BEGIN record, so an empty recording takes only its END. Games size command memory by GetCommandMemoryUsed
// (nvn_obj.h draw_rec): Breath of the Wild sums one scratch recording per mesh and per shape and render pass into each model's prerecorded commands, and with an
// 8-byte BEGIN on each of several hundred recordings a model came out a 4 KB page over its resource size table entry
NVN(nvnCommandBufferBeginRecording, void, NVNcommandBuffer* c) {
    cmd_t* o = CB(c);
    if (o->recording) LOG("[RED] nvn: BeginRecording re-entered");
    o->recording = 2;   // 2: recording, no command yet (cmd_alloc sets begin)
}
NVN(nvnCommandBufferEndRecording, NVNcommandHandle, NVNcommandBuffer* c) {
    cmd_t* o = CB(c);
    uint8_t* p = cmd_alloc_keep(o, 8, 0); put_hdr(p, OP_END, 8);   // nothing follows: the END may take the segment's last bytes
    o->recording = 0;
    return (NVNcommandHandle)ga(o->begin);
}
NVN(nvnCommandBufferIsRecording, NVNboolean, const NVNcommandBuffer* c) { return ((const cmd_t*)c)->recording != 0; }
NVN(nvnCommandBufferCopyCommands, void, NVNcommandBuffer* c, int n, const NVNcommandHandle* h) {
    for (int i = 0; i < n; i++) { uint8_t* p = cmd_alloc(CB(c), 16); put_hdr(p, OP_CALL, 16); memcpy(p + 8, &h[i], 8); }
}
NVN(nvnCommandBufferSignalEvent, void, NVNcommandBuffer* c, const NVNevent* e, NVNeventSignalMode m, NVNeventSignalLocation loc, int flags, uint32_t v) {
    (void)loc; (void)flags;
    uint8_t* p = cmd_alloc(CB(c), 24); put_hdr(p, OP_SIGNAL_EVENT, 24);
    const ev_t* ev = (const ev_t*)e; volatile uint32_t* val = ev->val; uint32_t mm = m;
    memcpy(p + 8, &val, 8); memcpy(p + 16, &mm, 4); memcpy(p + 20, &v, 4);
}
// A fence recorded in a command buffer: the renderer fences the sync with the submission that reaches this point (OP_FENCE_SYNC). Unreal Engine's NVN RHI fences its command buffers this way (Octopath Traveler 0); Pokemon Legends: Z-A fences once a frame this way and waits on it
NVN(nvnCommandBufferFenceSync, void, NVNcommandBuffer* c, NVNsync* s, NVNsyncCondition cond, int flags) {
    (void)cond; (void)flags;
    uint8_t* p = cmd_alloc(CB(c), 16); put_hdr(p, OP_FENCE_SYNC, 16); memcpy(p + 8, &s, 8);
}

NVN(nvnCommandBufferResetCounter, void, NVNcommandBuffer *c, NVNcounterType t) {
    uint8_t *p = cmd_alloc(CB(c), 16); put_hdr(p, OP_RESET_COUNTER, 16); uint64_t tt = t; memcpy(p + 8, &tt, 8);
}
NVN(nvnCommandBufferReportCounter, void, NVNcommandBuffer *c, NVNcounterType t, NVNbufferAddress a) {
    uint8_t *p = cmd_alloc(CB(c), 24); put_hdr(p, OP_REPORT_COUNTER, 24); uint64_t tt = t; memcpy(p + 8, &tt, 8); memcpy(p + 16, &a, 8);
}
NVN(nvnCommandBufferCopyBufferToBuffer, void, NVNcommandBuffer *c, NVNbufferAddress src, NVNbufferAddress dst, size_t n, int flags) {
    (void)flags;
    uint8_t *p = cmd_alloc(CB(c), 32); put_hdr(p, OP_COPY_B2B, 32); uint64_t nn = n;
    memcpy(p + 8, &src, 8); memcpy(p + 16, &dst, 8); memcpy(p + 24, &nn, 8);
}
// uniform update: the data is copied into command memory at record time (NVN semantics: the caller may modify the original data after recording) and written into the buffer at submit
NVN(nvnCommandBufferUpdateUniformBuffer, void, NVNcommandBuffer *c, NVNbufferAddress buf, size_t bufsz, ptrdiff_t off, size_t n, const void *data) {
    (void)bufsz;
    uint32_t sz = (uint32_t)(24 + n);
    uint8_t *p = cmd_alloc(CB(c), sz); put_hdr(p, OP_UPDATE_UB, (sz + 7) & ~7u);
    uint64_t dst = buf + off, nn = n; memcpy(p + 8, &dst, 8); memcpy(p + 16, &nn, 8); memcpy(p + 24, data, n);
}
// ---------- Render commands: parameters are recorded into command memory in the payload format of nvn_obj.h ----------
void* rec(NVNcommandBuffer* c, uint32_t op, size_t payload) {
    const uint32_t sz = (uint32_t)((8 + payload + 7) & ~(size_t)7);
    uint8_t* p = cmd_alloc(CB(c), sz); put_hdr(p, op, sz);
    memset(p + 8, 0, sz - 8);
    return p + 8;
}
void copy_view(view_t* dst, uint8_t* has, const NVNtextureView* v) { *has = v != nullptr; if (v) *dst = *(const view_t*)v; }
// Records with textures snapshot the texture objects (0x80 bytes each, after the record struct) and the renderer reads the snapshots: an NVN command no longer depends on the API object once recorded, and the game may finalize, free or reuse it before submitting.
// Unity (Engage) does so when it changes scenes; only the object pointer used to be recorded, and replay read other data.
const tex_t* tex_snap(void* rec_, size_t off, int k, const NVNtexture* t, uint64_t ra) {
    if (!t) return nullptr;
    tex_t* s = (tex_t*)((uint8_t*)rec_ + off) + k;
    memcpy(s, t, sizeof *s);
    // The object is invalid at record time (Unity finalizes and re-initializes the same render target object between recording and submitting): keep the object pointer as before and read it at submit, when it is usually valid again; the renderer still checks the magic
    if (s->magic != NVN_MAGIC('T', 'E', 'X', 0)) {
        static int red;
        if (red++ < 4) {   // the first 4 log the call site and the start of the object, to tell what the game passed
            const uint64_t* q = (const uint64_t*)t;
            LOG("[RED] nvn: texture object invalid while recording (uninitialized or already finalized): %p index %d, call site %s, object start %llx %llx %llx %llx %llx %llx; read again at submit", (const void*)t, k,
                describe_guest(ra).c_str(), (unsigned long long)q[0], (unsigned long long)q[1], (unsigned long long)q[2], (unsigned long long)q[3], (unsigned long long)q[4], (unsigned long long)q[5]);
        }
        return (const tex_t*)t;
    }
    return s;
}
static void rec_draw(NVNcommandBuffer *c, p_draw d) { size_t n = d.ind || d.mdi_cnt ? sizeof d : offsetof(p_draw, ind); memcpy(rec(c, OP_DRAW, n), &d, n); }
static void rec_draw_idx(NVNcommandBuffer *c, p_draw_idx d) { size_t n = d.ind || d.mdi_cnt ? sizeof d : offsetof(p_draw_idx, ind); memcpy(rec(c, OP_DRAW_IDX, n), &d, n); }

// ---------- Render commands: parameters are recorded into command memory in the payload format of nvn_obj.h ----------
// ct / cv are host pointers to arrays of GUEST pointers (the game's arrays), dt / dv are converted by the call adapter
NVN(nvnCommandBufferSetRenderTargets, void, NVNcommandBuffer *c, int n, const NVNtexture *const *ct, const NVNtextureView *const *cv,
    const NVNtexture *dt, const NVNtextureView *dv) {
    if (n < 0 || n > 8) { LOG("[RED] nvn: SetRenderTargets count %d", n); n = n < 0 ? 0 : 8; }
    const u64 *ctg = reinterpret_cast<const u64 *>(ct), *cvg = reinterpret_cast<const u64 *>(cv);
    const uint64_t ra = call_site();
    p_set_rt *r = REC_SNAP(c, OP_SET_RT, p_set_rt, n + 1); r->n = n;
    for (int i = 0; i < n; i++) { r->color[i] = ctg ? tex_snap(r, SNAP_OFF(p_set_rt), i, gp<NVNtexture>(ctg[i]), ra) : nullptr; copy_view(&r->cview[i], &r->has_cview[i], cvg ? gp<NVNtextureView>(cvg[i]) : nullptr); }
    r->depth = tex_snap(r, SNAP_OFF(p_set_rt), n, dt, ra); copy_view(&r->dview, &r->has_dview, dv);
}
NVN(nvnCommandBufferClearColor, void, NVNcommandBuffer *c, int index, const float *rgba, int mask) {
    p_clear_color *r = REC(c, OP_CLEAR_COLOR, p_clear_color); r->index = index; r->mask = mask; memcpy(r->rgba, rgba, 16);
}
// Clears of integer targets (Paper Mario: The Thousand-Year Door): recorded as floats, which the renderer converts to the attachment's integer type (the load action's clear colour,
// clear_color_tri); exact up to 2^24 in magnitude, and 0xffffffff rounds to 2^32, which converts back to it
NVN(nvnCommandBufferClearColori, void, NVNcommandBuffer *c, int index, const int *rgba, int mask) {
    p_clear_color *r = REC(c, OP_CLEAR_COLOR, p_clear_color); r->index = index; r->mask = mask; for (int i = 0; i < 4; i++) r->rgba[i] = (float)rgba[i];
}
NVN(nvnCommandBufferClearColorui, void, NVNcommandBuffer *c, int index, const uint32_t *rgba, int mask) {
    p_clear_color *r = REC(c, OP_CLEAR_COLOR, p_clear_color); r->index = index; r->mask = mask; for (int i = 0; i < 4; i++) r->rgba[i] = (float)rgba[i];
}
NVN(nvnCommandBufferClearDepthStencil, void, NVNcommandBuffer *c, float depth, uint32_t dmask, int stencil, int smask) {
    p_clear_ds *r = REC(c, OP_CLEAR_DS, p_clear_ds); *r = p_clear_ds{depth, (int)B(dmask), stencil, smask};
}
NVN(nvnCommandBufferSetViewports, void, NVNcommandBuffer *c, int first, int n, const float *v) {
    if (n < 0 || first + n > 16) { LOG("[RED] nvn: SetViewports first=%d n=%d", first, n); return; }
    p_viewports *r = REC(c, OP_VIEWPORTS, p_viewports); r->first = first; r->n = n;
    for (int i = 0; i < n; i++) memcpy(r->v[i], v + 4 * i, 16);
}
NVN(nvnCommandBufferSetScissor, void, NVNcommandBuffer *c, int x, int y, int w, int h) { *REC(c, OP_SCISSOR, p_scissor) = p_scissor{x, y, w, h}; }
NVN(nvnCommandBufferSetDepthRange, void, NVNcommandBuffer *c, float n, float f) { *REC(c, OP_DEPTH_RANGE, p_depth_range) = p_depth_range{n, f}; }
// NVN2's task (6) and mesh (7) stages take the slots of tessellation control (3) and evaluation (4), which no game here uses: the replay keeps six stages a binding table
// and the shader stage table, and a draw is either a mesh draw (task, mesh, fragment) or a vertex one, never both. Bindings and program stage bits move here
static int st_slot(int st) {
    if (st == 6 || st == 7) { static int once; if (!once++) LOG("nvn: first task / mesh stage binding (stage %d): kept in the tessellation slots", st); return st - 3; }
    return st;
}
NVN(nvnCommandBufferBindProgram, void, NVNcommandBuffer *c, const NVNprogram *p, int stages) {
    if (stages & 0xc0) stages = (stages & 0x3f & ~0x18) | (stages & 0x40 ? 0x08 : 0) | (stages & 0x80 ? 0x10 : 0);   // task / mesh bits (0x40 / 0x80) to the slots' bits
    *REC(c, OP_BIND_PROGRAM, p_bind_program) = p_bind_program{(const prog_t *)p, stages};
}
static void bind_state(NVNcommandBuffer *c, int kind, const void *st, size_t n) { p_bind_state *r = (p_bind_state *)rec(c, OP_BIND_STATE, offsetof(p_bind_state, bytes) + n); r->kind = kind; memcpy(r->bytes, st, n); }   // only the state's own bytes (nvn_obj.h p_bind_state)
NVN(nvnCommandBufferBindBlendState, void, NVNcommandBuffer *c, const NVNblendState *st) { bind_state(c, 0, st, sizeof(blend_st)); }
NVN(nvnCommandBufferBindColorState, void, NVNcommandBuffer *c, const NVNcolorState *st) { bind_state(c, 1, st, sizeof(color_st)); }
NVN(nvnCommandBufferBindChannelMaskState, void, NVNcommandBuffer *c, const NVNchannelMaskState *st) { bind_state(c, 2, st, sizeof(chmask_st)); }
NVN(nvnCommandBufferBindMultisampleState, void, NVNcommandBuffer *c, const NVNmultisampleState *st) { bind_state(c, 3, st, sizeof(ms_st)); }
NVN(nvnCommandBufferBindPolygonState, void, NVNcommandBuffer *c, const NVNpolygonState *st) { bind_state(c, 4, st, sizeof(poly_st)); }
NVN(nvnCommandBufferBindDepthStencilState, void, NVNcommandBuffer *c, const NVNdepthStencilState *st) { bind_state(c, 5, st, sizeof(ds_st)); }
NVN(nvnCommandBufferBindVertexAttribState, void, NVNcommandBuffer *c, int n, const NVNvertexAttribState *st) {
    if (n < 0 || n > 16) { LOG("[RED] nvn: BindVertexAttribState n=%d", n); return; }
    p_bind_vattr *r = RECN(c, OP_BIND_VATTR, p_bind_vattr, st, n); r->first = 0; r->n = n; memcpy(r->st, st, 4 * n);
}
NVN(nvnCommandBufferBindVertexStreamState, void, NVNcommandBuffer *c, int n, const NVNvertexStreamState *st) {
    if (n < 0 || n > 16) { LOG("[RED] nvn: BindVertexStreamState n=%d", n); return; }
    p_bind_vstream *r = RECN(c, OP_BIND_VSTREAM, p_bind_vstream, st, n); r->first = 0; r->n = n; memcpy(r->st, st, 8 * n);
}
NVN(nvnCommandBufferBindVertexBuffers, void, NVNcommandBuffer *c, int first, int n, const NVNbufferRange *rg) {
    if (n < 0 || first + n > 16) { LOG("[RED] nvn: BindVertexBuffers first=%d n=%d", first, n); return; }
    if (n == 1 && (unsigned)first < 16 && rg[0].size <= UINT32_MAX) {
        *REC(c, OP_BIND_VBUF1, p_bind_vbuf1) = p_bind_vbuf1{(uint32_t)first, (uint32_t)rg[0].size, rg[0].address}; return;
    }
    p_bind_vbuf *r = RECN(c, OP_BIND_VBUF, p_bind_vbuf, r, n); r->first = first; r->n = n; memcpy(r->r, rg, sizeof *rg * n);
}
NVN(nvnCommandBufferBindUniformBuffer, void, NVNcommandBuffer *c, NVNshaderStage st, int i, NVNbufferAddress a, size_t n) {
    st = (NVNshaderStage)st_slot(st);
    if ((unsigned)st < 6 && (unsigned)i < 16 && n <= UINT32_MAX) {
        *REC(c, OP_BIND_UBO_SMALL, p_bind_ubo_small) = p_bind_ubo_small{(uint16_t)st, (uint16_t)i, (uint32_t)n, a}; return;
    }
    *REC(c, OP_BIND_UBO, p_bind_buf) = p_bind_buf{st, i, a, n};
}
NVN(nvnCommandBufferBindUniformBuffers, void, NVNcommandBuffer *c, NVNshaderStage st, int first, int n, const NVNbufferRange *rg) {
    for (int i = 0; i < n; i++) h_nvnCommandBufferBindUniformBuffer(c, st, first + i, rg[i].address, rg[i].size);
}
static void ssbo_slot_check(int st, int i) {   // the replay keeps 16 storage buffer slots a stage (NVN's limit); a higher one is not bound
    if (i >= 16) { static int n; if (n++ < 8) LOG("[RED] nvn: storage buffer bound at slot %d of stage %d: the host keeps 16 a stage, the bind is dropped", i, st); }
}
NVN(nvnCommandBufferBindStorageBuffer, void, NVNcommandBuffer *c, NVNshaderStage st, int i, NVNbufferAddress a, size_t n) {
    st = (NVNshaderStage)st_slot(st);
    ssbo_slot_check(st, i);
    *REC(c, OP_BIND_SSBO, p_bind_buf) = p_bind_buf{st, i, a, n};
}
NVN(nvnCommandBufferBindStorageBuffers, void, NVNcommandBuffer *c, NVNshaderStage st, int first, int n, const NVNbufferRange *rg) {
    st = (NVNshaderStage)st_slot(st);
    for (int i = 0; i < n; i++) { ssbo_slot_check(st, first + i); *REC(c, OP_BIND_SSBO, p_bind_buf) = p_bind_buf{st, first + i, rg[i].address, rg[i].size}; }
}
static p_bind_handles *bind_handles(NVNcommandBuffer *c, int op, int st, int first, int n, const uint64_t *h) {
    st = (NVNshaderStage)st_slot(st);
    if (n < 0 || n > 32) { LOG("[RED] nvn: bind handle count %d", n); return NULL; }
    p_bind_handles *r = RECN(c, op, p_bind_handles, h, n); r->stage = st; r->first = first; r->n = n; r->defer = 0; r->src = 0;
    // The driver writes each handle into the c2 table as a 32-bit word (texture ID in bits 0-19, sampler ID in bits 20-31), so the GPU never sees the upper half of the 64-bit handle.
    // EA SPORTS FC 27 binds separate textures with bit 32 set (0x1000001d1): read whole, its sampler ID was 4096, beyond any pool, where the GPU reads sampler 0
    for (int i = 0; i < n; i++) r->h[i] = (uint32_t)h[i];
    {   // this command buffer may have bound a pool other than the first: move its IDs to that pool's part of the table (handle: texture ID in bits 0-19, sampler ID from bit 20)
        int tb = CB(c)->tex_base, sb = CB(c)->samp_base;
        for (int i = 0; i < n && (tb || sb); i++) {
            uint64_t v = r->h[i], tid = v & 0xfffff, sid = v >> 20;
            if (op == OP_BIND_SAMP) { if (sid) r->h[i] = (sid + (uint64_t)sb) << 20; }
            else { if (tid) tid += (uint64_t)tb; if (sid && op == OP_BIND_TEX) sid += (uint64_t)sb; r->h[i] = tid | sid << 20; }
        }
    }
    return r;
}
// Image (writable texture): handle = texture pool index (GetImageHandle returns it as-is; RegisterImage writes the same table as RegisterTexture)
NVN(nvnCommandBufferBindImages, void, NVNcommandBuffer *c, NVNshaderStage st, int first, int n, const NVNimageHandle *h) {
    bind_handles(c, OP_BIND_IMG, st, first, n, (const uint64_t *)h);
}
// Separate texture binds: on hardware they fill their own handle table (c2 0x168 + 8 slot), apart from BindTexture's, so they go to slots 128 + n (NVN_SEPTEX_SLOT0)
NVN(nvnCommandBufferBindSeparateTextures, void, NVNcommandBuffer *c, NVNshaderStage st, int first, int n, const NVNseparateTextureHandle *h) {
    bind_handles(c, OP_BIND_TEX, st, NVN_SEPTEX_SLOT0 + first, n, (const uint64_t *)h);
}
NVN(nvnCommandBufferBindSeparateTexture, void, NVNcommandBuffer *c, NVNshaderStage st, int i, NVNseparateTextureHandle h) { uint64_t v; memcpy(&v, &h, 8); bind_handles(c, OP_BIND_TEX, st, NVN_SEPTEX_SLOT0 + i, 1, &v); }   // single (Engage): as the plural version
NVN(nvnCommandBufferBindSeparateSamplers, void, NVNcommandBuffer *c, NVNshaderStage st, int first, int n, const NVNseparateSamplerHandle *h) {
    bind_handles(c, OP_BIND_SAMP, st, first, n, (const uint64_t *)h);
}
NVN(nvnCommandBufferBindSeparateSampler, void, NVNcommandBuffer *c, NVNshaderStage st, int i, NVNseparateSamplerHandle h) {
    bind_handles(c, OP_BIND_SAMP, st, i, 1, &h.value);
}
NVN(nvnCommandBufferSetStencilValueMask, void, NVNcommandBuffer *c, NVNface f, int m) { *REC(c, OP_STENCIL, p_stencil) = p_stencil{f, m, 0, 0, 0}; }
NVN(nvnCommandBufferSetStencilMask, void, NVNcommandBuffer *c, NVNface f, int m) { *REC(c, OP_STENCIL, p_stencil) = p_stencil{f, 0, m, 0, 1}; }
NVN(nvnCommandBufferSetStencilRef, void, NVNcommandBuffer *c, NVNface f, int v) { *REC(c, OP_STENCIL, p_stencil) = p_stencil{f, 0, 0, v, 2}; }
NVN(nvnCommandBufferSetPolygonOffsetClamp, void, NVNcommandBuffer *c, float fa, float un, float cl) {
    *REC(c, OP_POLY_OFFSET, p_poly_offset) = p_poly_offset{fa, un, cl};
}
NVN(nvnCommandBufferDrawArrays, void, NVNcommandBuffer *c, NVNdrawPrimitive pr, int first, int n) { rec_draw(c, p_draw{pr, first, n, 1, 0, 0}); }
// Indirect: the {count, instances, first, base instance} record at ind is usually written by the GPU earlier in the frame (Hyrule Warriors: Age of Calamity's first battle), so the replay
// issues an indirect draw from the pool buffer instead of reading it at record or replay time
// NVN2 mesh draw: count task (or, with no task stage, mesh) workgroups from first, recorded as a draw of the replay's own primitive (nvn_obj.h NVN_PRIM_MESH), so the pass
// planning sees a draw that needs the pass's depth and colour like any other. Mario Kart World's race pass draws with it
NVN_NOPROTO(nvnCommandBufferDrawMeshTasks, void, NVNcommandBuffer *c, int first, int n) { if (n > 0) rec_draw(c, p_draw{NVN_PRIM_MESH, first, n, 1, 0, 0}); }
NVN(nvnCommandBufferDrawArraysIndirect, void, NVNcommandBuffer *c, NVNdrawPrimitive pr, NVNbufferAddress ind) { rec_draw(c, p_draw{pr, 0, 0, 1, 0, ind}); }
NVN(nvnCommandBufferDrawElementsInstanced, void, NVNcommandBuffer *c, NVNdrawPrimitive pr, NVNindexType it, int n, NVNbufferAddress ib,
    int bv, int bi, int inst) {
    rec_draw_idx(c, p_draw_idx{pr, it, n, bv, bi, inst, ib});
}
NVN(nvnCommandBufferDispatchCompute, void, NVNcommandBuffer *c, int x, int y, int z) {
    // The thunk is a tail jump (br), so the return address here is the call site in the game
    *REC(c, OP_DISPATCH, p_dispatch) = p_dispatch{x, y, z, call_site()};
}
// Indirect: the three group counts at ind are read by the GPU when the dispatch runs
NVN(nvnCommandBufferDispatchComputeIndirect, void, NVNcommandBuffer *c, NVNbufferAddress ind) {
    *REC(c, OP_DISPATCH, p_dispatch) = p_dispatch{0, 0, 0, call_site(), ind};
}
NVN(nvnCommandBufferBarrier, void, NVNcommandBuffer *c, int bits) { *(int32_t *)rec(c, OP_BARRIER, 8) = bits; }
NVN(nvnCommandBufferSetTexturePool, void, NVNcommandBuffer *c, const NVNtexturePool *p) { REC(c, OP_SET_TEX_POOL, p_set_pool)->pool = (const dpool_t *)p; CB(c)->tex_base = dpool_base(p, true); }
NVN(nvnCommandBufferSetSamplerPool, void, NVNcommandBuffer *c, const NVNsamplerPool *p) { REC(c, OP_SET_SAMP_POOL, p_set_pool)->pool = (const dpool_t *)p; CB(c)->samp_base = dpool_base(p, false); }

NVN(nvnCommandBufferCopyBufferToTexture, void, NVNcommandBuffer *c, NVNbufferAddress src, const NVNtexture *t, const NVNtextureView *v,
    const NVNcopyRegion *r, int flags) {
    p_copy_b2t *o = REC_SNAP(c, OP_COPY_B2T, p_copy_b2t, 1); o->src = src; o->tex = tex_snap(o, SNAP_OFF(p_copy_b2t), 0, t, call_site()); copy_view(&o->view, &o->has_view, v); o->r = *r; o->flags = flags;
}
NVN(nvnCommandBufferCopyTextureToBuffer, void, NVNcommandBuffer *c, const NVNtexture *t, const NVNtextureView *v, const NVNcopyRegion *r,
    NVNbufferAddress dst, int flags) {
    p_copy_b2t *o = REC_SNAP(c, OP_COPY_T2B, p_copy_b2t, 1); o->src = dst; o->tex = tex_snap(o, SNAP_OFF(p_copy_b2t), 0, t, call_site()); copy_view(&o->view, &o->has_view, v); o->r = *r; o->flags = flags;
}
// Clear a texture region with the given value (float / unsigned integer versions; mask = channel write mask)
NVN(nvnCommandBufferClearTexture, void, NVNcommandBuffer *c, const NVNtexture *t, const NVNtextureView *v, const NVNcopyRegion *r, const float *rgba, int mask) {
    p_clear_tex *o = REC_SNAP(c, OP_CLEAR_TEX, p_clear_tex, 1); o->tex = tex_snap(o, SNAP_OFF(p_clear_tex), 0, t, call_site()); copy_view(&o->view, &o->has_view, v); o->r = *r; o->mask = mask; o->is_uint = 0; memcpy(o->c.f, rgba, 16);
}
NVN(nvnCommandBufferClearTextureui, void, NVNcommandBuffer *c, const NVNtexture *t, const NVNtextureView *v, const NVNcopyRegion *r, const uint32_t *rgba, int mask) {
    p_clear_tex *o = REC_SNAP(c, OP_CLEAR_TEX, p_clear_tex, 1); o->tex = tex_snap(o, SNAP_OFF(p_clear_tex), 0, t, call_site()); copy_view(&o->view, &o->has_view, v); o->r = *r; o->mask = mask; o->is_uint = 1; memcpy(o->c.u, rgba, 16);
}
NVN(nvnCommandBufferCopyTextureToTexture, void, NVNcommandBuffer *c, const NVNtexture *s, const NVNtextureView *sv, const NVNcopyRegion *sr,
    const NVNtexture *d, const NVNtextureView *dv, const NVNcopyRegion *dr, int flags) {
    p_copy_t2t *o = REC_SNAP(c, OP_COPY_T2T, p_copy_t2t, 2); o->src = tex_snap(o, SNAP_OFF(p_copy_t2t), 0, s, call_site()); o->dst = tex_snap(o, SNAP_OFF(p_copy_t2t), 1, d, call_site());
    copy_view(&o->sv, &o->has_sv, sv); copy_view(&o->dv, &o->has_dv, dv); o->sr = *sr; o->dr = *dr; o->flags = flags;
}
// A textured rectangle into the bound colour targets, replayed as a textured rectangle (OP_DRAW_TEX). The handle's IDs are moved to the pools this command buffer bound, as bind_handles does:
// Streets of Rage 4 draws its movie frames into its scene target this way, from a second texture pool. Only the handle's lower 32 bits reach the GPU (bind_handles)
NVN(nvnCommandBufferDrawTexture, void, NVNcommandBuffer *c, NVNtextureHandle h, const NVNdrawTextureRegion *d, const NVNdrawTextureRegion *s) {
    uint64_t tid = (uint32_t)h & 0xfffff, sid = (uint32_t)h >> 20;
    if (tid) tid += (uint64_t)CB(c)->tex_base;
    if (sid) sid += (uint64_t)CB(c)->samp_base;
    *REC(c, OP_DRAW_TEX, p_draw_tex) = p_draw_tex{tid | sid << 20, *d, *s};
}
NVN(nvnCommandBufferSetCopyRowStride, void, NVNcommandBuffer *c, ptrdiff_t st) { p_copy_stride *o = REC(c, OP_COPY_STRIDE, p_copy_stride); o->stride = st; o->image = 0; }
NVN(nvnCommandBufferSetCopyImageStride, void, NVNcommandBuffer *c, ptrdiff_t st) { p_copy_stride *o = REC(c, OP_COPY_STRIDE, p_copy_stride); o->stride = st; o->image = 1; }   // Link's Awakening
NVN(nvnCommandBufferSetBlendColor, void, NVNcommandBuffer *c, const float *v) { memcpy(REC(c, OP_BLEND_COLOR, p_blend_color)->rgba, v, 16); }
// ---------- Transform feedback ----------
NVN(nvnCommandBufferBindTransformFeedbackBuffer, void, NVNcommandBuffer *c, int i, NVNbufferAddress a, size_t n) { *REC(c, OP_TF_BIND, p_tf_bind) = p_tf_bind{i, 0, a, n}; }
NVN(nvnCommandBufferBindTransformFeedbackBuffers, void, NVNcommandBuffer *c, int first, int n, const NVNbufferRange *r) {
    for (int i = 0; i < n; i++) *REC(c, OP_TF_BIND, p_tf_bind) = p_tf_bind{first + i, 0, r[i].address, r[i].size};
}
NVN(nvnCommandBufferBeginTransformFeedback, void, NVNcommandBuffer *c, NVNbufferAddress a) { *REC(c, OP_TF_STATE, p_tf_state) = p_tf_state{0, 0, a}; }
NVN(nvnCommandBufferEndTransformFeedback, void, NVNcommandBuffer *c, NVNbufferAddress a) { *REC(c, OP_TF_STATE, p_tf_state) = p_tf_state{1, 0, a}; }
NVN(nvnCommandBufferPauseTransformFeedback, void, NVNcommandBuffer *c, NVNbufferAddress a) { *REC(c, OP_TF_STATE, p_tf_state) = p_tf_state{2, 0, a}; }
NVN(nvnCommandBufferResumeTransformFeedback, void, NVNcommandBuffer *c, NVNbufferAddress a) { *REC(c, OP_TF_STATE, p_tf_state) = p_tf_state{3, 0, a}; }
NVN(nvnCommandBufferDrawTransformFeedback, void, NVNcommandBuffer *c, NVNdrawPrimitive m, NVNbufferAddress a) {
    *REC(c, OP_DRAW, p_draw) = p_draw{.prim=m,.instances=1,.ind=a+16};
}
// ---------- Command memory usage ----------
// Command buffer memory usage: record bytes written in this recording (accumulated across segments; the host does not use control memory, always 0); the memory callback switch is ignored.
// It used to be cur - begin: once a recording overflowed into a host segment, cur is in host memory (an address below the guest begin) and it returned 0. SMBW asks for this value after recording each material setup handle,
// and on 0 (with control memory also 0) treats the recording as empty and marks the handle invalid (from main+0x4bb6c it clears bit 0 of the material entry +0x50); later draws no longer CALL it and inherit the previous object's vertex buffers / textures:
// on a phone a batch of draws on odd frames of the opening course fetched vertices out of bounds, giving huge triangles and NaN (black band, black-edged glow, grey stripes). Without overflow the accumulated value equals cur - begin.
// As in NVN, the count runs from the start of the command memory the game last added (AddCommandMemory), not from BeginRecording: Fire Emblem: Three Houses adds one 16 MB chunk of a ring to its main
// command buffer and records every frame into it; its memory callback (main+0x9ca960) takes chunk start + used as the position the GPU has reached and frees the ring up to there when the frame's fence
// signals. Counted per recording, the position stayed at one frame's size (about 18 KB), the ring never freed past it, and after the first 16 MB (about 15 s) the callback waited for ring space forever
NVN(nvnCommandBufferGetCommandMemoryUsed, size_t, const NVNcommandBuffer *c) { const cmd_t *m = (const cmd_t *)c;
    return m->begin ? (size_t)m->used : 0; }
NVN(nvnCommandBufferGetControlMemoryUsed, size_t, const NVNcommandBuffer *c) { (void)c; return 0; }
NVN(nvnCommandBufferGetControlMemoryFree, size_t, const NVNcommandBuffer *c) { return ((const cmd_t *)c)->ctl_size; }
NVN(nvnCommandBufferGetControlMemorySize, size_t, const NVNcommandBuffer *c) { return ((const cmd_t *)c)->ctl_size; }
// Size and free bytes of the command memory the game last added (DRAGON QUEST XI S, Unreal Engine 4.18): as in NVN, size = used + free; free is what is left in the current segment
NVN(nvnCommandBufferGetCommandMemoryFree, size_t, const NVNcommandBuffer *c) { const cmd_t *m = (const cmd_t *)c; return m->cur && m->end > m->cur ? (size_t)(m->end - m->cur) : 0; }
NVN(nvnCommandBufferGetCommandMemorySize, size_t, const NVNcommandBuffer *c) { return h_nvnCommandBufferGetCommandMemoryUsed(c) + h_nvnCommandBufferGetCommandMemoryFree(c); }
NVN_NOPROTO(nvnCommandBufferSetCommandMemoryCallbackEnabled, void, NVNcommandBuffer *c, uint32_t on) { (void)c; (void)on; }
// ---------- Commands of newer-SDK games (SMBW and later) ----------
NVN(nvnCommandBufferCallCommands, void, NVNcommandBuffer *c, int n, const NVNcommandHandle *h) { h_nvnCommandBufferCopyCommands(c, n, h); }   // the replay layer treats CALL and COPY alike
// Combined texture handle (texture id | sampler id << 20) is recorded into the texture slot as is: the replay layer takes the texture from the low 20 bits and the sampler from the high bits 
// One combined handle: recorded as the hardware lays the command out, the handle 16 bytes in, where Luigi's Mansion 3 patches it after recording (nvn_obj.h p_bind_tex1)
NVN(nvnCommandBufferBindTexture, void, NVNcommandBuffer *c, NVNshaderStage st, int i, NVNtextureHandle h) {
    st = (NVNshaderStage)st_slot(st);
    int32_t tb = CB(c)->tex_base, sb = CB(c)->samp_base;
    if (tb || sb) { *REC(c, OP_BIND_TEX1, p_bind_tex1) = p_bind_tex1{st, i, (uint64_t)h, tb, sb}; return; }
    p_bind_tex1 *r = (p_bind_tex1 *)rec(c, OP_BIND_TEX1, offsetof(p_bind_tex1, tbase)); r->stage = st | BIND_TEX1_NOBASE; r->slot = i; r->h = (uint64_t)h;   // no bases: the record ends after h
}
static_assert(offsetof(p_bind_tex1, h) == 8, "a single texture bind keeps its handle 16 bytes into the record (8 of header)");
NVN(nvnCommandBufferBindTextures, void, NVNcommandBuffer *c, NVNshaderStage st, int first, int n, const NVNtextureHandle *h) { bind_handles(c, OP_BIND_TEX, st, first, n, (const uint64_t *)h); }
// "Deferred" binding: the GPU reads the handles h points to when it executes this command, not at recording. The per-texture binding of The Legend of Zelda: Breath of the Wild's engine (main+0xec2e64) always uses it,
//   passing the address of the handle field in the texture sampler object with a count of 1; command blocks are recorded once and submitted repeatedly, and the handle is written / replaced afterwards. The host used to copy the handles at recording (mostly 0),
//   so every texture of the deferred lighting composite became the dummy texture 0 and the whole 3D picture was black. The address is recorded and the replay layer reads it when it reads the record (bh_fresh)
NVN_NOPROTO(nvnCommandBufferBindTexturesDeferred, void, NVNcommandBuffer *c, NVNshaderStage st, int first, int n, const NVNtextureHandle *h) {
    p_bind_handles *r = bind_handles(c, OP_BIND_TEX, st, first, n, (const uint64_t *)h);
    if (r && h) { r->defer = 1; r->src = (uint64_t)(uintptr_t)h; }
    if (CB(c)->tex_base || CB(c)->samp_base) ONCE_RED("deferred-bound handles are read only at replay: the numbering base of this command buffer's bound descriptor pools is not added (pools %d / %d)", CB(c)->tex_base, CB(c)->samp_base);
}
NVN(nvnCommandBufferBindImage, void, NVNcommandBuffer *c, NVNshaderStage st, int i, NVNimageHandle h) { uint64_t v = (uint64_t)h; bind_handles(c, OP_BIND_IMG, st, i, 1, &v); }
NVN(nvnCommandBufferBindVertexBuffer, void, NVNcommandBuffer *c, int i, NVNbufferAddress a, size_t n) { NVNbufferRange r = {a, n}; h_nvnCommandBufferBindVertexBuffers(c, i, 1, &r); }

// Draws
NVN(nvnCommandBufferDrawElements, void, NVNcommandBuffer *c, NVNdrawPrimitive pr, NVNindexType it, int n, NVNbufferAddress ib) { h_nvnCommandBufferDrawElementsInstanced(c, pr, it, n, ib, 0, 0, 1); }
NVN(nvnCommandBufferDrawElementsBaseVertex, void, NVNcommandBuffer *c, NVNdrawPrimitive pr, NVNindexType it, int n, NVNbufferAddress ib, int bv) { h_nvnCommandBufferDrawElementsInstanced(c, pr, it, n, ib, bv, 0, 1); }
NVN(nvnCommandBufferDrawArraysInstanced, void, NVNcommandBuffer *c, NVNdrawPrimitive pr, int first, int n, int bi, int inst) {
    rec_draw(c, p_draw{pr, first, n, inst, bi});
}
// Indirect, as DrawArraysIndirect: the record at ind is read by the GPU
// Indirect-count multi-draws (Monster Hunter Rise's GPU-driven scene: compute culling writes the draw records and a u32 count, then up to max draws are made from them; 1–49 per call,
// stride 20, the records and counts in its sparse virtual pool). Vulkan's indirect-count draw is optional and Metal has none, so each multi-draw becomes max ordinary indirect draws that carry the count's address
// and their index; the renderer masks their records on the GPU, zeros past the count. Both used to be empty, and the whole scene stayed undrawn
enum { MDI_MAX = 1024 };
NVN(nvnCommandBufferMultiDrawElementsIndirectCount, void, NVNcommandBuffer *c, NVNdrawPrimitive pr, NVNindexType it, NVNbufferAddress ib, NVNbufferAddress ind, NVNbufferAddress cnt, int max, ptrdiff_t stride) {
    if (max > MDI_MAX) { ONCE_RED("indirect-count multi-draw of up to %d draws: only the first %d are made", max, MDI_MAX); max = MDI_MAX; }
    for (int i = 0; i < max; i++) rec_draw_idx(c, p_draw_idx{pr, it, 0, 0, 0, 1, ib, ind + (uint64_t)i * (uint64_t)stride, cnt, i});
}
NVN(nvnCommandBufferMultiDrawArraysIndirectCount, void, NVNcommandBuffer *c, NVNdrawPrimitive pr, NVNbufferAddress ind, NVNbufferAddress cnt, int max, ptrdiff_t stride) {
    if (max > MDI_MAX) { ONCE_RED("indirect-count multi-draw of up to %d draws: only the first %d are made", max, MDI_MAX); max = MDI_MAX; }
    for (int i = 0; i < max; i++) rec_draw(c, p_draw{pr, 0, 0, 1, 0, ind + (uint64_t)i * (uint64_t)stride, cnt, i});
}
NVN(nvnCommandBufferDrawElementsIndirect, void, NVNcommandBuffer *c, NVNdrawPrimitive pr, NVNindexType it, NVNbufferAddress ib, NVNbufferAddress ind) {
    rec_draw_idx(c, p_draw_idx{pr, it, 0, 0, 0, 1, ib, ind});
}

// Viewport / scissor / depth range: single and multiple forms
NVN(nvnCommandBufferSetViewport, void, NVNcommandBuffer *c, int x, int y, int w, int h) { float v[4] = {(float)x, (float)y, (float)w, (float)h}; h_nvnCommandBufferSetViewports(c, 0, 1, v); }
NVN(nvnCommandBufferSetScissors, void, NVNcommandBuffer *c, int first, int n, const int *r) {
    if (first == 0 && n >= 1) h_nvnCommandBufferSetScissor(c, r[0], r[1], r[2], r[3]);
    if (first + n > 1) ONCE_LOG("SetScissors first=%d n=%d: the host uses only index 0", first, n);
}
NVN(nvnCommandBufferSetDepthRanges, void, NVNcommandBuffer *c, int first, int n, const float *r) {
    if (first == 0 && n >= 1) h_nvnCommandBufferSetDepthRange(c, r[0], r[1]);
    if (first + n > 1) ONCE_LOG("SetDepthRanges first=%d n=%d: the host uses only index 0", first, n);
}

// GPU value write: ReportValue = write a 32-bit value to the address when this point is reached (the replay layer's event write, applied on completion)
NVN(nvnCommandBufferReportValue, void, NVNcommandBuffer *c, uint32_t v, NVNbufferAddress a) {
    volatile uint32_t *val = (volatile uint32_t *)gpu_ptr(a, 4);   // resolved now (the record holds a host pointer, like SignalEvent's); a report into an unmapped sparse page is dropped
    if (!val) { ONCE_RED("ReportValue to %#llx: the address is not backed by memory; dropped", (unsigned long long)a); return; }
    uint8_t *p = cmd_alloc(CB(c), 24); put_hdr(p, OP_SIGNAL_EVENT, 24); uint32_t mm = NVN_EVENT_SIGNAL_MODE_WRITE;
    memcpy(p + 8, &val, 8); memcpy(p + 16, &mm, 4); memcpy(p + 20, &v, 4);
}
// Clear a buffer to a 32-bit value: one OP_CLEAR_BUF record, filled in order within the submission at replay (fillBuffer when all bytes match, otherwise copied from a pattern buffer)
NVN(nvnCommandBufferClearBuffer, void, NVNcommandBuffer *c, NVNbufferAddress a, size_t n, uint32_t v) { *REC(c, OP_CLEAR_BUF, p_clear_buf) = p_clear_buf{a, n, v, 0}; }

// Render enable / conditional render: always recorded (OP_RENDER_COND) and evaluated at execution. This game runs with the nxnative profile key render_cond on: agl wraps its
// clears in render conditions (as in Donkey Kong Bananza), so they are evaluated from the start. mode 2 / 3 = SetRenderEnable(true / false); 0 / 1 = conditional on the counters at addr
NVN(nvnCommandBufferSetRenderEnable, void, NVNcommandBuffer *c, uint32_t en) { *REC(c, OP_RENDER_COND, p_render_cond) = p_render_cond{(en & 0xff) ? 2 : 3, 0, 0}; }
NVN(nvnCommandBufferSetRenderEnableConditional, void, NVNcommandBuffer *c, NVNconditionalRenderMode m, NVNbufferAddress a) { *REC(c, OP_RENDER_COND, p_render_cond) = p_render_cond{(int)m & 1, 0, a}; }
// Fire Emblem: Three Houses turns depth clamp on for its scenes; ignored, the ground right under its close-up battle cameras was clipped away and the lighting pass painted the sky's fog colour there
NVN(nvnCommandBufferSetDepthClamp, void, NVNcommandBuffer *c, uint32_t en) { REC(c, OP_DEPTH_CLAMP, p_depth_clamp)->on = (en & 0xff) != 0; }
// Absolum (FNA3D's NVN driver) keeps the lower-left window origin and sets viewport 0's swizzle every frame (NEGATIVE_Y: clip y negated, how a GL-heritage renderer flips its picture).
// Ignored, its whole picture came out upside down. The renderer applies it to the viewport
NVN(nvnCommandBufferSetViewportSwizzles, void, NVNcommandBuffer *c, int first, int n, const NVNviewportSwizzle *s) {
    if (n < 0 || first < 0 || first + n > 16) { LOG("[RED] nvn: SetViewportSwizzles first=%d n=%d", first, n); return; }
    p_vp_swizzle *r = REC(c, OP_VP_SWIZZLE, p_vp_swizzle); r->first = first; r->n = n;
    for (int i = 0; i < n; i++) for (int k = 0; k < 4; k++) r->sw[i][k] = (uint8_t)((s[4 * i + k] ^ 2 * k) & 7);
}
NVN(nvnCommandBufferSetAlphaRef, void, NVNcommandBuffer *c, float r) { REC(c, OP_ALPHA_REF, p_alpha_ref)->ref = r; }
NVN(nvnCommandBufferSetLineWidth, void, NVNcommandBuffer *c, float w) { (void)c; if (w != 1.0f) ONCE_LOG("SetLineWidth %.2f: wide lines are not modelled", w); }
NVN(nvnCommandBufferSetPointSize, void, NVNcommandBuffer *c, float s) { (void)c; (void)s; ONCE_LOG("SetPointSize: the point size comes from the shader"); }
NVN(nvnCommandBufferSetPrimitiveRestart, void, NVNcommandBuffer *c, uint32_t en, int idx) { *REC(c, OP_PRIM_RESTART, p_prim_restart) = p_prim_restart{(en & 0xff) != 0, (uint32_t)idx}; }
// Rasterizer discard: draws run their vertex stage only (with transform feedback, the capture); recorded, and the renderer rasterizes nothing for such a draw
NVN(nvnCommandBufferSetRasterizerDiscard, void, NVNcommandBuffer *c, uint32_t en) { REC(c, OP_RAST_DISCARD, p_rast_discard)->on = (en & 0xff) != 0; }
NVN(nvnCommandBufferSetSampleMask, void, NVNcommandBuffer *c, int m) { (void)c; if ((m & 0xffff) != 0xffff) ONCE_LOG("SetSampleMask %#x: ignored by the host for now", m); }
NVN(nvnCommandBufferSetConservativeRasterEnable, void, NVNcommandBuffer *c, uint32_t en) { (void)c; if (en & 0xff) ONCE_LOG("SetConservativeRasterEnable on: ignored by the host for now"); }
NVN(nvnCommandBufferSetTiledCacheAction, void, NVNcommandBuffer *c, NVNtiledCacheAction a) { (void)c; (void)a; }        // Tegra tiled cache hint, no host equivalent
// A bound target's contents may be discarded: they are undefined afterwards, so keeping them is correct (The Great Ace Attorney Chronicles discards colour target 0)
NVN(nvnCommandBufferDiscardColor, void, NVNcommandBuffer *c, int i) { (void)c; (void)i; }
NVN(nvnCommandBufferDiscardDepthStencil, void, NVNcommandBuffer *c) { (void)c; }
NVN(nvnCommandBufferSetTiledCacheTileSize, void, NVNcommandBuffer *c, int w, int h) { (void)c; (void)w; (void)h; }
NVN_NOPROTO(nvnCommandBufferSetColorReductionEnable, void, NVNcommandBuffer *c, uint32_t en) { (void)c; (void)en; }   // color compression hints
NVN_NOPROTO(nvnCommandBufferSetColorReductionThresholds, void, NVNcommandBuffer *c, int cls, const int *t) { (void)c; (void)cls; (void)t; }
// Resolve a multisample texture into a single-sample one (whole texture): recorded as a texture copy with NVN_COPY_RESOLVE; the renderer resolves the multisample texture
static void rec_downsample(NVNcommandBuffer *c, const NVNtexture *s, const NVNtextureView *sv, const NVNtexture *d, const NVNtextureView *dv, uint64_t ra) {
    p_copy_t2t *o = REC_SNAP(c, OP_COPY_T2T, p_copy_t2t, 2);
    o->src = tex_snap(o, SNAP_OFF(p_copy_t2t), 0, s, ra); o->dst = tex_snap(o, SNAP_OFF(p_copy_t2t), 1, d, ra);
    copy_view(&o->sv, &o->has_sv, sv); copy_view(&o->dv, &o->has_dv, dv);
    if (o->src) o->sr = NVNcopyRegion{0, 0, 0, (int)o->src->d.width, (int)o->src->d.height, 1};
    if (o->dst) o->dr = NVNcopyRegion{0, 0, 0, (int)o->dst->d.width, (int)o->dst->d.height, 1};
    o->flags = NVN_COPY_RESOLVE;
}
NVN(nvnCommandBufferDownsample, void, NVNcommandBuffer *c, const NVNtexture *s, const NVNtexture *d) { rec_downsample(c, s, NULL, d, NULL, call_site()); }
NVN(nvnCommandBufferDownsampleTextureView, void, NVNcommandBuffer *c, const NVNtexture *s, const NVNtextureView *sv, const NVNtexture *d, const NVNtextureView *dv) {
    rec_downsample(c, s, sv, d, dv, call_site());
}

// ---------- Commands that only affect GPU state the renderer does not model, or only execute on the GPU: counted, nothing recorded ----------
NVN(nvnCommandBufferSetPatchSize, void, NVNcommandBuffer *c, int n) { (void)c; (void)n; }
NVN(nvnCommandBufferSetDepthBounds, void, NVNcommandBuffer *c, uint32_t en, float lo, float hi) { (void)c; (void)en; (void)lo; (void)hi; }
NVN(nvnCommandBufferWaitSync, void, NVNcommandBuffer *c, const NVNsync *s) { (void)c; (void)s; }   // submissions complete in order; cross-queue waits are by sync object on the CPU
NVN(nvnCommandBufferSetShaderScratchMemory, void, NVNcommandBuffer *c, const NVNmemoryPool *p, ptrdiff_t off, size_t n) { (void)c; (void)p; (void)off; (void)n; }
NVN(nvnCommandBufferPushDebugGroupDynamic, void, NVNcommandBuffer *c, uint32_t d, const char *s) { (void)c; (void)d; (void)s; }
NVN(nvnCommandBufferPopDebugGroup, void, NVNcommandBuffer *c) { (void)c; }
NVN(nvnCommandBufferInsertDebugMarkerDynamic, void, NVNcommandBuffer *c, uint32_t d, const char *s) { (void)c; (void)d; (void)s; }
NVN(nvnCommandBufferInsertDebugMarker, void, NVNcommandBuffer *c, const char *s) { (void)c; (void)s; }   // 13 Sentinels
NVN(nvnCommandBufferPushDebugGroup, void, NVNcommandBuffer *c, uint32_t d, const char *s) { (void)c; (void)d; (void)s; }   // NVN 53
NVN(nvnCommandBufferPushDebugGroupStatic, void, NVNcommandBuffer *c, uint32_t d, const char *s) { (void)c; (void)d; (void)s; }
NVN(nvnCommandBufferPopDebugGroupId, void, NVNcommandBuffer *c, uint32_t d) { (void)c; (void)d; }
NVN(nvnCommandBufferInsertDebugMarkerStatic, void, NVNcommandBuffer *c, uint32_t d, const char *s) { (void)c; (void)d; (void)s; }
NVN(nvnCommandBufferSetDebugLabel, void, NVNcommandBuffer *c, const char *s) { (void)c; (void)s; }
// ZCull data: the host does no ZCull, so there is nothing to save / restore
NVN(nvnCommandBufferRestoreZCullData, void, NVNcommandBuffer *c, NVNbufferAddress a, size_t n) { (void)c; (void)a; (void)n; }
NVN(nvnCommandBufferSaveZCullData, void, NVNcommandBuffer *c, NVNbufferAddress a, size_t n) { (void)c; (void)a; (void)n; }
// The stencil test ZCull's stencil cull assumes (a stencil function, then its reference and mask): only a hint for culling ahead of the real stencil test, which the draws'
// depth-stencil state still does. The host does no ZCull, so there is nothing to set (Donkey Kong Bananza: EQUAL, 0xff)
NVN_NOPROTO(nvnCommandBufferSetStencilCullCriteria, void, NVNcommandBuffer *c, int func, int ref) { (void)c; (void)func; (void)ref; }
// NVN2 variable-rate shading: the shading rate image palette and image are hints (Mario Kart World sets the palette on every command buffer); full-rate shading is always correct
NVN_NOPROTO(nvnCommandBufferShadingRateImagePalette, void, NVNcommandBuffer *c) { (void)c; }
NVN_NOPROTO(nvnCommandBufferBindShadingRateImage, void, NVNcommandBuffer *c) { (void)c; }
NVN_NOPROTO(nvnCommandBufferSetShadingRatePaletteMode, void, NVNcommandBuffer *c) { (void)c; }

}  // namespace nvn
