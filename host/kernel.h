// Native kernel: guest threads are host threads; synchronization objects and the address
// arbiter run under one kernel lock with a condition variable per waiting thread.
#pragma once
#include <condition_variable>
#include <functional>
#include <mutex>
#include <unordered_map>

#include "common.h"
#include "memory.h"

namespace kern {

struct KThread;

struct KObject : std::enable_shared_from_this<KObject> {
    virtual ~KObject() = default;
    virtual const char* type() const = 0;
    // Synchronization objects (threads, readable events) override these.
    virtual bool is_signaled() const { return false; }
    virtual bool waitable() const { return false; }
    std::vector<KThread*> waiters;  // threads in svcWaitSynchronization on this object
};

struct KThread : KObject {
    static constexpr const char* Type = "thread";
    const char* type() const override { return Type; }
    bool is_signaled() const override { return exited; }
    bool waitable() const override { return true; }

    Ctx ctx{};
    u64 id = 0;
    u32 handle = 0;  // the handle the creator received (used as the mutex owner tag)
    u64 entry = 0, tls = 0, stack_top = 0;
    s32 priority = 0x2c;
    s32 core = 0;        // ideal core: the core the thread runs on
    s32 sched_core = -1; // the core it owns or is queued for (scheduler, kernel lock)
    u64 affinity = 1;
    bool started = false, exited = false;
    std::string name;
    void* host_thread = nullptr;  // the host thread running it (for thread dumps)

    // Wait state (kernel lock held).
    std::condition_variable cv;
    bool woken = false;
    u32 wait_result = 0;
    KObject* signaled_by = nullptr;
    u64 wait_key = 0;     // condition variable key or arbiter address
    u64 mutex_addr = 0;   // mutex to re-acquire after a condition variable wakeup
    u32 mutex_tag = 0;
    int wait_kind = 0;    // 0 none, 1 sync objects, 2 cond var, 3 arbiter, 4 mutex
};

struct KReadableEvent : KObject {
    static constexpr const char* Type = "event";
    const char* type() const override { return Type; }
    bool is_signaled() const override { return signaled; }
    bool waitable() const override { return true; }
    bool signaled = false;
};

struct KEvent : KObject {
    static constexpr const char* Type = "writable event";
    const char* type() const override { return Type; }
    std::shared_ptr<KReadableEvent> readable = std::make_shared<KReadableEvent>();
};

// Memory a service shares with the guest: physical pages the guest maps into its view.
struct KSharedMemory : KObject {
    static constexpr const char* Type = "shared memory";
    const char* type() const override { return Type; }
    explicit KSharedMemory(u64 size_);
    ~KSharedMemory() override;
    u64 size;
    std::vector<mem::PhysRun> pages;  // one run (allocated whole)
    u8* data() { return mem::phys_ptr(pages[0].phys); }
};

struct KTransferMemory : KObject {
    static constexpr const char* Type = "transfer memory";
    const char* type() const override { return Type; }
    u64 addr = 0, size = 0;
    u32 perm = 0;
};

// ------------------------------------------------------------------ API used by services

extern std::mutex g_lock;
KThread* current_thread();
u32 add_handle(std::shared_ptr<KObject> obj);
std::shared_ptr<KObject> get_object(u32 handle);
// The object behind a handle if it is a T (built without RTTI: each type names itself).
template <class T> std::shared_ptr<T> get(u32 handle) {
    auto o = get_object(handle);
    return o && std::strcmp(o->type(), T::Type) == 0 ? std::static_pointer_cast<T>(o) : nullptr;
}

void signal_event(KReadableEvent* ev);  // takes the kernel lock
void clear_event(KReadableEvent* ev);

// Event owned by a service: the guest gets copy handles to the readable side.
struct ServiceEvent {
    std::shared_ptr<KEvent> ev = std::make_shared<KEvent>();
    void signal() { signal_event(ev->readable.get()); }
    void clear() { clear_event(ev->readable.get()); }
    u32 new_handle() { return add_handle(ev->readable); }
};

// Process startup.
void start_main_thread(u64 entry, u64 stack_top, u64 stack_size);
u64 alloc_tls();
void svc(Ctx* c, u32 imm);

// A preemption point (the recompiled code polls ctx.halt at loop back-edges): gives the core to a
// higher-priority thread that became ready on it.
void preempt(Ctx* c);

// Logs every guest thread: its wait, and for running threads the host function it is in.
void dump_threads();

}  // namespace kern
