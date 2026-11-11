"""Differential test: recompiled functions vs. Unicorn executing the original AArch64 code.

Usage: python difftest.py <out dir> <module> <work dir> [--funcs N] [--cases K] [--seed S]

Picks fully supported leaf functions, runs each on K random register states in Unicorn,
recompiles the same functions with MSVC into runtime/test_harness.cpp, replays the cases
natively and compares registers, flags and every changed 8-byte word of memory.
"""
import argparse
import collections
import os
import random
import struct
import subprocess
import sys
from pathlib import Path

from unicorn import Uc, UcError, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_MEM_WRITE, UC_PROT_ALL
from unicorn.arm64_const import *  # noqa: F403

TOOLS = Path(__file__).parent
ROOT = TOOLS.parent
sys.path.insert(0, str(TOOLS))
import a64  # noqa: E402
import lift  # noqa: E402
import paths  # noqa: E402

SCRATCH, SCRATCH_SIZE = 0x100000000, 0x10000
STACK, STACK_SIZE = 0x110000000, 0x10000
SENTINEL = 0x1000
MAX_STEPS = 200000
VCVARS = paths.get("VCVARS")

CASE_IN = struct.Struct("<QQ31QQQQ512s")
CASE_OUT = struct.Struct("<Q31QQQ512sQ")


def splitmix_fill(size, seed):
    out = bytearray(size)
    s = seed
    m = (1 << 64) - 1
    for i in range(0, size, 8):
        s = (s + 0x9E3779B97F4A7C15) & m
        z = s
        z = ((z ^ (z >> 30)) * 0xBF58476D1CE4E5B9) & m
        z = ((z ^ (z >> 27)) * 0x94D049BB133111EB) & m
        struct.pack_into("<Q", out, i, z ^ (z >> 31))
    return bytes(out)


def is_simd(w):
    """FP/SIMD data processing or a SIMD structure load/store."""
    return (w >> 25) & 0b0111 == 0b0111 or ((w >> 26) & 1 and (w >> 24) & 0x3F in (0b001100, 0b001101))


def pick_leaf_functions(mod, clean, n, rng, max_size=1600, simd_only=False):
    pool = list(clean)
    rng.shuffle(pool)
    sizes = {f["addr"]: f["size"] for f in mod.funcs}
    in_region = {f["addr"] for f in mod.funcs if "region" in f}  # lifted with other entries, not leaves
    chosen = []
    for off in pool:
        if sizes[off] > max_size or off in in_region:
            continue
        start = mod.base + off
        if simd_only and not any(is_simd(mod.word(pc)) for pc in range(start, start + sizes[off], 4)):
            continue
        ok = True
        for pc in range(start, start + sizes[off], 4):
            ins = a64.decode(mod.word(pc), pc)
            if ins.kind in ("call", "icall", "ijump", "trap") or (ins.kind in ("b", "bcond") and not start <= ins.target < start + sizes[off]):
                ok = False
                break
            if any("guest_svc" in line for line in ins.code):
                ok = False
                break
        last = a64.decode(mod.word(start + sizes[off] - 4), start + sizes[off] - 4)
        if ok and last.kind in ("ret", "b"):
            chosen.append(off)
            if len(chosen) >= n:
                break
    return chosen


def random_reg(rng):
    r = rng.random()
    if r < 0.4:
        return SCRATCH + rng.randrange(0x1000, 0xF000, 16)
    if r < 0.7:
        return rng.randrange(0, 17)
    if r < 0.85:
        return rng.getrandbits(32)
    return rng.getrandbits(64)


def make_case(fn, rng):
    x = [random_reg(rng) for _ in range(31)]
    x[30] = SENTINEL
    v = bytes(rng.getrandbits(8) for _ in range(512))
    nzcv = rng.randrange(16) << 28
    return {"fn": fn, "seed": rng.getrandbits(63), "x": x, "sp": STACK + 0xE000, "nzcv": nzcv,
            "tpidr": SCRATCH + 0x800, "v": v}


