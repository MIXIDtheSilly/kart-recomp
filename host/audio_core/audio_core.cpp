// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2022 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "audio_core/audio_core.h"
#include "audio_core/sink/null_sink.h"
#include "audio_core/sink/wasapi_sink.h"

namespace AudioCore {

AudioCore::AudioCore(HostSystem& system) {
    output_sink = Sink::CreateWasapiSink();
    if (!output_sink) {
        LOG_ERROR(Audio, "audio: no output device, rendering without sound");
        output_sink = std::make_unique<Sink::NullSink>("null");
    }
    // Must be created after the sink
    adsp.emplace(system, *output_sink);
}

AudioCore::~AudioCore() = default;

Sink::Sink& AudioCore::GetOutputSink() {
    return *output_sink;
}

ADSP::ADSP& AudioCore::ADSP() {
    return *adsp;
}

// Never destroyed: its sink and ADSP threads would block process exit.
AudioCore& HostSystem::AudioCore() {
    static auto* core = new ::AudioCore::AudioCore(*this);
    return *core;
}

HostSystem& GetHostSystem() {
    static auto* system = new HostSystem();
    return *system;
}

} // namespace AudioCore
