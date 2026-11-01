// audren: Eden's audio renderer (audio_core.lib) runs the game's voices, mixes and effects and plays
// the result through cubeb, paced by the output stream. The host provides the few kernel members
// audio_core calls: its rendered event, the process (only for guest memory) and the transfer
// memory's address.
#include <windows.h>

#include <chrono>

#include "audio_core/audio_core.h"
#include "audio_core/audio_render_manager.h"
#include "audio_core/common/audio_renderer_parameter.h"
#include "audio_core/renderer/audio_renderer.h"
#include "core/core.h"
#include "core/core_timing.h"
#include "core/hle/kernel/k_event.h"
#include "core/hle/kernel/k_process.h"
#include "core/hle/kernel/k_transfer_memory.h"

#include "../ipc.h"
#include "../video.h"

// ------------------------------------------------------------------ the emulator core, as audio_core sees it

namespace {

// Stand-ins for audio_core's kernel objects. It calls only out-of-line members on them (defined
// below) and the inline KProcess::GetMemory / KTransferMemory::GetSourceAddress: the process is
// zeroed memory whose embedded Core::Memory::Memory is the host's (its members ignore `this`), the
// transfer memory holds its address in every word, so the field reads it wherever it lies.
struct RenderedEvent {
    alignas(Kernel::KEvent) unsigned char object[sizeof(Kernel::KEvent)]{};
    kern::ServiceEvent event;
    Kernel::KEvent* get() { return reinterpret_cast<Kernel::KEvent*>(object); }
};

Kernel::KProcess* fake_process() {
    alignas(Kernel::KProcess) static unsigned char process[sizeof(Kernel::KProcess)]{};
    return reinterpret_cast<Kernel::KProcess*>(process);
}

struct TransferMemory {
    alignas(Kernel::KTransferMemory) u64 words[(sizeof(Kernel::KTransferMemory) + 7) / 8];
    explicit TransferMemory(u64 address) { std::fill(std::begin(words), std::end(words), address); }
    Kernel::KTransferMemory* get() { return reinterpret_cast<Kernel::KTransferMemory*>(words); }
};

const auto g_start = std::chrono::steady_clock::now();

}  // namespace

Result Kernel::KEvent::Signal(KernelCore&) {
    reinterpret_cast<RenderedEvent*>(this)->event.signal();
    return ResultSuccess;
}

Result Kernel::KEvent::Clear(KernelCore&) {
    reinterpret_cast<RenderedEvent*>(this)->event.clear();
    return ResultSuccess;
}

// Never destroyed: its sink and ADSP threads would block process exit.
::AudioCore::AudioCore& Core::System::AudioCore() {
    static auto* core = new ::AudioCore::AudioCore(*this);
    return *core;
}

Kernel::KernelCore& Core::System::Kernel() {
    alignas(16) static unsigned char kernel[16]{};  // only passed back to the members above
    return *reinterpret_cast<Kernel::KernelCore*>(kernel);
}

std::chrono::nanoseconds Core::Timing::CoreTiming::GetGlobalTimeNs() const noexcept {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - g_start);
}

std::chrono::microseconds Core::Timing::CoreTiming::GetGlobalTimeUs() const noexcept {
    return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - g_start);
}

namespace ipc {
namespace {

using Params = AudioCore::AudioRendererParameterInternal;

std::shared_ptr<AudioCore::Renderer::Manager> manager() {
    static auto m = std::make_shared<AudioCore::Renderer::Manager>(video::system());
    return m;
}

struct AudioRenderer : Service {
    AudioRenderer(const Params& params, u64 tmem_address, u64 tmem_size, u64 aruid, s32 session_id)
        : Service("IAudioRenderer"), impl(video::system(), *manager(), rendered.get()) {
        TransferMemory tmem{tmem_address};
        const auto res = impl.Initialize(params, tmem.get(), tmem_size, fake_process(), aruid, session_id);
        LOG("audren: session %d: %u Hz, %u samples, %u voices, %u mixes, %u sinks, rev %u: %#x", session_id,
            params.sample_rate, params.sample_count, params.voices, params.mixes, params.sinks,
            ((params.revision >> 24) & 0xff) - '0', res.raw);

        on(0, [this](Request& r) { r.push<u32>(impl.GetSystem().GetSampleRate()); });
        on(1, [this](Request& r) { r.push<u32>(impl.GetSystem().GetSampleCount()); });
        on(2, [this](Request& r) { r.push<u32>(impl.GetSystem().GetMixBufferCount()); });
        on(3, [this](Request& r) { r.push<u32>(impl.GetSystem().IsActive() ? 0 : 1); });
        on(4, [this](Request& r) { update(r); });
        on(5, [this](Request&) { impl.Start(); });
        on(6, [this](Request&) { impl.Stop(); });
        on(7, [this](Request& r) { r.out_copy.push_back(rendered.event.new_handle()); });
        on(8, [this](Request& r) { impl.GetSystem().SetRenderingTimeLimit(r.arg<u32>(0)); });
        on(9, [this](Request& r) { r.push<u32>(impl.GetSystem().GetRenderingTimeLimit()); });
        on(10, [this](Request& r) { update(r); });
        on(12, [this](Request& r) { impl.GetSystem().SetVoiceDropParameter(r.arg<f32>(0)); });
        on(13, [this](Request& r) { r.push<f32>(impl.GetSystem().GetVoiceDropParameter()); });
    }
    ~AudioRenderer() override { impl.Finalize(); }

