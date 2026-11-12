// Native NVN: what the implementation gets from the host.
#pragma once
#include <initializer_list>
#include <string>

#include "../common.h"
#include "../kernel.h"

namespace nvn {

void install();                      // take over nvnBootstrapLoader (dispatch.cpp)
u64 lookup(const std::string& name); // guest-callable address of an NVN function (stub if not implemented)
Ctx* guest_ctx();                    // the guest thread inside the current NVN call
// Calls a guest function (a callback the game gave NVN) from inside an NVN call; returns x0.
u64 call_guest(u64 fn, std::initializer_list<u64> args);

// Waits that may block a guest thread for a while (fences, syncs) go inside one of these, so the
// thread's core runs other guest threads meanwhile.
using HostWait = kern::HostWait;

}  // namespace nvn
