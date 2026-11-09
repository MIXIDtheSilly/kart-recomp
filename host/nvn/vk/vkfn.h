// Vulkan entry points the native NVN renderer uses, loaded at run time (no import library).
#pragma once
#include "video_core/vulkan_common/vulkan.h"  // VK_NO_PROTOTYPES, platform defines

#define VKFN_GLOBAL(X)                                                                             \
    X(vkCreateInstance) X(vkEnumerateInstanceExtensionProperties) X(vkEnumerateInstanceLayerProperties) \
    X(vkEnumerateInstanceVersion)

#define VKFN_INSTANCE(X)                                                                           \
    X(vkDestroyInstance) X(vkEnumeratePhysicalDevices) X(vkGetPhysicalDeviceProperties)            \
    X(vkGetPhysicalDeviceProperties2) X(vkGetPhysicalDeviceFeatures2) X(vkGetPhysicalDeviceMemoryProperties) \
    X(vkGetPhysicalDeviceMemoryProperties2) X(vkGetPhysicalDeviceQueueFamilyProperties)            \
    X(vkGetPhysicalDeviceFormatProperties) X(vkGetPhysicalDeviceFormatProperties2)                 \
    X(vkGetPhysicalDeviceImageFormatProperties) X(vkEnumerateDeviceExtensionProperties)           \
    X(vkCreateDevice) X(vkGetDeviceProcAddr) X(vkDestroySurfaceKHR) X(vkCreateWin32SurfaceKHR)    \
    X(vkGetPhysicalDeviceSurfaceSupportKHR) X(vkGetPhysicalDeviceSurfaceCapabilitiesKHR)          \
    X(vkGetPhysicalDeviceSurfaceFormatsKHR) X(vkGetPhysicalDeviceSurfacePresentModesKHR)          \
    X(vkCreateDebugUtilsMessengerEXT) X(vkDestroyDebugUtilsMessengerEXT)                           \
    X(vkGetPhysicalDeviceSparseImageFormatProperties)

