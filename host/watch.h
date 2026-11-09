// Guest write watch for the native NVN renderer (debug aid): CPU stores into chosen guest buffers are
// trapped one by one (the guest view's pages are write-protected; each store runs against the page's
// backing memory, single-stepped) and logged per game frame with the host call stack of the store,
// which names the recompiled guest functions that wrote the bytes.
// HOST_WATCH_SSBO=<shader>:<slot>[,...]: arm on the storage buffers a draw binds in that slot of that
// shader (the key a dump prints, e.g. 02cd0a028c99bc3b:2; uniform buffer slot n is 100 + n). HOST_WATCH_LINES caps the log (default 200000).
#pragma once
#include "common.h"

namespace watch {

// Whether HOST_WATCH_SSBO names this shader and storage slot.
bool wanted(const char* shader, u32 slot);
// Starts watching [addr, addr + size) (no-op when already watched).
void add(u64 addr, u64 size);
// A draw binds the watched buffer at `addr` (renderer thread): logs 16-byte units that changed since its last bind without a CPU store.
void bound(u64 addr);
// GPU work touching memory (renderer thread): logs it once per kind when [addr, addr + size) overlaps a watched range.
void gpu_access(const char* what, const char* shader, u32 slot, u64 addr, u64 size);
// From the exception handler: a write fault at guest address `guest`, a single-step trap.
bool on_write_fault(u64 guest, void* context);
bool on_single_step(void* context);

}  // namespace watch
