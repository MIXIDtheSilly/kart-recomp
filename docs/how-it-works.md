# How it works

Everything the [README](../README.md) leaves out: playing, building by hand, what the game's binaries
look like, how the recompiled code runs, what the host does, every setting, and the tools.

## Playing

Plug in an Xbox controller, DualShock 4, DualSense or Switch Pro Controller (USB or Bluetooth), or use
the keyboard. Up to 4 players: each controller joins as the next player when it first presses a button
(that press is not passed on), so have every player press one before picking multiplayer. The keyboard
always drives player 1; with `HOST_KEYBOARD_JOINS=1` it joins as a player of its own instead, like a controller (one
controller and the keyboard make two players). `HOST_PLAYERS=1` makes every controller drive player 1.

| Switch | Keyboard |
|--------|----------|
| Left stick / right stick | WASD / IJKL |
| D-pad | arrow keys |
| A, B, X, Y | C, X, V, Z |
| L, R, ZL, ZR | Q, E, R, T |
| + / - | M or Enter / N |

On a gamepad, buttons go by position: the bottom face button is B and the right one is A. F3 shows
an overlay with FPS, frame time and memory, and F11 switches to borderless fullscreen and back. The first races can stutter while pipelines and
textures are prepared; **Bake textures** in the launcher decodes every texture ahead of time.
Integrated GPUs and handheld PCs get a unified-memory mode automatically. Saves and logs live in
`host\out\user\` (or `user-1.8.0\`). Settings: see [Every setting](#every-setting).

### LAN Play

Races between PCs on the same network use the game's own LAN Play. The main menu's **Wireless Play**
entry is already **LAN Play**: a game patch (`tools/patches.py`) switches it on at start whenever the PC has a network
adapter, which on the console takes holding L and R and clicking the left stick. That combo (Q+E and F on the
keyboard) still switches back to Wireless Play, which has no service behind it here. One player picks
**Create Room**, the others see that room and join it. Every PC needs its own copy of the game and this port, on the
same network (Wi-Fi or cable; the router needs no internet connection), and all of them the same game version (1.0.0
and 1.8.0 most likely don't see each other's rooms). The first time, Windows asks whether the game (`kart.exe` or
`kart-1.8.0.exe`) may use the network: allow it on private networks, or the PCs never see each other. With several
network adapters (a VPN, virtual machines), `HOST_LAN_IP` picks the right one. `HOST_PLAYER_NAME` sets the name the
other players see. Online Play stays off: there is no internet service behind it, so the game returns to the menu.
Two copies on one PC can't play together (they would share the game's ports, and each needs about 14 GB of RAM).

The optional VP9 Video Extensions (Microsoft Store) play the movies; without them they're skipped.
The launcher's **Set up** installs Python 3.13 and the .NET 10 SDK into `tools\python\` and
`tools\dotnet\` when the PC has none (`tools\bootstrap.ps1`, no admin rights; the launcher puts them
first on PATH for what it runs), downloads the C++ compiler (LLVM MinGW, into `tools\llvm-mingw\`; see
[Build options](#build-options)), installs the Python packages (`requirements.txt`) and builds the
shader translator from the NuGet packages in `tools\xlate\packages\`; the SPIR-V tools are in
`third_party\spirv\`. Keep the repository in a path without spaces.

Everything the tools make from your dump (`out\`, `gen\`, `build\`, `ver\`, `work\`) is derived from
the game: keep it to yourself.

## Building by hand

```
:: The C++ compiler (LLVM MinGW, into tools\llvm-mingw) and the Python packages
python tools\toolchain.py install
python -m pip install -r requirements.txt

:: Where your dump is (local.cfg stays on your machine; tools\paths.py lists every setting)
echo KART_GAME=D:\dumps\kart> local.cfg

:: Unpack, lift and compile the game's modules
python tools\nso.py D:\dumps\kart\exefs out
for %m in (rtld main subsdk0 sdk) do python tools\scan.py out %m && python tools\funcs.py out %m
python tools\build_all.py
python tools\gen_glue.py

