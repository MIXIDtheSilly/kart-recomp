#!/bin/sh
# Builds out/kart (out/kart-<KART_VER> with KART_VER set): the host runtime, the recompiled modules and the native NVN
# renderer, with clang from LLVM (tools/build_host.py; tools/paths.py says where everything is).
# BUILD_MP=<n>: at most n compiler processes (default: one per core).
cd "$(dirname "$0")" && exec python3 ../tools/build_host.py "$@"