#define VKFN_DEVICE(X)                                                                             \
    X(vkDestroyDevice) X(vkGetDeviceQueue) X(vkDeviceWaitIdle) X(vkQueueSubmit) X(vkQueueSubmit2)  \
    X(vkQueueWaitIdle) X(vkQueueBindSparse)                                                        \
    X(vkAllocateMemory) X(vkFreeMemory) X(vkMapMemory) X(vkUnmapMemory) X(vkFlushMappedMemoryRanges) \
    X(vkInvalidateMappedMemoryRanges) X(vkGetMemoryHostPointerPropertiesEXT)                       \
    X(vkCreateBuffer) X(vkDestroyBuffer) X(vkBindBufferMemory) X(vkGetBufferMemoryRequirements)    \
    X(vkGetBufferMemoryRequirements2) X(vkBindBufferMemory2) X(vkGetBufferDeviceAddress)           \
    X(vkCreateImage) X(vkDestroyImage) X(vkBindImageMemory) X(vkBindImageMemory2)                  \
    X(vkGetImageMemoryRequirements) X(vkGetImageMemoryRequirements2) X(vkGetImageSubresourceLayout) \
    X(vkGetImageSparseMemoryRequirements)                                                          \
    X(vkCreateImageView) X(vkDestroyImageView) X(vkCreateBufferView) X(vkDestroyBufferView)        \
    X(vkCreateSampler) X(vkDestroySampler)                                                         \
    X(vkCreateShaderModule) X(vkDestroyShaderModule)                                               \
    X(vkCreatePipelineCache) X(vkDestroyPipelineCache) X(vkGetPipelineCacheData)                   \
    X(vkCreateGraphicsPipelines) X(vkCreateComputePipelines) X(vkDestroyPipeline)                  \
    X(vkCreatePipelineLayout) X(vkDestroyPipelineLayout)                                           \
    X(vkCreateDescriptorSetLayout) X(vkDestroyDescriptorSetLayout)                                 \
    X(vkCreateDescriptorPool) X(vkDestroyDescriptorPool) X(vkResetDescriptorPool)                  \
    X(vkAllocateDescriptorSets) X(vkUpdateDescriptorSets)                                          \
    X(vkCreateCommandPool) X(vkDestroyCommandPool) X(vkResetCommandPool)                           \
    X(vkAllocateCommandBuffers) X(vkFreeCommandBuffers) X(vkResetCommandBuffer) X(vkBeginCommandBuffer) X(vkEndCommandBuffer) \
    X(vkCreateFence) X(vkDestroyFence) X(vkWaitForFences) X(vkResetFences) X(vkGetFenceStatus)     \
    X(vkCreateSemaphore) X(vkDestroySemaphore) X(vkWaitSemaphores) X(vkSignalSemaphore)            \
    X(vkGetSemaphoreCounterValue)                                                                  \
    X(vkCreateQueryPool) X(vkDestroyQueryPool) X(vkGetQueryPoolResults) X(vkResetQueryPool)        \
    X(vkCreateSwapchainKHR) X(vkDestroySwapchainKHR) X(vkGetSwapchainImagesKHR)                    \
    X(vkAcquireNextImageKHR) X(vkQueuePresentKHR)                                                  \
    X(vkSetDebugUtilsObjectNameEXT)                                                                \
    X(vkCmdPipelineBarrier) X(vkCmdPipelineBarrier2) X(vkCmdBeginRendering) X(vkCmdEndRendering)   \
    X(vkCmdBindPipeline) X(vkCmdBindDescriptorSets) X(vkCmdPushDescriptorSetKHR) X(vkCmdPushConstants) \
    X(vkCmdBindVertexBuffers) X(vkCmdBindVertexBuffers2) X(vkCmdBindIndexBuffer)                   \
    X(vkCmdDraw) X(vkCmdDrawIndexed) X(vkCmdDrawIndirect) X(vkCmdDrawIndexedIndirect)              \
    X(vkCmdDrawIndirectCount) X(vkCmdDrawIndexedIndirectCount)                                     \
    X(vkCmdDrawMeshTasksEXT) X(vkCmdDrawMeshTasksIndirectEXT) X(vkCmdDrawMeshTasksIndirectCountEXT) \
    X(vkCmdDispatch) X(vkCmdDispatchIndirect)                                                      \
    X(vkCmdCopyBuffer) X(vkCmdCopyImage) X(vkCmdBlitImage) X(vkCmdCopyBufferToImage)               \
    X(vkCmdCopyImageToBuffer) X(vkCmdFillBuffer) X(vkCmdUpdateBuffer) X(vkCmdClearColorImage)      \
    X(vkCmdClearDepthStencilImage) X(vkCmdClearAttachments) X(vkCmdResolveImage)                   \
    X(vkCmdSetViewport) X(vkCmdSetScissor) X(vkCmdSetViewportWithCount) X(vkCmdSetScissorWithCount) \
    X(vkCmdSetLineWidth) X(vkCmdSetDepthBias) X(vkCmdSetBlendConstants) X(vkCmdSetDepthBounds)     \
    X(vkCmdSetStencilCompareMask) X(vkCmdSetStencilWriteMask) X(vkCmdSetStencilReference)          \
    X(vkCmdSetCullMode) X(vkCmdSetFrontFace) X(vkCmdSetPrimitiveTopology) X(vkCmdSetDepthTestEnable) \
    X(vkCmdSetDepthWriteEnable) X(vkCmdSetDepthCompareOp) X(vkCmdSetDepthBoundsTestEnable)         \
    X(vkCmdSetStencilTestEnable) X(vkCmdSetStencilOp) X(vkCmdSetRasterizerDiscardEnable)           \
    X(vkCmdSetDepthBiasEnable) X(vkCmdSetPrimitiveRestartEnable)                                   \
    X(vkCmdSetDepthClampEnableEXT) X(vkCmdSetPolygonModeEXT) X(vkCmdSetLogicOpEnableEXT)           \
    X(vkCmdSetLogicOpEXT) X(vkCmdSetColorBlendEnableEXT) X(vkCmdSetColorBlendEquationEXT)          \
    X(vkCmdSetColorWriteMaskEXT) X(vkCmdSetAlphaToCoverageEnableEXT) X(vkCmdSetRasterizationSamplesEXT) \
    X(vkCmdSetSampleMaskEXT) X(vkCmdSetVertexInputEXT) X(vkCmdSetDepthClipNegativeOneToOneEXT) X(vkCmdSetDepthClipEnableEXT) X(vkCmdSetAlphaToOneEnableEXT) X(vkCmdSetViewportSwizzleNV) X(vkCmdSetProvokingVertexModeEXT)                                             \
    X(vkCmdBeginQuery) X(vkCmdEndQuery) X(vkCmdResetQueryPool) X(vkCmdWriteTimestamp)              \
    X(vkCmdCopyQueryPoolResults) X(vkCmdBeginConditionalRenderingEXT) X(vkCmdEndConditionalRenderingEXT) \
    X(vkCmdBeginDebugUtilsLabelEXT) X(vkCmdEndDebugUtilsLabelEXT) X(vkGetDeviceFaultInfoEXT) X(vkCmdSetCheckpointNV) X(vkGetQueueCheckpointDataNV)

namespace vkfn {
#define VKFN_DECLARE(name) inline PFN_##name name = nullptr;
VKFN_GLOBAL(VKFN_DECLARE)
VKFN_INSTANCE(VKFN_DECLARE)
VKFN_DEVICE(VKFN_DECLARE)
#undef VKFN_DECLARE
inline PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr = nullptr;

bool load_global();                // vulkan-1.dll; false if absent
void load_instance(VkInstance i);  // entry points left null when the instance lacks them
void load_device(VkDevice d);
}  // namespace vkfn

using namespace vkfn;
