// The driver thread (vkq.h). One ring of variable-size entries {call, size} + the closure built in place, 16-byte
// aligned; an entry that does not fit before the end of the ring goes after a skip entry at its start. The producer
// owns `head` (bytes appended in all), the consumer `tail` (bytes run in all). The producer waits when the ring is
// full; the consumer spins a little when it is empty, then sleeps on `head` (WaitOnAddress). Each side makes the wake
// syscall only when the other has said it sleeps.
#include "vkq.h"

#include <windows.h>
#include <immintrin.h>

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>

#include "../../common.h"

#pragma comment(lib, "Synchronization.lib")

namespace nvk::vkq {
namespace {

constexpr size_t RingBytes = 32u << 20;   // (stage 1 appends a few entries a frame: never full in practice)
constexpr u64 SpinNs = 80'000;            // the consumer spins this long on an empty ring before it sleeps
// The producer publishes its head once this much is appended (and at every queue operation, sync and full ring), and
// the consumer its tail every 32 entries and when it has caught up: one shared-line write per entry cost the renderer
// ~10% (the other side's cache line taken back each time).
constexpr u64 BatchBytes = 8u << 10;

struct Hdr {
    detail::Call call;   // null: skip to the start of the ring
    u64 size;            // the whole entry, header included
};
static_assert(sizeof(Hdr) == 16);

u8* g_ring = nullptr;
alignas(64) std::atomic<u64> g_head{0};        // producer
alignas(64) std::atomic<u64> g_tail{0};        // consumer
alignas(64) std::atomic<u32> g_cons_sleeps{0};  // the consumer waits on g_head
alignas(64) std::atomic<u32> g_prod_sleeps{0};  // the producer waits on g_tail
u64 g_head_local = 0, g_pending = 0, g_tail_seen = 0;   // producer only: published, reserved, last tail read

void consumer_loop() {
    SetThreadDescription(GetCurrentThread(), L"NVN driver");
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
    u64 t = 0;
    for (;;) {
        u64 h = g_head.load(std::memory_order_acquire);
        if (h == t) {
            const u64 t0 = host_ns();
            int k = 0;
            while ((h = g_head.load(std::memory_order_acquire)) == t) {
                _mm_pause();
                if (++k % 64 == 0 && host_ns() - t0 > SpinNs) break;
            }
            if (h == t) {
                g_cons_sleeps.store(1, std::memory_order_seq_cst);
                h = g_head.load(std::memory_order_seq_cst);
                if (h == t) WaitOnAddress(&g_head, &t, sizeof t, INFINITE);
                g_cons_sleeps.store(0, std::memory_order_relaxed);
                continue;
            }
        }
        for (u32 n = 1; t != h; n++) {
            Hdr* e = reinterpret_cast<Hdr*>(g_ring + (t & (RingBytes - 1)));
            if (e->call) e->call(e + 1);
            t += e->size;
            if (n % 32 == 0 || t == h) {
                g_tail.store(t, std::memory_order_seq_cst);
                if (g_prod_sleeps.load(std::memory_order_seq_cst)) WakeByAddressAll(&g_tail);
            }
        }
    }
}

// The producer: until the consumer has run up to `target`.
void wait_tail(u64 target) {
    u64 t = g_tail.load(std::memory_order_acquire);
    if (t >= target) return;
    const u64 t0 = host_ns();
    int k = 0;
    while ((t = g_tail.load(std::memory_order_acquire)) < target) {
        _mm_pause();
        if (++k % 64 == 0 && host_ns() - t0 > SpinNs) break;
    }
    while ((t = g_tail.load(std::memory_order_seq_cst)) < target) {
        g_prod_sleeps.store(1, std::memory_order_seq_cst);
        t = g_tail.load(std::memory_order_seq_cst);
        if (t < target) WaitOnAddress(&g_tail, &t, sizeof t, 10);
        g_prod_sleeps.store(0, std::memory_order_relaxed);
    }
}

}  // namespace

bool on() {
    static const bool v = [] {
        if (getenv("HOST_NVN_DUMP") || getenv("HOST_NVN_DUMP_ON_FLASH") || getenv("HOST_NVN_DUMP_RING")) return false;
        const char* e = getenv("HOST_NVN_DRIVER_THREAD");
        return e ? std::strcmp(e, "0") != 0 : true;
    }();
    return v;
}

void attach() {
    if (!on()) return;
    static std::once_flag once;
    std::call_once(once, [] {
        g_ring = static_cast<u8*>(VirtualAlloc(nullptr, RingBytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        if (!g_ring) {
            LOG("[RED] nvk: driver thread: no memory for its ring; queue work stays on the renderer thread");
            return;
        }
        std::thread(consumer_loop).detach();
        detail::t_producer = true;
        LOG("nvk: queue work on the driver thread (HOST_NVN_DRIVER_THREAD=0: off)");
    });
}

void flush() {
    if (!detail::t_producer || g_pending == g_head_local) return;
    g_head_local = g_pending;
    g_head.store(g_pending, std::memory_order_seq_cst);
    if (g_cons_sleeps.load(std::memory_order_seq_cst)) WakeByAddressSingle(&g_head);
}

void sync() {
    if (!detail::t_producer) return;
    flush();
    wait_tail(g_head_local);
}

bool idle() {
    if (detail::t_producer && g_pending != g_head_local) return false;
    return g_tail.load(std::memory_order_acquire) == g_head.load(std::memory_order_acquire);
}

namespace detail {

// (from the end of what is reserved and not yet published: payloads go just before the closure that uses them)
void* reserve(size_t bytes, Call call) {
    const u64 total = sizeof(Hdr) + ((bytes + 15) & ~size_t{15});
    u64 h = g_pending;
    size_t pos = static_cast<size_t>(h & (RingBytes - 1));
    const u64 skip = pos + total > RingBytes ? RingBytes - pos : 0;
    if (h + skip + total - g_tail_seen > RingBytes) {
        g_tail_seen = g_tail.load(std::memory_order_acquire);
        if (h + skip + total - g_tail_seen > RingBytes) {
            flush();   // (everything reserved so far is complete: the consumer may run it)
            wait_tail(h + skip + total - RingBytes);
            g_tail_seen = g_tail.load(std::memory_order_acquire);
        }
    }
    if (skip) {
        Hdr* s = reinterpret_cast<Hdr*>(g_ring + pos);
        s->call = nullptr;
        s->size = skip;
        h += skip;
        pos = 0;
    }
    Hdr* e = reinterpret_cast<Hdr*>(g_ring + pos);
    e->call = call;
    e->size = total;
    g_pending = h + total;
    return e + 1;
}

void noop(void*) {}
void* payload(size_t bytes) { return reserve(bytes, &noop); }

void publish() {
    if (g_pending - g_head_local >= BatchBytes) flush();
}

}  // namespace detail
}  // namespace nvk::vkq