:: Translate the shaders to SPIR-V, then the tables the renderer needs
python tools\nvn_shaders.py
python tools\nvn_refl.py
python tools\nvn_mesh.py --flat
python tools\nvn_spvfix.py --all
python tools\nvn_barfix.py

:: Build the host and play
host\build.bat
host\out\kart.exe
```

A `.nup` dump unpacks with `python tools\nup.py extract <file> <folder>`. For 1.8.0, set
`KART_VER=1.8.0` in the environment first (see [Game versions](#game-versions)).

## Linux

The same game and host, built with clang from LLVM 23.1.4 (the clang llvm-mingw has) against the system's C and C++
libraries. `./kart.sh` opens the launcher, the Windows one built for Linux (`launcher/launcher.cpp`: an SDL 3 window
drawn with cairo and Pango, the desktop's file dialogs; `tools/linux.py launcher` builds it into `launcher/out/launcher`
when the source is newer). Its steps, settings and buttons are the Windows ones; Set up runs `./kart.sh setup <part>`
for each part that is missing, and Build runs the same stages, linking with `build_host.py`. The launcher stamps the
stages it ran in `build/stages-linux/` (beside Windows' `build/stages/`, so a checkout both systems build keeps both); a
build from the command line leaves no stamps, so the launcher's next Build runs every stage once more (each redoes only
what changed). `./kart.sh <command>` does the steps on the command line:

| command | what |
|---------|------|
| `./kart.sh` | the launcher (`./kart.sh help` lists the commands) |
| `./kart.sh setup` | checks SDL 3, the Vulkan loader, libvpx and the C++ headers; puts LLVM's release in `tools/llvm/` (`tools/toolchain.py`; `LLVM=/usr` in `local.linux.cfg` takes the distribution's clang instead), the Python packages in a virtual environment `tools/venv/`, the .NET 10 SDK in `tools/dotnet/` (when the system has none) and the Vulkan SDK's SPIR-V tools in `tools/spirv/` (when some are missing: Fedora has no spirv-cross) |
| `./kart.sh game <folder>` | the dump, as `KART_GAME` (or `KART_GAME_<version>` with `--version`) in `local.linux.cfg` |
| `./kart.sh build` | the build steps of [Building by hand](#building-by-hand), each one that is not done yet (`--force`: all) |
| `./kart.sh play`, `./kart.sh bake` | the game (`host/out/kart`, `kart-<version>`), or the texture bake |

`--version 1.8.0` picks the other game version (else `LAUNCHER_VERSION`). Settings come from `local.linux.cfg`
first, then `local.cfg`, whose Windows paths (`C:\...`) Linux skips, so one checkout on a drive both systems mount
serves both: the dump paths go into `local.linux.cfg`, the `HOST_*` settings can stay shared. Saves and the texture
cache are shared too; the pipeline cache is not (`host/out/nvn_pipelines-linux.bin`, started from Windows' list of
pipelines to build ahead).

What differs from Windows:

* **Window, input, sound**: SDL 3 (Wayland or X11). Gamepads are every pad SDL knows, Xbox, PlayStation and Switch Pro
  Controllers included, with vibration; the PlayStation and Pro Controller drivers need read access to
  `/dev/hidraw*` (most distributions give it to the logged-in user through udev, else SDL falls back to the kernel's
  driver). Sound goes to PipeWire, PulseAudio or ALSA, whichever SDL finds.
* **The F3 overlay** is the window title (FPS, frame time, GPU wait, RAM): Wayland draws no window over another
  program's.
* **Movies**: libvpx, loaded when the first one starts (no headers needed to build); without it they show black.
* **Guest memory**: the pool is a memfd and the guest view `MAP_FIXED` mappings of it. Freed memory stays in RAM and
  is cleared on reuse; `HOST_DISCARD_FREED=1` gives it back with `fallocate(PUNCH_HOLE)` 3 s later. Two traps there: the
  GPU driver pins the pages an import found, so a hole punched under an import left the GPU reading the old pages
  (stretched models; `mem::pin`/`unpin` count imports per 64 KiB and hold those back), and the renderer still reads some
  memory just after the game frees it, so a hole punched at once read zero and hung the GPU in the title demo (Xid 109).
  It saved next to nothing anyway: the game keeps ~10 GB allocated and reuses what it frees. Imports are lean (at first
  use, in 64 MB windows), as an import takes ~4 ms on Linux: 6.5-7.5 GB of guest memory instead of 8.5-9+.
  With many mappings Linux's `vm.max_map_count` (65530 on some distributions) can run out:
  the error says so.
* **Timing**: the timer slack is 1 ns (`PR_SET_TIMERSLACK`), so short sleeps spin only their last 100 us
  (`HOST_SLEEP_SPIN_US`). A thread can lower its priority but not raise it without `CAP_SYS_NICE`: the renderer
  threads run at normal priority.
* **LAN Play**: the gateway pings use an unprivileged ICMP socket, which `net.ipv4.ping_group_range` must allow (it
  does on most distributions); without it LAN play still works.
* **Crash and stall reports** name functions from kart's own symbol table, and other libraries' exports; each thread
  reads its own stack when signalled. `HOST_WATCH_STEP` (debug registers) is Windows only.
* **A GPU hang** freezes the game on Linux instead of losing the device: NVIDIA's driver logs it (`journalctl -k`,
  `NVRM: Xid ... 109, CTX SWITCH TIMEOUT`) but never fails the wait, so the stall report shows the game in
  `nvnSyncWait` and the completion thread inside the driver.
* **One slow file**: 1.0.0's `gen/main_005.cpp` takes ~24 minutes to compile for the Linux target (71 s for Windows with
  the same clang): one 27,000-line function (`main_30ac60`) where LLVM's SLP vectorizer and register coalescer take
  that long. Only the first build pays; 1.8.0's files are quick.

`host/build.sh` builds the exe by hand (`tools/build_host.py`; objects in `host/out/obj-linux/`, module libraries in
`build/linux/`). The tools in [Building by hand](#building-by-hand) run the same with `python3` and `/` paths, and
`tools/venv/bin/python` has the packages.

## FPS chart data

| milestone | date | race fps (uncapped) |
|-----------|------|---------------------|
| Emulated GPU | Nov 9 | ~42 |
| Native NVN-on-Vulkan renderer | Nov 10 | ~82 |
| Model pools kept in video memory | Nov 10 | 92.6 |
| Descriptor sets kept across submissions | Nov 10 | 106.5 |
| Texture views kept across submissions | Nov 11 | ~110 |
| Vulkan work on a driver thread | Nov 11 | ~128 |
| Renderer skips redundant binds | Nov 11 | ~140 |

Scripted race on an RTX 3080 at handheld resolution, frame cap off (`HOST_NVN_PRESENT_HZ=0`). When
you play, the game runs at a steady 60.

## The binaries (measured, 1.0.0)

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

1.8.0 has five modules: `rtld`, `main`, `subsdk0` (the movie player, new), `subsdk1` (`nnEcli`) and `sdk`.

## From AArch64 to an exe

```
 rtld/main/subsdk*/sdk (AArch64) --lift.py--> <module>_*.cpp --clang--> build/mingw/<module>.a
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

