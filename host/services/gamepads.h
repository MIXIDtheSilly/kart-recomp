// Gamepads: Xbox pads through XInput; DualShock 4, DualSense and the Switch Pro Controller (USB and Bluetooth)
// through their HID input reports.
#pragma once
#include <array>

#include "hid_shared.h"

namespace gamepads {

struct State {
    u64 buttons = 0;  // hid::NpadButton bits
    hid::Stick l, r;  // -0x7fff..0x7fff, up is positive
};

// Up to 4 players, one pad each. A pad that connects waits until it presses a button, then joins as the lowest free
// player (the first pad to press is player 1); that press reaches the game only once every button is up again. A pad
// keeps its player while it stays connected. A pad that presses what a joined one presses, at the same moment, is that
// controller twice (Steam Input, DS4Windows) and doesn't join. HOST_PLAYERS=<n> caps the players; 1: every pad drives
// player 1 and nobody joins.
constexpr int MaxPlayers = 4;
struct Players {
    std::array<State, MaxPlayers> state;
    std::array<bool, MaxPlayers> joined{};  // a pad holds that player
};

// One motor's vibration as the game sends it (nn::hid::VibrationValue): a low and a high band, amplitude 0..1 and
// frequency in Hz.
struct Vibration {
    float amp_low = 0, freq_low = 160, amp_high = 0, freq_high = 320;
    bool operator==(const Vibration&) const = default;
};
static_assert(sizeof(Vibration) == 16);

// Opens the pads connected now, and from then on those that connect (checked every 2 s).
void start();
// Every player's state (one call a tick: the joins happen here). `keyboard`: the keyboard as one more pad, which joins
// like the others (HOST_KEYBOARD_JOINS=1), or null.
void read(Players& out, const State* keyboard = nullptr);
// The pad of `player` (0-based) leaves; it joins again on its next button press (the game's DisconnectNpad).
void leave(int player);
// How many players are connected: player 1 always (the keyboard drives it without a pad), and each of 2-4 a pad holds.
int connected_players();
// Vibration for motor 0 (left) or 1 (right) of `player`'s pad. Xbox and PlayStation pads have one low and one high
// band motor: they get the stronger of the two motors' bands.
void vibrate(int player, int motor, const Vibration& v);
// Stops every pad's vibration now (the game is closing).
void stop_vibration();

}  // namespace gamepads
