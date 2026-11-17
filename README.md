# kart-recomp

A native Windows and Linux port of the Switch 2 kart game. Its ARM64 code is recompiled ahead of time
into C++, and a small host stands in for the console, with the game's graphics running on Vulkan.
Versions 1.0.0 and 1.8.0 work.

![Race FPS over time: from about 42 with an emulated GPU to about 140 with the native renderer](docs/fps.svg?v=2)

> [!IMPORTANT]
> No game code or data is included. Don't use this with pirated copies: own the game and a
> Switch 2, and dump the game from your own console.

## Run it

You need Windows 10/11 and a Vulkan 1.3 GPU. Nothing else to install: the launcher's Set up puts
what is missing into the repository (Python and the .NET 10 SDK when the PC has none, the C++
compiler LLVM MinGW, the Python packages).

```
launcher\build.bat
launcher\out\launcher.exe
```

The launcher takes your from your dump (or `.nup`). The first build takes about
half an hour on an 8-core CPU, with a progress bar. Controls, settings and how it all works: [docs/how-it-works.md](docs/how-it-works.md).

### Linux

You need a Vulkan 1.3 GPU and driver, SDL 3 (Fedora `SDL3`, Debian/Ubuntu `libsdl3-0`), the C++
standard library headers (`g++`) and Python 3.10 or later (with `venv`); libvpx plays the movies.

```
launcher/out/launcher
```

is the same launcher as on Windows: open it from your file manager or a terminal. A release has it built; in a checkout
`./kart.sh` builds it the first time and opens it. Its Set up puts the rest into the repository (clang from LLVM, the
Python packages, the .NET SDK, the SPIR-V tools), without root. The same steps without a window:

```
./kart.sh setup
./kart.sh game ~/dumps/kart          # the folder holding exefs/ and romfs/ (--version 1.8.0 for that dump)
./kart.sh build
./kart.sh play
```

Details and the differences from Windows: [docs/how-it-works.md](docs/how-it-works.md#linux).

Updating: unzip the new release into a new folder and click **Bring over an older version…** on
the launcher's first step. It moves your build, tools, saves and settings over from the old folder,
and Build then only redoes what changed. Scripted: `launcher.exe --bring-over <old folder> --build --quit` (`--setup` before `--build` sets up first).

## License

GPL-3.0-or-later. Third-party code and its licenses are listed in `third_party\README.md`.
