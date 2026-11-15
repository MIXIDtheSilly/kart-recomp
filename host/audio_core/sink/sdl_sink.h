// SPDX-License-Identifier: GPL-3.0-or-later
// Audio output through SDL 3 on Linux (PipeWire, PulseAudio or ALSA, whichever SDL finds; 48 kHz 16-bit stereo).
#pragma once

#include <memory>

#include "audio_core/sink/sink.h"

namespace AudioCore::Sink {

// The default output device, or nullptr when there is none.
std::unique_ptr<Sink> CreateSdlSink();

}  // namespace AudioCore::Sink
