// Gamepads without a library. Xbox pads: XInput (a slot without a pad is asked again only every 2 s: XInputGetState
// is slow for empty slots). DualShock 4 and DualSense: their input reports read from the HID device, one thread per
// pad; over Bluetooth, reading the calibration feature report switches them from the short report to the full one.
// Switch Pro Controller: subcommands switch it to its full report and read its stick calibration (see below).
// Buttons by position as on the Switch: the bottom face button (Cross, Xbox A) is B, the right one A.
// Vibration: one thread sends each player's latest motors to its pad.
#include <windows.h>

#include <Xinput.h>
#include <hidsdi.h>
#include <setupapi.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "../common.h"
#include "gamepads.h"

#ifdef _MSC_VER   // (mingw: tools/build_host.py links xinput1_4; its libxinput.a is XINPUT1_3.dll, not in Windows)
#pragma comment(lib, "xinput.lib")
#endif
#pragma comment(lib, "hid.lib")
#pragma comment(lib, "setupapi.lib")

namespace gamepads {
namespace {

using namespace hid;
constexpr s32 StickMax = 0x7fff;

// Radial dead zone of 12%, then the rest of the range stretched to full.
Stick radial(float x, float y) {
    const float mag = std::sqrt(x * x + y * y);
    constexpr float dead = 0.12f;
    if (mag < dead) return {};
    const float scale = std::min(1.0f, (mag - dead) / (1.0f - dead)) / mag;
    const auto c = [&](float v) { return static_cast<s32>(std::clamp(v * scale, -1.0f, 1.0f) * StickMax); };
    return {c(x), c(y)};
}

struct PsPad;

// One connected pad this tick. `key` names it for as long as it stays connected.
struct Source {
    std::wstring key;
    std::string name;
    State state;
    std::shared_ptr<PsPad> pad;  // HID pads (player lights, vibration)
    int xinput = -1;             // XInput slot
};

// ------------------------------------------------------------------ XInput

std::atomic<u32> g_xinput_slots{0};  // bit per slot that had a pad at the last look

void read_xinput(std::vector<Source>& out) {
    const u32 slots = g_xinput_slots.load(std::memory_order_relaxed);
    for (DWORD i = 0; i < XUSER_MAX_COUNT; ++i) {
        if (!(slots & (1u << i))) continue;
        XINPUT_STATE st{};
        if (XInputGetState(i, &st) != ERROR_SUCCESS) {
            g_xinput_slots.fetch_and(~(1u << i));
            LOG("hid: XInput pad %lu disconnected", i);
            continue;
        }
        const XINPUT_GAMEPAD& g = st.Gamepad;
        State s;
        const auto b = [&](WORD mask, u64 n) {
            if (g.wButtons & mask) s.buttons |= n;
        };
        b(XINPUT_GAMEPAD_A, B);
        b(XINPUT_GAMEPAD_B, A);
        b(XINPUT_GAMEPAD_X, Y);
        b(XINPUT_GAMEPAD_Y, X);
        b(XINPUT_GAMEPAD_LEFT_SHOULDER, L);
        b(XINPUT_GAMEPAD_RIGHT_SHOULDER, R);
        b(XINPUT_GAMEPAD_START, Plus);
        b(XINPUT_GAMEPAD_BACK, Minus);
        b(XINPUT_GAMEPAD_LEFT_THUMB, StickL);
        b(XINPUT_GAMEPAD_RIGHT_THUMB, StickR);
        b(XINPUT_GAMEPAD_DPAD_LEFT, Left);
        b(XINPUT_GAMEPAD_DPAD_UP, Up);
        b(XINPUT_GAMEPAD_DPAD_RIGHT, Right);
        b(XINPUT_GAMEPAD_DPAD_DOWN, Down);
        if (g.bLeftTrigger > XINPUT_GAMEPAD_TRIGGER_THRESHOLD) s.buttons |= ZL;
        if (g.bRightTrigger > XINPUT_GAMEPAD_TRIGGER_THRESHOLD) s.buttons |= ZR;
        s.l = radial(g.sThumbLX / 32767.0f, g.sThumbLY / 32767.0f);
        s.r = radial(g.sThumbRX / 32767.0f, g.sThumbRY / 32767.0f);
        out.push_back({L"xinput:" + std::to_wstring(i), "Xbox pad " + std::to_string(i + 1), s, nullptr, int(i)});
    }
}

void scan_xinput() {
    const u32 slots = g_xinput_slots.load();
    for (DWORD i = 0; i < XUSER_MAX_COUNT; ++i) {
        if (slots & (1u << i)) continue;
        XINPUT_STATE st{};
        if (XInputGetState(i, &st) == ERROR_SUCCESS) {
            g_xinput_slots.fetch_or(1u << i);
            LOG("hid: XInput pad %lu connected", i);
        }
    }
}

// ------------------------------------------------------------------ DualShock 4 / DualSense

// The report fields both pads share, wherever the report has them.
struct PsReport {
    u8 lx, ly, rx, ry, l2, r2, b0, b1, b2;
};

// Report layouts: DualSense full (USB 0x01, 64 bytes; Bluetooth 0x31, one byte further): sticks, triggers, a counter,
// then the buttons. Short (DualSense and DualShock 4 over Bluetooth before the switch, DualShock 4 over USB 0x01):
// sticks, buttons, triggers. DualShock 4 Bluetooth full (0x11): the short layout two bytes further.
bool parse(const u8* p, DWORD n, bool dualsense, PsReport& r) {
    const auto full = [&](u32 at) { r = {p[at], p[at + 1], p[at + 2], p[at + 3], p[at + 4], p[at + 5], p[at + 7], p[at + 8], p[at + 9]}; };
    const auto brief = [&](u32 at) { r = {p[at], p[at + 1], p[at + 2], p[at + 3], p[at + 7], p[at + 8], p[at + 4], p[at + 5], p[at + 6]}; };
    if (n < 10) return false;
    if (dualsense) {
        if (p[0] == 0x01 && n >= 64) full(1);
        else if (p[0] == 0x31 && n >= 12) full(2);
        else if (p[0] == 0x01) brief(1);
        else return false;
    } else {
        if (p[0] == 0x11 && n >= 12) brief(3);
        else if (p[0] == 0x01) brief(1);
        else return false;
    }
    return true;
}

State to_state(const PsReport& r) {
    State s;
    const u32 hat = r.b0 & 0xf;  // 0 up, clockwise in eighths; 8 centered
    if (hat == 7 || hat == 0 || hat == 1) s.buttons |= Up;
    if (hat >= 1 && hat <= 3) s.buttons |= Right;
    if (hat >= 3 && hat <= 5) s.buttons |= Down;
    if (hat >= 5 && hat <= 7) s.buttons |= Left;
    if (r.b0 & 0x10) s.buttons |= Y;  // Square
    if (r.b0 & 0x20) s.buttons |= B;  // Cross
    if (r.b0 & 0x40) s.buttons |= A;  // Circle
    if (r.b0 & 0x80) s.buttons |= X;  // Triangle
    if (r.b1 & 0x01) s.buttons |= L;
    if (r.b1 & 0x02) s.buttons |= R;
    if (r.b1 & 0x10) s.buttons |= Minus;  // Create / Share
    if (r.b1 & 0x20) s.buttons |= Plus;   // Options
    if (r.b1 & 0x40) s.buttons |= StickL;
    if (r.b1 & 0x80) s.buttons |= StickR;
    if (r.b2 & 0x02) s.buttons |= Minus;  // touchpad click
    constexpr u8 trigger = 62;            // about a quarter
    if (r.l2 > trigger) s.buttons |= ZL;
    if (r.r2 > trigger) s.buttons |= ZR;
    const auto ax = [](u8 v) { return (static_cast<int>(v) - 128) / 127.0f; };
    s.l = radial(ax(r.lx), -ax(r.ly));  // the report's Y axes point down
    s.r = radial(ax(r.rx), -ax(r.ry));
    return s;
}

// One stick's calibration from the Pro Controller's flash: center and the reach below and above it, 12-bit units
struct StickCal {
    s32 cx = 2048, cy = 2048, x_below = 2048, x_above = 2048, y_below = 2048, y_above = 2048;
};

struct PsPad {
    std::wstring path;
    std::string name;
    HANDLE handle = INVALID_HANDLE_VALUE;
    HANDLE out = INVALID_HANDLE_VALUE;  // overlapped, for output reports: `handle` itself on the Pro Controller
    bool dualsense = false;
    u32 report_size = 0, out_size = 0;
    // Switch Pro Controller: the handle is overlapped (replies are waited for with a timeout)
    bool nintendo = false, usb = false;
    std::atomic<u8> counter{0};
    std::atomic<bool> ready{false};      // set up (full reports, vibration on): rumble reports can be sent
    std::atomic<int> player{-1};         // the player it joined as (-1: not joined)
    std::atomic<bool> lights_due{true};  // its reader thread sends `player`'s light (show_player sets it)
    StickCal cal[2];
    std::atomic<bool> alive{true};
    std::mutex lock;
    State state;
    std::array<Vibration, 2> rumble{};  // last sent (the vibration thread's)
    ULONGLONG rumble_at = 0;

