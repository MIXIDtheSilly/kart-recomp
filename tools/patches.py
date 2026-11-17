"""Game patches the lifter applies: instructions replaced by C++ that reads a runtime switch.

find(name, mod) returns {pc: [C++ statements]}; lift_function emits those in place of the
instruction's own code. Sites are found by their code, so they follow the game version.
"""
import bisect


def find(name, mod):
    if name == "main":
        return {**mkw_frame_mode(mod), **lan_play_default(mod), **mkw_tick_counters(mod), **mkw_tick_adds(mod),
                **mkw_frame_counters(mod), **mkw_tick_seconds(mod), **mkw_tick_time(mod), **mkw_jobs(mod)}
    return {}


def mkw_tick_time(mod):
    """Before each phase of a frame the game fills the time its subsystems step by: a struct of the tick's length in
    seconds (1/60), a rate (1.0) and the frame's tick count, from constants. The two floats come from
    g_mkw_tick_dt / g_mkw_tick_rate (host/main.cpp, HOST_TICK_HZ) instead: the game's own values unless set. The first
    site (the frame's first phase) also calls mkw_tick_begin(), which advances the counters' step (mkw_tick_counters).

    Four sites (1.0.0 main+0x15a0a8, +0x15a104, +0x15a158, +0x45e800; 1.8.0 main+0x8d404, +0x8df54, +0x8e08c, +0x8e1cc):
        mov wR, #0x8889; movk wR, #0x3c88, lsl #16; ...; str wR, [xB]; mov wR, #0x3f800000; ...; str wR, [xB, #4]
    """
    lo, hi = mod.text
    out, sites, steps = {}, [], []
    for pc in range(lo, hi - 80, 4):
        w = mod.word(pc)
        if (w & 0xFFFFFFE0) != 0x52911120:  # movz wR, #0x8889
            continue
        r = w & 31
        win = [mod.word(pc + 4 * j) for j in range(1, 20)]
        if (0x72A79100 | r) not in win[:4]:  # movk wR, #0x3c88, lsl #16
            continue
        dt = next((j for j, x in enumerate(win) if (x & 0xFFFFFC1F) == (0xB9000000 | r)), None)     # str wR, [xB]
        one = next((j for j, x in enumerate(win) if x == (0x52A7F000 | r)), None)                   # mov wR, #1.0f
        rate = next((j for j, x in enumerate(win) if (x & 0xFFFFFC1F) == (0xB9000400 | r)), None)   # str wR, [xB, #4]
        if dt is None or one is None or rate is None or not dt < one < rate:
            continue
        for j, name, off in ((dt, "g_mkw_tick_dt", 0), (rate, "g_mkw_tick_rate", 4)):
            base = (win[j] >> 5) & 31
            begin = "extern void mkw_tick_begin(); mkw_tick_begin(); " if not sites and off == 0 else ""
            out[pc + 4 * (j + 1)] = [f"{{ {begin}extern uint32_t {name}; st<uint32_t>(c->x[{base}] + {off}ull, {name}); }}"]
        # The tick count: ldr wS, [xA, #0x50c]; ...; str wS, [xB, #8] -> mkw_tick_step(count) (HOST_TICK_STEP)
        load = next((j for j, x in enumerate(win) if j > rate and (x & 0xFFFFFC00) == 0xB9450C00), None)
        if load is not None:
            rs = win[load] & 31
            st = next((j for j, x in enumerate(win) if j > load and (x & 0xFFFFFC1F) == (0xB9000800 | rs)), None)
            if st is not None:
                b = (win[st] >> 5) & 31
                out[pc + 4 * (st + 1)] = [f"{{ extern uint32_t mkw_tick_step(uint32_t); st<uint32_t>(c->x[{b}] + 8ull, mkw_tick_step((uint32_t)c->x[{rs}])); }}"]
                steps.append(pc + 4 * (st + 1))
        sites.append(pc)
    if len(sites) != 4:
        print(f"  patches: MKW tick time sites not found ({len(sites)} matches)")
        return {}
    print("  patches: MKW tick time at " + ", ".join(f"{pc:#x}" for pc in sites) + "; count at " + ", ".join(f"{pc:#x}" for pc in steps))
    return out


