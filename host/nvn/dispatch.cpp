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
    std::atomic<u64> calls{0};
    u64 reported = 0;
};
std::mutex g_lock;
std::deque<Entry> g_entries;
std::unordered_map<std::string, u64> g_by_name;  // name -> guest-callable address
thread_local Ctx* t_ctx = nullptr;

void report_loop() {
    for (;;) {
        std::this_thread::sleep_for(std::chrono::seconds(10));
        std::vector<std::pair<u64, const Entry*>> rows;
        u64 total = 0;
        {
            std::scoped_lock lk{g_lock};
            for (Entry& e : g_entries) {
                const u64 n = e.calls.load(std::memory_order_relaxed);
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
    Entry* e = &g_entries.emplace_back();
    e->name = name;
    if (auto it = table.find(name); it != table.end()) e->fn = it->second;
    const u64 addr = gfx::native_function("nvn:" + name, [e](Ctx* c, const std::string&) {
        e->calls.fetch_add(1, std::memory_order_relaxed);
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
