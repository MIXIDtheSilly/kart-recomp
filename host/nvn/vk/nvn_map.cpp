// NVN -> Vulkan state mapping (see nvn_map.h).
#include "nvn_map.h"

#include <algorithm>

namespace nvk {

VkBlendFactor blend_factor(u32 v) {
    switch (v) {
    case NVN_BLEND_FUNC_ZERO: return VK_BLEND_FACTOR_ZERO;
    case NVN_BLEND_FUNC_ONE: return VK_BLEND_FACTOR_ONE;
    case NVN_BLEND_FUNC_SRC_COLOR: return VK_BLEND_FACTOR_SRC_COLOR;
    case NVN_BLEND_FUNC_ONE_MINUS_SRC_COLOR: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
    case NVN_BLEND_FUNC_SRC_ALPHA: return VK_BLEND_FACTOR_SRC_ALPHA;
    case NVN_BLEND_FUNC_ONE_MINUS_SRC_ALPHA: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case NVN_BLEND_FUNC_DST_ALPHA: return VK_BLEND_FACTOR_DST_ALPHA;
    case NVN_BLEND_FUNC_ONE_MINUS_DST_ALPHA: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    case NVN_BLEND_FUNC_DST_COLOR: return VK_BLEND_FACTOR_DST_COLOR;
    case NVN_BLEND_FUNC_ONE_MINUS_DST_COLOR: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
    case NVN_BLEND_FUNC_SRC_ALPHA_SATURATE: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
    case NVN_BLEND_FUNC_SRC1_COLOR: return VK_BLEND_FACTOR_SRC1_COLOR;
    case NVN_BLEND_FUNC_ONE_MINUS_SRC1_COLOR: return VK_BLEND_FACTOR_ONE_MINUS_SRC1_COLOR;
    case NVN_BLEND_FUNC_SRC1_ALPHA: return VK_BLEND_FACTOR_SRC1_ALPHA;
    case NVN_BLEND_FUNC_ONE_MINUS_SRC1_ALPHA: return VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA;
    case NVN_BLEND_FUNC_CONSTANT_COLOR: return VK_BLEND_FACTOR_CONSTANT_COLOR;
    case NVN_BLEND_FUNC_ONE_MINUS_CONSTANT_COLOR: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
    case NVN_BLEND_FUNC_CONSTANT_ALPHA: return VK_BLEND_FACTOR_CONSTANT_ALPHA;
    case NVN_BLEND_FUNC_ONE_MINUS_CONSTANT_ALPHA: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
    default: return VK_BLEND_FACTOR_ONE;
    }
}

bool blend_factor_uses_src1(u32 v) { return v >= NVN_BLEND_FUNC_SRC1_COLOR && v <= NVN_BLEND_FUNC_ONE_MINUS_SRC1_ALPHA; }

VkBlendOp blend_op(u32 v) {
    switch (v) {
    case NVN_BLEND_EQUATION_ADD: return VK_BLEND_OP_ADD;
    case NVN_BLEND_EQUATION_SUB: return VK_BLEND_OP_SUBTRACT;
    case NVN_BLEND_EQUATION_REVERSE_SUB: return VK_BLEND_OP_REVERSE_SUBTRACT;
    case NVN_BLEND_EQUATION_MIN: return VK_BLEND_OP_MIN;
    case NVN_BLEND_EQUATION_MAX: return VK_BLEND_OP_MAX;
    default: return VK_BLEND_OP_ADD;
    }
}

bool blend_equation(const nvn::blend_st& b, VkColorBlendEquationEXT* out, bool* advanced, bool* dual_src) {
    const VkBlendOp co = blend_op((u32)b.eq_rgb), ao = blend_op((u32)b.eq_a);
    const bool cminmax = co == VK_BLEND_OP_MIN || co == VK_BLEND_OP_MAX, aminmax = ao == VK_BLEND_OP_MIN || ao == VK_BLEND_OP_MAX;
    out->colorBlendOp = co;
    out->alphaBlendOp = ao;
    out->srcColorBlendFactor = cminmax ? VK_BLEND_FACTOR_ONE : blend_factor((u32)b.src_rgb);
    out->dstColorBlendFactor = cminmax ? VK_BLEND_FACTOR_ONE : blend_factor((u32)b.dst_rgb);
    out->srcAlphaBlendFactor = aminmax ? VK_BLEND_FACTOR_ONE : blend_factor((u32)b.src_a);
    out->dstAlphaBlendFactor = aminmax ? VK_BLEND_FACTOR_ONE : blend_factor((u32)b.dst_a);
    if (dual_src)
        *dual_src = (!cminmax && (blend_factor_uses_src1((u32)b.src_rgb) || blend_factor_uses_src1((u32)b.dst_rgb))) ||
                    (!aminmax && (blend_factor_uses_src1((u32)b.src_a) || blend_factor_uses_src1((u32)b.dst_a)));
    const bool adv = b.adv != NVN_BLEND_ADVANCED_MODE_NONE;
    if (advanced) *advanced = adv;
    return !adv;
}

VkLogicOp logic_op(u32 v, bool* is_copy) {
    if (is_copy) *is_copy = (v == NVN_LOGIC_OP_COPY) || v > NVN_LOGIC_OP_SET;
    switch (v) {
    case NVN_LOGIC_OP_CLEAR: return VK_LOGIC_OP_CLEAR;
    case NVN_LOGIC_OP_AND: return VK_LOGIC_OP_AND;
    case NVN_LOGIC_OP_AND_REVERSE: return VK_LOGIC_OP_AND_REVERSE;
    case NVN_LOGIC_OP_COPY: return VK_LOGIC_OP_COPY;
    case NVN_LOGIC_OP_AND_INVERTED: return VK_LOGIC_OP_AND_INVERTED;
    case NVN_LOGIC_OP_NOOP: return VK_LOGIC_OP_NO_OP;
    case NVN_LOGIC_OP_XOR: return VK_LOGIC_OP_XOR;
    case NVN_LOGIC_OP_OR: return VK_LOGIC_OP_OR;
    case NVN_LOGIC_OP_NOR: return VK_LOGIC_OP_NOR;
    case NVN_LOGIC_OP_EQUIV: return VK_LOGIC_OP_EQUIVALENT;
    case NVN_LOGIC_OP_INVERT: return VK_LOGIC_OP_INVERT;
    case NVN_LOGIC_OP_OR_REVERSE: return VK_LOGIC_OP_OR_REVERSE;
    case NVN_LOGIC_OP_COPY_INVERTED: return VK_LOGIC_OP_COPY_INVERTED;
    case NVN_LOGIC_OP_OR_INVERTED: return VK_LOGIC_OP_OR_INVERTED;
    case NVN_LOGIC_OP_NAND: return VK_LOGIC_OP_NAND;
    case NVN_LOGIC_OP_SET: return VK_LOGIC_OP_SET;
    default: return VK_LOGIC_OP_COPY;
    }
}

// The three enums share the numbering NEVER = 1 ... ALWAYS = 8.
static VkCompareOp compare_op(u32 v, VkCompareOp dflt) {
    switch (v) {
    case NVN_COMPARE_FUNC_NEVER: return VK_COMPARE_OP_NEVER;
    case NVN_COMPARE_FUNC_LESS: return VK_COMPARE_OP_LESS;
    case NVN_COMPARE_FUNC_EQUAL: return VK_COMPARE_OP_EQUAL;
    case NVN_COMPARE_FUNC_LEQUAL: return VK_COMPARE_OP_LESS_OR_EQUAL;
    case NVN_COMPARE_FUNC_GREATER: return VK_COMPARE_OP_GREATER;
    case NVN_COMPARE_FUNC_NOTEQUAL: return VK_COMPARE_OP_NOT_EQUAL;
    case NVN_COMPARE_FUNC_GEQUAL: return VK_COMPARE_OP_GREATER_OR_EQUAL;
    case NVN_COMPARE_FUNC_ALWAYS: return VK_COMPARE_OP_ALWAYS;
    default: return dflt;
    }
}
VkCompareOp compare_op_depth(u32 s) { return compare_op(s, VK_COMPARE_OP_LESS); }
VkCompareOp compare_op_stencil(u32 s) { return compare_op(s, VK_COMPARE_OP_ALWAYS); }
VkCompareOp compare_op_sampler(u32 v) { return compare_op(v, VK_COMPARE_OP_ALWAYS); }

VkStencilOp stencil_op(u32 v) {
    switch (v) {
    case NVN_STENCIL_OP_KEEP: return VK_STENCIL_OP_KEEP;
    case NVN_STENCIL_OP_ZERO: return VK_STENCIL_OP_ZERO;
    case NVN_STENCIL_OP_REPLACE: return VK_STENCIL_OP_REPLACE;
    case NVN_STENCIL_OP_INCR: return VK_STENCIL_OP_INCREMENT_AND_CLAMP;
    case NVN_STENCIL_OP_DECR: return VK_STENCIL_OP_DECREMENT_AND_CLAMP;
    case NVN_STENCIL_OP_INVERT: return VK_STENCIL_OP_INVERT;
    case NVN_STENCIL_OP_INCR_WRAP: return VK_STENCIL_OP_INCREMENT_AND_WRAP;
    case NVN_STENCIL_OP_DECR_WRAP: return VK_STENCIL_OP_DECREMENT_AND_WRAP;
    default: return VK_STENCIL_OP_KEEP;
    }
}

VkCullModeFlags cull_mode(u32 v) {
    switch (v) {
    case NVN_FACE_FRONT: return VK_CULL_MODE_FRONT_BIT;
    case NVN_FACE_BACK: return VK_CULL_MODE_BACK_BIT;
    case NVN_FACE_FRONT_AND_BACK: return VK_CULL_MODE_FRONT_AND_BACK;
    default: return VK_CULL_MODE_NONE;
    }
}

VkFrontFace front_face(u32 v) { return v == NVN_FRONT_FACE_CW ? VK_FRONT_FACE_CLOCKWISE : VK_FRONT_FACE_COUNTER_CLOCKWISE; }

VkPolygonMode polygon_mode(u32 v) {
    switch (v) {
    case NVN_POLYGON_MODE_POINT: return VK_POLYGON_MODE_POINT;
    case NVN_POLYGON_MODE_LINE: return VK_POLYGON_MODE_LINE;
    default: return VK_POLYGON_MODE_FILL;
    }
}

Topology topology(u32 v) {
    Topology t{VK_PRIMITIVE_TOPOLOGY_POINT_LIST, true, 0, false, false, false, false};
    switch (v) {
    case NVN_DRAW_PRIMITIVE_POINTS: t.topo = VK_PRIMITIVE_TOPOLOGY_POINT_LIST; break;
    case NVN_DRAW_PRIMITIVE_LINES: t.topo = VK_PRIMITIVE_TOPOLOGY_LINE_LIST; break;
    case NVN_DRAW_PRIMITIVE_LINE_LOOP: t.topo = VK_PRIMITIVE_TOPOLOGY_LINE_STRIP; t.line_loop = true; break;
    case NVN_DRAW_PRIMITIVE_LINE_STRIP: t.topo = VK_PRIMITIVE_TOPOLOGY_LINE_STRIP; break;
    case NVN_DRAW_PRIMITIVE_TRIANGLES: t.topo = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST; break;
    case NVN_DRAW_PRIMITIVE_TRIANGLE_STRIP: t.topo = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP; break;
    case NVN_DRAW_PRIMITIVE_TRIANGLE_FAN: t.topo = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN; t.fan = true; break;
    case NVN_DRAW_PRIMITIVE_QUADS: t.topo = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST; t.quads = true; break;
    case NVN_DRAW_PRIMITIVE_QUAD_STRIP: t.topo = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST; t.quad_strip = true; break;
    case NVN_DRAW_PRIMITIVE_POLYGON: t.topo = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN; t.fan = true; break;   // a GL polygon is a fan
    case NVN_DRAW_PRIMITIVE_LINES_ADJACENCY: t.topo = VK_PRIMITIVE_TOPOLOGY_LINE_LIST_WITH_ADJACENCY; break;
    case NVN_DRAW_PRIMITIVE_LINE_STRIP_ADJACENCY: t.topo = VK_PRIMITIVE_TOPOLOGY_LINE_STRIP_WITH_ADJACENCY; break;
    case NVN_DRAW_PRIMITIVE_TRIANGLES_ADJACENCY: t.topo = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST_WITH_ADJACENCY; break;
    case NVN_DRAW_PRIMITIVE_TRIANGLE_STRIP_ADJACENCY: t.topo = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP_WITH_ADJACENCY; break;
    case NVN_DRAW_PRIMITIVE_PATCHES: t.topo = VK_PRIMITIVE_TOPOLOGY_PATCH_LIST; break;
    default: t.supported = false; break;
    }
    return t;
}

VkIndexType index_type(u32 v, bool* needs_uint8) {
    if (needs_uint8) *needs_uint8 = v == NVN_INDEX_TYPE_UNSIGNED_BYTE;
    switch (v) {
    case NVN_INDEX_TYPE_UNSIGNED_BYTE: return VK_INDEX_TYPE_UINT8;
    case NVN_INDEX_TYPE_UNSIGNED_INT: return VK_INDEX_TYPE_UINT32;
    default: return VK_INDEX_TYPE_UINT16;
    }
}
u32 index_size(u32 v) { return v == NVN_INDEX_TYPE_UNSIGNED_BYTE ? 1 : v == NVN_INDEX_TYPE_UNSIGNED_INT ? 4 : 2; }

VkFormat vertex_format(u32 f, bool* exact) {
    bool ex = true;
    VkFormat r = VK_FORMAT_UNDEFINED;
    switch (f) {
    // 8-bit
    case NVN_FORMAT_R8: r = VK_FORMAT_R8_UNORM; break;
    case NVN_FORMAT_R8SN: r = VK_FORMAT_R8_SNORM; break;
    case NVN_FORMAT_R8UI: r = VK_FORMAT_R8_UINT; break;
    case NVN_FORMAT_R8I: r = VK_FORMAT_R8_SINT; break;
    case NVN_FORMAT_R8_UI2F: r = VK_FORMAT_R8_USCALED; break;
    case NVN_FORMAT_R8_I2F: r = VK_FORMAT_R8_SSCALED; break;
    case NVN_FORMAT_RG8: r = VK_FORMAT_R8G8_UNORM; break;
    case NVN_FORMAT_RG8SN: r = VK_FORMAT_R8G8_SNORM; break;
    case NVN_FORMAT_RG8UI: r = VK_FORMAT_R8G8_UINT; break;
    case NVN_FORMAT_RG8I: r = VK_FORMAT_R8G8_SINT; break;
    case NVN_FORMAT_RG8_UI2F: r = VK_FORMAT_R8G8_USCALED; break;
    case NVN_FORMAT_RG8_I2F: r = VK_FORMAT_R8G8_SSCALED; break;
    case NVN_FORMAT_RGB8: r = VK_FORMAT_R8G8B8_UNORM; break;
    case NVN_FORMAT_RGB8SN: r = VK_FORMAT_R8G8B8_SNORM; break;
    case NVN_FORMAT_RGB8UI: r = VK_FORMAT_R8G8B8_UINT; break;
    case NVN_FORMAT_RGB8I: r = VK_FORMAT_R8G8B8_SINT; break;
    case NVN_FORMAT_RGB8_UI2F: r = VK_FORMAT_R8G8B8_USCALED; break;
    case NVN_FORMAT_RGB8_I2F: r = VK_FORMAT_R8G8B8_SSCALED; break;
    case NVN_FORMAT_RGBX8: case NVN_FORMAT_RGBA8: r = VK_FORMAT_R8G8B8A8_UNORM; ex = f == NVN_FORMAT_RGBA8; break;
    case NVN_FORMAT_RGBX8SN: case NVN_FORMAT_RGBA8SN: r = VK_FORMAT_R8G8B8A8_SNORM; ex = f == NVN_FORMAT_RGBA8SN; break;
    case NVN_FORMAT_RGBX8UI: case NVN_FORMAT_RGBA8UI: r = VK_FORMAT_R8G8B8A8_UINT; ex = f == NVN_FORMAT_RGBA8UI; break;
    case NVN_FORMAT_RGBX8I: case NVN_FORMAT_RGBA8I: r = VK_FORMAT_R8G8B8A8_SINT; ex = f == NVN_FORMAT_RGBA8I; break;
    case NVN_FORMAT_RGBA8_UI2F: r = VK_FORMAT_R8G8B8A8_USCALED; break;
    case NVN_FORMAT_RGBA8_I2F: r = VK_FORMAT_R8G8B8A8_SSCALED; break;
    case NVN_FORMAT_BGRA8: case NVN_FORMAT_BGRX8: r = VK_FORMAT_B8G8R8A8_UNORM; ex = f == NVN_FORMAT_BGRA8; break;
    case NVN_FORMAT_RGBA8_SRGB: case NVN_FORMAT_RGBX8_SRGB: r = VK_FORMAT_R8G8B8A8_SRGB; ex = f == NVN_FORMAT_RGBA8_SRGB; break;
    case NVN_FORMAT_BGRA8_SRGB: case NVN_FORMAT_BGRX8_SRGB: r = VK_FORMAT_B8G8R8A8_SRGB; ex = f == NVN_FORMAT_BGRA8_SRGB; break;
    // 16-bit
    case NVN_FORMAT_R16F: r = VK_FORMAT_R16_SFLOAT; break;
    case NVN_FORMAT_R16: r = VK_FORMAT_R16_UNORM; break;
    case NVN_FORMAT_R16SN: r = VK_FORMAT_R16_SNORM; break;
    case NVN_FORMAT_R16UI: r = VK_FORMAT_R16_UINT; break;
    case NVN_FORMAT_R16I: r = VK_FORMAT_R16_SINT; break;
    case NVN_FORMAT_R16_UI2F: r = VK_FORMAT_R16_USCALED; break;
    case NVN_FORMAT_R16_I2F: r = VK_FORMAT_R16_SSCALED; break;
    case NVN_FORMAT_RG16F: r = VK_FORMAT_R16G16_SFLOAT; break;
    case NVN_FORMAT_RG16: r = VK_FORMAT_R16G16_UNORM; break;
    case NVN_FORMAT_RG16SN: r = VK_FORMAT_R16G16_SNORM; break;
    case NVN_FORMAT_RG16UI: r = VK_FORMAT_R16G16_UINT; break;
    case NVN_FORMAT_RG16I: r = VK_FORMAT_R16G16_SINT; break;
    case NVN_FORMAT_RG16_UI2F: r = VK_FORMAT_R16G16_USCALED; break;
    case NVN_FORMAT_RG16_I2F: r = VK_FORMAT_R16G16_SSCALED; break;
    case NVN_FORMAT_RGB16F: r = VK_FORMAT_R16G16B16_SFLOAT; break;
    case NVN_FORMAT_RGB16: r = VK_FORMAT_R16G16B16_UNORM; break;
    case NVN_FORMAT_RGB16SN: r = VK_FORMAT_R16G16B16_SNORM; break;
    case NVN_FORMAT_RGB16UI: r = VK_FORMAT_R16G16B16_UINT; break;
    case NVN_FORMAT_RGB16I: r = VK_FORMAT_R16G16B16_SINT; break;
    case NVN_FORMAT_RGB16_UI2F: r = VK_FORMAT_R16G16B16_USCALED; break;
    case NVN_FORMAT_RGB16_I2F: r = VK_FORMAT_R16G16B16_SSCALED; break;
    case NVN_FORMAT_RGBA16F: case NVN_FORMAT_RGBX16F: r = VK_FORMAT_R16G16B16A16_SFLOAT; ex = f == NVN_FORMAT_RGBA16F; break;
    case NVN_FORMAT_RGBA16: case NVN_FORMAT_RGBX16: r = VK_FORMAT_R16G16B16A16_UNORM; ex = f == NVN_FORMAT_RGBA16; break;
    case NVN_FORMAT_RGBA16SN: case NVN_FORMAT_RGBX16SN: r = VK_FORMAT_R16G16B16A16_SNORM; ex = f == NVN_FORMAT_RGBA16SN; break;
    case NVN_FORMAT_RGBA16UI: case NVN_FORMAT_RGBX16UI: r = VK_FORMAT_R16G16B16A16_UINT; ex = f == NVN_FORMAT_RGBA16UI; break;
    case NVN_FORMAT_RGBA16I: case NVN_FORMAT_RGBX16I: r = VK_FORMAT_R16G16B16A16_SINT; ex = f == NVN_FORMAT_RGBA16I; break;
    case NVN_FORMAT_RGBA16_UI2F: r = VK_FORMAT_R16G16B16A16_USCALED; break;
    case NVN_FORMAT_RGBA16_I2F: r = VK_FORMAT_R16G16B16A16_SSCALED; break;
    // 32-bit (Vulkan has no 32-bit scaled formats: UI2F / I2F map to UINT / SINT and the shader must convert)
    case NVN_FORMAT_R32F: r = VK_FORMAT_R32_SFLOAT; break;
    case NVN_FORMAT_R32UI: r = VK_FORMAT_R32_UINT; break;
    case NVN_FORMAT_R32I: r = VK_FORMAT_R32_SINT; break;
    case NVN_FORMAT_R32_UI2F: r = VK_FORMAT_R32_UINT; ex = false; break;
    case NVN_FORMAT_R32_I2F: r = VK_FORMAT_R32_SINT; ex = false; break;
    case NVN_FORMAT_RG32F: r = VK_FORMAT_R32G32_SFLOAT; break;
    case NVN_FORMAT_RG32UI: r = VK_FORMAT_R32G32_UINT; break;
    case NVN_FORMAT_RG32I: r = VK_FORMAT_R32G32_SINT; break;
    case NVN_FORMAT_RG32_UI2F: r = VK_FORMAT_R32G32_UINT; ex = false; break;
    case NVN_FORMAT_RG32_I2F: r = VK_FORMAT_R32G32_SINT; ex = false; break;
    case NVN_FORMAT_RGB32F: r = VK_FORMAT_R32G32B32_SFLOAT; break;
    case NVN_FORMAT_RGB32UI: r = VK_FORMAT_R32G32B32_UINT; break;
    case NVN_FORMAT_RGB32I: r = VK_FORMAT_R32G32B32_SINT; break;
    case NVN_FORMAT_RGB32_UI2F: r = VK_FORMAT_R32G32B32_UINT; ex = false; break;
    case NVN_FORMAT_RGB32_I2F: r = VK_FORMAT_R32G32B32_SINT; ex = false; break;
    case NVN_FORMAT_RGBA32F: case NVN_FORMAT_RGBX32F: r = VK_FORMAT_R32G32B32A32_SFLOAT; ex = f == NVN_FORMAT_RGBA32F; break;
    case NVN_FORMAT_RGBA32UI: case NVN_FORMAT_RGBX32UI: r = VK_FORMAT_R32G32B32A32_UINT; ex = f == NVN_FORMAT_RGBA32UI; break;
    case NVN_FORMAT_RGBA32I: case NVN_FORMAT_RGBX32I: r = VK_FORMAT_R32G32B32A32_SINT; ex = f == NVN_FORMAT_RGBA32I; break;
    case NVN_FORMAT_RGBA32_UI2F: r = VK_FORMAT_R32G32B32A32_UINT; ex = false; break;
    case NVN_FORMAT_RGBA32_I2F: r = VK_FORMAT_R32G32B32A32_SINT; ex = false; break;
    // packed 32-bit (NVN RGB10A2: R in the low 10 bits = A2B10G10R10 in Vulkan)
    case NVN_FORMAT_RGB10A2: r = VK_FORMAT_A2B10G10R10_UNORM_PACK32; break;
    case NVN_FORMAT_RGB10A2SN: r = VK_FORMAT_A2B10G10R10_SNORM_PACK32; break;
    case NVN_FORMAT_RGB10A2UI: r = VK_FORMAT_A2B10G10R10_UINT_PACK32; break;
    case NVN_FORMAT_RGB10A2I: r = VK_FORMAT_A2B10G10R10_SINT_PACK32; break;
    case NVN_FORMAT_RGB10A2_UI2F: r = VK_FORMAT_A2B10G10R10_USCALED_PACK32; break;
    case NVN_FORMAT_RGB10A2_I2F: r = VK_FORMAT_A2B10G10R10_SSCALED_PACK32; break;
    case NVN_FORMAT_R11G11B10F: r = VK_FORMAT_B10G11R11_UFLOAT_PACK32; break;
    // packed 16-bit: not vertex formats in practice, logical image-format equivalents
    case NVN_FORMAT_RGBA4: r = VK_FORMAT_R4G4B4A4_UNORM_PACK16; ex = false; break;
    case NVN_FORMAT_RGB565: r = VK_FORMAT_R5G6B5_UNORM_PACK16; ex = false; break;
    case NVN_FORMAT_RGB5A1: r = VK_FORMAT_R5G5B5A1_UNORM_PACK16; ex = false; break;
    case NVN_FORMAT_BGR565: r = VK_FORMAT_B5G6R5_UNORM_PACK16; ex = false; break;
    case NVN_FORMAT_BGR5A1: r = VK_FORMAT_B5G5R5A1_UNORM_PACK16; ex = false; break;
    case NVN_FORMAT_A1BGR5: r = VK_FORMAT_A1B5G5R5_UNORM_PACK16; ex = false; break;
    default: ex = false; break;
    }
    if (exact) *exact = ex;
    return r;
}

// ---- Sampler ----
VkSamplerAddressMode wrap_mode(u32 v, bool* exact) {
    if (exact) *exact = v != NVN_WRAP_MODE_MIRROR_CLAMP_TO_BORDER;
    switch (v) {
    case NVN_WRAP_MODE_CLAMP: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    case NVN_WRAP_MODE_REPEAT: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
    case NVN_WRAP_MODE_MIRROR_CLAMP:
    case NVN_WRAP_MODE_MIRROR_CLAMP_TO_EDGE: return VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE;
    case NVN_WRAP_MODE_MIRROR_CLAMP_TO_BORDER:
    case NVN_WRAP_MODE_CLAMP_TO_BORDER: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    case NVN_WRAP_MODE_MIRRORED_REPEAT: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
    case NVN_WRAP_MODE_CLAMP_TO_EDGE: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    default: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
    }
}

void SamplerDesc::link() {
    const void** tail = &ci.pNext;
    *tail = nullptr;
    if (use_red) { red.pNext = nullptr; *tail = &red; tail = &red.pNext; }
    if (use_border) { border.pNext = nullptr; *tail = &border; }
}
SamplerDesc::SamplerDesc() : ci{}, red{}, border{} {
    ci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    red.sType = VK_STRUCTURE_TYPE_SAMPLER_REDUCTION_MODE_CREATE_INFO;
    border.sType = VK_STRUCTURE_TYPE_SAMPLER_CUSTOM_BORDER_COLOR_CREATE_INFO_EXT;
}
SamplerDesc::SamplerDesc(const SamplerDesc& o)
    : ci(o.ci), red(o.red), border(o.border), use_red(o.use_red), use_border(o.use_border), border_mirror_unsupported(o.border_mirror_unsupported) {
    link();
}
SamplerDesc& SamplerDesc::operator=(const SamplerDesc& o) {
    ci = o.ci; red = o.red; border = o.border; use_red = o.use_red; use_border = o.use_border; border_mirror_unsupported = o.border_mirror_unsupported;
    link();
    return *this;
}

SamplerDesc sampler_desc(const nvn::samp_par& p) {
    SamplerDesc d;
    VkSamplerCreateInfo& c = d.ci;

    // Min filter: the mip filter is part of the NVN enum. Without mipmapping only level 0 may be sampled; Vulkan has no "no mip" mode,
    // so NEAREST mip mode with maxLod clamped to 0.25 (the usual trick) stays on level 0.
    bool mips = true;
    switch (p.minf) {
    case NVN_MIN_FILTER_NEAREST: c.minFilter = VK_FILTER_NEAREST; c.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST; mips = false; break;
    case NVN_MIN_FILTER_LINEAR: c.minFilter = VK_FILTER_LINEAR; c.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST; mips = false; break;
    case NVN_MIN_FILTER_NEAREST_MIPMAP_NEAREST: c.minFilter = VK_FILTER_NEAREST; c.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST; break;
    case NVN_MIN_FILTER_LINEAR_MIPMAP_NEAREST: c.minFilter = VK_FILTER_LINEAR; c.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST; break;
    case NVN_MIN_FILTER_NEAREST_MIPMAP_LINEAR: c.minFilter = VK_FILTER_NEAREST; c.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR; break;
    case NVN_MIN_FILTER_LINEAR_MIPMAP_LINEAR: c.minFilter = VK_FILTER_LINEAR; c.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR; break;
    default: c.minFilter = VK_FILTER_LINEAR; c.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR; break;
    }
    c.magFilter = p.magf == NVN_MAG_FILTER_NEAREST ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;

    bool ex = true;
    c.addressModeU = wrap_mode(p.ws, &ex); d.border_mirror_unsupported |= !ex;
    c.addressModeV = wrap_mode(p.wt, &ex); d.border_mirror_unsupported |= !ex;
    c.addressModeW = wrap_mode(p.wr, &ex); d.border_mirror_unsupported |= !ex;

    c.mipLodBias = p.lod_bias;
    c.minLod = p.lod_min;
    c.maxLod = p.lod_max;
    if (!mips) { c.minLod = std::min(c.minLod, 0.0f); c.maxLod = 0.25f; }
    if (c.maxLod < c.minLod) c.maxLod = c.minLod;

    c.compareEnable = p.cmp_mode == NVN_COMPARE_MODE_COMPARE_R_TO_TEXTURE ? VK_TRUE : VK_FALSE;
    c.compareOp = compare_op_sampler(p.cmp_func);

    c.anisotropyEnable = p.aniso > 1.0f ? VK_TRUE : VK_FALSE;
    c.maxAnisotropy = c.anisotropyEnable ? std::min(p.aniso, 16.0f) : 1.0f;

    // Border color: only matters with a border wrap mode. Standard colors when exactly representable.
    const bool border_wrap = c.addressModeU == VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER || c.addressModeV == VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER ||
                             c.addressModeW == VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    const float* b = p.border;
    c.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    if (border_wrap) {
        if (b[0] == 0.f && b[1] == 0.f && b[2] == 0.f && b[3] == 0.f) c.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
        else if (b[0] == 0.f && b[1] == 0.f && b[2] == 0.f && b[3] == 1.f) c.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
        else if (b[0] == 1.f && b[1] == 1.f && b[2] == 1.f && b[3] == 1.f) c.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
        else {
            c.borderColor = VK_BORDER_COLOR_FLOAT_CUSTOM_EXT;
            d.use_border = true;
            for (int i = 0; i < 4; i++) d.border.customBorderColor.float32[i] = b[i];
            d.border.format = VK_FORMAT_UNDEFINED;
        }
    }
    c.unnormalizedCoordinates = VK_FALSE;

    if (p.reduction == NVN_SAMPLER_REDUCTION_MIN || p.reduction == NVN_SAMPLER_REDUCTION_MAX) {
        d.use_red = true;
        d.red.reductionMode = p.reduction == NVN_SAMPLER_REDUCTION_MIN ? VK_SAMPLER_REDUCTION_MODE_MIN : VK_SAMPLER_REDUCTION_MODE_MAX;
    }
    d.link();
    return d;
}

VkSampleCountFlagBits sample_count(u32 n) {
    switch (n) {
    case 2: return VK_SAMPLE_COUNT_2_BIT;
    case 4: return VK_SAMPLE_COUNT_4_BIT;
    case 8: return VK_SAMPLE_COUNT_8_BIT;
    case 16: return VK_SAMPLE_COUNT_16_BIT;
    case 32: return VK_SAMPLE_COUNT_32_BIT;
    case 64: return VK_SAMPLE_COUNT_64_BIT;
    default: return VK_SAMPLE_COUNT_1_BIT;
    }
}

}  // namespace nvk
