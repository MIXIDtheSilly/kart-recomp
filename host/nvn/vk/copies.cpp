// Texture copies of the command stream: buffer -> texture (OP_COPY_B2T), texture -> buffer (OP_COPY_T2B),
// texture -> texture (OP_COPY_T2T, also multisample resolves), with the copy strides (OP_COPY_STRIDE).
#include "copies.h"

#include <span>
#include <vector>

#include "../nvn_int.h"
#include "draw.h"
#include "dump.h"
#include "format.h"
#include "pools.h"
#include "replay_int.h"
#include "textures.h"
#include "video_core/textures/astc.h"
#include "video_core/textures/bcn.h"
#include "../../watch.h"

namespace nvk {
namespace {

s64 g_row_stride = 0, g_img_stride = 0;

struct Sub {
    u32 level = 0, layer = 0;
};
Sub sub_of(const Image* img, const nvn::view_t* v, bool has) {
    Sub s;
    if (has && v->set_levels) s.level = std::min<u32>(v->base_level, img->levels - 1);
    if (has && v->set_layers) s.layer = v->min_layer;
    return s;
}

// A temporary buffer the GPU reads, freed once the submission completes.
bool staging(VkDeviceSize n, VkBuffer* buf, void** ptr) {
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = std::max<VkDeviceSize>(n, 16);
    bi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    VmaAllocationCreateInfo ai{};
    ai.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST;
    ai.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
    VmaAllocation alloc;
    VmaAllocationInfo info;
    if (vmaCreateBuffer(ctx().vma, &bi, &ai, buf, &alloc, &info) != VK_SUCCESS) return false;
    *ptr = info.pMappedData;
    VkBuffer b = *buf;
    defer([b, alloc] { vmaDestroyBuffer(ctx().vma, b, alloc); });
    return true;
}

// A buffer of the GPU's own, freed once the submission completes.
bool device_buffer(VkDeviceSize n, VkBufferUsageFlags usage, VkBuffer* buf) {
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = n;
    bi.usage = usage;
    VmaAllocationCreateInfo ai{};
    ai.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    VmaAllocation alloc;
    if (vmaCreateBuffer(ctx().vma, &bi, &ai, buf, &alloc, nullptr) != VK_SUCCESS) return false;
    defer([b = *buf, alloc] { vmaDestroyBuffer(ctx().vma, b, alloc); });
    return true;
}

// #version 450
// layout(local_size_x = 256) in;
// layout(std430, binding = 0) readonly buffer Src { uint src[]; };
// layout(std430, binding = 1) writeonly buffer Dst { uint dst[]; };
// layout(push_constant) uniform Push { uint blocks; };
// void main() {
//     const uint i = gl_GlobalInvocationID.x;
//     if (i >= blocks) return;
//     dst[i * 2] = src[i * 4 + 2];
//     dst[i * 2 + 1] = src[i * 4 + 3];
// }
const u32 kStripSpv[] = {
    0x07230203,0x00010500,0x0008000b,0x0000003e,0x00000000,0x00020011,0x00000001,0x0006000b,0x00000001,0x4c534c47,
    0x6474732e,0x3035342e,0x00000000,0x0003000e,0x00000000,0x00000001,0x0009000f,0x00000005,0x00000004,0x6e69616d,
    0x00000000,0x0000000b,0x00000013,0x00000021,0x00000028,0x00060010,0x00000004,0x00000011,0x00000100,0x00000001,
    0x00000001,0x00030003,0x00000002,0x000001c2,0x00040005,0x00000004,0x6e69616d,0x00000000,0x00030005,0x00000008,
    0x00000069,0x00080005,0x0000000b,0x475f6c67,0x61626f6c,0x766e496c,0x7461636f,0x496e6f69,0x00000044,0x00040005,
    0x00000011,0x68737550,0x00000000,0x00050006,0x00000011,0x00000000,0x636f6c62,0x0000736b,0x00030005,0x00000013,
    0x00000000,0x00030005,0x0000001f,0x00747344,0x00040006,0x0000001f,0x00000000,0x00747364,0x00030005,0x00000021,
    0x00000000,0x00030005,0x00000026,0x00637253,0x00040006,0x00000026,0x00000000,0x00637273,0x00030005,0x00000028,
    0x00000000,0x00040047,0x0000000b,0x0000000b,0x0000001c,0x00030047,0x00000011,0x00000002,0x00050048,0x00000011,
    0x00000000,0x00000023,0x00000000,0x00040047,0x0000001e,0x00000006,0x00000004,0x00030047,0x0000001f,0x00000002,
    0x00040048,0x0000001f,0x00000000,0x00000019,0x00050048,0x0000001f,0x00000000,0x00000023,0x00000000,0x00030047,
    0x00000021,0x00000019,0x00040047,0x00000021,0x00000021,0x00000001,0x00040047,0x00000021,0x00000022,0x00000000,
    0x00040047,0x00000025,0x00000006,0x00000004,0x00030047,0x00000026,0x00000002,0x00040048,0x00000026,0x00000000,
    0x00000018,0x00050048,0x00000026,0x00000000,0x00000023,0x00000000,0x00030047,0x00000028,0x00000018,0x00040047,
    0x00000028,0x00000021,0x00000000,0x00040047,0x00000028,0x00000022,0x00000000,0x00040047,0x0000003d,0x0000000b,
    0x00000019,0x00020013,0x00000002,0x00030021,0x00000003,0x00000002,0x00040015,0x00000006,0x00000020,0x00000000,
    0x00040020,0x00000007,0x00000007,0x00000006,0x00040017,0x00000009,0x00000006,0x00000003,0x00040020,0x0000000a,
    0x00000001,0x00000009,0x0004003b,0x0000000a,0x0000000b,0x00000001,0x0004002b,0x00000006,0x0000000c,0x00000000,
    0x00040020,0x0000000d,0x00000001,0x00000006,0x0003001e,0x00000011,0x00000006,0x00040020,0x00000012,0x00000009,
    0x00000011,0x0004003b,0x00000012,0x00000013,0x00000009,0x00040015,0x00000014,0x00000020,0x00000001,0x0004002b,
    0x00000014,0x00000015,0x00000000,0x00040020,0x00000016,0x00000009,0x00000006,0x00020014,0x00000019,0x0003001d,
    0x0000001e,0x00000006,0x0003001e,0x0000001f,0x0000001e,0x00040020,0x00000020,0x0000000c,0x0000001f,0x0004003b,
    0x00000020,0x00000021,0x0000000c,0x0004002b,0x00000006,0x00000023,0x00000002,0x0003001d,0x00000025,0x00000006,
    0x0003001e,0x00000026,0x00000025,0x00040020,0x00000027,0x0000000c,0x00000026,0x0004003b,0x00000027,0x00000028,
    0x0000000c,0x0004002b,0x00000006,0x0000002a,0x00000004,0x00040020,0x0000002d,0x0000000c,0x00000006,0x0004002b,
    0x00000006,0x00000033,0x00000001,0x0004002b,0x00000006,0x00000037,0x00000003,0x0004002b,0x00000006,0x0000003c,
    0x00000100,0x0006002c,0x00000009,0x0000003d,0x0000003c,0x00000033,0x00000033,0x00050036,0x00000002,0x00000004,
    0x00000000,0x00000003,0x000200f8,0x00000005,0x0004003b,0x00000007,0x00000008,0x00000007,0x00050041,0x0000000d,
    0x0000000e,0x0000000b,0x0000000c,0x0004003d,0x00000006,0x0000000f,0x0000000e,0x0003003e,0x00000008,0x0000000f,
    0x0004003d,0x00000006,0x00000010,0x00000008,0x00050041,0x00000016,0x00000017,0x00000013,0x00000015,0x0004003d,
    0x00000006,0x00000018,0x00000017,0x000500ae,0x00000019,0x0000001a,0x00000010,0x00000018,0x000300f7,0x0000001c,
    0x00000000,0x000400fa,0x0000001a,0x0000001b,0x0000001c,0x000200f8,0x0000001b,0x000100fd,0x000200f8,0x0000001c,
    0x0004003d,0x00000006,0x00000022,0x00000008,0x00050084,0x00000006,0x00000024,0x00000022,0x00000023,0x0004003d,
    0x00000006,0x00000029,0x00000008,0x00050084,0x00000006,0x0000002b,0x00000029,0x0000002a,0x00050080,0x00000006,
    0x0000002c,0x0000002b,0x00000023,0x00060041,0x0000002d,0x0000002e,0x00000028,0x00000015,0x0000002c,0x0004003d,
    0x00000006,0x0000002f,0x0000002e,0x00060041,0x0000002d,0x00000030,0x00000021,0x00000015,0x00000024,0x0003003e,
    0x00000030,0x0000002f,0x0004003d,0x00000006,0x00000031,0x00000008,0x00050084,0x00000006,0x00000032,0x00000031,
    0x00000023,0x00050080,0x00000006,0x00000034,0x00000032,0x00000033,0x0004003d,0x00000006,0x00000035,0x00000008,
    0x00050084,0x00000006,0x00000036,0x00000035,0x0000002a,0x00050080,0x00000006,0x00000038,0x00000036,0x00000037,
    0x00060041,0x0000002d,0x00000039,0x00000028,0x00000015,0x00000038,0x0004003d,0x00000006,0x0000003a,0x00000039,
    0x00060041,0x0000002d,0x0000003b,0x00000021,0x00000015,0x00000034,0x0003003e,0x0000003b,0x0000003a,0x000100fd,
    0x00010038,
};

VkPipelineLayout g_strip_layout = VK_NULL_HANDLE;
VkPipeline g_strip = VK_NULL_HANDLE;

bool strip_pipeline() {
    if (g_strip) return true;
    Device& c = ctx();
    VkDescriptorSetLayoutBinding b[2]{};
    for (u32 i = 0; i < 2; i++) b[i].binding = i, b[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, b[i].descriptorCount = 1, b[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    VkDescriptorSetLayoutCreateInfo di{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    di.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
    di.bindingCount = 2, di.pBindings = b;
    VkDescriptorSetLayout set;
    check(vkCreateDescriptorSetLayout(c.dev, &di, nullptr, &set), "vkCreateDescriptorSetLayout (BC3 to BC1)");
    VkPushConstantRange pc{VK_SHADER_STAGE_COMPUTE_BIT, 0, 4};
    VkPipelineLayoutCreateInfo li{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    li.setLayoutCount = 1, li.pSetLayouts = &set;
    li.pushConstantRangeCount = 1, li.pPushConstantRanges = &pc;
    check(vkCreatePipelineLayout(c.dev, &li, nullptr, &g_strip_layout), "vkCreatePipelineLayout (BC3 to BC1)");
    VkShaderModuleCreateInfo mi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    mi.codeSize = sizeof kStripSpv, mi.pCode = kStripSpv;
    VkShaderModule m;
    check(vkCreateShaderModule(c.dev, &mi, nullptr, &m), "vkCreateShaderModule (BC3 to BC1)");
    VkComputePipelineCreateInfo pi{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    pi.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, m, "main"};
    pi.layout = g_strip_layout;
    check(vkCreateComputePipelines(c.dev, VK_NULL_HANDLE, 1, &pi, nullptr, &g_strip), "vkCreateComputePipelines (BC3 to BC1)");
    vkDestroyShaderModule(c.dev, m, nullptr);
    return true;
}

// A region of a BC3 texture into a BC1 one (sparse.cpp stores the terrain caches as BC1, their sources are BC3): the second
// half of a BC3 block is a BC1 block, so the colours stay exactly as they are. On the GPU: the sources are textures, not bytes.
void copy_bc3_to_bc1(VkCommandBuffer cb, Image* si, Image* di, const VkImageSubresourceLayers& sl, const VkImageSubresourceLayers& dl,
                     const NVNcopyRegion& sr, const NVNcopyRegion& dr) {
    if (sl.layerCount != 1 || dl.layerCount != 1 || ((sr.xoffset | sr.yoffset | dr.xoffset | dr.yoffset) & 3)) {
        ONCE_RED("BC3 to BC1 copy of several layers or not on whole blocks: skipped");
        return;
    }
    const u32 sw = std::max(si->width >> sl.mipLevel, 1u), sh = std::max(si->height >> sl.mipLevel, 1u);
    const u32 dw = std::max(di->width >> dl.mipLevel, 1u), dh = std::max(di->height >> dl.mipLevel, 1u);
    if (static_cast<u32>(sr.xoffset) >= sw || static_cast<u32>(sr.yoffset) >= sh || static_cast<u32>(dr.xoffset) >= dw || static_cast<u32>(dr.yoffset) >= dh) return;
    // Whole blocks, within both textures
    const u32 w = std::min({(static_cast<u32>(sr.width) + 3) & ~3u, sw - sr.xoffset, dw - dr.xoffset});
    const u32 h = std::min({(static_cast<u32>(sr.height) + 3) & ~3u, sh - sr.yoffset, dh - dr.yoffset});
    const u32 bx = (w + 3) / 4, by = (h + 3) / 4;
    const VkDeviceSize blocks = static_cast<VkDeviceSize>(bx) * by;
    VkBuffer a, b;
    if (!strip_pipeline() || !device_buffer(blocks * 16, VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, &a) ||
        !device_buffer(blocks * 8, VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, &b)) {
        ONCE_RED("no memory for a BC3 to BC1 copy: skipped");
        return;
    }
    VkBufferImageCopy r{};
    r.bufferRowLength = bx * 4, r.bufferImageHeight = by * 4;
    r.imageSubresource = sl;
    r.imageOffset = {sr.xoffset, sr.yoffset, 0};
    r.imageExtent = {w, h, 1};
    vkCmdCopyImageToBuffer(cb, si->image, VK_IMAGE_LAYOUT_GENERAL, a, 1, &r);
    full_barrier(cb);
    const VkDescriptorBufferInfo infos[2]{{a, 0, VK_WHOLE_SIZE}, {b, 0, VK_WHOLE_SIZE}};
    VkWriteDescriptorSet wr[2]{};
    for (u32 i = 0; i < 2; i++) {
        wr[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        wr[i].dstBinding = i, wr[i].descriptorCount = 1;
        wr[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        wr[i].pBufferInfo = &infos[i];
    }
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, g_strip);
    vkCmdPushDescriptorSetKHR(cb, VK_PIPELINE_BIND_POINT_COMPUTE, g_strip_layout, 0, 2, wr);
    draw_compute_changed();
    const u32 n = static_cast<u32>(blocks);
    vkCmdPushConstants(cb, g_strip_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, 4, &n);
    vkCmdDispatch(cb, (n + 255) / 256, 1, 1);
    full_barrier(cb);
    r.imageSubresource = dl;
    r.imageOffset = {dr.xoffset, dr.yoffset, 0};
    vkCmdCopyBufferToImage(cb, b, di->image, VK_IMAGE_LAYOUT_GENERAL, 1, &r);
    full_barrier(cb);
    di->gpu_written = true;
}

VkImageAspectFlags copy_aspect(const Image* img) {
    if (img->aspect == (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) return VK_IMAGE_ASPECT_DEPTH_BIT;
    return img->aspect;
}

}  // namespace

void copy_stride(const nvn::p_copy_stride* c) {
    if (c->image) g_img_stride = c->stride;
    else g_row_stride = c->stride;
}

void copy_buffer_texture(VkCommandBuffer cb, const nvn::p_copy_b2t* c, bool to_texture) {
    if (!c->tex) return;
    end_rendering(cb);
    Image* img = image_of(c->tex, draw_pre_cb());
    if (!img) return;
    draw_ensure_content(img, c->tex);
    if (img->samples != VK_SAMPLE_COUNT_1_BIT) {
        ONCE_RED("copy between a buffer and a multisample texture: skipped");
        return;
    }
    const nvn::view_t* v = c->has_view ? &c->view : nullptr;
    const u32 fmt = v && v->set_format && v->format ? v->format : img->nvn_format;
    const Format& f = format(fmt);
    int bw = 1, bh = 1;
    const int bpb = ::nvn_fmt_bpb(static_cast<int>(fmt), &bw, &bh);
    if (!bpb) return;
    const Sub s = sub_of(img, v, c->has_view);
    const NVNcopyRegion& r = c->r;
    if (r.width <= 0 || r.height <= 0) return;
    const bool is3d = img->type == VK_IMAGE_TYPE_3D;
    const u32 depth = static_cast<u32>(std::max(r.depth, 1));
    const u32 bx = (static_cast<u32>(r.width) + bw - 1) / bw, by = (static_cast<u32>(r.height) + bh - 1) / bh;
    const u64 row = g_row_stride > 0 ? static_cast<u64>(g_row_stride) : static_cast<u64>(bx) * bpb;
    const u64 slice = g_img_stride > 0 ? static_cast<u64>(g_img_stride) : row * by;
    const u64 bytes = slice * (depth - 1) + row * (by - 1) + static_cast<u64>(bx) * bpb;

    VkBufferImageCopy bic{};
    bic.imageSubresource = {copy_aspect(img), s.level, is3d ? 0 : s.layer + static_cast<u32>(r.zoffset), is3d ? 1 : depth};
    bic.imageOffset = {r.xoffset, r.yoffset, is3d ? r.zoffset : 0};
    bic.imageExtent = {static_cast<u32>(r.width), static_cast<u32>(r.height), is3d ? depth : 1};
    const u32 lw = std::max(img->width >> s.level, 1u), lh = std::max(img->height >> s.level, 1u);
    bic.imageExtent.width = std::min<u32>(bic.imageExtent.width, lw - std::min<u32>(lw, static_cast<u32>(r.xoffset)));
    bic.imageExtent.height = std::min<u32>(bic.imageExtent.height, lh - std::min<u32>(lh, static_cast<u32>(r.yoffset)));
    if (!bic.imageExtent.width || !bic.imageExtent.height) return;
    dump_note("copy %s buffer %#llx: image %llu fmt %#x level %u layer %u region %d,%d %ux%u row %llu", to_texture ? "from" : "to",
              static_cast<unsigned long long>(c->src), static_cast<unsigned long long>(img->serial), fmt, s.level,
              bic.imageSubresource.baseArrayLayer, r.xoffset, r.yoffset, bic.imageExtent.width, bic.imageExtent.height,
              static_cast<unsigned long long>(row));
    if (img->sparse) {
        static int n = 0;
        if (n++ < 16)
            LOG("nvk: copy %s buffer %#llx: sparse image %llu fmt %#x level %u layer %u region %d,%d %ux%u row %llu", to_texture ? "from" : "to",
                static_cast<unsigned long long>(c->src), static_cast<unsigned long long>(img->serial), fmt, s.level,
                bic.imageSubresource.baseArrayLayer, r.xoffset, r.yoffset, bic.imageExtent.width, bic.imageExtent.height,
                static_cast<unsigned long long>(row));
    }

    if (f.kind == FmtKind::Astc || format(img->nvn_format).kind == FmtKind::Astc) {
        if (!to_texture) { ONCE_RED("copy from an ASTC texture to a buffer: skipped"); return; }
        // ASTC is decoded on the CPU from the bytes as they are now.
        std::vector<u8> blocks(static_cast<size_t>(bx) * bpb * by * depth);
        for (u32 z = 0; z < depth; z++)
            for (u32 y = 0; y < by; y++)
                nvn::gpu_read(c->src + z * slice + y * row, blocks.data() + (static_cast<size_t>(z) * by + y) * bx * bpb, static_cast<u64>(bx) * bpb);
        const u32 w = bx * bw, h = by * bh;
        if ((r.xoffset | r.yoffset) & 3) {
            ONCE_RED("ASTC copy into a texture at %d,%d (not BC3 block aligned): skipped", r.xoffset, r.yoffset);
            return;
        }
        std::vector<u8> rgba(static_cast<size_t>(w) * h * depth * 4);
        Tegra::Texture::ASTC::Decompress(std::span<const u8>(blocks), w, h, depth, static_cast<u32>(bw), static_cast<u32>(bh), std::span<u8>(rgba));
        const bool bc1 = img->format == VK_FORMAT_BC1_RGBA_SRGB_BLOCK;
        const size_t bc = static_cast<size_t>((w + 3) / 4) * ((h + 3) / 4) * depth * (bc1 ? 8 : 16);
        VkBuffer buf;
        void* ptr;
        if (!staging(bc, &buf, &ptr)) return;
        if (bc1) Tegra::Texture::BCN::CompressBC1(std::span<const u8>(rgba), w, h, depth, std::span<u8>(static_cast<u8*>(ptr), bc));
        else Tegra::Texture::BCN::CompressBC3(std::span<const u8>(rgba), w, h, depth, std::span<u8>(static_cast<u8*>(ptr), bc));
        bic.bufferRowLength = (w + 3) & ~3u, bic.bufferImageHeight = (h + 3) & ~3u;
        vkCmdCopyBufferToImage(cb, buf, img->image, VK_IMAGE_LAYOUT_GENERAL, 1, &bic);
        full_barrier(cb);
        img->uploaded = true;
        return;
    }
    if (row % bpb || slice % row) {
        ONCE_RED("buffer/texture copy with row stride %lld, image stride %lld (not whole blocks): skipped",
                 static_cast<long long>(g_row_stride), static_cast<long long>(g_img_stride));
        return;
    }
    bic.bufferRowLength = static_cast<u32>(row / bpb) * bw;
    bic.bufferImageHeight = static_cast<u32>(slice / row) * bh;
    if (!to_texture) watch::gpu_access("texture to buffer", nullptr, 0, c->src, bytes), pool_gpu_write(c->src, bytes);
    BufferRef br;
    const bool direct = nvn::host_readable(c->src, bytes) && pool_find(c->src, bytes, br) && br.offset % 4 == 0 && br.offset % bpb == 0;
    if (direct) {
        bic.bufferOffset = br.offset;
        if (to_texture) vkCmdCopyBufferToImage(cb, br.buffer, img->image, VK_IMAGE_LAYOUT_GENERAL, 1, &bic);
        else {
            vkCmdCopyImageToBuffer(cb, img->image, VK_IMAGE_LAYOUT_GENERAL, br.buffer, 1, &bic);
            // (the game reads such copies back; where it does not ask first, they must be in system memory all the same)
            if (pool_gpu_owned(c->src, bytes)) {
                full_barrier(cb);
                pool_write_through(cb, c->src, bytes);
            }
        }
    } else if (to_texture) {
        VkBuffer buf;
        void* ptr;
        if (!staging(bytes, &buf, &ptr)) return;
        nvn::gpu_read(c->src, ptr, bytes);
        vkCmdCopyBufferToImage(cb, buf, img->image, VK_IMAGE_LAYOUT_GENERAL, 1, &bic);
    } else {
        ONCE_RED("texture to buffer copy into memory the GPU cannot reach (%#llx): skipped", static_cast<unsigned long long>(c->src));
        return;
    }
    full_barrier(cb);
    if (to_texture) img->gpu_written = true;
    if (is3d) {
        static std::unordered_map<u64, int> n3d;
        if (n3d[img->serial]++ < 1)
            LOG("nvk: 3D copy %s: image %llu %ux%ux%u fmt %#x region %d,%d,%d %dx%dx%d row %llu slice %llu direct %d",
                to_texture ? "into" : "from", static_cast<unsigned long long>(img->serial), img->width, img->height, img->depth,
                img->nvn_format, r.xoffset, r.yoffset, r.zoffset, r.width, r.height, r.depth, static_cast<unsigned long long>(row),
                static_cast<unsigned long long>(slice), direct);
    }
}

void copy_texture_texture(VkCommandBuffer cb, const nvn::p_copy_t2t* c) {
    if (!c->src || !c->dst) return;
    end_rendering(cb);
    Image* si = image_of(c->src, draw_pre_cb());
    Image* di = image_of(c->dst, draw_pre_cb());
    if (!si || !di) return;
    draw_ensure_content(si, c->src);
    draw_ensure_content(di, c->dst);
    const Sub ss = sub_of(si, &c->sv, c->has_sv), ds = sub_of(di, &c->dv, c->has_dv);
    const NVNcopyRegion &sr = c->sr, &dr = c->dr;
    const bool s3 = si->type == VK_IMAGE_TYPE_3D, d3 = di->type == VK_IMAGE_TYPE_3D;
    const u32 sd = static_cast<u32>(std::max(sr.depth, 1)), dd = static_cast<u32>(std::max(dr.depth, 1));
    const VkImageSubresourceLayers sl{copy_aspect(si), ss.level, s3 ? 0 : ss.layer + static_cast<u32>(sr.zoffset), s3 ? 1 : sd};
    const VkImageSubresourceLayers dl{copy_aspect(di), ds.level, d3 ? 0 : ds.layer + static_cast<u32>(dr.zoffset), d3 ? 1 : dd};
    dump_note("copy image %llu (fmt %#x level %u layer %u %d,%d %dx%d) -> image %llu (fmt %#x level %u layer %u %d,%d %dx%d)",
              static_cast<unsigned long long>(si->serial), si->nvn_format, sl.mipLevel, sl.baseArrayLayer, sr.xoffset, sr.yoffset, sr.width,
              sr.height, static_cast<unsigned long long>(di->serial), di->nvn_format, dl.mipLevel, dl.baseArrayLayer, dr.xoffset,
              dr.yoffset, dr.width, dr.height);
    if (di->sparse) {
        static int n = 0;
        if (n++ < 16)
            LOG("nvk: copy image %llu (%ux%u fmt %#x level %u layer %u %d,%d %dx%d) -> sparse image %llu (fmt %#x level %u layer %u %d,%d %dx%d)",
                static_cast<unsigned long long>(si->serial), si->width, si->height, si->nvn_format, sl.mipLevel, sl.baseArrayLayer,
                sr.xoffset, sr.yoffset, sr.width, sr.height, static_cast<unsigned long long>(di->serial), di->nvn_format, dl.mipLevel,
                dl.baseArrayLayer, dr.xoffset, dr.yoffset, dr.width, dr.height);
    }
    if (di->format == VK_FORMAT_BC1_RGBA_SRGB_BLOCK && format(di->nvn_format).kind == FmtKind::Astc && si->format != di->format) {
        if (si->format == VK_FORMAT_BC3_SRGB_BLOCK) copy_bc3_to_bc1(cb, si, di, sl, dl, sr, dr);
        else ONCE_RED("copy into a BC1 terrain cache from format %d: skipped", si->format);
        return;
    }
    if (si->format == VK_FORMAT_BC1_RGBA_SRGB_BLOCK && format(si->nvn_format).kind == FmtKind::Astc && si->format != di->format) {
        ONCE_RED("copy out of a BC1 terrain cache into format %d: skipped", di->format);
        return;
    }
    if (c->flags & NVN_COPY_RESOLVE) {
        if (si->samples == VK_SAMPLE_COUNT_1_BIT || di->samples != VK_SAMPLE_COUNT_1_BIT || (si->aspect & ~VK_IMAGE_ASPECT_COLOR_BIT)) {
            ONCE_RED("downsample between these textures (samples %d -> %d): skipped", si->samples, di->samples);
            return;
        }
        VkImageResolve rs{};
        rs.srcSubresource = sl;
        rs.dstSubresource = dl;
        rs.extent = {std::min(si->width, di->width), std::min(si->height, di->height), 1};
        vkCmdResolveImage(cb, si->image, VK_IMAGE_LAYOUT_GENERAL, di->image, VK_IMAGE_LAYOUT_GENERAL, 1, &rs);
    } else if (sr.width == dr.width && sr.height == dr.height && !(c->flags & (NVN_COPY_FLAGS_MIRROR_X_BIT | NVN_COPY_FLAGS_MIRROR_Y_BIT)) &&
               si->samples == di->samples && format(si->nvn_format).bytes == format(di->nvn_format).bytes &&
               format(si->nvn_format).block_w == format(di->nvn_format).block_w) {
        VkImageCopy ic{};
        ic.srcSubresource = sl;
        ic.dstSubresource = dl;
        ic.srcOffset = {sr.xoffset, sr.yoffset, s3 ? sr.zoffset : 0};
        ic.dstOffset = {dr.xoffset, dr.yoffset, d3 ? dr.zoffset : 0};
        ic.extent = {static_cast<u32>(sr.width), static_cast<u32>(sr.height), s3 ? sd : 1};
        if (si->aspect != di->aspect) {
            ONCE_RED("texture copy between colour and depth: skipped");
            return;
        }
        vkCmdCopyImage(cb, si->image, VK_IMAGE_LAYOUT_GENERAL, di->image, VK_IMAGE_LAYOUT_GENERAL, 1, &ic);
    } else {
        if (si->samples != VK_SAMPLE_COUNT_1_BIT || di->samples != VK_SAMPLE_COUNT_1_BIT) {
            ONCE_RED("scaled copy of a multisample texture: skipped");
            return;
        }
        VkImageBlit bl{};
        bl.srcSubresource = sl;
        bl.dstSubresource = dl;
        bl.srcOffsets[0] = {sr.xoffset, sr.yoffset, s3 ? sr.zoffset : 0};
        bl.srcOffsets[1] = {sr.xoffset + sr.width, sr.yoffset + sr.height, s3 ? sr.zoffset + static_cast<int>(sd) : 1};
        bl.dstOffsets[0] = {dr.xoffset, dr.yoffset, d3 ? dr.zoffset : 0};
        bl.dstOffsets[1] = {dr.xoffset + dr.width, dr.yoffset + dr.height, d3 ? dr.zoffset + static_cast<int>(dd) : 1};
        if (c->flags & NVN_COPY_FLAGS_MIRROR_X_BIT) std::swap(bl.dstOffsets[0].x, bl.dstOffsets[1].x);
        if (c->flags & NVN_COPY_FLAGS_MIRROR_Y_BIT) std::swap(bl.dstOffsets[0].y, bl.dstOffsets[1].y);
        const bool depth = (si->aspect & VK_IMAGE_ASPECT_DEPTH_BIT) != 0;
        vkCmdBlitImage(cb, si->image, VK_IMAGE_LAYOUT_GENERAL, di->image, VK_IMAGE_LAYOUT_GENERAL, 1, &bl,
                       (c->flags & NVN_COPY_FLAGS_LINEAR_FILTER_BIT) && !depth ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);
    }
    full_barrier(cb);
    di->gpu_written = true;
}

}  // namespace nvk