class Emu:
    def __init__(self, mod):
        self.mod = mod
        self.uc = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
        try:
            self.uc.ctl_set_cpu_model(UC_CPU_ARM64_MAX)
        except (UcError, NameError):
            pass
        self.img_size = (len(mod.img) + 0xFFFF) & ~0xFFFF
        self.uc.mem_map(mod.load, self.img_size, UC_PROT_ALL)
        self.uc.mem_write(mod.load, mod.img)
        self.uc.mem_map(SCRATCH, SCRATCH_SIZE, UC_PROT_ALL)
        self.uc.mem_map(STACK, STACK_SIZE, UC_PROT_ALL)
        self.written = set()
        self.uc.hook_add(UC_HOOK_MEM_WRITE, self._on_write)

    def _on_write(self, uc, access, addr, size, value, user):
        for a in range(addr & ~7, addr + size, 8):
            self.written.add(a)

    def run(self, case):
        uc = self.uc
        scratch = splitmix_fill(SCRATCH_SIZE, case["seed"])
        stack = splitmix_fill(STACK_SIZE, case["seed"] ^ 0x5555)
        uc.mem_write(SCRATCH, scratch)
        uc.mem_write(STACK, stack)
        for i in range(29):
            uc.reg_write(UC_ARM64_REG_X0 + i, case["x"][i])
        uc.reg_write(UC_ARM64_REG_X29, case["x"][29])
        uc.reg_write(UC_ARM64_REG_X30, case["x"][30])
        uc.reg_write(UC_ARM64_REG_SP, case["sp"])
        uc.reg_write(UC_ARM64_REG_NZCV, case["nzcv"])
        uc.reg_write(UC_ARM64_REG_TPIDR_EL0, case["tpidr"])
        uc.reg_write(UC_ARM64_REG_FPCR, 0)
        for i in range(32):
            uc.reg_write(UC_ARM64_REG_Q0 + i, int.from_bytes(case["v"][i * 16:i * 16 + 16], "little"))
        self.written = set()
        ok = True
        try:
            uc.emu_start(case["fn"], SENTINEL, count=MAX_STEPS)
            ok = uc.reg_read(UC_ARM64_REG_PC) == SENTINEL
        except UcError:
            ok = False

        result = None
        if ok:
            x = [uc.reg_read(UC_ARM64_REG_X0 + i) for i in range(29)]
            x += [uc.reg_read(UC_ARM64_REG_X29), uc.reg_read(UC_ARM64_REG_X30)]
            v = b"".join(uc.reg_read(UC_ARM64_REG_Q0 + i).to_bytes(16, "little") for i in range(32))
            diffs = {}
            for a in sorted(self.written):
                now = struct.unpack("<Q", uc.mem_read(a, 8))[0]
                if SCRATCH <= a < SCRATCH + SCRATCH_SIZE:
                    before = struct.unpack_from("<Q", scratch, a - SCRATCH)[0]
                elif STACK <= a < STACK + STACK_SIZE:
                    before = struct.unpack_from("<Q", stack, a - STACK)[0]
                else:
                    before = struct.unpack_from("<Q", self.mod.img, a - self.mod.load)[0]
                if now != before:
                    diffs[a] = now
            result = {"x": x, "sp": uc.reg_read(UC_ARM64_REG_SP), "nzcv": uc.reg_read(UC_ARM64_REG_NZCV) & 0xF0000000,
                      "v": v, "diffs": diffs}
        # Restore image bytes written outside scratch/stack so cases stay independent.
        for a in self.written:
            if self.mod.load <= a < self.mod.load + len(self.mod.img):
                uc.mem_write(a, self.mod.img[a - self.mod.load:a - self.mod.load + 8])
        return result


