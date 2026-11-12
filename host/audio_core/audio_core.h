// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2022 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Adapted for the kart host: only the audio renderer's output sink and the ADSP; no audio in/out.

#pragma once

#include <memory>
#include <optional>

#include "audio_core/adsp/adsp.h"
#include "audio_core/host.h"
#include "audio_core/sink/sink.h"

namespace AudioCore {

/// Holds the output sink and the ADSP that renders into it.
class AudioCore {
public:
    explicit AudioCore(HostSystem& system);
    ~AudioCore();

    Sink::Sink& GetOutputSink();

    ADSP::ADSP& ADSP();

private:
    std::unique_ptr<Sink::Sink> output_sink;
    std::optional<ADSP::ADSP> adsp;
};

} // namespace AudioCore
