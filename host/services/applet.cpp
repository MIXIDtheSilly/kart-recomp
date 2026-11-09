// am (appletOE): the application's view of the applet manager.
#include <cstdlib>
#include <deque>

#include "../ipc.h"

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
    bool docked = std::getenv("HOST_HANDHELD") == nullptr;   // HOST_HANDHELD=1: the game runs as undocked (lower resolution)

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
            char v[16] = "1.0.0";
            r.push_bytes(v, 16);
        });
        on(40, [](Request& r) { r.push<u8>(0); });  // NotifyRunning
        on(130, [](Request& r) { r.out_copy.push_back(applet().gpu_error_event.new_handle()); });
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
        on(11, [](Request& r) { r.push_object(stub("ILibraryAppletCreator")); });
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
