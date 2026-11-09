// Native NVN: windows (swapchains) and present.
//
// Real-device semantics: present only enqueues and returns immediately; this frame's display time is scheduled by the pacer, and the texture can be acquired again only at
// "display time + one present period" (blocking in AcquireTexture, not in present: the game often calls present while holding a lock). The period is the present interval the
// game set times 1/60 s (present is fixed at 60 Hz as in nxnative: HOST_NVN_PRESENT_HZ=0 uncaps it for profiling only; some of the game's UI logic counts frames, and the game steps its simulation per interval).
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "nvn_int.h"
#include "../video.h"

namespace nvn {

#define WB(b) ((win_builder_t*)(b))
#define CTX(t) ((const tex_t*)(t))

namespace {
int present_hz() {
    static int hz = [] {
        const char* e = std::getenv("HOST_NVN_PRESENT_HZ");
        const int v = e ? std::atoi(e) : 60;
        if (!v) LOG("nvn: [WARNING] HOST_NVN_PRESENT_HZ=0: frame rate uncapped, frame pacing unlike the device, UI logic may drift (profiling only)");
        return v;
    }();
    return hz;
}
// Frames follow the present interval the game sets (1 = 60 fps, 2 = 30 fps ...), except in the dynamic frame mode (cpu.h g_mkw_frame_mode 0):
// there the game marks a frame that ran late as 2, and holding it for two vblanks would keep the game at 30 fps (as pace_to_vblank in gsv.cpp)
int present_itv(const win_t* o) { return g_mkw_frame_mode == 0 || o->interval <= 0 ? 1 : o->interval; }
uint64_t present_period(const win_t* o) { const int hz = present_hz(); return hz > 0 ? (uint64_t)present_itv(o) * 1000000000ull / (uint64_t)hz : 0; }

// Present pacer. Semantics as vsync: present slot = previous present slot + period. Early -> wait until the present slot; late by less than one period -> present immediately and keep the original
// phase; behind by one period or more (stutter) -> restart the cadence from the current time, no burst of presents to catch up. Period 0 = no wait. -> the ns to wait (0 = present immediately); updates *target.
uint64_t pace(uint64_t* target, uint64_t now, uint64_t period) {
    if (!period || !*target) { *target = now; return 0; }
    const uint64_t next = *target + period;
    if (now <= next) { *target = next; return next - now; }
    *target = now - next < period ? next : now;
    return 0;
}

// Sleep until an absolute time on the host clock, in HostWait (the guest thread's core runs other threads meanwhile). A high-resolution waitable timer: the default Windows timer tick is 15.6 ms.
void sleep_until_ns(uint64_t dl) {
    const uint64_t now = now_ns();
    if (dl <= now) return;
    thread_local HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    HostWait hw;
    if (timer) {
        LARGE_INTEGER due; due.QuadPart = -(LONGLONG)((dl - now) / 100);
        if (SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) { WaitForSingleObject(timer, INFINITE); return; }
    }
    Sleep((DWORD)((dl - now) / 1000000));
}

// Frame times (from present to present) and the game's waits in AcquireTexture, logged every 10 s: whether frames
// that miss 60 fps are a little late or a few very late, and how long the game waited for the pacer or the renderer.
u64 g_acq_pace_ns = 0, g_acq_renderer_ns = 0;
// The corner overlay's numbers (video.cpp), over half a second: fps, mean frame time, the game's mean wait for the renderer.
void overlay_frame(u64 now, u64 renderer_ns) {
    static u64 at = 0, frames = 0, waited = 0;
    if (!at) { at = now; return; }
    frames++, waited += renderer_ns;
    if (now - at < 500'000'000ull) return;
    const double span = (now - at) / 1e9;
    video::report_frame_stats(frames / span, span * 1000.0 / frames, waited / 1e6 / frames);
    at = now, frames = waited = 0;
}
void note_frame(u64 now) {
    static u64 acq_renderer_seen = 0;
    overlay_frame(now, g_acq_renderer_ns - acq_renderer_seen);
    acq_renderer_seen = g_acq_renderer_ns;
    static u64 last = 0, at = 0, worst = 0, n[6] = {};
    static constexpr u64 Edge[5] = {17'500'000, 20'000'000, 25'000'000, 34'000'000, 50'000'000};
    if (last) {
        const u64 d = now - last;
        int b = 0;
        while (b < 5 && d >= Edge[b]) b++;
        n[b]++;
        worst = d > worst ? d : worst;
    }
    last = now;
    if (!at) at = now;
    if (now - at < 10'000'000'000ull) return;
    LOG("nvn: frame times: %llu under 17.5 ms, %llu under 20, %llu under 25, %llu under 34, %llu under 50, %llu longer (longest %llu ms); "
        "the game waited in AcquireTexture for the pacer %llu ms, for the renderer %llu ms",
        (unsigned long long)n[0], (unsigned long long)n[1], (unsigned long long)n[2], (unsigned long long)n[3], (unsigned long long)n[4],
        (unsigned long long)n[5], (unsigned long long)(worst / 1000000), (unsigned long long)(g_acq_pace_ns / 1000000),
        (unsigned long long)(g_acq_renderer_ns / 1000000));
    at = now, worst = 0, g_acq_pace_ns = g_acq_renderer_ns = acq_renderer_seen = 0;
    for (u64& x : n) x = 0;
}
}  // namespace

NVN(nvnWindowBuilderSetDefaults, void, NVNwindowBuilder* b) { NVNdevice* dev = WB(b)->dev; memset(b, 0, sizeof(win_builder_t)); WB(b)->dev = dev; WB(b)->interval = 1; }
NVN(nvnWindowBuilderSetDevice, void, NVNwindowBuilder* b, NVNdevice* d) { WB(b)->dev = d; }
NVN(nvnWindowBuilderSetNativeWindow, void, NVNwindowBuilder* b, NVNnativeWindow w) { WB(b)->native = w; }
// t: host pointer to the game's array of GUEST texture pointers
NVN(nvnWindowBuilderSetTextures, void, NVNwindowBuilder* b, int n, NVNtexture* const* t) {
    if (n < 0 || n > 4) { LOG("[RED] nvn: WindowBuilderSetTextures count %d", n); n = n < 0 ? 0 : 4; }
    const u64* tg = reinterpret_cast<const u64*>(t);
    WB(b)->ntex = n; for (int i = 0; i < n; i++) WB(b)->tex[i] = gp<NVNtexture>(tg[i]);
}
NVN(nvnWindowBuilderSetPresentInterval, void, NVNwindowBuilder* b, int v) { WB(b)->interval = v; }
// The host rotates through the count given at creation; logged once
NVN_NOPROTO(nvnWindowBuilderSetNumActiveTextures, void, NVNwindowBuilder* b, int n) { (void)b; ONCE_LOG("WindowBuilderSetNumActiveTextures %d (the host rotates through the SetTextures count)", n); }
NVN_NOPROTO(nvnWindowSetNumActiveTextures, void, NVNwindow* w, int n) { (void)w; ONCE_LOG("WindowSetNumActiveTextures %d (the host rotates through the SetTextures count)", n); }
NVN(nvnWindowInitialize, NVNboolean, NVNwindow* w, const NVNwindowBuilder* b) {
    const win_builder_t* s = (const win_builder_t*)b; win_t* o = (win_t*)w;
    memset(o, 0, sizeof *o); o->magic = NVN_MAGIC('W', 'I', 'N', 'D'); o->native = s->native; o->ntex = s->ntex; o->interval = s->interval;
    memcpy(o->tex, s->tex, sizeof o->tex);
    for (int i = 0; i < o->ntex; i++) if (o->tex[i]) rp_window_tex(CTX(o->tex[i]));
    LOG("nvn: WindowInitialize swap chain %d textures, interval %d, first %dx%d format %#x", o->ntex, o->interval,
        o->ntex && o->tex[0] ? (int)CTX(o->tex[0])->d.width : 0, o->ntex && o->tex[0] ? (int)CTX(o->tex[0])->d.height : 0, o->ntex && o->tex[0] ? CTX(o->tex[0])->d.format : 0);
    return 1;
}
NVN(nvnWindowFinalize, void, NVNwindow* w) { (void)w; }
NVN(nvnWindowSetDebugLabel, void, NVNwindow* w, const char* s) { (void)w; (void)s; }
NVN(nvnWindowGetPresentInterval, int, const NVNwindow* w) { return ((const win_t*)w)->interval; }   // Link's Awakening reads it back every frame
NVN(nvnWindowSetPresentInterval, void, NVNwindow* w, int v) {
    if (((win_t*)w)->interval != v) LOG("nvn: the game sets present interval %d -> %d (from %s)", ((win_t*)w)->interval, v, describe_guest(call_site()).c_str());
    ((win_t*)w)->interval = v;
}
NVN(nvnWindowSetCrop, void, NVNwindow* w, int x, int y, int ww, int hh) {
    win_t* o = (win_t*)w; int v[4] = {x, y, ww, hh};
    if (memcmp(o->crop, v, sizeof v)) LOG("nvn: WindowCrop %d,%d %dx%d", x, y, ww, hh);
    memcpy(o->crop, v, sizeof v);
}
NVN(nvnWindowGetCrop, void, const NVNwindow* w, NVNrectangle* r) {   // Bayonetta 3 reads it back every few frames; before any SetCrop it is the whole first texture, as NVN starts it
    const win_t* o = (const win_t*)w;
    if (o->crop[2] > 0 && o->crop[3] > 0) { r->x = o->crop[0]; r->y = o->crop[1]; r->width = o->crop[2]; r->height = o->crop[3]; return; }
    r->x = r->y = 0; r->width = o->ntex && o->tex[0] ? (int)CTX(o->tex[0])->d.width : 0; r->height = o->ntex && o->tex[0] ? (int)CTX(o->tex[0])->d.height : 0;
}

NVN(nvnWindowAcquireTexture, NVNwindowAcquireTextureResult, NVNwindow* w, NVNsync* s, int* idx) {
    win_t* o = (win_t*)w;
    *idx = o->ntex ? o->next++ % o->ntex : 0;
    // Vsync blocks here (real-device NVN: when all textures in the swapchain are waiting for display, wait for the next one to be released), not in present
    const uint64_t at = *idx < 4 ? o->avail[*idx] : 0;
    const uint64_t t0 = now_ns();
    if (at) sleep_until_ns(at);
    const uint64_t t1 = now_ns();
    g_acq_pace_ns += t1 - t0;
    // The previous present of this texture has not yet been processed by the renderer (display cannot keep up, a whole swapchain round behind): wait for it to be returned, then hand it to the game to draw
    const uint64_t tk = *idx < 4 ? o->ticket[*idx] : 0;
    if (tk) { HostWait hw; rp_present_wait(tk); g_acq_renderer_ns += now_ns() - t1; }
    TIMELINE("acquire %d: paced %llu us, renderer %llu us", *idx, (unsigned long long)((t1 - t0) / 1000), (unsigned long long)((now_ns() - t1) / 1000));
    if (s) { ((sync_t*)s)->fenced = 2; ((sync_t*)s)->fence = rp_last_fence(); }   // The swapchain texture becomes available once the commits before this point have completed (fenced 2: an acquire sync)
    return NVN_WINDOW_ACQUIRE_TEXTURE_RESULT_SUCCESS;
}
// The older API (NVN 53, Super Mario 3D World + Bowser's Fury): the queue waits for the texture on the GPU and no sync object is handed out. The renderer completes in submission order, so later commands already come after earlier presents;
// the same as WindowAcquireTexture without a sync object
NVN(nvnQueueAcquireTexture, NVNqueueAcquireTextureResult, NVNqueue* q, NVNwindow* w, int* idx) {
    (void)q; return h_nvnWindowAcquireTexture(w, nullptr, idx) == NVN_WINDOW_ACQUIRE_TEXTURE_RESULT_SUCCESS ? (NVNqueueAcquireTextureResult)0 : (NVNqueueAcquireTextureResult)1;
}

NVN(nvnQueuePresentTexture, void, NVNqueue* q, NVNwindow* w, int idx) {
    (void)q; win_t* o = (win_t*)w; o->presents++;
    const uint64_t period = present_period(o);
    if (idx >= 0 && idx < o->ntex && idx < 4 && o->tex[idx]) {   // enqueue only, return immediately
        const int crop[4] = {o->crop[0], o->crop[1], o->crop[2], o->crop[3]};   // the window's own crop (the vi layer crop, nn::vi SetLayerCrop, is not read here)
        o->ticket[idx] = rp_present(CTX(o->tex[idx]), (double)period / 1e9, crop);
    }
    const uint64_t f = ++g_frame;
    if (f % 600 == 0) LOG("nvn: frame %llu", (unsigned long long)f);
    // The slot this frame is shown in, and when its texture can be acquired again
    const uint64_t now = now_ns();
    note_frame(now);
    TIMELINE("present %d", idx);
    const uint64_t slot = now + pace(&o->pace, now, period);
    if (idx >= 0 && idx < 4) o->avail[idx] = period ? slot + period : 0;
}

}  // namespace nvn
