# Third-party code

Copied unchanged from upstream, each folder with its license. `host\build.bat` compiles the C sources
once into `host\out\obj\third_party\` (with Visual Studio; delete that folder after changing a file
here). `tools\build_host.py` (LLVM MinGW) recompiles whatever changed by itself.

The C++ compiler is not in the repository: `tools\toolchain.py` downloads llvm-mingw by Martin Storsjö
(https://github.com/mstorsjo/llvm-mingw: LLVM under Apache-2.0 with LLVM exceptions, MinGW-w64 under
its own permissive licenses, listed in its `LICENSE.TXT`) into `tools\llvm-mingw\`. The exes it links
carry libc++, libunwind (Apache-2.0 with LLVM exceptions) and winpthreads (MIT-style).

| folder | what | version | license |
|--------|------|---------|---------|
| `vulkan\` | Vulkan-Headers: the C headers for Windows (`vulkan.h`, `vk_platform.h`, `vulkan_core.h`, `vulkan_win32.h`, `vk_video\`) | 1.4.345 | Apache-2.0 or MIT (`LICENSE.md`, `LICENSES\`) |
| `vma\` | Vulkan Memory Allocator (`vk_mem_alloc.h`) | 3.3.0 | MIT (`LICENSE.txt`) |
| `lz4\` | LZ4 block format (`lz4.c`): the renderer's decoded-texture cache on disk | 1.10.0 (ebb370ca83) | BSD-2-Clause (`LICENSE`) |
| `zstd\` | Zstandard, the decoder only (`common\`, `decompress\`): the game's `.zs` files, read by the texture bake | 1.5.8 (33618c8) | BSD-3-Clause (`LICENSE`) |
| `opus\` | Opus: the float build in plain C, without the SIMD paths, for the `hwopus` service (movie audio) | a3f0ec02b3 | BSD-3-Clause (`COPYING`) |
| `spirv\bin\` | Khronos SPIR-V tools, prebuilt for Windows x64 (from the Vulkan SDK): `spirv-val`, `spirv-opt`, `spirv-dis` (SPIRV-Tools v2026.4), `spirv-cross`, `glslangValidator` (16.6.0). The shader tools run them; they need only the VC++ runtime | SDK 1.4.363.0 | Apache-2.0 (`LICENSE-SPIRV-Tools`, `LICENSE-SPIRV-Cross`); glslang: BSD-3-Clause and others (`LICENSE-glslang.txt`) |
| `stb\` | stb_dxt by Fabian Giesen: BC1/BC3 block encoding. This is Eden's copy, split into a header and a source file | 1.12 | MIT or public domain |

## Code adapted into `host\`

This code was taken from other projects and changed to fit the host. Each file keeps its original
copyright and license header.

| files | from | license |
|-------|------|---------|
| `host\audio_core\` | Eden's audio renderer (from yuzu) | GPL-3.0-or-later / GPL-2.0-or-later |
| `host\nvn\vk\astc.cpp` | Eden's ASTC decoder (from yuzu, based on FasTC by the University of North Carolina) | GPL-3.0-or-later, Apache-2.0 |
| `host\nvn\vk\bcn.cpp` | yuzu's BC1/BC3 encoder | GPL-2.0-or-later |
| `host\cityhash.*` | CityHash64 by Geoff Pike and Jyrki Alakuijala (Google), from Eden's copy | MIT |
| `host\services\hid_shared.h` | layout of nn::hid's shared memory, after yuzu's `hid_core` | GPL-3.0-or-later |
| `tools\shader_split\` | plumber-racer-2's shader splitter | MIT |
| `tools\xlate\` | dmsh_xlate and a subset of the Ryujinx shader compiler | MIT |

`tools\xlate\packages\` holds the NuGet packages the translator builds with, unchanged from nuget.org (each `.nupkg`
carries its license): Gommon 2.8.1.2 (MIT), Humanizer.Core 3.0.10 (MIT), Microsoft.IO.RecyclableMemoryStream 3.0.1 (MIT),
MsgPack.Cli 1.0.1 (Apache-2.0), System.CodeDom 4.4.0 (MIT).
