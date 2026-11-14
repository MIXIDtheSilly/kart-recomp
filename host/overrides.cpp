// Native replacements for SDK functions (called through main's imports).
#include <cstdlib>
#include <cstring>
#include <unordered_map>

#include <algorithm>

#include "graphics.h"
#include "host.h"
#include "nvn/nvn_host.h"
#include "services/gamepads.h"

namespace {

// Exported symbols of a loaded (not yet relocated) module, from its dynamic section.
std::unordered_map<std::string, u64> read_exports(u64 base) {
    std::unordered_map<std::string, u64> out;
    const u64 mod0 = base + ld<u32>(base + 4);
    if (ld<u32>(mod0) != 0x30444F4D) return out;  // "MOD0"
    const u64 dynamic = mod0 + static_cast<s32>(ld<u32>(mod0 + 4));
    u64 symtab = 0, strtab = 0, hash = 0;
    for (u64 d = dynamic;; d += 16) {
        const s64 tag = static_cast<s64>(ld<u64>(d));
        const u64 val = ld<u64>(d + 8);
        if (tag == 0) break;
        if (tag == 4) hash = base + val;
        if (tag == 5) strtab = base + val;
        if (tag == 6) symtab = base + val;
    }
    if (!symtab || !strtab) return out;
    const u64 count = hash ? ld<u32>(hash + 4) : (strtab > symtab ? (strtab - symtab) / 24 : 0);
    for (u64 i = 1; i < count; i++) {
        const u64 sym = symtab + i * 24;
        const u32 name = ld<u32>(sym);
        const u16 shndx = ld<u16>(sym + 6);
        const u64 value = ld<u64>(sym + 8);
        if (shndx == 0 || value == 0) continue;  // imports
        out.emplace(read_cstr(strtab + name, 512), base + value);
    }
    return out;
}

std::unordered_map<std::string, u64>& sdk_exports() {
    static auto e = read_exports(module_base("sdk"));
    return e;
}

void override_sdk(const char* name, GuestFn fn) {
    auto it = sdk_exports().find(name);
    if (it == sdk_exports().end()) fatal("sdk export %s not found", name);
    host_override(it->second, fn);
}

// nn::hid::ShowControllerSupport(ControllerSupportResultInfo*, const ControllerSupportArg&): the console's "connect
// controllers" applet, through the library applet services (not here). Its answer instead: the players with a pad
// (services/gamepads.h: a pad joins on a button press; player 1 is there with the keyboard alone), at most the
// arg's player_count_max (byte 1), and npad No1 as the one selected. ResultInfo: s8 player_count, u32 selected npad
// id at +4 (main+0x292e99c reads both: a count of 0 or a failed result is "cancelled", id 0x20 is handheld).
void show_controller_support(Ctx* c) {
    const u64 info = c->x[0], arg = c->x[1];
    const int min = ld<s8>(arg), max = ld<s8>(arg + 1);
    int players = gamepads::connected_players();
    if (max > 0) players = std::min(players, max);
    if (players < min)
        LOG("hid: controller applet wants %d-%d players, %d have joined (a pad joins on a button press)", min, max,
            players);
    else
        LOG("hid: controller applet (%d-%d players): %d", min, max, players);
    st<s8>(info, static_cast<s8>(players));
    st<u32>(info + 4, 0);  // No1
    c->x[0] = 0;
}

// nn::gs::sdk::BootGraphicsSystem(void*, size_t): boots the console's graphics servers
// (gsv, nvsrv, syncpt, vi). The native renderer needs none of them.
void boot_graphics_system(Ctx* c) {
    LOG("nn::gs::sdk::BootGraphicsSystem(%#llx, %#llx): skipped, graphics are native", c->x[0], c->x[1]);
}

}  // namespace

u64 sdk_export(const char* name) {
    auto it = sdk_exports().find(name);
    return it == sdk_exports().end() ? 0 : it->second;
}

void host_install_overrides() {
    LOG("sdk exports: %zu", sdk_exports().size());
    override_sdk("_ZN2nn3hid21ShowControllerSupportEPNS0_27ControllerSupportResultInfoERKNS0_20ControllerSupportArgE",
                 show_controller_support);
    // HOST_HLE_GRAPHICS=1: the old native nv::/nn::vi/NVN stand-ins (no GPU).
    if (const char* v = std::getenv("HOST_HLE_GRAPHICS"); v && v[0] == '1') {
        override_sdk("_ZN2nn2gs3sdk18BootGraphicsSystemEPvm", boot_graphics_system);
        gfx::install();
        return;
    }
    // NVN is the host's (nvn/, on Vulkan): the SDK's NVN driver never runs; nn::vi gets the
    // stand-ins. The console graphics boot and nv:: still run: nn::codec::video (the race intro
    // movies) decodes through their gsv session.
    gfx::install_display();
    nvn::install();
}
