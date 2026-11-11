// video_core glue: definitions of the emulator-core symbols video_core.lib links against
// (Core::System accessors, process memory, timing), the window and the GPU's setup.
#include <windows.h>
#include <psapi.h>

#include <atomic>
#include <cstring>
#include <mutex>
#include <optional>
#include <chrono>
#include <thread>
#include <vector>
#include <intrin.h>
#include <unordered_set>

#include <filesystem>

#include "common/fs/path_util.h"
#include "common/scratch_buffer.h"
#include "common/settings.h"
#include "common/cpu_features.h"
#include "core/core.h"
#include "core/core_timing.h"
#include "core/device_memory.h"
#include "core/frontend/emu_window.h"
#include "core/frontend/graphics_context.h"
#include "core/gpu_dirty_memory_manager.h"
#include "core/hardware_properties.h"
#include "core/memory.h"
#include "core/perf_stats.h"
#include "video_core/gpu.h"
#include "video_core/host1x/host1x.h"
#include "video_core/rasterizer_interface.h"
#include "video_core/renderer_base.h"
#include "video_core/video_core.h"

#include "memory.h"
#include "video.h"

namespace VideoCommon {
// video_core debug hook (buffer_cache_base.h): set to video::debug_page_state at init.
extern int (*g_nx2_debug_page_state)(u64 cpu_address);
}  // namespace VideoCommon

namespace video {
Core::Frontend::EmuWindow* window();
inline bool g_gpu_created = false;
}

// ------------------------------------------------------------------ the emulator core, as video_core sees it

namespace Core {

struct System::Impl {
    std::optional<Tegra::Host1x::Host1x> host1x;
    std::optional<Tegra::GPU> gpu;
    std::optional<Memory::Memory> memory;
    // Never constructed: video_core only calls members the host defines below, which use no state.
    alignas(Timing::CoreTiming) unsigned char timing[sizeof(Timing::CoreTiming)]{};
    alignas(PerfStats) unsigned char perf[sizeof(PerfStats)]{};
    std::atomic<bool> shutting_down{false};
};

System::System() : impl{std::make_unique<Impl>()} {}
System::~System() = default;

Tegra::GPU& System::GPU() { return *impl->gpu; }
Tegra::Host1x::Host1x& System::Host1x() { return *impl->host1x; }
DeviceMemory& System::DeviceMemory() { return mem::device_memory(); }
Memory::Memory& System::ApplicationMemory() { return *impl->memory; }
Timing::CoreTiming& System::CoreTiming() { return *reinterpret_cast<Timing::CoreTiming*>(impl->timing); }
PerfStats& System::GetPerfStats() { return *reinterpret_cast<PerfStats*>(impl->perf); }
void System::RegisterHostThread() {}
bool System::IsPoweredOn() const { return true; }
bool System::IsShuttingDown() const { return impl->shutting_down; }
bool System::IsPaused() const { return false; }

// CPU writes to GPU-modified pages, applied by the GPU at its next sync (InvalidateGPUCache).
std::array<GPUDirtyMemoryManager, Hardware::NUM_CPU_CORES> g_dirty;

// The same writes as exact byte ranges. The managers above work in 64-byte units, and applying a
// unit uploads all of it from guest memory, which never received what the GPU wrote: a store next
// to bytes a compute shader wrote (the game's GPU particle simulation shares its buffers with the
// CPU's spawn writes) put stale guest bytes over the GPU's, and those particles were drawn
// screen-sized for a frame. Applying the exact range lets the buffer cache keep the GPU's bytes
// around it. HOST_COLLECT_64=1 collects in 64-byte units again.
std::mutex g_exact_lock;
std::vector<std::pair<PAddr, size_t>> g_exact_writes;
std::vector<std::pair<PAddr, size_t>> g_exact_gathered;

bool collect_exact() {
    static const bool exact = [] {
        const char* v = std::getenv("HOST_COLLECT_64");
        return !v || v[0] != '1';
    }();
    return exact;
}

void collect_write(u32 core, PAddr address, size_t size) {
    if (!collect_exact()) {
        g_dirty[core % g_dirty.size()].Collect(address, size);
        return;
    }
    {
        std::scoped_lock lk{g_exact_lock};
        g_exact_writes.emplace_back(address, size);
    }
    GPUDirtyMemoryManager::MarkPending();
}

void System::GatherGPUDirtyMemory(std::function<void(PAddr, size_t)>& callback) {
    for (auto& manager : g_dirty) manager.Gather(callback);
    {
        std::scoped_lock lk{g_exact_lock};
        g_exact_gathered.swap(g_exact_writes);
    }
    for (const auto& [address, size] : g_exact_gathered) callback(address, size);
    g_exact_gathered.clear();
}

// The host's setup: guest memory interface, host1x and the GPU with its renderer (needs the window).
void System::Initialize() {
    impl->memory.emplace(*this);
    impl->host1x.emplace(*this);
    // With native NVN (HOST_NVN=native) the game never reaches the emulated GPU: the window is the
    // native renderer's (nvn/vk), and audio_core still gets this System.
    if (video::native_nvn()) return;
    VideoCore::CreateGPU(impl->gpu, *video::window(), *this);
    video::g_gpu_created = impl->gpu.has_value();
}

u64 Timing::CoreTiming::GetGPUTicks() const { return Common::g_wall_clock.GetGPUTick(); }
void PerfStats::EndGameFrame() {}

}  // namespace Core