    // RequestUpdate(out: output, out: performance, in: parameters).
    void update(Request& r) {
        const std::vector<u8> in = r.read_in(0);
        std::vector<u8> out(r.out_buf(0).size), performance(r.out_buf(1).size);
        const auto res = impl.RequestUpdate(in, performance, out);
        if (res.IsError() && errors++ < 16) LOG("audren: update failed: %#x", res.raw);
        r.result = res.raw;
        r.write_out(0, out.data(), out.size());
        if (!performance.empty()) r.write_out(1, performance.data(), performance.size());
        if (++updates == 3000) log_voices(out);
    }

    // Once, as a check that the DSP renders: the voices whose status reports played samples
    // (output: 0x40 header {rev, behavior, pools, voices, ...}, pool statuses, voice statuses 0x10 each).
    void log_voices(const std::vector<u8>& out) {
        if (out.size() < 0x40) return;
        u32 pools, voices;
        std::memcpy(&pools, out.data() + 8, 4);
        std::memcpy(&voices, out.data() + 12, 4);
        u32 playing = 0;
        for (u64 at = 0x40 + pools; at + 0x10 <= std::min<u64>(out.size(), 0x40ull + pools + voices); at += 0x10) {
            u64 played;
            std::memcpy(&played, out.data() + at, 8);
            playing += played != 0;
        }
        LOG("audren: after %u updates %u of %u voices report played samples", updates, playing, voices / 0x10);
    }

    RenderedEvent rendered;
    AudioCore::Renderer::Renderer impl;
    int errors = 0;
    u32 updates = 0;
};

struct AudioDevice : Service {
    AudioDevice() : Service("IAudioDevice") {
        auto names = [](Request& r) {
            char name[0x100] = "AudioTvOutput";
            r.write_out(0, name, sizeof name);
            r.push<u32>(1);
        };
        on(0, names);
        on(1, [](Request&) {});
        on(2, [](Request& r) { r.push<f32>(1.0f); });
        on(3, [](Request& r) {
            char name[0x100] = "AudioTvOutput";
            r.write_out(0, name, sizeof name);
        });
        on(4, [this](Request& r) { r.out_copy.push_back(event.new_handle()); });
        on(5, [](Request& r) { r.push<u32>(2); });
        on(6, names);
        on(7, [](Request&) {});
        on(8, [](Request& r) { r.push<f32>(1.0f); });
        on(10, [](Request& r) {
            char name[0x100] = "AudioTvOutput";
            r.write_out(0, name, sizeof name);
        });
        on(11, [this](Request& r) { r.out_copy.push_back(event.new_handle()); });
        on(12, [this](Request& r) { r.out_copy.push_back(event.new_handle()); });
        on(13, [](Request& r) {
            char name[0x100] = "AudioTvOutput";
            r.write_out(0, name, sizeof name);
        });
        on(14, names);
        on(19, [](Request&) {});
        on(20, [](Request& r) { r.push<u8>(0); });
    }
    kern::ServiceEvent event;
};

struct AudRenManager : Service {
    explicit AudRenManager(std::string n) : Service(std::move(n)) {
        // OpenAudioRenderer(params, u64 tmem size, u64 aruid; copy handles: tmem, process).
        on(0, [](Request& r) {
            const Params params = r.arg<Params>(0);
            auto tmem = r.copy_handles.empty() ? nullptr : kern::get<kern::KTransferMemory>(r.copy_handles[0]);
            if (!tmem) {
                LOG("audren: open without transfer memory");
                r.result = MakeResult(153, 41);
                return;
            }
            const s32 session_id = manager()->GetSessionId();
            if (session_id < 0) {
                r.result = MakeResult(153, 8);  // out of sessions
                return;
            }
            r.push_object(std::make_shared<AudioRenderer>(params, tmem->addr, r.arg<u64>(0x38), r.arg<u64>(0x40),
                                                          session_id));
        });
        on(1, [](Request& r) {  // GetWorkBufferSize
            u64 size = 0;
            const auto res = manager()->GetWorkBufferSize(r.arg<Params>(0), size);
            r.result = res.raw;
            r.push<u64>(size);
        });
        on(2, [](Request& r) { r.push_object(std::make_shared<AudioDevice>()); });
        on(4, [](Request& r) { r.push_object(std::make_shared<AudioDevice>()); });
    }
};

struct Nx2AudU : Service {
    Nx2AudU() : Service("nx2:aud:u") {
        on(2, [](Request& r) { r.push_object(std::make_shared<AudRenManager>("IAudioRendererManager")); });
    }
};

}  // namespace

void register_audio() {
    register_service("audren:u", [] { return std::make_shared<AudRenManager>("audren:u"); });
    register_service("aud:u", [] { return std::make_shared<Nx2AudU>(); });  // nn::audio user client
}

}  // namespace ipc
