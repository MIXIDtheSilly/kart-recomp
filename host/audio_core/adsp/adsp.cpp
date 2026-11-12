// SPDX-FileCopyrightText: Copyright 2025 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2023 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "audio_core/adsp/adsp.h"
#include "audio_core/host.h"

namespace AudioCore::ADSP {

ADSP::ADSP(HostSystem& system, Sink::Sink& sink) {
    audio_renderer.emplace(system, sink);
}

AudioRenderer::AudioRenderer& ADSP::AudioRenderer() {
    return *audio_renderer;
}

} // namespace AudioCore::ADSP