def mkw_frame_counters(mod):
    """Float counters the game advances by a constant 1.0 a tick (the race clock among them, 1.8.0 main+0x405be8):
        ldr sA, [xB, #off]; fmov sC, #1.0; fadd sR, sA, sC; str sR, [xB, #off]
    Each add takes its step from a slot mkw_tick_fslot(pc, function) hands out the first time it runs (host/main.cpp):
    1.0, or the tick rate for the sites HOST_TICK_PCS / HOST_TICK_FUNCS name (or all of them: HOST_TICK_FLOATS=all).
    """
    starts = sorted(mod.starts)
    lo, hi = mod.text
    out, sites = {}, []
    for pc in range(lo + 44, hi - 24, 4):
        w = mod.word(pc)
        if (w & 0xFFE0FC00) != 0x1E202800:  # fadd sR, sN, sM
            continue
        rd, rn, rm = w & 31, (w >> 5) & 31, (w >> 16) & 31
        prev = [mod.word(pc - 4 * j) for j in range(1, 11)]
        one = next((x & 31 for x in prev if (x & 0xFFFFFFE0) == 0x1E2E1000 and (x & 31) in (rn, rm)), None)  # fmov sC, #1.0
        if one is None or rn == rm:
            continue
        other = rm if one == rn else rn
        load = None
        for x in prev:
            if (x & 0xFFC0001F) == (0xBD400000 | other):  # ldr sA, [xB, #off]
                load = x
                break
            if (x & 0xFF00001F) == (0x1E000000 | other):  # (sA computed since)
                break
        if load is None:
            continue
        store = (load & ~0x00400000 & 0xFFFFFFE0) | rd  # str sR, [xB, #off]
        if store not in [mod.word(pc + 4 * j) for j in range(1, 6)]:
            continue
        sites.append(pc)
        fn = starts[bisect.bisect_right(starts, pc) - 1]
        # The slot's default (1.0) does the game's own add, registers and all: the fmov above is not proven to reach this
        # add (another path, or the register written again in between), so only named sites trust it.
        out[pc] = [f"{{ extern const float* mkw_tick_fslot(uint32_t, uint32_t); static const float* const s = mkw_tick_fslot({pc:#x}u, {fn:#x}u); "
                   f"c->v[{rd}] = V128::f32(*s == 1.f ? fadd_(c->v[{rn}].f[0], c->v[{rm}].f[0]) : fadd_(c->v[{other}].f[0], *s)); }}"]
    print(f"  patches: MKW frame counters (float) at {len(sites)} sites")
    return out


