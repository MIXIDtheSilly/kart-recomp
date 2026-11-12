// SPDX-License-Identifier: GPL-3.0-or-later
// Audio output through WASAPI. Each stream owns a render thread that does all the COM work: it opens
// the default output device in shared mode (48 kHz, 16-bit, stereo; Windows converts to the mix
// format), waits on the buffer event and fills what the device consumed from the sink stream's
// queue. When the device goes away (headphones unplugged) it opens the new default device.
#include <windows.h>
#include <audioclient.h>
#include <avrt.h>
#include <mmdeviceapi.h>

#include <algorithm>
#include <future>
#include <mutex>
#include <thread>
#include <vector>

#include "audio_core/common/common.h"
#include "audio_core/sink/wasapi_sink.h"

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "avrt.lib")

namespace AudioCore::Sink {
namespace {

template <class T> void release(T*& p) {
    if (p) p->Release();
    p = nullptr;
}

constexpr u32 OutputChannels = 2;
constexpr REFERENCE_TIME BufferDuration = 20 * 10000;  // 20 ms in 100 ns units

bool has_output_device() {
    IMMDeviceEnumerator* enumerator = nullptr;
    IMMDevice* device = nullptr;
    const bool com = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
    const bool ok = SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                               __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(&enumerator))) &&
                    SUCCEEDED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device));
    release(device);
    release(enumerator);
    if (com) CoUninitialize();
    return ok;
}

class WasapiSinkStream final : public SinkStream {
public:
    WasapiSinkStream(HostSystem& system_, u32 system_channels_, const std::string& name_, StreamType type_)
        : SinkStream(system_, type_) {
        name = name_;
        device_channels = OutputChannels;
        system_channels = system_channels_;
        wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        std::promise<bool> opened;
        auto result = opened.get_future();
        thread = std::thread([this, &opened] { Run(opened); });
        if (!result.get()) LOG_ERROR(Audio, "audio: could not open the output device for {}", name);
    }

    ~WasapiSinkStream() override { Finalize(); }

    void Finalize() override {
        if (!thread.joinable()) return;
        Stop();
        quit = true;
        SetEvent(wake);
        thread.join();
        CloseHandle(wake);
    }

    void Start(bool resume = false) override {
        if (!paused) return;
        paused = false;
        SetEvent(wake);
    }

    void Stop() override {
        if (paused) return;
        SignalPause();
        SetEvent(wake);
    }

private:
    bool Open() {
        IMMDeviceEnumerator* enumerator = nullptr;
        IMMDevice* device = nullptr;
        HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                                      reinterpret_cast<void**>(&enumerator));
        if (SUCCEEDED(hr)) hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
        if (SUCCEEDED(hr))
            hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&client));
        release(device);
        release(enumerator);
        if (FAILED(hr)) {
            Close();
            return false;
        }

        WAVEFORMATEX format{};
        format.wFormatTag = WAVE_FORMAT_PCM;
        format.nChannels = OutputChannels;
        format.nSamplesPerSec = TargetSampleRate;
        format.wBitsPerSample = 16;
        format.nBlockAlign = static_cast<WORD>(format.nChannels * format.wBitsPerSample / 8);
        format.nAvgBytesPerSec = format.nSamplesPerSec * format.nBlockAlign;
        hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                                    AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
                                BufferDuration, 0, &format, nullptr);
        if (SUCCEEDED(hr)) hr = client->SetEventHandle(buffer_event);
        if (SUCCEEDED(hr)) hr = client->GetBufferSize(&buffer_frames);
        if (SUCCEEDED(hr)) hr = client->GetService(__uuidof(IAudioRenderClient), reinterpret_cast<void**>(&render));
        if (FAILED(hr)) {
            LOG_ERROR(Audio, "audio: WASAPI setup failed: {:#x}", static_cast<u32>(hr));
            Close();
            return false;
        }
        LOG_INFO(Audio, "audio: output {} ({} Hz, {} channels, buffer {} frames)", name, TargetSampleRate,
                 OutputChannels, buffer_frames);
        running = false;
        return true;
    }

    void Close() {
        if (client && running) client->Stop();
        running = false;
        release(render);
        release(client);
    }

    // Fills the free part of the device buffer. False when the device went away.
    bool Fill() {
        UINT32 padding = 0;
        if (FAILED(client->GetCurrentPadding(&padding))) return false;
        const UINT32 frames = buffer_frames - padding;
        if (frames == 0) return true;
        BYTE* data = nullptr;
        if (FAILED(render->GetBuffer(frames, &data))) return false;
        ProcessAudioOutAndRender(std::span<s16>(reinterpret_cast<s16*>(data), frames * OutputChannels), frames);
        return SUCCEEDED(render->ReleaseBuffer(frames, OutputMuted() ? AUDCLNT_BUFFERFLAGS_SILENT : 0));
    }

    void Run(std::promise<bool>& opened) {
        SetThreadName("AudioOutput");
        const bool com = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
        DWORD task_index = 0;
        HANDLE task = AvSetMmThreadCharacteristicsW(L"Pro Audio", &task_index);
        buffer_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        opened.set_value(Open());

        const HANDLE events[2] = {wake, buffer_event};
        while (!quit) {
            if (!client) {
                // Device lost or never opened: try again every second.
                WaitForSingleObject(wake, 1000);
                if (!quit && Open()) LOG_INFO(Audio, "audio: output device reopened");
                continue;
            }
            const bool want = !paused;
            if (want != running) {
                if (want) {
                    Fill();  // the first period starts full
                    running = SUCCEEDED(client->Start());
                } else {
                    client->Stop();
                    running = false;
                }
            }
            WaitForMultipleObjects(2, events, FALSE, running ? 200 : INFINITE);
            if (running && !quit && !Fill()) {
                LOG_WARNING(Audio, "audio: output device lost");
                Close();
            }
        }
        Close();
        CloseHandle(buffer_event);
        if (task) AvRevertMmThreadCharacteristics(task);
        if (com) CoUninitialize();
    }

    std::thread thread;
    HANDLE wake = nullptr;
    HANDLE buffer_event = nullptr;
    IAudioClient* client = nullptr;
    IAudioRenderClient* render = nullptr;
    UINT32 buffer_frames = 0;
    bool running = false;
    std::atomic<bool> quit{false};
};

class WasapiSink final : public Sink {
public:
    WasapiSink() { device_channels = OutputChannels; }
    ~WasapiSink() override { CloseStreams(); }

    SinkStream* AcquireSinkStream(HostSystem& system, u32 system_channels_, const std::string& name,
                                  StreamType type) override {
        std::scoped_lock lk{lock};
        system_channels = system_channels_;
        auto stream = std::make_unique<WasapiSinkStream>(system, system_channels_, name, type);
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

std::unique_ptr<Sink> CreateWasapiSink() {
    if (!has_output_device()) return nullptr;
    return std::make_unique<WasapiSink>();
}

}  // namespace AudioCore::Sink
