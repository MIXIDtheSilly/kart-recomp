// The Vulkan replay's draw path: NVN binding state per queue, the descriptor tables (RegisterTexture /
// RegisterSampler), pipelines, and the draws and dispatches themselves.
//
// Pipelines are keyed only by their shaders and attachment formats: everything else NVN can change between
// draws is dynamic state (extended dynamic state 1-3, dynamic vertex input). Each draw gets a descriptor set
// per stage from the submission's arena, with the driver segments (c0, c2), push constants and the
// translator's support buffer written into the arena's upload memory. Texture content the GPU has not made
// is uploaded from guest memory in the submission's pre-command buffer, which runs first.
#pragma once
#include "../nvn_obj.h"
#include "vkctx.h"

namespace nvk {

void draw_init();

// Submission bracketing (replay_vk.cpp). sub_begin: `done` = newest completed timeline value (arenas of
// completed submissions are reused). sub_commit: the command buffer to submit before the main one, or
// null; `value` is the timeline value the submission signals.
void draw_sub_begin(u64 done);
VkCommandBuffer draw_sub_commit(u64 value);
// A partial flush of the submission (replay_vk.cpp flush_partial): its uploads so far, ended (null if none); the
// uploads after go to a new command buffer, and the dynamic state is set again in the next command buffer.
VkCommandBuffer draw_sub_flush();

void draw_queue_activate(u64 serial);
void draw_queue_forget(u64 serial);

// Binding records (everything rp_exec does not handle itself). False: not a binding record.
bool draw_state(u32 op, const u8* payload, u32 size);
// OP_DRAW / OP_DRAW_IDX / OP_DISPATCH / OP_DRAW_TEX. `next`: the record after this one (a multi-draw looks ahead).
void draw(VkCommandBuffer cb, u32 op, const u8* payload, u32 size, const u8* next);

// Descriptor tables and objects.
void draw_set_tex_pool_n(int n);
void draw_reg_tex(int id, const nvn::tex_t* t, const nvn::view_t* v, bool image);
void draw_reg_samp(int id, const nvn::samp_par* p);
void draw_prog_release(const nvn::prog_t* p);
void draw_prog_forget(const nvn::prog_t* p);
void draw_tex_cpu_written(const nvn::tex_t* t);

// The submission's pre-command buffer (runs before the main one): new images' layout transitions and uploads go here.
VkCommandBuffer draw_pre_cb();
// Uploads img's content from t's storage when the GPU has not made it (once).
struct Image;
void draw_ensure_content(Image* img, const nvn::tex_t* t);

// The active queue's scissor rectangle within the current render area (clears honour it, as on NVN).
VkRect2D draw_scissor();

// False while the active queue has rendering turned off (SetRenderEnable(false)): draws and clears are skipped.
bool draw_render_enabled();
// A report / event write replayed in this submission (applied to memory when it completes): render conditions later
// in the stream read memory through these.
void draw_note_write(volatile void* host, const void* data, u32 size);
void draw_cb_changed();   // the commands that follow go into another command buffer state: forget what is bound
u32 draw_written_word(volatile void* host);
// Draws issued so far (for the dump trigger).
u64 draw_total();

// One log line of draw statistics since the last call.
void draw_log_stats();

}  // namespace nvk
