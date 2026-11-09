#include "format.h"

#include <array>

namespace nvk {
namespace {

struct Table {
    std::array<Format, 0x9D> f{};

    void set(u32 n, VkFormat vk, u8 bytes, FmtKind kind = FmtKind::Exact) {
        f[n].vk = vk;
        f[n].bytes = bytes;
        f[n].kind = kind;
    }
    void block(u32 n, VkFormat vk, u8 bw, u8 bh, u8 bytes, FmtKind kind = FmtKind::Exact) {
        set(n, vk, bytes, kind);
        f[n].block_w = bw;
        f[n].block_h = bh;
    }
    void noalpha(u32 n, VkFormat vk, u8 bytes) {
        set(n, vk, bytes);
        f[n].noalpha = true;
    }
    // A 3-component format: no Vulkan image format; the bytes per texel are still known.
    void none(u32 n, u8 bytes) { f[n].bytes = bytes; }

    Table() {
        constexpr auto S = FmtKind::Substitute;
        set(0x01, VK_FORMAT_R8_UNORM, 1); set(0x02, VK_FORMAT_R8_SNORM, 1); set(0x03, VK_FORMAT_R8_UINT, 1); set(0x04, VK_FORMAT_R8_SINT, 1);
        set(0x05, VK_FORMAT_R16_SFLOAT, 2); set(0x06, VK_FORMAT_R16_UNORM, 2); set(0x07, VK_FORMAT_R16_SNORM, 2);
        set(0x08, VK_FORMAT_R16_UINT, 2); set(0x09, VK_FORMAT_R16_SINT, 2);
        set(0x0A, VK_FORMAT_R32_SFLOAT, 4); set(0x0B, VK_FORMAT_R32_UINT, 4); set(0x0C, VK_FORMAT_R32_SINT, 4);
        set(0x0D, VK_FORMAT_R8G8_UNORM, 2); set(0x0E, VK_FORMAT_R8G8_SNORM, 2); set(0x0F, VK_FORMAT_R8G8_UINT, 2); set(0x10, VK_FORMAT_R8G8_SINT, 2);
        set(0x11, VK_FORMAT_R16G16_SFLOAT, 4); set(0x12, VK_FORMAT_R16G16_UNORM, 4); set(0x13, VK_FORMAT_R16G16_SNORM, 4);
        set(0x14, VK_FORMAT_R16G16_UINT, 4); set(0x15, VK_FORMAT_R16G16_SINT, 4);
        set(0x16, VK_FORMAT_R32G32_SFLOAT, 8); set(0x17, VK_FORMAT_R32G32_UINT, 8); set(0x18, VK_FORMAT_R32G32_SINT, 8);
        for (u32 n = 0x19; n <= 0x1C; n++) none(n, 3);
        for (u32 n = 0x1D; n <= 0x21; n++) none(n, 6);
        for (u32 n = 0x22; n <= 0x24; n++) none(n, 12);
        set(0x25, VK_FORMAT_R8G8B8A8_UNORM, 4); set(0x26, VK_FORMAT_R8G8B8A8_SNORM, 4); set(0x27, VK_FORMAT_R8G8B8A8_UINT, 4); set(0x28, VK_FORMAT_R8G8B8A8_SINT, 4);
        set(0x29, VK_FORMAT_R16G16B16A16_SFLOAT, 8); set(0x2A, VK_FORMAT_R16G16B16A16_UNORM, 8); set(0x2B, VK_FORMAT_R16G16B16A16_SNORM, 8);
        set(0x2C, VK_FORMAT_R16G16B16A16_UINT, 8); set(0x2D, VK_FORMAT_R16G16B16A16_SINT, 8);
        set(0x2E, VK_FORMAT_R32G32B32A32_SFLOAT, 16); set(0x2F, VK_FORMAT_R32G32B32A32_UINT, 16); set(0x30, VK_FORMAT_R32G32B32A32_SINT, 16);
        set(0x31, VK_FORMAT_S8_UINT, 1); f[0x31].stencil = true;
        set(0x32, VK_FORMAT_D16_UNORM, 2); f[0x32].depth = true;
        set(0x33, VK_FORMAT_X8_D24_UNORM_PACK32, 4); f[0x33].depth = true;
        set(0x34, VK_FORMAT_D32_SFLOAT, 4); f[0x34].depth = true;
        set(0x35, VK_FORMAT_D24_UNORM_S8_UINT, 4); f[0x35].depth = f[0x35].stencil = true;
        set(0x36, VK_FORMAT_D32_SFLOAT_S8_UINT, 8); f[0x36].depth = f[0x36].stencil = true;
        noalpha(0x37, VK_FORMAT_R8G8B8A8_SRGB, 4); set(0x38, VK_FORMAT_R8G8B8A8_SRGB, 4);
        // 16-bit packed: Tegra puts R in the low bits for the RGB names, B for the BGR names. Not
        // yet checked against real textures of this game.
        set(0x39, VK_FORMAT_A4B4G4R4_UNORM_PACK16, 2, S);
        noalpha(0x3A, VK_FORMAT_A1B5G5R5_UNORM_PACK16_KHR, 2); f[0x3A].kind = S;
        set(0x3B, VK_FORMAT_A1B5G5R5_UNORM_PACK16_KHR, 2, S);
        set(0x3C, VK_FORMAT_B5G6R5_UNORM_PACK16, 2, S);
        set(0x3D, VK_FORMAT_A2B10G10R10_UNORM_PACK32, 4); set(0x3E, VK_FORMAT_A2B10G10R10_UINT_PACK32, 4);
        set(0x3F, VK_FORMAT_B10G11R11_UFLOAT_PACK32, 4); set(0x40, VK_FORMAT_E5B9G9R9_UFLOAT_PACK32, 4);
        block(0x41, VK_FORMAT_BC1_RGB_UNORM_BLOCK, 4, 4, 8); f[0x41].noalpha = true;
        block(0x42, VK_FORMAT_BC1_RGBA_UNORM_BLOCK, 4, 4, 8);
        block(0x43, VK_FORMAT_BC2_UNORM_BLOCK, 4, 4, 16);
        block(0x44, VK_FORMAT_BC3_UNORM_BLOCK, 4, 4, 16);
        block(0x45, VK_FORMAT_BC1_RGB_SRGB_BLOCK, 4, 4, 8); f[0x45].noalpha = true;
        block(0x46, VK_FORMAT_BC1_RGBA_SRGB_BLOCK, 4, 4, 8);
        block(0x47, VK_FORMAT_BC2_SRGB_BLOCK, 4, 4, 16);
        block(0x48, VK_FORMAT_BC3_SRGB_BLOCK, 4, 4, 16);
        block(0x49, VK_FORMAT_BC4_UNORM_BLOCK, 4, 4, 8); block(0x4A, VK_FORMAT_BC4_SNORM_BLOCK, 4, 4, 8);
        block(0x4B, VK_FORMAT_BC5_UNORM_BLOCK, 4, 4, 16); block(0x4C, VK_FORMAT_BC5_SNORM_BLOCK, 4, 4, 16);
        block(0x4D, VK_FORMAT_BC7_UNORM_BLOCK, 4, 4, 16); block(0x4E, VK_FORMAT_BC7_SRGB_BLOCK, 4, 4, 16);
        block(0x4F, VK_FORMAT_BC6H_SFLOAT_BLOCK, 4, 4, 16); block(0x50, VK_FORMAT_BC6H_UFLOAT_BLOCK, 4, 4, 16);
        // Integer storage read as float (no normalisation): as images, the integer format (the
        // shader converts); as vertex data the *SCALED formats (vertex.cpp).
        set(0x51, VK_FORMAT_R8_UINT, 1, S); set(0x52, VK_FORMAT_R8_SINT, 1, S);
        set(0x53, VK_FORMAT_R16_UINT, 2, S); set(0x54, VK_FORMAT_R16_SINT, 2, S);
        set(0x55, VK_FORMAT_R32_UINT, 4, S); set(0x56, VK_FORMAT_R32_SINT, 4, S);
        set(0x57, VK_FORMAT_R8G8_UINT, 2, S); set(0x58, VK_FORMAT_R8G8_SINT, 2, S);
        set(0x59, VK_FORMAT_R16G16_UINT, 4, S); set(0x5A, VK_FORMAT_R16G16_SINT, 4, S);
        set(0x5B, VK_FORMAT_R32G32_UINT, 8, S); set(0x5C, VK_FORMAT_R32G32_SINT, 8, S);
        none(0x5D, 3); none(0x5E, 3); none(0x5F, 6); none(0x60, 6); none(0x61, 12); none(0x62, 12);
        set(0x63, VK_FORMAT_R8G8B8A8_UINT, 4, S); set(0x64, VK_FORMAT_R8G8B8A8_SINT, 4, S);
        set(0x65, VK_FORMAT_R16G16B16A16_UINT, 8, S); set(0x66, VK_FORMAT_R16G16B16A16_SINT, 8, S);
        set(0x67, VK_FORMAT_R32G32B32A32_UINT, 16, S); set(0x68, VK_FORMAT_R32G32B32A32_SINT, 16, S);
        set(0x69, VK_FORMAT_A2B10G10R10_SNORM_PACK32, 4); set(0x6A, VK_FORMAT_A2B10G10R10_SINT_PACK32, 4);
        set(0x6B, VK_FORMAT_A2B10G10R10_UINT_PACK32, 4, S); set(0x6C, VK_FORMAT_A2B10G10R10_SINT_PACK32, 4, S);
        noalpha(0x6D, VK_FORMAT_R8G8B8A8_UNORM, 4); noalpha(0x6E, VK_FORMAT_R8G8B8A8_SNORM, 4);
        noalpha(0x6F, VK_FORMAT_R8G8B8A8_UINT, 4); noalpha(0x70, VK_FORMAT_R8G8B8A8_SINT, 4);
        noalpha(0x71, VK_FORMAT_R16G16B16A16_SFLOAT, 8); noalpha(0x72, VK_FORMAT_R16G16B16A16_UNORM, 8);
        noalpha(0x73, VK_FORMAT_R16G16B16A16_SNORM, 8); noalpha(0x74, VK_FORMAT_R16G16B16A16_UINT, 8);
        noalpha(0x75, VK_FORMAT_R16G16B16A16_SINT, 8);
        noalpha(0x76, VK_FORMAT_R32G32B32A32_SFLOAT, 16); noalpha(0x77, VK_FORMAT_R32G32B32A32_UINT, 16);
        noalpha(0x78, VK_FORMAT_R32G32B32A32_SINT, 16);
        static constexpr u8 astc[14][2] = {{4, 4}, {5, 4}, {5, 5}, {6, 5}, {6, 6}, {8, 5}, {8, 6},
                                           {8, 8}, {10, 5}, {10, 6}, {10, 8}, {10, 10}, {12, 10}, {12, 12}};
        for (u32 k = 0; k < 14; k++) {
            // decoded on the CPU and recompressed to BC3 (1 byte a texel: RGBA8 would take 4 to 16 times the ASTC size)
            block(0x79 + k, VK_FORMAT_BC3_UNORM_BLOCK, astc[k][0], astc[k][1], 16, FmtKind::Astc);
            block(0x87 + k, VK_FORMAT_BC3_SRGB_BLOCK, astc[k][0], astc[k][1], 16, FmtKind::Astc);
        }
        set(0x95, VK_FORMAT_R5G6B5_UNORM_PACK16, 2, S);
        noalpha(0x96, VK_FORMAT_A1R5G5B5_UNORM_PACK16, 2); f[0x96].kind = S;
        set(0x97, VK_FORMAT_A1R5G5B5_UNORM_PACK16, 2, S);
        set(0x98, VK_FORMAT_B5G5R5A1_UNORM_PACK16, 2, S);
        noalpha(0x99, VK_FORMAT_B8G8R8A8_UNORM, 4); set(0x9A, VK_FORMAT_B8G8R8A8_UNORM, 4);
        noalpha(0x9B, VK_FORMAT_B8G8R8A8_SRGB, 4); set(0x9C, VK_FORMAT_B8G8R8A8_SRGB, 4);
    }
};

}  // namespace

const Format& format(u32 nvn_format) {
    static const Table t;
    static const Format none{};
    return nvn_format < t.f.size() ? t.f[nvn_format] : none;
}

}  // namespace nvk
