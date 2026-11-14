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
// keeps its player while it stays connected.
constexpr int MaxPlayers = 4;
struct Players {
    std::array<State, MaxPlayers> state;
    std::array<bool, MaxPlayers> joined{};  // a pad holds that player
};

// Opens the pads connected now, and from then on those that connect (checked every 2 s).
void start();
// Every player's state (one call a tick: the joins happen here).
void read(Players& out);
// The pad of `player` (0-based) leaves; it joins again on its next button press (the game's DisconnectNpad).
void leave(int player);
// How many players are connected: player 1 always (the keyboard drives it without a pad), and each of 2-4 a pad holds.
int connected_players();

}  // namespace gamepads
