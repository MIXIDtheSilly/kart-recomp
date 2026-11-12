"""Generate gen/aot_modules.cpp: the module descriptors that tie the recompiled libraries together.

Usage: python gen_glue.py [module ...]   (default: every module nso.py unpacked, out/layout.json)
"""
import json
import sys
from pathlib import Path

import paths

ROOT = paths.art()  # holds out/, gen/ and build/ (ver/<KART_VER>/ for another game version)


def main():
    mods = sys.argv[1:] or [m["name"] for m in json.loads((ROOT / "out" / "layout.json").read_text())]
    lines = ['#include "aot_modules.h"', ""]
    rows = []
    for m in mods:
        info = json.loads((ROOT / "out" / f"{m}.json").read_text())
        lines += [f"extern const FnEntry {m}_table[];", f"extern const size_t {m}_table_size;", f"uint64_t g_base_{m};", ""]
        rows.append(f'    {{"{m}", "{info["module_id"]}", &g_base_{m}, {m}_table, {info["image_size"]:#x}ull}},')
    lines.append("extern const AotModuleDesc g_aot_modules[] = {")
    lines += rows
    lines.append("};")
    lines.append(f"extern const size_t g_aot_module_count = {len(mods)};")
    lines.append("")
    # Table sizes live in the module libraries, so they are looked up rather than stored above.
    lines.append("size_t aot_table_size(size_t i) {")
    lines.append("    switch (i) {")
    for i, m in enumerate(mods):
        lines.append(f"    case {i}: return {m}_table_size;")
    lines.append("    }")
    lines.append("    return 0;")
    lines.append("}")
    out = ROOT / "gen" / "aot_modules.cpp"
    out.write_text("\n".join(lines) + "\n")
    # The module libraries host/build.bat links (1.8.0 has one module more than 1.0.0).
    (ROOT / "gen" / "aot_libs.rsp").write_text("".join(f'"{ROOT / "build" / m}.lib"\n' for m in mods))
    print(f"wrote {out} ({len(mods)} modules)")


if __name__ == "__main__":
    main()
