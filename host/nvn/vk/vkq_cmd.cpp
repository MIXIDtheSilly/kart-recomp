// Command recording on the driver thread (vkq.h, HOST_NVN_CMD_PROXY=1). The vkCmd* entry points in namespace vkfn
// become encoders of the same signature: on the renderer thread (the producer) an encoder appends a closure that
// calls the real function later on the driver thread, with copies of its arguments; on any other thread (present()
// on the driver thread, upload and warm-up threads) it calls the real function at once. The handles stay the real
// ones, so no call site changes; Begin/End/Reset return VK_SUCCESS at once and the closure checks the real result.
//
// Arrays a command points to are copied into the ring just before its closure (vkq::copy). The all-scalar commands
// share one template, which refuses a pointer-to-const argument, so a command that needs a deep copy cannot slip
// through as scalar. Commands the renderer does not use get the "synced" form: the producer waits for the driver
// thread, then calls the real function itself (correct, slow; never on a hot path).
//
// Command pools are externally synchronised: vkAllocateCommandBuffers / vkFreeCommandBuffers on the producer wait
// for the driver thread first (rare after warm-up); vkResetCommandPool goes through the ring in order.
#include "vkq.h"

#include <cstdlib>
#include <cstring>
#include <type_traits>

#include "vkctx.h"

