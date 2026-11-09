// Native NVN: textures (builders, objects, views), CPU texel access.
// Texture storage lives in pool memory in Tegra block-linear layout (nvn_tex.h); the renderer reads it from there (packaged and CPU-written
// textures) or owns the content (render targets). The interface layer only computes the layout, copies packaged data, and tells the renderer.
#include "nvn_int.h"

namespace nvn {

#define TB(b) ((tex_builder_t*)(b))
#define TX(t) ((tex_t*)(t))
#define CTX(t) ((const tex_t*)(t))
#define VW(v) ((view_t*)(v))

// A texture in a virtual (sparse) pool has no host address: its pool's address range is reserved in the guest address space but not mapped, so t->mem must never be dereferenced.
// The renderer reads such textures through nvn::sparse_* (replay.h); the CPU paths below (packaged data, WriteTexels, copy mirroring) cannot reach their storage.
static bool unmapped(const tex_t* t) { return t->pool && sparse_pool(t->pool); }

// ---------- Texture builder ----------
NVN(nvnTextureBuilderSetDefaults, void, NVNtextureBuilder *b) {
    NVNdevice *dev = TB(b)->dev; memset(b, 0, sizeof(tex_builder_t)); TB(b)->dev = dev;
    TB(b)->d.target = NVN_TEXTURE_TARGET_2D; TB(b)->d.width = TB(b)->d.height = TB(b)->d.depth = TB(b)->d.levels = 1;
}
NVN(nvnTextureBuilderSetDevice, void, NVNtextureBuilder *b, NVNdevice *d) { TB(b)->dev = d; }
NVN(nvnTextureBuilderSetFlags, void, NVNtextureBuilder *b, int f) { TB(b)->d.flags = f; }
NVN(nvnTextureBuilderSetTarget, void, NVNtextureBuilder *b, NVNtextureTarget t) { TB(b)->d.target = t; }
NVN(nvnTextureBuilderSetWidth, void, NVNtextureBuilder *b, int v) { TB(b)->d.width = v; }
NVN(nvnTextureBuilderSetHeight, void, NVNtextureBuilder *b, int v) { TB(b)->d.height = v; }
NVN(nvnTextureBuilderSetDepth, void, NVNtextureBuilder *b, int v) { TB(b)->d.depth = v; }
NVN(nvnTextureBuilderSetSize1D, void, NVNtextureBuilder *b, int w) { TB(b)->d.width = w; TB(b)->d.height = TB(b)->d.depth = 1; }
NVN(nvnTextureBuilderSetLevels, void, NVNtextureBuilder *b, int v) { TB(b)->d.levels = v; }
NVN(nvnTextureBuilderSetFormat, void, NVNtextureBuilder *b, NVNformat f) { TB(b)->d.format = f; }
NVN(nvnTextureBuilderSetSamples, void, NVNtextureBuilder *b, int s) { TB(b)->d.samples = s; }
NVN(nvnTextureBuilderSetStride, void, NVNtextureBuilder *b, ptrdiff_t s) { TB(b)->d.stride = (uint32_t)s; }
NVN(nvnTextureBuilderSetStorage, void, NVNtextureBuilder *b, NVNmemoryPool *p, ptrdiff_t off) { TB(b)->pool = (pool_t *)p; TB(b)->off = off; }
NVN(nvnTextureBuilderSetPackagedTextureLayout, void, NVNtextureBuilder *b, const NVNpackagedTextureLayout *l) {
    TB(b)->has_layout = l != NULL; if (l) memcpy(TB(b)->layout, l->layout, 8);
    TB(b)->d.pkg_block_h = l ? l->layout[0] + 1 : 0;
}
NVN(nvnTextureBuilderGetFlags, NVNtextureFlags, const NVNtextureBuilder *b) { return (NVNtextureFlags)((const tex_builder_t *)b)->d.flags; }
static uint64_t g_layout_logged;
static void log_layout(const tex_builder_t *b, const nvn_texlayout *L) {
    if (b->has_layout && g_layout_logged++ < 16)
        LOG("nvn: packaged texture %ux%ux%u levels %u format %#x target %d layout bytes %02x%02x%02x%02x%02x%02x%02x%02x → block height log2=%u (from layout byte 0) size %#llx",
            b->d.width, b->d.height, b->d.depth, b->d.levels, b->d.format, b->d.target, b->layout[0], b->layout[1], b->layout[2],
            b->layout[3], b->layout[4], b->layout[5], b->layout[6], b->layout[7], L->tileH, L->size);
}
NVN(nvnTextureBuilderGetStorageSize, size_t, const NVNtextureBuilder *b) {
    nvn_texlayout L; nvn_tex_layout(&((const tex_builder_t *)b)->d, &L);
    if (!L.bpb) LOG("[RED] nvn: TextureBuilderGetStorageSize unknown format %#x", ((const tex_builder_t *)b)->d.format);
    log_layout((const tex_builder_t *)b, &L);
    return L.size;
}
NVN(nvnTextureBuilderGetStorageAlignment, size_t, const NVNtextureBuilder *b) {
    nvn_texlayout L; nvn_tex_layout(&((const tex_builder_t *)b)->d, &L); return L.align;
}
// The tiled levels of a sparse texture and its tile size (nvn_tex_sparse_tiles). OCTOPATH TRAVELER (Unreal Engine 4 on SDK 3.5.1) sizes a sparse texture's mip tail as storage(all
// levels) − storage(numTiledLevels levels) and allocates it from its heap at boot (main+0x1f1a688); unimplemented, the count was stack garbage and it asked for the whole 1.75 GB texture twice
static void sparse_layout(const nvn_texdesc *d, NVNtextureSparseTileLayout *o) {
    uint32_t tw, th, td; memset(o, 0, sizeof *o);
    o->numTiledLevels = nvn_tex_sparse_tiles(d, &tw, &th, &td); o->tileWidth = (int)tw; o->tileHeight = (int)th; o->tileDepth = (int)td;
}
NVN(nvnTextureBuilderGetSparseTileLayout, void, const NVNtextureBuilder *b, NVNtextureSparseTileLayout *o) { sparse_layout(&((const tex_builder_t *)b)->d, o); }

NVN(nvnTextureInitialize, NVNboolean, NVNtexture* t, const NVNtextureBuilder* b) {
    const tex_builder_t* s = (const tex_builder_t*)b; tex_t* o = TX(t);
    // Re-initializing a texture object that is still live (without a Finalize first): release the host texture of the old serial first. SMBW re-initializes its transient render targets on the same object every frame and never Finalizes them;
    // previously every init created a new private image and the old one stayed in the serial table forever, so GPU memory grew about 13 MB per frame
    if (o->magic == NVN_MAGIC('T', 'E', 'X', 0) && o->serial) { ONCE_LOG("TextureInitialize on a texture object that is still live (no Finalize): releasing the old host texture (first case ser=%llu)", (unsigned long long)o->serial); rp_tex_release(o); }
    memset(o, 0, sizeof *o);
    o->magic = NVN_MAGIC('T', 'E', 'X', 0); o->d = s->d; o->pool = s->pool; o->off = s->off;
    o->has_layout = s->has_layout; memcpy(o->layout, s->layout, 8); o->set_swz = s->set_swz; o->stencil = s->stencil; o->swz = s->swz;
    nvn_tex_layout(&o->d, &o->L);
    o->mem = s->pool && s->pool->mem ? s->pool->mem + s->off : nullptr;
    o->serial = new_serial(); rp_tex_init(o);
    // See SetPackagedTextureData. Linear textures are not re-uploaded as packaged: they are created on the pool buffer, whatever the pool's CPU access, and read what was copied in
    if (s->pkg_data && o->mem && unmapped(o)) ONCE_RED("TextureInitialize: packaged data for a texture in a sparse pool cannot be copied; ignored");
    else if (s->pkg_data && o->mem) {
        memcpy(o->mem, s->pkg_data, o->L.size);
        if (!(o->d.flags & (NVN_TEXTURE_FLAGS_LINEAR | NVN_TEXTURE_FLAGS_LINEAR_RENDER_TARGET))) rp_tex_cpu_written(o);
    }
    OLOG("Texture %p %ux%ux%u lv%u fmt=%#x tgt=%d flags=%#x size=%#llx", (void*)t, o->d.width, o->d.height, o->d.depth,
         o->d.levels, o->d.format, o->d.target, o->d.flags, (unsigned long long)o->L.size);
    return 1;
}
NVN(nvnTextureFinalize, void, NVNtexture* t) { rp_tex_release(TX(t)); TX(t)->magic = 0; }
NVN(nvnTextureGetFlags, NVNtextureFlags, const NVNtexture *t) { return (NVNtextureFlags)CTX(t)->d.flags; }
NVN(nvnTextureGetWidth, int, const NVNtexture *t) { return (int)CTX(t)->d.width; }
NVN(nvnTextureGetHeight, int, const NVNtexture *t) { return (int)CTX(t)->d.height; }
NVN(nvnTextureGetDepth, int, const NVNtexture *t) { return (int)CTX(t)->d.depth; }
NVN(nvnTextureGetStorageSize, int, const NVNtexture *t) { return (int)CTX(t)->L.size; }
NVN(nvnTextureGetStride, ptrdiff_t, const NVNtexture *t) { return (ptrdiff_t)CTX(t)->d.stride; }   // the row stride of a linear texture as built (0 for block-linear ones)
// The queries below are used by Engage (Unity) at startup: it calls MemoryPoolMap on "the pool the texture / buffer lives in", and with a stub returning 0 it dereferenced a null pool and crashed
NVN(nvnTextureGetLevels, int, const NVNtexture *t) { return (int)CTX(t)->d.levels; }
NVN(nvnTextureGetSparseTileLayout, void, const NVNtexture *t, NVNtextureSparseTileLayout *o) { sparse_layout(&CTX(t)->d, o); }
NVN(nvnTextureGetSamples, int, const NVNtexture *t) { return CTX(t)->d.samples; }
NVN(nvnTextureGetMemoryPool, NVNmemoryPool *, const NVNtexture *t) { return (NVNmemoryPool *)CTX(t)->pool; }
NVN(nvnTextureGetMemoryOffset, ptrdiff_t, const NVNtexture *t) { return (ptrdiff_t)CTX(t)->off; }
// The GPU address of the texture's storage (DRAGON QUEST XI S, Unreal Engine 4.18, asks for it at start): pool address + offset, the same address space as nvnMemoryPoolGetBufferAddress
NVN(nvnTextureGetTextureAddress, NVNtextureAddress, const NVNtexture *t) { const tex_t *o = CTX(t); return o->pool ? (NVNtextureAddress)(ga(o->pool->mem) + o->off) : 0; }
// The size of a texture's ZCull save area (SaveZCullData / RestoreZCullData). The host does no ZCull and never touches the area, but games allocate it from their own pools, and NVN
// does not report 0. Cuphead (Unity 2017.4) allocates one per depth target (main+0x1948060) from its pool allocator, which gives a zero-size request the offset of a live block; its
// map of allocations then loses that block (the zero-size one's size overwrites it, and freeing the zero-size one deletes it), and the block's own free later pushes a garbage-sized
// range onto the free list (main+0x1945b10). Buffers allocated after that overlap live ones: copies read staging bytes other copies had overwritten, and sprites broke into slivers,
// went missing, or flickered every fourth frame (the upload ring's period). The hardware's formula is not known; this one grows with the texture: 8 bytes per 32x32 tile and layer,
// at least 256
static size_t zcull_size(const nvn_texdesc *d) {
    uint64_t tiles = (uint64_t)((d->width + 31) / 32) * ((d->height + 31) / 32) * (d->depth ? d->depth : 1);
    return tiles ? (size_t)((tiles * 8 + 0xff) & ~0xffull) : 0x100;
}
NVN(nvnTextureGetZCullStorageSize, size_t, const NVNtexture *t) { return zcull_size(&CTX(t)->d); }
static unsigned tex_swz(const tex_t *t) { return t->set_swz ? t->swz : 2u | 3u << 3 | 4u << 6 | 5u << 9; }   // the channel mapping: identity R G B A when TextureBuilderSetSwizzle was not called
// The mapping counts: BotW and Odyssey rebuild layout captures on the same storage with a new mapping, and replace the registered descriptor only if the two compare different (BotW main+0xec33f0)
NVN(nvnTextureCompare, NVNboolean, const NVNtexture *a, const NVNtexture *b) {
    const tex_t *x = CTX(a), *y = CTX(b);
    return !memcmp(&x->d, &y->d, sizeof x->d) && x->mem == y->mem && tex_swz(x) == tex_swz(y);
}
// No prototype in the header; call sites such as main 0x174899c compare the return value with 1 and set some status bit 0x100 if not equal. Meaning unverified; return 1 for now
NVN_NOPROTO(nvnTextureGetRawStorageClass, int, const NVNtexture *t) { (void)t; return 1; }
// The class a texture from this builder would report, so the two agree: EA SPORTS FC 27's Frostbite renderer asks the builder (the class names the page kind a virtual pool maps,
// NVNmappingRequest.storageClass, which the host's virtual pools do not distinguish)
NVN_NOPROTO(nvnTextureBuilderGetRawStorageClass, int, const NVNtextureBuilder *b) { (void)b; return 1; }
NVN(nvnTextureViewSetDefaults, void, NVNtextureView *v) { memset(v, 0, sizeof(view_t)); }
NVN(nvnTextureViewSetLevels, void, NVNtextureView *v, int base, int n) { VW(v)->set_levels = 1; VW(v)->base_level = base; VW(v)->nlevels = n; }
NVN(nvnTextureViewSetLayers, void, NVNtextureView *v, int min, int n) { VW(v)->set_layers = 1; VW(v)->min_layer = min; VW(v)->nlayers = n; }
NVN(nvnTextureViewSetFormat, void, NVNtextureView *v, NVNformat f) { VW(v)->set_format = 1; VW(v)->format = f; }
NVN(nvnTextureViewSetTarget, void, NVNtextureView *v, NVNtextureTarget t) { VW(v)->set_target = 1; VW(v)->target = t; }
NVN(nvnTextureViewGetLayers, NVNboolean, const NVNtextureView *v, int *min, int *n) {
    const view_t *o = (const view_t *)v; *min = o->min_layer; *n = o->nlayers; return o->set_layers;
}
NVN(nvnTextureGetViewOffset, ptrdiff_t, const NVNtexture *t, const NVNtextureView *v) {
    const tex_t *o = CTX(t); const view_t *w = (const view_t *)v;
    if (!w) return 0;
    uint64_t off = w->set_levels ? nvn_tex_level_offset(&o->d, (unsigned)w->base_level) : 0;
    if (w->set_layers) off += (uint64_t)w->min_layer * o->L.layer_size;
    return (ptrdiff_t)off;
}
// CPU texture read (NVN ReadTexels): for packaged textures the pool memory is the ground truth (de-layout and read directly); GPU-written textures are read back by the renderer (rp_read_texels)
static void read_texels(const tex_t *o, const view_t *v, const NVNcopyRegion *r, void *dstv, ptrdiff_t row, ptrdiff_t img) {
    uint8_t *dst = (uint8_t *)dstv;
    int bw, bh, bpb = nvn_fmt_bpb(o->d.format, &bw, &bh);
    uint32_t wb = (uint32_t)((r->width + bw - 1) / bw) * bpb, hb = (uint32_t)(r->height + bh - 1) / bh;
    if (!row) row = wb;
    if (!img) img = row * hb;
    unsigned level = v && v->set_levels ? (unsigned)v->base_level : 0, layer0 = v && v->set_layers ? (unsigned)v->min_layer : 0;
    rp_read_texels(o, level, layer0, r, dst, (size_t)row, (size_t)img);
}
NVN(nvnTextureReadTexels, void, const NVNtexture *t, const NVNtextureView *v, const NVNcopyRegion *r, void *dst) {
    read_texels(CTX(t), (const view_t *)v, r, dst, 0, 0);
}
NVN(nvnTextureReadTexelsStrided, void, const NVNtexture *t, const NVNtextureView *v, const NVNcopyRegion *r, void *dst, ptrdiff_t row, ptrdiff_t img) {
    read_texels(CTX(t), (const view_t *)v, r, dst, row, img);
}
NVN(nvnTextureInvalidateTexels, void, const NVNtexture *t, const NVNtextureView *v, const NVNcopyRegion *r) { (void)t; (void)v; (void)r; }

// ---------- Builder and view details ----------
NVN(nvnTextureBuilderSetSize2D, void, NVNtextureBuilder *b, int w, int h) { TB(b)->d.width = w; TB(b)->d.height = h; TB(b)->d.depth = 1; }
NVN(nvnTextureBuilderSetSize3D, void, NVNtextureBuilder *b, int w, int h, int d) { TB(b)->d.width = w; TB(b)->d.height = h; TB(b)->d.depth = d; }
NVN(nvnTextureBuilderSetSwizzle, void, NVNtextureBuilder *b, NVNtextureSwizzle r, NVNtextureSwizzle g, NVNtextureSwizzle bl, NVNtextureSwizzle a) {
    tex_builder_t *o = (tex_builder_t *)b; o->set_swz = 1; o->swz = (uint16_t)((r & 7) | (g & 7) << 3 | (bl & 7) << 6 | (a & 7) << 9);
    if (r != NVN_TEXTURE_SWIZZLE_R || g != NVN_TEXTURE_SWIZZLE_G || bl != NVN_TEXTURE_SWIZZLE_B || a != NVN_TEXTURE_SWIZZLE_A) ONCE_LOG("TextureBuilderSetSwizzle not identity (%d %d %d %d): remapped by the renderer when sampling", (int)r, (int)g, (int)bl, (int)a);
}
// STENCIL: the texture reads the stencil of its depth-stencil storage, as a stencil-mode view does (the renderer's descriptor resolution). Unreal Engine 4.27 makes its stencil shader resources this way: a second texture
// on the depth buffer's storage, built in stencil mode (DRAGON QUEST VII Reimagined's character occlusion mask, edbfc7a088be545e, tests stencil bits 0x1 and 0x84 of two of them; read as depth, the
// bits followed the depth values and the hero was drawn as a dithered silhouette inside a dithered square)
NVN(nvnTextureBuilderSetDepthStencilMode, void, NVNtextureBuilder *b, NVNtextureDepthStencilMode m) { TB(b)->stencil = m == NVN_TEXTURE_DEPTH_STENCIL_MODE_STENCIL; }
NVN(nvnTextureBuilderGetStorageClass, NVNstorageClass, const NVNtextureBuilder *b) { (void)b; return 0; }
// Used by Bowser's Fury: the texture's channel swizzle (set by TextureBuilderSetSwizzle at build time, identity R G B A when unset) and storage class (the host has none, 0 as for the builder)
NVN(nvnTextureGetSwizzle, void, const NVNtexture *t, NVNtextureSwizzle *r, NVNtextureSwizzle *g, NVNtextureSwizzle *b, NVNtextureSwizzle *a) {
    unsigned w = tex_swz(CTX(t));
    if (r) *r = (NVNtextureSwizzle)(w & 7); if (g) *g = (NVNtextureSwizzle)(w >> 3 & 7); if (b) *b = (NVNtextureSwizzle)(w >> 6 & 7); if (a) *a = (NVNtextureSwizzle)(w >> 9 & 7);
}
NVN(nvnTextureGetStorageClass, NVNstorageClass, const NVNtexture *t) { (void)t; return 0; }
NVN(nvnTextureBuilderGetZCullStorageSize, size_t, const NVNtextureBuilder *b) { return zcull_size(&TB(b)->d); }   // as for the texture (zcull_size)
// Packaged texture data in CPU memory (not written into the pool by the game): NVN copies it into the texture storage at TextureInitialize (the CPU cannot write a compressible pool; the driver copies). 13 Sentinels creates all its textures this way
NVN(nvnTextureBuilderSetPackagedTextureData, void, NVNtextureBuilder *b, const void *data) { TB(b)->pkg_data = data; }
NVN(nvnTextureGetFormat, NVNformat, const NVNtexture *t) { return (NVNformat)CTX(t)->d.format; }
NVN(nvnTextureGetTarget, NVNtextureTarget, const NVNtexture *t) { return (NVNtextureTarget)CTX(t)->d.target; }
NVN(nvnTextureSetDebugLabel, void, NVNtexture *t, const char *s) { (void)t; (void)s; }
NVN(nvnTextureViewGetLevels, NVNboolean, const NVNtextureView *v, int *base, int *n) { const view_t *w = (const view_t *)v; *base = w->base_level; *n = w->nlevels; return w->set_levels; }
NVN(nvnTextureViewGetTarget, NVNboolean, const NVNtextureView *v, NVNtextureTarget *t) { const view_t *w = (const view_t *)v; *t = (NVNtextureTarget)w->target; return w->set_target; }
NVN(nvnTextureViewSetDepthStencilMode, void, NVNtextureView *v, NVNtextureDepthStencilMode m) { VW(v)->set_dsmode = 1; VW(v)->dsmode = (uint8_t)m; }   // STENCIL: the replay binds a stencil-only view (tex_by_id)
NVN(nvnTextureViewSetSwizzle, void, NVNtextureView *v, NVNtextureSwizzle r, NVNtextureSwizzle g, NVNtextureSwizzle bl, NVNtextureSwizzle a) {
    view_t *w = VW(v); w->set_swizzle = 1; w->swz[0] = (uint8_t)r; w->swz[1] = (uint8_t)g; w->swz[2] = (uint8_t)bl; w->swz[3] = (uint8_t)a;
    if (r != NVN_TEXTURE_SWIZZLE_R || g != NVN_TEXTURE_SWIZZLE_G || bl != NVN_TEXTURE_SWIZZLE_B || a != NVN_TEXTURE_SWIZZLE_A) ONCE_LOG("TextureViewSetSwizzle not identity (%d %d %d %d): remapped by the renderer when sampling", (int)r, (int)g, (int)bl, (int)a);
}
NVN_NOPROTO(nvnTextureBuilderGetMemoryPool, NVNmemoryPool *, const NVNtextureBuilder *b) { return (NVNmemoryPool *)((const tex_builder_t *)b)->pool; }
NVN(nvnTextureBuilderGetMemoryOffset, ptrdiff_t, const NVNtextureBuilder *b) { return (ptrdiff_t)((const tex_builder_t *)b)->off; }
NVN(nvnTextureBuilderGetTarget, NVNtextureTarget, const NVNtextureBuilder *b) { return (NVNtextureTarget)((const tex_builder_t *)b)->d.target; }
NVN(nvnTextureBuilderGetWidth, int, const NVNtextureBuilder *b) { return (int)((const tex_builder_t *)b)->d.width; }
NVN(nvnTextureBuilderGetHeight, int, const NVNtextureBuilder *b) { return (int)((const tex_builder_t *)b)->d.height; }
NVN(nvnTextureBuilderGetDepth, int, const NVNtextureBuilder *b) { return (int)((const tex_builder_t *)b)->d.depth; }
NVN(nvnTextureBuilderGetLevels, int, const NVNtextureBuilder *b) { return (int)((const tex_builder_t *)b)->d.levels; }
NVN(nvnTextureBuilderGetFormat, NVNformat, const NVNtextureBuilder *b) { return (NVNformat)((const tex_builder_t *)b)->d.format; }
NVN(nvnTextureBuilderGetSamples, int, const NVNtextureBuilder *b) { return ((const tex_builder_t *)b)->d.samples; }
NVN(nvnTextureBuilderGetStride, ptrdiff_t, const NVNtextureBuilder *b) { return (ptrdiff_t)((const tex_builder_t *)b)->d.stride; }
NVN(nvnTextureViewGetFormat, NVNboolean, const NVNtextureView *v, NVNformat *f) { const view_t *w = (const view_t *)v; if (f) *f = (NVNformat)w->format; return w->set_format; }
NVN(nvnTextureViewGetDepthStencilMode, NVNboolean, const NVNtextureView *v, NVNtextureDepthStencilMode *m) { const view_t *w = (const view_t *)v; if (m) *m = (NVNtextureDepthStencilMode)w->dsmode; return w->set_dsmode; }
NVN(nvnTextureViewGetSwizzle, NVNboolean, const NVNtextureView *v, NVNtextureSwizzle *r, NVNtextureSwizzle *g, NVNtextureSwizzle *b, NVNtextureSwizzle *a) {
    const view_t *w = (const view_t *)v; NVNtextureSwizzle *o[4] = {r, g, b, a};
    for (int c = 0; c < 4; c++) if (o[c]) *o[c] = (NVNtextureSwizzle)(w->set_swizzle ? w->swz[c] : NVN_TEXTURE_SWIZZLE_R + c);
    return w->set_swizzle;
}
NVN(nvnTextureGetDepthStencilMode, NVNtextureDepthStencilMode, const NVNtexture *t) { return ((const tex_t *)t)->stencil ? NVN_TEXTURE_DEPTH_STENCIL_MODE_STENCIL : NVN_TEXTURE_DEPTH_STENCIL_MODE_DEPTH; }
// ---------- CPU texel access (WriteTexels) ----------
// CPU texture write (NVN WriteTexels): texture storage lives in pool memory in Tegra block-linear layout; decode the whole level → patch the region → re-encode.
// Write protection is lifted and the range marked dirty first, so the replay layer re-uploads it the next time it is used
// Non-3D textures: only the bytes inside the region are written, by block-linear address, without decoding the whole level. Metroid Dread's minimap (main+0x160e80) calls WriteTexels once per texel (a 1×1 region);
// previously every call decoded, patched and re-encoded the whole level, and after a few minutes the main thread spent whole frames there (frames stopped from 175 s). Returns −1 when the region is out of range or the format unsupported, falling back to the whole-level path
static int write_region_bl(const tex_t *t, unsigned level, unsigned layer, const NVNcopyRegion *r, const uint8_t *src, size_t row, int bw, int bh, int bpb) {
    uint32_t row_bytes, rows, slices, gbpb; uint64_t base; unsigned th, td;
    if (nvn_deswizzle_geom(&t->d, level, layer, &row_bytes, &rows, &slices, &base, &th, &td, &gbpb) || (int)gbpb != bpb || r->xoffset < 0 || r->yoffset < 0) return -1;
    uint32_t x0 = (uint32_t)(r->xoffset / bw) * (uint32_t)bpb, y0 = (uint32_t)(r->yoffset / bh);
    size_t wb = (size_t)((r->width + bw - 1) / bw) * (size_t)bpb, hb = (size_t)((r->height + bh - 1) / bh);
    if (x0 + wb > row_bytes || y0 + hb > rows) return -1;
    uint32_t lb = nvn_tex_level_gobs(&t->d, level) * 64; if (lb < row_bytes) lb = row_bytes;   // the row as laid out (padded in a sparse texture's tiled levels)
    for (size_t y = 0; y < hb; y++)
        for (size_t x = 0; x < wb;) {   // 16-byte runs are contiguous within a GOB
            uint32_t bx = x0 + (uint32_t)x; size_t n = 16 - bx % 16; if (n > wb - x) n = wb - x;
            memcpy(t->mem + base + nvn_bl_offset(bx, y0 + (uint32_t)y, 0, lb, rows, th, td), src + y * row + x, n); x += n;
        }
    return 0;
}
static void write_texels(const tex_t *t, const view_t *v, const NVNcopyRegion *r, const void *srcv, size_t row, size_t img) {
    const uint8_t *src = (const uint8_t *)srcv;
    if (!t->mem || unmapped(t)) { ONCE_RED("WriteTexels: texture has no host storage (%s)", t->mem ? "sparse pool" : "no pool memory"); return; }
    int bw, bh, bpb = nvn_fmt_bpb(t->d.format, &bw, &bh);
    if (!bpb) { ONCE_RED("WriteTexels: unknown format %#x", t->d.format); return; }
    unsigned level = v && v->set_levels ? (unsigned)v->base_level : 0, layer0 = v && v->set_layers ? (unsigned)v->min_layer : 0;
    int is3d = t->d.target == NVN_TEXTURE_TARGET_3D, nz = r->depth ? r->depth : 1;
    if (!is3d) {
        size_t wb0 = (size_t)((r->width + bw - 1) / bw) * bpb, row0 = row ? row : wb0, img0 = img ? img : row0 * (size_t)((r->height + bh - 1) / bh);
        int ok = 1;
        for (int z = 0; z < nz && ok; z++) ok = !write_region_bl(t, level, layer0 + (unsigned)(r->zoffset + z), r, src + (size_t)z * img0, row0, bw, bh, bpb);
        if (ok) { rp_tex_cpu_written(t); return; }
    }
    uint32_t lw = t->d.width >> level ? t->d.width >> level : 1, lh = t->d.height >> level ? t->d.height >> level : 1, lz = is3d ? (t->d.depth >> level ? t->d.depth >> level : 1) : 1;
    size_t lp = (size_t)((lw + bw - 1) / bw) * bpb, lr = (lh + bh - 1) / bh;
    size_t wb = (size_t)((r->width + bw - 1) / bw) * bpb, hb = (size_t)(r->height + bh - 1) / bh;
    if (!row) row = wb;
    if (!img) img = row * hb;
    uint8_t *lin = (uint8_t *)malloc(lp * lr * lz); if (!lin) fatal("WriteTexels allocation failed");
    // when the region is exactly the whole level (2D) every texel is overwritten, so the old content need not be decoded first (movies write whole frames; saves one deswizzle per 1080p frame)
    int whole = !is3d && !r->xoffset && !r->yoffset && (uint32_t)r->width == lw && (uint32_t)r->height == lh;
    for (int z = 0; z < nz; z++) {
        unsigned layer = is3d ? 0 : layer0 + (unsigned)(r->zoffset + z);
        if (((is3d && z == 0) || !is3d) && !whole) if (nvn_deswizzle(&t->d, level, layer, t->mem, lin, lp)) { ONCE_RED("WriteTexels deswizzle failed (format %#x target %d)", t->d.format, t->d.target); free(lin); return; }
        uint8_t *dst = lin + (is3d ? (size_t)(r->zoffset + z) * lp * lr : 0) + (size_t)(r->yoffset / bh) * lp + (size_t)(r->xoffset / bw) * bpb;
        for (size_t y = 0; y < hb; y++) memcpy(dst + y * lp, src + (size_t)z * img + y * row, wb);
        if (!is3d || z == nz - 1) nvn_swizzle(&t->d, level, layer, lin, lp, t->mem);
    }
    free(lin);
    rp_tex_cpu_written(t);
}
NVN(nvnTextureWriteTexels, void, const NVNtexture *t, const NVNtextureView *v, const NVNcopyRegion *r, const void *src) { write_texels(CTX(t), (const view_t *)v, r, src, 0, 0); }
// with the source's row and image strides in bytes (0 = tightly packed, as WriteTexels)
NVN(nvnTextureWriteTexelsStrided, void, const NVNtexture *t, const NVNtextureView *v, const NVNcopyRegion *r, const void *src, ptrdiff_t row, ptrdiff_t img) {
    write_texels(CTX(t), (const view_t *)v, r, src, (size_t)row, (size_t)img);
}
// GPU copies into a texture the host uploads from its storage (packaged textures: a packaged layout or CPU-written, and not
// resolved yet): the hardware's copy writes the storage, so the host writes the copied bytes there as well (tex_mirror_b2t / tex_mirror_t2t), and an upload from the storage later
// carries them. Yoshi's Crafted World's texture streamer re-creates a texture with more levels, copies the resident levels from the old texture on the GPU (Unreal's
// RHIAsyncReallocateTexture2D: nvnCommandBufferCopyTextureToTexture, a level at a time) and writes the new, finer levels from the CPU; uploaded from the storage, the copied levels held
// what the pool had there before (another texture's data, zeros), and the Yoshis' felt sampled speckles from the moment a level streamed in. −1 when the copy cannot be mirrored (3D,
// a format mismatch, a region out of range): the GPU copy alone then stands until the next upload
static int mirror_prepare(const tex_t *t, unsigned level, unsigned layer) {   // unprotect (and mark written) the storage of one level of one layer
    uint64_t lo = nvn_tex_level_offset(&t->d, level), hi = level + 1 < t->d.levels ? nvn_tex_level_offset(&t->d, level + 1) : t->L.layer_size;
    if (hi <= lo || hi > t->L.layer_size || (uint64_t)layer * t->L.layer_size + hi > t->L.size) return -1;
    return 0;
}
int tex_mirror_b2t(const tex_t *t, unsigned level, unsigned layer0, const NVNcopyRegion *r, const uint8_t *src, size_t row, size_t img) {
    int bw, bh, bpb = nvn_fmt_bpb(t->d.format, &bw, &bh);
    if (!t->mem || unmapped(t) || !bpb || t->d.target == NVN_TEXTURE_TARGET_3D || level >= t->d.levels) return -1;
    int nz = r->depth ? r->depth : 1;
    for (int z = 0; z < nz; z++) {
        unsigned layer = layer0 + (unsigned)(r->zoffset + z);
        if (mirror_prepare(t, level, layer) || write_region_bl(t, level, layer, r, src + (size_t)z * img, row, bw, bh, bpb)) return -1;
    }
    return 0;
}
int tex_mirror_t2t(const tex_t *s, unsigned sl, unsigned ss, const NVNcopyRegion *sr, const tex_t *d, unsigned dl, unsigned ds, const NVNcopyRegion *dr) {
    int bw, bh, bpb = nvn_fmt_bpb(s->d.format, &bw, &bh), dbw, dbh, dbpb = nvn_fmt_bpb(d->d.format, &dbw, &dbh);
    if (!s->mem || !d->mem || unmapped(s) || unmapped(d) || !bpb || bpb != dbpb || bw != dbw || bh != dbh || s->d.target == NVN_TEXTURE_TARGET_3D || d->d.target == NVN_TEXTURE_TARGET_3D) return -1;
    if (sl >= s->d.levels || dl >= d->d.levels || sr->width != dr->width || sr->height != dr->height || sr->xoffset < 0 || sr->yoffset < 0) return -1;
    uint32_t lw = s->d.width >> sl ? s->d.width >> sl : 1, lh = s->d.height >> sl ? s->d.height >> sl : 1;
    size_t lp = (size_t)((lw + bw - 1) / bw) * bpb, lr = (lh + bh - 1) / bh, x0 = (size_t)(sr->xoffset / bw) * bpb, y0 = (size_t)(sr->yoffset / bh);
    if (x0 + (size_t)((sr->width + bw - 1) / bw) * bpb > lp || y0 + (size_t)((sr->height + bh - 1) / bh) > lr) return -1;
    uint8_t *lin = (uint8_t *)malloc(lp * lr); if (!lin) fatal("texture copy mirror allocation failed");
    int nz = sr->depth ? sr->depth : 1, ok = 1;
    for (int z = 0; z < nz && ok; z++) {
        unsigned layer = ds + (unsigned)(dr->zoffset + z);
        ok = !nvn_deswizzle(&s->d, sl, ss + (unsigned)(sr->zoffset + z), s->mem, lin, lp) && !mirror_prepare(d, dl, layer) && !write_region_bl(d, dl, layer, dr, lin + y0 * lp + x0, lp, bw, bh, bpb);
    }
    free(lin);
    return ok ? 0 : -1;
}
NVN(nvnTextureFlushTexels, void, const NVNtexture *t, const NVNtextureView *v, const NVNcopyRegion *r) { (void)t; (void)v; (void)r; }   // unified memory + dirty page tracking: nothing to flush

}  // namespace nvn
