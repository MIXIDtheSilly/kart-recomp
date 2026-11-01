#include "kernel.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <random>

#include "ipc.h"
#include "memory.h"

namespace kern {

KSharedMemory::KSharedMemory(u64 size_) : size((size_ + mem::PageSize - 1) & ~(mem::PageSize - 1)) {
    pages = mem::phys_alloc(size);
    if (pages.size() != 1) fatal("shared memory of %#llx bytes is not contiguous", size);
}
KSharedMemory::~KSharedMemory() { mem::phys_free(pages); }


std::mutex g_lock;

namespace {

constexpr u32 CurrentThreadHandle = 0xFFFF8000;
constexpr u32 CurrentProcessHandle = 0xFFFF8001;
constexpr u32 MutexHasWaiters = 0x40000000;

std::mutex g_handle_lock;
std::unordered_map<u32, std::shared_ptr<KObject>> g_handles;
u32 g_next_handle = 0x101;
std::atomic<u64> g_next_thread_id{0x56};
thread_local KThread* t_current = nullptr;

// Waiting threads, ordered by priority then arrival (kernel lock held).
using WaitList = std::vector<KThread*>;
std::unordered_map<u64, WaitList> g_arbiter;  // svcWaitForAddress
std::unordered_map<u64, WaitList> g_condvar;  // svcWaitProcessWideKeyAtomic
std::unordered_map<u64, WaitList> g_mutex;    // svcArbitrateLock

std::unordered_map<u32, KThread*> g_threads_by_handle;

void enqueue(WaitList& list, KThread* t) {
    auto it = std::find_if(list.begin(), list.end(), [&](KThread* o) { return o->priority > t->priority; });
    list.insert(it, t);
}
void remove(std::unordered_map<u64, WaitList>& map, u64 key, KThread* t) {
    auto it = map.find(key);
    if (it == map.end()) return;
    auto& l = it->second;
    l.erase(std::remove(l.begin(), l.end(), t), l.end());
    if (l.empty()) map.erase(it);
}

void wake(KThread* t, u32 result) {
    t->woken = true;
    t->wait_result = result;
    t->wait_kind = 0;
    t->cv.notify_one();
}

// ------------------------------------------------------------------ scheduler
// As on the console, a core runs one guest thread at a time, the highest-priority ready one, and a
// thread stays on its ideal core. Game code relies on it: a thread that signals a lower-priority
// thread on its own core keeps running until it blocks (with every thread running in parallel the
// game's workers read job data before it was written). A thread gives up its core when it blocks,
// sleeps or makes a service request; a higher-priority thread that becomes ready on a busy core
// sets the owner's ctx.halt, and the owner yields at its next preemption point.
// HOST_NO_SCHED=1 runs every thread freely in parallel instead. All of it under the kernel lock.

// The process's cores (NPDM: 0-5). A thread runs on any core of its affinity mask, preferring the
// one it last ran on, as Horizon migrates threads to idle cores they may use.
constexpr s32 NumCores = 6;
constexpr u64 ProcessCoreMask = (u64{1} << NumCores) - 1;
KThread* g_owner[NumCores] = {};
WaitList g_ready;  // threads waiting for a core: priority order, first come first served

bool scheduling() {
    static const bool on = std::getenv("HOST_NO_SCHED") == nullptr;
    return on;
}

u64 core_mask(const KThread* t) {
    const u64 m = t->affinity & ProcessCoreMask;
    return m ? m : u64{1} << std::clamp(t->core, 0, NumCores - 1);
}

bool owns_core(const KThread* t) { return t->sched_core >= 0 && g_owner[t->sched_core] == t; }

// Gives a free core to the thread if one of its cores is free (its last core first).
bool take_free_core(KThread* t) {
    const u64 mask = core_mask(t);
    if (t->sched_core >= 0 && (mask >> t->sched_core & 1) && !g_owner[t->sched_core]) {
        g_owner[t->sched_core] = t;
        return true;
    }
    for (s32 c = 0; c < NumCores; c++) {
        if ((mask >> c & 1) && !g_owner[c]) {
            g_owner[c] = t;
            t->sched_core = c;
            return true;
        }
    }
    return false;
}

// Waits until the thread owns a core. With every core of its mask busy it queues, and the owner
// with the lowest priority below its own is asked to yield.
void sched_acquire(std::unique_lock<std::mutex>& lk, KThread* t) {
    if (!scheduling() || !t) return;
    if (take_free_core(t)) return;
    enqueue(g_ready, t);
    KThread* victim = nullptr;
    const u64 mask = core_mask(t);
    for (s32 c = 0; c < NumCores; c++) {
        KThread* o = g_owner[c];
        if ((mask >> c & 1) && o && o->priority > t->priority && (!victim || o->priority > victim->priority)) victim = o;
    }
    if (victim) victim->ctx.halt = 1;
    t->cv.wait(lk, [&] { return owns_core(t); });
}

// Gives the thread's core to the best queued thread that may run on it.
void sched_release(KThread* t) {
    if (!scheduling() || !t || !owns_core(t)) return;
    const s32 c = t->sched_core;
    g_owner[c] = nullptr;
    for (auto it = g_ready.begin(); it != g_ready.end(); ++it) {
        KThread* n = *it;
        if (core_mask(n) >> c & 1) {
            g_ready.erase(it);
            g_owner[c] = n;
            n->sched_core = c;
            n->cv.notify_all();
            return;
        }
    }
}

// A queued thread that should run on the current thread's core instead of it.
bool should_yield(const KThread* t) {
    for (KThread* n : g_ready) {
        if (n->priority >= t->priority) break;
        if (core_mask(n) >> t->sched_core & 1) return true;
    }
    return false;
}

// svcSleepThread(0): gives the core to a queued thread of the same or higher priority that may run
// here, and returns at once if there is none (the game spins on yields).
void yield_core(KThread* t) {
    if (!t) return;
    std::unique_lock lk{g_lock};
    if (!owns_core(t)) return;
    bool other = false;
    for (KThread* n : g_ready) {
        if (n->priority > t->priority) break;
        if (core_mask(n) >> t->sched_core & 1) {
            other = true;
            break;
        }
    }
    if (!other) return;
    sched_release(t);
    sched_acquire(lk, t);
}

// Releases the core around host waits that aren't kernel waits (service requests, sleeps).
struct CoreReleased {
    KThread* t;
    explicit CoreReleased(KThread* t_) : t(t_) {
        std::scoped_lock lk{g_lock};
        sched_release(t);
    }
    ~CoreReleased() {
        std::unique_lock lk{g_lock};
        sched_acquire(lk, t);
    }
};

// Sleep until woken or timed out (timeout < 0: forever). Returns false on timeout. The thread's
// core goes to other threads meanwhile.
bool block(std::unique_lock<std::mutex>& lk, KThread* t, s64 timeout_ns) {
    t->woken = false;
    sched_release(t);
    bool ok = true;
    if (timeout_ns < 0) {
        t->cv.wait(lk, [&] { return t->woken; });
    } else {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::nanoseconds(timeout_ns);
        ok = t->cv.wait_until(lk, deadline, [&] { return t->woken; });
    }
    sched_acquire(lk, t);
    return ok;
}

std::atomic_ref<u32> word(u64 addr) { return std::atomic_ref<u32>(*reinterpret_cast<u32*>(gptr(addr))); }

// Hand a mutex to its highest-priority waiter (kernel lock held).
void release_mutex(u64 addr) {
    auto it = g_mutex.find(addr);
    if (it == g_mutex.end()) {
        word(addr).store(0);
        return;
    }
    KThread* next = it->second.front();
    it->second.erase(it->second.begin());
    const bool more = !it->second.empty();
    if (!more) g_mutex.erase(it);
    word(addr).store(next->mutex_tag | (more ? MutexHasWaiters : 0));
    wake(next, ResultOk);
}

// After a condition variable wakeup: take the mutex or queue on it (kernel lock held).
void acquire_after_signal(KThread* t) {
    auto w = word(t->mutex_addr);
    u32 v = w.load();
    for (;;) {
        if (v == 0) {
            if (w.compare_exchange_weak(v, t->mutex_tag)) {
                wake(t, ResultOk);
                return;
            }
        } else if (w.compare_exchange_weak(v, v | MutexHasWaiters)) {
            break;
        }
    }
    t->wait_kind = 4;
    t->wait_key = t->mutex_addr;
    enqueue(g_mutex[t->mutex_addr], t);
}

u32 handle_of_current() { return t_current ? t_current->handle : 0; }

std::shared_ptr<KThread> thread_from_handle(u32 h) {
    if (h == CurrentThreadHandle && t_current) return std::static_pointer_cast<KThread>(t_current->shared_from_this());
    return get<KThread>(h);
}

// ------------------------------------------------------------------ threads

DWORD WINAPI host_thread_main(void* param) {
    auto* t = static_cast<KThread*>(param);
    t_current = t;
    Ctx* c = &t->ctx;
    c->rt = t;
    {
        std::unique_lock lk{g_lock};
        sched_acquire(lk, t);
    }
    call_indirect(c, t->entry);
    if (c->x[30]) call_indirect(c, c->x[30]);
    fatal("guest thread %llu returned from its entry", t->id);
}

void launch(KThread* t) {
    HANDLE h = CreateThread(nullptr, 64ull << 20, host_thread_main, t, STACK_SIZE_PARAM_IS_A_RESERVATION | CREATE_SUSPENDED, nullptr);
    if (!h) fatal("CreateThread failed (%lu)", GetLastError());
    t->host_thread = h;  // kept open for dump_threads
    const std::wstring name = L"guest " + std::to_wstring(t->id);
    SetThreadDescription(h, name.c_str());
    // Experiment: HOST_ONE_CPU=1 runs every guest thread on host CPU 0.
    static const bool one_cpu = std::getenv("HOST_ONE_CPU") != nullptr;
    if (one_cpu) SetThreadAffinityMask(h, 1);
    ResumeThread(h);
}

u64 g_tls_page = 0;
u32 g_tls_used = 8;  // slots per page

}  // namespace

KThread* current_thread() { return t_current; }

u32 add_handle(std::shared_ptr<KObject> obj) {
    std::scoped_lock lk{g_handle_lock};
    const u32 h = g_next_handle++;
    g_handles[h] = std::move(obj);
    return h;
}

std::shared_ptr<KObject> get_object(u32 handle) {
    std::scoped_lock lk{g_handle_lock};
    auto it = g_handles.find(handle);
    return it == g_handles.end() ? nullptr : it->second;
}

static void close_handle(u32 handle) {
    std::shared_ptr<KObject> obj;
    {
        std::scoped_lock lk{g_handle_lock};
        auto it = g_handles.find(handle);
        if (it == g_handles.end()) return;
        obj = std::move(it->second);
        g_handles.erase(it);
    }
    // obj (and a session's service, if this was the last reference) is released here,
    // outside the handle lock.
}

void signal_event(KReadableEvent* ev) {
    std::scoped_lock lk{g_lock};
    if (ev->signaled) return;
    ev->signaled = true;
    for (KThread* t : ev->waiters) {
        if (t->wait_kind == 1 && !t->woken) {
            t->signaled_by = ev;
            wake(t, ResultOk);
        }
    }
}

void clear_event(KReadableEvent* ev) {
    std::scoped_lock lk{g_lock};
    ev->signaled = false;
}

u64 alloc_tls() {
    std::scoped_lock lk{g_lock};
    if (g_tls_used == 8) {
        g_tls_page = mem::find_free(0x85404000, mem::AslrSize, mem::PageSize);
        mem::map(g_tls_page, mem::PageSize, mem::ThreadLocal, mem::RW);
        g_tls_used = 0;
    }
    return g_tls_page + 0x200 * g_tls_used++;
}

static u64 g_exception_context = 0;

void start_main_thread(u64 entry, u64 stack_top, u64 stack_size) {
    g_exception_context = alloc_tls();  // the kernel keeps the user exception context in a TLS slot
    auto t = std::make_shared<KThread>();
    t->id = g_next_thread_id++;
    t->entry = entry;
    t->tls = alloc_tls();
    t->stack_top = stack_top;
    t->handle = add_handle(t);
    t->started = true;
    t->name = "main";
    t->ctx.sp = stack_top;
    t->ctx.tpidrro_el0 = t->tls;
    t->ctx.x[0] = 0;
    t->ctx.x[1] = t->handle;
    {
        std::scoped_lock lk{g_lock};
        g_threads_by_handle[t->handle] = t.get();
    }
    (void)stack_size;
    launch(t.get());
}

// ------------------------------------------------------------------ svc handlers

static u32 svc_wait_sync(Ctx* c) {
    const u64 handles_addr = c->x[1];
    const s32 count = static_cast<s32>(c->x[2]);
    const s64 timeout = static_cast<s64>(c->x[3]);
    KThread* self = t_current;
    if (count < 0 || count > 64) return KResult::OutOfRange;
    std::vector<std::shared_ptr<KObject>> objs;
    for (s32 i = 0; i < count; i++) {
        const u32 h = ld<u32>(handles_addr + 4 * i);
        auto o = h == CurrentThreadHandle ? self->shared_from_this() : get_object(h);
        if (!o || !o->waitable()) {
            LOG("svcWaitSynchronization: bad handle %#x (%s)", h, o ? o->type() : "none");
            return KResult::InvalidHandle;
        }
        objs.push_back(std::move(o));
    }
    std::unique_lock lk{g_lock};
    for (s32 i = 0; i < count; i++) {
        if (objs[i]->is_signaled()) {
            c->x[1] = static_cast<u64>(i);
            return ResultOk;
        }
    }
    if (timeout == 0) return KResult::TimedOut;
    if (self->wait_result == KResult::Cancelled && self->wait_kind == -1) {
        self->wait_kind = 0;
        return KResult::Cancelled;
    }
    self->wait_kind = 1;
    self->signaled_by = nullptr;
    for (auto& o : objs) o->waiters.push_back(self);
    const bool ok = block(lk, self, timeout);
    for (auto& o : objs) o->waiters.erase(std::remove(o->waiters.begin(), o->waiters.end(), self), o->waiters.end());
    self->wait_kind = 0;
    if (!ok) return KResult::TimedOut;
    if (self->wait_result != ResultOk) return self->wait_result;
    for (s32 i = 0; i < count; i++) {
        if (objs[i].get() == self->signaled_by) c->x[1] = static_cast<u64>(i);
    }
    return ResultOk;
}

static u32 svc_arbitrate_lock(Ctx* c) {
    const u32 owner = static_cast<u32>(c->x[0]);
    const u64 addr = c->x[1];
    const u32 tag = static_cast<u32>(c->x[2]);
    if (addr & 3) return KResult::InvalidAddress;
    KThread* self = t_current;
    std::unique_lock lk{g_lock};
    if (word(addr).load() != (owner | MutexHasWaiters)) return ResultOk;
    self->mutex_tag = tag;
    self->mutex_addr = addr;
    self->wait_kind = 4;
    self->wait_key = addr;
    enqueue(g_mutex[addr], self);
    block(lk, self, -1);
    return self->wait_result;
}

static u32 svc_arbitrate_unlock(Ctx* c) {
    const u64 addr = c->x[0];
    if (addr & 3) return KResult::InvalidAddress;
    std::unique_lock lk{g_lock};
    release_mutex(addr);
    return ResultOk;
}

static u32 svc_wait_key(Ctx* c) {
    const u64 addr = c->x[0], key = c->x[1];
    const u32 tag = static_cast<u32>(c->x[2]);
    const s64 timeout = static_cast<s64>(c->x[3]);
    KThread* self = t_current;
    std::unique_lock lk{g_lock};
    release_mutex(addr);
    word(key).store(1);
    self->mutex_addr = addr;
    self->mutex_tag = tag;
    self->wait_kind = 2;
    self->wait_key = key;
    enqueue(g_condvar[key], self);
    const bool ok = block(lk, self, timeout == 0 ? 0 : timeout);
    if (!ok) {
        // Timed out: still queued on the condition variable or on the mutex.
        if (self->wait_kind == 2) remove(g_condvar, key, self);
        if (self->wait_kind == 4) remove(g_mutex, addr, self);
        self->wait_kind = 0;
        return KResult::TimedOut;
    }
    return self->wait_result;
}

static u32 svc_signal_key(Ctx* c) {
    const u64 key = c->x[0];
    const s32 count = static_cast<s32>(c->x[1]);
    std::unique_lock lk{g_lock};
    auto it = g_condvar.find(key);
    for (s32 n = 0; it != g_condvar.end() && !it->second.empty() && (count <= 0 || n < count); n++) {
        KThread* t = it->second.front();
        it->second.erase(it->second.begin());
        acquire_after_signal(t);
    }
    if (it != g_condvar.end() && it->second.empty()) {
        g_condvar.erase(it);
        it = g_condvar.end();
    }
    if (it == g_condvar.end()) word(key).store(0);
    return ResultOk;
}

static u32 svc_wait_for_address(Ctx* c) {
    const u64 addr = c->x[0];
    const u32 type = static_cast<u32>(c->x[1]);
    const s64 value = static_cast<s64>(c->x[2]);
    const s64 timeout = static_cast<s64>(c->x[3]);
    KThread* self = t_current;
    std::unique_lock lk{g_lock};
    switch (type) {
    case 0:  // WaitIfLessThan
        if (static_cast<s32>(word(addr).load()) >= static_cast<s32>(value)) return KResult::InvalidState;
        break;
    case 1: {  // DecrementAndWaitIfLessThan
        u32 v = word(addr).load();
        do {
            if (static_cast<s32>(v) >= static_cast<s32>(value)) return KResult::InvalidState;
        } while (!word(addr).compare_exchange_weak(v, v - 1));
        break;
    }
    case 2:  // WaitIfEqual
        if (static_cast<s32>(word(addr).load()) != static_cast<s32>(value)) return KResult::InvalidState;
        break;
    case 3:  // WaitIfEqual64
        if (std::atomic_ref<u64>(*reinterpret_cast<u64*>(gptr(addr))).load() != static_cast<u64>(value))
            return KResult::InvalidState;
        break;
    default:
        return KResult::InvalidEnumValue;
    }
    if (timeout == 0) return KResult::TimedOut;
    self->wait_kind = 3;
    self->wait_key = addr;
    enqueue(g_arbiter[addr], self);
    if (!block(lk, self, timeout)) {
        remove(g_arbiter, addr, self);
        self->wait_kind = 0;
        return KResult::TimedOut;
    }
    return self->wait_result;
}

static u32 svc_signal_to_address(Ctx* c) {
    const u64 addr = c->x[0];
    const u32 type = static_cast<u32>(c->x[1]);
    const s32 value = static_cast<s32>(c->x[2]);
    const s32 count = static_cast<s32>(c->x[3]);
    std::unique_lock lk{g_lock};
    auto it = g_arbiter.find(addr);
    const size_t waiting = it == g_arbiter.end() ? 0 : it->second.size();
    if (type == 1 || type == 2) {
        s32 new_value = value + 1;
        if (type == 2) {
            if (count <= 0) new_value = waiting ? value - 2 : value + 1;
            else if (waiting == 0) new_value = value + 1;
            else if (waiting <= static_cast<size_t>(count)) new_value = value - 1;
            else new_value = value;
        }
        u32 expected = static_cast<u32>(value);
        if (new_value != value) {
            if (!word(addr).compare_exchange_strong(expected, static_cast<u32>(new_value))) return KResult::InvalidState;
        } else if (word(addr).load() != static_cast<u32>(value)) {
            return KResult::InvalidState;
        }
    } else if (type != 0) {
        return KResult::InvalidEnumValue;
    }
    s32 n = 0;
    while (it != g_arbiter.end() && !it->second.empty() && (count <= 0 || n < count)) {
        KThread* t = it->second.front();
        it->second.erase(it->second.begin());
        wake(t, ResultOk);
        n++;
    }
    if (it != g_arbiter.end() && it->second.empty()) g_arbiter.erase(it);
    return ResultOk;
}

static void sleep_ns(s64 ns) {
    if (ns <= 0) {
        SwitchToThread();
        return;
    }
    thread_local HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    LARGE_INTEGER due;
    due.QuadPart = -std::max<s64>(1, ns / 100);
    SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE);
    WaitForSingleObject(timer, INFINITE);
}

