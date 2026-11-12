# Kart static recompilation

Goal: run the game's AArch64 code natively on x86-64 Windows by translating it ahead of time
to C++ and compiling it with MSVC.

The game files are never committed or redistributed. Every tool here reads them from a local dump,
and `out/`, `gen/`, `build/` and `work/` hold code derived from the game: keep them private.

## What the binaries look like (measured)

| module  | role                    | code     | instructions | functions | coverage |
|---------|-------------------------|----------|--------------|-----------|----------|
| main    | the game                | 40.2 MiB | 10.5 M       | 183,009   | 100 %    |
| sdk     | Nintendo SDK + libc++   | 7.5 MiB  | 2.0 M        | 44,712    | 99.86 %  |
| subsdk0 | `nnEcli` (eShop client) | 2.5 MiB  | 0.6 M        | 6,300     | 100 %    |
| rtld    | dynamic loader          | 8 KiB    | 2 k          | 32        | 100 %    |

* `main` makes no syscalls; it reaches the console only through 1,289 sdk imports.
* 121 k indirect calls (`blr`, mostly C++ virtual calls) and 13.9 k indirect jumps
  (2.2 k switch tables, the rest indirect tail calls).
* Pointer authentication is used everywhere (`pacibsp`/`retab`); the translator treats it as a no-op.

## Architecture (current route: Eglantine backend)

```
 rtld/main/subsdk0/sdk (AArch64) --lift.py--> <module>_*.cpp --MSVC /MD--> build/<module>.lib
                                                                              │
 Eglantine (deps\Eglantine + patches\eglantine.patch, GPL-3.0)                │
   core/arm/aot/arm_aot.cpp   CPU backend replacing dynarmic  ◄───────────────┘
   kernel, Switch 2 services, GPU (Ampere/NVN2), audio: unchanged emulator code
```

* Generated code is **position-independent**: every address is `MB + offset`, where `MB` is the
  module's load base (`g_base_<module>`), set by the loader hook. ASLR therefore works.
* Guest memory is the emulator's fastmem arena: `hp(a) = g_membase + a`. Pages the GPU tracks are
  protected; a vectored exception handler calls `Memory::InvalidateNCE` and retries (as NCE does).
* Each guest thread runs on its own boost.context fiber. `svc` switches back to the kernel
  (`HaltReason::SupervisorCall`), the next `RunThread` resumes after it. Loop back-edges poll
  `c->halt` so `SignalInterrupt` can preempt spinning threads.
* Indirect calls look up `<module>_table` (with a per-thread cache). The loader checks every
  module's build id against the one the code was recompiled from.
* Run with `EDEN_AOT=1`; without it the emulator uses dynarmic as before.

## Standalone runtime (`host/`, in progress)

`host\build.bat` links the same module libraries into `host\out\kart.exe <dump dir>`, with no emulator:

