// nn::hid's shared memory: the parts the host writes (player entries, their input lifos) at the offsets the sdk reads,
// the rest left as bytes. Offsets and default contents as in Eden/yuzu's hid_core/resources/shared_memory_format.h.
#pragma once
#include <array>
#include <atomic>
#include <cstddef>
#include <cstring>
#include <new>

#include "../common.h"

namespace hid {

// nn::hid::NpadButton
enum NpadButton : u64 {
    A = 1ull << 0, B = 1ull << 1, X = 1ull << 2, Y = 1ull << 3, StickL = 1ull << 4, StickR = 1ull << 5,
    L = 1ull << 6, R = 1ull << 7, ZL = 1ull << 8, ZR = 1ull << 9, Plus = 1ull << 10, Minus = 1ull << 11,
    Left = 1ull << 12, Up = 1ull << 13, Right = 1ull << 14, Down = 1ull << 15,
    StickLLeft = 1ull << 16, StickLUp = 1ull << 17, StickLRight = 1ull << 18, StickLDown = 1ull << 19,
    StickRLeft = 1ull << 20, StickRUp = 1ull << 21, StickRRight = 1ull << 22, StickRDown = 1ull << 23,
};
// nn::hid::NpadStyleSet
enum NpadStyle : u32 { StyleFullkey = 1u << 0, StyleHandheld = 1u << 1 };

struct Stick {
    s32 x = 0, y = 0;
};

// nn::hid::detail::RingLifo: the newest entry is entries[buffer_tail].
template <class State, size_t N>
struct Lifo {
    s64 timestamp = 0;
    s64 total_buffer_count = static_cast<s64>(N);
    s64 buffer_tail = 0;
    s64 buffer_count = 0;
    struct Entry {
        s64 sampling_number;
        State state;
    } entries[N]{};
};
constexpr size_t EntryCount = 17;

// nn::hid::detail::NpadGenericState (attributes: 1 connected, 2 wired, 4/8 left connected/wired, 0x10/0x20 right)
struct NpadGenericState {
    s64 sampling_number = 0;
    u64 buttons = 0;
    Stick l, r;
    u32 attributes = 0;
    u32 reserved = 0;
};
static_assert(sizeof(NpadGenericState) == 0x28);
enum NpadAttribute : u32 {
    Connected = 1, Wired = 2, LeftConnected = 4, LeftWired = 8, RightConnected = 0x10, RightWired = 0x20,
};

struct NpadGcTriggerState {
    s64 sampling_number = 0;
    s32 l_analog = 0, r_analog = 0;
};

struct Color {
    u8 body[4], buttons[4];
};
constexpr u32 ColorOk = 0, ColorNoController = 2;

// nn::hid::detail::NpadInternalState
struct NpadInternalState {
    u32 style = 0;                     // NpadStyle bits
    u32 assignment_mode = 0;           // 0 dual
    u32 fullkey_color_attribute = ColorNoController;
    Color fullkey_color{};
    u32 joycon_color_attribute = ColorNoController;
    Color joycon_left{}, joycon_right{};
    Lifo<NpadGenericState, EntryCount> fullkey_lifo, handheld_lifo, joy_dual_lifo, joy_left_lifo, joy_right_lifo,
        palma_lifo, system_ext_lifo;
    Lifo<std::array<u8, 0x60>, EntryCount> sixaxis_lifos[6];  // SixAxisSensorState, not written
    u32 device_type = 0;               // 1 fullkey, 4 handheld left, 8 handheld right
    u32 reserved0 = 0;
    u64 system_properties = 0;         // 0x800 vertical, 0x2000 use plus, 0x4000 use minus, 0x8000 directional buttons
    u32 button_properties = 0;
    u32 battery_dual = 0, battery_left = 0, battery_right = 0;
    u32 applet_footer_attributes = 0;
    u8 applet_footer_type = 0;         // 4 handheld with both Joy-Cons, 12 Pro Controller
    u8 reserved1[0x5b + 0x20]{};
    Lifo<NpadGcTriggerState, EntryCount> gc_trigger_lifo;
    u32 lark_l = 0, lark_r = 0, lucia = 0, lager = 0;
    u8 sixaxis_properties[6]{};        // bit 0: newly assigned; fullkey, handheld, dual left/right, left, right
    u8 reserved2[2]{};
};
static_assert(offsetof(NpadInternalState, fullkey_color) == 0xc);
static_assert(offsetof(NpadInternalState, joycon_left) == 0x18);
static_assert(offsetof(NpadInternalState, fullkey_lifo) == 0x28);
static_assert(offsetof(NpadInternalState, handheld_lifo) == 0x378);
static_assert(offsetof(NpadInternalState, system_ext_lifo) == 0x1408);
static_assert(offsetof(NpadInternalState, sixaxis_lifos) == 0x1758);
static_assert(offsetof(NpadInternalState, device_type) == 0x4188);
static_assert(offsetof(NpadInternalState, system_properties) == 0x4190);
static_assert(offsetof(NpadInternalState, battery_dual) == 0x419c);
static_assert(offsetof(NpadInternalState, applet_footer_type) == 0x41ac);
static_assert(offsetof(NpadInternalState, gc_trigger_lifo) == 0x4228);
static_assert(offsetof(NpadInternalState, sixaxis_properties) == 0x43f0);
static_assert(sizeof(NpadInternalState) == 0x43f8);

// nn::hid::detail::SharedMemoryFormat, 256 KiB.
struct SharedMemoryFormat {
    static constexpr size_t NpadOffset = 0x9a00, NpadEntrySize = 0x5000, NpadConditionOffset = 0x3e200;
    alignas(8) u8 bytes[0x40000];

    // The contents nn::hid expects before any input: empty lifos that know their size, players without a controller.
    void init() {
        std::memset(bytes, 0, sizeof bytes);
        // The other sections' first lifo (debug pad, touch screen, mouse, keyboard, home, sleep and capture buttons,
        // gesture, debug mouse): total_buffer_count
        for (size_t at : {0x0, 0x400, 0x3400, 0x3800, 0x4c00, 0x4e00, 0x5000, 0x3ba00, 0x3dc00})
            *reinterpret_cast<s64*>(bytes + at + 8) = EntryCount;
        for (u32 i = 0; i < 10; i++) new (&npad(i)) NpadInternalState{};
        // NpadCondition: initialized, hold type horizontal, valid
        u32* cond = reinterpret_cast<u32*>(bytes + NpadConditionOffset);
        cond[1] = 1, cond[2] = 1, cond[3] = 1;
    }
    // Player 1-8 are 0-7, handheld is 8, other is 9.
    NpadInternalState& npad(u32 index) {
        return *reinterpret_cast<NpadInternalState*>(bytes + NpadOffset + index * NpadEntrySize);
    }
};

// Writes the next entry before moving the tail, so a reader never sees a half-written newest one.
template <class State, size_t N>
void lifo_push(Lifo<State, N>& lifo, State st) {
    st.sampling_number = lifo.entries[lifo.buffer_tail].state.sampling_number + 1;
    const size_t next = (lifo.buffer_tail + 1) % N;
    lifo.entries[next].sampling_number = st.sampling_number << 1;
    lifo.entries[next].state = st;
    std::atomic_thread_fence(std::memory_order_release);
    if (lifo.buffer_count < static_cast<s64>(N) - 1) lifo.buffer_count++;
    lifo.buffer_tail = next;
}

}  // namespace hid
