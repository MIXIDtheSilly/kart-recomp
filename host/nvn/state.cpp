// Native NVN: state objects (blend, color, channel mask, multisample, polygon, depth-stencil, vertex attrib / stream). They are small structs the game copies by
// value, so their fields are packed in place with bit-fields (nvn_obj.h) and recording a bind copies only the state's own bytes. Defaults follow the NVN / GL defaults.
#include "nvn_int.h"

namespace nvn {

#define ST(T, x) ((T*)(x))
NVN(nvnBlendStateSetDefaults, void, NVNblendState *s) { *ST(blend_st, s) = blend_st{0, NVN_BLEND_FUNC_ONE, NVN_BLEND_FUNC_ZERO, NVN_BLEND_FUNC_ONE, NVN_BLEND_FUNC_ZERO, NVN_BLEND_EQUATION_ADD, NVN_BLEND_EQUATION_ADD}; }
NVN(nvnBlendStateSetBlendTarget, void, NVNblendState *s, int t) { ST(blend_st, s)->target = t; }
NVN(nvnBlendStateSetBlendFunc, void, NVNblendState *s, NVNblendFunc sr, NVNblendFunc dr, NVNblendFunc sa, NVNblendFunc da) {
    blend_st *o = ST(blend_st, s); o->src_rgb = sr; o->dst_rgb = dr; o->src_a = sa; o->dst_a = da;
}
NVN(nvnBlendStateSetBlendEquation, void, NVNblendState *s, NVNblendEquation er, NVNblendEquation ea) { ST(blend_st, s)->eq_rgb = er; ST(blend_st, s)->eq_a = ea; }
// Advanced blend modes (NV_blend_equation_advanced): kept in the state, and while one is set the hardware ignores the blend functions. The replay blends the Porter-Duff modes with
// the fixed-function factors they amount to (the renderer's advanced-blend mapping); multiply, screen, overlay and the rest have no fixed-function blend state. Mode 0 (none, which Luigi's Mansion 3 sets
// on every blend state) is the default; one earlier game sets SRC_OVER (8) on one state in its levels
NVN(nvnBlendStateSetAdvancedMode, void, NVNblendState *s, NVNblendAdvancedMode mode) { ST(blend_st, s)->adv = (unsigned)mode <= NVN_BLEND_ADVANCED_MODE_HSL_LUMINOSITY ? mode : 0; }
NVN(nvnBlendStateSetAdvancedPremultipliedSrc, void, NVNblendState *s, uint32_t pm) { ST(blend_st, s)->adv_npm = !B(pm); }
NVN(nvnBlendStateSetAdvancedOverlap, void, NVNblendState *s, NVNblendAdvancedOverlap o) { ST(blend_st, s)->adv_ovl = (unsigned)o & 3; }
NVN(nvnColorStateSetDefaults, void, NVNcolorState *s) { *ST(color_st, s) = color_st{0, NVN_LOGIC_OP_COPY, NVN_ALPHA_FUNC_ALWAYS}; }
NVN(nvnColorStateSetBlendEnable, void, NVNcolorState *s, int i, uint32_t en) {
    color_st *o = ST(color_st, s); if (i < 0 || i > 7) return;
    o->blend_en = B(en) ? (o->blend_en | 1u << i) : (o->blend_en & ~(1u << i));
}
NVN(nvnChannelMaskStateSetDefaults, void, NVNchannelMaskState *s) { ST(chmask_st, s)->mask = 0xffffffffu; }
NVN(nvnChannelMaskStateSetChannelMask, void, NVNchannelMaskState *s, int i, uint32_t r, uint32_t g, uint32_t b, uint32_t a) {
    if (i < 0 || i > 7) return;
    uint32_t m = (B(r) ? 1 : 0) | (B(g) ? 2 : 0) | (B(b) ? 4 : 0) | (B(a) ? 8 : 0);
    chmask_st *o = ST(chmask_st, s); o->mask = (o->mask & ~(0xfu << (4 * i))) | m << (4 * i);
}
NVN(nvnMultisampleStateSetDefaults, void, NVNmultisampleState *s) { memset(s, 0, sizeof(ms_st)); ST(ms_st, s)->enable = 1; }
NVN(nvnMultisampleStateSetMultisampleEnable, void, NVNmultisampleState *s, uint32_t e) { ST(ms_st, s)->enable = B(e) != 0; }
NVN(nvnMultisampleStateSetSamples, void, NVNmultisampleState *s, int n) { ST(ms_st, s)->samples = (uint8_t)n; }
NVN(nvnMultisampleStateSetAlphaToCoverageEnable, void, NVNmultisampleState *s, uint32_t e) { ST(ms_st, s)->a2c = B(e) != 0; }
NVN(nvnMultisampleStateSetSampleLocationsEnable, void, NVNmultisampleState *s, uint32_t e) { ST(ms_st, s)->sl_en = B(e) != 0; }
NVN(nvnMultisampleStateSetSampleLocations, void, NVNmultisampleState *s, int start, int n, const float *v) { (void)s; (void)start; (void)n; (void)v; }
NVN(nvnMultisampleStateGetSamples, int, const NVNmultisampleState *s) { return ((const ms_st *)s)->samples; }
NVN(nvnMultisampleStateGetMultisampleEnable, NVNboolean, const NVNmultisampleState *s) { return ((const ms_st *)s)->enable; }
NVN(nvnPolygonStateSetDefaults, void, NVNpolygonState *s) { *ST(poly_st, s) = poly_st{NVN_FACE_NONE, NVN_FRONT_FACE_CCW, NVN_POLYGON_MODE_FILL, 0}; }
NVN(nvnPolygonStateSetCullFace, void, NVNpolygonState *s, NVNface f) { ST(poly_st, s)->cull = f; }
NVN(nvnPolygonStateSetFrontFace, void, NVNpolygonState *s, NVNfrontFace f) { ST(poly_st, s)->front = f; }
NVN(nvnPolygonStateSetPolygonMode, void, NVNpolygonState *s, NVNpolygonMode m) { ST(poly_st, s)->mode = m; }
NVN(nvnPolygonStateSetPolygonOffsetEnables, void, NVNpolygonState *s, int e) { ST(poly_st, s)->offset_en = e; }
NVN(nvnDepthStencilStateSetDefaults, void, NVNdepthStencilState *s) {
    *ST(ds_st, s) = ds_st{0, 1, NVN_DEPTH_FUNC_LESS, 0, NVN_STENCIL_FUNC_ALWAYS, NVN_STENCIL_FUNC_ALWAYS,
                            NVN_STENCIL_OP_KEEP, NVN_STENCIL_OP_KEEP, NVN_STENCIL_OP_KEEP, NVN_STENCIL_OP_KEEP, NVN_STENCIL_OP_KEEP, NVN_STENCIL_OP_KEEP};
}
NVN(nvnDepthStencilStateSetDepthTestEnable, void, NVNdepthStencilState *s, uint32_t e) { ST(ds_st, s)->dt = B(e) != 0; }
NVN(nvnDepthStencilStateSetDepthWriteEnable, void, NVNdepthStencilState *s, uint32_t e) { ST(ds_st, s)->dw = B(e) != 0; }
NVN(nvnDepthStencilStateSetDepthFunc, void, NVNdepthStencilState *s, NVNdepthFunc f) { ST(ds_st, s)->df = f; }
NVN(nvnDepthStencilStateSetStencilTestEnable, void, NVNdepthStencilState *s, uint32_t e) { ST(ds_st, s)->st = B(e) != 0; }
NVN(nvnDepthStencilStateSetStencilFunc, void, NVNdepthStencilState *s, NVNface f, NVNstencilFunc fn) {
    ds_st *o = ST(ds_st, s); if (f & NVN_FACE_FRONT) o->sf_f = fn; if (f & NVN_FACE_BACK) o->sf_b = fn;
}
NVN(nvnDepthStencilStateSetStencilOp, void, NVNdepthStencilState *s, NVNface f, NVNstencilOp fail, NVNstencilOp zfail, NVNstencilOp zpass) {
    ds_st *o = ST(ds_st, s);
    if (f & NVN_FACE_FRONT) { o->fail_f = fail; o->zfail_f = zfail; o->zpass_f = zpass; }
    if (f & NVN_FACE_BACK) { o->fail_b = fail; o->zfail_b = zfail; o->zpass_b = zpass; }
}
NVN(nvnVertexAttribStateSetDefaults, void, NVNvertexAttribState *s) { *ST(va_st, s) = va_st{0, 0, 0}; }
NVN(nvnVertexAttribStateSetFormat, void, NVNvertexAttribState *s, NVNformat f, ptrdiff_t off) {
    if (off < 0 || off >= (1 << 19)) LOG("[RED] nvn: vertex attribute offset %td exceeds the packed range", off);
    ST(va_st, s)->fmt = f; ST(va_st, s)->offset = (uint32_t)off;
}
NVN(nvnVertexAttribStateSetStreamIndex, void, NVNvertexAttribState *s, int i) { ST(va_st, s)->stream = i; }
NVN(nvnVertexStreamStateSetDefaults, void, NVNvertexStreamState *s) { *ST(vs_st, s) = vs_st{0, 0}; }
NVN(nvnVertexStreamStateSetStride, void, NVNvertexStreamState *s, ptrdiff_t st) { ST(vs_st, s)->stride = (uint32_t)st; }
NVN(nvnVertexStreamStateSetDivisor, void, NVNvertexStreamState *s, int d) { ST(vs_st, s)->divisor = d; }
// Getters: Monster Hunter Rise reads its stream states back when it builds a draw (about 24,000 calls in its character creation)
NVN(nvnVertexStreamStateGetStride, ptrdiff_t, const NVNvertexStreamState *s) { return (ptrdiff_t)((const vs_st *)s)->stride; }
NVN(nvnVertexStreamStateGetDivisor, int, const NVNvertexStreamState *s) { return ((const vs_st *)s)->divisor; }
NVN(nvnVertexAttribStateGetFormat, void, const NVNvertexAttribState *s, NVNformat *f, ptrdiff_t *off) { const va_st *o = (const va_st *)s; if (f) *f = (NVNformat)o->fmt; if (off) *off = (ptrdiff_t)o->offset; }
NVN(nvnVertexAttribStateGetStreamIndex, int, const NVNvertexAttribState *s) { return ((const va_st *)s)->stream; }
// ---------- Color state, getters and the rest of the multisample state ----------
// Color state: alpha test function, logic op (the fields already exist in color_st)
NVN(nvnColorStateSetAlphaTest, void, NVNcolorState *s, NVNalphaFunc f) { ST(color_st, s)->alpha_func = (uint32_t)f; }
NVN(nvnColorStateSetLogicOp, void, NVNcolorState *s, NVNlogicOp op) { ST(color_st, s)->logic_op = (uint32_t)op; }
// Getters and the rest of the multisample state: Pokémon Legends: Z-A (Trinity, SDK 19) reads its state objects and builders back when it builds its pipeline states; the stub returned
// whatever was left in x0 (the object's address), so a blend enable or depth test read back as true, a texture builder's width as an address
NVN(nvnColorStateGetBlendEnable, NVNboolean, const NVNcolorState *s, int i) { return i >= 0 && i < 8 && (((const color_st *)s)->blend_en >> i & 1); }
NVN(nvnColorStateGetAlphaTest, NVNalphaFunc, const NVNcolorState *s) { return (NVNalphaFunc)((const color_st *)s)->alpha_func; }
NVN(nvnColorStateGetLogicOp, NVNlogicOp, const NVNcolorState *s) { return (NVNlogicOp)((const color_st *)s)->logic_op; }
NVN(nvnChannelMaskStateGetChannelMask, void, const NVNchannelMaskState *s, int i, NVNboolean *r, NVNboolean *g, NVNboolean *b, NVNboolean *a) {
    uint32_t m = i >= 0 && i < 8 ? ((const chmask_st *)s)->mask >> (4 * i) & 0xf : 0;
    if (r) *r = (m & 1) != 0; if (g) *g = (m & 2) != 0; if (b) *b = (m & 4) != 0; if (a) *a = (m & 8) != 0;
}
NVN(nvnBlendStateGetBlendTarget, int, const NVNblendState *s) { return (int)((const blend_st *)s)->target; }
NVN(nvnBlendStateGetBlendFunc, void, const NVNblendState *s, NVNblendFunc *sr, NVNblendFunc *dr, NVNblendFunc *sa, NVNblendFunc *da) {
    const blend_st *o = (const blend_st *)s;
    if (sr) *sr = (NVNblendFunc)o->src_rgb; if (dr) *dr = (NVNblendFunc)o->dst_rgb; if (sa) *sa = (NVNblendFunc)o->src_a; if (da) *da = (NVNblendFunc)o->dst_a;
}
NVN(nvnBlendStateGetBlendEquation, void, const NVNblendState *s, NVNblendEquation *er, NVNblendEquation *ea) {
    const blend_st *o = (const blend_st *)s; if (er) *er = (NVNblendEquation)o->eq_rgb; if (ea) *ea = (NVNblendEquation)o->eq_a;
}
NVN(nvnBlendStateGetAdvancedMode, NVNblendAdvancedMode, const NVNblendState *s) { return (NVNblendAdvancedMode)((const blend_st *)s)->adv; }
NVN(nvnBlendStateGetAdvancedPremultipliedSrc, NVNboolean, const NVNblendState *s) { return !((const blend_st *)s)->adv_npm; }
NVN(nvnBlendStateGetAdvancedOverlap, NVNblendAdvancedOverlap, const NVNblendState *s) { return (NVNblendAdvancedOverlap)((const blend_st *)s)->adv_ovl; }
NVN(nvnPolygonStateGetCullFace, NVNface, const NVNpolygonState *s) { return (NVNface)((const poly_st *)s)->cull; }
NVN(nvnPolygonStateGetFrontFace, NVNfrontFace, const NVNpolygonState *s) { return (NVNfrontFace)((const poly_st *)s)->front; }
NVN(nvnPolygonStateGetPolygonMode, NVNpolygonMode, const NVNpolygonState *s) { return (NVNpolygonMode)((const poly_st *)s)->mode; }
NVN(nvnPolygonStateGetPolygonOffsetEnables, NVNpolygonOffsetEnable, const NVNpolygonState *s) { return (NVNpolygonOffsetEnable)((const poly_st *)s)->offset_en; }
NVN(nvnDepthStencilStateGetDepthTestEnable, NVNboolean, const NVNdepthStencilState *s) { return ((const ds_st *)s)->dt; }
NVN(nvnDepthStencilStateGetDepthWriteEnable, NVNboolean, const NVNdepthStencilState *s) { return ((const ds_st *)s)->dw; }
NVN(nvnDepthStencilStateGetDepthFunc, NVNdepthFunc, const NVNdepthStencilState *s) { return (NVNdepthFunc)((const ds_st *)s)->df; }
NVN(nvnDepthStencilStateGetStencilTestEnable, NVNboolean, const NVNdepthStencilState *s) { return ((const ds_st *)s)->st; }
NVN(nvnDepthStencilStateGetStencilFunc, NVNstencilFunc, const NVNdepthStencilState *s, NVNface f) { const ds_st *o = (const ds_st *)s; return (NVNstencilFunc)(f == NVN_FACE_BACK ? o->sf_b : o->sf_f); }
NVN(nvnDepthStencilStateGetStencilOp, void, const NVNdepthStencilState *s, NVNface f, NVNstencilOp *fail, NVNstencilOp *zfail, NVNstencilOp *zpass) {
    const ds_st *o = (const ds_st *)s; int b = f == NVN_FACE_BACK;
    if (fail) *fail = (NVNstencilOp)(b ? o->fail_b : o->fail_f); if (zfail) *zfail = (NVNstencilOp)(b ? o->zfail_b : o->zfail_f); if (zpass) *zpass = (NVNstencilOp)(b ? o->zpass_b : o->zpass_f);
}
// Multisample state: recorded only (the replay uses the sample count, multisample enable and alpha to coverage)
NVN(nvnMultisampleStateSetAlphaToCoverageDither, void, NVNmultisampleState *s, uint32_t e) { ST(ms_st, s)->a2c_dither = B(e) != 0; }
NVN(nvnMultisampleStateSetRasterSamples, void, NVNmultisampleState *s, int n) { ST(ms_st, s)->raster_samples = (uint8_t)n; }
NVN(nvnMultisampleStateSetCoverageModulationMode, void, NVNmultisampleState *s, NVNcoverageModulationMode m) { ST(ms_st, s)->cov_mod = (uint8_t)m; }
NVN(nvnMultisampleStateSetCoverageToColorEnable, void, NVNmultisampleState *s, uint32_t e) { ST(ms_st, s)->cov2color_en = B(e) != 0; }
NVN(nvnMultisampleStateSetCoverageToColorOutput, void, NVNmultisampleState *s, int i) { ST(ms_st, s)->cov2color_out = (uint8_t)i; }
NVN(nvnMultisampleStateSetSampleLocationsGridEnable, void, NVNmultisampleState *s, uint32_t e) { ST(ms_st, s)->sl_grid = B(e) != 0; }
NVN(nvnMultisampleStateGetAlphaToCoverageEnable, NVNboolean, const NVNmultisampleState *s) { return ((const ms_st *)s)->a2c; }
NVN(nvnMultisampleStateGetAlphaToCoverageDither, NVNboolean, const NVNmultisampleState *s) { return ((const ms_st *)s)->a2c_dither; }
NVN(nvnMultisampleStateGetRasterSamples, int, NVNmultisampleState *s) { return ((const ms_st *)s)->raster_samples; }
NVN(nvnMultisampleStateGetCoverageModulationMode, NVNcoverageModulationMode, const NVNmultisampleState *s) { return (NVNcoverageModulationMode)((const ms_st *)s)->cov_mod; }
NVN(nvnMultisampleStateGetCoverageToColorEnable, NVNboolean, const NVNmultisampleState *s) { return ((const ms_st *)s)->cov2color_en; }
NVN(nvnMultisampleStateGetCoverageToColorOutput, int, const NVNmultisampleState *s) { return ((const ms_st *)s)->cov2color_out; }
NVN(nvnMultisampleStateGetSampleLocationsEnable, NVNboolean, const NVNmultisampleState *s) { return ((const ms_st *)s)->sl_en; }
NVN(nvnMultisampleStateGetSampleLocationsGridEnable, NVNboolean, const NVNmultisampleState *s) { return ((const ms_st *)s)->sl_grid; }

}  // namespace nvn
