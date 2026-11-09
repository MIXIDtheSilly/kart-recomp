// NVN texture storage layout (Tegra X1 / Switch 2 block-linear): size, alignment and level offsets of a texture,
// and the swizzle / deswizzle of its texels. A C++ port of nxnative's nvn_tex.c + nvn_deswizzle.c (MIT);
// the layout algorithm is from deko3d source/dk_image.cpp (zlib license, Copyright (C) 2018-2020 fincs).
#pragma once
#include <cstddef>
#include <cstdint>

struct nvn_texdesc {
    int target, format, flags, samples;   // NVNtextureTarget / NVNformat / NVNtextureFlags / sample count (0 or 1 = single-sampled)
    uint32_t width, height, depth, levels, stride;
    int pkg_block_h;                      // Packaged byte 0 (low nibble log2 block height, high nibble log2 block depth) + 1; 0 = use the automatic algorithm
};

struct nvn_texlayout {
    uint64_t size, layer_size;            // Total storage bytes, bytes per layer
    uint32_t align, layers, bpb;          // Alignment, layer count, bytes per block
    unsigned tileH, tileD;                // Block height / block depth (log2 of the GOB count)
};

void nvn_tex_layout(const nvn_texdesc* d, nvn_texlayout* o);
uint64_t nvn_tex_level_offset(const nvn_texdesc* d, unsigned level);
uint32_t nvn_tex_level_gobs(const nvn_texdesc* d, unsigned level);   // GOB columns level `level` is laid out with (padded to whole 64 KB tiles in a sparse texture's tiled levels)
int nvn_fmt_bpb(int format, int* bw, int* bh);   // Bytes per block (0 = unknown format), block width and height
// Block height / block depth actually used by level `level` (log2 of the GOB count): the per-level block height shrinks with that level's row count
void nvn_tex_level_tiles(const nvn_texdesc* d, unsigned level, unsigned* tileH, unsigned* tileD);
// A sparse texture's tiled levels (the levels before its mip tail) and one 64 KB tile in texels; 0 and no tile for a texture that is not sparse
int nvn_tex_sparse_tiles(const nvn_texdesc* d, uint32_t* tw, uint32_t* th, uint32_t* td);
// A texture buffer of n elements of bpb bytes read as 2D rows of NVN_TBUF_ROW texels (the translator's wrapped 1D fetch): width, height and bytes per row of the
// linear 2D texture over the same memory. 0 when n or bpb is 0
#define NVN_TBUF_ROW 16384u
int nvn_tbuf_rows(uint64_t n, uint32_t bpb, uint32_t align, uint32_t* w, uint32_t* h, uint32_t* row);

// ---- Block-linear <-> linear (nvn_deswizzle.c). Level / layer offsets and per-level block height / depth come from the layout above.
// Decode level `level`, layer `layer` (0 for non-layered textures) from the whole-texture storage into linear dst_linear, dst_pitch bytes per block row.
// 3D textures: all depth slices of the level at once, slice k at dst_linear + k * dst_pitch * rows. -1 for multisampled, LINEAR and BUFFER textures, 0 on success.
int nvn_deswizzle(const nvn_texdesc* d, unsigned level, unsigned layer, const void* src_storage, void* dst_linear, size_t dst_pitch);
int nvn_swizzle(const nvn_texdesc* d, unsigned level, unsigned layer, const void* src_linear, size_t src_pitch, void* dst_storage);
// The order of 16-byte pieces inside a GOB: 0 the Tegra X1's, 1 the Switch 2's (bits 5 and 6 of the offset exchanged). Set at DeviceInitialize
// (this game is a Switch 2 title: always 1). Measured on Donkey Kong Bananza's BNTX textures by nxnative.
extern int nvn_gob_switch2;
// Byte offset of (x byte, y row, z slice) within a level and layer.
uint64_t nvn_bl_offset(uint32_t x, uint32_t y, uint32_t z, uint32_t row_bytes, uint32_t rows, unsigned tileH, unsigned tileD);
int nvn_deswizzle_geom(const nvn_texdesc* d, unsigned level, unsigned layer, uint32_t* row_bytes, uint32_t* rows, uint32_t* slices,
                       uint64_t* base, unsigned* tileH, unsigned* tileD, uint32_t* bpb);
