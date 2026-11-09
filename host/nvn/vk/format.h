// NVN texture formats (nvn.h NVNformat, 0x01-0x9C) as Vulkan formats.
#pragma once
#include "vkfn.h"
#include "../../common.h"

namespace nvk {

enum class FmtKind : u8 {
    None,        // no Vulkan counterpart yet (3-component RGB, signed RGB10A2)
    Exact,
    Substitute,  // same bits, read differently (integer-as-float *_UI2F / *_I2F read as integer)
    Astc,        // ASTC: the GPU has no ASTC, decoded on upload and recompressed to BC3 (UNORM or sRGB)
};

struct Format {
    VkFormat vk = VK_FORMAT_UNDEFINED;  // for Astc: the decoded format
    FmtKind kind = FmtKind::None;
    u8 block_w = 1, block_h = 1;        // of the NVN data (ASTC block size for Astc)
    u8 bytes = 0;                       // per block (per texel for uncompressed) of the NVN data
    bool depth = false, stencil = false;
    bool noalpha = false;               // alpha reads as 1 (RGBX, DXT1 without alpha)
};

const Format& format(u32 nvn_format);

}  // namespace nvk
