// hid: input. Player 1 as a Pro Controller (Handheld when the game doesn't take Fullkey), fed by the first gamepad
// to press a button and the keyboard while the game window has the focus; players 2-4 as Pro Controllers, one more
// gamepad each (gamepads.cpp: XInput, DualShock 4, DualSense, Switch Pro Controller), which also play the game's
// vibration. The state goes into the shared memory in nn::hid's layout (hid_shared.h).
#ifdef _WIN32
#include <windows.h>
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <thread>

#include "../ipc.h"
#include "../os.h"
#include "gamepads.h"
#ifndef _WIN32
#include "../video.h"
#endif
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

#ifdef _WIN32
bool window_focused() {
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    return pid == GetCurrentProcessId();
}
const auto down = [](int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; };
#define KEY(windows_vk, sdl_scancode) windows_vk
#else
// Linux: the SDL window's keyboard (video_sdl.cpp), by scancode: the keys' positions, as Windows' virtual keys are on a
// US layout. (Values from SDL_scancode.h.)
bool window_focused() { return video::focused(); }
const auto down = [](int scancode) { return video::key_down(scancode); };
#define KEY(windows_vk, sdl_scancode) sdl_scancode
#endif

// Eden's default keyboard layout.
void read_keyboard(PadInput& in) {
    const struct { int vk; u64 b; } keys[] = {
        {KEY('C', 6), A},          {KEY('X', 27), B},       {KEY('V', 25), X},
        {KEY('Z', 29), Y},         {KEY('Q', 20), L},       {KEY('E', 8), R},
        {KEY('R', 21), ZL},        {KEY('T', 23), ZR},      {KEY('M', 16), Plus},
        {KEY('N', 17), Minus},     {KEY('F', 9), StickL},   {KEY('G', 10), StickR},
        {KEY(VK_LEFT, 80), Left},  {KEY(VK_UP, 82), Up},    {KEY(VK_RIGHT, 79), Right},
        {KEY(VK_DOWN, 81), Down},  {KEY(VK_RETURN, 40), Plus},
    };
    for (const auto& k : keys)
        if (down(k.vk)) in.buttons |= k.b;
    const auto stick = [&](int left, int up, int right, int dn, Stick& s) {
        const s32 x = (down(right) ? StickMax : 0) - (down(left) ? StickMax : 0);
        const s32 y = (down(up) ? StickMax : 0) - (down(dn) ? StickMax : 0);
        if (x || y) s = {x, y};
    };
    stick(KEY('A', 4), KEY('W', 26), KEY('D', 7), KEY('S', 22), in.l);
    stick(KEY('J', 13), KEY('I', 12), KEY('L', 15), KEY('K', 14), in.r);
}

