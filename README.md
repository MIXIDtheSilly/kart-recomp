# Kart static recompilation

Goal: run the game's AArch64 code natively on x86-64 Windows by translating it ahead of time
to C++ and compiling it with MSVC.

The game files are never committed or redistributed. Every tool here reads them from a local dump,
and `out/`, `gen/`, `build/`, `ver/` and `work/` hold code and data derived from the game: keep them
private.

## What the binaries look like (measured, 1.0.0)

| module  | role                    | code     | instructions | functions | coverage |
|---------|-------------------------|----------|--------------|-----------|----------|
| main    | the game                | 40.2 MiB | 10.5 M       | 183,009   | 100 %    |
| sdk     | Nintendo SDK + libc++   | 7.5 MiB  | 2.0 M        | 44,712    | 99.86 %  |
| subsdk0 | `nnEcli` (eShop client) | 2.5 MiB  | 0.6 M        | 6,300     | 100 %    |
| rtld    | dynamic loader          | 8 KiB    | 2 k          | 32        | 100 %    |

* `main` makes no syscalls. It reaches the console only through 1,289 sdk imports.
* There are 121 k indirect calls (`blr`, mostly C++ virtual calls) and 13.9 k indirect jumps
  (2.2 k switch tables; the rest are indirect tail calls).
* Pointer authentication is used everywhere (`pacibsp`/`retab`). The translator treats it as a no-op.

## How it runs

```
 rtld/main/subsdk*/sdk (AArch64) --lift.py--> <module>_*.cpp --MSVC /MD--> build/<module>.lib
                                                                                 │
 host\ (this repository): loader, kernel, services, native NVN on Vulkan  ◄──────┘  =  host\out\kart.exe
```

* Generated code is **position-independent**: every address is `MB + offset`, where `MB` is the
  module's load base (`g_base_<module>`). `rtld` relocates the modules itself, as on the console.
* Guest memory is a fastmem view: `hp(a) = g_membase + a`. Indirect calls look up
  `<module>_table` (with a per-thread cache). The loader checks every module's build id against the
  one the code was recompiled from.
* Guest threads are host threads. `svc` instructions call the host kernel directly.
* The game's graphics API, NVN, is implemented natively (`host\nvn\`). The game's NVN lookups
  resolve to the host's functions, so the sdk's NVN driver never runs. The game's shaders are
  translated to SPIR-V ahead of time.

## The host (`host\`)

