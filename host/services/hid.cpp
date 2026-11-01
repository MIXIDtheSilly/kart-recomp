// hid: input. One Pro Controller on player 1 (Handheld when the game doesn't take Fullkey), fed by
// every gamepad SDL3 sees (DualSense, DualShock, Xbox, Switch Pro, ...; XInput if SDL can't start)
// and the keyboard while the game window has the focus. The state goes into the shared memory in
// nn::hid's layout (Eglantine's hid_core/resources/shared_memory_format.h).
#include <windows.h>
#include <Xinput.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <map>
#include <thread>

#include <SDL3/SDL.h>

#include "../ipc.h"
#include "hid_core/resources/shared_memory_format.h"

#pragma comment(lib, "xinput.lib")

namespace ipc {
namespace {

namespace HID = ::Service::HID;
using Core::HID::NpadButton;

std::shared_ptr<kern::KSharedMemory>& hid_shared() {
    static auto shm = [] {
        auto s = std::make_shared<kern::KSharedMemory>(sizeof(HID::SharedMemoryFormat));
        new (s->data()) HID::SharedMemoryFormat{};
        return s;
    }();
    return shm;
}
HID::SharedMemoryFormat& shared_format() {
    return *reinterpret_cast<HID::SharedMemoryFormat*>(hid_shared()->data());
}

kern::ServiceEvent& style_event() {
    static kern::ServiceEvent ev;
    return ev;
}

std::atomic<u32> g_supported_styles{0};
std::atomic<bool> g_started{false};

struct PadInput {
    u64 buttons = 0;
    Core::HID::AnalogStickState l{}, r{};
};

constexpr s32 StickMax = 0x7fff;

s32 axis(SHORT v, SHORT dead) {
    if (std::abs(v) < dead) return 0;
    return v == -32768 ? -StickMax : v;
}

void read_xinput(PadInput& in) {
    XINPUT_STATE st{};
    for (DWORD i = 0; i < XUSER_MAX_COUNT; ++i) {
        if (XInputGetState(i, &st) != ERROR_SUCCESS) continue;
        const auto& g = st.Gamepad;
        const auto b = [&](WORD mask, NpadButton n) {
            if (g.wButtons & mask) in.buttons |= static_cast<u64>(n);
        };
        // Positions as on the Switch: the bottom face button is B, the right one A.
        b(XINPUT_GAMEPAD_A, NpadButton::B);
        b(XINPUT_GAMEPAD_B, NpadButton::A);
        b(XINPUT_GAMEPAD_X, NpadButton::Y);
        b(XINPUT_GAMEPAD_Y, NpadButton::X);
        b(XINPUT_GAMEPAD_LEFT_SHOULDER, NpadButton::L);
        b(XINPUT_GAMEPAD_RIGHT_SHOULDER, NpadButton::R);
        b(XINPUT_GAMEPAD_START, NpadButton::Plus);
        b(XINPUT_GAMEPAD_BACK, NpadButton::Minus);
        b(XINPUT_GAMEPAD_LEFT_THUMB, NpadButton::StickL);
        b(XINPUT_GAMEPAD_RIGHT_THUMB, NpadButton::StickR);
        b(XINPUT_GAMEPAD_DPAD_LEFT, NpadButton::Left);
        b(XINPUT_GAMEPAD_DPAD_UP, NpadButton::Up);
        b(XINPUT_GAMEPAD_DPAD_RIGHT, NpadButton::Right);
        b(XINPUT_GAMEPAD_DPAD_DOWN, NpadButton::Down);
        if (g.bLeftTrigger > XINPUT_GAMEPAD_TRIGGER_THRESHOLD) in.buttons |= static_cast<u64>(NpadButton::ZL);
        if (g.bRightTrigger > XINPUT_GAMEPAD_TRIGGER_THRESHOLD) in.buttons |= static_cast<u64>(NpadButton::ZR);
        in.l = {axis(g.sThumbLX, XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE), axis(g.sThumbLY, XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE)};
        in.r = {axis(g.sThumbRX, XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE), axis(g.sThumbRY, XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE)};
        return;  // the first connected pad
    }
}

// SDL3 gamepads, read from the input thread (the thread that initialized SDL).
struct SdlPads {
    bool ok = false;
    std::map<SDL_JoystickID, SDL_Gamepad*> open;