    // Closed only once nothing holds the pad: the vibration thread may still be writing to it.
    ~PsPad() {
        if (out != handle && out != INVALID_HANDLE_VALUE) CloseHandle(out);
        if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
    }
};

std::mutex g_pads_lock;
std::vector<std::shared_ptr<PsPad>> g_pads;

void read_reports(std::shared_ptr<PsPad> pad) {
    SetThreadDescription(GetCurrentThread(), L"gamepad");
    std::vector<u8> buf(std::max<u32>(pad->report_size, 64));
    bool logged = false;
    for (;;) {
        DWORD n = 0;
        if (!ReadFile(pad->handle, buf.data(), static_cast<DWORD>(buf.size()), &n, nullptr)) break;
        PsReport r;
        if (!parse(buf.data(), n, pad->dualsense, r)) {
            if (!logged) LOG("hid: %s: report 0x%02x of %lu bytes not understood", pad->name.c_str(), buf[0], n);
            logged = true;
            continue;
        }
        const State s = to_state(r);
        std::lock_guard lk{pad->lock};
        pad->state = s;
    }
    LOG("hid: gamepad disconnected: %s", pad->name.c_str());
    {
        std::lock_guard lk{pad->lock};
        pad->state = {};
    }
    pad->alive = false;
}

// ------------------------------------------------------------------ Switch Pro Controller
// The protocol as Linux's hid-nintendo and SDL's HIDAPI Switch driver use it (documented in dekuNukem's
// Nintendo_Switch_Reverse_Engineering). Output report 0x01: a 4-bit counter, 8 bytes of rumble (neutral), then a
// subcommand and its arguments; the pad answers with input report 0x21 (byte 14: the subcommand). Subcommand 0x03
// with 0x30 switches it to the full report, 0x10 reads its SPI flash (the stick calibration), 0x30 sets the player
// lights. Over USB the pad first needs the 0x80 commands (handshake, baud rate, handshake, USB only).

// Overlapped read or write that gives up after `ms`: false on error or timeout. The pad's reader thread does its I/O
// on the handle, and the vibration thread writes a Pro Controller's rumble reports to it too; each thread has its own
// event (ReadFile and WriteFile reset it when they start).
bool hid_io(HANDLE h, bool write, u8* buf, DWORD size, DWORD& n, DWORD ms) {
    struct Event {
        HANDLE h = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        ~Event() { CloseHandle(h); }
    };
    thread_local Event ev;
    OVERLAPPED ov{};
    ov.hEvent = ev.h;
    BOOL ok = write ? WriteFile(h, buf, size, nullptr, &ov) : ReadFile(h, buf, size, nullptr, &ov);
    if (!ok && GetLastError() == ERROR_IO_PENDING) {
        if (WaitForSingleObject(ov.hEvent, ms) != WAIT_OBJECT_0) CancelIoEx(h, &ov);
        ok = GetOverlappedResult(h, &ov, &n, TRUE);
    } else if (ok) {
        ok = GetOverlappedResult(h, &ov, &n, TRUE);
    }
    return ok;
}

// Waits for an input report starting with `id` (and `sub` at byte 14 for a 0x21 reply), at most ~`ms`
bool await_report(PsPad& p, u8 id, int sub, std::vector<u8>& in, DWORD ms) {
    const ULONGLONG end = GetTickCount64() + ms;
    for (ULONGLONG now; (now = GetTickCount64()) < end;) {
        DWORD n = 0;
        if (!hid_io(p.handle, false, in.data(), static_cast<DWORD>(in.size()), n, static_cast<DWORD>(end - now))) return false;
        if (n > 0 && in[0] == id && (sub < 0 || (n > 14 && in[14] == sub))) return true;
    }
    return false;
}

bool send_usb(PsPad& p, u8 cmd, bool reply) {
    std::vector<u8> out(p.out_size), in(std::max<u32>(p.report_size, 64));
    out[0] = 0x80;
    out[1] = cmd;
    DWORD n = 0;
    if (!hid_io(p.handle, true, out.data(), static_cast<DWORD>(out.size()), n, 500)) return false;
    return !reply || await_report(p, 0x81, -1, in, 500);
}

// Sends a subcommand without waiting for its 0x21 reply
bool write_subcommand(PsPad& p, u8 id, std::initializer_list<u8> args) {
    static constexpr u8 rumble[8] = {0x00, 0x01, 0x40, 0x40, 0x00, 0x01, 0x40, 0x40};  // neutral
    std::vector<u8> out(p.out_size);
    out[0] = 0x01;
    out[1] = p.counter++ & 0xf;
    std::copy(std::begin(rumble), std::end(rumble), out.begin() + 2);
    out[10] = id;
    std::copy(args.begin(), args.end(), out.begin() + 11);
    DWORD n = 0;
    return hid_io(p.handle, true, out.data(), static_cast<DWORD>(out.size()), n, 500);
}

// The player lights (subcommand 0x30: bit n lights LED n+1; none before it joins). The reply arrives as an ordinary
// 0x21 report in the reader loop.
void send_lights(PsPad& p) {
    const int player = p.player;
    write_subcommand(p, 0x30, {u8(player < 0 ? 0 : 1u << player)});
}

// A subcommand, retried 3 times; `reply` gets the 0x21 report
bool subcommand(PsPad& p, u8 id, std::initializer_list<u8> args, std::vector<u8>* reply = nullptr) {
    std::vector<u8> in(std::max<u32>(p.report_size, 64));
    for (int attempt = 0; attempt < 3; attempt++) {
        if (!write_subcommand(p, id, args)) continue;
        if (await_report(p, 0x21, id, in, 500)) {
            if (reply) *reply = in;
            return true;
        }
    }
    return false;
}

// SPI flash read: `size` bytes at `addr` (at most 0x1d a read)
bool read_spi(PsPad& p, u32 addr, u8 size, u8* to) {
    std::vector<u8> r;
    if (!subcommand(p, 0x10, {u8(addr), u8(addr >> 8), u8(addr >> 16), u8(addr >> 24), size}, &r)) return false;
    // Reply: byte 13 ack, 14 subcommand, 15..18 address, 19 size, then the data
    if (r.size() < 20u + size || r[19] != size) return false;
    std::copy(r.begin() + 20, r.begin() + 20 + size, to);
    return true;
}

// 9 bytes of stick calibration -> six 12-bit values
void unpack12(const u8* d, u16 v[6]) {
    for (int i = 0; i < 3; i++) {
        v[i * 2] = static_cast<u16>(((d[i * 3 + 1] << 8) & 0xf00) | d[i * 3]);
        v[i * 2 + 1] = static_cast<u16>((d[i * 3 + 2] << 4) | (d[i * 3 + 1] >> 4));
    }
}

// The left stick stores (above, center, below), the right one (center, below, above)
StickCal left_cal(const u8* d) {
    u16 v[6];
    unpack12(d, v);
    return {v[2], v[3], v[4], v[0], v[5], v[1]};
}

StickCal right_cal(const u8* d) {
    u16 v[6];
    unpack12(d, v);
    return {v[0], v[1], v[2], v[4], v[3], v[5]};
}

// User calibration (0x8010 left, 0x801b right, each after the 0xb2 0xa1 marker) when set, else the factory one
// (0x603d left, 0x6046 right). Without either the raw 12-bit range is used.
void read_calibration(PsPad& p) {
    u8 factory[18], user[22];
    const bool have_factory = read_spi(p, 0x603d, sizeof factory, factory);
    const bool have_user = read_spi(p, 0x8010, sizeof user, user);
    if (have_factory) {
        p.cal[0] = left_cal(factory);
        p.cal[1] = right_cal(factory + 9);
    }
    if (have_user && user[0] == 0xb2 && user[1] == 0xa1) p.cal[0] = left_cal(user + 2);
    if (have_user && user[11] == 0xb2 && user[12] == 0xa1) p.cal[1] = right_cal(user + 13);
    if (!have_factory && !have_user) LOG("hid: %s: stick calibration not read: raw range", p.name.c_str());
}

float cal_axis(s32 v, s32 center, s32 below, s32 above) {
    const s32 d = v - center;
    return std::clamp(d / static_cast<float>(d < 0 ? std::max(below, 1) : std::max(above, 1)), -1.0f, 1.0f);
}

Stick switch_stick(const u8* d, const StickCal& c) {
    const s32 x = d[0] | ((d[1] & 0xf) << 8), y = (d[1] >> 4) | (d[2] << 4);  // up is positive already
    return radial(cal_axis(x, c.cx, c.x_below, c.x_above), cal_axis(y, c.cy, c.y_below, c.y_above));
}

// Report 0x30 (and the same first 13 bytes of 0x21): 3 right buttons, 4 shared, 5 left, 6..8 left stick, 9..11 right
State switch_state(const u8* p, const PsPad& pad) {
    State s;
    const auto b = [&](u8 byte, u8 mask, u64 n) {
        if (byte & mask) s.buttons |= n;
    };
    b(p[3], 0x01, Y), b(p[3], 0x02, X), b(p[3], 0x04, B), b(p[3], 0x08, A), b(p[3], 0x40, R), b(p[3], 0x80, ZR);
    b(p[4], 0x01, Minus), b(p[4], 0x02, Plus), b(p[4], 0x04, StickR), b(p[4], 0x08, StickL);
    b(p[5], 0x01, Down), b(p[5], 0x02, Up), b(p[5], 0x04, Right), b(p[5], 0x08, Left), b(p[5], 0x40, L), b(p[5], 0x80, ZL);
    s.l = switch_stick(p + 6, pad.cal[0]);
    s.r = switch_stick(p + 9, pad.cal[1]);
    return s;
}

void read_switch(std::shared_ptr<PsPad> pad) {
    SetThreadDescription(GetCurrentThread(), L"gamepad");
    PsPad& p = *pad;
    if (p.usb) {
        const bool ok = send_usb(p, 0x02, true) && send_usb(p, 0x03, true) && send_usb(p, 0x02, true) && send_usb(p, 0x04, false);
        if (!ok) LOG("hid: %s: USB handshake not answered", p.name.c_str());
    }
    read_calibration(p);
    if (!subcommand(p, 0x03, {0x30})) LOG("hid: %s: full report mode not acknowledged", p.name.c_str());
    if (!subcommand(p, 0x48, {0x01})) LOG("hid: %s: vibration not enabled", p.name.c_str());
    p.ready = true;
    LOG("hid: gamepad ready: %s", p.name.c_str());

    std::vector<u8> buf(std::max<u32>(p.report_size, 64));
    bool logged = false;
    ULONGLONG last_reset = 0;
    for (;;) {
        // A new player light (joined or left): sent from here, so the hid thread never waits on the pad. The full
        // report comes 60-120 times a second, so it goes out within a few ms.
        if (p.lights_due.exchange(false)) send_lights(p);
        DWORD n = 0;
        if (!hid_io(p.handle, false, buf.data(), static_cast<DWORD>(buf.size()), n, INFINITE)) break;
        // 0x3f: the pad is back in its simple HID mode (it was reset: a Bluetooth reconnect, or another program
        // such as Steam talking to it). Full reports again, and its player light; at most once a second.
        if (n > 0 && buf[0] == 0x3f) {
            if (GetTickCount64() - last_reset >= 1000) {
                last_reset = GetTickCount64();
                const bool ok = subcommand(p, 0x03, {0x30});
                subcommand(p, 0x48, {0x01});
                p.lights_due = true;
                LOG("hid: %s: back in simple mode, full reports %s", p.name.c_str(), ok ? "again" : "not acknowledged");
            }
            continue;
        }
        if (n < 12 || (buf[0] != 0x30 && buf[0] != 0x21)) {
            if (!logged && n > 0) LOG("hid: %s: report 0x%02x of %lu bytes not understood", p.name.c_str(), buf[0], n);
            logged = true;
            continue;
        }
        const State s = switch_state(buf.data(), p);
        std::lock_guard lk{p.lock};
        p.state = s;
    }
    LOG("hid: gamepad disconnected: %s", p.name.c_str());
    {
        std::lock_guard lk{p.lock};
        p.state = {};
    }
    p.alive = false;
}

bool is_open(const std::wstring& path) {
    std::lock_guard lk{g_pads_lock};
    return std::any_of(g_pads.begin(), g_pads.end(), [&](const auto& p) { return p->alive && p->path == path; });
}

void add_pad(std::shared_ptr<PsPad> pad) {
    std::lock_guard lk{g_pads_lock};
    std::erase_if(g_pads, [](const auto& p) { return !p->alive; });
    g_pads.push_back(pad);
}

// Switch Pro Controller (Nintendo 0x057e, 0x2009): needs writing (subcommands), so read-write and overlapped
void open_switch(const std::wstring& path) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                           FILE_FLAG_OVERLAPPED, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    HIDD_ATTRIBUTES attr{sizeof attr};
    HIDP_CAPS caps{};
    PHIDP_PREPARSED_DATA pre = nullptr;
    bool ok = HidD_GetAttributes(h, &attr) && attr.VendorID == 0x057e && attr.ProductID == 0x2009 && HidD_GetPreparsedData(h, &pre);
    if (ok) {
        ok = HidP_GetCaps(pre, &caps) == HIDP_STATUS_SUCCESS && caps.UsagePage == 1 && caps.OutputReportByteLength >= 12;
        HidD_FreePreparsedData(pre);
    }
    if (!ok) {
        CloseHandle(h);
        return;
    }
    auto pad = std::make_shared<PsPad>();
    pad->path = path;
    pad->handle = h;
    pad->out = h;
    pad->nintendo = true;
    pad->report_size = caps.InputReportByteLength;
    pad->out_size = caps.OutputReportByteLength;
    pad->usb = caps.InputReportByteLength <= 64;  // the Bluetooth descriptor declares larger reports
    pad->name = std::string("Pro Controller") + (pad->usb ? " (USB)" : " (Bluetooth)");
    LOG("hid: gamepad connected: %s", pad->name.c_str());
    add_pad(pad);
    std::thread(read_switch, pad).detach();
}

void open_pad(const std::wstring& path) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    HIDD_ATTRIBUTES attr{sizeof attr};
    HIDP_CAPS caps{};
    PHIDP_PREPARSED_DATA pre = nullptr;
    bool ok = HidD_GetAttributes(h, &attr) && attr.VendorID == 0x054c && HidD_GetPreparsedData(h, &pre);
    if (ok) {
        ok = HidP_GetCaps(pre, &caps) == HIDP_STATUS_SUCCESS && caps.UsagePage == 1 && caps.Usage == 5;  // a gamepad
        HidD_FreePreparsedData(pre);
    }
    const u16 pid = attr.ProductID;
    const bool dualsense = pid == 0x0ce6 || pid == 0x0df2;
    const bool ds4 = pid == 0x05c4 || pid == 0x09cc || pid == 0x0ba0;
    if (!ok || (!dualsense && !ds4)) {
        CloseHandle(h);
        return;
    }
    auto pad = std::make_shared<PsPad>();
    pad->path = path;
    pad->handle = h;
    pad->dualsense = dualsense;
    pad->report_size = caps.InputReportByteLength;
    pad->out_size = caps.OutputReportByteLength;
    pad->out = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                           FILE_FLAG_OVERLAPPED, nullptr);
    const bool bluetooth = caps.InputReportByteLength > 64;
    pad->name = std::string(pid == 0x0df2 ? "DualSense Edge" : dualsense ? "DualSense" : "DualShock 4") +
                (bluetooth ? " (Bluetooth)" : " (USB)");
    if (bluetooth) {
        // The calibration report; reading it makes the pad send full reports (0x31 / 0x11).
        std::vector<u8> feature(std::max<u32>(caps.FeatureReportByteLength, 64));
        feature[0] = dualsense ? 0x05 : 0x02;
        if (!HidD_GetFeature(h, feature.data(), static_cast<ULONG>(feature.size())))
            LOG("hid: %s: calibration report not read (error %lu): short reports only", pad->name.c_str(), GetLastError());
    }
    LOG("hid: gamepad connected: %s", pad->name.c_str());
    add_pad(pad);
    std::thread(read_reports, pad).detach();
}

