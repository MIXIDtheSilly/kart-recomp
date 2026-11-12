#include "syncpoints.h"

#include <array>
#include <atomic>
#include <mutex>
#include <vector>

namespace sync {
namespace {

struct Action {
    u32 value;
    Handle handle;
    std::function<void()> f;
};

struct Syncpoint {
    std::atomic<u32> value{0};
    std::mutex m;
    std::vector<Action> actions;
};

std::array<Syncpoint, Count> g_syncpoints;
std::atomic<Handle> g_next_handle{1};
std::mutex g_alloc_lock;
std::array<bool, Count> g_allocated{};
constexpr u32 FirstOwn = Count / 2;   // below: the ones the game names itself

// Reached: the counter wraps, so compare by distance
bool reached(u32 now, u32 value) { return static_cast<s32>(now - value) >= 0; }

}  // namespace

u32 value(u32 id) { return id < Count ? g_syncpoints[id].value.load(std::memory_order_acquire) : 0; }

bool ready(u32 id, u32 v) { return id >= Count || reached(value(id), v); }

void increment(u32 id) {
    if (id >= Count) return;
    Syncpoint& s = g_syncpoints[id];
    std::vector<Action> due;
    {
        std::scoped_lock lk{s.m};
        const u32 now = s.value.fetch_add(1, std::memory_order_acq_rel) + 1;
        for (size_t i = 0; i < s.actions.size();) {
            if (reached(now, s.actions[i].value)) {
                due.push_back(std::move(s.actions[i]));
                s.actions[i] = std::move(s.actions.back());
                s.actions.pop_back();
            } else {
                i++;
            }
        }
    }
    for (auto& a : due) a.f();
}

Handle on_reach(u32 id, u32 v, std::function<void()> f) {
    if (id >= Count) {
        f();
        return 0;
    }
    Syncpoint& s = g_syncpoints[id];
    {
        std::scoped_lock lk{s.m};
        if (!reached(s.value.load(std::memory_order_acquire), v)) {
            const Handle h = g_next_handle.fetch_add(1);
            s.actions.push_back({v, h, std::move(f)});
            return h;
        }
    }
    f();
    return 0;
}

void cancel(u32 id, Handle h) {
    if (id >= Count || !h) return;
    Syncpoint& s = g_syncpoints[id];
    std::scoped_lock lk{s.m};
    for (size_t i = 0; i < s.actions.size(); i++) {
        if (s.actions[i].handle == h) {
            s.actions[i] = std::move(s.actions.back());
            s.actions.pop_back();
            return;
        }
    }
}

u32 allocate() {
    std::scoped_lock lk{g_alloc_lock};
    for (u32 id = FirstOwn; id < Count; id++) {
        if (!g_allocated[id]) {
            g_allocated[id] = true;
            return id;
        }
    }
    fatal("no free syncpoint");
}

void release(u32 id) {
    std::scoped_lock lk{g_alloc_lock};
    if (id < Count) g_allocated[id] = false;
}

}  // namespace sync
