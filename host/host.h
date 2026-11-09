// Host-wide configuration and the hooks that replace SDK functions with native code.
#pragma once
#include <filesystem>

#include "common.h"

struct HostConfig {
    std::filesystem::path game_dir;  // dump root: exefs/ and romfs/
    std::filesystem::path user_dir;  // saves, logs
};
extern HostConfig g_config;

// Calls to `addr` through call_indirect run `fn` instead of the recompiled function.
void host_override(u64 addr, GuestFn fn);
GuestFn recompiled_fn(u64 addr);  // the recompiled function at `addr`, ignoring overrides
u64 module_base(const char* name);
u64 sdk_export(const char* name);  // address of an exported SDK symbol, 0 if absent
void host_install_overrides();