def build_harness(work, gen_files):
    exe = work / "test_harness.exe"
    srcs = " ".join(f'"{p}"' for p in [ROOT / "runtime" / "test_harness.cpp", *gen_files])
    bat = work / "build.bat"
    # DIFFTEST_CC replaces the compiler, e.g. "clang-cl.exe -ffp-contract=off" (clang-cl ignores /MP).
    cc = os.environ.get("DIFFTEST_CC", "cl")
    bat.write_text(f'@call "{VCVARS}" >nul\r\n{cc} /nologo /O2 /std:c++20 /EHa /MP /bigobj /w /I"{ROOT / "runtime"}" '
                   f'/Fo"{work}\\\\" /Fe"{exe}" {srcs}\r\n')
    r = subprocess.run(["cmd", "/c", str(bat)], capture_output=True, text=True, cwd=work)
    if r.returncode != 0:
        print(r.stdout[-6000:], r.stderr[-3000:])
        sys.exit("harness build failed")
    return exe


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("out")
    ap.add_argument("module")
    ap.add_argument("work")
    ap.add_argument("--funcs", type=int, default=500)
    ap.add_argument("--cases", type=int, default=6)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--simd", action="store_true", help="only pick functions that use FP/SIMD instructions")
    a = ap.parse_args()

    rng = random.Random(a.seed)
    out, work = Path(a.out), Path(a.work)
    work.mkdir(parents=True, exist_ok=True)
    gen = work / "gen"
    gen.mkdir(exist_ok=True)
    for old in gen.glob("*.cpp"):
        old.unlink()

    mod = lift.Module(out, a.module)
    mod.load = mod.info["load_base"]  # where Unicorn and the harness place the image; code is lifted relative
    clean = __import__("json").loads((ROOT / "gen" / f"{a.module}_clean.json").read_text())
    funcs = pick_leaf_functions(mod, clean, a.funcs, rng, simd_only=a.simd)
    print(f"testing {len(funcs)} leaf functions x {a.cases} cases")

    subprocess.run([sys.executable, str(TOOLS / "lift.py"), str(out), a.module, str(gen), "--asm", "--chunk", "200",
                    "--only", ",".join(f"{f:x}" for f in funcs)], check=True, capture_output=True)
    exe = build_harness(work, sorted(gen.glob("*.cpp")))

    emu = Emu(mod)
    cases, expected = [], []
    skipped = 0
    for f in funcs:
        for _ in range(a.cases):
            case = make_case(mod.load + f, rng)
            res = emu.run(case)
            if res is None:
                skipped += 1
                continue
            cases.append(case)
            expected.append(res)
    print(f"unicorn: {len(cases)} cases completed, {skipped} skipped (faults or step limit)")

    with open(work / "cases.bin", "wb") as fp:
        for c in cases:
            fp.write(CASE_IN.pack(c["fn"], c["seed"], *c["x"], c["sp"], c["nzcv"], c["tpidr"], c["v"]))
    rw_off = 0  # random pointers can land anywhere in the image, so restore and diff all of it
    r = subprocess.run([str(exe), str(out / f"{a.module}.img"), hex(mod.load), hex(rw_off),
                        str(work / "cases.bin"), str(work / "results.bin")], timeout=600)
    if r.returncode:
        sys.exit(f"harness exited with {r.returncode:#x}")

    data = (work / "results.bin").read_bytes()
    pos = 0
    failures = collections.defaultdict(list)
    for case, exp in zip(cases, expected):
        status, *rest = CASE_OUT.unpack_from(data, pos)
        pos += CASE_OUT.size
        x, sp, nzcv, v, ndiff = rest[:31], rest[31], rest[32], rest[33], rest[34]
        diffs = dict(struct.unpack_from(f"<{ndiff * 2}Q", data, pos)[i:i + 2] for i in range(0, ndiff * 2, 2))
        pos += ndiff * 16
        problems = []
        if status:
            problems.append(f"native status {status:#x}")
        else:
            for i in range(31):
                if x[i] != exp["x"][i]:
                    problems.append(f"x{i}: native {x[i]:#x} unicorn {exp['x'][i]:#x}")
            if sp != exp["sp"]:
                problems.append(f"sp: native {sp:#x} unicorn {exp['sp']:#x}")
            if nzcv != exp["nzcv"]:
                problems.append(f"nzcv: native {nzcv >> 28:04b} unicorn {exp['nzcv'] >> 28:04b}")
            for i in range(32):
                if v[i * 16:i * 16 + 16] != exp["v"][i * 16:i * 16 + 16]:
                    problems.append(f"v{i}: native {v[i * 16:i * 16 + 16].hex()} unicorn {exp['v'][i * 16:i * 16 + 16].hex()}")
            for addr in sorted(set(diffs) | set(exp["diffs"])):
                if diffs.get(addr) != exp["diffs"].get(addr):
                    problems.append(f"mem {addr:#x}: native {diffs.get(addr, 'unchanged')} unicorn {exp['diffs'].get(addr, 'unchanged')}")
        if problems:
            failures[case["fn"]].append(problems)

    passed = len({c["fn"] for c in cases}) - len(failures)
    print(f"functions passing every case: {passed}/{len({c['fn'] for c in cases})}")
    for fn, runs in list(failures.items())[:12]:
        print(f"  FAIL f_{fn:x} ({len(runs)} cases): {'; '.join(runs[0][:4])}")
    report = {f"{fn:x}": runs for fn, runs in failures.items()}
    (work / "failures.json").write_text(__import__("json").dumps(report, indent=1))


if __name__ == "__main__":
    main()
