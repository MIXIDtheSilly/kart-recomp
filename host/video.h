// The GPU: Eglantine's video_core (Ampere command processing, shaders, Vulkan renderer) linked
// into the host. The host provides what video_core takes from the emulator core: guest memory
// (the physical pool), GPU-cached page tracking, a window and a clock.
#pragma once
#include "common.h"

namespace Core { class System; }
struct HWND__;

namespace video {

// HOST_NVN=native: NVN is the host's own (nvn/), on its own Vulkan device; no emulated GPU.
bool native_nvn();
// The game window (created by init).
::HWND__* main_window();

// Applies the renderer settings and creates the window, the GPU and the Vulkan renderer.
void init();
// Starts the GPU thread (once the guest's first GPU work can arrive).
void start();
// Runs the window's message loop on the calling thread until the window closes.
void run_window_loop();

// Present statistics for the overlay in the window's corner (vi2 reports them twice a second):
// frames per second, mean time between presents, mean wait for the frame's GPU fence.
void report_frame_stats(double fps, double frame_ms, double gpu_wait_ms);
// When report_frame_stats was last called (host_ns; 0 = never): frames are being shown while it advances.
unsigned long long last_frame_ns();

// The emulator-core object the GPU and the graphics services work through.
Core::System& system();

// HOST_GPU_FENCES=host: GPU fences count as reached for the game only once the host GPU has run
// the work before them and the GPU results the CPU reads are copied back (host syncpoint values,
// delayed fences). Default: as soon as the GPU thread has queued them (the game's CPU runs ahead).
bool host_fences();

// A guest write hit a page the GPU caches (write-protected in the view): drop the GPU's copies
// and unprotect it. Returns false if the address is not a GPU-tracked page.
// A guest write to a page the GPU caches; `context` is the faulting thread's CONTEXT (the store
// may be redirected to the page's backing memory and single-stepped: on_single_step undoes that).
bool on_cpu_write_fault(u64 guest_addr, void* context);
bool on_single_step(void* context);
// For the store at `code`: the register addressing its memory operand that can be moved (or -1), and the bytes it writes (0: unknown form).
int store_address_register(const void* code);
u32 store_bytes(const void* code);
// A guest access to a page with an armed read trap (GPU-written, not yet read by the CPU).
bool on_read_trap(u64 guest_addr, bool write);

}  // namespace video
