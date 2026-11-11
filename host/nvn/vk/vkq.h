// The renderer's queue work on a thread of its own ("NVN driver"). Mid-race the renderer thread is the one that limits
// the frame rate, and a good part of it is the driver inside vkQueueSubmit / vkQueueBindSparse / present. Moving only
// the present to another thread gained nothing: the driver serialises present with submit, so the renderer then
// waited on the queue instead. So every queue operation of the renderer goes to one other thread, in the order the
// renderer made them: the renderer (the single producer) appends closures to a ring, the driver thread (the single
// consumer) runs them in order.
//
// HOST_NVN_DRIVER_THREAD=0: off (everything on the renderer thread, as before). Also off with the dump modes
// (HOST_NVN_DUMP, HOST_NVN_DUMP_ON_FLASH, HOST_NVN_DUMP_RING), which read back per submission and per present, and
// with HOST_NVN_SYNC_REPLAY=1 (no renderer thread: nothing attaches).
#pragma once
#include <cstddef>
#include <new>
#include <type_traits>
#include <utility>

namespace nvk::vkq {

bool on();       // the switch (read once)
void attach();   // the calling thread (the renderer) becomes the producer; starts the driver thread
void flush();    // on the producer: what is appended may run now (run() publishes in batches; run_now() flushes)
void sync();     // on the producer: back once the driver thread has run everything appended so far (elsewhere: nothing)
bool idle();     // nothing appended is still waiting to run

namespace detail {
inline thread_local bool t_producer = false;
using Call = void (*)(void*);
void* reserve(size_t bytes, Call call);   // room for a closure of `bytes` in the ring, 16-byte aligned
void publish();                           // the reserved entry is complete (published to the driver thread in batches)
void* payload(size_t bytes);              // room in the ring for data the next closure points to (valid until it has run)
template <class T> void thunk(void* p) {
    T* f = static_cast<T*>(p);
    (*f)();
    f->~T();
}
}  // namespace detail

inline bool producer() { return detail::t_producer; }

// With the driver thread on, command recording goes to it too (vkq_cmd.cpp); HOST_NVN_CMD_PROXY=0: not.
bool cmd_proxy_on();
void install_cmd_proxy();   // after vkfn::load_device: the vkCmd* pointers become encoders that append to the ring

// A copy of n elements of p in the ring (null for none), for a closure appended right after.
template <class T> T* copy(const T* p, size_t n) {
    if (!p || !n) return nullptr;
    T* d = static_cast<T*>(detail::payload(sizeof(T) * n));
    for (size_t i = 0; i < n; i++) d[i] = p[i];
    return d;
}

// f() on the driver thread after everything appended before it; at once when the caller is not the producer.
template <class F> void run(F&& f) {
    using T = std::decay_t<F>;
    static_assert(alignof(T) <= 16, "closure over-aligned for the ring");
    if (!detail::t_producer) {
        f();
        return;
    }
    void* p = detail::reserve(sizeof(T), &detail::thunk<T>);
    new (p) T(std::forward<F>(f));
    detail::publish();
}

// run(f), published at once: for queue operations (a batch would wait for more appends that may not come soon).
template <class F> void run_now(F&& f) {
    run(std::forward<F>(f));
    flush();
}

}  // namespace nvk::vkq
