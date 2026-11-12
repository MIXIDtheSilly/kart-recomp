// Gamepads: Xbox pads through XInput, DualShock 4 and DualSense (USB and Bluetooth) through their HID input reports.
#pragma once
#include "hid_shared.h"

namespace gamepads {

struct State {
    u64 buttons = 0;  // hid::NpadButton bits
    hid::Stick l, r;  // -0x7fff..0x7fff, up is positive
};

// Opens the pads connected now, and from then on those that connect (checked every 2 s).
void start();
// Adds every pad's state to `in`: buttons are combined, the stick pushed furthest wins.
void read(State& in);

}  // namespace gamepads
