// The rp_* entry points (replay.h) over the Vulkan renderer (vk_rp_*, replay_vk.cpp), with the renderer on a thread of
// its own: every call is a step of one queue, done in the order the calls were made, so the renderer sees exactly the
// sequence it saw when the game's threads called it directly, only later. A submission returns at once with the fence
// value it will commit with (the renderer commits the queued submissions in order, one value each, so the n-th
// queued is the n-th committed); its fence-sync records are fenced with that value right away, as the game may wait on
// them before the renderer reaches them. Calls that return something from the renderer (texel readback, GPU
// ownership) wait for the queue to get there. At most MaxQueued submissions wait: the submitting thread blocks beyond.
//
// Guest memory the renderer reads (command memory, uniforms, texture storage) is read later than before, as the
// console's GPU reads it later than the submit: the game does not change it before the fence it waits on.
//
// HOST_NVN_SYNC_REPLAY=1: everything on the calling thread, as before.
#include "replay_async.h"

#include <windows.h>

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <functional>
#include <future>
#include <mutex>
#include <thread>
#include <vector>

#include "../nvn_int.h"
#include "hitch.h"

namespace nvk {
u64 g_hitch_ns[HI_N];
}

namespace nvn {
namespace {

constexpr int MaxQueued = 6;
constexpr int HoldMs = 33;

struct Step {
    std::function<void()> fn;
    bool sub = false;  // a submission (counted against MaxQueued)
};

std::mutex g_q_mu;
std::condition_variable g_work, g_room;
std::deque<Step> g_q;
int g_subs = 0;                 // submissions queued and not yet replayed (under g_q_mu)
std::atomic<u64> g_issued{0};   // fence value of the last queued submission
std::thread::id g_renderer;
u64 g_stall_ns = 0, g_stall_n = 0, g_stats_at = 0;   // submitting threads waiting for room (under g_q_mu)

// A step of the renderer that took HitchMs or more: logged with the kinds of work it spent its time in (hitch.h), at
// most HitchLogs times in 10 s; the game waits on its fences meanwhile.
constexpr u64 HitchMs = 25, HitchLogs = 12;

void log_hitch(u64 ns, const u64* before) {
    static u64 at = 0, logged = 0, n = 0, total = 0;
    const u64 now = host_ns();
    if (now - at > 10'000'000'000ull) {
        if (at && n > logged)
            LOG("nvk: renderer: %llu more steps of %llu ms or more (%llu ms in all)", static_cast<unsigned long long>(n - logged),
                static_cast<unsigned long long>(HitchMs), static_cast<unsigned long long>(total / 1'000'000));
        at = now, logged = n = total = 0;
    }
    n++, total += ns;
    if (logged >= HitchLogs) return;
    logged++;
    char b[512];
    int o = std::snprintf(b, sizeof b, "nvk: renderer step took %llu ms:", static_cast<unsigned long long>(ns / 1'000'000));
    u64 known = 0;
    for (int k = 0; k < nvk::HI_N; k++) {
        const u64 d = nvk::g_hitch_ns[k] - before[k];
        known += d;
        if (d >= 1'000'000 && o < static_cast<int>(sizeof b) - 64)
            o += std::snprintf(b + o, sizeof b - o, " %s %llu,", nvk::HitchNames[k], static_cast<unsigned long long>(d / 1'000'000));
    }
    if (o < static_cast<int>(sizeof b) - 64)
        std::snprintf(b + o, sizeof b - o, " other %llu", static_cast<unsigned long long>(ns > known ? (ns - known) / 1'000'000 : 0));
    LOG("%s", b);
}

void renderer_loop() {
    SetThreadDescription(GetCurrentThread(), L"NVN renderer");
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
    nvk::t_hitch = true;
    for (;;) {
        Step s;
        {
            std::unique_lock lk{g_q_mu};
            // Held submissions (replay_vk.cpp) wait for the one that signals them, not for ever
            if (vk_rp_holding()) {
                if (!g_work.wait_for(lk, std::chrono::milliseconds(HoldMs), [] { return !g_q.empty(); })) {
                    lk.unlock();
                    vk_rp_force_held();
                    continue;
                }
            } else {
                g_work.wait(lk, [] { return !g_q.empty(); });
            }
            s = std::move(g_q.front());
            g_q.pop_front();
        }
        u64 before[nvk::HI_N];
        std::memcpy(before, nvk::g_hitch_ns, sizeof before);
        const u64 t0 = host_ns();
        s.fn();
        if (const u64 ns = host_ns() - t0; ns >= HitchMs * 1'000'000) log_hitch(ns, before);
        if (s.sub) {
            std::scoped_lock lk{g_q_mu};
            g_subs--;
            g_room.notify_all();
        }
    }
}

void start() {
    static std::once_flag once;
    std::call_once(once, [] {
        std::thread t(renderer_loop);
        g_renderer = t.get_id();
        t.detach();
    });
}

void post(std::function<void()> fn, bool sub = false) {
    if (!async_replay()) return fn();
    start();
    {
        std::unique_lock lk{g_q_mu};
        if (sub) {
            if (g_subs >= MaxQueued) {
                const u64 t0 = host_ns();
                g_room.wait(lk, [] { return g_subs < MaxQueued; });
                g_stall_ns += host_ns() - t0, g_stall_n++;
            }
            g_subs++;
            if (const u64 now = host_ns(); now - g_stats_at > 10'000'000'000ull) {
                if (g_stats_at)
                    LOG("nvk: renderer thread: the game waited for room in its queue %llu times (%llu ms in all)",
                        static_cast<unsigned long long>(g_stall_n), static_cast<unsigned long long>(g_stall_ns / 1'000'000));
                g_stats_at = now, g_stall_n = g_stall_ns = 0;
            }
        }
        g_q.push_back({std::move(fn), sub});
    }
    g_work.notify_one();
}

// f's result, from the renderer thread once everything queued before is done.
template <class F> auto call(F f) -> decltype(f()) {
    if (!async_replay() || std::this_thread::get_id() == g_renderer) return f();
    std::packaged_task<decltype(f())()> task(std::move(f));
    auto result = task.get_future();
    post([&task] { task(); });
    return result.get();
}

// The fence-sync records of a recording (following CALL and JUMP) fenced with `fence`.
void fence_syncs(const uint8_t* p, u64 fence, int depth = 0) {
    if (depth > 16) return;
    for (;;) {
        op_hdr h;
        memcpy(&h, p, sizeof h);
        if (h.size < 8 || h.op >= OP_COUNT_ || (h.size & 7)) return;   // (the renderer logs it)
        const uint8_t* a = p + 8;
        switch (h.op) {
        case OP_END: return;
        case OP_JUMP: {
            const uint8_t* t;
            memcpy(&t, a, 8);
            if (!t) return;
            p = t;
            continue;
        }
        case OP_CALL: {
            uint64_t t;
            memcpy(&t, a, 8);
            if (t) fence_syncs(static_cast<const uint8_t*>(hp(t)), fence, depth + 1);
            break;
        }
        case OP_FENCE_SYNC: {
            sync_t* s;
            memcpy(&s, a, 8);
            s->fence = fence;
            s->fenced = 1;
            break;
        }
        default: break;
        }
        p += h.size;
    }
}

// The submission being made (rp_sub_begin .. rp_sub_commit run under the submission lock).
std::vector<const uint8_t*> g_handles;
uint64_t g_sub_queue = 0;

struct TexArg {
    bool has = false;
    tex_t t{};
    TexArg(const tex_t* p) {
        if (p) has = true, t = *p;
    }
    tex_t* get() { return has ? &t : nullptr; }
};

}  // namespace

bool async_replay() {
    static const bool on = getenv("HOST_NVN_SYNC_REPLAY") == nullptr;
    return on;
}

void rp_set_origin_lower_left(int on) { post([on] { vk_rp_set_origin_lower_left(on); }); }
void rp_set_depth_zero(int on) { post([on] { vk_rp_set_depth_zero(on); }); }
void rp_set_queue_origin_lower_left(int on) { post([on] { vk_rp_set_queue_origin_lower_left(on); }); }

void rp_pool_add(u64 gpu, u64 size, u32 flags) { post([gpu, size, flags] { vk_rp_pool_add(gpu, size, flags); }); }
void rp_pool_flushed(u64 gpu, u64 size) { post([gpu, size] { vk_rp_pool_flushed(gpu, size); }); }
void rp_pool_invalidated(u64 gpu, u64 size) {
    // (only the GPU's copy has what the game is about to read: back when it is in system memory again)
    if (vk_rp_pool_gpu_owned(gpu, size)) call([gpu, size] { vk_rp_pool_invalidated(gpu, size); });
    else post([gpu, size] { vk_rp_pool_invalidated(gpu, size); });
}
void rp_pool_del(u64 gpu) { post([gpu] { vk_rp_pool_del(gpu); }); }
void rp_alias_drop_pool(const pool_t* pool) { post([pool] { vk_rp_alias_drop_pool(pool); }); }
void rp_vpool_create(u64 base, u64 size) { post([base, size] { vk_rp_vpool_create(base, size); }); }
void rp_vpool_map(u64 base, u64 voff, u64 size, u64 phys) { post([=] { vk_rp_vpool_map(base, voff, size, phys); }); }
void rp_vpool_destroy(u64 base) { post([base] { vk_rp_vpool_destroy(base); }); }

void rp_set_tex_pool_n(int n) { post([n] { vk_rp_set_tex_pool_n(n); }); }
void rp_reg_tex(int id, const tex_t* t, const view_t* v) {
    const bool has_v = v != nullptr;
    const view_t vc = v ? *v : view_t{};
    post([id, ta = TexArg(t), has_v, vc]() mutable { vk_rp_reg_tex(id, ta.get(), has_v ? &vc : nullptr); });
}
void rp_reg_image(int id, const tex_t* t, const view_t* v) {
    const bool has_v = v != nullptr;
    const view_t vc = v ? *v : view_t{};
    post([id, ta = TexArg(t), has_v, vc]() mutable { vk_rp_reg_image(id, ta.get(), has_v ? &vc : nullptr); });
}
void rp_reg_samp(int id, const samp_par* p) {
    const bool has = p != nullptr;
    const samp_par pc = p ? *p : samp_par{};
    post([id, has, pc] { vk_rp_reg_samp(id, has ? &pc : nullptr); });
}

void rp_tex_init(const tex_t* t) { post([ta = TexArg(t)]() mutable { vk_rp_tex_init(ta.get()); }); }
void rp_tex_release(tex_t* t) { post([ta = TexArg(t)]() mutable { vk_rp_tex_release(ta.get()); }); }
void rp_tex_cpu_written(const tex_t* t) { post([ta = TexArg(t)]() mutable { vk_rp_tex_cpu_written(ta.get()); }); }
int rp_tex_gpu_owned(uint64_t serial) { return call([serial] { return vk_rp_tex_gpu_owned(serial); }); }
void rp_read_texels(const tex_t* t, unsigned level, unsigned layer0, const NVNcopyRegion* r, uint8_t* dst, size_t row, size_t img) {
    call([&] { vk_rp_read_texels(t, level, layer0, r, dst, row, img); });
}

void rp_prog_release(prog_t* p) {
    post([pc = *p]() mutable { vk_rp_prog_release(&pc); });
}
void rp_prog_prefetch(const prog_t* p) {
    post([pc = *p] { vk_rp_prog_prefetch(&pc); });
}
void rp_prog_forget(const prog_t* p) {
    post([pc = *p] { vk_rp_prog_forget(&pc); });
}

void rp_queue_activate(uint64_t serial) {
    g_sub_queue = serial;
    post([serial] { vk_rp_queue_activate(serial); });
}
void rp_queue_forget(uint64_t serial) { post([serial] { vk_rp_queue_forget(serial); }); }
void rp_prepare() {
    // (once: the renderer's own setup, on its thread)
    static std::atomic<bool> done{false};
    if (done.load(std::memory_order_acquire)) return;
    call([] { vk_rp_prepare(); });
    done.store(true, std::memory_order_release);
}

void rp_sub_begin() {
    if (!async_replay()) return vk_rp_sub_begin();
    g_handles.clear();
}
void rp_exec_rest(const uint64_t* handles, int n) {
    if (!async_replay()) vk_rp_exec_rest(handles, n);
}
void rp_exec(const uint8_t* handle) {
    if (!async_replay()) return vk_rp_exec(handle);
    fence_syncs(handle, g_issued.load() + 1);
    g_handles.push_back(handle);
}
uint64_t rp_sub_commit() {
    if (!async_replay()) return vk_rp_sub_commit();
    const u64 fence = g_issued.load() + 1;
    g_issued.store(fence);
    // HOST_NVN_EVENT_ORDER=0: every submission as it comes, whatever its events say
    static const bool ordered = getenv("HOST_NVN_EVENT_ORDER") == nullptr || atoi(getenv("HOST_NVN_EVENT_ORDER")) != 0;
    if (ordered) {
        post([handles = std::move(g_handles), fence, queue = g_sub_queue]() mutable { vk_rp_submit(queue, std::move(handles), fence); }, true);
        g_handles.clear();
        return fence;
    }
    post(
        [handles = std::move(g_handles), fence] {
            vk_rp_sub_begin();
            for (const uint8_t* h : handles) vk_rp_exec(h);
            vk_rp_exec_rest(nullptr, 0);
            const u64 v = vk_rp_sub_commit();
            if (v != fence) {
                static int n;
                if (n++ < 8) LOG("[RED] nvk: renderer thread: submission queued as fence %llu committed as %llu",
                                 static_cast<unsigned long long>(fence), static_cast<unsigned long long>(v));
            }
        },
        true);
    g_handles.clear();
    return fence;
}

uint64_t rp_last_fence() { return async_replay() ? g_issued.load() : vk_rp_last_fence(); }
void rp_finish() {
    if (!async_replay()) return vk_rp_finish();
    rp_wait(g_issued.load(), UINT64_MAX);
}
void rp_force_held() {
    if (async_replay()) post([] { vk_rp_force_held(); });
}
void rp_drain() {
    call([] {
        vk_rp_force_held();
        vk_rp_drain();
    });
}

uint64_t rp_present(const tex_t* t, double period_s, const int crop[4]) {
    if (!async_replay()) return vk_rp_present(t, period_s, crop);
    static std::atomic<u64> n{0};
    std::array<int, 4> c{};
    const bool has_crop = crop != nullptr;
    if (crop) std::memcpy(c.data(), crop, sizeof c);
    post([ta = TexArg(t), period_s, has_crop, c]() mutable { vk_rp_present(ta.get(), period_s, has_crop ? c.data() : nullptr); });
    return ++n;
}

}  // namespace nvn
