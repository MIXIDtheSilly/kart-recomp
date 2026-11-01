// video_core glue: definitions of the emulator-core symbols video_core.lib links against
// (Core::System accessors, process memory, timing), the window and the GPU's setup.
#include <windows.h>
#include <psapi.h>

#include <atomic>
#include <cstring>
#include <mutex>
#include <optional>
#include <thread>
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

void System::GatherGPUDirtyMemory(std::function<void(PAddr, size_t)>& callback) {
    for (auto& manager : g_dirty) manager.Gather(callback);
}

// The host's setup: guest memory interface, host1x and the GPU with its renderer (needs the window).
void System::Initialize() {
    impl->memory.emplace(*this);
    impl->host1x.emplace(*this);
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
        const char* v = std::getenv("HOST_GPU_FENCES");
        return !v || std::strcmp(v, "guest") != 0;
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
    swprintf(line, 64, L"RAM        %5.1f GB", pmc.WorkingSetSize / 1073741824.0);
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
    v.async_presentation.SetValue(true);  // vsync waits stay off the GPU thread
    v.nvdec_emulation.SetValue(Settings::NvdecEmulation::Cpu);
    // Audio (audio_core): cubeb output; HOST_AUDIO_MUTE=1 keeps test runs silent.
    const char* mute = std::getenv("HOST_AUDIO_MUTE");
    v.audio_muted.SetValue(mute && mute[0] == '1');
    Settings::UpdateGPUAccuracy();
    Settings::SetCurrentProgramID(ProgramId);
    Settings::SetCurrentProgramIsSwitch2(true);
}

}  // namespace

void init() {
    std::error_code ec;
    std::filesystem::create_directories(Common::FS::GetEdenPath(Common::FS::EdenPath::ShaderDir), ec);
    g_cached_bits = new std::vector<std::atomic<u64>>(mem::AddressSpaceSize / mem::PageSize / 64);
    apply_settings();
    g_window = new HostWindow(create_window());
    g_system = new Core::System();
    g_system->Initialize();
    if (!g_gpu_created) fatal("could not create the GPU (Vulkan renderer)");
    LOG("video: GPU and Vulkan renderer created");
}

void report_frame_stats(double fps, double frame_ms, double gpu_wait_ms) {
    std::scoped_lock lk{g_stats_lock};
    g_stats = {fps, frame_ms, gpu_wait_ms, true};
}

void start() {
    g_system->GPU().Start();
    LOG("video: GPU thread started");
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
void notify_write(u64 guest_addr, u64 size) {
    const u8* p = mem::backing_ptr(guest_addr);
    if (!p) return;
    thread_local Common::ScratchBuffer<u32> scratch;
    static std::atomic<u32> next{0};
    thread_local const u32 core = next++ % Core::g_dirty.size();
    auto& gpu = g_system->GPU();
    g_system->Host1x().MemoryManager().ApplyOpOnPointer(p, scratch, [&](DAddr address) {
        if (gpu.OnCPUWrite(address, size)) Core::g_dirty[core].Collect(address, size);
    });
}

}  // namespace

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
            // The access size is unknown: 32 bytes covers every store the recompiled code emits,
            // and the dirty tracking works in 64-byte units anyway.
            t_redirect_count = 0;
            t_redirect_writes[t_redirect_count++] = {guest_addr, std::min<u64>(32, mem::PageSize - offset)};
            if (offset > mem::PageSize - 32 && page_cached(page + 1))
                t_redirect_writes[t_redirect_count++] = {(page + 1) * mem::PageSize, offset - (mem::PageSize - 32)};
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
        // The dirty managers track 2 KiB units.
        static std::atomic<u32> next{0};
        auto& dirty = Core::g_dirty[next++ % Core::g_dirty.size()];
        for (u64 off = 0; off < mem::PageSize; off += 0x800) dirty.Collect(address + off, 0x800);
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
