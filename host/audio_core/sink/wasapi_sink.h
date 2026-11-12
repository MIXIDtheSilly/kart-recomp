// SPDX-License-Identifier: GPL-3.0-or-later
// Audio output through WASAPI (shared mode, 48 kHz 16-bit stereo, event driven).
#pragma once

#include <memory>

#include "audio_core/sink/sink.h"

namespace AudioCore::Sink {

// The default output device, or nullptr when there is none.
std::unique_ptr<Sink> CreateWasapiSink();

}  // namespace AudioCore::Sink
