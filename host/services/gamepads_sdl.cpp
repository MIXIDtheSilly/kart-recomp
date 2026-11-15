// Gamepads on Linux, through SDL 3's gamepad API: Xbox pads, DualShock 4, DualSense, the Switch Pro Controller and every
// other pad SDL has a mapping for (its HIDAPI drivers where /dev/hidraw* may be opened, else the kernel's evdev devices).
// Pads are opened and closed on the window's thread as SDL reports them (video_sdl.cpp passes its events here); the
// hid thread reads their state, which that loop keeps current. Buttons by position as on the Switch: the bottom face
// button (Cross, Xbox A) is B, the right one A. Joining players works as on Windows (gamepads.h, gamepads.cpp).
// Vibration: one thread sends each player's latest motors to its pad; SDL plays the low band on the big motor and the
// high band on the small one (on a Pro Controller as HD rumble).
#ifndef _WIN32
#include <SDL3/SDL.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "../common.h"
#include "../os.h"
#include "gamepads.h"

namespace gamepads {
namespace {

using namespace hid;
constexpr s32 StickMax = 0x7fff;
constexpr s16 TriggerThreshold = 30 * 32767 / 255;   // XInput's trigger threshold (30 of 255)

// Radial dead zone of 12%, then the rest of the range stretched to full.
Stick radial(float x, float y) {
    const float mag = std::sqrt(x * x + y * y);
    constexpr float dead = 0.12f;
    if (mag < dead) return {};
    const float scale = std::min(1.0f, (mag - dead) / (1.0f - dead)) / mag;
    const auto c = [&](float v) { return static_cast<s32>(std::clamp(v * scale, -1.0f, 1.0f) * StickMax); };
    return {c(x), c(y)};
}

using Motors = std::array<Vibration, 2>;

struct Pad {
    SDL_Gamepad* gamepad = nullptr;
    std::string key, name;
    Motors rumble{};   // last sent (vibration thread)
    u64 rumble_at = 0;
};

// Open pads by SDL instance id. Opened and closed on the window's thread, read by the hid and vibration threads.
std::mutex g_pads_lock;
std::unordered_map<SDL_JoystickID, std::shared_ptr<Pad>> g_pads;

// One connected pad this tick. `key` names it for as long as it stays connected.
struct Source {
    std::string key;
    std::string name;
    State state;
    std::shared_ptr<Pad> pad;
};

State read_pad(SDL_Gamepad* g) {
    State s;
    const auto b = [&](SDL_GamepadButton button, u64 n) {
        if (SDL_GetGamepadButton(g, button)) s.buttons |= n;
    };
    b(SDL_GAMEPAD_BUTTON_SOUTH, B);
    b(SDL_GAMEPAD_BUTTON_EAST, A);
    b(SDL_GAMEPAD_BUTTON_WEST, Y);
    b(SDL_GAMEPAD_BUTTON_NORTH, X);
    b(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, L);
    b(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, R);
    b(SDL_GAMEPAD_BUTTON_START, Plus);
    b(SDL_GAMEPAD_BUTTON_BACK, Minus);
    b(SDL_GAMEPAD_BUTTON_LEFT_STICK, StickL);
    b(SDL_GAMEPAD_BUTTON_RIGHT_STICK, StickR);
    b(SDL_GAMEPAD_BUTTON_DPAD_LEFT, Left);
    b(SDL_GAMEPAD_BUTTON_DPAD_UP, Up);
    b(SDL_GAMEPAD_BUTTON_DPAD_RIGHT, Right);
    b(SDL_GAMEPAD_BUTTON_DPAD_DOWN, Down);
    if (SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > TriggerThreshold) s.buttons |= ZL;
    if (SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > TriggerThreshold) s.buttons |= ZR;
    const auto axis = [&](SDL_GamepadAxis a) { return std::max(-1.0f, SDL_GetGamepadAxis(g, a) / 32767.0f); };
    // (SDL's y axes grow downwards; the Switch's upwards)
    s.l = radial(axis(SDL_GAMEPAD_AXIS_LEFTX), -axis(SDL_GAMEPAD_AXIS_LEFTY));
    s.r = radial(axis(SDL_GAMEPAD_AXIS_RIGHTX), -axis(SDL_GAMEPAD_AXIS_RIGHTY));
    return s;
}

// HOST_PLAYERS=<1-4>: at most that many players (4 by default). 1: every pad drives player 1, nobody joins.
int max_players() {
    static const int n = [] {
        const char* v = std::getenv("HOST_PLAYERS");
        return std::clamp(v && *v ? std::atoi(v) : MaxPlayers, 1, MaxPlayers);
    }();
    return n;
}

// HOST_PLAYERS=1: buttons are combined, the stick pushed furthest wins.
void merge(Stick& to, Stick s) {
    if (std::abs(s.x) + std::abs(s.y) > std::abs(to.x) + std::abs(to.y)) to = s;
}

void add(State& in, const State& s) {
    in.buttons |= s.buttons;
    merge(in.l, s.l);
    merge(in.r, s.r);
}

// When each pad last pressed a button. One controller can show up twice: Steam Input adds a virtual pad and the
// controller is still read directly. The copy presses what the original presses, a few ms apart (either one first), so
// a pad joins MirrorMs after its press, and not when a joined player pressed the same buttons within MirrorMs of it.
struct Press {
    u64 buttons = 0;       // now
    u64 pressed = 0;       // the buttons down when one last went down
    u64 at_ms = 0;         // when that was
    bool pending = false;  // a press of a pad without a player: it joins once MirrorMs have passed
};
constexpr u64 MirrorMs = 60;

u64 now_ms() {
    return static_cast<u64>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now().time_since_epoch()).count());
}