// HOST_INPUT_SCRIPT="<seconds>:<buttons>:<duration>;..." presses buttons at given times since
// the first hid update (for test runs), e.g. "40:L+R:0.3;50:A:0.2". Buttons: A B X Y L R ZL ZR
// Plus Minus Left Up Right Down LS RS (stick clicks).
struct ScriptStep {
    double at, until;
    u64 buttons;
};
// Steps of `s` ("<seconds>:<buttons>:<duration>;..."), their times offset by `base`.
std::vector<ScriptStep> parse_script(const std::string& s, double base) {
    std::vector<ScriptStep> steps;
    const struct { const char* name; u64 b; } names[] = {
        {"A", A},   {"B", B},       {"X", X},
        {"Y", Y},   {"L", L},       {"R", R},
        {"ZL", ZL}, {"ZR", ZR},     {"Plus", Plus},
        {"Minus", Minus}, {"Left", Left}, {"Up", Up},
        {"Right", Right}, {"Down", Down}, {"LS", StickL}, {"RS", StickR},
    };
    size_t pos = 0;
    while (pos < s.size()) {
        size_t end = s.find_first_of(";\n", pos);
        if (end == std::string::npos) end = s.size();
        const std::string step = s.substr(pos, end - pos);
        pos = end + 1;
        const size_t c1 = step.find(':'), c2 = step.rfind(':');
        if (c1 == std::string::npos || c2 == c1) continue;
        ScriptStep st{base + std::atof(step.c_str()), 0, 0};
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
std::vector<ScriptStep> load_script() {
    const char* env = std::getenv("HOST_INPUT_SCRIPT");
    return env ? parse_script(env, 0) : std::vector<ScriptStep>{};
}

// HOST_INPUT_FILE=<file>: lines appended to it while the game runs are script steps timed from when they are read
// ("0:A:0.2" presses A now), for driving test runs (two instances in LAN play) step by step.
struct LiveScript {
    const char* path = std::getenv("HOST_INPUT_FILE");
    size_t done = 0;
    double next_check = 0;
    void poll(std::vector<ScriptStep>& steps, double t) {
        if (!path || t < next_check) return;
        next_check = t + 0.1;
        FILE* f = std::fopen(path, "rb");
        if (!f) return;
        std::string text;
        char buf[4096];
        for (size_t n; (n = std::fread(buf, 1, sizeof buf, f)) > 0;) text.append(buf, n);
        std::fclose(f);
        if (text.size() < done) done = 0;  // the file was replaced
        const size_t end = text.rfind('\n');
        if (end == std::string::npos || end + 1 <= done) return;
        const auto add = parse_script(text.substr(done, end + 1 - done), t);
        for (const auto& st : add) LOG("hid: input file step 0x%llx in %.2f s for %.2f s", st.buttons, st.at - t, st.until - st.at);
        steps.insert(steps.end(), add.begin(), add.end());
        done = end + 1;
    }
};

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
// are connected while a pad (or the keyboard, with HOST_KEYBOARD_JOINS=1) holds them (gamepads.h: a pad joins on its
// first button press), as Pro Controllers.
void input_thread() {
    os::set_thread_name("hid");
    auto script = load_script();
    LiveScript live;
    const u64 start = host_ns();
    const u32 styles = g_supported_styles.load();
    const bool handheld = !(styles & StyleFullkey) && (styles & StyleHandheld);
    auto& np = shared_format().npad(handheld ? 8 : 0);
    connect(np, handheld);
    LOG("hid: player 1 connected as %s (game supports styles 0x%x)", handheld ? "handheld" : "Pro Controller",
        styles);
    style_event().signal();

    gamepads::start();

    // HOST_KEYBOARD_JOINS=1: the keyboard is a player of its own, joining on its first key press like a pad (one pad
    // and the keyboard make two players). Otherwise it drives player 1 alongside the first pad.
    const char* kb_env = std::getenv("HOST_KEYBOARD_JOINS");
    const bool kb_joins = kb_env && kb_env[0] == '1';
    if (kb_joins) LOG("hid: the keyboard joins as a player of its own");

    u64 last_buttons = 0;
    std::array<bool, gamepads::MaxPlayers> connected{true};
    for (;;) {
        const bool focused = window_focused();
        PadInput keyboard;
        if (kb_joins && focused) read_keyboard(keyboard);
        gamepads::Players players;
        gamepads::read(players, kb_joins ? &keyboard : nullptr);
        PadInput& in = players.state[0];
        if (!kb_joins && focused) read_keyboard(in);
        const double t = (host_ns() - start) / 1e9;
        live.poll(script, t);
        for (const auto& s : script)
            if (t >= s.at && t < s.until) in.buttons |= s.buttons;
        add_stick_buttons(in);
        if (in.buttons != last_buttons && (!script.empty() || live.path)) LOG("hid: buttons 0x%llx at %.2f s", in.buttons, t);
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
#ifdef _WIN32
        Sleep(4);
#else
        std::this_thread::sleep_for(std::chrono::milliseconds(4));
#endif
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
        on(214, [](Request& r) {  // SendVibrationValues: device handles (type, npad id, motor, 0), a value for each
            const auto handles = r.read_in(0), values = r.read_in(1);
            const size_t n = std::min(handles.size() / 4, values.size() / sizeof(gamepads::Vibration));
            for (size_t i = 0; i < n; i++) {
                gamepads::Vibration v;
                std::memcpy(&v, values.data() + i * sizeof v, sizeof v);
                const u8 npad = handles[i * 4 + 1];
                gamepads::vibrate(npad == 0x20 ? 0 : npad, handles[i * 4 + 2], v);  // 0x20: handheld, player 1
            }
        });
        on(11, [](Request&) {});    // ActivateTouchScreen
        on(1004, [](Request&) {});  // SetTouchScreenResolution
    }
};

}  // namespace

void register_hid() {
    register_service("hid", [] { return std::make_shared<Hid>(); });
}

}  // namespace ipc
