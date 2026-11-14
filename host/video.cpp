// The game window: created by init, its message loop run by run_window_loop on the main thread. The
// renderer (nvn/vk) puts its swapchain on it. Also the performance overlay in its corner.
#include <windows.h>
#include <psapi.h>

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

#include "memory.h"
#include "video.h"

namespace video {
namespace {

// ------------------------------------------------------------------ performance overlay

// A small semi-transparent window owned by the game window (so it stays above it and DWM composes
// it over the Vulkan swapchain), in the client area's top-left corner. Click-through; F3 toggles.
struct FrameStats {
    double fps = 0, frame_ms = 0, gpu_ms = 0;
    bool valid = false;
};
std::mutex g_stats_lock;
FrameStats g_stats;
HWND g_main = nullptr, g_overlay = nullptr;
HFONT g_overlay_font = nullptr;
bool g_overlay_on = std::getenv("HOST_NO_OVERLAY") == nullptr;
constexpr int OverlayW = 230, OverlayH = 76, OverlayMargin = 8;

// The working set counts a guest page once per view that maps it (the guest address space and
// the pool's backing view): ~4 GB of a race's working set were such doubles. A low-priority
// thread counts the pool's pages that are in both every 10 s while the overlay is on; the
// overlay's RAM leaves them out once.
std::atomic<u64> g_double_counted_bytes{0};
void start_double_count() {
    std::thread([] {
        SetThreadDescription(GetCurrentThread(), L"overlay RAM count");
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_LOWEST);
        std::vector<PSAPI_WORKING_SET_EX_INFORMATION> info(65536);
        for (;; Sleep(10000)) {
            if (!g_overlay_on) continue;
            u8* const base = mem::phys_ptr(0);
            const u64 size = mem::phys_pool_bytes();
            u64 pages_in_both = 0;
            for (u64 off = 0; off < size; off += info.size() * mem::PageSize) {
                const u64 pages = std::min<u64>(info.size(), (size - off) / mem::PageSize);
                for (u64 i = 0; i < pages; ++i) info[i].VirtualAddress = base + off + i * mem::PageSize;
                if (!QueryWorkingSetEx(GetCurrentProcess(), info.data(), static_cast<DWORD>(pages * sizeof(info[0]))))
                    continue;
                for (u64 i = 0; i < pages; ++i)
                    pages_in_both += info[i].VirtualAttributes.Valid && info[i].VirtualAttributes.ShareCount >= 2;
            }
            g_double_counted_bytes = pages_in_both * mem::PageSize;
        }
    }).detach();
}

LRESULT CALLBACK overlay_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCHITTEST) return HTTRANSPARENT;
    if (msg != WM_PAINT) return DefWindowProcW(hwnd, msg, wp, lp);
    FrameStats s;
    {
        std::scoped_lock lk{g_stats_lock};
        s = g_stats;
    }
    PROCESS_MEMORY_COUNTERS pmc{sizeof pmc};
    GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof pmc);

    PAINTSTRUCT ps;
    HDC dc = BeginPaint(hwnd, &ps);
    RECT rc;
    GetClientRect(hwnd, &rc);
    FillRect(dc, &rc, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
    SelectObject(dc, g_overlay_font);
    SetBkMode(dc, TRANSPARENT);
    wchar_t line[64];
    const auto text = [&](int row, COLORREF color) {
        SetTextColor(dc, color);
        TextOutW(dc, 8, 4 + row * 23, line, static_cast<int>(wcslen(line)));
    };
    const COLORREF white = RGB(235, 235, 235);
    if (s.valid) {
        const COLORREF fps_color = s.fps >= 55 ? RGB(90, 230, 110) : s.fps >= 30 ? RGB(240, 210, 80) : RGB(240, 90, 80);
        swprintf(line, 64, L"%5.1f FPS  %5.1f ms", s.fps, s.frame_ms);
        text(0, fps_color);
        swprintf(line, 64, L"GPU wait   %5.1f ms", s.gpu_ms);
        text(1, white);
    } else {
        swprintf(line, 64, L"  --  FPS");
        text(0, white);
    }
    const u64 doubled = std::min<u64>(g_double_counted_bytes.load(), pmc.WorkingSetSize);
    swprintf(line, 64, L"RAM        %5.1f GB", (pmc.WorkingSetSize - doubled) / 1073741824.0);
    text(2, white);
    EndPaint(hwnd, &ps);
    return 0;
}