| file | role |
|------|------|
| `main.cpp` | loads the NSOs from `exefs/` (LZ4) at the console's addresses, runtime entry points, crash handler |
| `memory.cpp` | 512 GiB guest view over a physical pool, the kernel's block map (`svcQueryMemory`), remap notifications |
| `kernel.cpp` | system calls; guest threads are host threads under a per-core priority scheduler; mutex / condition variable / address arbiter / events under one kernel lock |
| `ipc.cpp` | HIPC with CMIF, TIPC, domains (shared by cloned sessions) and control commands |
| `services/` | sm, applet (appletOE), fs (RomFS image built over the loose `romfs/` dir, saves in `user/save`), hid, audren, time, acc, pctl, nifm (offline), friend, lm |
| `services/hid.cpp` | one Pro Controller on player 1, from every SDL3 gamepad (DualSense, DualShock, Xbox, Switch Pro; XInput if SDL can't start) and the keyboard (when the window has the focus), written into the hid shared memory in Eden's `SharedMemoryFormat` layout every 4 ms |
| `services/audio.cpp` | audren on Eglantine's `audio_core.lib` (Eden's renderer: voices, mixes, effects, output through cubeb/WASAPI, paced by the output stream); kernel objects it takes are stand-ins (the rendered event, the process for guest memory, the transfer memory's address) |
| `services/opus.cpp` | `hwopus` on libopus (the movies' audio) |
| `services/gsv.cpp` | the Switch 2 graphics server: `nx2:gsv` and the sessions it hands out (`syncpt`, `gpu`, `vi2`, `nvsrv`, the video decoder), ported from Eglantine's `nx2_gsv.h`. GPU-only regions get host memory in the code region; sparse regions are backed 4 MiB at a time as the GPU writes them; `vi2` 229 composites the presented image |
| `nv/` | Eglantine's nvdrv devices (`nvhost-as-gpu`, `-gpu`, `-ctrl`, `-ctrl-gpu`, `nvmap`, container, syncpoints) behind `nx2:nvsrv`, with `Kernel::KEvent` mapped to host events (`nv/kevent.h`) |
| `video.cpp`, `vma.cpp` | links Eglantine's `video_core.lib` (Ampere command processing, shader recompiler, Vulkan renderer): defines the 19 emulator-core symbols it needs (`Core::System` accessors, guest memory reads, GPU-cached page tracking, clock), the Win32 window, renderer settings (Eglantine's), and the CPU-write fault path |
| `overrides.cpp`, `graphics.cpp`, `nvn.cpp` | the earlier native-NVN experiment, off unless `HOST_HLE_GRAPHICS=1` |

`rtld` relocates the modules itself, as on the console. Logs go to `host\out\user\host.log`
(video_core logs through Eglantine's logger); `HOST_IPC_TRACE=1` logs every service request.

Guest memory is Eden's `Common::HostMemory`: a physical pool (16 GiB committed, `HOST_PHYS_GIB`)
mapped into the 512 GiB fastmem view. The GPU reads the pool directly, so pages it caches can be
write-protected in the view. `svcMapMemory` and shared memory are real aliases of physical pages.

The host compiles with Eglantine's core flags (`egl.rsp`: its defines and include paths, `/GR-`), so
class layouts match the prebuilt libraries in `deps\egl-build` (`EGL_BUILD`). Rebuild those first
(`python tools\setup_deps.py egl`) when Eglantine's `video_core` changes. `egl-build` is RelWithDebInfo with
`CMAKE_CXX_FLAGS_RELWITHDEBINFO=/O2 /Ob2 /DNDEBUG` (CMake's default `/Ob1` inlines only functions
marked inline: races ran ~5% slower).

Status: playable. Runs the game's real NVN driver: title screen, menus, course videos, races.
On the RTX 3080: menus 60 fps, races about 43-50 fps (Eglantine: 40-55), with sound.

Controls (Eden's defaults): left stick WASD, right stick IJKL, D-pad arrows, A = C, B = X, X = V,
Y = Z, L = Q, R = E, ZL = R, ZR = T, + = M or Enter, - = N. Gamepads map by position (the bottom
face button, Cross or Xbox A, is B; the right one A), triggers are ZL/ZR, Options/Start is +,
Create/Back and the touchpad are -. F3 toggles the performance overlay (FPS, frame time, GPU
fence wait, RAM) in the top-left corner.

What it took, beyond the services:
* **Scheduling as on the console**: one guest thread per core (cores 0-5 from the NPDM), the
  highest-priority ready thread runs, a thread runs on any core of its affinity mask. With every
  thread running in parallel the game's job workers read data before it was written. Threads give
  up their core when they wait, sleep or make a service request; preemption uses `ctx.halt`.
  `svcSleepThread(0)` yields only to equal or higher priority. `HOST_NO_SCHED=1` turns it off.
* **The GPU follows the CPU's mappings**: when the guest maps or unmaps memory under a GPU
  mapping, the SMMU pages are mapped again (`nvhost_as_gpu::Nx2Remap`). Stale, the GPU read pages
  the pool had given to other memory and ran them as commands. GPU unmaps (regions and memory
  the game destroys) wait for the GPU thread to finish the work queued before them, maps happen at
  once; a queued unmap skips what was mapped again after it was queued (title 1 destroys and
  re-creates GPU-only regions at the same addresses at course loads: the late unmap left the new
  region unmapped). `HOST_STALE_UNMAP=1` restores the old behaviour.
* Faults on pages another thread is remapping are retried (Windows unmaps a whole view to unmap
  part of it). The guest counter runs at 31.25 MHz (the Switch 2 SDK's constant). The host timer
  resolution is raised to 0.5 ms as in Eden (frames took twice as long at the default).
  Guest sleeps under 4 ms wait on the clock for their last 700 us (`HOST_SLEEP_SPIN_US`, 0 = timer
  only): the game's job loops sleep 20 us between polls (main thread and workers ~100 times a
  second each in races) and a Windows timer made each ~0.5 ms. Races +2-4% (A/B pairs 42.1/44.0,
  41.0/42.0 fps).
* **CPU writes to GPU-cached pages** are tracked per store, as Eden's write path and Eglantine's
  AOT handler do: the fault reports 32 bytes at the address (`OnCPUWrite`; on GPU-modified pages
  collected in `GPUDirtyMemoryManager`), the page stays write-protected, and the store runs against
  the page's backing memory (its address register is moved there for one single-stepped
  instruction, `video::on_single_step`). The collected writes are applied in order before every
  GPFIFO submit (`nvhost_gpu::SubmitGPFIFOImpl`), not only at fences. Reporting whole pages
  (`HOST_WRITE_FAULT=page`) made the buffer cache drop the GPU's data in the rest of the page and
  upload stale guest memory over it: objects or the whole world drawn black every other frame.
  Applying the writes only at fences left frames with stale data (stretched triangles, empty
  frames). `HOST_WRITE_FAULT=sync` downloads the page and waits for the host GPU on each fault.
  Races ~20k redirected stores/s, 40-50 fps.
* **GPU results the CPU reads** (its auto-exposure among them: read as zeros, the world was white)
  reach guest memory through read traps (`Memory::RasterizerArmReadTraps`): GPU-written pages lose
  all access until the CPU's first read, which downloads them and makes the buffer cache copy that
  range back at every fence. `HOST_READ_TRAPS=0` turns them off. `EDEN_NX2_BUFFER_DOWNLOADS=1`
  copies every GPU-written buffer back instead; those copies land after the CPU has written the
  next frame's data next to them (camera data: frames of streaks).
* **Testing for flicker**: `HOST_FRAME_CAPTURE_EVERY=1` with `HOST_FRAME_CAPTURE_FROM/_TO` and
  `HOST_FRAME_CAPTURE_WIDTH=640` captures consecutive frames; compare neighbours for cells that go
  dark or bright for one capture.
* **Memory**: the pool is 24 GiB (`HOST_PHYS_GIB`). Sparse GPU regions (two ~6 GB virtual textures)
  get 4 MiB blocks as the GPU first writes them and keep them while the region lives (~250 MB more
  per course); GPU-only memory leaves the pool's last GiB to the game, past that sparse writes drop.
  Sparse blocks without GPU writes for 30 s (`HOST_TRIM_SPARSE_S`) and, every 60 s
  (`HOST_TRIM_GPU_ONLY`), the whole GPU-only regions leave the working set (`VirtualUnlock`; 0 turns
  either off): their data was uploaded already and comes back from the standby list or pagefile if
  read again. Guest memory resident after 10 minutes of races: ~3 GB instead of ~6 GB, same frame
  rate. `HOST_MEM_REPORT=1` logs residency per kind once a minute. The overlay's RAM counts pages
  mapped in both views (guest address space and the GPU's) once; the working set counts them twice.
* **Disk pipeline cache** (`user/cache/shader/<title>`): loaded at GPU start, new pipelines are saved
  as they're built. Without it every run compiled each pipeline again (hitches at every new scene).
  `nx2:vi2 hitch` log lines name frames over 100 ms.
* Video: gsv 902/906/1002 sessions decode with Eglantine's `Nx2VideoDecoder` (FFmpeg, VP9) into the
  output images (gsv 991/911/961), counting finished frames on a syncpoint of their own.
* Unknown TIPC commands reply like Eglantine's auto stub: the result and one zero word, the rest
  of the message keeps the request's words (the SDK reads them back).

Presents wait for the frame's own GPU fence. `HOST_FRAMES_AHEAD=1` waits for the previous one's
(Eglantine's default): faster, but the game then rewrites command memory the GPU hasn't read
(`EDEN_NX2_PB_CHECK=1` reports it, with the small entries' words at submit in host.log) and the GPU
soon runs other data (crash ~16 s in). The rewritten entries are NVN's 3-word queue-ring commands,
read 320 ms after submit. The game's render thread already builds frame N+1 while N runs; one more
frame lets it reuse N's memory. Copying entries at submit is no fix: NVN2 also submits command
buffers before writing them, behind a semaphore (NX2_NOTES row 50).
`HOST_FRAMES_AHEAD_AFTER=<presents>` starts running ahead only after boot: menus then hold 60 fps
with ~0 ms fence wait, and the one run that got through course select raced at ~60 fps (vs ~40).
But 4 of 5 runs broke at the course-select load (~100 s): GPU state from garbage commands, then a
crash or a GPU hang. Waiting until every channel had read all command lists submitted before the
previous present (also ones suspended on semaphores) changed nothing, so unread GPFIFO entries are
not the cause. What the traces show: per frame, channel 2 compute releases 1..5 on a sync
semaphore (0x21f6693040 / ..140, alternating per frame) that channel 1 acquires (>= 1..5); the CPU
resets it to 0 (no GPU release of 0 ever), and channel 2 itself waits on CPU-released gates
(..3000 / ..3100). At the crash, channel 1 resumed on ..3140 right after channel 2's gate and then
parsed commands the CPU was still writing. The open question is which ordering the console
guarantees there and the emulated GPU doesn't. `EDEN_NX2_LONG_ACQUIRE_MS=<ms>` logs channel waits
on semaphores longer than that; `EDEN_NX2_SEM_WATCH=<address>` every release/acquire on one.
What limits a race (2026-11-05, `HOST_TIMELINE`): a frame (~21-23 ms) is the GPU threads running
the submitted frame (~11 ms, while the render thread waits in present) and then the render thread
building the next one (~9 ms). Running ahead does not overlap them: with present no longer waiting,
the render thread instead waits ~26 ms right after present on an address-arbiter word another game
thread releases (the next frame's simulation data). Race frames therefore stay at ~22 ms whatever
the GPU side does; tried without gain: draining the split's front less (`EDEN_NX2_SPLIT_OWN_ACQUIRES`,
the back then idles only ~3%), the modules built with `/arch:AVX2`, spinning before sleeping in kernel
waits (`HOST_SPIN_US`). Guest cores are ~45-60% busy (`HOST_SCHED_STATS=1`), the game's busiest job
threads 25-40%: the game's own job chain sets the pace. The zone profiler (`EDEN_NX2_GPU_PROFILE=1`)
adds so much per-call time that it shows the back thread saturated; sampling shows it ~70% busy.
2026-11-06 re-check (correcting the above): the game's threads are mostly idle in races (main
thread ~17% of a core, the busiest job thread ~27%, cores 280% of 600%); `HOST_SCHED_STATS=1` now
also logs wakeup latency (~20k/s, mean ~9 us: not a cost) and the threads that queue longest for a
core (low-priority single-core threads; the main thread ~12 ms/s). The render thread waits on the
frame's fence ~19 of ~23 ms: the GPU emulation is the limit. Its two threads mostly wait for each
other: the front ~45% in `GpuSplit::Drain` and ~18% for the host GPU (GPU-written command lists,
below), the back ~50% idle - a frame costs about the front's work plus the back's plus the host
GPU round trips, run one after another. GPU-written "command lists" are the 5 parameters of macro
0xe50/0xe51 (topology, then count, instances, first, base vertex, base instance, written by the
culling compute; ~9k/s, many with 0 instances): the macro writes base vertex/instance to the
driver constant buffer (0x640/0x644) and draws once per instance. Not waiting for them
(`EDEN_NX2_GPU_WRITTEN_COMMANDS=0`, stale words: missing trees) is ~6% faster (43.3/47.0,
45.3/47.8). Diagnostics: `EDEN_NX2_GPU_WRITTEN_DUMP=1` (their words and the pending method),
`EDEN_NX2_MACRO_TRACE=<method hex>,...` (parameters and register writes of 40 non-culled runs).

Course-load crashes with frames ahead: during loads the split's back falls tens of ms behind, and
the front's wait for it on one channel's acquire held every channel; the compute channel's
CPU-released gate stayed unconsumed for 65 ms and the game recycled it (written back to 0), after
which the next gate opened early and the graphics channel parsed half-written commands. Acquires
whose word holds its final value now suspend without waiting for the back (`EDEN_NX2_SPLIT_DRAINLESS=0`
reverts), and compute QMD releases are tracked per word, but loads still break with frames ahead:
the remaining waits are on words with queued releases.

Diagnostics: `HOST_TIMELINE=<file>` (submits, fences, syncpoint waits, presents, guest waits of 100+
us, sleeps; microseconds), `HOST_POLL_VA=<hex>,...` (with the timeline: changes of guest words, with
the steady clock of the front trace), `EDEN_NX2_FRONT_TRACE=1` (the GPU front's last 512 acquires,
resumes, releases, QMD signals and entries, dumped at garbage commands or a failed macro),
`HOST_SCHED_STATS=1` (guest core use and ready-queue waits every 5 s), `EDEN_NX2_SPLIT_ACQUIRE_STATS=1`
(why acquires wait for the back, unknown writes by source). `HOST_FRAMES_AHEAD_ALL=1` also waits for
every channel's fence at present (no change).

Run options: `HOST_VULKAN_DEVICE` (default 2), `HOST_RENDER_HIDDEN=1` and
`HOST_FRAME_CAPTURE=<dir>` (a PNG every 300 presents) for hidden test runs, `HOST_DUMP_AFTER=<s>`
(guest thread states), `HOST_NV_TRACE=1` (ioctls), `HOST_INPUT_SCRIPT="<s>:<buttons>:<duration>;..."`
(scripted presses for test runs, e.g. `30:L+R:0.4;35:A:0.2`), `HOST_ASTC=cpu|async` (ASTC decodes
on the GPU by default; async shows streamed textures black until decoded), `HOST_AUDIO_MUTE=1`, `HOST_NO_OVERLAY=1`. A crash dump lists
every guest thread.

NVN cannot be replaced function by function. The driver is in `sdk` (a sorted table of 698 names
at sdk+0xb79b90, implementations at sdk+0xd95168). A trace of the real driver in Eglantine
(`EDEN_AOT_NVN_TRACE`, `EDEN_AOT_NVN_PROCS`; see `arm_aot.cpp`) shows the game calls 196 of them,
only for objects and state. It writes draws (~55k a second) and compute dispatches straight into
command memory as Ampere methods and Turing MME macro calls. So the host runs the real NVN driver
and Eglantine's GPU consumes the command stream.

## Tools

| tool | purpose |
|------|---------|
| `nso.py` | unpack NSOs, verify hashes, symbols/relocs/init arrays, ELF for Ghidra |
| `scan.py` | instruction census |
| `funcs.py` | function bounds from `.eh_frame` plus call targets |
| `a64.py`, `neon.py` | instruction decoder → C++ (integer, FP, atomics, NEON) |
| `lift.py` | per-function C++, switch-table recovery (reaching definitions), chunked output |
| `difftest.py` | compare recompiled leaf functions with Unicorn, registers + memory |
| `build_lib.py`, `build_all.py` | compile modules into static libraries |
| `gen_glue.py` | module descriptors (`gen/aot_modules.cpp`) |

Verification so far: 2,481 integer and 997 SIMD leaf functions identical to Unicorn on every case.

## Building

You need your own dump of the game (its `exefs\` and `romfs\` folders). Two versions are known:

| version | modules (build id) |
|---------|--------------------|
| 1.0.0 | `rtld` 35b31ae2..., `main` 9e244438..., `subsdk0` (nnEcli) d39a7a3d..., `sdk` 90ebba18... |
| 1.8.0 | `rtld`, `main`, `subsdk0` (movie, new), `subsdk1` (nnEcli), `sdk` (`out\<module>.json` after step 3) |

The tools take the module list from the dump (`out\layout.json`), the game patch (`tools\patches.py`)
finds its site by its code, and `host\` binds to the sdk through its exports, so another version
needs the steps below again, not code changes. `tools\hot_<module>_<build id>.txt` lists the
functions MSVC needs its huge-function optimization for (C4883); for a new version, build once with
`BUILD_WARN_HUGE=1` and list the functions `build\main_cl.log` names.

A dump in the NUP container format (one file holding the base game, updates and add-ons) unpacks
with `python tools\nup.py list <file>` / `extract <file> <dump folder>` (newest update by default).

Two versions side by side: with `KART_VER=<version>` in the environment, steps 3-5 write to
`ver\<version>\{out,gen,build}` and `work\nvn_shaders-<version>`, and `host\build.bat` links
`host\out\kart-<version>.exe`. That exe takes its dump from `KART_GAME_<version>` in `local.cfg`
and keeps saves, caches and logs in `host\out\user-<version>` and pipelines in
`host\out\nvn_pipelines-<version>.bin`. Leave `KART_VER` out of `local.cfg`: there it would switch
every build.

Requirements: Windows 10/11 x64; Visual Studio 2026 with "Desktop development with C++" (it brings
CMake and Ninja); Git; Python 3.13 with `pip install backports.zstd` (and `unicorn` for
`difftest.py`); the .NET 10 SDK (shader translator); a Vulkan 1.3 GPU. Optional: LLVM for
`BUILD_CC=clang`. Keep the repository in a path without spaces.

The launcher does the steps below for you, one at a time: `launcher\build.bat` (it needs only Visual
Studio), then `launcher\out\launcher.exe`. It checks the dump's version, writes `local.cfg`, shows what
is missing and runs the tools with their output in its window. By hand:

```
:: 1. Where the dump is (local.cfg is not committed; tools\paths.py lists every setting)
echo KART_GAME=D:\dumps\kart> local.cfg

:: 2. Dependencies into deps\: Eglantine and plumber-racer-2 cloned at pinned commits, patches\ applied,
::    Eglantine's libraries and the shader translator built (an hour or more, mostly Eglantine)
python tools\setup_deps.py

:: 3. Unpack and lift the game's modules, then compile them (~25 min for all four)
python tools\nso.py D:\dumps\kart\exefs out
for %m in (rtld main subsdk0 sdk) do python tools\scan.py out %m && python tools\funcs.py out %m
python tools\build_all.py
python tools\gen_glue.py

:: 4. The game's shaders, translated to SPIR-V into work\nvn_shaders
python tools\nvn_shaders.py

:: 5. The host and the run
host\build.bat
host\out\kart.exe
```

`kart.exe` takes the dump folder as its first argument, else `KART_GAME` from the environment or
`local.cfg`. `kart.exe <dump> --bake-textures` fills the decoded texture cache
(`work\nvn_texcache`) ahead of time; pipelines are made ahead of time on the first runs.
`BUILD_MP=<n>` caps parallel compiles in every build step.

The dependencies stay where they are upstream; this repository keeps only its changes to them:

| dependency | upstream | patch | used for |
|------------|----------|-------|----------|
| Eglantine (GPL-3.0) | [Djihads80/Eglantine](https://github.com/Djihads80/Eglantine) `adae763570` | `patches\eglantine.patch` | GPU (`video_core`), audio, the AOT CPU backend (`src\core\arm\aot`) |
| plumber-racer-2 (MIT) | [cheemu21/plumber-racer-2](https://github.com/cheemu21/plumber-racer-2) `843170ac3e` | `patches\plumber-racer-2.patch` | shader splitting and the Ryujinx-based shader translator |

After changing a dependency in its `deps\` folder, `python tools\setup_deps.py patches` writes its
patch again. To use existing clones elsewhere, point `EGL`, `EGL_BUILD` and `NVN_REF` at them in
`local.cfg`.

The emulator route (recompiled code inside Eglantine instead of the standalone host):

```
cmake -S deps\Eglantine -B deps\egl-build -DEDEN_AOT_DIR=<this repository>
cmake --build deps\egl-build --target yuzu-cmd
set EDEN_AOT=1 && deps\egl-build\bin\eden-cli.exe -g "<dump>\exefs\main"
```

## License

GPL-3.0-or-later (`LICENSE`): `host\nv\` and `host\services\gsv.cpp` come from Eden / yuzu
(GPL-3.0-or-later), and `kart.exe` links Eglantine's GPL libraries. `host\nvn\nvn.h` is MIT
(h1k421), `tools\texbake\stb_image_write.h` public domain / MIT. The game's code and data are not
part of this repository and must never be added to it.