void scan_hid() {
    GUID guid;
    HidD_GetHidGuid(&guid);
    HDEVINFO set = SetupDiGetClassDevsW(&guid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (set == INVALID_HANDLE_VALUE) return;
    SP_DEVICE_INTERFACE_DATA iface{sizeof iface};
    for (DWORD i = 0; SetupDiEnumDeviceInterfaces(set, nullptr, &guid, i, &iface); i++) {
        DWORD size = 0;
        SetupDiGetDeviceInterfaceDetailW(set, &iface, nullptr, 0, &size, nullptr);
        if (!size) continue;
        std::vector<u8> buf(size);
        auto* detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(buf.data());
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        if (!SetupDiGetDeviceInterfaceDetailW(set, &iface, detail, size, nullptr, nullptr)) continue;
        std::wstring path = detail->DevicePath;
        std::wstring lower = path;
        std::transform(lower.begin(), lower.end(), lower.begin(), ::towlower);
        // Sony's and Nintendo's vendor ids, in the path of USB and Bluetooth HID devices alike
        const bool sony = lower.find(L"054c") != std::wstring::npos;
        const bool nintendo = lower.find(L"057e") != std::wstring::npos && lower.find(L"2009") != std::wstring::npos;
        if ((!sony && !nintendo) || is_open(path)) continue;
        if (nintendo) open_switch(path);
        else open_pad(path);
    }
    SetupDiDestroyDeviceInfoList(set);
}

void send_vibration();

}  // namespace