    void init() {
        // No SDL window: pads must report while another window (ours) has the focus.
        SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
        ok = SDL_Init(SDL_INIT_GAMEPAD);
        if (!ok) LOG("hid: SDL gamepads unavailable (%s), using XInput", SDL_GetError());
    }

    void add(SDL_JoystickID id) {
        if (open.count(id)) return;
        if (SDL_Gamepad* g = SDL_OpenGamepad(id)) {
            open[id] = g;
            LOG("hid: gamepad connected: %s", SDL_GetGamepadName(g));
        }
    }

    void poll() {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_EVENT_GAMEPAD_ADDED) add(e.gdevice.which);
            if (e.type == SDL_EVENT_GAMEPAD_REMOVED) {
                auto it = open.find(e.gdevice.which);
                if (it == open.end()) continue;
                LOG("hid: gamepad disconnected: %s", SDL_GetGamepadName(it->second));
                SDL_CloseGamepad(it->second);
                open.erase(it);
            }
        }
    }

    // Radial dead zone; SDL's Y axes point down, the Switch's up.
    static Core::HID::AnalogStickState stick(SDL_Gamepad* g, SDL_GamepadAxis ax, SDL_GamepadAxis ay) {
        const float x = SDL_GetGamepadAxis(g, ax) / 32767.0f, y = -SDL_GetGamepadAxis(g, ay) / 32767.0f;
        const float mag = std::sqrt(x * x + y * y);
        constexpr float dead = 0.12f;
        if (mag < dead) return {};
        const float scale = std::min(1.0f, (mag - dead) / (1.0f - dead)) / mag;
        const auto c = [&](float v) { return static_cast<s32>(std::clamp(v * scale, -1.0f, 1.0f) * StickMax); };
        return {c(x), c(y)};
    }

    void read(PadInput& in) {
        for (const auto& [id, g] : open) {
            const auto b = [&](SDL_GamepadButton sb, NpadButton n) {
                if (SDL_GetGamepadButton(g, sb)) in.buttons |= static_cast<u64>(n);
            };
            // Positions as on the Switch: the bottom face button (Cross, Xbox A) is B, the right one A.
            b(SDL_GAMEPAD_BUTTON_SOUTH, NpadButton::B);
            b(SDL_GAMEPAD_BUTTON_EAST, NpadButton::A);
            b(SDL_GAMEPAD_BUTTON_WEST, NpadButton::Y);
            b(SDL_GAMEPAD_BUTTON_NORTH, NpadButton::X);
            b(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, NpadButton::L);
            b(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, NpadButton::R);
            b(SDL_GAMEPAD_BUTTON_START, NpadButton::Plus);
            b(SDL_GAMEPAD_BUTTON_BACK, NpadButton::Minus);
            b(SDL_GAMEPAD_BUTTON_TOUCHPAD, NpadButton::Minus);
            b(SDL_GAMEPAD_BUTTON_LEFT_STICK, NpadButton::StickL);
            b(SDL_GAMEPAD_BUTTON_RIGHT_STICK, NpadButton::StickR);
            b(SDL_GAMEPAD_BUTTON_DPAD_LEFT, NpadButton::Left);
            b(SDL_GAMEPAD_BUTTON_DPAD_UP, NpadButton::Up);
            b(SDL_GAMEPAD_BUTTON_DPAD_RIGHT, NpadButton::Right);
            b(SDL_GAMEPAD_BUTTON_DPAD_DOWN, NpadButton::Down);
            constexpr Sint16 trigger = 8000;
            if (SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > trigger) in.buttons |= static_cast<u64>(NpadButton::ZL);
            if (SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > trigger) in.buttons |= static_cast<u64>(NpadButton::ZR);
            // With several pads, the stick pushed furthest wins.
            const auto merge = [](Core::HID::AnalogStickState& to, Core::HID::AnalogStickState s) {
                if (std::abs(s.x) + std::abs(s.y) > std::abs(to.x) + std::abs(to.y)) to = s;
            };
            merge(in.l, stick(g, SDL_GAMEPAD_AXIS_LEFTX, SDL_GAMEPAD_AXIS_LEFTY));
            merge(in.r, stick(g, SDL_GAMEPAD_AXIS_RIGHTX, SDL_GAMEPAD_AXIS_RIGHTY));
        }
    }
};

