// NVN textures in sparse virtual pools that are too big to create whole (the terrain caches: 8192x8192 arrays of 68
// layers, mostly unmapped) as Vulkan sparse-residency images. The game maps 64 KB tiles of their storage (MapVirtual);
// each mapped tile gets device memory bound at its texels and its content uploaded from the physical pages behind it,
// an unmapped one is unbound again (reads 0, as on the console). The levels past the tiled ones (the mip tail) stay
// bound and are uploaded a layer at a time.
#pragma once
#include "textures.h"

namespace nvk {

// The sparse image of t, shared by every texture over the same storage and shape; null when it cannot be made.
// `cb`: where a new image's layout transition goes. `mostly_unmapped`: only if less than a quarter of t's storage has
// memory behind it (sparse images are only sampled, and the game renders to a half-mapped one).
Image* sparse_image_of(const nvn::tex_t* t, VkCommandBuffer cb, bool mostly_unmapped = false);
// A texture using img is gone; the image goes with the last one (once the GPU is done with it).
void sparse_image_release(Image* img);
// A use of img in the submission being recorded: tiles mapped, remapped or unmapped since are bound / unbound and
// their content uploaded from storage in `pre`, all of them before this use (the game fills fresh tiles with GPU copies
// right after mapping them: a copy into a tile not bound yet would be lost, an upload after it would overwrite it).
void sparse_image_use(Image* img, VkCommandBuffer pre);
// rp_vpool_map: [gpu, gpu + size) of a sparse pool shows other memory now. Any thread, with the interface's sparse lock
// held (so this only notes the range).
void sparse_note_map(u64 gpu, u64 size);
// rp_sub_commit, right before the submission: this submission's binds go to the queue. Returns the value of *sem the
// submission waits for, 0 = nothing to wait for.
u64 sparse_commit(VkSemaphore* sem);

// Memory bound to sparse images now, in bytes.
u64 sparse_bytes();
// Once a submission (after it began): sparse images not used for HOST_NVN_EVICT_S seconds (default 60, 0 = never) are
// moved out to host memory while device memory is short, and back at their next use (sparse_image_use). `pre`: the
// submission's command buffer before the others.
void sparse_tick(VkCommandBuffer (*pre)());
// Memory bound to sparse image img (tiles and tail).
u64 sparse_image_bytes(const Image* img);

}  // namespace nvk
