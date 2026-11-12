// Vulkan Memory Allocator (third_party/vma, MIT), with the entry points it needs found at run time through
// vkGetInstanceProcAddr / vkGetDeviceProcAddr (the renderer has no Vulkan import library). nvn/vk/vma.cpp holds
// the implementation.
#pragma once
#include "vkfn.h"

#define VMA_STATIC_VULKAN_FUNCTIONS 0
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 1

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4189)
#endif
#include "vk_mem_alloc.h"
#ifdef _MSC_VER
#pragma warning(pop)
#endif
