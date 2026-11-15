// SPDX-License-Identifier: GPL-3.0-or-later
// Audio output through SDL 3 (Linux). Each stream is an SDL audio stream on the default output device: SDL's audio
// thread calls back whenever the device wants more, and the callback fills that much from the sink stream's queue, so
// the device sets the pace as WASAPI's buffer event does on Windows. SDL follows the default device by itself (headphones
// plugged in or out).
#ifndef _WIN32
#include <SDL3/SDL.h>

#include <mutex>
#include <vector>

#include "audio_core/common/common.h"
#include "audio_core/sink/sdl_sink.h"

namespace AudioCore::Sink {
namespace {

constexpr u32 OutputChannels = 2;

class SdlSinkStream final : public SinkStream {
public:
    SdlSinkStream(HostSystem& system_, u32 system_channels_, const std::string& name_, StreamType type_)
        : SinkStream(system_, type_) {
        name = name_;
        device_channels = OutputChannels;
        system_channels = system_channels_;
        const SDL_AudioSpec spec{SDL_AUDIO_S16, static_cast<int>(OutputChannels), static_cast<int>(TargetSampleRate)};
        stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, &SdlSinkStream::Pull, this);
        if (!stream) {
            LOG_ERROR(Audio, "audio: could not open the output device for {}: {}", name, SDL_GetError());
            return;
        }
        SDL_AudioSpec dev{};
        int frames = 0;
        SDL_GetAudioDeviceFormat(SDL_GetAudioStreamDevice(stream), &dev, &frames);
        LOG_INFO(Audio, "audio: output {} through SDL {} ({} Hz, {} channels; device {} Hz, {} frames a period)", name,
                 SDL_GetCurrentAudioDriver() ? SDL_GetCurrentAudioDriver() : "?", TargetSampleRate, OutputChannels, dev.freq, frames);
    }

    ~SdlSinkStream() override { Finalize(); }

    void Finalize() override {
        if (!stream) return;
        Stop();
        SDL_DestroyAudioStream(stream);   // (waits for a callback in progress)
        stream = nullptr;
    }

    void Start(bool resume = false) override {
        if (!paused || !stream) return;
        paused = false;
        SDL_ResumeAudioStreamDevice(stream);
    }

    void Stop() override {
        if (paused) return;
        SignalPause();
        if (stream) SDL_PauseAudioStreamDevice(stream);
    }

private:
    // SDL's audio thread: the device wants `additional` more bytes.
    static void SDLCALL Pull(void* self_, SDL_AudioStream* s, int additional, int) {
        auto* self = static_cast<SdlSinkStream*>(self_);
        const size_t frames = static_cast<size_t>(additional) / (OutputChannels * sizeof(s16));
        if (!frames) return;
        self->buffer.resize(frames * OutputChannels);
        self->ProcessAudioOutAndRender(std::span<s16>(self->buffer.data(), self->buffer.size()), frames);
        if (OutputMuted()) std::fill(self->buffer.begin(), self->buffer.end(), s16{0});
        SDL_PutAudioStreamData(s, self->buffer.data(), static_cast<int>(self->buffer.size() * sizeof(s16)));
    }

    SDL_AudioStream* stream = nullptr;
    std::vector<s16> buffer;   // (SDL's audio thread only)
};

class SdlSink final : public Sink {
public:
    SdlSink() { device_channels = OutputChannels; }
    ~SdlSink() override { CloseStreams(); }

    SinkStream* AcquireSinkStream(HostSystem& system, u32 system_channels_, const std::string& name,
                                  StreamType type) override {
        std::scoped_lock lk{lock};
        system_channels = system_channels_;
        auto stream = std::make_unique<SdlSinkStream>(system, system_channels_, name, type);
        stream->SetDeviceVolume(device_volume);
        streams.push_back(std::move(stream));
        return streams.back().get();
    }

    void CloseStream(SinkStream* stream) override {
        std::scoped_lock lk{lock};
        std::erase_if(streams, [&](const SinkStreamPtr& s) { return s.get() == stream; });
    }

    void CloseStreams() override {
        std::scoped_lock lk{lock};
        streams.clear();
    }

    f32 GetDeviceVolume() const override { return device_volume; }

    void SetDeviceVolume(f32 volume) override {
        std::scoped_lock lk{lock};
        device_volume = volume;
        for (auto& s : streams) s->SetDeviceVolume(volume);
    }

    void SetSystemVolume(f32 volume) override {
        std::scoped_lock lk{lock};
        for (auto& s : streams) s->SetSystemVolume(volume);
    }

private:
    std::mutex lock;
    std::vector<SinkStreamPtr> streams;
};

}  // namespace

std::unique_ptr<Sink> CreateSdlSink() {
    // (video_sdl.cpp started SDL's audio on the main thread; it fails without any output device)
    if (!(SDL_WasInit(SDL_INIT_AUDIO) & SDL_INIT_AUDIO)) return nullptr;
    int n = 0;
    SDL_free(SDL_GetAudioPlaybackDevices(&n));
    if (n == 0) return nullptr;
    return std::make_unique<SdlSink>();
}

}  // namespace AudioCore::Sink
#endif