struct Slot {
    std::string key;  // empty: free
    std::string name;
    std::shared_ptr<Pad> pad;
    bool holding = false;  // the joining press: nothing reaches the game until every button is up
};

std::mutex g_slots_lock;
std::array<Slot, MaxPlayers> g_slots;
std::vector<std::string> g_left;  // pads sent away by leave(): they join again on a new press, not the one held now
std::unordered_map<std::string, Press> g_presses;

// The pad's player lights (PlayStation light bar colour, Pro Controller LEDs); -1: none.
void show_player(Pad& p, int player) { SDL_SetGamepadPlayerIndex(p.gamepad, player); }

// ------------------------------------------------------------------ Vibration

// Guarded by g_slots_lock: each player's motors as the game last set them, the pads that left a player (they stop),
// and whether anything changed since the vibration thread last looked.
std::array<Motors, MaxPlayers> g_motors{};
std::vector<Slot> g_released;
bool g_motors_changed = false;
std::condition_variable g_motors_cv;

std::timed_mutex g_send_lock;  // one sender at a time; stop_vibration() ends the sending for good
std::atomic<bool> g_stopped{false};

constexpr u32 RumbleMs = 1000;   // each rumble call lasts this long; sent again every RumbleMs / 2 while it goes on

void send_rumble(Pad& p, const Motors& m) {
    const auto level = [](float a) { return static_cast<Uint16>(std::clamp(a, 0.0f, 1.0f) * 65535 + 0.5f); };
    const Uint16 low = level(std::max(m[0].amp_low, m[1].amp_low)), high = level(std::max(m[0].amp_high, m[1].amp_high));
    SDL_RumbleGamepad(p.gamepad, low, high, low || high ? RumbleMs : 0);
}