void start() {
    static std::atomic<bool> started{false};
    if (started.exchange(true)) return;
    scan_xinput();
    scan_hid();
    std::thread(send_vibration).detach();
    std::thread([] {
        SetThreadDescription(GetCurrentThread(), L"gamepad scan");
        for (;;) {
            Sleep(2000);
            scan_xinput();
            scan_hid();
            // Disconnected pads go (their handles close once the vibration thread and the slots let go of them too)
            std::lock_guard lk{g_pads_lock};
            std::erase_if(g_pads, [](const auto& p) { return !p->alive; });
        }
    }).detach();
}

namespace {

// The pad's player; a Pro Controller's reader thread lights it (this never waits on the pad).
void show_player(PsPad& p, int player) {
    p.player = player;
    p.lights_due = true;
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

// When each pad last pressed a button. One controller can show up twice: Steam Input or DS4Windows add a virtual
// Xbox pad and the controller is still read through HID. The copy presses what the original presses, a few ms
// apart (either one first), so a pad joins MirrorMs after its press, and not when a joined player pressed the same
// buttons within MirrorMs of it.
struct Press {
    u64 buttons = 0;     // now
    u64 pressed = 0;     // the buttons down when one last went down
    u64 at_ms = 0;       // when that was
    bool pending = false;  // a press of a pad without a player: it joins once MirrorMs have passed
};
constexpr u64 MirrorMs = 60;

u64 now_ms() {
    return static_cast<u64>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now().time_since_epoch()).count());
}