// Guest memory for the GPU: reads go through the physical pool, so they never fault on pages the
// GPU write-protected in the view.
namespace Core::Memory {

struct Memory::Impl {};

Memory::Memory(Core::System& system_) : system{system_} {}
Memory::~Memory() = default;

u8* Memory::GetPointerSilent(Common::ProcessAddress vaddr) { return mem::backing_ptr(GetInteger(vaddr)); }

u8* Memory::GetSpan(const VAddr src_addr, const std::size_t size) {
    const auto runs = mem::phys_runs(src_addr, size);
    if (runs.size() != 1 || runs[0].size < size) return nullptr;
    return mem::phys_ptr(runs[0].phys);
}

bool Memory::ReadBlock(Common::ProcessAddress src_addr, void* dest_buffer, std::size_t size) {
    u64 addr = GetInteger(src_addr);
    u8* out = static_cast<u8*>(dest_buffer);
    while (size) {
        const u64 chunk = std::min<u64>(size, mem::PageSize - (addr & (mem::PageSize - 1)));
        const u8* p = mem::backing_ptr(addr);
        if (p) std::memcpy(out, p, chunk);
        else std::memset(out, 0, chunk);
        addr += chunk;
        out += chunk;
        size -= chunk;
    }
    return true;
}

u32 Memory::Read32(Common::ProcessAddress addr) {
    u32 v = 0;
    ReadBlock(addr, &v, sizeof v);
    return v;
}

// For the audio renderer (audio_core): voice buffers, aux buffers, the work buffer.
bool Memory::ReadBlockUnsafe(Common::ProcessAddress src_addr, void* dest_buffer, std::size_t size) {
    return ReadBlock(src_addr, dest_buffer, size);
}

namespace {
template <class F> void for_each_page(u64 addr, std::size_t size, F&& f) {
    while (size) {
        const u64 chunk = std::min<u64>(size, mem::PageSize - (addr & (mem::PageSize - 1)));
        if (u8* p = mem::backing_ptr(addr)) f(p, chunk);
        addr += chunk;
        size -= chunk;
    }
}
}  // namespace

bool Memory::WriteBlockUnsafe(Common::ProcessAddress dest_addr, const void* src_buffer, std::size_t size) {
    const u8* in = static_cast<const u8*>(src_buffer);
    for_each_page(GetInteger(dest_addr), size, [&](u8* p, u64 n) {
        std::memcpy(p, in, n);
        in += n;
    });
    return true;
}

bool Memory::ZeroBlock(Common::ProcessAddress dest_addr, std::size_t size) {
    for_each_page(GetInteger(dest_addr), size, [](u8* p, u64 n) { std::memset(p, 0, n); });
    return true;
}

void Memory::Write32(Common::ProcessAddress addr, u32 data) { WriteBlockUnsafe(addr, &data, sizeof data); }

}  // namespace Core::Memory

namespace video {
namespace {

Core::System* g_system = nullptr;

// Pages the GPU caches: write-protected in the view, one bit per guest page.
std::vector<std::atomic<u64>>* g_cached_bits = nullptr;
// Keeps a page's bit and its view protection changing together: otherwise two GPU threads
// marking the same page cached and uncached could leave it write-protected with the bit clear,
// and a guest write there would not be recognized as the GPU's.
std::mutex g_cached_lock;

bool page_cached(u64 page) {
    return ((*g_cached_bits)[page / 64].load(std::memory_order_relaxed) >> (page % 64)) & 1;
}

void set_cached(u64 first_page, u64 count, bool cached) {
    for (u64 p = first_page; p < first_page + count; p++) {
        const u64 bit = u64{1} << (p % 64);
        if (cached) (*g_cached_bits)[p / 64].fetch_or(bit, std::memory_order_relaxed);
        else (*g_cached_bits)[p / 64].fetch_and(~bit, std::memory_order_relaxed);
    }
}

// Read traps: cached pages the GPU wrote and the guest CPU has not read since, with no access at
// all in the view. Changed under g_cached_lock, like the cached bits.
std::unordered_set<u64> g_trapped;
std::atomic<u64> g_trapped_count{0};

bool read_traps_enabled() {
    static const bool on = [] {
        const char* v = std::getenv("HOST_READ_TRAPS");
        return !v || v[0] != '0';
    }();
    return on;
}

// View protection for a page from its cached and trapped state (g_cached_lock held).
void protect_page(u64 page) {
    const bool trapped = g_trapped_count.load(std::memory_order_relaxed) && g_trapped.contains(page);
    mem::protect_view(page * mem::PageSize, mem::PageSize, !trapped, !trapped && !page_cached(page));
}

}  // namespace
}  // namespace video

