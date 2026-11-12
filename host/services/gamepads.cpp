// Gamepads without a library. Xbox pads: XInput (a slot without a pad is asked again only every 2 s: XInputGetState
// is slow for empty slots). DualShock 4 and DualSense: their input reports read from the HID device, one thread per
// pad; over Bluetooth, reading the calibration feature report switches them from the short report to the full one.
// Buttons by position as on the Switch: the bottom face button (Cross, Xbox A) is B, the right one A.
#include <windows.h>

#include <Xinput.h>
#include <hidsdi.h>
#include <setupapi.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "../common.h"
#include "gamepads.h"

#pragma comment(lib, "xinput.lib")
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

void merge(Stick& to, Stick s) {
    if (std::abs(s.x) + std::abs(s.y) > std::abs(to.x) + std::abs(to.y)) to = s;
}

void add(State& in, const State& s) {
    in.buttons |= s.buttons;
    merge(in.l, s.l);
    merge(in.r, s.r);
}

// ------------------------------------------------------------------ XInput

std::atomic<u32> g_xinput_slots{0};  // bit per slot that had a pad at the last look

void read_xinput(State& in) {
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
        add(in, s);
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

struct PsPad {
    std::wstring path;
    std::string name;
    HANDLE handle = INVALID_HANDLE_VALUE;
    bool dualsense = false;
    u32 report_size = 0;
    std::atomic<bool> alive{true};
    std::mutex lock;
    State state;
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
    CloseHandle(pad->handle);
    pad->alive = false;
}

bool is_open(const std::wstring& path) {
    std::lock_guard lk{g_pads_lock};
    return std::any_of(g_pads.begin(), g_pads.end(), [&](const auto& p) { return p->alive && p->path == path; });
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
    {
        std::lock_guard lk{g_pads_lock};
        std::erase_if(g_pads, [](const auto& p) { return !p->alive; });
        g_pads.push_back(pad);
    }
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
        // Sony's vendor id, in the path of USB and Bluetooth HID devices alike
        if (lower.find(L"054c") == std::wstring::npos || is_open(path)) continue;
        open_pad(path);
    }
    SetupDiDestroyDeviceInfoList(set);
}

}  // namespace

void start() {
    static std::atomic<bool> started{false};
    if (started.exchange(true)) return;
    scan_xinput();
    scan_hid();
    std::thread([] {
        SetThreadDescription(GetCurrentThread(), L"gamepad scan");
        for (;;) {
            Sleep(2000);
            scan_xinput();
            scan_hid();
        }
    }).detach();
}

void read(State& in) {
    read_xinput(in);
    std::lock_guard lk{g_pads_lock};
    for (const auto& p : g_pads) {
        if (!p->alive) continue;
        std::lock_guard plk{p->lock};
        add(in, p->state);
    }
}

}  // namespace gamepads
