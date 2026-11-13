// Texture content from guest memory: the game's packaged (block-linear) or CPU-written texture storage,
// untiled (and ASTC-decoded) into the texture's image.
#pragma once
#include <filesystem>
#include "textures.h"

namespace nvk {

// Copies every level and layer of t's storage into img, recorded into cb (outside a render pass).
// False when the storage cannot be read (logged once per reason).
bool upload_from_storage(Image* img, const nvn::tex_t* t, VkCommandBuffer cb);

// ASTC textures (decoded and recompressed on the CPU: up to a second for the large ones) upload on a worker thread
// instead: the storage is copied now and img->pending set. False = not an upload for the worker (use
// upload_from_storage). HOST_NVN_SYNC_UPLOADS=1 turns it off.
bool upload_async_start(Image* img, const nvn::tex_t* t);
// img's pending upload, if any: once prepared, recorded into cb and true; false while it is still being prepared
// (after a short wait for it, within what the frame may spend: HOST_NVN_UPLOAD_WAIT_MS).
// `wait`: prepare it now (in this thread when the worker has not started it) and record it.
bool upload_async_poll(Image* img, VkCommandBuffer cb, bool wait);
// The game created t (any thread): a big ASTC texture's prepared upload read from the disk cache now on a worker, for
// its first use to take (HOST_NVN_NO_PREFETCH=1: not).
void upload_prefetch(const nvn::tex_t* t);
// A memory pool goes (its storage may be freed next): no prefetch reads it any more once this returns.
void upload_prefetch_drop(const u8* mem, u64 size);
// img's pending upload is not wanted any more (its storage changed again, or img goes).
void upload_async_cancel(Image* img);

// kart.exe <game> --bake-textures [n]: every ASTC texture of the game's .bntx.zs files decoded into the texture cache,
// so none is decoded during play; n > 0: only counts which of n files' textures the cache has. The process's exit code.
int bake_textures(const std::filesystem::path& game, size_t check);

}  // namespace nvk
