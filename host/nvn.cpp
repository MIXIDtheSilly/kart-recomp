// NVN: the game's graphics API, implemented natively.
#include <mutex>
#include <set>

#include "graphics.h"
#include "host.h"

namespace gfx {
namespace {

bool ends_with(const std::string& s, const char* suffix) {
    const size_t n = std::strlen(suffix);
    return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
}

// Placeholder for functions without a native implementation yet: logs the first call.
void unimplemented(Ctx* c, const std::string& name) {
    static std::mutex m;
    static std::set<std::string> seen;
    {
        std::scoped_lock lk{m};
        if (seen.insert(name).second)
            LOG("nvn: %s(%#llx, %#llx, %#llx, %#llx) not implemented (from %s)", name.c_str(), c->x[0], c->x[1], c->x[2],
                c->x[3], describe_guest(c->x[30]).c_str());
    }
    c->x[0] = ends_with(name, "Initialize") ? 1 : 0;
}

NativeFn lookup(const std::string& name);

void get_proc_address(Ctx* c, const std::string&) {
    const std::string name = read_cstr(c->x[1], 256);
    c->x[0] = native_function(name, lookup(name));
}

NativeFn lookup(const std::string& name) {
    if (name == "nvnDeviceGetProcAddress") return get_proc_address;
    return unimplemented;
}

// nvnBootstrapLoader(const char* name)
void bootstrap_loader(Ctx* c) {
    const std::string name = read_cstr(c->x[0], 256);
    LOG("nvnBootstrapLoader(%s)", name.c_str());
    c->x[0] = native_function(name, lookup(name));
}

}  // namespace

void install_nvn() {
    host_override(sdk_fn("nvnBootstrapLoader"), bootstrap_loader);
}

}  // namespace gfx
