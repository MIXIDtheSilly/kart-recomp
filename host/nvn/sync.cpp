// Native NVN: sync objects and events.
//
// A sync is a fence value of the renderer ("everything submitted to the queue so far has completed"); waiting on it goes through rp_wait inside
// HostWait. Its visible effects obey the completion rule in replay.h: a fenced sync counts as signalled only after the GPU has completed the work.
#include "nvn_int.h"

namespace nvn {

NVN(nvnSyncInitialize, NVNboolean, NVNsync *s, NVNdevice *d) { (void)d; *(sync_t *)s = sync_t{NVN_MAGIC('S', 'Y', 'N', 'C'), 0, 0}; return 1; }
NVN(nvnSyncFinalize, void, NVNsync *s) { ((sync_t *)s)->magic = 0; }
NVN(nvnSyncSetDebugLabel, void, NVNsync *s, const char *l) { (void)s; (void)l; }
// Fence = "all commands previously submitted to the queue have completed": record the fence value of the most recent submit
NVN(nvnQueueFenceSync, void, NVNqueue *q, NVNsync *s, NVNsyncCondition c, int f) { (void)q; (void)c; (void)f; ((sync_t *)s)->fenced = 1; ((sync_t *)s)->fence = rp_last_fence(); }
// A sync object that was never fenced waits until timeout, as on a real device; a fenced one is released only after the GPU has actually completed up to that fence value
// A GPU-side wait on a sync: the renderer executes submissions in order on one queue, so commands before the fence are always done first and nothing needs waiting. A sync not fenced yet (fenced later by another thread) is not waited for here; logged once
NVN(nvnQueueWaitSync, NVNboolean, NVNqueue *q, const NVNsync *s) { (void)q; static int once; if (!((const sync_t *)s)->fenced && !once++) LOG("nvn: QueueWaitSync: the sync object has no fence yet, the host does not wait"); return 1; }
// A sync object that was never fenced waits until the timeout, as on a real device; a fenced one is released only after the GPU has actually completed up to that fence value
NVN(nvnSyncWait, NVNsyncWaitResult, const NVNsync* s, uint64_t timeout) {
    const sync_t* o = (const sync_t*)s;
    if (!o->fenced) return NVN_SYNC_WAIT_RESULT_TIMEOUT_EXPIRED;
    bool (*w)(uint64_t, uint64_t) = o->fenced == 2 ? rp_wait_acq : rp_wait;
    const uint64_t fence = o->fence;
    if (w(fence, 0)) return NVN_SYNC_WAIT_RESULT_ALREADY_SIGNALED;
    HostWait hw;   // the guest thread's core runs other threads meanwhile
    return w(fence, timeout) ? NVN_SYNC_WAIT_RESULT_CONDITION_SATISFIED : NVN_SYNC_WAIT_RESULT_TIMEOUT_EXPIRED;
}
NVN(nvnEventBuilderSetDefaults, void, NVNeventBuilder *b) { memset(b, 0, sizeof(ev_builder_t)); }
NVN(nvnEventBuilderSetStorage, void, NVNeventBuilder *b, const NVNmemoryPool *p, int64_t off) { *(ev_builder_t *)b = ev_builder_t{(const pool_t *)p, off}; }
NVN(nvnEventInitialize, NVNboolean, NVNevent *e, const NVNeventBuilder *b) {
    const ev_builder_t* s = (const ev_builder_t*)b;
    if (!s->pool || !s->pool->mem) { LOG("[RED] nvn: EventInitialize has no storage"); return 0; }
    *(ev_t*)e = ev_t{NVN_MAGIC('E', 'V', 'N', 'T'), (volatile uint32_t*)(s->pool->mem + s->off)}; return 1;
}
NVN(nvnEventFinalize, void, NVNevent *e) { ((ev_t *)e)->magic = 0; }
NVN(nvnEventGetValue, uint32_t, const NVNevent *e) { return *((const ev_t *)e)->val; }
NVN(nvnEventSignal, void, NVNevent* e, NVNeventSignalMode m, uint32_t v) { event_apply(((const ev_t*)e)->val, m, v); }
// The storage set with SetStorage (EA SPORTS FC 27's Frostbite renderer reads it back)
NVN_NOPROTO(nvnEventBuilderGetMemoryPool, NVNmemoryPool*, const NVNeventBuilder* b) { return (NVNmemoryPool*)((const ev_builder_t*)b)->pool; }
NVN_NOPROTO(nvnEventBuilderGetMemoryOffset, int64_t, const NVNeventBuilder* b) { return ((const ev_builder_t*)b)->off; }

}  // namespace nvn