bool window_focused() {
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    return pid == GetCurrentProcessId();
}

// Eden's default keyboard layout.
void read_keyboard(PadInput& in) {
    const auto down = [](int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; };
    const struct { int vk; NpadButton b; } keys[] = {
        {'C', NpadButton::A},       {'X', NpadButton::B},      {'V', NpadButton::X},
        {'Z', NpadButton::Y},       {'Q', NpadButton::L},      {'E', NpadButton::R},
        {'R', NpadButton::ZL},      {'T', NpadButton::ZR},     {'M', NpadButton::Plus},
        {'N', NpadButton::Minus},   {'F', NpadButton::StickL}, {'G', NpadButton::StickR},
        {VK_LEFT, NpadButton::Left}, {VK_UP, NpadButton::Up},  {VK_RIGHT, NpadButton::Right},
        {VK_DOWN, NpadButton::Down}, {VK_RETURN, NpadButton::Plus},
    };
    for (const auto& k : keys)
        if (down(k.vk)) in.buttons |= static_cast<u64>(k.b);
    const auto stick = [&](int left, int up, int right, int dn, Core::HID::AnalogStickState& s) {
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
    const struct { const char* name; NpadButton b; } names[] = {
        {"A", NpadButton::A},   {"B", NpadButton::B},       {"X", NpadButton::X},
        {"Y", NpadButton::Y},   {"L", NpadButton::L},       {"R", NpadButton::R},
        {"ZL", NpadButton::ZL}, {"ZR", NpadButton::ZR},     {"Plus", NpadButton::Plus},
        {"Minus", NpadButton::Minus}, {"Left", NpadButton::Left}, {"Up", NpadButton::Up},
        {"Right", NpadButton::Right}, {"Down", NpadButton::Down},
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
                if (n == nm.name) st.buttons |= static_cast<u64>(nm.b);
            b = e + 1;
        }
        steps.push_back(st);
    }
    return steps;
}

void add_stick_buttons(PadInput& in) {
    constexpr s32 t = StickMax / 2;
    const auto set = [&](bool c, NpadButton b) {
        if (c) in.buttons |= static_cast<u64>(b);
    };
    set(in.l.x < -t, NpadButton::StickLLeft);
    set(in.l.y > t, NpadButton::StickLUp);
    set(in.l.x > t, NpadButton::StickLRight);
    set(in.l.y < -t, NpadButton::StickLDown);
    set(in.r.x < -t, NpadButton::StickRLeft);
    set(in.r.y > t, NpadButton::StickRUp);
    set(in.r.x > t, NpadButton::StickRRight);
    set(in.r.y < -t, NpadButton::StickRDown);
}

// Writes the next entry before moving the tail, so a reader never sees a half-written newest one.
template <class State, size_t N>
void lifo_push(HID::Lifo<State, N>& lifo, State st) {
    st.sampling_number = lifo.entries[lifo.buffer_tail].state.sampling_number + 1;
    const size_t next = (lifo.buffer_tail + 1) % N;
    lifo.entries[next].sampling_number = st.sampling_number << 1;
    lifo.entries[next].state = st;
    std::atomic_thread_fence(std::memory_order_release);
    if (lifo.buffer_count < static_cast<s64>(N) - 1) lifo.buffer_count++;
    lifo.buffer_tail = next;
}