struct Slot {
    std::wstring key;  // empty: free
    std::string name;
    std::shared_ptr<PsPad> pad;
    int xinput = -1;
    bool holding = false;  // the joining press: nothing reaches the game until every button is up
};

std::mutex g_slots_lock;
std::array<Slot, MaxPlayers> g_slots;
std::vector<std::wstring> g_left;  // pads sent away by leave(): they join again on a new press, not the one held now
std::unordered_map<std::wstring, Press> g_presses;

// ------------------------------------------------------------------ Vibration

using Motors = std::array<Vibration, 2>;

// Guarded by g_slots_lock: each player's motors as the game last set them, the pads that left a player (they stop),
// and whether anything changed since the vibration thread last looked.
std::array<Motors, MaxPlayers> g_motors{};
std::vector<Slot> g_released;
bool g_motors_changed = false;
std::condition_variable g_motors_cv;

std::timed_mutex g_send_lock;  // one sender at a time; stop_vibration() ends the sending for good
std::atomic<bool> g_stopped{false};

// HD rumble amplitude, 0 (none) to 100 (1.0, the safe maximum), as in dekuNukem's rumble_data_table.md: its formulas
// above 0.12, and below that its table's quarter-octave steps from 0.011823 (step 2).
int rumble_amp(float a) {
    if (a <= 0) return 0;
    const float e = a > 0.23f ? std::log2(a * 8.7f) * 32 : a > 0.12f ? std::log2(a * 17) * 16 : std::log2(a / 0.011823f) * 4 + 2;
    return static_cast<int>(std::clamp(std::lround(e), 0L, 100L));
}

