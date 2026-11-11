// Native NVN: the game's NVN lookups (nvnBootstrapLoader, nvnDeviceGetProcAddress) resolve to the
// implementations registered with the NVN macro; names without one resolve to a stub that logs its
// first call. HOST_NVN=native turns it on (nvn::install); the sdk's NVN driver then never runs.
#include <algorithm>
#include <atomic>
#include <deque>
#include <mutex>
#include <set>
#include <thread>
#include <unordered_map>
#include <vector>

#include "../graphics.h"
#include "../host.h"
#include "abi.h"
#include "nvn_host.h"

namespace nvn {
namespace {

std::vector<Impl>& impls() {
    static std::vector<Impl> v;
    return v;
}

struct Entry {
    std::string name;
    Adapter fn = nullptr;
    u32 index = 0;  // into the per-thread call counts
    u64 reported = 0;
};
std::mutex g_lock;
std::deque<Entry> g_entries;
std::unordered_map<std::string, u64> g_by_name;  // name -> guest-callable address
thread_local Ctx* t_ctx = nullptr;

// Call counts for the report, one block per calling thread: the game's job threads make millions of
// NVN calls a second between them, and one shared counter per function bounced its cache line
// between their cores on every call. Only the owning thread writes a block (relaxed load + store);
// the report sums them.
constexpr u32 MaxEntries = 4096;  // as many as there are native thunks
struct Counts {
    std::atomic<u64> n[MaxEntries];
};
std::vector<Counts*> g_counts;  // under g_lock; blocks live as long as the process
thread_local Counts* t_counts = nullptr;

Counts* thread_counts() {
    auto* b = new Counts{};
    std::scoped_lock lk{g_lock};
    g_counts.push_back(b);
    return t_counts = b;
}

void report_loop() {
    for (;;) {
        std::this_thread::sleep_for(std::chrono::seconds(10));
        std::vector<std::pair<u64, const Entry*>> rows;
        u64 total = 0;
        {
            std::scoped_lock lk{g_lock};
            for (Entry& e : g_entries) {
                u64 n = 0;
                for (const Counts* b : g_counts) n += b->n[e.index].load(std::memory_order_relaxed);
                if (n != e.reported) rows.emplace_back(n - e.reported, &e);
                total += n - e.reported;
                e.reported = n;
            }
        }
        std::sort(rows.begin(), rows.end(), [](auto& a, auto& b) { return a.first > b.first; });
        std::string line;
        for (size_t i = 0; i < rows.size() && i < 24; i++) {
            char buf[160];
            snprintf(buf, sizeof buf, " %s:%llu", rows[i].second->name.c_str() + 3, rows[i].first / 10);
            line += buf;
        }
        LOG("nvn: %llu calls/s;%s", total / 10, line.c_str());
    }
}

}  // namespace

int register_impl(const char* name, Adapter fn) {
    impls().push_back({name, fn});
    return 0;
}

Ctx* guest_ctx() { return t_ctx; }

u64 call_guest(u64 fn, std::initializer_list<u64> args) {
    if (!t_ctx) fatal("nvn: guest callback outside an NVN call");
    return gfx::guest_call(t_ctx, fn, args);
}

u64 lookup(const std::string& name) {
    std::scoped_lock lk{g_lock};
    if (auto it = g_by_name.find(name); it != g_by_name.end()) return it->second;
    static const std::unordered_map<std::string, Adapter> table = [] {
        std::unordered_map<std::string, Adapter> t;
        for (const Impl& i : impls()) t.emplace(i.name, i.fn);
        return t;
    }();
    if (g_entries.size() >= MaxEntries) fatal("nvn: too many functions");
    Entry* e = &g_entries.emplace_back();
    e->name = name;
    e->index = static_cast<u32>(g_entries.size() - 1);
    if (auto it = table.find(name); it != table.end()) e->fn = it->second;
    const u64 addr = gfx::native_function("nvn:" + name, [e](Ctx* c, const std::string&) {
        std::atomic<u64>& n = (t_counts ? t_counts : thread_counts())->n[e->index];
        n.store(n.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
        if (!e->fn) {
            static std::mutex m;
            static std::set<std::string> seen;
            std::scoped_lock lk{m};
            if (seen.insert(e->name).second)
                LOG("[RED] nvn: %s not implemented (x0=%#llx x1=%#llx x2=%#llx, from %s)", e->name.c_str(), c->x[0],
                    c->x[1], c->x[2], describe_guest(c->x[30]).c_str());
            c->x[0] = 0;
            return;
        }
        Ctx* const outer = t_ctx;
        t_ctx = c;
        e->fn(c);
        t_ctx = outer;
    });
    g_by_name.emplace(name, addr);
    return addr;
}

namespace {

// nvnBootstrapLoader(const char* name)
void bootstrap_loader(Ctx* c) {
    const std::string name = read_cstr(c->x[0], 256);
    c->x[0] = lookup(name);
}

}  // namespace

void install() {
    host_override(gfx::sdk_fn("nvnBootstrapLoader"), bootstrap_loader);
    std::thread(report_loop).detach();
    LOG("nvn: native implementation (%zu functions), the sdk's driver is not used", impls().size());
}

}  // namespace nvn
