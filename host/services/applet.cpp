// am (appletOE): the application's view of the applet manager.
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>

#include "../ipc.h"

std::string host_exe_variant();  // localcfg.cpp: "1.8.0" for kart-1.8.0.exe, "" for kart.exe

namespace ipc {
namespace {

constexpr u32 ResultNoMessages = MakeResult(128, 3);
constexpr u64 AppletResourceUserId = 0x100;
constexpr u8 UserId[16] = {0x4b, 0x61, 0x72, 0x74, 0x50, 0x6c, 0x61, 0x79, 0x65, 0x72, 0x30, 0x30, 0x30, 0x30, 0x30, 0x31};

// Applet message queue shared by the proxy objects.
struct AppletState {
    std::mutex lock;
    std::deque<u32> messages;
    kern::ServiceEvent message_event;
    kern::ServiceEvent gpu_error_event, suspended_tick_event, launch_param_event;
    kern::ServiceEvent operation_mode_refresh_event;  // never signalled: the operation mode does not change
    kern::ServiceEvent launchable_event;
    // Handheld (undocked, lower resolution) by default; HOST_HANDHELD=0: docked
    bool docked = [] {
        const char* v = std::getenv("HOST_HANDHELD");
        return v && std::strcmp(v, "0") == 0;
    }();

    void push(u32 msg) {
        std::scoped_lock lk{lock};
        messages.push_back(msg);
        message_event.signal();
    }
};
AppletState& applet() {
    static AppletState s;
    return s;
}

struct StorageAccessor : Service {
    explicit StorageAccessor(std::shared_ptr<std::vector<u8>> d) : Service("IStorageAccessor"), data(std::move(d)) {
        on(0, [this](Request& r) { r.push<u64>(data->size()); });
        on(10, [this](Request& r) {  // Write
            const u64 off = r.arg<u64>(0);
            auto in = r.read_in(0);
            if (off + in.size() > data->size()) { r.result = MakeResult(128, 503); return; }
            std::memcpy(data->data() + off, in.data(), in.size());
        });
        on(11, [this](Request& r) {  // Read
            const u64 off = r.arg<u64>(0);
            if (off > data->size()) { r.result = MakeResult(128, 503); return; }
            r.write_out(0, data->data() + off, data->size() - off);
        });
    }
    std::shared_ptr<std::vector<u8>> data;
};

struct Storage : Service {
    explicit Storage(std::vector<u8> d) : Service("IStorage"), data(std::make_shared<std::vector<u8>>(std::move(d))) {
        on(0, [this](Request& r) { r.push_object(std::make_shared<StorageAccessor>(data)); });
    }
    std::shared_ptr<std::vector<u8>> data;
};

// ------------------------------------------------------------------ library applets

// A library applet (error viewer, network connection dialog, ...): its input is logged and it completes as soon as it
// starts, as if the player closed it at once. Without one the game waited forever on GetLibraryAppletLaunchableEvent.
struct LibraryAppletAccessor : Service {
    u32 id, mode;
    kern::ServiceEvent state_event, out_event, interactive_event;
    bool started = false;

