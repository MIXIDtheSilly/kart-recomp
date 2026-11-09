// Native NVN: NVN2 (Switch 2) interfaces: DLSS, push constants, command buffer modes. (Mesh draws, ShadingRateImage and the other NVN2 commands that map onto
// ordinary records are in cmdbuf.cpp; NVN2 device values in device.cpp; program builders in program.cpp.)
#include "nvn_int.h"

namespace nvn {

// NVN2's Z-cull compression hint for 32-bit float depth: the host's depth buffers keep their own compression, so there is nothing to set (Fire Emblem Fortune's Weave)
NVN_NOPROTO(nvnCommandBufferSetZCullZF32CompressionEnable, void, NVNcommandBuffer *c, uint32_t on) { (void)c; (void)on; }
// ---------- NVN2's DLSS ----------
// A Switch 2 game calls DLSS through NVN itself (Fire Emblem Fortune's Weave's gfxa::TaskDLSS: main+0x1913b48 makes the network, main+0xb70f00 evaluates it): a neural network on NVIDIA's GPU, with
// no counterpart here. The renderer can stand in a temporal upscaler (the record is OP_DLSS), which wants the same things: colour, depth, motion vectors at the input's size, the jitter and a history reset.
// The builder and the parameters are packed in place: the game's builder is 0x108 bytes of its stack frame, and the parameters hold no more than the driver's own, which keeps the same fields for
// its getters. The network's texture IDs and storage are the driver's own needs, which the scaler does not have, so they are taken and dropped
typedef struct { int32_t w, h, method, preset; uint32_t flags; } dlss_builder_t;
typedef dlss_builder_t dlss_net_t;   // the network keeps what it was built with: the game gives it 0x800 bytes
typedef struct { const NVNtexture *tex[DLSS_TEXTURES]; int32_t iw, ih, base[DLSS_TEXTURES][2]; float mv[2], jitter[2], pre_exposure; uint32_t reset; } dlss_params_t;
static inline int dlss_ceil_div(int v, int d) { return (v + d - 1) / d; }
// The output is this many times the input, by the builder's method (read from the game's choice at main+0x1913c24: output / input of 2 and under is 0, 3 is 1, 1 is 2)
static int dlss_factor(int method) { return method == 1 ? 3 : method == 2 ? 1 : 2; }
NVN_NOPROTO(nvnDlssNetworkBuilderSetDefaults, void, dlss_builder_t *b) { memset(b, 0, sizeof *b); }
NVN_NOPROTO(nvnDlssNetworkBuilderSetDevice, void, dlss_builder_t *b, NVNdevice *d) { (void)b; (void)d; }
NVN_NOPROTO(nvnDlssNetworkBuilderSetOutputResolution, void, dlss_builder_t *b, int w, int h) { b->w = w; b->h = h; }
NVN_NOPROTO(nvnDlssNetworkBuilderSetMethod, void, dlss_builder_t *b, int m) { b->method = m; }
NVN_NOPROTO(nvnDlssNetworkBuilderSetPreset, void, dlss_builder_t *b, int p) { b->preset = p; }
NVN_NOPROTO(nvnDlssNetworkBuilderSetFlags, void, dlss_builder_t *b, uint32_t f) { b->flags = f; }
NVN_NOPROTO(nvnDlssNetworkBuilderSetMaxEvaluationEntries, void, dlss_builder_t *b, int n) { (void)b; (void)n; }
NVN_NOPROTO(nvnDlssNetworkBuilderSetTextureIDs, void, dlss_builder_t *b, const void *pool, const int *ids, int n) { (void)b; (void)pool; (void)ids; (void)n; }
NVN_NOPROTO(nvnDlssNetworkBuilderSetStorage, void, dlss_builder_t *b, void *mem, size_t n) { (void)b; (void)mem; (void)n; }
NVN_NOPROTO(nvnDlssNetworkBuilderGetStorageSize, size_t, const dlss_builder_t *b) { (void)b; return 0x1000; }   // the scaler keeps its memory on the renderer's side; a page, for a game that allocates what it is told
// The argument order is NGX's NGX_DLSS_GET_OPTIMAL_SETTINGS: optimal, largest and smallest input, sharpness (the game stores them so at main+0x1913cfc). One input size per method: no dynamic range
NVN_NOPROTO(nvnDlssNetworkBuilderGetOptimalSettings, NVNboolean, const dlss_builder_t *b, int *ow, int *oh, int *maxw, int *maxh, int *minw, int *minh, float *sharp) {
    int f = dlss_factor(b->method), w = dlss_ceil_div(b->w, f), h = dlss_ceil_div(b->h, f);
    if (ow) *ow = w;
    if (oh) *oh = h;
    if (maxw) *maxw = w;
    if (maxh) *maxh = h;
    if (minw) *minw = w;
    if (minh) *minh = h;
    if (sharp) *sharp = 0;
    return 1;
}
NVN_NOPROTO(nvnDlssNetworkInitialize, NVNboolean, dlss_net_t *n, const dlss_builder_t *b) {
    *n = *b;
    LOG("nvn: DLSS network %dx%d, method %d (input 1/%d), preset %d, flags %#x: the renderer's temporal upscaler stands in", b->w, b->h, b->method, dlss_factor(b->method), b->preset, b->flags);
    return 1;
}
NVN_NOPROTO(nvnDlssNetworkFinalize, void, dlss_net_t *n) { (void)n; }
NVN_NOPROTO(nvnDlssParametersSetDefaults, void, dlss_params_t *p) { memset(p, 0, sizeof *p); p->pre_exposure = 1; p->mv[0] = p->mv[1] = 1; }
static void dlss_set_tex(dlss_params_t *p, int k, const NVNtexture *t, const NVNtextureView *v) {
    p->tex[k] = t;
    if (v) ONCE_RED("nvn: DLSS parameter %d given a texture view: the whole texture is used", k);
}
NVN_NOPROTO(nvnDlssParametersSetInputColor, void, dlss_params_t *p, const NVNtexture *t, const NVNtextureView *v) { dlss_set_tex(p, DLSS_COLOR, t, v); }
NVN_NOPROTO(nvnDlssParametersSetDepth, void, dlss_params_t *p, const NVNtexture *t, const NVNtextureView *v) { dlss_set_tex(p, DLSS_DEPTH, t, v); }
NVN_NOPROTO(nvnDlssParametersSetMotionVectors, void, dlss_params_t *p, const NVNtexture *t, const NVNtextureView *v) { dlss_set_tex(p, DLSS_MOTION, t, v); }
NVN_NOPROTO(nvnDlssParametersSetOutputColor, void, dlss_params_t *p, const NVNtexture *t, const NVNtextureView *v) { dlss_set_tex(p, DLSS_OUTPUT, t, v); }
NVN_NOPROTO(nvnDlssParametersSetBiasCurrentColorMask, void, dlss_params_t *p, const NVNtexture *t, const NVNtextureView *v) { dlss_set_tex(p, DLSS_BIAS, t, v); }
NVN_NOPROTO(nvnDlssParametersSetExposureTexture, void, dlss_params_t *p, const NVNtexture *t, const NVNtextureView *v) { dlss_set_tex(p, DLSS_EXPOSURE, t, v); }
NVN_NOPROTO(nvnDlssParametersSetInputSubrectangleSize, void, dlss_params_t *p, int w, int h) { p->iw = w; p->ih = h; }
NVN_NOPROTO(nvnDlssParametersSetInputColorSubrectangleBase, void, dlss_params_t *p, int x, int y) { p->base[DLSS_COLOR][0] = x; p->base[DLSS_COLOR][1] = y; }
NVN_NOPROTO(nvnDlssParametersSetDepthSubrectangleBase, void, dlss_params_t *p, int x, int y) { p->base[DLSS_DEPTH][0] = x; p->base[DLSS_DEPTH][1] = y; }
NVN_NOPROTO(nvnDlssParametersSetMotionVectorSubrectangleBase, void, dlss_params_t *p, int x, int y) { p->base[DLSS_MOTION][0] = x; p->base[DLSS_MOTION][1] = y; }
NVN_NOPROTO(nvnDlssParametersSetOutputSubrectangleBase, void, dlss_params_t *p, int x, int y) { p->base[DLSS_OUTPUT][0] = x; p->base[DLSS_OUTPUT][1] = y; }
NVN_NOPROTO(nvnDlssParametersSetBiasCurrentColorSubrectangleBase, void, dlss_params_t *p, int x, int y) { p->base[DLSS_BIAS][0] = x; p->base[DLSS_BIAS][1] = y; }
NVN_NOPROTO(nvnDlssParametersSetMotionVectorScale, void, dlss_params_t *p, float x, float y) { p->mv[0] = x; p->mv[1] = y; }
NVN_NOPROTO(nvnDlssParametersSetJitterOffset, void, dlss_params_t *p, float x, float y) { p->jitter[0] = x; p->jitter[1] = y; }
NVN_NOPROTO(nvnDlssParametersSetPreExposure, void, dlss_params_t *p, float e) { p->pre_exposure = e; }
NVN_NOPROTO(nvnDlssParametersSetHistoryReset, void, dlss_params_t *p, uint32_t on) { p->reset = on & 0xff; }   // an NVNboolean: only its low 8 bits are the guest's
NVN_NOPROTO(nvnDlssParametersSetEvaluationIndex, void, dlss_params_t *p, int i) { (void)p; (void)i; }   // which of the network's frames in flight: the replay runs them in order
NVN_NOPROTO(nvnCommandBufferEvaluateDlss, void, NVNcommandBuffer *c, const dlss_net_t *n, const dlss_params_t *p) {
    p_dlss *o = REC_SNAP(c, OP_DLSS, p_dlss, DLSS_TEXTURES);
    for (int k = 0; k < DLSS_TEXTURES; k++) {
        o->tex[k] = tex_snap(o, SNAP_OFF(p_dlss), k, p->tex[k], call_site());
        memcpy(o->base[k], p->base[k], sizeof o->base[k]);
    }
    o->net = (uint64_t)(uintptr_t)n;   // the network object (host pointer) o->iw = p->iw; o->ih = p->ih; o->ow = n->w; o->oh = n->h; o->flags = n->flags;
    memcpy(o->mv, p->mv, sizeof o->mv); memcpy(o->jitter, p->jitter, sizeof o->jitter); o->pre_exposure = p->pre_exposure; o->reset = p->reset != 0;
}
// NVN2 push constants: (command buffer, a word seen 0, byte offset, size, data). The bytes form a constant block of the graphics stages or of compute, which their shaders read
// as c[24] (DMSH_PUSH_BANK; NVN2 compute's own c0 layout starts with them, which the translator maps there). Donkey Kong Bananza's upscaler pushes (1 / w, 1 / h, w, h) of its
// target for its graphics passes, and two integer rectangles (source, destination) for its compute network
static void push_const(NVNcommandBuffer *c, int compute, uint64_t w, uint64_t off, uint64_t n, const void *data) {
    if (w) { static int once; if (!once++) LOG("[RED] nvn: push constants with the word before the offset %#llx: its meaning is not known", (unsigned long long)w); }
    if (off + n > NVN_PUSH_SIZE || !data) { static int k; if (k++ < 8) LOG("[RED] nvn: push constants at %#llx + %#llx: beyond the host's %#x-byte block, dropped", (unsigned long long)off, (unsigned long long)n, NVN_PUSH_SIZE); return; }
    p_push_const *r = RECN(c, OP_PUSH_CONST, p_push_const, data, n); r->compute = (uint16_t)compute; r->offset = (uint16_t)off; r->size = (uint32_t)n; memcpy(r->data, data, n);
}
NVN_NOPROTO(nvnCommandBufferPushGraphicsConstants, void, NVNcommandBuffer *c, uint64_t w, uint64_t off, uint64_t n, const void *data) { push_const(c, 0, w, off, n, data); }
NVN_NOPROTO(nvnCommandBufferPushComputeConstants, void, NVNcommandBuffer *c, uint64_t w, uint64_t off, uint64_t n, const void *data) { push_const(c, 1, w, off, n, data); }
// NVN2 command buffer modes Donkey Kong Bananza sets on every command buffer it begins: they tune how the GPU's front end runs the commands (whether compute may overlap the next
// work, how geometry is processed, how commands are recorded), not what they produce. The host records and replays every command in order, so none changes its result.
// Logged once each with the first value; open: what the values select on the console
NVN_NOPROTO(nvnCommandBufferSetRecordingMode, void, NVNcommandBuffer *c, int mode) { (void)c; ONCE_LOG("nvnCommandBufferSetRecordingMode %d: the host records every command buffer alike", mode); }
NVN_NOPROTO(nvnCommandBufferSetGeometryProcessingMode, void, NVNcommandBuffer *c, int mode) { (void)c; ONCE_LOG("nvnCommandBufferSetGeometryProcessingMode %d: no host effect", mode); }
NVN_NOPROTO(nvnCommandBufferSetComputeOverlapMode, void, NVNcommandBuffer *c, int mode) { (void)c; ONCE_LOG("nvnCommandBufferSetComputeOverlapMode %d: the host runs compute in submission order", mode); }

}  // namespace nvn
