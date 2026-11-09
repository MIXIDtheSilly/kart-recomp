// Texture copies of the command stream (copies.cpp).
#pragma once
#include "../nvn_obj.h"
#include "vkctx.h"

namespace nvk {

void copy_stride(const nvn::p_copy_stride* c);
// OP_COPY_B2T (to_texture) / OP_COPY_T2B: the record's src is the buffer address either way.
void copy_buffer_texture(VkCommandBuffer cb, const nvn::p_copy_b2t* c, bool to_texture);
void copy_texture_texture(VkCommandBuffer cb, const nvn::p_copy_t2t* c);

}  // namespace nvk