def mkw_tick_seconds(mod):
    """A module that turns the tick struct's step count (mkw_tick_time) into time itself: frames = step and seconds =
    step / fps into a second time object (the one seconds-based clocks such as main+0x616a00's add from) and a global:
        ldr wN, [x1, #8]; ...; scvtf sD, wN; ...; fdiv sE, sD, sF; stp sD, sE, [xB, #0x18]
    (1.8.0 main+0x2a8e5ec). The step is scaled by the tick rate (g_mkw_tick_secf: the tick rate with HOST_TICK_HZ, 1 when the count itself counts 60ths, HOST_TICK_STEP).
    """
    lo, hi = mod.text
    out = {}
    for pc in range(lo + 16, hi - 32, 4):
        w = mod.word(pc)
        if (w & 0xFFFFFC00) != 0x1E220000:  # scvtf sD, wN
            continue
        d, n = w & 31, (w >> 5) & 31
        if not any(mod.word(pc - 4 * j) == (0xB9400820 | n) for j in range(1, 5)):  # ldr wN, [x1, #8]
            continue
        after = [mod.word(pc + 4 * j) for j in range(1, 13)]
        fdiv = next((x for x in after if (x & 0xFFE0FC00) == 0x1E201800 and (x >> 5) & 31 == d), None)  # fdiv sE, sD, sF
        stp = 0x2D000000 | (0x18 // 4) << 15 | (fdiv & 31) << 10 | d  # stp sD, sE, [xB, #0x18]
        if fdiv is None or not any((x & 0xFFFFFC1F) == stp for x in after):
            continue
        out[pc] = [f"{{ extern float g_mkw_tick_secf; c->v[{d}] = V128::f32((float)(int32_t)(uint32_t)c->x[{n}] * g_mkw_tick_secf); }}"]
    print("  patches: MKW tick seconds at " + (", ".join(f"{pc:#x}" for pc in out) or "no sites"))
    return out


def mkw_tick_adds(mod):
    """Float accumulators: x += y, read and stored back to the same field:
        ldr sA, [xB, #off]; ...; fadd sR, sA, sY (either order); then within 3 instructions str sR, [xB, #off]
    The addend is multiplied by a slot mkw_tick_aslot(pc, function) hands out the first time it runs (host/main.cpp):
    1.0 (the add is then exactly the game's), or the tick rate for the sites HOST_TICK_PCS / HOST_TICK_ADD_FUNCS name.
    """
    lo, hi = mod.text
    starts = sorted(mod.starts)
    out = {}
    for pc in range(lo + 32, hi - 16, 4):
        w = mod.word(pc)
        if (w & 0xFFE0FC00) != 0x1E202800:  # fadd sR, sN, sM
            continue
        rd, rn, rm = w & 31, (w >> 5) & 31, (w >> 16) & 31
        if rn == rm:
            continue
        st = next((y for y in (mod.word(pc + 4 * j) for j in range(1, 4)) if (y & 0xFFC0001F) == (0xBD000000 | rd)), None)
        if st is None:
            continue
        ld = 0xBD400000 | (st & 0x003FFFE0)  # ldr s?, [xB, #off] (the same field)
        src = next((y & 31 for y in (mod.word(pc - 4 * j) for j in range(1, 9)) if (y & 0xFFFFFFE0) == ld and (y & 31) in (rn, rm)), None)
        if src is None:
            continue
        other = rm if src == rn else rn
        fn = starts[bisect.bisect_right(starts, pc) - 1]
        out[pc] = [f"{{ extern const float* mkw_tick_aslot(uint32_t, uint32_t); static const float* const s = mkw_tick_aslot({pc:#x}u, {fn:#x}u); "
                   f"c->v[{rd}] = V128::f32(*s == 1.f ? fadd_(c->v[{rn}].f[0], c->v[{rm}].f[0]) : fadd_(c->v[{src}].f[0], c->v[{other}].f[0] * *s)); }}"]
    print(f"  patches: MKW tick adds (float x += y) at {len(out)} sites")
    return out


def mkw_jobs(mod):
    """Hooks into the game's job system for running chosen subsystems at half the tick rate (host/main.cpp, 120 Hz).

    The scheduler (1.8.0 main+0x8d520) starts a job list: mkw_job_list(list) sees it first:
        ldr w8, [x1, #0x50]; ldr w9, [x1, #0x54]; add x20, x1, #0x40          (x2 = the list)
    A worker runs a job (main+0x8ed78): mkw_job_obj(obj) may hand it a job that does nothing instead:
        ldr x0, [xN]; tbz w0, #0, ...; and x0, x0, #~1                          (xN = the job slot, +0 its object)
    The module phase (main+0x8d4bc), after giving every module the tick time: mkw_tick_modules(sys):
        ldr x0, [x19, #0x4c0]; ldr x1, [x19, #0x4c8]; ldr x2, [x19, #0x4d0]
    """
    lo, hi = mod.text
    sched, run, modules = [], [], []
    for pc in range(lo + 8, hi - 12, 4):
        w = mod.word(pc)
        if w == 0xB9405028 and mod.word(pc + 4) == 0xB9405429 and mod.word(pc + 8) == 0x91010034:
            sched.append(pc)
        elif w == 0x927FF800 and (mod.word(pc - 4) & 0xFFF8001F) == 0x36000000 and (mod.word(pc - 8) & 0xFFFFFC1F) == 0xF9400000:
            run.append(pc - 8)
        elif w == 0xF9426260 and mod.word(pc + 4) == 0xF9426661 and mod.word(pc + 8) == 0xF9426A62:
            modules.append(pc)
    if len(sched) != 1 or len(run) != 1 or len(modules) != 1:
        print(f"  patches: MKW job sites not found ({len(sched)}, {len(run)}, {len(modules)} matches)")
        return {}
    n = (mod.word(run[0]) >> 5) & 31
    # After the job returns (mkw_job_done): ldr x0, [xR, #0xe0] straight after a blr, in the same function.
    starts = sorted(mod.starts)
    i = bisect.bisect_right(starts, run[0])
    fn_end = starts[i] if i < len(starts) else hi
    done = [pc for pc in range(run[0], fn_end, 4)
            if (mod.word(pc) & 0xFFFFFC1F) == 0xF9407000 and (mod.word(pc - 4) & 0xFFFFFC1F) == 0xD63F0000]
    print(f"  patches: MKW jobs at {sched[0]:#x}, {run[0]:#x}, {modules[0]:#x}; done at " + ", ".join(f"{pc:#x}" for pc in done))
    out = {pc: [f"{{ extern void mkw_job_done(); mkw_job_done(); c->x[0] = ld<uint64_t>(c->x[{(mod.word(pc) >> 5) & 31}] + 0xe0ull); }}"]
           for pc in done}
    return {**out,
        sched[0]: ["{ extern void mkw_job_list(uint64_t); mkw_job_list(c->x[2]); c->x[8] = ld<uint32_t>(c->x[1] + 0x50ull); }"],
        run[0]: [f"{{ extern uint64_t mkw_job_obj(uint64_t); c->x[0] = mkw_job_obj(ld<uint64_t>(c->x[{n}])); }}"],
        modules[0]: ["{ extern void mkw_tick_modules(uint64_t); mkw_tick_modules(c->x[19]); c->x[0] = ld<uint64_t>(c->x[19] + 0x4c0ull); }"],
    }


def mkw_tick_counters(mod):
    """Integer counters: an add or sub of 1 whose result is stored (a frame counter or timer, or anything else that
    counts by one):
        add|sub wC, wA, #1; then within 3 instructions str|strh wC, [xB, #off]   (or 64-bit: add|sub xC, xA, #1; str xC)
    Each takes its 1 from a slot mkw_tick_slot(pc, function) hands out the first time it runs (host/main.cpp): a
    constant 1, or for the sites HOST_TICK_PCS / HOST_TICK_FUNCS name a step that is 0 or 1 a tick, so that they count
    in 60ths of a second at another tick rate (HOST_TICK_HZ). Unnamed sites behave as before.
    """
    lo, hi = mod.text
    starts = sorted(mod.starts)
    out = {}
    for pc in range(lo, hi - 16, 4):
        w = mod.word(pc)
        if (w & 0x3FFFFC00) != 0x11000400:  # add|sub wC|xC, wA|xA, #1
            continue
        c, a, sub, wide = w & 31, (w >> 5) & 31, bool(w & 0x40000000), bool(w & 0x80000000)
        if c == 31 or a == 31:
            continue
        for m in range(1, 4):
            y = mod.word(pc + 4 * m)
            if (y & 0xFFC0001F) in ((0xF9000000 | c,) if wide else (0xB9000000 | c, 0x79000000 | c)):  # str xC | str|strh wC, [xB, #off]
                fn = starts[bisect.bisect_right(starts, pc) - 1]
                value = f"c->x[{a}] {'-' if sub else '+'} (uint64_t)*s" if wide else f"(uint32_t)(c->x[{a}] {'-' if sub else '+'} *s)"
                out[pc] = [f"{{ extern const uint32_t* mkw_tick_slot(uint32_t, uint32_t); static const uint32_t* const s = mkw_tick_slot({pc:#x}u, {fn:#x}u); "
                           f"c->x[{c}] = {value}; }}"]
                break
            if (y & 31) == c and (y & 0x3B000000) != 0x39000000:  # (wC written again first; loads/stores aside)
                break
    print(f"  patches: MKW tick counters (int) at {len(out)} sites")
    return out


def lan_play_default(mod):
    """The main menu's Wireless Play entry turns into LAN Play when L+R are held and the left stick is clicked (the
    game's hidden LAN mode): a byte of the menu object (1.0.0 +0x77c, 1.8.0 +0x784) the combo flips and the menu's
    setup clears. Its setup stores g_lan_play_default (host/services/net.cpp: 1 when the PC has a network adapter)
    instead, so LAN Play is the entry from the start; the combo still switches back.

    The entry's label function picks message "2104" (Wireless Play) or "2106" (LAN Play) by its bool argument. The
    menu's setup calls it with that byte (1.0.0 main+0xbf1dc4, 1.8.0 main+0xbdfcb4):
        ldrb w1, [x19, #X]; ldr x0, [...]; bl <label function>
    and cleared it before (1.0.0 main+0xbf1d44, 1.8.0 main+0xbdfc2c): strb wzr, [x19, #X]; str wzr, [x19, #X-4].
    """
    import struct
    lo, hi = mod.text
    words = struct.unpack_from(f"<{(hi - lo) // 4}I", mod.img, lo - mod.base)

    def string(t):  # the address of a unique NUL-terminated string
        z = bytes(1)
        i = mod.img.find(z + t + z)
        return mod.base + i + 1 if i >= 0 and mod.img.find(z + t + z, i + 1) < 0 else None

    wireless, lan = string(b"2104"), string(b"2106")
    refs = {}  # string -> pcs of adrp+add pairs that make its address
    for i, w in enumerate(words):
        if (w & 0x9F000000) != 0x90000000:
            continue
        rd = w & 31
        imm = ((w >> 29) & 3) | (((w >> 5) & 0x7FFFF) << 2)
        page = ((lo + 4 * i) & ~0xFFF) + ((imm - (1 << 21) if imm & (1 << 20) else imm) << 12)
        w2 = words[i + 1] if i + 1 < len(words) else 0
        if (w2 & 0xFFC00000) == 0x91000000 and ((w2 >> 5) & 31) == rd:
            refs.setdefault(page + ((w2 >> 10) & 0xFFF), []).append(lo + 4 * i)
    funcs = sorted(mod.starts)

    def func_of(pc):
        import bisect
        return funcs[bisect.bisect_right(funcs, pc) - 1]

    label = {func_of(pc) for pc in refs.get(lan, [])} & {func_of(pc) for pc in refs.get(wireless, [])}
    hits = []
    for i, w in enumerate(words):
        if (w & 0xFC000000) != 0x94000000 or i < 2:
            continue
        imm = w & 0x3FFFFFF
        if lo + 4 * i + 4 * (imm - (1 << 26) if imm & (1 << 25) else imm) not in label:
            continue
        a = words[i - 2]
        if (a & 0xFFC003FF) != 0x39400261:  # ldrb w1, [x19, #X]
            continue
        x = (a >> 10) & 0xFFF
        if x % 4 or x < 4:
            continue
        clear, prev = 0x3900027F | (x << 10), 0xB900027F | (((x - 4) // 4) << 10)
        hits += [(lo + 4 * j, x) for j in range(max(0, i - 200), i) if words[j] == clear and words[j + 1] == prev]
    if len(hits) != 1:
        print(f"  patches: LAN Play default site not found ({len(hits)} matches)")
        return {}
    pc, x = hits[0]
    print(f"  patches: LAN Play default at {pc:#x} (menu +{x:#x})")
    return {pc: ["{ extern uint8_t g_lan_play_default;",
                 f"st<uint8_t>(c->x[19] + {x:#x}ull, g_lan_play_default); }}"]}


def mkw_frame_mode(mod):
    """The game's frame loop picks how many vblanks each frame lasts (its present interval) from a
    mode, [x19+0x210]: 1 = every vblank, 2 = every other one, 0 = from how late its frames run.
    The game clock advances by that interval each frame, so at a fixed 1 the game slows down with
    the frame rate. g_mkw_frame_mode (runtime/cpu.h) overrides the mode. Mode 0 normally times only
    the CPU part of the frame ([x19+0x128]), which stays short here where the GPU is the slow part,
    so the forced dynamic mode reads the whole frame's time ([x19+0xe8]) instead.

    Site (1.0.0 main+0x3a9ec8, 1.8.0 main+0x69cc54):
        cmp w9, w1; b.ne ...; ldr w8, [x19, #0x210]; cbnz w8, ...; ... mov w8, #0x42700000 (60.0f)
    then within 32 instructions: ldr xN, [x19, #0x128].
    """
    lo, hi = mod.text
    hits = []
    for pc in range(lo + 8, hi - 32, 4):
        if mod.word(pc) != 0xB9421268:  # ldr w8, [x19, #0x210]
            continue
        if (mod.word(pc - 8) != 0x6B01013F                        # cmp w9, w1
                or (mod.word(pc - 4) & 0xFF00001F) != 0x54000001  # b.ne
                or (mod.word(pc + 4) & 0xFF00001F) != 0x35000008):  # cbnz w8
            continue
        if any(mod.word(pc + 4 * j) == 0x52A84E08 for j in range(2, 8)):  # mov w8, #0x42700000
            hits.append(pc)
    if len(hits) != 1:
        print(f"  patches: MKW frame mode site not found ({len(hits)} matches)")
        return {}
    mode = hits[0]
    frame_time = next((mode + 4 * j for j in range(1, 32)
                       if (mod.word(mode + 4 * j) & 0xFFFFFFE0) == 0xF9409660), None)  # ldr xN, [x19, #0x128]
    if frame_time is None:
        print("  patches: MKW frame time load not found")
        return {}
    rt = mod.word(frame_time) & 0x1F
    print(f"  patches: MKW frame mode at {mode:#x}, frame time at {frame_time:#x}")
    return {
        mode: ["{ const int32_t m = g_mkw_frame_mode; g_mkw_frame_obj = c->x[19];",
               "c->x[8] = m >= 0 ? (uint64_t)(uint32_t)m : (uint64_t)ld<uint32_t>(c->x[19] + 0x210ull); }"],
        frame_time: [f"c->x[{rt}] = ld<uint64_t>(c->x[19] + (g_mkw_frame_mode == 0 ? 0xe8ull : 0x128ull));"],
    }
