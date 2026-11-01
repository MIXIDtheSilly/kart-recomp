// The nvdrv devices' events: Kernel::KEvent as the devices use it, backed by a host kernel event.
#pragma once
#include "kernel.h"

namespace Kernel {

class KEvent {
public:
    void Signal() { ev.signal(); }
    void Clear() { ev.clear(); }
    // A copy handle to the readable side for the guest.
    u32 NewHandle() { return ev.new_handle(); }

private:
    kern::ServiceEvent ev;
};

}  // namespace Kernel
