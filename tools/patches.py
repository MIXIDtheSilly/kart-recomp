"""Game patches the lifter applies: instructions replaced by C++ that reads a runtime switch.

find(name, mod) returns {pc: [C++ statements]}; lift_function emits those in place of the
instruction's own code. Sites are found by their code, so they follow the game version.
"""


def find(name, mod):
    if name == "main":
        return mkw_frame_mode(mod)
    return {}


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
