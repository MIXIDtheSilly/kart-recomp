// hid: input. Player 1 as a Pro Controller (Handheld when the game doesn't take Fullkey), fed by the first gamepad
// to press a button and the keyboard while the game window has the focus; players 2-4 as Pro Controllers, one more
// gamepad each (gamepads.cpp: XInput, DualShock 4, DualSense, Switch Pro Controller). The state goes into the shared
// memory in nn::hid's layout (hid_shared.h).
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <thread>

#include "../ipc.h"
#include "gamepads.h"
#include "hid_shared.h"

namespace ipc {
namespace {

using namespace hid;

std::shared_ptr<kern::KSharedMemory>& hid_shared() {
    static auto shm = [] {
        auto s = std::make_shared<kern::KSharedMemory>(sizeof(SharedMemoryFormat));
        reinterpret_cast<SharedMemoryFormat*>(s->data())->init();
        return s;
    }();
    return shm;
}
SharedMemoryFormat& shared_format() {
    return *reinterpret_cast<SharedMemoryFormat*>(hid_shared()->data());
}

kern::ServiceEvent& style_event() {
    static kern::ServiceEvent ev;
    return ev;
}

std::atomic<u32> g_supported_styles{0};
std::atomic<bool> g_started{false};

using PadInput = gamepads::State;

constexpr s32 StickMax = 0x7fff;

bool window_focused() {
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    return pid == GetCurrentProcessId();
}

// Eden's default keyboard layout.
void read_keyboard(PadInput& in) {
    const auto down = [](int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; };
    const struct { int vk; u64 b; } keys[] = {
        {'C', A},       {'X', B},      {'V', X},
        {'Z', Y},       {'Q', L},      {'E', R},
        {'R', ZL},      {'T', ZR},     {'M', Plus},
        {'N', Minus},   {'F', StickL}, {'G', StickR},
        {VK_LEFT, Left}, {VK_UP, Up},  {VK_RIGHT, Right},
        {VK_DOWN, Down}, {VK_RETURN, Plus},
    };
    for (const auto& k : keys)
        if (down(k.vk)) in.buttons |= k.b;
    const auto stick = [&](int left, int up, int right, int dn, Stick& s) {
        const s32 x = (down(right) ? StickMax : 0) - (down(left) ? StickMax : 0);
        const s32 y = (down(up) ? StickMax : 0) - (down(dn) ? StickMax : 0);
        if (x || y) s = {x, y};
    };
    stick('A', 'W', 'D', 'S', in.l);
    stick('J', 'I', 'L', 'K', in.r);
}

// HOST_INPUT_SCRIPT="<seconds>:<buttons>:<duration>;..." presses buttons at given times since
// the first hid update (for test runs), e.g. "40:L+R:0.3;50:A:0.2". Buttons: A B X Y L R ZL ZR
// Plus Minus Left Up Right Down.
struct ScriptStep {
    double at, until;
    u64 buttons;
};
std::vector<ScriptStep> load_script() {
    std::vector<ScriptStep> steps;
    const char* env = std::getenv("HOST_INPUT_SCRIPT");
    if (!env) return steps;
    const struct { const char* name; u64 b; } names[] = {
        {"A", A},   {"B", B},       {"X", X},
        {"Y", Y},   {"L", L},       {"R", R},
        {"ZL", ZL}, {"ZR", ZR},     {"Plus", Plus},
        {"Minus", Minus}, {"Left", Left}, {"Up", Up},
        {"Right", Right}, {"Down", Down},
    };
    std::string s = env;
    size_t pos = 0;
    while (pos < s.size()) {
        size_t end = s.find(';', pos);
        if (end == std::string::npos) end = s.size();
        const std::string step = s.substr(pos, end - pos);
        pos = end + 1;
        const size_t c1 = step.find(':'), c2 = step.rfind(':');
        if (c1 == std::string::npos || c2 == c1) continue;
        ScriptStep st{std::atof(step.c_str()), 0, 0};
        st.until = st.at + std::atof(step.c_str() + c2 + 1);
        const std::string btns = step.substr(c1 + 1, c2 - c1 - 1);
        size_t b = 0;
        while (b < btns.size()) {
            size_t e = btns.find('+', b);
            if (e == std::string::npos) e = btns.size();
            const std::string n = btns.substr(b, e - b);
            for (const auto& nm : names)
                if (n == nm.name) st.buttons |= nm.b;
            b = e + 1;
        }
        steps.push_back(st);
    }
    return steps;
}

void add_stick_buttons(PadInput& in) {
    constexpr s32 t = StickMax / 2;
    const auto set = [&](bool c, u64 b) {
        if (c) in.buttons |= b;
    };
    set(in.l.x < -t, StickLLeft);
    set(in.l.y > t, StickLUp);
    set(in.l.x > t, StickLRight);
    set(in.l.y < -t, StickLDown);
    set(in.r.x < -t, StickRLeft);
    set(in.r.y > t, StickRUp);
    set(in.r.x > t, StickRRight);
    set(in.r.y < -t, StickRDown);
}

// Player 1 as a Pro Controller, or the console's own controls when the game takes only those.
void connect(NpadInternalState& np, bool handheld) {
    const Color grey{{0x32, 0x32, 0x32, 0xff}, {0xe6, 0xe6, 0xe6, 0xff}};
    np.style = 0;
    np.device_type = 0;
    np.fullkey_color_attribute = ColorOk;
    np.fullkey_color = grey;
    np.battery_dual = 4;                       // full
    np.system_properties = 0x800 | 0x2000 | 0x4000;  // vertical, uses plus and minus
    if (handheld) {
        np.joycon_color_attribute = ColorOk;
        np.joycon_left = grey;
        np.joycon_right = grey;
        np.style = StyleHandheld;
        np.device_type = 4 | 8;                // handheld left and right
        np.system_properties |= 0x8000;        // directional buttons
        np.assignment_mode = 0;                // dual
        np.applet_footer_type = 4;             // handheld, both Joy-Cons
        np.sixaxis_properties[1] |= 1;         // handheld: newly assigned
    } else {
        np.style = StyleFullkey;
        np.device_type = 1;                    // fullkey
        np.applet_footer_type = 12;            // Pro Controller
        np.sixaxis_properties[0] |= 1;         // fullkey: newly assigned
    }
}

// A player whose pad left: no style, no device, and a last entry that says so.
void disconnect(NpadInternalState& np) {
    np.style = 0;
    np.device_type = 0;
    np.fullkey_color_attribute = ColorNoController;
    np.joycon_color_attribute = ColorNoController;
    np.battery_dual = 0;
    np.applet_footer_type = 0;
    std::fill(std::begin(np.sixaxis_properties), std::end(np.sixaxis_properties), u8(0));
    const NpadGenericState empty{};
    for (auto* lifo : {&np.fullkey_lifo, &np.handheld_lifo, &np.joy_dual_lifo, &np.joy_left_lifo, &np.joy_right_lifo,
                       &np.palma_lifo, &np.system_ext_lifo})
        lifo_push(*lifo, empty);
}

// One player's state into its npad: the style's lifo gets it, the others advance with empty entries, as in Eden.
void push_state(NpadInternalState& np, const PadInput& in, bool handheld) {
    NpadGenericState st{};
    st.buttons = in.buttons;
    st.l = in.l;
    st.r = in.r;
    st.attributes = Connected | Wired;
    if (handheld) {
        st.attributes |= LeftConnected | RightConnected | LeftWired | RightWired;
        lifo_push(np.handheld_lifo, st);
    } else {
        lifo_push(np.fullkey_lifo, st);
    }
    NpadGenericState empty{};
    if (handheld) lifo_push(np.fullkey_lifo, empty);
    else lifo_push(np.handheld_lifo, empty);
    lifo_push(np.joy_dual_lifo, empty);
    lifo_push(np.joy_left_lifo, empty);
    lifo_push(np.joy_right_lifo, empty);
    lifo_push(np.palma_lifo, empty);
    lifo_push(np.system_ext_lifo, empty);
    lifo_push(np.gc_trigger_lifo, NpadGcTriggerState{});
}

// Player 1 is always connected (the keyboard drives it too, and whichever pad joins first). Players 2-4 (npads No2-No4)
// are connected while a pad holds them (gamepads.h: a pad joins on its first button press), as Pro Controllers.
void input_thread() {
    SetThreadDescription(GetCurrentThread(), L"hid");
    const auto script = load_script();
    const u64 start = host_ns();
    const u32 styles = g_supported_styles.load();
    const bool handheld = !(styles & StyleFullkey) && (styles & StyleHandheld);
    auto& np = shared_format().npad(handheld ? 8 : 0);
    connect(np, handheld);
    LOG("hid: player 1 connected as %s (game supports styles 0x%x)", handheld ? "handheld" : "Pro Controller",
        styles);
    style_event().signal();

    gamepads::start();

    u64 last_buttons = 0;
    std::array<bool, gamepads::MaxPlayers> connected{true};
    for (;;) {
        gamepads::Players players;
        gamepads::read(players);
        PadInput& in = players.state[0];
        if (window_focused()) read_keyboard(in);
        const double t = (host_ns() - start) / 1e9;
        for (const auto& s : script)
            if (t >= s.at && t < s.until) in.buttons |= s.buttons;
        add_stick_buttons(in);
        if (in.buttons != last_buttons && !script.empty()) LOG("hid: buttons 0x%llx at %.2f s", in.buttons, t);
        last_buttons = in.buttons;
        push_state(np, in, handheld);

        // Players 2-4: only as Pro Controllers (a game that takes only handheld has no room for them)
        bool changed = false;
        for (int p = 1; p < gamepads::MaxPlayers; p++) {
            const bool on = players.joined[p] && (styles & StyleFullkey);
            auto& other = shared_format().npad(p);
            if (on != connected[p]) {
                connected[p] = on;
                changed = true;
                if (on) connect(other, false);
                else disconnect(other);
                LOG("hid: player %d %s", p + 1, on ? "connected as Pro Controller" : "disconnected");
            }
            if (on) {
                add_stick_buttons(players.state[p]);
                push_state(other, players.state[p], false);
            }
        }
        if (changed) style_event().signal();
        Sleep(4);
    }
}

void start_input() {
    if (g_started.exchange(true)) return;
    std::thread(input_thread).detach();
}

struct AppletResource : Service {
    AppletResource() : Service("IAppletResource") {
        on(0, [](Request& r) { r.out_copy.push_back(kern::add_handle(hid_shared())); });
    }
};

struct VibrationDeviceList : Service {
    VibrationDeviceList() : Service("IActiveVibrationDeviceList") {
        on(0, [](Request&) {});
    }
};

struct Hid : Service {
    Hid() : Service("hid") {
        on(0, [](Request& r) { r.push_object(std::make_shared<AppletResource>()); });
        on(100, [](Request& r) {  // SetSupportedNpadStyleSet
            g_supported_styles = r.arg<u32>(0);
            start_input();
        });
        on(102, [](Request&) {});  // SetSupportedNpadIdType
        on(103, [](Request&) {});  // ActivateNpad
        on(107, [](Request& r) {   // DisconnectNpad(u32 npad id, aruid): players 2-4's pad leaves until its next press
            const u32 id = r.arg<u32>(0);
            if (id >= 1 && id < u32(gamepads::MaxPlayers)) gamepads::leave(static_cast<int>(id));
            else LOG("hid: DisconnectNpad(%#x) kept", id);
        });
        on(109, [](Request&) {});  // ActivateNpadWithRevision
        on(106, [](Request& r) { r.out_copy.push_back(style_event().new_handle()); });  // AcquireNpadStyleSetUpdateEventHandle
        on(120, [](Request&) {});  // SetNpadJoyHoldType
        on(128, [](Request& r) { r.push<u64>(0); });  // GetNpadHandheldActivationMode
        on(203, [](Request& r) { r.push_object(std::make_shared<VibrationDeviceList>()); });
        on(214, [](Request&) {});  // Switch 2 vibration values
        on(11, [](Request&) {});    // ActivateTouchScreen
        on(1004, [](Request&) {});  // SetTouchScreenResolution
    }
};

}  // namespace

void register_hid() {
    register_service("hid", [] { return std::make_shared<Hid>(); });
}

}  // namespace ipc