static u64 get_info(u32 id, u32 handle, u64 sub, u32* result) {
    *result = ResultOk;
    switch (id) {
    case 0: return 0x3f;  // core mask
    case 1: return 0xfffffffffffffff0ull;  // priority mask
    case 2: return mem::AliasBase;
    case 3: return mem::AliasSize;
    case 4: return mem::HeapBase;
    case 5: return mem::HeapSize;
    case 6: case 21: return 0x261100000ull;
    case 7: case 22: return mem::heap_size() + 0x6000000;
    case 8: return 0;  // debugger attached
    case 11: {         // random entropy
        static std::mt19937_64 rng{std::random_device{}()};
        return rng();
    }
    case 12: return mem::AslrBase;
    case 13: return mem::AslrSize;
    case 14: return mem::StackBase;
    case 15: return mem::StackSize;
    case 16: return 0x4000000;
    case 17: return 0x1000000;
    case 18: return ProgramId;
    case 20: return g_exception_context;
    case 23: return 1;    // is application
    case 24: return 0x200;  // free thread count
    case 25: return guest_ticks() / 2;  // thread tick count (approximate)
    case 28: return 0;    // alias region extra size
    case 29: return mem::Nx2Base;
    case 30: return mem::Nx2Size;
    case 31: case 32: return 0;
    case 33: return 0x1000;
    }
    LOG("svcGetInfo: unknown id %u handle %#x sub %#llx", id, handle, sub);
    *result = KResult::InvalidEnumValue;
    return 0;
}

