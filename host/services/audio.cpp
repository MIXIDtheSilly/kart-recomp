// audren: the audio renderer (audio_core/, adapted from Eden) runs the game's voices, mixes and
// effects and plays the result through WASAPI, paced by the output stream. This file is its host
// side: guest memory, the clock, logging, and the rendered event.
#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstring>

#include "audio_core/audio_core.h"
#include "audio_core/audio_render_manager.h"
#include "audio_core/common/audio_renderer_parameter.h"
#include "audio_core/renderer/audio_renderer.h"

#include "../ipc.h"
#include "../memory.h"

// ------------------------------------------------------------------ audio_core/host.h, defined by the host

namespace AudioCore {

namespace {
const auto g_start = std::chrono::steady_clock::now();

template <class F> void for_each_page(u64 addr, std::size_t size, F&& f) {
    while (size) {
        const u64 chunk = std::min<u64>(size, mem::PageSize - (addr & (mem::PageSize - 1)));
        f(mem::backing_ptr(addr), chunk);
        addr += chunk;
        size -= chunk;
    }
}
}  // namespace

std::chrono::nanoseconds Clock::GetGlobalTimeNs() const {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - g_start);
}

void LogMessage(LogLevel level, std::string_view text) {
    static constexpr const char* names[] = {"debug", "info", "warning", "error", "critical"};
    LOG("audio %s: %.*s", names[static_cast<int>(level)], static_cast<int>(text.size()), text.data());
}

void AssertFailed(const char* what, const char* file, int line) { fatal("audio: assert %s at %s:%d", what, file, line); }

// Reads go through the physical pool, so they never fault on pages the renderer write-protected.
bool GuestMemory::ReadBlockUnsafe(u64 address, void* dest, std::size_t size) {
    u8* out = static_cast<u8*>(dest);
    for_each_page(address, size, [&](const u8* p, u64 n) {
        if (p) std::memcpy(out, p, n);
        else std::memset(out, 0, n);
        out += n;
    });
    return true;
}

bool GuestMemory::WriteBlockUnsafe(u64 address, const void* src, std::size_t size) {
    const u8* in = static_cast<const u8*>(src);
    for_each_page(address, size, [&](u8* p, u64 n) {
        if (p) std::memcpy(p, in, n);
        in += n;
    });
    return true;
}

bool GuestMemory::ZeroBlock(u64 address, std::size_t size) {
    for_each_page(address, size, [](u8* p, u64 n) {
        if (p) std::memset(p, 0, n);
    });
    return true;
}

u8* GuestMemory::GetSpan(u64 address, std::size_t size) {
    const auto runs = mem::phys_runs(address, size);
    if (runs.size() != 1 || runs[0].size < size) return nullptr;
    return mem::phys_ptr(runs[0].phys);
}

}  // namespace AudioCore

namespace {

// The game's "frame rendered" event.
struct RenderedEvent final : AudioCore::HostEvent {
    kern::ServiceEvent event;
    void Signal() override { event.signal(); }
    void Clear() override { event.clear(); }
};

AudioCore::HostProcess g_process;

}  // namespace

namespace ipc {
namespace {

using Params = AudioCore::AudioRendererParameterInternal;

std::shared_ptr<AudioCore::Renderer::Manager> manager() {
    static auto m = std::make_shared<AudioCore::Renderer::Manager>(AudioCore::GetHostSystem());
    return m;
}

struct AudioRenderer : Service {
    AudioRenderer(const Params& params, u64 tmem_address, u64 tmem_size, u64 aruid, s32 session_id)
        : Service("IAudioRenderer"), impl(AudioCore::GetHostSystem(), *manager(), &rendered) {
        AudioCore::HostTransferMemory tmem{tmem_address};
        const auto res = impl.Initialize(params, &tmem, tmem_size, &g_process, aruid, session_id);
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
