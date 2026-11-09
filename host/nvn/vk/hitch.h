// Where the renderer thread's time goes, by kind, so that a step of it that runs long (the game waits on its fences)
// can be told apart: the renderer loop logs the kinds a long step spent its time in (replay_async.cpp).
#pragma once
#include "../../common.h"

namespace nvk {

enum Hitch { HI_PIPELINE, HI_SHADER, HI_UPLOAD_WAIT, HI_UPLOAD, HI_SPARSE, HI_IMAGE, HI_SUBMIT, HI_PRESENT, HI_IMPORT, HI_N };
inline const char* const HitchNames[HI_N] = {"pipelines", "shaders", "ASTC waits", "uploads", "sparse tiles", "image creation", "submits", "present", "imports"};
extern u64 g_hitch_ns[HI_N];          // ns in all, by kind, on the renderer thread
inline thread_local bool t_hitch = false;   // this thread is the renderer (counted)

struct HitchTimer {
    Hitch h;
    u64 t0;
    explicit HitchTimer(Hitch h) : h(h), t0(host_ns()) {}
    ~HitchTimer() {
        if (t_hitch) g_hitch_ns[h] += host_ns() - t0;
    }
};

}  // namespace nvk
