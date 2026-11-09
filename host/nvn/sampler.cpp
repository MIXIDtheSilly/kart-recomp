// Native NVN: samplers (builder and object). Parameters are kept in samp_par and handed to the renderer at RegisterSampler.
#include "nvn_int.h"

namespace nvn {

#define SB(b) (&((samp_builder_t *)(b))->p)
NVN(nvnSamplerBuilderSetDefaults, void, NVNsamplerBuilder *b) {
    NVNdevice *dev = ((samp_builder_t *)b)->dev; memset(b, 0, sizeof(samp_builder_t)); ((samp_builder_t *)b)->dev = dev;
    samp_par *o = SB(b);
    o->minf = NVN_MIN_FILTER_NEAREST_MIPMAP_LINEAR; o->magf = NVN_MAG_FILTER_LINEAR;   // GL defaults
    o->ws = o->wt = o->wr = NVN_WRAP_MODE_REPEAT; o->cmp_func = NVN_COMPARE_FUNC_LESS;
    o->lod_min = -1000.f; o->lod_max = 1000.f; o->aniso = 1.f;
}
NVN(nvnSamplerBuilderSetDevice, void, NVNsamplerBuilder *b, NVNdevice *d) { ((samp_builder_t *)b)->dev = d; }
NVN(nvnSamplerBuilderSetMinMagFilter, void, NVNsamplerBuilder *b, NVNminFilter mi, NVNmagFilter ma) { SB(b)->minf = mi; SB(b)->magf = ma; }
NVN(nvnSamplerBuilderSetWrapMode, void, NVNsamplerBuilder *b, NVNwrapMode s, NVNwrapMode t, NVNwrapMode r) { SB(b)->ws = s; SB(b)->wt = t; SB(b)->wr = r; }
NVN(nvnSamplerBuilderSetLodClamp, void, NVNsamplerBuilder *b, float lo, float hi) { SB(b)->lod_min = lo; SB(b)->lod_max = hi; }
NVN(nvnSamplerBuilderSetLodBias, void, NVNsamplerBuilder *b, float v) { SB(b)->lod_bias = v; }
NVN(nvnSamplerBuilderSetCompare, void, NVNsamplerBuilder *b, NVNcompareMode m, NVNcompareFunc f) { SB(b)->cmp_mode = m; SB(b)->cmp_func = f; }
NVN(nvnSamplerBuilderSetBorderColor, void, NVNsamplerBuilder *b, const float *c) { memcpy(SB(b)->border, c, 16); }
// Border colours are converted numerically, also for integer textures. Bit-casting integer 1 to float would select transparent black instead of opaque black/white.
NVN(nvnSamplerBuilderSetBorderColori, void, NVNsamplerBuilder *b, const int *c) { for (int i = 0; i < 4; i++) SB(b)->border[i] = (float)c[i]; }
NVN(nvnSamplerBuilderSetBorderColorui, void, NVNsamplerBuilder *b, const uint32_t *c) { for (int i = 0; i < 4; i++) SB(b)->border[i] = (float)c[i]; }
NVN(nvnSamplerBuilderSetMaxAnisotropy, void, NVNsamplerBuilder *b, float v) { SB(b)->aniso = v; }
NVN(nvnSamplerBuilderSetLodSnap, void, NVNsamplerBuilder *b, float v) { (void)b; (void)v; }   // No host equivalent, ignored
NVN(nvnSamplerBuilderGetMinMagFilter, void, const NVNsamplerBuilder *b, NVNminFilter *mi, NVNmagFilter *ma) {
    const samp_par *o = &((const samp_builder_t *)b)->p; *mi = (NVNminFilter)o->minf; *ma = (NVNmagFilter)o->magf;
}
NVN(nvnSamplerBuilderGetWrapMode, void, const NVNsamplerBuilder *b, NVNwrapMode *s, NVNwrapMode *t, NVNwrapMode *r) {
    const samp_par *o = &((const samp_builder_t *)b)->p; *s = (NVNwrapMode)o->ws; *t = (NVNwrapMode)o->wt; *r = (NVNwrapMode)o->wr;
}
NVN(nvnSamplerBuilderGetLodClamp, void, const NVNsamplerBuilder *b, float *lo, float *hi) {   // Bayonetta 3 reads it back while building its samplers
    const samp_par *o = &((const samp_builder_t *)b)->p; *lo = o->lod_min; *hi = o->lod_max;
}
NVN(nvnSamplerInitialize, NVNboolean, NVNsampler *s, const NVNsamplerBuilder *b) {
    samp_t *o = (samp_t *)s; o->magic = NVN_MAGIC('S', 'M', 'P', 0); o->p = ((const samp_builder_t *)b)->p; return 1;
}
NVN(nvnSamplerFinalize, void, NVNsampler *s) { ((samp_t *)s)->magic = 0; }
// Samplers: the getters read the parameters in the object. Reduction (min / max of the texels a filtered sample covers) maps to the renderer's sampler reduction mode
NVN(nvnSamplerBuilderSetReductionFilter, void, NVNsamplerBuilder *b, NVNsamplerReduction r) { SB(b)->reduction = (uint8_t)r; }
NVN(nvnSamplerSetDebugLabel, void, NVNsampler *s, const char *l) { (void)s; (void)l; }
#define SP(s) (&((const samp_t *)(s))->p)
NVN(nvnSamplerGetMinMagFilter, void, const NVNsampler *s, NVNminFilter *mi, NVNmagFilter *ma) { *mi = (NVNminFilter)SP(s)->minf; *ma = (NVNmagFilter)SP(s)->magf; }
NVN(nvnSamplerGetWrapMode, void, const NVNsampler *s, NVNwrapMode *a, NVNwrapMode *b, NVNwrapMode *c) { *a = (NVNwrapMode)SP(s)->ws; *b = (NVNwrapMode)SP(s)->wt; *c = (NVNwrapMode)SP(s)->wr; }
NVN(nvnSamplerGetLodClamp, void, const NVNsampler *s, float *lo, float *hi) { *lo = SP(s)->lod_min; *hi = SP(s)->lod_max; }
NVN(nvnSamplerGetLodBias, float, const NVNsampler *s) { return SP(s)->lod_bias; }
NVN(nvnSamplerGetCompare, void, const NVNsampler *s, NVNcompareMode *m, NVNcompareFunc *f) { *m = (NVNcompareMode)SP(s)->cmp_mode; *f = (NVNcompareFunc)SP(s)->cmp_func; }
NVN(nvnSamplerGetBorderColor, void, const NVNsampler *s, float *c) { memcpy(c, SP(s)->border, 16); }
NVN(nvnSamplerGetMaxAnisotropy, float, const NVNsampler *s) { return SP(s)->aniso; }
NVN(nvnSamplerGetReductionFilter, NVNsamplerReduction, const NVNsampler *s) { return (NVNsamplerReduction)SP(s)->reduction; }
// whether two samplers have the same state (games use it to deduplicate sampler pool entries)
NVN(nvnSamplerCompare, NVNboolean, const NVNsampler *a, const NVNsampler *b) {
    const samp_par *x = SP(a), *y = SP(b);
    return x->minf == y->minf && x->magf == y->magf && x->ws == y->ws && x->wt == y->wt && x->wr == y->wr && x->cmp_mode == y->cmp_mode && x->cmp_func == y->cmp_func && x->reduction == y->reduction &&
           x->lod_min == y->lod_min && x->lod_max == y->lod_max && x->lod_bias == y->lod_bias && x->aniso == y->aniso && !memcmp(x->border, y->border, sizeof x->border);
}

}  // namespace nvn
