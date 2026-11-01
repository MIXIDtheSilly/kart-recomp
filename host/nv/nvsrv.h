// nx2:nvsrv, the NVIDIA driver server (nvdrv devices), handed out by gsv 1402.
#pragma once
#include "ipc.h"

namespace nv {
ipc::ServicePtr make_nvsrv();

// Syncpoints of the driver's pool, for server-side work the guest waits on (video decoding).
u32 allocate_syncpoint();
void free_syncpoint(u32 id);
}