void preempt(Ctx* c) {
    c->halt = 0;
    KThread* t = t_current;
    if (!scheduling() || !t) return;
    std::unique_lock lk{g_lock};
    if (!owns_core(t)) return;
    const bool moved = !(core_mask(t) >> t->sched_core & 1);
    if (!moved && !should_yield(t)) return;
    sched_release(t);
    sched_acquire(lk, t);
}

void dump_threads() {
    std::vector<KThread*> threads;
    std::unordered_map<KThread*, std::string> sched;
    {
        std::unique_lock lk{g_lock, std::try_to_lock};  // the crashing thread may hold it
        for (auto& [h, t] : g_threads_by_handle) threads.push_back(t);
        for (s32 core = 0; core < NumCores; core++)
            if (g_owner[core]) sched[g_owner[core]] = "owns core " + std::to_string(core);
        for (KThread* r : g_ready) sched[r] = "ready";
    }
    static const char* kinds[] = {"running", "sync", "condvar", "arbiter", "mutex"};
    for (KThread* t : threads) {
        if (!t->started || t->exited) continue;
        const int k = t->wait_kind;
        std::string where;
        if (k == 0 && t->host_thread) {
            HANDLE h = static_cast<HANDLE>(t->host_thread);
            CONTEXT ctx{};
            ctx.ContextFlags = CONTEXT_CONTROL;
            if (SuspendThread(h) != static_cast<DWORD>(-1)) {
                if (GetThreadContext(h, &ctx)) where = host_symbol(ctx.Rip);
                ResumeThread(h);
            }
        }
        LOG("thread %llu (handle %#x, prio %d, core %d) %s key %#llx lr %s %s [%s]", t->id, t->handle, t->priority,
            t->core, k >= 0 && k <= 4 ? kinds[k] : "?", t->wait_key, describe_guest(t->ctx.x[30]).c_str(),
            where.c_str(), sched.count(t) ? sched[t].c_str() : "-");
    }
}

