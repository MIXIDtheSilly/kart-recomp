// HOST_NVN_SHIM=1: the real NVN driver behind counting thunks, with its inlined fast path off.
//
// main checks the sdk flag nvnPrivate0 before each command it could write itself (811 sites): when
// it is set, draws and dispatches go straight into command memory as GPU methods; when it is 0 they
// are NVN calls. The driver sets it in nvnBootstrapLoader (sdk+0x5b670c), so the shim clears it after
// every lookup. Every proc the game gets (through nvnBootstrapLoader or nvnDeviceGetProcAddress) is
// wrapped in a thunk that counts calls and runs the driver's function; every 10 s the busiest are
// logged. This is the call surface a native NVN implementation has to cover.
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

#include "graphics.h"
#include "host.h"

namespace gfx {
namespace {

struct Proc {
    std::string name;
    u64 driver = 0;
    std::atomic<u64> calls{0};
    u64 reported = 0;
};
std::mutex g_lock;
std::deque<Proc> g_procs;  // stable addresses
std::unordered_map<std::string, u64> g_wrapped;  // name -> thunk
u64 g_private0 = 0;        // &nvnPrivate0
GuestFn g_bootstrap = nullptr;

void clear_fast_path() {
    if (g_private0) st<u32>(g_private0, 0);
}

u64 wrap(const std::string& name, u64 driver);

void device_get_proc_address(Ctx* c, Proc* p) {
    const std::string name = read_cstr(c->x[1], 256);
    call_indirect(c, p->driver);
    clear_fast_path();
    if (c->x[0]) c->x[0] = wrap(name, c->x[0]);
}

u64 wrap(const std::string& name, u64 driver) {
    std::scoped_lock lk{g_lock};
    if (auto it = g_wrapped.find(name); it != g_wrapped.end()) return it->second;
    Proc* p = &g_procs.emplace_back();
    p->name = name;
    p->driver = driver;
    const bool gpa = name == "nvnDeviceGetProcAddress";
    const bool get_integer = name == "nvnDeviceGetInteger";
    const u64 thunk = native_function("shim:" + name, [p, gpa, get_integer](Ctx* c, const std::string&) {
        p->calls.fetch_add(1, std::memory_order_relaxed);
        if (gpa) {
            device_get_proc_address(c, p);
        } else if (get_integer) {
            // The driver's answers, once per item: what a native NVN must report.
            const u64 item = c->x[1] & 0xffffffff, out = c->x[2];
            call_indirect(c, p->driver);
            static std::mutex m;
            static std::unordered_map<u64, s32> seen;
            const s32 v = ld<s32>(out);
            std::scoped_lock lk{m};
            if (auto [it, fresh] = seen.emplace(item, v); fresh || it->second != v) {
                it->second = v;
                LOG("nvn shim: nvnDeviceGetInteger(%#llx) = %d (%#x)", item, v, static_cast<u32>(v));
            }
        } else {
            call_indirect(c, p->driver);
        }
    });
    g_wrapped.emplace(name, thunk);
    return thunk;
}

// nvnBootstrapLoader(const char* name)
void bootstrap_loader(Ctx* c) {
    const std::string name = read_cstr(c->x[0], 256);
    g_bootstrap(c);
    clear_fast_path();
    if (c->x[0]) c->x[0] = wrap(name, c->x[0]);
}

void reporter() {
    for (;;) {
        std::this_thread::sleep_for(std::chrono::seconds(10));
        std::vector<std::pair<u64, const Proc*>> rows;
        u64 total = 0;
        {
            std::scoped_lock lk{g_lock};
            for (Proc& p : g_procs) {
                const u64 n = p.calls.load(std::memory_order_relaxed);
                if (n != p.reported) rows.emplace_back(n - p.reported, &p);
                total += n - p.reported;
                p.reported = n;
            }
        }
        std::sort(rows.begin(), rows.end(), [](auto& a, auto& b) { return a.first > b.first; });
        // Every function called so far, with its total, in user/nvn_calls.txt.
        if (FILE* f = std::fopen((g_config.user_dir / "nvn_calls.txt").string().c_str(), "w")) {
            std::scoped_lock lk{g_lock};
            for (const Proc& p : g_procs)
                if (const u64 n = p.calls.load(std::memory_order_relaxed)) std::fprintf(f, "%s\t%llu\n", p.name.c_str(), n);
            std::fclose(f);
        }
        std::string line;
        for (size_t i = 0; i < rows.size() && i < 40; i++) {
            char buf[160];
            snprintf(buf, sizeof buf, " %s:%llu", rows[i].second->name.c_str() + 3, rows[i].first / 10);
            line += buf;
        }
        LOG("nvn shim: %llu calls/s, %zu functions, %zu procs looked up;%s", total / 10, rows.size(), g_procs.size(),
            line.c_str());
    }
}

}  // namespace

void install_nvn_shim() {
    const u64 bootstrap = sdk_fn("nvnBootstrapLoader");
    g_bootstrap = recompiled_fn(bootstrap);
    if (!g_bootstrap) fatal("nvn shim: nvnBootstrapLoader not recompiled");
    g_private0 = sdk_export("nvnPrivate0");
    if (!g_private0) fatal("nvn shim: nvnPrivate0 not exported");
    host_override(bootstrap, bootstrap_loader);
    std::thread(reporter).detach();
    LOG("nvn shim: NVN calls counted, inlined command fast path off (nvnPrivate0 at %s)",
        describe_guest(g_private0).c_str());
}

}  // namespace gfx
