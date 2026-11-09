// Native graphics: nv::, nn::vi:: and NVN (on Vulkan).
#pragma once
#include <functional>
#include <initializer_list>
#include <string>

#include "common.h"

namespace gfx {

using NativeFn = std::function<void(Ctx*, const std::string& name)>;

// A guest-callable address that runs `fn` (the same address for the same name).
u64 native_function(const std::string& name, NativeFn fn);
// Calls a guest function with integer arguments; returns x0. Registers are preserved.
u64 guest_call(Ctx* c, u64 fn, std::initializer_list<u64> args);
u64 sdk_fn(const char* name);
void attach_system_event(Ctx* c, u64 event_type, u32 handle, bool auto_clear);

void install();       // graphics.cpp
void install_display();  // graphics.cpp: nn::vi stand-ins only (native NVN)
void install_nvn();   // nvn.cpp
void install_nvn_shim();  // nvn_shim.cpp: HOST_NVN_SHIM=1

}  // namespace gfx
