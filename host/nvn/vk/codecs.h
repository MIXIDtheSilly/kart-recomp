// Texture codecs for formats the host GPU lacks: ASTC decode (astc.cpp, from Eden) and BC1/BC3 encode (bcn.cpp, stb_dxt
// blocks), spread over the transcode threads.
#pragma once
#include <functional>
#include <span>

#include "../../common.h"

namespace codec {

// ASTC blocks (16 bytes each, depth slices of rows) to RGBA8, width * height * depth * 4 bytes.
void astc_decode(std::span<const u8> data, u32 width, u32 height, u32 depth, u32 block_width, u32 block_height,
                 std::span<u8> output);
// True if no 16-byte block in `data` decodes to alpha below 1.
bool astc_opaque(std::span<const u8> data);

// RGBA8 to BC1 (8 bytes per 4x4 block; alpha below 128 becomes transparent black) or BC3 (16 bytes per block).
void bc1_encode(std::span<const u8> data, u32 width, u32 height, u32 depth, std::span<u8> output);
void bc3_encode(std::span<const u8> data, u32 width, u32 height, u32 depth, std::span<u8> output);

// fn(0) .. fn(n - 1) on the transcode threads and the calling thread; returns when all have run. A quarter of the
// hardware threads (at least 2), or HOST_TRANSCODE_THREADS.
void parallel_for(u32 n, const std::function<void(u32)>& fn);

}  // namespace codec