namespace nvk::vkq {
namespace {

#define VKQ_SCALAR(X)                                                                                                    \
    X(vkCmdEndRendering) X(vkCmdBindPipeline) X(vkCmdBindIndexBuffer) X(vkCmdDraw) X(vkCmdDrawIndexed)                   \
    X(vkCmdDrawIndirect) X(vkCmdDrawIndexedIndirect) X(vkCmdDrawIndirectCount) X(vkCmdDrawIndexedIndirectCount)           \
    X(vkCmdDrawMeshTasksEXT) X(vkCmdDrawMeshTasksIndirectEXT) X(vkCmdDrawMeshTasksIndirectCountEXT) X(vkCmdDispatch)     \
    X(vkCmdDispatchIndirect) X(vkCmdFillBuffer) X(vkCmdSetLineWidth) X(vkCmdSetDepthBias) X(vkCmdSetDepthBounds)          \
    X(vkCmdSetStencilCompareMask) X(vkCmdSetStencilWriteMask) X(vkCmdSetStencilReference) X(vkCmdSetCullMode)             \
    X(vkCmdSetFrontFace) X(vkCmdSetPrimitiveTopology) X(vkCmdSetDepthTestEnable) X(vkCmdSetDepthWriteEnable)              \
    X(vkCmdSetDepthCompareOp) X(vkCmdSetDepthBoundsTestEnable) X(vkCmdSetStencilTestEnable) X(vkCmdSetStencilOp)          \
    X(vkCmdSetRasterizerDiscardEnable) X(vkCmdSetDepthBiasEnable) X(vkCmdSetPrimitiveRestartEnable)                       \
    X(vkCmdSetDepthClampEnableEXT) X(vkCmdSetPolygonModeEXT) X(vkCmdSetLogicOpEnableEXT) X(vkCmdSetLogicOpEXT)            \
    X(vkCmdSetAlphaToCoverageEnableEXT) X(vkCmdSetRasterizationSamplesEXT) X(vkCmdSetDepthClipNegativeOneToOneEXT)        \
    X(vkCmdSetDepthClipEnableEXT) X(vkCmdSetAlphaToOneEnableEXT) X(vkCmdSetProvokingVertexModeEXT) X(vkCmdBeginQuery)     \
    X(vkCmdEndQuery) X(vkCmdResetQueryPool) X(vkCmdWriteTimestamp) X(vkCmdCopyQueryPoolResults)                           \
    X(vkCmdEndConditionalRenderingEXT) X(vkCmdEndDebugUtilsLabelEXT)

// Not used by the renderer (or only where a stall does not matter)
#define VKQ_SYNCED(X)                                                                                                    \
    X(vkCmdPipelineBarrier2) X(vkCmdBindVertexBuffers) X(vkCmdSetViewport) X(vkCmdSetScissor)                             \
    X(vkCmdSetViewportSwizzleNV) X(vkCmdBeginConditionalRenderingEXT) X(vkCmdBeginDebugUtilsLabelEXT)                     \
    X(vkAllocateCommandBuffers) X(vkFreeCommandBuffers)

#define VKQ_HAND(X)                                                                                                      \
    X(vkCmdPipelineBarrier) X(vkCmdBeginRendering) X(vkCmdBindDescriptorSets) X(vkCmdPushDescriptorSetKHR)               \
    X(vkCmdPushConstants) X(vkCmdBindVertexBuffers2) X(vkCmdCopyBuffer) X(vkCmdCopyImage) X(vkCmdBlitImage)               \
    X(vkCmdCopyBufferToImage) X(vkCmdCopyImageToBuffer) X(vkCmdResolveImage) X(vkCmdUpdateBuffer)                         \
    X(vkCmdClearColorImage) X(vkCmdClearDepthStencilImage) X(vkCmdClearAttachments) X(vkCmdSetViewportWithCount)          \
    X(vkCmdSetScissorWithCount) X(vkCmdSetBlendConstants) X(vkCmdSetColorBlendEnableEXT) X(vkCmdSetColorBlendEquationEXT) \
    X(vkCmdSetColorWriteMaskEXT) X(vkCmdSetSampleMaskEXT) X(vkCmdSetVertexInputEXT) X(vkCmdSetCheckpointNV)               \
    X(vkBeginCommandBuffer) X(vkEndCommandBuffer) X(vkResetCommandBuffer) X(vkResetCommandPool)

namespace real {
#define VKQ_REAL(name) PFN_##name name = nullptr;
VKQ_SCALAR(VKQ_REAL)
VKQ_SYNCED(VKQ_REAL)
VKQ_HAND(VKQ_REAL)
#undef VKQ_REAL
}  // namespace real

template <class T> constexpr bool is_cptr = std::is_pointer_v<T> && std::is_const_v<std::remove_pointer_t<T>>;

template <auto* Real, class Sig> struct Scalar;
template <auto* Real, class... A> struct Scalar<Real, void(VKAPI_PTR*)(A...)> {
    static_assert(!(is_cptr<A> || ...), "a command with a data pointer needs a hand-written encoder (deep copy)");
    static void VKAPI_CALL call(A... a) {
        if (!detail::t_producer) return (*Real)(a...);
        run([a...] { (*Real)(a...); });
    }
};

template <auto* Real, class Sig> struct Synced;
template <auto* Real, class R, class... A> struct Synced<Real, R(VKAPI_PTR*)(A...)> {
    static R VKAPI_CALL call(A... a) {
        if (detail::t_producer) sync();
        return (*Real)(a...);
    }
};

#define PASS(name, ...) \
    if (!detail::t_producer) return real::name(__VA_ARGS__)

void VKAPI_CALL e_vkCmdPipelineBarrier(VkCommandBuffer cb, VkPipelineStageFlags src, VkPipelineStageFlags dst, VkDependencyFlags dep,
                                       uint32_t nm, const VkMemoryBarrier* m, uint32_t nb, const VkBufferMemoryBarrier* b, uint32_t ni,
                                       const VkImageMemoryBarrier* i) {
    PASS(vkCmdPipelineBarrier, cb, src, dst, dep, nm, m, nb, b, ni, i);
    VkMemoryBarrier* mc = copy(m, nm);
    VkBufferMemoryBarrier* bc = copy(b, nb);
    VkImageMemoryBarrier* ic = copy(i, ni);
    run([=] { real::vkCmdPipelineBarrier(cb, src, dst, dep, nm, mc, nb, bc, ni, ic); });
}

void VKAPI_CALL e_vkCmdBeginRendering(VkCommandBuffer cb, const VkRenderingInfo* ri) {
    PASS(vkCmdBeginRendering, cb, ri);
    // (the renderer sets no pNext on these)
    VkRenderingInfo* r = copy(ri, 1);
    r->pColorAttachments = copy(ri->pColorAttachments, ri->colorAttachmentCount);
    r->pDepthAttachment = copy(ri->pDepthAttachment, 1);
    r->pStencilAttachment = copy(ri->pStencilAttachment, 1);
    run([cb, r] { real::vkCmdBeginRendering(cb, r); });
}

void VKAPI_CALL e_vkCmdBindDescriptorSets(VkCommandBuffer cb, VkPipelineBindPoint bp, VkPipelineLayout layout, uint32_t first, uint32_t n,
                                          const VkDescriptorSet* sets, uint32_t nd, const uint32_t* dyn) {
    PASS(vkCmdBindDescriptorSets, cb, bp, layout, first, n, sets, nd, dyn);
    VkDescriptorSet* s = copy(sets, n);
    uint32_t* d = copy(dyn, nd);
    run([=] { real::vkCmdBindDescriptorSets(cb, bp, layout, first, n, s, nd, d); });
}

void VKAPI_CALL e_vkCmdPushDescriptorSetKHR(VkCommandBuffer cb, VkPipelineBindPoint bp, VkPipelineLayout layout, uint32_t set, uint32_t n,
                                            const VkWriteDescriptorSet* w) {
    PASS(vkCmdPushDescriptorSetKHR, cb, bp, layout, set, n, w);
    VkWriteDescriptorSet* wc = copy(w, n);
    for (uint32_t k = 0; k < n; k++) {
        wc[k].pImageInfo = copy(w[k].pImageInfo, w[k].descriptorCount);
        wc[k].pBufferInfo = copy(w[k].pBufferInfo, w[k].descriptorCount);
        wc[k].pTexelBufferView = copy(w[k].pTexelBufferView, w[k].descriptorCount);
    }
    run([=] { real::vkCmdPushDescriptorSetKHR(cb, bp, layout, set, n, wc); });
}

void VKAPI_CALL e_vkCmdPushConstants(VkCommandBuffer cb, VkPipelineLayout layout, VkShaderStageFlags st, uint32_t off, uint32_t size,
                                     const void* v) {
    PASS(vkCmdPushConstants, cb, layout, st, off, size, v);
    const uint8_t* d = copy(static_cast<const uint8_t*>(v), size);
    run([=] { real::vkCmdPushConstants(cb, layout, st, off, size, d); });
}

void VKAPI_CALL e_vkCmdBindVertexBuffers2(VkCommandBuffer cb, uint32_t first, uint32_t n, const VkBuffer* bufs, const VkDeviceSize* offs,
                                          const VkDeviceSize* sizes, const VkDeviceSize* strides) {
    PASS(vkCmdBindVertexBuffers2, cb, first, n, bufs, offs, sizes, strides);
    VkBuffer* b = copy(bufs, n);
    VkDeviceSize* o = copy(offs, n);
    VkDeviceSize* s = copy(sizes, n);
    VkDeviceSize* t = copy(strides, n);
    run([=] { real::vkCmdBindVertexBuffers2(cb, first, n, b, o, s, t); });
}

void VKAPI_CALL e_vkCmdCopyBuffer(VkCommandBuffer cb, VkBuffer src, VkBuffer dst, uint32_t n, const VkBufferCopy* r) {
    PASS(vkCmdCopyBuffer, cb, src, dst, n, r);
    VkBufferCopy* rc = copy(r, n);
    run([=] { real::vkCmdCopyBuffer(cb, src, dst, n, rc); });
}

void VKAPI_CALL e_vkCmdCopyImage(VkCommandBuffer cb, VkImage src, VkImageLayout sl, VkImage dst, VkImageLayout dl, uint32_t n,
                                 const VkImageCopy* r) {
    PASS(vkCmdCopyImage, cb, src, sl, dst, dl, n, r);
    VkImageCopy* rc = copy(r, n);
    run([=] { real::vkCmdCopyImage(cb, src, sl, dst, dl, n, rc); });
}

void VKAPI_CALL e_vkCmdBlitImage(VkCommandBuffer cb, VkImage src, VkImageLayout sl, VkImage dst, VkImageLayout dl, uint32_t n,
                                 const VkImageBlit* r, VkFilter f) {
    PASS(vkCmdBlitImage, cb, src, sl, dst, dl, n, r, f);
    VkImageBlit* rc = copy(r, n);
    run([=] { real::vkCmdBlitImage(cb, src, sl, dst, dl, n, rc, f); });
}

void VKAPI_CALL e_vkCmdCopyBufferToImage(VkCommandBuffer cb, VkBuffer src, VkImage dst, VkImageLayout dl, uint32_t n,
                                         const VkBufferImageCopy* r) {
    PASS(vkCmdCopyBufferToImage, cb, src, dst, dl, n, r);
    VkBufferImageCopy* rc = copy(r, n);
    run([=] { real::vkCmdCopyBufferToImage(cb, src, dst, dl, n, rc); });
}

void VKAPI_CALL e_vkCmdCopyImageToBuffer(VkCommandBuffer cb, VkImage src, VkImageLayout sl, VkBuffer dst, uint32_t n,
                                         const VkBufferImageCopy* r) {
    PASS(vkCmdCopyImageToBuffer, cb, src, sl, dst, n, r);
    VkBufferImageCopy* rc = copy(r, n);
    run([=] { real::vkCmdCopyImageToBuffer(cb, src, sl, dst, n, rc); });
}

void VKAPI_CALL e_vkCmdResolveImage(VkCommandBuffer cb, VkImage src, VkImageLayout sl, VkImage dst, VkImageLayout dl, uint32_t n,
                                    const VkImageResolve* r) {
    PASS(vkCmdResolveImage, cb, src, sl, dst, dl, n, r);
    VkImageResolve* rc = copy(r, n);
    run([=] { real::vkCmdResolveImage(cb, src, sl, dst, dl, n, rc); });
}

void VKAPI_CALL e_vkCmdUpdateBuffer(VkCommandBuffer cb, VkBuffer dst, VkDeviceSize off, VkDeviceSize size, const void* data) {
    PASS(vkCmdUpdateBuffer, cb, dst, off, size, data);
    const uint8_t* d = copy(static_cast<const uint8_t*>(data), static_cast<size_t>(size));
    run([=] { real::vkCmdUpdateBuffer(cb, dst, off, size, d); });
}

void VKAPI_CALL e_vkCmdClearColorImage(VkCommandBuffer cb, VkImage img, VkImageLayout l, const VkClearColorValue* c, uint32_t n,
                                       const VkImageSubresourceRange* r) {
    PASS(vkCmdClearColorImage, cb, img, l, c, n, r);
    const VkClearColorValue cv = *c;
    VkImageSubresourceRange* rc = copy(r, n);
    run([=] { real::vkCmdClearColorImage(cb, img, l, &cv, n, rc); });
}

void VKAPI_CALL e_vkCmdClearDepthStencilImage(VkCommandBuffer cb, VkImage img, VkImageLayout l, const VkClearDepthStencilValue* c,
                                              uint32_t n, const VkImageSubresourceRange* r) {
    PASS(vkCmdClearDepthStencilImage, cb, img, l, c, n, r);
    const VkClearDepthStencilValue cv = *c;
    VkImageSubresourceRange* rc = copy(r, n);
    run([=] { real::vkCmdClearDepthStencilImage(cb, img, l, &cv, n, rc); });
}

void VKAPI_CALL e_vkCmdClearAttachments(VkCommandBuffer cb, uint32_t na, const VkClearAttachment* a, uint32_t nr, const VkClearRect* r) {
    PASS(vkCmdClearAttachments, cb, na, a, nr, r);
    VkClearAttachment* ac = copy(a, na);
    VkClearRect* rc = copy(r, nr);
    run([=] { real::vkCmdClearAttachments(cb, na, ac, nr, rc); });
}

void VKAPI_CALL e_vkCmdSetViewportWithCount(VkCommandBuffer cb, uint32_t n, const VkViewport* v) {
    PASS(vkCmdSetViewportWithCount, cb, n, v);
    VkViewport* vc = copy(v, n);
    run([=] { real::vkCmdSetViewportWithCount(cb, n, vc); });
}

void VKAPI_CALL e_vkCmdSetScissorWithCount(VkCommandBuffer cb, uint32_t n, const VkRect2D* s) {
    PASS(vkCmdSetScissorWithCount, cb, n, s);
    VkRect2D* sc = copy(s, n);
    run([=] { real::vkCmdSetScissorWithCount(cb, n, sc); });
}

void VKAPI_CALL e_vkCmdSetBlendConstants(VkCommandBuffer cb, const float c[4]) {
    PASS(vkCmdSetBlendConstants, cb, c);
    struct F4 { float v[4]; } f;
    std::memcpy(f.v, c, sizeof f.v);
    run([cb, f] { real::vkCmdSetBlendConstants(cb, f.v); });
}

void VKAPI_CALL e_vkCmdSetColorBlendEnableEXT(VkCommandBuffer cb, uint32_t first, uint32_t n, const VkBool32* e) {
    PASS(vkCmdSetColorBlendEnableEXT, cb, first, n, e);
    VkBool32* ec = copy(e, n);
    run([=] { real::vkCmdSetColorBlendEnableEXT(cb, first, n, ec); });
}

void VKAPI_CALL e_vkCmdSetColorBlendEquationEXT(VkCommandBuffer cb, uint32_t first, uint32_t n, const VkColorBlendEquationEXT* e) {
    PASS(vkCmdSetColorBlendEquationEXT, cb, first, n, e);
    VkColorBlendEquationEXT* ec = copy(e, n);
    run([=] { real::vkCmdSetColorBlendEquationEXT(cb, first, n, ec); });
}

void VKAPI_CALL e_vkCmdSetColorWriteMaskEXT(VkCommandBuffer cb, uint32_t first, uint32_t n, const VkColorComponentFlags* m) {
    PASS(vkCmdSetColorWriteMaskEXT, cb, first, n, m);
    VkColorComponentFlags* mc = copy(m, n);
    run([=] { real::vkCmdSetColorWriteMaskEXT(cb, first, n, mc); });
}

void VKAPI_CALL e_vkCmdSetSampleMaskEXT(VkCommandBuffer cb, VkSampleCountFlagBits samples, const VkSampleMask* m) {
    PASS(vkCmdSetSampleMaskEXT, cb, samples, m);
    VkSampleMask* mc = copy(m, (static_cast<size_t>(samples) + 31) / 32);
    run([=] { real::vkCmdSetSampleMaskEXT(cb, samples, mc); });
}

void VKAPI_CALL e_vkCmdSetVertexInputEXT(VkCommandBuffer cb, uint32_t nb, const VkVertexInputBindingDescription2EXT* b, uint32_t na,
                                         const VkVertexInputAttributeDescription2EXT* a) {
    PASS(vkCmdSetVertexInputEXT, cb, nb, b, na, a);
    // (the renderer sets no pNext on these)
    VkVertexInputBindingDescription2EXT* bc = copy(b, nb);
    VkVertexInputAttributeDescription2EXT* ac = copy(a, na);
    run([=] { real::vkCmdSetVertexInputEXT(cb, nb, bc, na, ac); });
}

void VKAPI_CALL e_vkCmdSetCheckpointNV(VkCommandBuffer cb, const void* marker) {
    PASS(vkCmdSetCheckpointNV, cb, marker);
    // (an opaque marker value, not data)
    run([=] { real::vkCmdSetCheckpointNV(cb, marker); });
}

VkResult VKAPI_CALL e_vkBeginCommandBuffer(VkCommandBuffer cb, const VkCommandBufferBeginInfo* bi) {
    PASS(vkBeginCommandBuffer, cb, bi);
    // (primary command buffers only: no inheritance info; no pNext)
    const VkCommandBufferUsageFlags flags = bi->flags;
    run([cb, flags] {
        VkCommandBufferBeginInfo b{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        b.flags = flags;
        check(real::vkBeginCommandBuffer(cb, &b), "vkBeginCommandBuffer (driver thread)");
    });
    return VK_SUCCESS;
}

VkResult VKAPI_CALL e_vkEndCommandBuffer(VkCommandBuffer cb) {
    PASS(vkEndCommandBuffer, cb);
    run([cb] { check(real::vkEndCommandBuffer(cb), "vkEndCommandBuffer (driver thread)"); });
    return VK_SUCCESS;
}

VkResult VKAPI_CALL e_vkResetCommandBuffer(VkCommandBuffer cb, VkCommandBufferResetFlags f) {
    PASS(vkResetCommandBuffer, cb, f);
    run([cb, f] { check(real::vkResetCommandBuffer(cb, f), "vkResetCommandBuffer (driver thread)"); });
    return VK_SUCCESS;
}

VkResult VKAPI_CALL e_vkResetCommandPool(VkDevice d, VkCommandPool p, VkCommandPoolResetFlags f) {
    PASS(vkResetCommandPool, d, p, f);
    run([d, p, f] { check(real::vkResetCommandPool(d, p, f), "vkResetCommandPool (driver thread)"); });
    return VK_SUCCESS;
}

#undef PASS

}  // namespace

bool cmd_proxy_on() {
    static const bool v = [] {
        const char* e = getenv("HOST_NVN_CMD_PROXY");
        return on() && e && std::strcmp(e, "0") != 0;
    }();
    return v;
}

void install_cmd_proxy() {
    if (!cmd_proxy_on()) return;
#define VKQ_SET_SCALAR(name) \
    real::name = vkfn::name;  \
    if (vkfn::name) vkfn::name = &Scalar<&real::name, PFN_##name>::call;
#define VKQ_SET_SYNCED(name) \
    real::name = vkfn::name;  \
    if (vkfn::name) vkfn::name = &Synced<&real::name, PFN_##name>::call;
#define VKQ_SET_HAND(name)   \
    real::name = vkfn::name; \
    if (vkfn::name) vkfn::name = &e_##name;
    VKQ_SCALAR(VKQ_SET_SCALAR)
    VKQ_SYNCED(VKQ_SET_SYNCED)
    VKQ_HAND(VKQ_SET_HAND)
#undef VKQ_SET_SCALAR
#undef VKQ_SET_SYNCED
#undef VKQ_SET_HAND
    LOG("nvk: command recording on the driver thread too (HOST_NVN_CMD_PROXY)");
}

}  // namespace nvk::vkq