namespace Core::Memory {

// The GPU caches the range (cached) or stopped caching it: guest writes to cached pages fault
// so the GPU's copies can be dropped (video::on_cpu_write_fault).
void Memory::RasterizerMarkRegionCached(Common::ProcessAddress vaddr, u64 size, bool cached) {
    const u64 addr = GetInteger(vaddr);
    if (addr == 0 || size == 0 || addr + size > mem::AddressSpaceSize) return;
    const u64 first = addr / mem::PageSize, last = (addr + size - 1) / mem::PageSize;
    std::scoped_lock lk{video::g_cached_lock};
    video::set_cached(first, last - first + 1, cached);
    // Pages that stop being cached lose their traps (nothing of the GPU's is left to download).
    if (!cached && video::g_trapped_count.load(std::memory_order_relaxed)) {
        for (u64 p = first; p <= last; p++)
            if (video::g_trapped.erase(p)) video::g_trapped_count--;
    }
    mem::protect_view(first * mem::PageSize, (last - first + 1) * mem::PageSize, true, !cached);
    if (cached && video::g_trapped_count.load(std::memory_order_relaxed)) {
        for (u64 p = first; p <= last; p++)
            if (video::g_trapped.contains(p)) video::protect_page(p);
    }
}

// The GPU wrote a buffer whose data is not copied back at fences: the guest CPU's next read of it
// faults (video::on_read_trap), which downloads it once and makes the buffer cache copy it back at
// every fence from then on (Preflushable). Without this the game never sees what its compute
// shaders write for the CPU. HOST_READ_TRAPS=0 turns them off.
void Memory::RasterizerArmReadTraps(Common::ProcessAddress vaddr, u64 size) {
    const u64 addr = GetInteger(vaddr);
    if (!video::read_traps_enabled() || addr == 0 || size == 0 || addr + size > mem::AddressSpaceSize) return;
    const u64 first = addr / mem::PageSize, last = (addr + size - 1) / mem::PageSize;
    std::scoped_lock lk{video::g_cached_lock};
    u64 run = 0, run_pages = 0;
    const auto protect_run = [&] {
        if (run_pages) mem::protect_view(run * mem::PageSize, run_pages * mem::PageSize, false, false);
        run_pages = 0;
    };
    static std::atomic<u64> skipped{0}, armed{0};
    for (u64 p = first; p <= last; p++) {
        if (!video::page_cached(p)) {
            if (const u64 n = ++skipped; (n & (n - 1)) == 0 && n >= 64)
                LOG("video: %llu read-trap pages skipped as not cached (%llu armed)", n, armed.load());
        }
        if (!video::page_cached(p) || !video::g_trapped.insert(p).second) {
            protect_run();
            continue;
        }
        armed++;
        video::g_trapped_count++;
        if (!run_pages) run = p;
        run_pages++;
    }
    protect_run();
}

}  // namespace Core::Memory

// ------------------------------------------------------------------ window

namespace video {
namespace {

class DummyContext final : public Core::Frontend::GraphicsContext {};

class HostWindow final : public Core::Frontend::EmuWindow {
public:
    explicit HostWindow(HWND hwnd_) : hwnd{hwnd_} {
        window_info.type = Core::Frontend::WindowSystemType::Windows;
        window_info.render_surface = hwnd;
        RECT rc;
        GetClientRect(hwnd, &rc);
        Resize(rc.right - rc.left, rc.bottom - rc.top);
    }
    std::unique_ptr<Core::Frontend::GraphicsContext> CreateSharedContext() const override {
        return std::make_unique<DummyContext>();
    }
    // HOST_RENDER_HIDDEN=1: render (and take screenshots) with the window hidden, for test runs.
    bool IsShown() const override {
        static const bool always = std::getenv("HOST_RENDER_HIDDEN") != nullptr;
        return always || (IsWindowVisible(hwnd) && !IsIconic(hwnd));
    }
    void Resize(int w, int h) {
        if (w > 0 && h > 0) UpdateCurrentFramebufferLayout(static_cast<u32>(w), static_cast<u32>(h));
    }

private:
    HWND hwnd;
};

HostWindow* g_window = nullptr;

}  // namespace

Core::Frontend::EmuWindow* window() { return g_window; }
Core::System& system() { return *g_system; }

bool host_fences() {
    static const bool host = [] {
        // Off by default: the streak frames it was meant to fix stayed (45 in 27020 frames over
        // 16 min, as without it) and races ran ~35 fps instead of ~42 (the game's CPU no longer
        // runs ahead of the host GPU).
        const char* v = std::getenv("HOST_GPU_FENCES");
        return v && std::strcmp(v, "host") == 0;
    }();
    return host;
}

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
// the GPU's view of the pool, Common::HostMemory): ~4 GB of a race's working set were such
// doubles. A low-priority thread counts the pool's pages that are in both every 10 s while the
// overlay is on; the overlay's RAM leaves them out once.
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

LRESULT CALLBACK window_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_SIZE:
        if (g_window) g_window->Resize(LOWORD(lp), HIWORD(lp));
        update_overlay();
        return 0;
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

// Eglantine's tuned setup for this title (its sdl2-config.ini).
void apply_settings() {
    auto& v = Settings::values;
    v.renderer_backend.SetValue(Settings::RendererBackend::Vulkan);
    // Vulkan device index: Eglantine's config here uses 2 (the RTX 3080; 0 is the RTX A2000).
    const char* device = std::getenv("HOST_VULKAN_DEVICE");
    v.vulkan_device.SetValue(device ? std::atoi(device) : 2);
    v.memory_layout_mode.SetValue(Settings::MemoryLayout::Memory_12Gb);
    v.use_asynchronous_gpu_emulation.SetValue(true);
    v.gpu_accuracy.SetValue(Settings::GpuAccuracy::Low);
    v.dma_accuracy.SetValue(Settings::DmaAccuracy::Unsafe);
    // HOST_EXTENDED_LOG=1: let eden_log.txt grow to 1 GiB instead of stopping at 100 MiB.
    if (std::getenv("HOST_EXTENDED_LOG")) v.extended_logging.SetValue(true);
    // Fences (syncpoints, semaphore releases) signal once the host GPU is done and the buffers
    // copied back for the CPU are in guest memory (see host_fences).
    if (host_fences()) v.gpu_fence_behavior.SetValue(Settings::GpuFenceBehavior::Balanced);
    // GPU-written buffers reach guest memory through read traps (Memory::RasterizerArmReadTraps):
    // the ranges the CPU reads are then copied back at every fence. EDEN_NX2_BUFFER_DOWNLOADS=1
    // copies every GPU-written buffer back instead: the copies land after the CPU has written the
    // next frame's data next to them and overwrite it (camera data: frames of streaks).
    // Pipelines build on worker threads and draws that need one are skipped until it is ready
    // (objects pop in a few frames late the first time); HOST_ASYNC_SHADERS=0 builds them on the
    // GPU thread instead (no pop-in, a hitch per new pipeline; the disk cache keeps them after).
    const char* async_shaders = std::getenv("HOST_ASYNC_SHADERS");
    v.use_asynchronous_shaders.SetValue(!async_shaders || async_shaders[0] != '0');
    v.astc_recompression.SetValue(Settings::AstcRecompression::Bc3);
    // ASTC decodes on the GPU (then BC3 via bc_encoder.comp), ready the frame a texture is used.
    // CpuAsynchronous shows streamed textures black until a CPU thread gets to them, so surfaces
    // flash black/colour while the texture cache churns (NX2_NOTES row 107). HOST_ASTC=cpu|async.
    const char* astc = std::getenv("HOST_ASTC");
    v.accelerate_astc.SetValue(!astc                        ? Settings::AstcDecodeMode::Gpu
                               : std::strcmp(astc, "cpu") == 0 ? Settings::AstcDecodeMode::Cpu
                               : std::strcmp(astc, "async") == 0 ? Settings::AstcDecodeMode::CpuAsynchronous
                                                                 : Settings::AstcDecodeMode::Gpu);
    v.use_disk_shader_cache.SetValue(true);
    v.use_vulkan_driver_pipeline_cache.SetValue(true);
    v.use_reactive_flushing.SetValue(true);
    // Vsync waits stay off the GPU thread. HOST_ASYNC_PRESENT=0 presents on it (Eglantine
    // NX2_NOTES row 118: a present-thread/scheduler race).
    const char* async_present = std::getenv("HOST_ASYNC_PRESENT");
    v.async_presentation.SetValue(!async_present || async_present[0] != '0');
    v.nvdec_emulation.SetValue(Settings::NvdecEmulation::Cpu);
    // Audio (audio_core): cubeb output; HOST_AUDIO_MUTE=1 keeps test runs silent.
    const char* mute = std::getenv("HOST_AUDIO_MUTE");
    v.audio_muted.SetValue(mute && mute[0] == '1');
    Settings::UpdateGPUAccuracy();
    Settings::SetCurrentProgramID(ProgramId);
    Settings::SetCurrentProgramIsSwitch2(true);
}

}  // namespace

