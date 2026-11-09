// nx2:nvsrv, the NVIDIA driver server (nvdrv devices), handed out by gsv 1402.
#pragma once
#include <utility>
#include <vector>
#include "ipc.h"

namespace nv {
ipc::ServicePtr make_nvsrv();

// Syncpoints of the driver's pool, for server-side work the guest waits on (video decoding).
u32 allocate_syncpoint();
void free_syncpoint(u32 id);

// The fence (syncpoint id, value) of each GPU channel's latest submit with one: the GPU has run
// everything a channel submitted once its fence is reached.
std::vector<std::pair<u32, u32>> latest_channel_fences();
}
