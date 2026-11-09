// Native NVN: a null renderer behind replay.h. No GPU: submissions complete when they commit, textures read back as zeros, draws, clears and texture copies are ignored.
// It still walks the recorded command streams exactly as a real renderer would (following JUMP and CALL) and performs every effect the game can see on the CPU, in command
// order: buffer copies, inline uniform updates and buffer fills (memmove on guest memory), event signals and counter reports (applied at commit, before the fence completes:
// the completion rule of replay.h), fence syncs. That keeps the game's CPU-side logic (event polling, GPU timestamps, readbacks through buffers) correct without a GPU.
// Replace this file's functions with the real renderer's; nothing else in nvn/ depends on it.
// Built only with NVN_NULL_RENDERER defined; vk/replay_vk.cpp is the renderer otherwise.
#ifdef NVN_NULL_RENDERER
#include <condition_variable>

#include "nvn_int.h"

namespace nvn {

namespace {
std::mutex g_mu;
std::condition_variable g_cv;
std::atomic<uint64_t> g_fence{0};
std::atomic<uint64_t> g_present_n{0};
std::atomic<uint64_t> g_commits{0}, g_records{0};

// Effects with a CPU-visible result that wait for "GPU completion": applied at commit, in order, before the fence is published.
struct Post { enum Kind { Event, Counter } kind; volatile uint32_t* val; uint32_t mode, v; u64 addr; uint32_t type; };
std::vector<Post> g_post;   // the submission in progress (under the submission lock)

int g_depth;                // rp_exec nesting (CALL): a game bug must not recurse without bound
constexpr int MaxDepth = 64;

void fill32(u64 dst, u64 n, uint32_t value) {
    // 32-bit pattern, little endian from dst; filled through gpu_write in chunks so sparse ranges work too
    uint32_t chunk[4096];
    for (uint32_t& w : chunk) w = value;
    for (u64 off = 0; off < n;) {
        const u64 k = std::min<u64>(n - off, sizeof chunk);
        gpu_write(dst + off, chunk, k);
        off += k;
    }
}
}  // namespace

// ---------------------------------------------------------------- device state
void rp_set_origin_lower_left(int) {}
void rp_set_depth_zero(int) {}
void rp_set_queue_origin_lower_left(int) {}

// ---------------------------------------------------------------- memory pools
void rp_pool_add(u64, u64) {}
void rp_pool_del(u64) {}
void rp_alias_drop_pool(const pool_t*) {}
void rp_vpool_create(u64, u64) {}
void rp_vpool_map(u64, u64, u64, u64) {}
void rp_vpool_destroy(u64) {}

// ---------------------------------------------------------------- descriptors
void rp_set_tex_pool_n(int) {}
void rp_reg_tex(int, const tex_t*, const view_t*) {}
void rp_reg_image(int, const tex_t*, const view_t*) {}
void rp_reg_samp(int, const samp_par*) {}

// ---------------------------------------------------------------- textures
void rp_tex_init(const tex_t*) {}
void rp_tex_release(tex_t*) {}
void rp_tex_cpu_written(const tex_t*) {}
int rp_tex_gpu_owned(uint64_t) { return 0; }
void rp_read_texels(const tex_t* t, unsigned, unsigned, const NVNcopyRegion* r, uint8_t* dst, size_t row, size_t img) {
    // A GPU-written texture reads as zeros here
    int bw, bh; const int bpb = nvn_fmt_bpb(t->d.format, &bw, &bh);
    if (!bpb || !dst) return;
    const size_t wb = (size_t)((r->width + bw - 1) / bw) * bpb, hb = (size_t)(r->height + bh - 1) / bh;
    const int nz = r->depth ? r->depth : 1;
    if (!row) row = wb;
    if (!img) img = row * hb;
    for (int z = 0; z < nz; z++) for (size_t y = 0; y < hb; y++) memset(dst + (size_t)z * img + y * row, 0, wb);
}

// ---------------------------------------------------------------- programs
void rp_prog_release(prog_t*) {}
void rp_prog_prefetch(const prog_t*) {}
void rp_prog_forget(const prog_t*) {}

// ---------------------------------------------------------------- queues / submissions
void rp_queue_activate(uint64_t) {}
void rp_queue_forget(uint64_t) {}
void rp_prepare() {
    static bool once = [] { LOG("nvn: null renderer: no GPU; submissions complete at commit, presents are paced at the game's interval, textures read as zeros"); return true; }();
    (void)once;
}
void rp_sub_begin() { g_post.clear(); }

void rp_exec_rest(const uint64_t*, int) {}

// Executes a recording. Returns at its END. Also an END reached through a JUMP chain.
void rp_exec(const uint8_t* p) {
    if (g_depth >= MaxDepth) { LOG("[RED] nvn: command stream calls nest deeper than %d; ignored", MaxDepth); return; }
    struct Depth { Depth() { g_depth++; } ~Depth() { g_depth--; } } depth_guard;
    for (;;) {
        op_hdr h; memcpy(&h, p, sizeof h);
        if (h.size < 8 || h.op >= OP_COUNT_ || (h.size & 7)) { LOG("[RED] nvn: corrupt command record at %p (op %u size %u): the recording is cut here", (const void*)p, h.op, h.size); return; }
        g_records.fetch_add(1, std::memory_order_relaxed);
        const uint8_t* a = p + 8;
        switch (h.op) {
        case OP_END: return;
        case OP_JUMP: { const uint8_t* t; memcpy(&t, a, 8); if (!t) { LOG("[RED] nvn: JUMP to null"); return; } p = t; continue; }
        case OP_CALL: { uint64_t t; memcpy(&t, a, 8); if (t) rp_exec(static_cast<const uint8_t*>(hp(t))); break; }
        case OP_SIGNAL_EVENT: { volatile uint32_t* val; uint32_t m, v; memcpy(&val, a, 8); memcpy(&m, a + 8, 4); memcpy(&v, a + 12, 4); rp_sub_event(val, m, v); break; }
        case OP_REPORT_COUNTER: { uint64_t t, ad; memcpy(&t, a, 8); memcpy(&ad, a + 8, 8); rp_sub_counter(ad, (uint32_t)t); break; }
        case OP_FENCE_SYNC: { sync_t* s; memcpy(&s, a, 8); s->fence = g_fence.load() + 1; s->fenced = 1; break; }   // the fence value this submission commits with
        case OP_CLEAR_BUF: { const p_clear_buf* cb = reinterpret_cast<const p_clear_buf*>(a); rp_sub_fill(cb->addr, cb->size, cb->value); break; }
        case OP_COPY_B2B: { uint64_t s_, d, n; memcpy(&s_, a, 8); memcpy(&d, a + 8, 8); memcpy(&n, a + 16, 8); rp_sub_copy(s_, d, n); break; }
        case OP_UPDATE_UB: { uint64_t d, n; memcpy(&d, a, 8); memcpy(&n, a + 8, 8); rp_sub_update(a + 16, d, n); break; }
        default: break;   // state, draws, clears, texture copies, dispatches, barriers, render conditions: nothing to see on the CPU
        }
        p += h.size;
    }
}

uint64_t rp_sub_commit() {
    // Everything the submission does completes now: effects first, then the fence
    for (const Post& op : g_post) {
        if (op.kind == Post::Event) event_apply(op.val, op.mode, op.v);
        else { const uint64_t rec[2] = {0, gpu_ticks()}; gpu_write(op.addr, rec, sizeof rec); }   // count 0 (render conditions on: the GPU writes none), timestamp now
    }
    g_post.clear();
    g_commits.fetch_add(1, std::memory_order_relaxed);
    uint64_t f;
    { std::scoped_lock lk{g_mu}; f = g_fence.fetch_add(1) + 1; }
    g_cv.notify_all();
    return f;
}

void rp_sub_copy(u64 src, u64 dst, u64 n) {
    if (!n) return;
    uint8_t *s = gpu_ptr(src, n), *d = gpu_ptr(dst, n);
    if (s && d) { memmove(d, s, n); return; }
    std::vector<uint8_t> tmp(n);   // a range across sparse mappings: through a temporary (unmapped source pages read as zero, unmapped destination pages swallow the write)
    gpu_read(src, tmp.data(), n);
    gpu_write(dst, tmp.data(), n);
}
void rp_sub_update(const void* data, u64 dst, u64 n) { if (n) gpu_write(dst, data, n); }
void rp_sub_fill(u64 dst, u64 n, uint32_t value) { if (n) fill32(dst, n, value); }
void rp_sub_event(volatile uint32_t* val, uint32_t mode, uint32_t v) { g_post.push_back(Post{Post::Event, val, mode, v, 0, 0}); }
void rp_sub_counter(u64 addr, uint32_t type) { g_post.push_back(Post{Post::Counter, nullptr, 0, 0, addr, type}); }

// ---------------------------------------------------------------- fences / waiting
uint64_t rp_last_fence() { return g_fence.load(); }
bool rp_wait(uint64_t fence, uint64_t timeout_ns) {
    if (g_fence.load() >= fence) return true;
    if (!timeout_ns) return false;
    std::unique_lock lk{g_mu};
    if (timeout_ns == UINT64_MAX) { g_cv.wait(lk, [&] { return g_fence.load() >= fence; }); return true; }
    return g_cv.wait_for(lk, std::chrono::nanoseconds(timeout_ns), [&] { return g_fence.load() >= fence; });
}
bool rp_wait_acq(uint64_t fence, uint64_t timeout_ns) { return rp_wait(fence, timeout_ns); }
void rp_finish() {}   // every submission completed at its commit
void rp_drain() {}

// ---------------------------------------------------------------- present
uint64_t rp_present(const tex_t*, double, const int*) { return ++g_present_n; }
void rp_present_wait(uint64_t) {}
void rp_window_tex(const tex_t*) {}

}  // namespace nvn
#endif  // NVN_NULL_RENDERER
