// SPDX-License-Identifier: GPL-3.0-or-later
// The Windows parts of audio_core/host.h.
#include <windows.h>

#include <cstdlib>
#include <cstring>
#include <string>

#include "audio_core/host.h"

namespace AudioCore {

void SetThreadName(const char* name) {
    const std::wstring wide(name, name + std::strlen(name));
    SetThreadDescription(GetCurrentThread(), wide.c_str());
}

void SetThreadPriority(ThreadPriority priority) {
    int value = THREAD_PRIORITY_NORMAL;
    switch (priority) {
    case ThreadPriority::Low: value = THREAD_PRIORITY_BELOW_NORMAL; break;
    case ThreadPriority::Normal: value = THREAD_PRIORITY_NORMAL; break;
    case ThreadPriority::High: value = THREAD_PRIORITY_ABOVE_NORMAL; break;
    case ThreadPriority::VeryHigh: value = THREAD_PRIORITY_HIGHEST; break;
    case ThreadPriority::Critical: value = THREAD_PRIORITY_TIME_CRITICAL; break;
    }
    ::SetThreadPriority(GetCurrentThread(), value);
}

f32 OutputVolume() { return 1.0f; }

bool OutputMuted() {
    static const bool muted = [] {
        const char* mute = std::getenv("HOST_AUDIO_MUTE");
        return mute && mute[0] == '1';
    }();
    return muted;
}

}  // namespace AudioCore