Verification so far: 2,481 integer and 997 SIMD leaf functions identical to Unicorn on every case.

## The host (`host\`)

| file | role |
|------|------|
| `main.cpp` | loads the NSOs from `exefs\` at the console's addresses; runtime entry points; crash handler; stall watchdog |
| `memory.cpp` | the 512 GiB guest view over a physical pool, and the kernel's block map (`svcQueryMemory`) |
| `kernel.cpp` | system calls. Guest threads run under a per-core priority scheduler; mutexes, condition variables, the address arbiter and events share one kernel lock |
| `ipc.cpp` | HIPC with CMIF, TIPC, domains (shared by cloned sessions) and control commands |
| `services\` | sm, applet (appletOE; library applets such as the error viewer close as soon as they start), fs (a RomFS image built over the loose `romfs\` folder; saves in `user\save`), hid, audren, hwopus, time, acc, pctl, friend, chat, csrng, lm |
| `services\net.cpp` | LAN Play: nifm accepts local-network requests (the game's "no internet check" ones) when the PC has a network adapter, and bsd is nn::socket on Winsock (BSD layouts with Linux errno values, blocking calls emulated on non-blocking sockets; the raw ICMP socket Pia pings the gateway with is emulated with `IcmpSendEcho`) |
| `services\hid.cpp`, `gamepads.cpp` | player 1 as a Pro Controller (Handheld when the game doesn't take Fullkey) and players 2-4 as Pro Controllers, one gamepad each (a pad joins as the next player on its first button press; `ShowControllerSupport` answers with the players joined), written into nn::hid's shared memory (`hid_shared.h`) every 4 ms. Input comes from Xbox pads (XInput), DualShock 4, DualSense and the Switch Pro Controller (raw HID reports, over USB and Bluetooth) and the keyboard; the game's vibration (`SendVibrationValues`) goes back to the pads |
| `audio_core\`, `services\audio.cpp` | audren: Eden's audio renderer (voices, mixes, effects), adapted to the host and played through WASAPI. The output stream sets the pace |
| `services\opus.cpp` | `hwopus` on libopus (the movies' audio) |
| `services\gsv.cpp`, `vdec_mf.cpp` | the Switch 2 graphics server (`nx2:gsv`: syncpoints, native memory and images, the video decoder). Movies are VP9, decoded with Media Foundation |
| `syncpoints.cpp` | the graphics syncpoints the game waits on |
| `nvn\` | native NVN: devices, memory pools, textures, programs, command buffers (recorded as records in the game's command memory) and queues |
| `nvn\vk\` | the Vulkan renderer behind it: submissions replayed into Vulkan command buffers (on a driver thread), pipelines, textures (ASTC decoded on the CPU and encoded to BC3, cached on disk), sparse textures, present. On an integrated GPU it reads guest memory in place (unified-memory mode) |
| `video.cpp` | the game window, and the F3 performance overlay |
| `watch.cpp` | a debug write watch on guest memory |
| `localcfg.cpp` | reads `KART_GAME` and the `HOST_*` settings from `local.cfg` |
| `overrides.cpp`, `graphics.cpp`, `nvn.cpp` | an earlier native-NVN experiment, off unless `HOST_HLE_GRAPHICS=1` |

Logs go to `host\out\user\host.log`. `HOST_IPC_TRACE=1` logs every service request.

* **Scheduling as on the console**: one guest thread per core (cores 0-5, from the NPDM). The
  highest-priority ready thread runs, and a thread can run on any core in its affinity mask. When
  every thread ran in parallel, the game's job workers read data before it was written. Threads
  give up their core when they wait, sleep or make a service request. `svcSleepThread(0)` yields
  only to equal or higher priority. `HOST_NO_SCHED=1` turns the scheduler off.
* **Memory**: the physical pool is a pagefile-backed section of 24 GiB (`HOST_PHYS_GIB`; it asks
  for less if Windows refuses), mapped whole as the backing view. The guest view is a placeholder
  reservation holding one view of the section per mapping, so `svcMapMemory` and shared memory are
  real aliases of the same pages. Memory the game frees goes back to Windows.
* **Timing**: faults on pages another thread is remapping are retried, because Windows unmaps a
  whole view to unmap part of it. The guest counter runs at 31.25 MHz (the Switch 2 SDK's
  constant). The host timer resolution is raised to 0.5 ms. Guest sleeps under 4 ms wait on the
  clock for their last 700 us (`HOST_SLEEP_SPIN_US`; 0 means timer only), because the game's job
  loops sleep 20 us between polls.
* **Frames**: the game steps its simulation once per 1/60 s present interval, so present is fixed
  at 60 Hz (`HOST_NVN_PRESENT_HZ=0` uncaps it, for profiling only; that is how the FPS chart in the
  README is measured). Pipelines are built ahead of time from the shaders' pair tables and kept in
  `host\out\nvn_pipelines.bin`. Decoded textures are kept in `work\nvn_texcache`.

## Every setting

Set them in the environment, or as `HOST_*` lines in `local.cfg`.

| option | effect |
|--------|--------|
| `HOST_HANDHELD=0` | docked (higher resolution); handheld by default |
| `HOST_PRESENT_MODE=fifo\|immediate` | swapchain present mode; mailbox by default |
| `HOST_VULKAN_DEVICE=<index>` | which GPU; by default the discrete GPU with the most memory |
| `HOST_NVN_UMA=0\|1` | unified-memory mode off / forced; on by itself for an integrated GPU |
| `HOST_NVN_SUBGROUP32=0\|1` | pipelines asking for 32-wide subgroups off / forced; on by itself where the GPU could run another width (AMD, Intel): the translated shaders are 32-lane warp code |
| `HOST_FPS_MODE=dynamic\|60\|30\|game` | the game's present interval; dynamic keeps the game clock at real time when frames run late |
| `HOST_AUDIO_MUTE=1`, `HOST_AUDIO_LEVEL=1` | silence the output / log its peak level |
| `HOST_NO_OVERLAY=1` | no performance overlay |
| `HOST_PLAYERS=<1-4>` | at most that many local players (4); 1: every controller drives player 1 |
| `HOST_KEYBOARD_JOINS=1` | the keyboard is a player of its own, joining on its first key press like a controller |
| `HOST_LAN=0`, `HOST_LAN_IP=<address>` | LAN Play: no network (the menu shows Wireless Play again) / the network adapter with that address (by default the one the default route uses) |
| `HOST_PLAYER_NAME=<name>` | the account nickname other LAN players see ("Player" by default) |
| `HOST_USER_DIR=<dir>` | saves and logs in that folder instead of `host\out\user\` |
| `HOST_NVN_TEXCACHE=<dir>\|0` | decoded texture cache folder, or none; `HOST_NVN_TEXCACHE_MB` caps it (64 GiB) |
| `HOST_NVN_PIPELINE_CACHE=<file>` | where the pipeline cache lives (its `.keys` and `.primed` lists sit beside it) |
| `HOST_NVN_OWN_CPU_CACHED=1` | GPU-written CPU-cached pools may move to video memory once the CPU leaves them alone (faster, but a view's lighting can freeze: the game reads some without asking) |
| `HOST_TRANSCODE_THREADS=<n>` | threads for ASTC decoding / BC encoding (a quarter of the cores by default) |
| `HOST_RENDER_HIDDEN=1`, `HOST_INPUT_SCRIPT="<s>:<buttons>:<duration>;..."` | hidden test runs with scripted presses, e.g. `30:L+R:0.4;35:A:0.2` (buttons also `LS`, `RS`: stick clicks) |
| `HOST_INPUT_FILE=<file>` | lines appended to the file while the game runs are pressed from when they are read (`0:A:0.2`): driving a test run step by step |
| `HOST_NET_TRACE=1\|2\|3` | LAN diagnostics: nifm and socket calls; 2 also every IPC request from LAN Play's start and the results behind error messages; 3 also every packet |
| `HOST_STALL_DUMP_S=<s>` | after frames stop for this long, log every guest thread's state and stack (10 s by default) |
| `HOST_TIMELINE=<file>`, `HOST_SCHED_STATS=1`, `HOST_DUMP_AFTER=<s>` | diagnostics: timed events, guest core use, thread states |
| `HOST_NVN_CHECKPOINTS=1`, `HOST_VK_VALIDATION=1` | GPU diagnostics: the last draws before a device loss, Vulkan validation |

## Tools (`tools\`)

| tool | purpose |
|------|---------|
| `nso.py` | unpack NSOs, verify hashes; symbols, relocations and init arrays; ELF for Ghidra |
| `nup.py` | read a NUP container (`info`, `list`) and unpack it into a dump folder (`extract`) |
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

## Game versions

| version | modules (build id) |
|---------|--------------------|
| 1.0.0 | `rtld` 35b31ae2..., `main` 9e244438..., `subsdk0` (nnEcli) d39a7a3d..., `sdk` 90ebba18... |
| 1.8.0 | `rtld`, `main` d00e4f39..., `subsdk0` (movie, new), `subsdk1` (nnEcli), `sdk` (`out\<module>.json` after unpacking) |

The tools take the module list from the dump (`out\layout.json`), and the game patch
(`tools\patches.py`) finds its site by its code. `host\` binds to the sdk through its exports. So
another version needs the build steps run again, not code changes. `tools\hot_<module>_<build id>.txt`
lists the functions MSVC needs its huge-function optimization for (C4883; clang needs no list). For a
new version, build once with `BUILD_CC=cl BUILD_WARN_HUGE=1` and list the functions
`build\main_cl.log` names.

With `KART_VER=<version>` in the environment, the build steps write to `ver\<version>\{out,gen,build}`
and `work\nvn_shaders-<version>`, and `host\build.bat` links `host\out\kart-<version>.exe`. That exe
takes its dump from `KART_GAME_<version>` in `local.cfg`, keeps saves, caches and logs in
`host\out\user-<version>`, and pipelines in `host\out\nvn_pipelines-<version>.bin`. Leave `KART_VER`
out of `local.cfg`, because there it would switch every build.

## Build options

* `BUILD_MP=<n>` caps parallel compiles in every build step.
* `BUILD_CC` picks the C++ compiler. The default is `mingw`: clang from LLVM MinGW (llvm-mingw
  20261020, LLVM 23.1.4, which `tools\toolchain.py` downloads and checks by its SHA-256). It needs no
  Visual Studio, links everything statically and compiles the game several times faster than MSVC
  (1.8.0's main module: 5 minutes against 46 on 10 threads). Its objects go to `build\mingw\` and the
  host's to `host\out\obj-mingw\` (`tools\build_host.py`, which `host\build.bat` runs). `BUILD_ARCH`
  (default none: any x86-64 CPU) adds target flags such as `-march=native`.
* `BUILD_CC=cl` builds with Visual Studio 2026 (C++), `BUILD_CC=clang` with clang-cl (Visual Studio and
  LLVM). clang-cl's default `BUILD_ARCH=-march=native` is a few percent faster but the exe then only
  runs on CPUs with the same features; set `BUILD_ARCH` to something portable for an exe you take to
  another machine.

## Third-party code

GPL-3.0-or-later overall (`LICENSE`); `third_party\README.md` lists each project with its license:

* the audio renderer and the ASTC decoder are from Eden / yuzu (GPL), and `host\services\gsv.cpp`
  is ported from Eden-based code (GPL-3.0-or-later)
* the shader tools are from plumber-racer-2 and Ryujinx (MIT)
* Vulkan-Headers, VMA, LZ4, Zstandard, Opus and stb_dxt are in `third_party\`, and so are the
  Khronos SPIR-V tools the shader stage runs (SPIRV-Tools, SPIRV-Cross, glslang)
* `host\nvn\nvn.h` is MIT (h1k421), and `tools\texbake\stb_image_write.h` is public domain / MIT
* the launcher's fonts, Instrument Sans and DM Mono, are SIL OFL (licenses in `launcher\fonts\`)