int debug_page_state(u64 va);

void init() {
    std::error_code ec;
    std::filesystem::create_directories(Common::FS::GetEdenPath(Common::FS::EdenPath::ShaderDir), ec);
    g_cached_bits = new std::vector<std::atomic<u64>>(mem::AddressSpaceSize / mem::PageSize / 64);
    apply_settings();
    g_window = new HostWindow(create_window());
    g_system = new Core::System();
    g_system->Initialize();
    if (native_nvn()) {
        LOG("video: native NVN, the emulated GPU is not created");
        return;
    }
    VideoCommon::g_nx2_debug_page_state = &debug_page_state;
    if (!g_gpu_created) fatal("could not create the GPU (Vulkan renderer)");
    LOG("video: GPU and Vulkan renderer created");
}

bool native_nvn() {
    static const bool on = [] {
        const char* v = std::getenv("HOST_NVN");
        return v && std::strcmp(v, "native") == 0;
    }();
    return on;
}

HWND main_window() { return g_main; }

// For video_core's debug checks (VideoCommon::g_nx2_debug_page_state): the view protection of a
// guest page, | 0x1000 when the GPU caches it, | 0x2000 when it is read-trapped, | (guest pages
// mapping its physical page) << 16.
int debug_page_state(u64 va) {
    const u64 page = va / mem::PageSize;
    if (page >= mem::AddressSpaceSize / mem::PageSize) return -1;
    MEMORY_BASIC_INFORMATION mbi{};
    VirtualQuery(hp(page * mem::PageSize), &mbi, sizeof mbi);
    int state = mbi.State == MEM_COMMIT ? static_cast<int>(mbi.Protect & 0xff) : 0;
    if (page_cached(page)) state |= 0x1000;
    // Guest pages mapping the same physical page (bits 16 and up; 1 = no alias).
    std::vector<u64> others;
    const u32 aliases = mem::alias_count(page * mem::PageSize, &others);
    state |= static_cast<int>(std::min<u32>(aliases, 0x7fff) << 16);
    if (aliases > 1) {
        static std::atomic<int> logged{0};
        if (logged++ < 32) {
            std::string list;
            for (const u64 a : others) list += std::format(" {:#x}", a);
            LOG("video: guest page %#llx shares its physical page with %u other page(s):%s (cached %d)",
                page * mem::PageSize, aliases - 1, list.c_str(), page_cached(page) ? 1 : 0);
        }
    }
    std::scoped_lock lk{g_cached_lock};
    if (g_trapped.contains(page)) state |= 0x2000;
    return state;
}

static std::atomic<u64> g_last_frame_ns{0};
void report_frame_stats(double fps, double frame_ms, double gpu_wait_ms) {
    g_last_frame_ns.store(host_ns(), std::memory_order_relaxed);
    std::scoped_lock lk{g_stats_lock};
    g_stats = {fps, frame_ms, gpu_wait_ms, true};
}
unsigned long long last_frame_ns() { return g_last_frame_ns.load(std::memory_order_relaxed); }

// HOST_POLL_VA=<addr>[,<addr>...] (hex, with HOST_TIMELINE): a thread samples these guest words
// every few microseconds and logs each change, with the steady clock in microseconds (the clock
// of video_core's EDEN_NX2_FRONT_TRACE), to line CPU writes up with the GPU thread's reads.
static void start_poll_thread() {
    const char* v = std::getenv("HOST_POLL_VA");
    if (!v || !g_timeline_on) return;
    std::vector<u64> addrs;
    for (const char* p = v; *p;) {
        char* end = nullptr;
        addrs.push_back(std::strtoull(p, &end, 16));
        p = *end == ',' ? end + 1 : end;
        if (end == p && *p) break;
    }
    std::thread([addrs] {
        std::vector<u32> last(addrs.size(), 0xffffffffu);
        for (;;) {
            for (size_t i = 0; i < addrs.size(); i++) {
                const u8* ptr = mem::backing_ptr(addrs[i]);
                if (!ptr) continue;
                const u32 now = *reinterpret_cast<const volatile u32*>(ptr);
                if (now != last[i]) {
                    const long long steady = std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::steady_clock::now().time_since_epoch()).count();
                    TIMELINE("poll %#llx %#x -> %#x steady=%lld", static_cast<unsigned long long>(addrs[i]), last[i],
                             now, steady);
                    last[i] = now;
                }
            }
            for (int k = 0; k < 64; k++) _mm_pause();
        }
    }).detach();
}