| file | role |
|------|------|
| `main.cpp` | loads the NSOs from `exefs\` at the console's addresses; runtime entry points; crash handler; stall watchdog |
| `memory.cpp` | the 512 GiB guest view over a physical pool, and the kernel's block map (`svcQueryMemory`) |
| `kernel.cpp` | system calls. Guest threads run under a per-core priority scheduler; mutexes, condition variables, the address arbiter and events share one kernel lock |
| `ipc.cpp` | HIPC with CMIF, TIPC, domains (shared by cloned sessions) and control commands |
| `services\` | sm, applet (appletOE), fs (a RomFS image built over the loose `romfs\` folder; saves in `user\save`), hid, audren, hwopus, time, acc, pctl, nifm (offline), friend, chat, lm |
| `services\hid.cpp`, `gamepads.cpp` | one Pro Controller on player 1 (Handheld when the game doesn't take Fullkey), written into nn::hid's shared memory (`hid_shared.h`) every 4 ms. Input comes from Xbox pads (XInput), DualShock 4 and DualSense (raw HID reports, over USB and Bluetooth) and the keyboard |
| `audio_core\`, `services\audio.cpp` | audren: Eden's audio renderer (voices, mixes, effects), adapted to the host and played through WASAPI. The output stream sets the pace |
| `services\opus.cpp` | `hwopus` on libopus (the movies' audio) |
| `services\gsv.cpp`, `vdec_mf.cpp` | the Switch 2 graphics server (`nx2:gsv`: syncpoints, native memory and images, the video decoder). Movies are VP9, decoded with Media Foundation |
| `syncpoints.cpp` | the graphics syncpoints the game waits on |
| `nvn\` | native NVN: devices, memory pools, textures, programs, command buffers (recorded as records in the game's command memory) and queues |
| `nvn\vk\` | the Vulkan renderer behind it: submissions replayed into Vulkan command buffers (on a driver thread), pipelines, textures (ASTC decoded on the CPU and encoded to BC3, cached on disk), sparse textures, present |
| `video.cpp` | the game window, and the F3 performance overlay |
| `watch.cpp` | a debug write watch on guest memory |
| `localcfg.cpp` | reads `KART_GAME` and the `HOST_*` settings from `local.cfg` |
| `overrides.cpp`, `graphics.cpp`, `nvn.cpp` | an earlier native-NVN experiment, off unless `HOST_HLE_GRAPHICS=1` |

Logs go to `host\out\user\host.log`. `HOST_IPC_TRACE=1` logs every service request.

How the host works:
* **Scheduling as on the console**: one guest thread per core (cores 0-5, from the NPDM). The
  highest-priority ready thread runs, and a thread can run on any core in its affinity mask. When
  every thread ran in parallel, the game's job workers read data before it was written. Threads
  give up their core when they wait, sleep or make a service request. `svcSleepThread(0)` yields
  only to equal or higher priority. `HOST_NO_SCHED=1` turns the scheduler off.
* **Memory**: the physical pool is a pagefile-backed section of 24 GiB (`HOST_PHYS_GIB`; it asks
  for less if Windows refuses), mapped whole as the backing view. The guest view is a placeholder
  reservation holding one view of the section per mapping. So `svcMapMemory` and shared memory are
  real aliases of the same pages. Memory the game frees goes back to Windows.
* Faults on pages another thread is remapping are retried, because Windows unmaps a whole view to
  unmap part of it. The guest counter runs at 31.25 MHz (the Switch 2 SDK's constant). The host
  timer resolution is raised to 0.5 ms. Guest sleeps under 4 ms wait on the clock for their last
  700 us (`HOST_SLEEP_SPIN_US`; 0 means timer only), because the game's job loops sleep 20 us
  between polls.
* **Frames**: the game steps its simulation once per 1/60 s present interval, so present is fixed
  at 60 Hz (`HOST_NVN_PRESENT_HZ=0` uncaps it, for profiling only). Pipelines are built ahead of
  time from the shaders' pair tables and kept in `host\out\nvn_pipelines.bin`. Decoded textures are
  kept in `work\nvn_texcache`.

Controls: the left stick is WASD, the right stick IJKL and the D-pad the arrow keys. A = C, B = X,
X = V, Y = Z, L = Q, R = E, ZL = R, ZR = T, + = M or Enter, - = N. Gamepads map by position: the
bottom face button (Cross, Xbox A) is B and the right one is A. The triggers are ZL/ZR,
Options/Start is +, and Create/Back and the touchpad are -. F3 toggles the performance overlay (FPS,
frame time, RAM) in the top-left corner.

Run options (environment, or `HOST_*` lines in `local.cfg`):

| option | effect |
|--------|--------|
| `HOST_HANDHELD=0` | docked (higher resolution); handheld by default |
| `HOST_PRESENT_MODE=fifo\|immediate` | swapchain present mode; mailbox by default |
| `HOST_VULKAN_DEVICE=<index>` | which GPU; by default the discrete GPU with the most memory |
| `HOST_FPS_MODE=dynamic\|60\|30\|game` | the game's present interval; dynamic keeps the game clock at real time when frames run late |
| `HOST_AUDIO_MUTE=1`, `HOST_AUDIO_LEVEL=1` | silence the output / log its peak level |
| `HOST_NO_OVERLAY=1` | no performance overlay |
| `HOST_NVN_TEXCACHE=<dir>\|0` | decoded texture cache folder, or none; `HOST_NVN_TEXCACHE_MB` caps it (64 GiB) |
| `HOST_TRANSCODE_THREADS=<n>` | threads for ASTC decoding / BC encoding (a quarter of the cores by default) |
| `HOST_RENDER_HIDDEN=1`, `HOST_INPUT_SCRIPT="<s>:<buttons>:<duration>;..."` | hidden test runs with scripted presses, e.g. `30:L+R:0.4;35:A:0.2` |
| `HOST_STALL_DUMP_S=<s>` | after frames stop for this long, log every guest thread's state and stack (10 s by default) |
| `HOST_TIMELINE=<file>`, `HOST_SCHED_STATS=1`, `HOST_DUMP_AFTER=<s>` | diagnostics: timed events, guest core use, thread states |
| `HOST_NVN_CHECKPOINTS=1`, `HOST_VK_VALIDATION=1` | GPU diagnostics: the last draws before a device loss, Vulkan validation |

## Tools

| tool | purpose |
|------|---------|
| `nso.py` | unpack NSOs, verify hashes; symbols, relocations and init arrays; ELF for Ghidra |
| `nup.py` | unpack a NUP container (base game, updates, add-ons) into a dump folder |
| `scan.py` | instruction census |
| `funcs.py` | function bounds from `.eh_frame`, plus call targets |
| `a64.py`, `neon.py` | instruction decoder → C++ (integer, FP, atomics, NEON) |
| `lift.py` | per-function C++, switch-table recovery (reaching definitions), chunked output |
| `difftest.py` | compare recompiled leaf functions with Unicorn (registers and memory) |
| `build_lib.py`, `build_all.py` | compile modules into static libraries |
| `gen_glue.py` | module descriptors (`gen\aot_modules.cpp`) and the library list the link uses |
| `nvn_shaders.py` | split the game's shader archives (`shader_split\`) and translate them to SPIR-V (`xlate\`, built on first use) |
| `nvn_refl.py`, `nvn_mesh.py`, `nvn_spvfix.py` | the renderer's shader reflection table, mesh-shader variants, and fixes for shaders a driver miscompiles |
| `paths.py` | where the dump and the compilers are (environment, else `local.cfg`) |

Verification so far: 2,481 integer and 997 SIMD leaf functions identical to Unicorn on every case.

## Building

You need your own dump of the game (its `exefs\` and `romfs\` folders). Two versions are known:

| version | modules (build id) |
|---------|--------------------|
| 1.0.0 | `rtld` 35b31ae2..., `main` 9e244438..., `subsdk0` (nnEcli) d39a7a3d..., `sdk` 90ebba18... |
| 1.8.0 | `rtld`, `main`, `subsdk0` (movie, new), `subsdk1` (nnEcli), `sdk` (`out\<module>.json` after step 2) |

The tools take the module list from the dump (`out\layout.json`), and the game patch
(`tools\patches.py`) finds its site by its code. `host\` binds to the sdk through its exports. So
another version needs the steps below run again, not code changes. `tools\hot_<module>_<build id>.txt`
lists the functions MSVC needs its huge-function optimization for (C4883). For a new version, build
once with `BUILD_WARN_HUGE=1` and list the functions `build\main_cl.log` names.

A dump in the NUP container format (one file holding the base game, updates and add-ons) unpacks
with `python tools\nup.py list <file>` / `extract <file> <dump folder>` (newest update by default).

Two versions side by side: with `KART_VER=<version>` in the environment, steps 2-4 write to
`ver\<version>\{out,gen,build}` and `work\nvn_shaders-<version>`, and `host\build.bat` links
`host\out\kart-<version>.exe`. That exe takes its dump from `KART_GAME_<version>` in `local.cfg`. It
keeps saves, caches and logs in `host\out\user-<version>`, and pipelines in
`host\out\nvn_pipelines-<version>.bin`. Leave `KART_VER` out of `local.cfg`, because there it would
switch every build.

Requirements:
* Windows 10/11 x64.
* Visual Studio 2026 with "Desktop development with C++".
* Python 3.13 with `pip install backports.zstd` (and `unicorn` for `difftest.py`). Python 3.14
  needs no extra package.
* The .NET 10 SDK, for the shader translator.
* A Vulkan 1.3 GPU.
* For the movies: Microsoft's VP9 Video Extensions (without them, movies are skipped).
* Optional: LLVM, for `BUILD_CC=clang`.

Keep the repository in a path without spaces. Nothing else is downloaded: the third-party code the
host uses is in `third_party\`.

The launcher does the steps below for you, one at a time. Run `launcher\build.bat` (it needs only
Visual Studio), then `launcher\out\launcher.exe`. It checks the dump's version, writes `local.cfg`,
shows what is missing, and runs the tools with their output in its window. Its version buttons (top
right) switch every step between 1.0.0 and 1.8.0 as described above. The choice is kept as
`LAUNCHER_VERSION` in `local.cfg`. To do the steps by hand:

```
:: 1. Where the dump is (local.cfg is not committed; tools\paths.py lists every setting)
echo KART_GAME=D:\dumps\kart> local.cfg