// Sends what changed, at most every 15 ms, and a vibration that lasts again before SDL's duration runs out.
void send_vibration() {
    os::set_thread_name("vibration");
    for (;;) {
        std::vector<std::pair<std::shared_ptr<Pad>, Motors>> targets;
        {
            std::unique_lock lk{g_slots_lock};
            g_motors_cv.wait_for(lk, std::chrono::milliseconds(50), [] { return g_motors_changed; });
            g_motors_changed = false;
            for (int i = 0; i < MaxPlayers; i++)
                if (!g_slots[i].key.empty() && g_slots[i].pad) targets.push_back({g_slots[i].pad, g_motors[i]});
            for (Slot& s : g_released)
                if (s.pad) targets.push_back({s.pad, Motors{}});
            g_released.clear();
        }
        if (max_players() == 1) {  // nobody joins and every pad drives player 1: they all play its vibration
            Motors m;
            {
                std::lock_guard lk{g_slots_lock};
                m = g_motors[0];
            }
            targets.clear();
            std::lock_guard lk{g_pads_lock};
            for (const auto& [id, p] : g_pads) targets.push_back({p, m});
        }
        std::lock_guard send{g_send_lock};
        if (g_stopped) return;
        const u64 now = now_ms();
        for (const auto& [pad, motors] : targets) {
            const bool refresh = motors != Motors{} && now - pad->rumble_at >= RumbleMs / 2;
            if (motors != pad->rumble || refresh) {
                send_rumble(*pad, motors);
                pad->rumble = motors;
                pad->rumble_at = now;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(15));
    }
}

// The pad leaving `player` stops, and the next pad there starts still.
void release(int player) {
    g_released.push_back(g_slots[player]);
    g_motors[player] = {};
    g_motors_changed = true;
    g_motors_cv.notify_one();
}

}  // namespace

void on_sdl_event(const void* event) {
    const auto* e = static_cast<const SDL_Event*>(event);
    if (e->type == SDL_EVENT_GAMEPAD_ADDED) {
        SDL_Gamepad* g = SDL_OpenGamepad(e->gdevice.which);
        if (!g) {
            LOG("hid: a pad could not be opened: %s", SDL_GetError());
            return;
        }
        auto pad = std::make_shared<Pad>();
        pad->gamepad = g;
        pad->key = "sdl:" + std::to_string(e->gdevice.which);
        const char* name = SDL_GetGamepadName(g);
        pad->name = name ? name : "Gamepad";
        SDL_SetGamepadPlayerIndex(g, -1);
        LOG("hid: %s connected (%s)", pad->name.c_str(), SDL_GetGamepadSerial(g) ? "serial known" : pad->key.c_str());
        std::lock_guard lk{g_pads_lock};
        g_pads[e->gdevice.which] = std::move(pad);
    } else if (e->type == SDL_EVENT_GAMEPAD_REMOVED) {
        std::shared_ptr<Pad> pad;
        {
            std::lock_guard lk{g_pads_lock};
            auto it = g_pads.find(e->gdevice.which);
            if (it == g_pads.end()) return;
            pad = std::move(it->second);
            g_pads.erase(it);
        }
        LOG("hid: %s disconnected", pad->name.c_str());
        // Closed once the vibration thread and the slots let go of it too: they only hold it under their locks
        std::lock_guard send{g_send_lock};
        std::lock_guard slots{g_slots_lock};
        for (Slot& s : g_slots)
            if (s.pad == pad) s.pad = nullptr;
        for (Slot& s : g_released)
            if (s.pad == pad) s.pad = nullptr;
        SDL_CloseGamepad(pad->gamepad);
        pad->gamepad = nullptr;
    }
}

void start() {
    static std::atomic<bool> started{false};
    if (started.exchange(true)) return;
    std::thread(send_vibration).detach();
}