void create_overlay(HWND owner) {
    WNDCLASSEXW wc{sizeof wc};
    wc.lpfnWndProc = overlay_proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"KartHostOverlay";
    RegisterClassExW(&wc);
    g_overlay = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                                wc.lpszClassName, L"", WS_POPUP, 0, 0, OverlayW, OverlayH, owner, nullptr,
                                wc.hInstance, nullptr);
    if (!g_overlay) return;
    SetLayeredWindowAttributes(g_overlay, 0, 200, LWA_ALPHA);
    g_overlay_font = CreateFontW(-18, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                 CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, FIXED_PITCH, L"Consolas");
    SetTimer(owner, 1, 250, nullptr);
    start_double_count();
}

// Follows the game window's client corner; hidden with it (minimized) or when toggled off.
void update_overlay() {
    if (!g_overlay) return;
    if (!g_overlay_on || IsIconic(g_main) || !IsWindowVisible(g_main)) {
        ShowWindow(g_overlay, SW_HIDE);
        return;
    }
    POINT pt{OverlayMargin, OverlayMargin};
    ClientToScreen(g_main, &pt);
    SetWindowPos(g_overlay, nullptr, pt.x, pt.y, OverlayW, OverlayH,
                 SWP_NOACTIVATE | SWP_NOZORDER | SWP_SHOWWINDOW);
    InvalidateRect(g_overlay, nullptr, FALSE);
}

// ------------------------------------------------------------------ window

// F11: borderless fullscreen on the window's monitor, and back to the window as it was. The swapchain follows the
// new size (present.cpp recreates it when it is out of date).
WINDOWPLACEMENT g_windowed{sizeof g_windowed};
bool g_fullscreen = false;

void toggle_fullscreen(HWND hwnd) {
    const LONG style = GetWindowLongW(hwnd, GWL_STYLE);
    if (!g_fullscreen) {
        MONITORINFO mi{sizeof mi};
        if (!GetWindowPlacement(hwnd, &g_windowed) || !GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi))
            return;
        SetWindowLongW(hwnd, GWL_STYLE, style & ~WS_OVERLAPPEDWINDOW);
        const RECT& m = mi.rcMonitor;
        SetWindowPos(hwnd, HWND_TOP, m.left, m.top, m.right - m.left, m.bottom - m.top, SWP_FRAMECHANGED);
        g_fullscreen = true;
    } else {
        SetWindowLongW(hwnd, GWL_STYLE, style | WS_OVERLAPPEDWINDOW);
        SetWindowPlacement(hwnd, &g_windowed);
        SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
        g_fullscreen = false;
    }
    LOG("window: %s", g_fullscreen ? "fullscreen" : "windowed");
}

LRESULT CALLBACK window_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_SIZE:
    case WM_MOVE:
    case WM_TIMER:
        update_overlay();
        return 0;
    case WM_KEYDOWN:
        if (wp == VK_F3) {
            g_overlay_on = !g_overlay_on;
            update_overlay();
            return 0;
        }
        if (wp == VK_F11 && !(lp & (1 << 30))) {  // not the key's auto-repeat
            toggle_fullscreen(hwnd);
            update_overlay();
            return 0;
        }
        break;
    case WM_CLOSE:
        LOG("window closed");
        std::fflush(nullptr);
        TerminateProcess(GetCurrentProcess(), 0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

HWND create_window() {
    WNDCLASSEXW wc{sizeof wc};
    wc.lpfnWndProc = window_proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"KartHost";
    RegisterClassExW(&wc);
    RECT rc{0, 0, 1920, 1080};
    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"Kart (native)", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT,
                                CW_USEDEFAULT, rc.right - rc.left, rc.bottom - rc.top, nullptr, nullptr,
                                wc.hInstance, nullptr);
    if (!hwnd) fatal("CreateWindow failed (%lu)", GetLastError());
    g_main = hwnd;
    create_overlay(hwnd);
    ShowWindow(hwnd, SW_SHOW);
    return hwnd;
}

std::atomic<u64> g_last_frame_ns{0};

}  // namespace

void init() { create_window(); }

HWND main_window() { return g_main; }

void report_frame_stats(double fps, double frame_ms, double gpu_wait_ms) {
    g_last_frame_ns.store(host_ns(), std::memory_order_relaxed);
    std::scoped_lock lk{g_stats_lock};
    g_stats = {fps, frame_ms, gpu_wait_ms, true};
}

unsigned long long last_frame_ns() { return g_last_frame_ns.load(std::memory_order_relaxed); }

void run_window_loop() {
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

}  // namespace video
