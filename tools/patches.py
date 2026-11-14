"""Game patches the lifter applies: instructions replaced by C++ that reads a runtime switch.

find(name, mod) returns {pc: [C++ statements]}; lift_function emits those in place of the
instruction's own code. Sites are found by their code, so they follow the game version.
"""


def find(name, mod):
    if name == "main":
        return {**mkw_frame_mode(mod), **lan_play_default(mod)}
    return {}


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