void read(Players& out, const State* keyboard) {
    std::vector<Source> now;
    {
        std::lock_guard lk{g_pads_lock};
        for (const auto& [id, p] : g_pads) now.push_back({p->key, p->name, read_pad(p->gamepad), p});
    }
    if (keyboard) now.push_back({"keyboard", "Keyboard", *keyboard, nullptr});
    const auto find = [&](const std::string& key) {
        return std::find_if(now.begin(), now.end(), [&](const Source& s) { return s.key == key; });
    };
    out = {};
    if (max_players() == 1) {
        for (const Source& src : now) add(out.state[0], src.state);
        return;
    }

    std::lock_guard lk{g_slots_lock};
    const u64 t = now_ms();
    const auto joined = [&](const std::string& key) {
        return std::any_of(g_slots.begin(), g_slots.end(), [&](const Slot& s) { return s.key == key; });
    };
    std::erase_if(g_presses, [&](const auto& kv) { return find(kv.first) == now.end(); });
    for (const Source& src : now) {
        Press& p = g_presses[src.key];
        if (src.state.buttons & ~p.buttons) {
            p.pressed = src.state.buttons;
            p.at_ms = t;
            p.pending = !joined(src.key);
        }
        p.buttons = src.state.buttons;
    }
    // The joined player whose press `mine` copies, or null
    const auto copy_of = [&](const Press& mine) -> const Slot* {
        for (const Slot& s : g_slots) {
            if (s.key.empty()) continue;
            const Press& other = g_presses[s.key];
            const u64 apart = mine.at_ms > other.at_ms ? mine.at_ms - other.at_ms : other.at_ms - mine.at_ms;
            if (other.pressed == mine.pressed && apart <= MirrorMs) return &s;
        }
        return nullptr;
    };
    // Pads that are gone free their player
    for (int i = 0; i < MaxPlayers; i++) {
        Slot& s = g_slots[i];
        if (s.key.empty() || find(s.key) != now.end()) continue;
        LOG("hid: player %d left (%s disconnected)", i + 1, s.name.c_str());
        release(i);
        s = {};
    }
    // A pad sent away by leave() may join again once its buttons are up
    std::erase_if(g_left, [&](const std::string& key) {
        auto it = find(key);
        return it == now.end() || it->state.buttons == 0;
    });
    // Pads without a player join on a button press (MirrorMs later), as the lowest free player
    for (const Source& src : now) {
        Press& p = g_presses[src.key];
        if (!p.pending || t - p.at_ms < MirrorMs) continue;
        p.pending = false;
        if (joined(src.key) || std::find(g_left.begin(), g_left.end(), src.key) != g_left.end()) continue;
        const auto last = g_slots.begin() + max_players();
        auto free = std::find_if(g_slots.begin(), last, [](const Slot& s) { return s.key.empty(); });
        if (free == last) continue;
        if (const Slot* orig = copy_of(p)) {
            LOG("hid: %s pressed with %s: the same controller twice (Steam Input?), not a new player", src.name.c_str(),
                orig->name.c_str());
            continue;
        }
        *free = {src.key, src.name, src.pad, true};
        const int player = static_cast<int>(free - g_slots.begin());
        LOG("hid: player %d: %s", player + 1, src.name.c_str());
        if (src.pad) show_player(*src.pad, player);
    }
    for (int i = 0; i < MaxPlayers; i++) {
        Slot& s = g_slots[i];
        out.joined[i] = !s.key.empty();
        out.state[i] = {};
        if (s.key.empty()) continue;
        const State& st = find(s.key)->state;
        if (s.holding && st.buttons == 0) s.holding = false;
        if (!s.holding) out.state[i] = st;
    }
}

void leave(int player) {
    if (player < 0 || player >= MaxPlayers) return;
    std::lock_guard lk{g_slots_lock};
    Slot& s = g_slots[player];
    if (s.key.empty()) return;
    LOG("hid: player %d left (the game disconnected it): %s joins again on its next press", player + 1, s.name.c_str());
    g_left.push_back(s.key);
    if (s.pad && s.pad->gamepad) show_player(*s.pad, -1);
    release(player);
    s = {};
}

int connected_players() {
    std::lock_guard lk{g_slots_lock};
    return 1 + static_cast<int>(std::count_if(g_slots.begin() + 1, g_slots.end(), [](const Slot& s) { return !s.key.empty(); }));
}

void vibrate(int player, int motor, const Vibration& v) {
    if (player < 0 || player >= MaxPlayers || motor < 0 || motor > 1) return;
    std::lock_guard lk{g_slots_lock};
    if (g_motors[player][motor] == v) return;
    g_motors[player][motor] = v;
    g_motors_changed = true;
    g_motors_cv.notify_one();
}

void stop_vibration() {
    // Also called while crashing: it waits a little for the vibration thread to finish sending, and skips the pads when
    // the crashing thread may hold their list.
    std::unique_lock send{g_send_lock, std::chrono::milliseconds(300)};
    g_stopped = true;
    std::unique_lock lk{g_pads_lock, std::try_to_lock};
    if (!lk) return;
    for (const auto& [id, p] : g_pads)
        if (p->gamepad) SDL_RumbleGamepad(p->gamepad, 0, 0, 0);
}

}  // namespace gamepads
#endif