static std::atomic<u64> g_unknown_svcs{0};

void svc(Ctx* c, u32 imm) {
    u32 r = ResultOk;
    switch (imm) {
    case 0x01: {  // SetHeapSize
        u64 addr = 0;
        r = mem::set_heap_size(c->x[1], &addr);
        c->x[1] = addr;
        break;
    }
    case 0x02:  // SetMemoryPermission
        mem::set_perm(c->x[0], c->x[1], static_cast<u32>(c->x[2]));
        break;
    case 0x03:  // SetMemoryAttribute
        mem::set_attr(c->x[0], c->x[1], static_cast<u32>(c->x[2]), static_cast<u32>(c->x[3]));
        break;
    case 0x04: {  // MapMemory(dst, src, size): dst aliases src's physical pages, src becomes inaccessible
        const u64 dst = c->x[0], src = c->x[1], size = c->x[2];
        if (!mem::is_free(dst, size)) { r = KResult::InvalidMemoryRegion; break; }
        mem::map_phys(dst, mem::phys_runs(src, size), mem::Stack, mem::RW);
        mem::set_perm(src, size, mem::None);
        mem::set_attr(src, size, mem::Locked, mem::Locked);
        break;
    }
    case 0x05: {  // UnmapMemory
        const u64 dst = c->x[0], src = c->x[1], size = c->x[2];
        mem::unmap(dst, size);
        mem::set_perm(src, size, mem::RW);
        mem::set_attr(src, size, mem::Locked, 0);
        break;
    }
    case 0x06: {  // QueryMemory
        const mem::Info i = mem::query(c->x[2]);
        const u64 out = c->x[0];
        st<u64>(out + 0x00, i.addr);
        st<u64>(out + 0x08, i.size);
        st<u32>(out + 0x10, i.state);
        st<u32>(out + 0x14, i.attr);
        st<u32>(out + 0x18, i.perm);
        st<u32>(out + 0x1c, 0);
        st<u32>(out + 0x20, 0);
        st<u32>(out + 0x24, 0);
        c->x[1] = 0;
        break;
    }
    case 0x07:  // ExitProcess
        LOG("svcExitProcess");
        ExitProcess(0);
    case 0x08: {  // CreateThread
        auto t = std::make_shared<KThread>();
        t->id = g_next_thread_id++;
        t->entry = c->x[1];
        t->stack_top = c->x[3];
        t->priority = static_cast<s32>(c->x[4]);
        // -2 (and -3 below) mean the process's ideal core: 0. GetCurrentProcessorNumber must name a
        // real core; the game indexes per-core arrays with it.
        t->core = std::max(static_cast<s32>(c->x[5]), 0);
        t->affinity = u64{1} << t->core;
        t->tls = alloc_tls();
        t->ctx.x[0] = c->x[2];
        t->ctx.sp = c->x[3];
        t->ctx.tpidrro_el0 = t->tls;
        t->handle = add_handle(t);
        {
            std::scoped_lock lk{g_lock};
            g_threads_by_handle[t->handle] = t.get();
        }
        c->x[1] = t->handle;
        break;
    }
    case 0x09: {  // StartThread
        auto t = get<KThread>(static_cast<u32>(c->x[0]));
        if (!t) { r = KResult::InvalidHandle; break; }
        if (!t->started) {
            t->started = true;
            launch(t.get());
        }
        break;
    }
    case 0x0A: {  // ExitThread
        KThread* self = t_current;
        {
            std::scoped_lock lk{g_lock};
            self->exited = true;
            sched_release(self);
            for (KThread* w : self->waiters) {
                if (w->wait_kind == 1 && !w->woken) {
                    w->signaled_by = self;
                    wake(w, ResultOk);
                }
            }
        }
        ExitThread(0);
    }
    case 0x0B: {  // SleepThread (0, -1, -2: yield)
        const s64 ns = static_cast<s64>(c->x[0]);
        if (ns <= 0 && scheduling()) {
            yield_core(t_current);
            break;
        }
        CoreReleased released{t_current};
        sleep_ns(ns);
        break;
    }
    case 0x0C: {  // GetThreadPriority
        auto t = thread_from_handle(static_cast<u32>(c->x[1]));
        if (!t) { r = KResult::InvalidHandle; break; }
        c->x[1] = static_cast<u64>(t->priority);
        break;
    }
    case 0x0D: {  // SetThreadPriority
        auto t = thread_from_handle(static_cast<u32>(c->x[0]));
        if (!t) { r = KResult::InvalidHandle; break; }
        std::scoped_lock lk{g_lock};
        t->priority = static_cast<s32>(c->x[1]);
        if (owns_core(t.get())) {
            t->ctx.halt = 1;  // re-checks the queued threads at its next preemption point
        } else if (std::find(g_ready.begin(), g_ready.end(), t.get()) != g_ready.end()) {
            g_ready.erase(std::remove(g_ready.begin(), g_ready.end(), t.get()), g_ready.end());
            enqueue(g_ready, t.get());
            for (KThread* o : g_owner)
                if (o && o->priority > t->priority && (core_mask(t.get()) >> o->sched_core & 1)) o->ctx.halt = 1;
        }
        break;
    }
    case 0x0E: {  // GetThreadCoreMask
        auto t = thread_from_handle(static_cast<u32>(c->x[2]));
        if (!t) { r = KResult::InvalidHandle; break; }
        c->x[1] = static_cast<u64>(t->core);
        c->x[2] = t->affinity;
        break;
    }
    case 0x0F: {  // SetThreadCoreMask
        auto t = thread_from_handle(static_cast<u32>(c->x[0]));
        if (!t) { r = KResult::InvalidHandle; break; }
        const s32 core = static_cast<s32>(c->x[1]);
        const s32 old = t->core;
        if (core >= 0) t->core = core;
        else if (core == -3) t->core = 0;  // the process's ideal core (-1 leaves it unchanged)
        if (c->x[2]) t->affinity = c->x[2];
        (void)old;
        if (t.get() == t_current) {
            // May now run elsewhere (or no longer here): requeue with the new mask.
            std::unique_lock lk{g_lock};
            sched_release(t.get());
            sched_acquire(lk, t.get());
        } else {
            t->ctx.halt = 1;  // re-checks its mask at its next preemption point
        }
        break;
    }
    case 0x10:  // GetCurrentProcessorNumber
        c->x[0] = static_cast<u64>(t_current ? std::max(t_current->sched_core, 0) : 0);
        return;
    case 0x11: {  // SignalEvent
        auto e = get<KEvent>(static_cast<u32>(c->x[0]));
        if (!e) { r = KResult::InvalidHandle; break; }
        signal_event(e->readable.get());
        break;
    }
    case 0x12:    // ClearEvent
    case 0x17: {  // ResetSignal
        const u32 h = static_cast<u32>(c->x[0]);
        KReadableEvent* ev = nullptr;
        auto w = get<KEvent>(h);
        auto rd = get<KReadableEvent>(h);
        if (w) ev = w->readable.get();
        else ev = rd.get();
        if (!ev) { r = KResult::InvalidHandle; break; }
        std::scoped_lock lk{g_lock};
        if (imm == 0x17 && !ev->signaled) r = KResult::InvalidState;
        ev->signaled = false;
        break;
    }
    case 0x13: {  // MapSharedMemory
        auto s = get<KSharedMemory>(static_cast<u32>(c->x[0]));
        if (!s) { r = KResult::InvalidHandle; break; }
        const u64 addr = c->x[1], size = c->x[2];
        if (!mem::is_free(addr, size)) { r = KResult::InvalidMemoryRegion; break; }
        if (size != s->size) { r = KResult::InvalidSize; break; }
        mem::map_phys(addr, s->pages, mem::Shared, static_cast<u32>(c->x[3]));
        break;
    }
    case 0x14: {  // UnmapSharedMemory
        auto s = get<KSharedMemory>(static_cast<u32>(c->x[0]));
        if (!s) { r = KResult::InvalidHandle; break; }
        mem::unmap(c->x[1], c->x[2]);
        break;
    }
    case 0x15: {  // CreateTransferMemory
        auto tm = std::make_shared<KTransferMemory>();
        tm->addr = c->x[1];
        tm->size = c->x[2];
        tm->perm = static_cast<u32>(c->x[3]);
        mem::set_perm(tm->addr, tm->size, tm->perm);
        c->x[1] = add_handle(tm);
        break;
    }
    case 0x16:  // CloseHandle
        if (auto tm = get<KTransferMemory>(static_cast<u32>(c->x[0]))) mem::set_perm(tm->addr, tm->size, mem::RW);
        close_handle(static_cast<u32>(c->x[0]));
        break;
    case 0x18:
        r = svc_wait_sync(c);
        break;
    case 0x19: {  // CancelSynchronization
        auto t = thread_from_handle(static_cast<u32>(c->x[0]));
        if (!t) { r = KResult::InvalidHandle; break; }
        std::scoped_lock lk{g_lock};
        if (t->wait_kind == 1) wake(t.get(), KResult::Cancelled);
        else { t->wait_kind = -1; t->wait_result = KResult::Cancelled; }
        break;
    }
    case 0x1A: r = svc_arbitrate_lock(c); break;
    case 0x1B: r = svc_arbitrate_unlock(c); break;
    case 0x1C: r = svc_wait_key(c); break;
    case 0x1D: r = svc_signal_key(c); break;
    case 0x1E:  // GetSystemTick
        c->x[0] = guest_ticks();
        return;
    case 0x1F: {  // ConnectToNamedPort
        const std::string name = read_cstr(c->x[1], 12);
        u32 h = 0;
        r = ipc::connect_named_port(name, &h);
        c->x[1] = h;
        break;
    }
    case 0x21: {  // SendSyncRequest: the core is free while the service works
        CoreReleased released{t_current};
        r = ipc::send_sync_request(static_cast<u32>(c->x[0]), c->tpidrro_el0);
        break;
    }
    case 0x22: {  // SendSyncRequestWithUserBuffer
        CoreReleased released{t_current};
        r = ipc::send_sync_request(static_cast<u32>(c->x[2]), c->x[0]);
        break;
    }
    case 0x25: {  // GetThreadId
        auto t = thread_from_handle(static_cast<u32>(c->x[1]));
        if (!t) { r = KResult::InvalidHandle; break; }
        c->x[1] = t->id;
        break;
    }
    case 0x26: {  // Break
        LOG("svcBreak(reason %#llx, info %#llx, size %#llx) on thread %llu", c->x[0], c->x[1], c->x[2],
            t_current ? t_current->id : 0);
        if (c->x[2] == 4) LOG("  info word: %#x", ld<u32>(c->x[1]));
        dump_guest_stack(c);
        if (c->x[0] & 0x80000000) break;  // notification only
        fatal("guest called svcBreak");
    }
    case 0x27:  // OutputDebugString
        LOG("guest: %s", std::string(reinterpret_cast<char*>(gptr(c->x[0])), c->x[1]).c_str());
        break;
    case 0x29: {  // GetInfo
        u32 res;
        c->x[1] = get_info(static_cast<u32>(c->x[1]), static_cast<u32>(c->x[2]), c->x[3], &res);
        r = res;
        break;
    }
    case 0x2C: {  // MapPhysicalMemory: back every unmapped page of the range
        const u64 addr = c->x[0], size = c->x[1];
        for (u64 a = addr; a < addr + size;) {
            const mem::Info i = mem::query(a);
            const u64 end = std::min(i.addr + i.size, addr + size);
            if (i.state == mem::Free) mem::map(a, end - a, mem::Normal, mem::RW);
            a = end;
        }
        break;
    }
    case 0x2D: {  // UnmapPhysicalMemory
        mem::unmap(c->x[0], c->x[1]);
        break;
    }
    case 0x34: r = svc_wait_for_address(c); break;
    case 0x35: r = svc_signal_to_address(c); break;
    case 0x36: break;  // SynchronizePreemptionState
    case 0x45: {  // CreateEvent
        auto e = std::make_shared<KEvent>();
        c->x[1] = add_handle(e);
        c->x[2] = add_handle(e->readable);
        break;
    }
    case 0x80:  // SetGpuMemoryAttribute (Switch 2)
        mem::set_attr(c->x[0], c->x[1], static_cast<u32>(c->x[2]), static_cast<u32>(c->x[3]));
        break;
    default:
        if (g_unknown_svcs++ < 64)
            LOG("unimplemented svc %#x x0=%#llx x1=%#llx x2=%#llx (%s)", imm, c->x[0], c->x[1], c->x[2],
                describe_guest(c->x[30]).c_str());
        r = KResult::NotImplemented;
        break;
    }
    c->x[0] = r;
    if (c->halt) preempt(c);  // a higher-priority thread became ready while this one was in the kernel
}

}  // namespace kern
