// Native NVN: queues and submission.
//
// A submission runs under g_sub_mu, one at a time: the renderer keeps one submission in progress and per-queue binding state. NVN orders work on different queues only
// through syncs and events: submissions replay in lock order except where their event waits say otherwise (vk/replay_vk.cpp), which is one valid order (Fire Emblem: Three Houses initializes two queues and submits on both from two threads).
#include "nvn_int.h"

#include <optional>

namespace nvn {

std::mutex g_sub_mu;

#define QB(b) ((queue_builder_t*)(b))
NVN(nvnQueueBuilderSetDefaults, void, NVNqueueBuilder *b) {
    NVNdevice *dev = QB(b)->dev; memset(b, 0, sizeof(queue_builder_t)); QB(b)->dev = dev;
    QB(b)->cmd_sz = 65536; QB(b)->comp_sz = 262144; QB(b)->ctl_sz = 16384; QB(b)->flush = 4096;   // Device defaults
}
NVN(nvnQueueBuilderSetDevice, void, NVNqueueBuilder *b, NVNdevice *d) { QB(b)->dev = d; }
NVN(nvnQueueBuilderSetFlags, void, NVNqueueBuilder *b, int f) { QB(b)->flags = f; }
NVN(nvnQueueBuilderSetCommandMemorySize, void, NVNqueueBuilder *b, size_t n) { QB(b)->cmd_sz = n; }
NVN(nvnQueueBuilderSetComputeMemorySize, void, NVNqueueBuilder *b, size_t n) { QB(b)->comp_sz = n; }
NVN(nvnQueueBuilderSetControlMemorySize, void, NVNqueueBuilder *b, size_t n) { QB(b)->ctl_sz = n; }
NVN(nvnQueueBuilderSetCommandFlushThreshold, void, NVNqueueBuilder *b, size_t n) { QB(b)->flush = n; }
// Queue memory: the sum of the command + compute + control blocks rounded to 4 KiB (the host does not use it; the game only needs to allocate this size)
NVN(nvnQueueBuilderGetQueueMemorySize, size_t, const NVNqueueBuilder *b) {
    const queue_builder_t *o = (const queue_builder_t *)b;
    return (o->cmd_sz + o->comp_sz + o->ctl_sz + 4095) & ~(size_t)4095;
}
NVN(nvnQueueBuilderSetQueueMemory, void, NVNqueueBuilder *b, void *m, size_t n) { QB(b)->mem = m; QB(b)->mem_sz = n; }
NVN(nvnQueueInitialize, NVNboolean, NVNqueue *q, const NVNqueueBuilder *b) {
    NVNdevice *d = ((const queue_builder_t *)b)->dev;
    // NVN emits SET_WINDOW_ORIGIN / FLIP_Y into the queue's initial GPU state.
    // Changing the device origin later changes viewport construction, but does
    // not rewrite that queue state (Xenoblade 2 creates its queue first).
    *(queue_t *)q = queue_t{NVN_MAGIC('Q', 'U', 'E', 'U'), d, 0,
                            !d || ((const dev_t_ *)d)->origin == NVN_WINDOW_ORIGIN_MODE_LOWER_LEFT, new_serial()};
    OLOG("QueueInitialize %p", (void *)q); return 1;
}
NVN(nvnQueueFinalize, void, NVNqueue *q) {
    { HostWait hw; std::scoped_lock lk{g_sub_mu}; rp_queue_forget(((queue_t *)q)->serial); ((queue_t *)q)->magic = 0; }
}
NVN(nvnQueueGetError, NVNqueueGetErrorResult, NVNqueue *q, NVNqueueErrorInfo *info) {
    (void)q; if (info) memset(info, 0, sizeof *info); return NVN_QUEUE_GET_ERROR_RESULT_GPU_NO_ERROR;
}
NVN(nvnQueueFlush, void, NVNqueue* q) { (void)q; }
// nvnQueueFinish waits for the GPU work submitted so far, not for the display (waiting for queued presents made every call cost one or two display frames:
// Fire Emblem: Three Houses' loader submits a copy and calls QueueFinish for each resource it uploads). A swapchain texture is still not handed back
// before its previous present is processed (AcquireTexture waits for that ticket).
NVN(nvnQueueFinish, void, NVNqueue* q) { (void)q; HostWait hw; rp_finish(); }
// Queue priority: one renderer queue executes every NVN queue's submissions
NVN_NOPROTO(nvnQueueBuilderSetQueuePriority, void, NVNqueueBuilder* b, int prio) { (void)b; (void)prio; }
// A queue NVN2 ties to a primary queue: submissions of every queue are replayed in order, so the tie needs no work
NVN_NOPROTO(nvnQueueBuilderSetPrimaryQueue, void, NVNqueueBuilder* b, NVNqueue* primary) { (void)b; (void)primary; }
NVN(nvnQueueSetDebugLabel, void, NVNqueue* q, const char* s) { (void)q; (void)s; }

// handles: guest values as returned by EndRecording / given to CallCommands (ga() of the first record).
// The submission lock comes first and is taken inside HostWait: another thread's submission (or the one-time prepare step, which loads shaders and may take minutes) must
// not hold this thread's emulated core.
// The replay itself (recording the Vulkan commands: milliseconds a frame) runs with the core given away too, so the game's other threads on
// this core go on meanwhile; HOST_NVN_SUBMIT_KEEPS_CORE=1 keeps the core during it (to compare).
NVN(nvnQueueSubmitCommands, void, NVNqueue* q, int n, const NVNcommandHandle* h) {
    static const bool keep_core = getenv("HOST_NVN_SUBMIT_KEEPS_CORE") != nullptr;
    std::optional<HostWait> away;
    if (!keep_core) away.emplace();
    std::unique_lock lk{g_sub_mu, std::try_to_lock};
    if (!lk.owns_lock()) { std::optional<HostWait> hw; if (!away) hw.emplace(); lk.lock(); }
    { std::optional<HostWait> hw; if (!away) hw.emplace(); rp_prepare(); }
    queue_t* o = (queue_t*)q;
    o->submits++;
    rp_queue_activate(o->serial);
    rp_set_queue_origin_lower_left(o->origin_ll);
    rp_sub_begin();
    for (int i = 0; i < n; i++) {
        if (!h[i]) { ONCE_RED("SubmitCommands: null command handle"); continue; }
        rp_exec_rest(h + i + 1, n - i - 1); rp_exec(static_cast<const uint8_t*>(hp(h[i])));
    }
    rp_exec_rest(nullptr, 0);
    rp_sub_commit();
    // HOST_NVN_SERIAL=1 (testing): every submission is waited for on the GPU before the game goes on, so no CPU work overlaps GPU work.
    static const bool serial = getenv("HOST_NVN_SERIAL") != nullptr;
    if (serial) { std::optional<HostWait> hw; if (!away) hw.emplace(); rp_finish(); }
}

}  // namespace nvn