int rumble_freq(float hz, int lo, int hi) {
    return static_cast<int>(std::clamp(std::lround(std::log2(std::max(hz, 1.0f) / 10) * 32), long(lo), long(hi)));
}

// One motor's 4 bytes: 9-bit high-band frequency, high-band amplitude, 7-bit low-band frequency, 9-bit low-band
// amplitude (its low bit is byte 2's top bit). Neutral: 00 01 40 40.
void encode_rumble(const Vibration& v, u8* d) {
    const int hf = (rumble_freq(v.freq_high, 0x61, 0xdf) - 0x60) * 4, lf = rumble_freq(v.freq_low, 0x41, 0xbf) - 0x40;
    const int lf_amp = rumble_amp(v.amp_low) + 0x80;
    d[0] = static_cast<u8>(hf);
    d[1] = static_cast<u8>(rumble_amp(v.amp_high) * 2 | hf >> 8);
    d[2] = static_cast<u8>(lf | (lf_amp & 1) << 7);
    d[3] = static_cast<u8>(lf_amp >> 1);
}

// Bluetooth output reports of the PlayStation pads end in a CRC-32 of 0xa2 (the HID header) and the report.
u32 bt_crc(const u8* p, size_t n) {
    u32 c = ~0u;
    const auto add = [&](u8 b) {
        c ^= b;
        for (int k = 0; k < 8; k++) c = c >> 1 ^ (0xedb88320u & (0u - (c & 1)));
    };
    add(0xa2);
    for (size_t i = 0; i < n; i++) add(p[i]);
    return ~c;
}

