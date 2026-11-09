// NVN -> Vulkan state mapping. Every function maps by the raw numeric NVN enum value, because the interface layer
// (state.cpp / sampler.cpp) stores the values as the game passed them, in the bit-fields of nvn_obj.h (nothing is stored
// as value-1). Unknown values map to a safe default; where the caller may care, a flag reports it.
#pragma once
#include <cstdint>

#include "vkfn.h"
#include "../nvn_obj.h"

namespace nvk {

using u32 = uint32_t;

// ---- Blend ----
// NVNblendFunc (ZERO = 1 ... SRC_ALPHA_SATURATE = 0xB, SRC1_* = 0x10..0x13, CONSTANT_* = 0x61..0x64). Unknown -> ONE.
VkBlendFactor blend_factor(u32 nvn);
bool blend_factor_uses_src1(u32 nvn);        // dual-source factor: needs the dualSrcBlend feature
// NVNblendEquation (ADD = 1, SUB = 2, REVERSE_SUB = 3, MIN = 4, MAX = 5). Unknown -> ADD.
VkBlendOp blend_op(u32 nvn);
// Fills the equation of one attachment from a blend_st. MIN/MAX ignore the factors in both APIs; they are written as ONE/ONE.
// Returns false when the state selects an NVN advanced blend mode (blend_st::adv != 0; the hardware then ignores the
// blend functions, and this renderer does not implement the mode): the fixed-function equation is still filled and *advanced is set.
// *dual_src is set when a src1 factor is used.
bool blend_equation(const nvn::blend_st& b, VkColorBlendEquationEXT* out, bool* advanced = nullptr, bool* dual_src = nullptr);

// NVNlogicOp (CLEAR = 0 ... SET = 0xF, same order as VkLogicOp). Default COPY. *is_copy: COPY means "logic op disabled".
VkLogicOp logic_op(u32 nvn, bool* is_copy = nullptr);

// ---- Compare ----
// NVNdepthFunc / NVNstencilFunc / NVNcompareFunc are all NEVER = 1 ... ALWAYS = 8, stored as given. Unknown -> LESS (depth),
// ALWAYS (stencil, sampler).
VkCompareOp compare_op_depth(u32 stored);
VkCompareOp compare_op_stencil(u32 stored);
VkCompareOp compare_op_sampler(u32 nvn);
// NVNstencilOp (KEEP = 1, ZERO, REPLACE, INCR (clamp), DECR (clamp), INVERT, INCR_WRAP, DECR_WRAP = 8). Unknown -> KEEP.
VkStencilOp stencil_op(u32 stored);

// ---- Rasterizer ----
// poly_st::cull holds NVNface (NONE 0, FRONT 1, BACK 2, FRONT_AND_BACK 3), the same bits as VkCullModeFlags.
VkCullModeFlags cull_mode(u32 stored);
// poly_st::front holds NVNfrontFace (CW = 0, CCW = 1). With an upper-left origin and a positive viewport height Vulkan's
// winding equals GL's when the projection is unflipped; whether to flip (negative-height viewport or inverted front face)
// is the caller's decision.
VkFrontFace front_face(u32 stored);
// poly_st::mode holds NVNpolygonMode (POINT 0, LINE 1, FILL 2). Unknown -> FILL.
VkPolygonMode polygon_mode(u32 stored);

// ---- Primitives ----
struct Topology {
    VkPrimitiveTopology topo;
    bool supported;        // false: no mapping at all (unknown value)
    u32 patch_vertices;    // PATCHES: 0, the count comes from SetPatchSize
    bool quads;            // QUADS: draw as triangle list through a generated index buffer (0 1 2, 0 2 3 per quad)
    bool quad_strip;       // QUAD_STRIP: triangle list through a generated index buffer (quad i = v2i v2i+1 v2i+3 v2i+2)
    bool fan;              // TRIANGLE_FAN and POLYGON: topo is TRIANGLE_FAN (needs triangleFans where VK_KHR_portability_subset applies)
    bool line_loop;        // LINE_LOOP: topo is LINE_STRIP, the caller must close the loop (extra index / vertex)
};
// NVNdrawPrimitive: POINTS 0, LINES 1, LINE_LOOP 2, LINE_STRIP 3, TRIANGLES 4, TRIANGLE_STRIP 5, TRIANGLE_FAN 6, QUADS 7,
// QUAD_STRIP 8, POLYGON 9, LINES_ADJACENCY 0xA, LINE_STRIP_ADJACENCY 0xB, TRIANGLES_ADJACENCY 0xC, TRIANGLE_STRIP_ADJACENCY 0xD, PATCHES 0xE.
// Adjacency topologies need the geometryShader feature and patches the tessellationShader feature (not checked here).
Topology topology(u32 nvn_draw_primitive);

// NVNindexType: UNSIGNED_BYTE 0, UNSIGNED_SHORT 1, UNSIGNED_INT 2. *needs_uint8 is set for the byte type, which requires
// VK_INDEX_TYPE_UINT8 (VK_EXT/KHR_index_type_uint8) or expansion to 16 bits by the caller. Unknown -> UINT16.
VkIndexType index_type(u32 nvn, bool* needs_uint8 = nullptr);
u32 index_size(u32 nvn);   // bytes: 1, 2, 4 (unknown -> 2)

// ---- Vertex attributes ----
// va_st::fmt holds an NVNformat. Returns the logical Vulkan vertex format, VK_FORMAT_UNDEFINED for formats that are not vertex
// formats. *exact is false when Vulkan has no format with the same bit layout / conversion and the returned one is the closest
// (32-bit *_UI2F / *_I2F -> UINT / SINT: the shader must convert; packed 16-bit formats; the X variants RGBX*, BGRX*
// are treated as their A counterparts).
VkFormat vertex_format(u32 nvn_format, bool* exact = nullptr);

// ---- Sampler ----
// The pNext chain points into the struct itself. After a raw memcpy / move call link(); the copy constructor and assignment
// relink for you.
struct SamplerDesc {
    VkSamplerCreateInfo ci;
    VkSamplerReductionModeCreateInfo red;
    VkSamplerCustomBorderColorCreateInfoEXT border;
    bool use_red = false, use_border = false;
    bool border_mirror_unsupported = false;   // MIRROR_CLAMP_TO_BORDER was requested; mapped to CLAMP_TO_BORDER
    SamplerDesc();
    SamplerDesc(const SamplerDesc& o);
    SamplerDesc& operator=(const SamplerDesc& o);
    void link();   // rebuilds ci.pNext -> red -> border according to use_red / use_border
};
// Builds the create info. The custom border color (and so use_border) is only used when a wrap mode is CLAMP_TO_BORDER and the
// color is not one of the three standard ones. Without mipmapping minFilter (NEAREST / LINEAR) maxLod is clamped to 0.25.
SamplerDesc sampler_desc(const nvn::samp_par& p);
// NVNwrapMode: CLAMP 0 (legacy GL clamp -> CLAMP_TO_EDGE), REPEAT 1, MIRROR_CLAMP 2 / MIRROR_CLAMP_TO_EDGE 3 (-> MIRROR_CLAMP_TO_EDGE),
// MIRROR_CLAMP_TO_BORDER 4 (no Vulkan equivalent -> CLAMP_TO_BORDER, *exact=false), CLAMP_TO_BORDER 5, MIRRORED_REPEAT 6, CLAMP_TO_EDGE 7.
VkSamplerAddressMode wrap_mode(u32 nvn, bool* exact = nullptr);

// Sample count from an integer sample count (ms_st::samples). 0 -> 1 sample; non powers of two -> 1.
VkSampleCountFlagBits sample_count(u32 n);

}  // namespace nvk
