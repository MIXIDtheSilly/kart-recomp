// sm: the service manager (TIPC on this firmware).
#include "../ipc.h"

namespace ipc {

namespace {

std::string service_name(u64 packed) {
    std::string s;
    for (int i = 0; i < 8; i++) {
        const char ch = static_cast<char>(packed >> (8 * i));
        if (!ch) break;
        s += ch;
    }
    return s;
}

struct SM : Service {
    SM() : Service("sm:") {
        on(0, [](Request&) {});  // Initialize
        on(1, [](Request& r) {   // GetService
            const std::string name = service_name(r.arg<u64>(0));
            ServicePtr s = make_service(name);
            if (!s) {
                LOG("sm: %s has no native implementation, using a stub", name.c_str());
                s = stub(name);
            }
            r.push_object(s);
        });
    }
};

}  // namespace

void register_sm() {
    register_service("sm:", [] { return std::make_shared<SM>(); });
}

}  // namespace ipc
