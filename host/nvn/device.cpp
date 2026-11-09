// Native NVN: device objects and queries (nvnDevice*, nvnDeviceGetInteger).
#include <intrin.h>

#include "nvn_int.h"

namespace nvn {

bool g_log = std::getenv("HOST_NVN_LOG") != nullptr;
std::atomic<uint64_t> g_frame{0};

static std::atomic<uint64_t> g_serial{0};
uint64_t new_serial() { return ++g_serial; }

// ---------- Device ----------
NVN(nvnDeviceBuilderSetDefaults, void, NVNdeviceBuilder* b) { memset(b, 0, sizeof(dev_builder_t)); }
NVN(nvnDeviceBuilderSetFlags, void, NVNdeviceBuilder* b, int f) { ((dev_builder_t*)b)->flags = f; }
NVN(nvnDeviceInitialize, NVNboolean, NVNdevice* d, const NVNdeviceBuilder* b) {
    dev_t_* o = (dev_t_*)d; memset(o, 0, sizeof *o);
    o->magic = NVN_MAGIC('D', 'E', 'V', 0); o->flags = ((const dev_builder_t*)b)->flags;
    nvn_gob_switch2 = 1;   // the Switch 2's order inside a GOB, for the textures the game tiled itself (nvn_tex.h)
    o->origin = NVN_WINDOW_ORIGIN_MODE_LOWER_LEFT; rp_set_origin_lower_left(1);   // NVN's default; most games switch to upper-left right away
    rp_set_depth_zero(0);   // NVN's default depth mode: near = -w (GL)
    OLOG("DeviceInitialize %p flags=%#x", (void*)d, o->flags);
    return 1;
}
NVN(nvnDeviceFinalize, void, NVNdevice* d) { ((dev_t_*)d)->magic = 0; }
// The returned address is guest-callable (dispatch.cpp); a null device is fine (Super Smash Bros. fetches extensions with a null device).
NVN(nvnDeviceGetProcAddress, PFNNVNGENERICFUNCPTRPROC, const NVNdevice* d, const char* name) {
    (void)d; return reinterpret_cast<PFNNVNGENERICFUNCPTRPROC>(static_cast<uintptr_t>(lookup(name ? name : "")));
}

// Device properties measured on the Tegra X1 (nnheaders nvn_DeviceConstantsNX.h). The Switch 2 table below keeps their values for the
// items NVN2 reads from the GPU's own description at run time (DEVINFO_RT) below 0x60.
static const int g_devinfo[] = {
    55, 15, 14, 65536, 256, 8, 16, 4, 16, 32, 16, 4,
    4, 4, 16, 32, 32, 256, 256, 4, 8, 512, 256, 2,
    256, 4096, 131072, 16, 1536, 1024, 64, 1536, 65535, 65535, 65535, 8,
    1048576, 4096, 16, 4096, 1, 1, 1, 262144, 15, 2048, 1, 1,
    14, 5, 1, 8, 8, 4, 32, 128, 32, 128, 65536, 1,
    4, 16384, 134217728, 2048, 16384, 16384, 1, 1, 1, 1, 5, 1,
    1, 1, 1, 1, 1, 16, 1024, 32, 4096, 65536, 65536, 256,
    16384, 262144, 4096, 1, 4, 2, 0, 4096, 16384, 4096, 128, 4096,
};
// The Switch 2's values: NVN2 is compiled into the game's own sdk and answers from constants (Donkey Kong Bananza, sdk 20.5.19: the switch at sdk+0x5d9960 and its table lookup
// sdk+0x5c0a20, read statically; the switch's own constants, among them the versions, are read from the game's sdk at run time, sdk_nvn2_item, and this table has the newest seen, SDK 21.4.4's).
// The item numbers are NVN's: API 231.39 at 0 / 1 and GPU code 201.0-201.20 at 0x2E-0x31 (0xc9 = the version of its shaders' control sections).
// Ampere differs from the X1 in: 32 vertex bindings and attributes (the host keeps 16, all its tables hold), 1,024 compute threads, an unbounded dispatch width, 16 texture
// levels, 16 KB scratch granularity, 2 KB shader code padding, 32 separate sampler bindings, and items up to 0xA9. DEVINFO_RT: items NVN2 reads from the device's own GPU
// description at run time (scratch scale factors, L2 size, layers, texture sizes, linear stride alignment, pool page size, ...), not knowable from the binary: these keep the X1 table's
// value below 0x60, and above it answer 0 with a red line.
static constexpr int DEVINFO_RT = -1;
static const int g_devinfo2[] = {
    231, 39, 14, 65536, 256, 8, 16, 4, 16, 32, 16, 4,
    4, 4, 16, 32, 32, 256, 256, 4, 8, 512, 256, DEVINFO_RT,
    DEVINFO_RT, 4096, 16384, 16, 1024, 1024, 64, 1024, 2147483647, 65535, 65535, 8,
    1048576, 4096, 16, 4096, 1, 1, 1, DEVINFO_RT, 16, DEVINFO_RT, 201, 201,
    20, 0, 1, 8, 8, 4, 32, DEVINFO_RT, DEVINFO_RT, 128, DEVINFO_RT, 1,
    4, DEVINFO_RT, DEVINFO_RT, DEVINFO_RT, DEVINFO_RT, DEVINFO_RT, 1, 1, 1, 1, 5, 1,
    1, 1, 1, 1, 1, 16, 2048, 32, 4096, 65536, 65536, 256,
    16384, 262144, 4096, 1, 4, 2, 0, 4096, 16384, 4096, 128, 32,
    65535, 1, DEVINFO_RT, 256, 16, 65535, 65535, 256, 1, 16, 16, 16,
    1, 1, 1, 256, 0, 16, 8, 16, 32, 31, 536870912, 16777216,
    16777216, 1073741824, 32, 1, 64, DEVINFO_RT, 1, 2147483647, 32, 32, 4194303, 512,
    256, 32, 16383, DEVINFO_RT, 8, 0, 0, 4, 256, 16, 0, 1,
    31, 0, 64, 1, 58368, 24576, 0, 256, 262144, DEVINFO_RT, DEVINFO_RT, 0,
    201, 14, 201, 20, 163840, 8, 4, 256, 256, 4, 65536, 201,
    5, 1,
};
static constexpr unsigned N1 = sizeof g_devinfo / sizeof *g_devinfo, N2 = sizeof g_devinfo2 / sizeof *g_devinfo2;

// NVN2's own nvnDeviceGetInteger is a switch inside the game's sdk: it loads w0 with the API major first, then jumps through a byte table:
// mov w0, #231 / adr / adrp + add (the table) / ldrb / add, lsl #2 / br, with cmp wN, #<last item> before it. A case that is a constant is mov w0, #v; b.
// Scans the sdk module's original code (mapped in guest memory at its base) for that shape and reads the constant of `item`. false: that item's case reads
// something else (most go through the driver's table lookup or read the GPU's description), or there is no such switch.
namespace {
struct Nvn2Switch { bool found = false; u64 table = 0, cases = 0; unsigned last = 0; };
Nvn2Switch scan_sdk() {
    Nvn2Switch r;
    const u64 base = module_base("sdk");
    if (base) {
        // The module's text is the run of executable pages at its base; its read-only data follows (state Code), then its data; all up to the first page of another kind.
        // Should the original bytes be mapped without execute permission, the whole Code run is searched (the instruction pattern does not occur in data).
        u64 text_end = base, end = base;
        for (mem::Info i = mem::query(base); i.perm & mem::X; i = mem::query(text_end)) text_end = i.addr + i.size;
        for (mem::Info i = mem::query(base); i.state == mem::Code || i.state == mem::CodeData; i = mem::query(end)) end = i.addr + i.size;
        if (text_end <= base) for (mem::Info i = mem::query(base); i.state == mem::Code; i = mem::query(text_end ? text_end : base)) text_end = i.addr + i.size;
        if (end < text_end) end = text_end;
        auto word = [](u64 a) { return ld<uint32_t>(a); };
        for (u64 p = base + 8 * 4; p + 9 * 4 < text_end && !r.found; p += 4) {
            if (word(p) != 0x52801CE0) continue;   // mov w0, #231
            u64 tb = 0, adr = 0; int lim = -1, ldrb = 0, br = 0;
            for (u64 q = p - 8 * 4; q <= p + 8 * 4; q += 4) {
                const uint32_t w = word(q);
                if ((w & 0xFFC0001F) == 0x7100001F) lim = (w >> 10) & 0xFFF;   // cmp wN, #imm
                else if ((w & 0x9F000000) == 0x90000000 && (word(q + 4) & 0xFFC00000) == 0x91000000) {   // adrp + add
                    int64_t pg = (int64_t)(((w >> 3) & 0x1FFFFC) | ((w >> 29) & 3)); if (pg & 0x100000) pg -= 0x200000;
                    tb = (q & ~(u64)0xFFF) + (u64)(pg * 4096) + ((word(q + 4) >> 10) & 0xFFF);
                } else if ((w & 0x9F000000) == 0x10000000) {   // adr
                    int64_t off = (int64_t)(((w >> 3) & 0x1FFFFC) | ((w >> 29) & 3)); if (off & 0x100000) off -= 0x200000;
                    adr = q + (u64)off;
                } else if ((w & 0xFFE0FC00) == 0x38606800) ldrb = 1;   // ldrb wK, [xT, xI]
                else if ((w & 0xFFFFFC1F) == 0xD61F0000) br = 1;       // br
            }
            if (!tb || !adr || lim <= 1 || !ldrb || !br) continue;
            if (tb < base || tb + lim >= end) continue;   // the byte table lives in the module (its read-only data)
            r.table = tb; r.cases = adr; r.last = (unsigned)lim; r.found = true;
        }
    }
    if (!r.found) LOG("[RED] nvn: sdk: NVN2's nvnDeviceGetInteger switch not found; the device answers the newest versions known");
    else LOG("nvn: sdk: NVN2's nvnDeviceGetInteger switch found (items 0-%#x)", r.last);
    return r;
}
}  // namespace
bool sdk_nvn2_item(unsigned item, int* out) {
    static const Nvn2Switch sw = scan_sdk();   // thread-safe one-time scan
    if (!sw.found || item > sw.last) return false;
    const u64 c = sw.cases + (u64)ld<uint8_t>(sw.table + item) * 4;
    const uint32_t c0 = ld<uint32_t>(c), c1 = ld<uint32_t>(c + 4);
    if ((c0 & 0xFFE0001F) != 0x52800000 || (c1 & 0xFC000000) != 0x14000000) return false;   // mov w0, #imm16; b
    *out = (int)((c0 >> 5) & 0xFFFF);
    return true;
}

// The NVN API version the game's sdk reports: games check it and create no device on a mismatch (Splatoon Raiders requires minor >= 39, Donkey Kong Bananza minor >= 30).
// API major 231 for NVN2; the minor moves with the SDK and is read from the sdk's own switch.
static void nvn_api(int api[2]) {
    api[0] = 231;
    if (!sdk_nvn2_item(1, &api[1])) api[1] = g_devinfo2[1];
    LOG("nvn: device API %d.%d (Switch 2)", api[0], api[1]);
}
NVN(nvnDeviceGetInteger, void, const NVNdevice* d, NVNdeviceInfo what, int* out) {
    (void)d;
    const unsigned w = (unsigned)what;
    if (w <= 1) {
        static const std::pair<int, int> api = [] {
            int a[2]; nvn_api(a);
            if (const char* e = std::getenv("HOST_NVN_API")) sscanf(e, "%d.%d", &a[0], &a[1]);   // override: major.minor
            return std::pair<int, int>{a[0], a[1]};
        }();
        *out = w ? api.second : api.first;
    } else if (w >= N2) {
        LOG("[RED] nvn: nvnDeviceGetInteger unknown item %#x, returning 0", w); *out = 0;
    } else if (g_devinfo2[w] == DEVINFO_RT) {
        // NVN2 reads these from the GPU at run time. Below 0x60 the X1's value is a workable stand-in; above it nothing is known.
        if (w < N1) *out = g_devinfo[w];
        else { LOG("[RED] nvn: nvnDeviceGetInteger item %#x: NVN2 reads it from the GPU at run time, not known here; returning 0", w); *out = 0; }
    } else {
        *out = g_devinfo2[w];
        sdk_nvn2_item(w, out);   // the sdk's own constant where its switch has one: the GPU code versions move with the SDK
    }
    OLOG("DeviceGetInteger %#x -> %d", w, *out);
}
NVN(nvnDeviceGetImageHandle, NVNimageHandle, const NVNdevice* d, int id) { (void)d; return (NVNimageHandle)id; }
// Separate texture / sampler handles: value = descriptor index (the sampler index goes in bit 20 and above, consistent with the bit layout of combined handles)
NVN(nvnDeviceGetSeparateTextureHandle, NVNseparateTextureHandle, const NVNdevice* d, int id) { (void)d; return NVNseparateTextureHandle{(uint64_t)id}; }
NVN(nvnDeviceGetSeparateSamplerHandle, NVNseparateSamplerHandle, const NVNdevice* d, int id) { (void)d; return NVNseparateSamplerHandle{(uint64_t)id << 20}; }
NVN(nvnDeviceInstallDebugCallback, void, NVNdevice* d, const PFNNVNDEBUGCALLBACKPROC cb, void* data, uint32_t enable) {
    dev_t_* o = (dev_t_*)d; o->dbg_cb = B(enable) ? (uint64_t)(uintptr_t)cb : 0; o->dbg_data = ga(data);
}
NVN(nvnDeviceSetWindowOriginMode, void, NVNdevice* d, NVNwindowOriginMode m) {
    ((dev_t_*)d)->origin = m; rp_set_origin_lower_left(m == NVN_WINDOW_ORIGIN_MODE_LOWER_LEFT);
    LOG("nvn: window origin %s", m ? "upper left (as D3D / Vulkan)" : "lower left (GL)");
}
NVN(nvnDeviceSetDepthMode, void, NVNdevice* d, NVNdepthMode m) {
    ((dev_t_*)d)->depth_mode = m; rp_set_depth_zero(m != 0);
    LOG("nvn: depth mode %s", m ? "near=0 (as D3D / Vulkan)" : "near=-w (GL)");
}
// Pokemon Sword sets neither mode and reads both, keeping NVN's defaults (lower-left origin, near = -w)
NVN(nvnDeviceGetWindowOriginMode, NVNwindowOriginMode, const NVNdevice* d) { return (NVNwindowOriginMode)((const dev_t_*)d)->origin; }
NVN(nvnDeviceGetDepthMode, NVNdepthMode, const NVNdevice* d) { return (NVNdepthMode)((const dev_t_*)d)->depth_mode; }
NVN(nvnDeviceFinalizeCommandHandle, void, NVNdevice* d, NVNcommandHandle h) { (void)d; (void)h; }

// Device: combined texture handle = texture id | sampler id << 20 (same encoding as the separate handles)
NVN(nvnDeviceGetTextureHandle, NVNtextureHandle, const NVNdevice* d, int tex, int samp) { (void)d; return (NVNtextureHandle)((uint64_t)(uint32_t)tex | (uint64_t)(uint32_t)samp << 20); }
// Handle for texel fetches only (Monster Hunter Rise): a fetch reads the texture descriptor and no sampler, so the handle is the texture ID with sampler ID 0
NVN(nvnDeviceGetTexelFetchHandle, NVNtextureHandle, const NVNdevice* d, int tex) { (void)d; return (NVNtextureHandle)(uint64_t)(uint32_t)tex; }
// The 614.4 MHz report -> ns on the same guest clock as GetCurrentTimestampInNanoseconds.
NVN(nvnDeviceGetTimestampInNanoseconds, uint64_t, const NVNdevice* d, const NVNcounterData* c) {
    (void)d; uint64_t hi; const uint64_t lo = _umul128(c->timestamp, 625, &hi); uint64_t rem; return _udiv128(hi, lo, 384, &rem);
}
// Current GPU time (ns): the same clock as the counter timestamps -- the guest clock
NVN(nvnDeviceGetCurrentTimestampInNanoseconds, uint64_t, const NVNdevice* d) { (void)d; return guest_ns(); }
NVN(nvnDeviceRegisterFastClearColor, NVNboolean, NVNdevice* d, const float* c, NVNformat f) { (void)d; (void)c; (void)f; return 1; }
// Same as the color one: a fast clear value is only a hint to the Tegra hardware (Tears of the Kingdom registers its depth clear at startup)
NVN(nvnDeviceRegisterFastClearDepth, NVNboolean, NVNdevice* d, float v) { (void)d; (void)v; return 1; }
// The size of the driver's cache for shaders it compiles from intermediate code; the host compiles nothing at run time
NVN(nvnDeviceSetIntermediateShaderCache, void, NVNdevice* d, int n) { (void)d; (void)n; }
// The driver's external graphics debugger switch (Nsight / NVN debugger attach): Super Smash Bros. Ultimate fetches it with nvnDeviceGetProcAddress(NULL, ...) and calls it without a null check
NVN_NOPROTO(nvnSetExternalDebuggerEnabledNVX, void, NVNboolean enable) { (void)enable; }

// ---------- GPU clock ----------
// NVN counter timestamps are 614.4 MHz ticks (384 ticks per 625 ns) of the guest clock; the guest's system counter runs at 31.25 MHz (32 ns per tick).
uint64_t guest_ns() { return guest_ticks() * (1000000000ull / GuestTickHz); }
uint64_t gpu_ticks() { const uint64_t ns = guest_ns(); uint64_t hi; const uint64_t lo = _umul128(ns, 384, &hi); uint64_t rem; return _udiv128(hi, lo, 625, &rem); }

}  // namespace nvn