:: 2. Unpack and lift the game's modules, then compile them (~25 min for all four)
python tools\nso.py D:\dumps\kart\exefs out
for %m in (rtld main subsdk0 sdk) do python tools\scan.py out %m && python tools\funcs.py out %m
python tools\build_all.py
python tools\gen_glue.py

:: 3. The game's shaders, translated to SPIR-V into work\nvn_shaders. Then the renderer's reflection
::    table (nothing is drawn without it), the mesh shaders' per-vertex variants, and the shaders the
::    NVIDIA driver miscompiles as they are
python tools\nvn_shaders.py
python tools\nvn_refl.py
python tools\nvn_mesh.py --flat
python tools\nvn_spvfix.py --all

:: 4. The host, and the run
host\build.bat
host\out\kart.exe
```

`kart.exe` takes the dump folder as its first argument, else `KART_GAME` from the environment or
`local.cfg`. `kart.exe <dump> --bake-textures` fills the decoded texture cache ahead of time.
Pipelines are made ahead of time over the first runs. `BUILD_MP=<n>` caps parallel compiles in every
build step.

## License

GPL-3.0-or-later (`LICENSE`). Some of the code comes from other projects; `third_party\README.md`
lists each with its license:
* `host\audio_core\` and the ASTC decoder come from Eden / yuzu (GPL).
* `host\services\gsv.cpp` is ported from Eden-based code (GPL-3.0-or-later).
* The shader tools come from plumber-racer-2 and Ryujinx (MIT).
* Vulkan-Headers, VMA, LZ4, Zstandard, Opus and stb_dxt are in `third_party\`.
* `host\nvn\nvn.h` is MIT (h1k421), and `tools\texbake\stb_image_write.h` is public domain / MIT.

The game's code and data are not part of this repository and must never be added to it.