// The Pro Controller plays both bands of each motor (report 0x10). The PlayStation pads have a big motor (the low
// band) and a small one (the high band); report layouts as in Linux's hid-playstation.
bool send_rumble(PsPad& p, const Motors& m) {
    if (!p.alive || p.out == INVALID_HANDLE_VALUE) return false;
    std::vector<u8> r(p.out_size);
    if (p.nintendo) {
        if (!p.ready || r.size() < 10) return false;
        r[0] = 0x10;
        r[1] = p.counter++ & 0xf;
        encode_rumble(m[0], &r[2]);
        encode_rumble(m[1], &r[6]);
    } else {
        const auto level = [](float a) { return static_cast<u8>(std::clamp(a, 0.0f, 1.0f) * 255 + 0.5f); };
        const u8 big = level(std::max(m[0].amp_low, m[1].amp_low)), small = level(std::max(m[0].amp_high, m[1].amp_high));
        const bool bluetooth = p.report_size > 64;
        if (r.size() < (bluetooth ? 78u : 8u)) return false;
        if (p.dualsense && bluetooth) r[0] = 0x31, r[1] = static_cast<u8>(p.counter++ << 4), r[2] = 0x10, r[3] = 0x03, r[5] = small, r[6] = big;
        else if (p.dualsense) r[0] = 0x02, r[1] = 0x03, r[3] = small, r[4] = big;
        else if (bluetooth) r[0] = 0x11, r[1] = 0xc4, r[3] = 0x01, r[6] = small, r[7] = big;
        else r[0] = 0x05, r[1] = 0x01, r[4] = small, r[5] = big;
        if (bluetooth) {
            const u32 crc = bt_crc(r.data(), 74);
            std::memcpy(&r[74], &crc, 4);
        }
    }
    DWORD n = 0;
    return hid_io(p.out, true, r.data(), static_cast<DWORD>(r.size()), n, 100);
}

