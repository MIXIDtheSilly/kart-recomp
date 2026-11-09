// The Vulkan renderer behind replay.h runs on a thread of its own (replay_async.cpp): each rp_* call becomes a step of
// one queue, done in call order on that thread, so the game's submitting thread does not wait for the Vulkan
// recording (milliseconds a frame). The vk_rp_* functions (replay_vk.cpp) are the renderer itself; replay_async.cpp
// defines the rp_* entry points over them.
#pragma once
#include <vector>

#include "../replay.h"

namespace nvn {

// True when submissions are replayed on the renderer thread (HOST_NVN_SYNC_REPLAY=1: on the calling thread, as before).
// Then the fence-sync records of a submission are fenced when it is queued, not when it is replayed.
bool async_replay();

void vk_rp_set_origin_lower_left(int on);
void vk_rp_set_depth_zero(int on);
void vk_rp_set_queue_origin_lower_left(int on);
void vk_rp_pool_add(u64 gpu, u64 size);
void vk_rp_pool_del(u64 gpu);
void vk_rp_alias_drop_pool(const pool_t* pool);
void vk_rp_vpool_create(u64 base, u64 size);
void vk_rp_vpool_map(u64 base, u64 voff, u64 size, u64 phys);
void vk_rp_vpool_destroy(u64 base);
void vk_rp_set_tex_pool_n(int n);
void vk_rp_reg_tex(int id, const tex_t* t, const view_t* v);
void vk_rp_reg_image(int id, const tex_t* t, const view_t* v);
void vk_rp_reg_samp(int id, const samp_par* p);
void vk_rp_tex_init(const tex_t* t);
void vk_rp_tex_release(tex_t* t);
void vk_rp_tex_cpu_written(const tex_t* t);
int vk_rp_tex_gpu_owned(uint64_t serial);
void vk_rp_read_texels(const tex_t* t, unsigned level, unsigned layer0, const NVNcopyRegion* r, uint8_t* dst, size_t row, size_t img);
void vk_rp_prog_release(prog_t* p);
void vk_rp_prog_prefetch(const prog_t* p);
void vk_rp_prog_forget(const prog_t* p);
void vk_rp_queue_activate(uint64_t serial);
void vk_rp_queue_forget(uint64_t serial);
void vk_rp_prepare();
void vk_rp_sub_begin();
void vk_rp_exec(const uint8_t* handle);
void vk_rp_exec_rest(const uint64_t* handles, int n);
uint64_t vk_rp_sub_commit();
uint64_t vk_rp_last_fence();
void vk_rp_finish();
void vk_rp_drain();
// A submission whose event waits may hold it back until the one that signals them comes (replay_vk.cpp). fence: the value rp_sub_commit gave it.
void vk_rp_submit(u64 queue, std::vector<const uint8_t*> handles, u64 fence);
bool vk_rp_holding();
void vk_rp_force_held();
void rp_force_held();   // queues vk_rp_force_held
uint64_t vk_rp_present(const tex_t* t, double period_s, const int crop[4]);

}  // namespace nvn
