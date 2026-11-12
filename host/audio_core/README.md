# audio_core

The game's audio renderer: voices, mixes, effects and the DSP command lists that render them.

Adapted from the audio renderer of Eden (https://git.eden-emu.dev/eden-emu/eden), itself derived
from yuzu. Those files keep their original copyright and SPDX lines (GPL-2.0-or-later or
GPL-3.0-or-later); this repository is GPL-3.0 as a whole.

What changed from the original:

- Only the renderer path is kept: no audio in/out managers, no hardware Opus app (the host decodes
  Opus itself in `services/opus.cpp`), no cubeb or SDL3 sinks.
- The emulator core it used (Core::System, the kernel's process, event and transfer memory, guest
  memory, settings, logging, the result type) is replaced by our own small stand-ins in `host.h`,
  defined by `host.cpp` and the host's `services/audio.cpp`.
- `sink/wasapi_sink.*` is new: output through WASAPI.
- `base/` holds four header-only helpers copied unchanged from Eden's `common/` (fixed point,
  ring buffer, bounded queues, a stoppable wait).
