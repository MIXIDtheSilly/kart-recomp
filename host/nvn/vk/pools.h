// Memory pools as the GPU sees them: a GPU address (= guest address) -> the Vulkan buffer holding it.
#pragma once
#include "vkctx.h"

namespace nvk {

struct BufferRef {
    VkBuffer buffer = VK_NULL_HANDLE;
    u64 offset = 0;          // of the address in buffer
    u64 size = 0;            // bytes from offset to the end of the buffer
    VkDeviceAddress gpu = 0; // device address of the guest address
};

// A pool's storage [addr, addr + size) (NVN pool Initialize / Finalize). Physical pools only.
void pool_add(u64 addr, u64 size);
void pool_del(u64 addr);

// The buffer behind [addr, addr + len); false if no pool holds all of it.
bool pool_find(u64 addr, u64 len, BufferRef& out);

}  // namespace nvk