    LibraryAppletAccessor(u32 id_, u32 mode_) : Service("ILibraryAppletAccessor"), id(id_), mode(mode_) {
        on(0, [this](Request& r) { r.out_copy.push_back(state_event.new_handle()); });  // GetAppletStateChangedEvent
        on(1, [this](Request& r) { r.push<u8>(started); });                             // IsCompleted
        on(10, [this](Request&) {                                                       // Start
            LOG("am: library applet %#x (mode %u) started; it closes at once", id, mode);
            started = true;
            state_event.signal();
        });
        for (u32 c : {20u, 25u}) on(c, [this](Request&) { started = true; state_event.signal(); });  // RequestExit, Terminate
        on(30, [](Request&) {});                                                         // GetResult: success
        auto push = [this](Request& r, const char* what) {
            std::string hex;
            if (!r.in_services.empty() && r.in_services[0] && r.in_services[0]->name == "IStorage") {
                const auto& d = *static_cast<Storage*>(r.in_services[0].get())->data;
                for (size_t i = 0; i < d.size() && i < 64; i++) {
                    char b[4];
                    std::snprintf(b, sizeof b, "%02x", d[i]);
                    hex += b;
                    if (i % 4 == 3) hex += ' ';
                }
                LOG("am: library applet %#x %s (%zu bytes): %s", id, what, d.size(), hex.c_str());
            }
        };
        on(100, [push](Request& r) { push(r, "in data"); });              // PushInData
        on(103, [push](Request& r) { push(r, "interactive in data"); });  // PushInteractiveInData
        for (u32 c : {101u, 104u}) on(c, [](Request& r) { r.result = MakeResult(128, 3); });  // PopOutData: none
        on(105, [this](Request& r) { r.out_copy.push_back(out_event.new_handle()); });
        on(106, [this](Request& r) { r.out_copy.push_back(interactive_event.new_handle()); });
        on(110, [](Request& r) { r.push<u8>(0); });  // NeedsToExitProcess
        on(120, [this](Request& r) {                 // GetLibraryAppletInfo
            r.push<u32>(id);
            r.push<u32>(mode);
        });
        for (u32 c : {50u, 60u, 102u, 150u}) on(c, [](Request&) {});
    }
};

struct LibraryAppletCreator : Service {
    LibraryAppletCreator() : Service("ILibraryAppletCreator") {
        // CreateLibraryApplet (0) and its Switch 2 form (3): applet id, mode, then (3) more words
        auto create = [](Request& r) {
            LOG("am: CreateLibraryApplet(%u): %08x %08x %08x %08x %08x %08x", r.cmd, r.arg<u32>(0), r.arg<u32>(4), r.arg<u32>(8),
                r.arg<u32>(12), r.arg<u32>(16), r.arg<u32>(20));
            if (kern::KThread* t = kern::current_thread(); t && std::getenv("HOST_NET_TRACE")) dump_guest_stack(&t->ctx);
            r.push_object(std::make_shared<LibraryAppletAccessor>(r.arg<u32>(0), r.arg<u32>(4)));
        };
        on(0, create);
        on(3, create);
        on(10, [](Request& r) {  // CreateStorage
            r.push_object(std::make_shared<Storage>(std::vector<u8>(static_cast<size_t>(std::clamp<s64>(r.arg<s64>(0), 0, 1 << 26)))));
        });
    }
};

struct CommonStateGetter : Service {
    CommonStateGetter() : Service("ICommonStateGetter") {
        on(0, [](Request& r) { r.out_copy.push_back(applet().message_event.new_handle()); });
        on(1, [](Request& r) {  // ReceiveMessage
            auto& a = applet();
            std::scoped_lock lk{a.lock};
            if (a.messages.empty()) {
                a.message_event.clear();
                r.result = ResultNoMessages;
                return;
            }
            r.push<u32>(a.messages.front());
            a.messages.pop_front();
            if (a.messages.empty()) a.message_event.clear();
        });
        on(5, [](Request& r) { r.push<u8>(applet().docked ? 1 : 0); });  // GetOperationMode
        on(6, [](Request& r) { r.push<u32>(applet().docked ? 1 : 0); });  // GetPerformanceMode
        on(9, [](Request& r) { r.push<u8>(1); });                         // GetCurrentFocusState: in focus
        on(66, [](Request&) {});                                          // SetCpuBoostMode
        on(900, [](Request&) {});
    }
};

struct SelfController : Service {
    SelfController() : Service("ISelfController") {
        for (u32 id : {1u, 2u, 11u, 12u, 13u, 14u, 16u, 19u, 40u, 68u}) on(id, [](Request&) {});
        on(9, [](Request& r) {  // GetLibraryAppletLaunchableEvent: always launchable
            applet().launchable_event.signal();
            r.out_copy.push_back(applet().launchable_event.new_handle());
        });
        on(91, [](Request& r) { r.out_copy.push_back(applet().suspended_tick_event.new_handle()); });
        on(2000, [](Request& r) {  // GetGraphicsSystemSession: a plain session (copy handle) even inside the am domain
            r.out_copy.push_back(new_session(make_service("nx2:gsv")));
        });
    }
};

struct WindowController : Service {
    WindowController() : Service("IWindowController") {
        on(1, [](Request& r) { r.push<u64>(AppletResourceUserId); });
        on(10, [](Request&) {});
    }
};

struct ApplicationFunctions : Service {
    ApplicationFunctions() : Service("IApplicationFunctions") {
        on(1, [this](Request& r) {  // PopLaunchParameter
            const u32 kind = r.arg<u32>(0);
            if (kind != 2 || popped_user) {
                r.result = MakeResult(128, 2);  // no more launch parameters
                return;
            }
            popped_user = true;
            // Preselected user: magic, is_account_selected, user id.
            std::vector<u8> d(0x88);
            const u32 magic = 0xC79497CA, selected = 1;
            std::memcpy(d.data(), &magic, 4);
            std::memcpy(d.data() + 4, &selected, 4);
            std::memcpy(d.data() + 8, UserId, 16);
            r.push_object(std::make_shared<Storage>(std::move(d)));
        });
        on(21, [](Request& r) { r.push<u64>(0x53552D6E65ull); });  // GetDesiredLanguage: "en-US"
        on(23, [](Request& r) {                                   // GetDisplayVersion
            // The game version kart-<version>.exe was built for, else 1.0.0 (the default build's)
            const std::string ver = host_exe_variant();
            char v[16] = "1.0.0";
            if (!ver.empty() && ver.size() < sizeof v && ver.find_first_not_of("0123456789.") == std::string::npos)
                std::strncpy(v, ver.c_str(), sizeof v - 1);
            r.push_bytes(v, 16);
        });
        on(40, [](Request& r) { r.push<u8>(0); });  // NotifyRunning
        on(130, [](Request& r) { r.out_copy.push_back(applet().gpu_error_event.new_handle()); });
        // GetOperationModeRefreshEvent (Eglantine's name, guessed). 1.8.0 asks for it at boot and waits on it with the
        // other applet events: without a handle it waited on handle 0 in a loop and never drew a frame.
        on(210, [](Request& r) { r.out_copy.push_back(applet().operation_mode_refresh_event.new_handle()); });
        on(321, [](Request&) {});
    }
    bool popped_user = false;
};

struct GraphicsScaleController : Service {
    GraphicsScaleController() : Service("IGraphicsScaleController") {
        on(10, [](Request& r) { r.push<u32>(10); });  // GetCurrentGraphicsScale (as Eglantine)
        on(1050, [](Request&) {});                    // takes a bool
    }
};

struct ApplicationProxy : Service {
    ApplicationProxy() : Service("IApplicationProxy") {
        on(0, [](Request& r) { r.push_object(std::make_shared<CommonStateGetter>()); });
        on(1, [](Request& r) { r.push_object(std::make_shared<SelfController>()); });
        on(2, [](Request& r) { r.push_object(std::make_shared<WindowController>()); });
        on(3, [](Request& r) { r.push_object(stub("IAudioController")); });
        on(4, [](Request& r) { r.push_object(stub("IDisplayController")); });
        on(10, [](Request& r) { r.push_object(stub("IProcessWindingController")); });
        on(11, [](Request& r) { r.push_object(std::make_shared<LibraryAppletCreator>()); });
        on(20, [](Request& r) { r.push_object(std::make_shared<ApplicationFunctions>()); });
        for (u32 id : {40u, 50u, 70u, 80u, 90u}) {  // Switch 2 getters with unknown interfaces
            on(id, [id](Request& r) { r.push_object(stub("IApplicationProxy.getter" + std::to_string(id))); });
        }
        on(60, [](Request& r) { r.push_object(std::make_shared<GraphicsScaleController>()); });
        on(1000, [](Request& r) { r.push_object(stub("IDebugFunctions")); });
    }
};

struct AppletOE : Service {
    AppletOE() : Service("appletOE") {
        auto open = [](Request& r) { r.push_object(std::make_shared<ApplicationProxy>()); };
        on(0, open);
        on(10, open);
    }
};

}  // namespace

void register_applet() {
    // The application starts in focus: the first message tells it so.
    applet().push(15);  // FocusStateChanged
    register_service("appletOE", [] { return std::make_shared<AppletOE>(); });
}

}  // namespace ipc