void send_xinput(int slot, const Motors& m) {
    const auto level = [](float a) { return static_cast<WORD>(std::clamp(a, 0.0f, 1.0f) * 65535 + 0.5f); };
    XINPUT_VIBRATION v{level(std::max(m[0].amp_low, m[1].amp_low)), level(std::max(m[0].amp_high, m[1].amp_high))};
    XInputSetState(slot, &v);
}

// Sends what changed, at most every 15 ms. A Pro Controller's vibration is sent again every 50 ms while it lasts, as
// SDL does.
void send_vibration() {
    SetThreadDescription(GetCurrentThread(), L"vibration");
    std::array<Motors, XUSER_MAX_COUNT> xinput_sent{};
    for (;;) {
        std::vector<std::pair<Slot, Motors>> targets;
        {
            std::unique_lock lk{g_slots_lock};
            g_motors_cv.wait_for(lk, std::chrono::milliseconds(50), [] { return g_motors_changed; });
            g_motors_changed = false;
            for (int i = 0; i < MaxPlayers; i++)
                if (!g_slots[i].key.empty()) targets.push_back({g_slots[i], g_motors[i]});
            for (Slot& s : g_released) targets.push_back({std::move(s), Motors{}});
            g_released.clear();
        }
        if (max_players() == 1) {  // nobody joins and every pad drives player 1: they all play its vibration
            Motors m;
            {
                std::lock_guard lk{g_slots_lock};
                m = g_motors[0];
            }
            targets.clear();
            for (int i = 0; i < XUSER_MAX_COUNT; i++) targets.push_back({Slot{{}, {}, nullptr, i}, m});
            std::lock_guard lk{g_pads_lock};
            for (const auto& p : g_pads)
                if (p->alive) targets.push_back({Slot{{}, {}, p}, m});
        }
        std::lock_guard send{g_send_lock};
        if (g_stopped) return;
        const ULONGLONG now = GetTickCount64();
        for (const auto& [slot, motors] : targets) {
            if (slot.xinput >= 0) {
                if (motors != xinput_sent[slot.xinput]) send_xinput(slot.xinput, motors);
                xinput_sent[slot.xinput] = motors;
            } else if (PsPad* p = slot.pad.get()) {
                const bool refresh = p->nintendo && motors != Motors{} && now - p->rumble_at >= 50;
                if ((motors != p->rumble || refresh) && send_rumble(*p, motors)) {
                    p->rumble = motors;
                    p->rumble_at = now;
                }
            }
        }
        Sleep(15);
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

void read(Players& out, const State* keyboard) {
    std::vector<Source> now;
    read_xinput(now);
    {
        std::lock_guard lk{g_pads_lock};
        for (const auto& p : g_pads) {
            if (!p->alive) continue;
            std::lock_guard plk{p->lock};
            now.push_back({p->path, p->name, p->state, p});
        }
    }
    if (keyboard) now.push_back({L"keyboard", "Keyboard", *keyboard, nullptr});
    const auto find = [&](const std::wstring& key) {
        return std::find_if(now.begin(), now.end(), [&](const Source& s) { return s.key == key; });
    };
    out = {};
    if (max_players() == 1) {
        for (const Source& src : now) add(out.state[0], src.state);
        return;
    }

    std::lock_guard lk{g_slots_lock};
    const u64 t = now_ms();
    const auto joined = [&](const std::wstring& key) {
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
    std::erase_if(g_left, [&](const std::wstring& key) {
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
            LOG("hid: %s pressed with %s: the same controller twice (Steam Input, DS4Windows?), not a new player",
                src.name.c_str(), orig->name.c_str());
            continue;
        }
        *free = {src.key, src.name, src.pad, src.xinput, true};
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
    if (s.pad) show_player(*s.pad, -1);
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
    // Also called while crashing: it waits a little for the vibration thread to finish sending, and skips the HID
    // pads when the crashing thread may hold their list.
    std::unique_lock send{g_send_lock, std::chrono::milliseconds(300)};
    g_stopped = true;
    for (int i = 0; i < XUSER_MAX_COUNT; i++) send_xinput(i, {});
    std::unique_lock lk{g_pads_lock, std::try_to_lock};
    if (!lk) return;
    for (const auto& p : g_pads) send_rumble(*p, {});
}

}  // namespace gamepads