void start() {
    if (native_nvn()) return;
    g_system->GPU().Start();
    LOG("video: GPU thread started");
    start_poll_thread();
    // The disk pipeline cache (user/cache/shader/<title>): loads the pipelines earlier runs built and
    // makes new ones save as they're built. Without it every run compiled each pipeline again the
    // first time it was drawn: hitches of 100+ ms at every new scene.
    if (Settings::values.use_disk_shader_cache.GetValue()) {
        const auto t0 = std::chrono::steady_clock::now();
        g_system->GPU().Renderer().ReadRasterizer()->LoadDiskResources(
            ProgramId, std::stop_token{}, [](VideoCore::LoadCallbackStage, size_t, size_t) {});
        LOG("video: disk pipeline cache loaded in %lld ms",
            static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                       std::chrono::steady_clock::now() - t0)
                                       .count()));
    }
}

void run_window_loop() {
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

bool on_read_trap(u64 guest_addr, bool write) {
    if (!g_system || guest_addr >= mem::AddressSpaceSize || !g_trapped_count.load(std::memory_order_relaxed))
        return false;
    const u64 page = guest_addr / mem::PageSize;
    {
        std::scoped_lock lk{g_cached_lock};
        if (!g_trapped.contains(page)) return false;
    }
    // A read downloads what the GPU wrote (once: the range is then copied back at every fence). A
    // write only disarms the trap and goes on as any write to a cached page: downloading there
    // would stall every store into GPU-written memory on the GPU, as Eden's InvalidateRegion did.
    if (!write) {
        if (const u8* p = mem::backing_ptr(page * mem::PageSize)) {
            thread_local Common::ScratchBuffer<u32> scratch;
            auto& gpu = g_system->GPU();
            g_system->Host1x().MemoryManager().ApplyOpOnPointer(
                p, scratch, [&](DAddr address) { (void)gpu.OnCPURead(address, mem::PageSize); });
        }
        static std::atomic<u64> reads{0};
        if (const u64 n = ++reads; n <= 16 || (n & (n - 1)) == 0)
            LOG("video: CPU read of GPU-written page %#llx downloaded (%llu so far)", page * mem::PageSize, n);
    }
    std::scoped_lock lk{g_cached_lock};
    if (g_trapped.erase(page)) g_trapped_count--;
    protect_page(page);
    return true;
}

namespace {

// The general-purpose register that addresses the memory operand of the x86 instruction at `code`
// and that can be moved without changing what the instruction does (it is not also a data
// operand), or -1. Recompiled code addresses guest memory as [membase + offset] through ordinary
// ModRM operands. (Eglantine's AOT fault handler, arm_aot.cpp.)
int redirectable_address_register(const u8* code) {
    int i = 0;
    bool vex = false;
    u8 rex = 0;
    for (;; i++) {
        const u8 b = code[i];
        if (b == 0xF0 || b == 0xF2 || b == 0xF3 || b == 0x66 || b == 0x67 || b == 0x2E || b == 0x3E || b == 0x26 ||
            b == 0x36 || b == 0x64 || b == 0x65)
            continue;
        break;
    }
    if ((code[i] & 0xF0) == 0x40) rex = code[i++];
    u8 opcode{};
    bool two_byte = false;
    if (code[i] == 0xC5) {  // two-byte VEX: inverted R in bit 7, then the opcode
        rex = (code[i + 1] & 0x80) ? 0 : 4;
        opcode = code[i + 2];
        i += 3;
        vex = true;
    } else if (code[i] == 0xC4) {  // three-byte VEX: inverted R, X, B in bits 7-5
        const u8 b1 = code[i + 1];
        rex = ((b1 & 0x80) ? 0 : 4) | ((b1 & 0x40) ? 0 : 2) | ((b1 & 0x20) ? 0 : 1);
        opcode = code[i + 3];
        i += 4;
        vex = true;
    } else if (code[i] == 0x62) {
        return -1;  // EVEX: not emitted
    } else if (code[i] == 0x0F) {
        two_byte = true;
        i++;
        if (code[i] == 0x38 || code[i] == 0x3A) i++;
        opcode = code[i++];
    } else {
        opcode = code[i++];
        // String instructions and moffs moves address memory without a ModRM operand.
        if ((opcode >= 0xA0 && opcode <= 0xA7) || (opcode >= 0xAA && opcode <= 0xAF)) return -1;
    }
    const u8 modrm = code[i];
    const u32 mod = modrm >> 6;
    const int reg = ((modrm >> 3) & 7) | ((rex & 4) << 1);
    if (mod == 3) return -1;
    int base = -1, index = -1;
    u32 scale = 0;
    if ((modrm & 7) == 4) {
        const u8 sib = code[i + 1];
        scale = sib >> 6;
        index = ((sib >> 3) & 7) | ((rex & 2) << 2);
        if (index == 4) index = -1;  // no index
        base = (sib & 7) | ((rex & 1) << 3);
        if ((sib & 7) == 5 && mod == 0) base = -1;  // disp32 only
    } else if ((modrm & 7) == 5 && mod == 0) {
        return -1;  // RIP-relative
    } else {
        base = (modrm & 7) | ((rex & 1) << 3);
    }
    // Registers the instruction reads or writes besides the address: the ModRM reg operand of
    // non-vector forms, RAX for CMPXCHG, RCX for shifts by CL, all of RAX-RDX for CMPXCHG16B.
    const auto conflicts = [&](int r) {
        if (r < 0 || r == 4) return true;  // RSP
        if (!vex && r == reg) return true;
        if (two_byte && (opcode == 0xB0 || opcode == 0xB1) && r == 0) return true;
        if (two_byte && opcode == 0xC7 && r <= 3) return true;
        if (!two_byte && !vex && (opcode == 0xD2 || opcode == 0xD3) && r == 1) return true;
        return false;
    };
    if (!conflicts(base)) return base;
    if (scale == 0 && !conflicts(index)) return index;
    return -1;
}

// The bytes the store at `code` writes to its memory operand, or 0 when the form is not known.
// Covers what MSVC emits for the recompiled code and the CRT: moves, SSE/AVX stores and extracts,
// read-modify-write ALU forms, atomics, SETcc. Collected writes upload exactly these bytes.
u32 store_width(const u8* code) {
    int i = 0;
    bool opsize = false, rep_f3 = false, rep_f2 = false;
    for (;; i++) {
        const u8 b = code[i];
        if (b == 0x66) opsize = true;
        else if (b == 0xF3) rep_f3 = true;
        else if (b == 0xF2) rep_f2 = true;
        else if (b == 0xF0 || b == 0x67 || b == 0x2E || b == 0x3E || b == 0x26 || b == 0x36 || b == 0x64 || b == 0x65) {
        } else break;
    }
    bool w = false;
    if ((code[i] & 0xF0) == 0x40) w = (code[i++] & 8) != 0;
    const u32 word = w ? 8 : (opsize ? 2 : 4);
    if (code[i] == 0xC5 || code[i] == 0xC4) {
        u32 map = 1, pp, l;
        u8 op;
        if (code[i] == 0xC5) {
            pp = code[i + 1] & 3;
            l = (code[i + 1] >> 2) & 1;
            op = code[i + 2];
        } else {
            map = code[i + 1] & 0x1F;
            w = (code[i + 2] & 0x80) != 0;
            pp = code[i + 2] & 3;
            l = (code[i + 2] >> 2) & 1;
            op = code[i + 3];
        }
        const u32 vec = l ? 32 : 16;
        if (map == 1) {
            if (op == 0x11) return pp == 2 ? 4 : pp == 3 ? 8 : vec;  // vmovups/vmovss/vmovsd
            if (op == 0x29 || op == 0x2B || op == 0xE7) return vec;  // vmovaps, vmovntps, vmovntdq
            if (op == 0x7F && (pp == 1 || pp == 2)) return vec;     // vmovdqa/vmovdqu
            if (op == 0x7E && pp == 1) return w ? 8 : 4;           // vmovd/vmovq to memory
            if (op == 0xD6 || op == 0x13 || op == 0x17) return 8;  // vmovq, vmovlps, vmovhps
        } else if (map == 2) {
            if (op == 0x2E || op == 0x2F || op == 0x8E) return vec;  // masked stores (upper bound)
        } else if (map == 3) {
            if (op == 0x14) return 1;
            if (op == 0x15) return 2;
            if (op == 0x16) return w ? 8 : 4;
            if (op == 0x17) return 4;
            if (op == 0x19 || op == 0x39) return 16;  // vextractf128/vextracti128
            if (op == 0x1D) return l ? 16 : 8;        // vcvtps2ph
        }
        return 0;
    }
    if (code[i] == 0x0F) {
        const u8 op = code[i + 1];
        if (op == 0x3A) {
            const u8 op3 = code[i + 2];
            if (op3 == 0x14) return 1;
            if (op3 == 0x15) return 2;
            if (op3 == 0x16) return w ? 8 : 4;
            if (op3 == 0x17) return 4;
            return 0;
        }
        if (op == 0x38) return code[i + 2] == 0xF1 ? word : 0;  // movbe
        if (op == 0x11) return rep_f3 ? 4 : rep_f2 ? 8 : 16;    // movups/movupd/movss/movsd
        if (op == 0x13 || op == 0x17 || op == 0xD6) return 8;   // movlps, movhps, movq
        if (op == 0x29 || op == 0x2B || op == 0xE7) return 16;  // movaps, movntps, movntdq
        if (op == 0x7F) return (opsize || rep_f3) ? 16 : 8;     // movdqa/movdqu, mmx movq
        if (op == 0x7E && opsize) return w ? 8 : 4;             // movd/movq to memory
        if (op == 0xC3) return w ? 8 : 4;                       // movnti
        if (op == 0xB0 || op == 0xC0) return 1;                 // cmpxchg/xadd byte
        if (op == 0xB1 || op == 0xC1 || op == 0xAB || op == 0xB3 || op == 0xBB || op == 0xBA) return word;
        if (op == 0xC7) return w ? 16 : 8;                      // cmpxchg8b/16b
        if (op >= 0x90 && op <= 0x9F) return 1;                 // setcc
        return 0;
    }
    switch (code[i]) {
    case 0x88: case 0xC6: case 0x86: case 0x80: case 0xFE: case 0xC0: case 0xD0: case 0xD2: case 0xF6:
    case 0x00: case 0x08: case 0x10: case 0x18: case 0x20: case 0x28: case 0x30:
        return 1;
    case 0x89: case 0xC7: case 0x87: case 0x81: case 0x83: case 0xFF: case 0xC1: case 0xD1: case 0xD3: case 0xF7:
    case 0x01: case 0x09: case 0x11: case 0x19: case 0x21: case 0x29: case 0x31:
        return word;
    default:
        return 0;
    }
}

// A store to a GPU-cached page being executed against the page's backing memory: the register that
// was moved there and by how much (undone at the single-step exception after it).
thread_local int t_redirect_reg = -1;
thread_local u64 t_redirect_delta = 0;
// The bytes that store writes, reported again once it has executed (on_single_step).
thread_local std::array<std::pair<u64, u64>, 2> t_redirect_writes{};
thread_local int t_redirect_count = 0;

// Eden's write path for one store (Memory::HandleRasterizerWrite): pages only the CPU side holds
// are marked CPU-modified at once; on pages the GPU modified, the bytes written are collected and
// applied at the GPU's next sync, and only those lose the GPU's data. Every store goes through
// OnCPUWrite: it also drops the bytes from downloads still pending, which would otherwise copy the
// GPU's older data over them before the collected write is applied.
// HOST_WATCH_VA=<lo>-<hi> (hex): log write faults in that guest range and what the GPU caches
// did with them (debug aid for CPU writes the caches miss).
std::pair<u64, u64> watch_range() {
    static const std::pair<u64, u64> range = [] {
        const char* v = std::getenv("HOST_WATCH_VA");
        if (!v) return std::pair<u64, u64>{0, 0};
        char* end = nullptr;
        const u64 lo = std::strtoull(v, &end, 16);
        const u64 hi = end && *end == '-' ? std::strtoull(end + 1, nullptr, 16) : 0;
        return std::pair<u64, u64>{lo, hi};
    }();
    return range;
}
bool watched(u64 addr) {
    const auto [lo, hi] = watch_range();
    return addr >= lo && addr < hi;
}

void notify_write(u64 guest_addr, u64 size) {
    const u8* p = mem::backing_ptr(guest_addr);
    if (!p) return;
    if (watched(guest_addr)) [[unlikely]] {
        static std::atomic<u64> notes{0};
        const u64 n = ++notes;
        int devices = 0, collected = 0;
        thread_local Common::ScratchBuffer<u32> watch_scratch;
        DAddr first_device = 0;
        g_system->Host1x().MemoryManager().ApplyOpOnPointer(p, watch_scratch, [&](DAddr address) {
            if (devices++ == 0) first_device = address;
        });
        (void)collected;
        if (n <= 64 || (n & (n - 1)) == 0)
            LOG("watch: notify %#llx+%llu -> %d device address(es), first %#llx (%llu notifies)", guest_addr, size,
                devices, static_cast<unsigned long long>(first_device), n);
    }
    thread_local Common::ScratchBuffer<u32> scratch;
    static std::atomic<u32> next{0};
    thread_local const u32 core = next++ % Core::g_dirty.size();
    auto& gpu = g_system->GPU();
    g_system->Host1x().MemoryManager().ApplyOpOnPointer(p, scratch, [&](DAddr address) {
        if (gpu.OnCPUWrite(address, size)) Core::collect_write(core, address, size);
    });
}

}  // namespace

int store_address_register(const void* code) { return redirectable_address_register(static_cast<const u8*>(code)); }
u32 store_bytes(const void* code) { return store_width(static_cast<const u8*>(code)); }

bool on_single_step(void* context) {
    if (t_redirect_reg < 0) return false;
    auto* ctx = static_cast<CONTEXT*>(context);
    (&ctx->Rax)[t_redirect_reg] -= t_redirect_delta;
    t_redirect_reg = -1;
    ctx->EFlags &= ~0x100u;
    // Reported before the store too (that drops pending downloads of the bytes), but a GPU sync
    // on another thread between that and the store gathers or uploads the old bytes and counts
    // them as done: the GPU then kept half-old constants (camera matrices: one frame of the
    // world stretched into streaks every few hundred frames). Reporting them again here, before
    // the writing thread can signal anyone, makes the next sync take the stored bytes.
    for (int i = 0; i < t_redirect_count; i++) notify_write(t_redirect_writes[i].first, t_redirect_writes[i].second);
    t_redirect_count = 0;
    return true;
}

bool on_cpu_write_fault(u64 guest_addr, void* context) {
    if (!g_system || guest_addr >= mem::AddressSpaceSize) return false;
    if (watched(guest_addr)) [[unlikely]] {
        static std::atomic<u64> faults{0};
        const u64 n = ++faults;
        if (n <= 64 || (n & (n - 1)) == 0) {
            const u64 rip = context ? static_cast<CONTEXT*>(context)->Rip : 0;
            LOG("watch: write fault %#llx at %s, cached %d, redirect pending %d (%llu faults)", guest_addr,
                host_symbol(rip).c_str(), page_cached(guest_addr / mem::PageSize) ? 1 : 0, t_redirect_reg >= 0 ? 1 : 0, n);
        }
    }
    const u64 page = guest_addr / mem::PageSize;
    // A store to a page the GPU caches, as Eglantine's AOT handler does it: the caches are told
    // about these bytes only, the page stays protected, and the store itself goes to the page's
    // backing memory (the register addressing it is moved there for this one instruction, trap
    // flag, and moved back after). Unprotecting the page instead made every later store to it
    // invisible to the caches, so the whole page had to be reported as CPU-written: the buffer
    // cache then dropped what the GPU wrote to the rest of it and uploaded stale guest memory
    // over it (karts, items, rocks drawn black every other frame). A store may cross into the
    // neighbouring page (up to 32 bytes): only where the backing memory continues there.
    // HOST_WRITE_FAULT=page reports the whole page and unprotects it (the old path).
    static const bool per_page = [] {
        const char* v = std::getenv("HOST_WRITE_FAULT");
        return v && (std::strcmp(v, "page") == 0 || std::strcmp(v, "sync") == 0);
    }();
    const u64 offset = guest_addr & (mem::PageSize - 1);
    u8* const backing = mem::backing_ptr(page * mem::PageSize);
    const auto continues = [&](u64 other) {
        u8* b = other < mem::AddressSpaceSize / mem::PageSize ? mem::backing_ptr(other * mem::PageSize) : nullptr;
        return b && b == backing + (static_cast<s64>(other) - static_cast<s64>(page)) * static_cast<s64>(mem::PageSize);
    };
    auto* ctx = static_cast<CONTEXT*>(context);
    if (!per_page && ctx && backing && t_redirect_reg < 0 && page_cached(page) && (offset != 0 || continues(page - 1)) &&
        (offset <= mem::PageSize - 32 || continues(page + 1))) {
        const int reg = redirectable_address_register(reinterpret_cast<const u8*>(ctx->Rip));
        if (reg >= 0) {
            // The bytes written, from the instruction (32 when its form is not known: that covers
            // every store the recompiled code emits). The fault address is the first protected byte
            // the store touches, so a store reaching in from the previous page is over-reported by
            // the part it wrote there.
            static const bool exact_width = [] {
                const char* v = std::getenv("HOST_STORE_WIDTH");
                return !v || v[0] != '0';
            }();
            const u32 decoded = exact_width ? store_width(reinterpret_cast<const u8*>(ctx->Rip)) : 0;
            const u64 width = decoded != 0 ? decoded : 32;
            t_redirect_count = 0;
            t_redirect_writes[t_redirect_count++] = {guest_addr, std::min<u64>(width, mem::PageSize - offset)};
            if (offset + width > mem::PageSize && page_cached(page + 1))
                t_redirect_writes[t_redirect_count++] = {(page + 1) * mem::PageSize, offset + width - mem::PageSize};
            for (int i = 0; i < t_redirect_count; i++) notify_write(t_redirect_writes[i].first, t_redirect_writes[i].second);
            t_redirect_reg = reg;
            t_redirect_delta = reinterpret_cast<u64>(backing + offset) - reinterpret_cast<u64>(hp(guest_addr));
            (&ctx->Rax)[reg] += t_redirect_delta;
            ctx->EFlags |= 0x100u;
            static std::atomic<u64> redirected{0};
            if (const u64 n = ++redirected; (n & (n - 1)) == 0 && n >= 4096)
                LOG("video: %llu stores to GPU-cached pages redirected", n);
            return true;
        }
        static std::atomic<int> declined{0};
        if (declined++ < 16) {
            const u8* code = reinterpret_cast<const u8*>(ctx->Rip);
            LOG("video: store to GPU-cached page %#llx not redirected (%02x %02x %02x %02x %02x %02x)", guest_addr, code[0],
                code[1], code[2], code[3], code[4], code[5]);
        }
    }
    const auto view_protection = [&] {
        MEMORY_BASIC_INFORMATION mbi{};
        VirtualQuery(hp(page * mem::PageSize), &mbi, sizeof mbi);
        return mbi.State == MEM_COMMIT ? mbi.Protect & 0xff : PAGE_NOACCESS;
    };
    const bool marked = page_cached(page);
    if (!marked) {
        // The GPU may be uncaching the page right now (bit cleared, protection not yet lifted):
        // once that is done the page is writable again and the store can be retried.
        std::scoped_lock lk{g_cached_lock};
        const DWORD prot = view_protection();
        if (prot == PAGE_READWRITE) return true;
        // Only the GPU's tracking write-protects the view; a protected page without its mark is
        // still one the GPU cached (seen after remaps), so it is handled as one below.
        if (prot != PAGE_READONLY) return false;
    }
    const u8* p = mem::backing_ptr(page * mem::PageSize);
    if (!p) return false;
    // Stores that cannot be redirected (and host code writing guest memory): the whole page is
    // reported as written, then unprotected. HOST_WRITE_FAULT=sync downloads the page first and
    // waits for the host GPU (GPU::InvalidateRegion with preserve), ~30% slower races.
    thread_local Common::ScratchBuffer<u32> scratch;
    auto& gpu = g_system->GPU();
    bool deferred = false;
    static const bool sync = [] {
        const char* v = std::getenv("HOST_WRITE_FAULT");
        return v && std::strcmp(v, "sync") == 0;
    }();
    g_system->Host1x().MemoryManager().ApplyOpOnPointer(p, scratch, [&](DAddr address) {
        if (sync) {
            gpu.InvalidateRegion(address, mem::PageSize, true);
            return;
        }
        if (!gpu.OnCPUWrite(address, mem::PageSize)) return;
        deferred = true;
        static std::atomic<u32> next{0};
        Core::collect_write(next++, address, mem::PageSize);
    });
    std::scoped_lock lk{g_cached_lock};
    if (page_cached(page) || view_protection() == PAGE_READONLY) {
        // Still protected: let the write through. A deferred page stays counted as cached until
        // the GPU applies the collected write (then the caches release and re-protect it as usual).
        static std::atomic<int> forced{0};
        if (!deferred && forced++ < 16)
            LOG("video: page %#llx stayed write-protected after invalidation (%s), unprotecting", page * mem::PageSize,
                marked ? "marked" : "unmarked");
        set_cached(page, 1, false);
        mem::protect_view(page * mem::PageSize, mem::PageSize, true, true);
    }
    if (deferred) {
        static std::atomic<u64> deferred_count{0};
        if (const u64 n = ++deferred_count; (n & (n - 1)) == 0 && n >= 1024)
            LOG("video: %llu CPU writes to GPU-modified pages deferred to the GPU's next sync", n);
    }
    return true;
}

}  // namespace video