// Player 1 as a Pro Controller, or the console's own controls when the game takes only those.
void connect(HID::NpadInternalState& np, bool handheld) {
    np.style_tag.raw = Core::HID::NpadStyleSet::None;
    np.device_type.raw = 0;
    np.system_properties.raw = 0;
    const Core::HID::NpadControllerColor grey{{0x32, 0x32, 0x32, 0xff}, {0xe6, 0xe6, 0xe6, 0xff}};
    np.fullkey_color.attribute = HID::ColorAttribute::Ok;
    np.fullkey_color.fullkey = grey;
    np.battery_level_dual = Core::HID::NpadBatteryLevel::Full;
    np.system_properties.is_vertical.Assign(1);
    np.system_properties.use_plus.Assign(1);
    np.system_properties.use_minus.Assign(1);
    if (handheld) {
        np.joycon_color.attribute = HID::ColorAttribute::Ok;
        np.joycon_color.left = grey;
        np.joycon_color.right = grey;
        np.style_tag.handheld.Assign(1);
        np.device_type.handheld_left.Assign(1);
        np.device_type.handheld_right.Assign(1);
        np.system_properties.use_directional_buttons.Assign(1);
        np.assignment_mode = HID::NpadJoyAssignmentMode::Dual;
        np.applet_footer_type = HID::AppletFooterUiType::HandheldJoyConLeftJoyConRight;
        np.sixaxis_handheld_properties.is_newly_assigned.Assign(1);
    } else {
        np.style_tag.fullkey.Assign(1);
        np.device_type.fullkey.Assign(1);
        np.applet_footer_type = HID::AppletFooterUiType::SwitchProController;
        np.sixaxis_fullkey_properties.is_newly_assigned.Assign(1);
    }
}

void input_thread() {
    SetThreadDescription(GetCurrentThread(), L"hid");
    const auto script = load_script();
    const u64 start = host_ns();
    const u32 styles = g_supported_styles.load();
    const bool handheld = !(styles & static_cast<u32>(Core::HID::NpadStyleSet::Fullkey)) &&
                          (styles & static_cast<u32>(Core::HID::NpadStyleSet::Handheld));
    auto& np = shared_format().npad.npad_entry[handheld ? 8 : 0].internal_state;
    connect(np, handheld);
    LOG("hid: player 1 connected as %s (game supports styles 0x%x)", handheld ? "handheld" : "Pro Controller",
        styles);
    style_event().signal();

    SdlPads pads;
    pads.init();

    u64 last_buttons = 0;
    for (;;) {
        PadInput in;
        if (pads.ok) {
            pads.poll();
            pads.read(in);
        } else {
            read_xinput(in);
        }
        if (window_focused()) read_keyboard(in);
        const double t = (host_ns() - start) / 1e9;
        for (const auto& s : script)
            if (t >= s.at && t < s.until) in.buttons |= s.buttons;
        add_stick_buttons(in);
        if (in.buttons != last_buttons && !script.empty()) LOG("hid: buttons 0x%llx at %.2f s", in.buttons, t);
        last_buttons = in.buttons;

        HID::NPadGenericState st{};
        st.npad_buttons.raw = static_cast<NpadButton>(in.buttons);
        st.l_stick = in.l;
        st.r_stick = in.r;
        st.connection_status.is_connected.Assign(1);
        st.connection_status.is_wired.Assign(1);
        if (handheld) {
            st.connection_status.is_left_connected.Assign(1);
            st.connection_status.is_right_connected.Assign(1);
            st.connection_status.is_left_wired.Assign(1);
            st.connection_status.is_right_wired.Assign(1);
            lifo_push(np.handheld_lifo, st);
        } else {
            lifo_push(np.fullkey_lifo, st);
        }
        // The other styles' lifos advance with empty entries, as in Eden.
        HID::NPadGenericState empty{};
        if (handheld) lifo_push(np.fullkey_lifo, empty);
        else lifo_push(np.handheld_lifo, empty);
        lifo_push(np.joy_dual_lifo, empty);
        lifo_push(np.joy_left_lifo, empty);
        lifo_push(np.joy_right_lifo, empty);
        lifo_push(np.palma_lifo, empty);
        lifo_push(np.system_ext_lifo, empty);
        lifo_push(np.gc_trigger_lifo, HID::NpadGcTriggerState{});
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
