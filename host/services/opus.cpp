// hwopus: the hardware Opus decoder, on libopus. The movie player decodes its videos' audio with it.
// Command layouts from Eden's hardware_opus_decoder(_manager).cpp; each input packet starts with
// a big-endian {u32 size, u32 final range} header.
#include <opus.h>
#include <opus_multistream.h>

#include "../ipc.h"

namespace ipc {
namespace {

constexpr u32 ResultInvalidData = (153 << 9) | 2;  // audio module (decoding failed)

u32 byteswap(u32 v) { return _byteswap_ulong(v); }

struct OpusDecoderSession : Service {
    OpusDecoder* single = nullptr;
    OpusMSDecoder* multi = nullptr;
    u32 channels = 0;

    OpusDecoderSession(u32 sample_rate, u32 channels_) : Service("IHardwareOpusDecoder"), channels(channels_) {
        int err = 0;
        single = opus_decoder_create(static_cast<opus_int32>(sample_rate), static_cast<int>(channels), &err);
        if (err != OPUS_OK) LOG("hwopus: opus_decoder_create(%u, %u) failed: %d", sample_rate, channels, err);
        register_commands();
    }
    OpusDecoderSession(u32 sample_rate, u32 channels_, u32 streams, u32 coupled, const u8* mapping)
        : Service("IHardwareOpusDecoder"), channels(channels_) {
        int err = 0;
        multi = opus_multistream_decoder_create(static_cast<opus_int32>(sample_rate), static_cast<int>(channels),
                                                static_cast<int>(streams), static_cast<int>(coupled), mapping, &err);
        if (err != OPUS_OK) LOG("hwopus: opus_multistream_decoder_create failed: %d", err);
        register_commands();
    }
    ~OpusDecoderSession() override {
        if (single) opus_decoder_destroy(single);
        if (multi) opus_multistream_decoder_destroy(multi);
    }

    void register_commands() {
        // 0/2 (old), 4/5 (with perf), 6/7 (with perf and reset), 8/9: single / multi stream.
        for (u32 c : {0u, 2u}) on(c, [this](Request& r) { decode(r, false, false); });
        for (u32 c : {4u, 5u}) on(c, [this](Request& r) { decode(r, true, false); });
        for (u32 c : {6u, 7u, 8u, 9u}) on(c, [this](Request& r) { decode(r, true, r.arg<u8>(0) != 0); });
        on(1, [](Request&) {});  // SetContext
        on(3, [](Request&) {});
    }

    void decode(Request& r, bool perf, bool reset) {
        const auto in = r.read_in(0);
        const Buffer out = r.out_buf(0);
        if (in.size() <= 8 || (!single && !multi) || !channels) {
            r.result = ResultInvalidData;
            return;
        }
        u32 hdr[2];
        std::memcpy(hdr, in.data(), 8);
        const u32 size = byteswap(hdr[0]);
        if (size + 8 > in.size()) {
            r.result = ResultInvalidData;
            return;
        }
        if (reset) {
            if (single) opus_decoder_ctl(single, OPUS_RESET_STATE);
            if (multi) opus_multistream_decoder_ctl(multi, OPUS_RESET_STATE);
        }
        const u64 t0 = host_ns();
        thread_local std::vector<opus_int16> pcm;
        const int max_frames = static_cast<int>(std::min<u64>(out.size / (2 * channels), 5760));
        pcm.resize(static_cast<size_t>(max_frames) * channels);
        const int n = single ? opus_decode(single, in.data() + 8, static_cast<opus_int32>(size), pcm.data(), max_frames, 0)
                             : opus_multistream_decode(multi, in.data() + 8, static_cast<opus_int32>(size), pcm.data(),
                                                       max_frames, 0);
        if (n < 0) {
            static int logged = 0;
            if (logged++ < 8) LOG("hwopus: decode of %u bytes failed: %d", size, n);
            r.result = ResultInvalidData;
            return;
        }
        r.write_out(0, pcm.data(), static_cast<size_t>(n) * channels * 2);
        r.push<u32>(size + 8);
        r.push<u32>(static_cast<u32>(n));
        if (perf) r.push<u64>((host_ns() - t0) / 1000);
    }
};

// A generous work buffer: the decoder state lives on the host, the guest's buffer is unused.
u32 work_buffer_size(u32 channels) { return 0x10000 + std::max<u32>(channels, 1) * 0x8000; }

struct HwOpus : Service {
    HwOpus() : Service("hwopus") {
        // 0 Open(OpusParameters{sample rate, channels}, u32 work size, copy handle) / 4 OpenEx(0x10 bytes).
        for (u32 c : {0u, 4u}) {
            on(c, [](Request& r) {
                const u32 rate = r.arg<u32>(0), ch = r.arg<u32>(4);
                LOG("hwopus: decoder %u Hz, %u channels", rate, ch);
                r.push_object(std::make_shared<OpusDecoderSession>(rate, ch));
            });
        }
        // 2 OpenForMultiStream(X: {rate, channels, streams, stereo streams, mapping[0x100]}) /
        // 6 ...Ex (use_large_frame_size and padding before the mapping, at +0x18).
        for (u32 c : {2u, 6u}) {
            on(c, [](Request& r) {
                std::vector<u8> p = r.read_in(0);
                p.resize(0x118);
                u32 v[4];
                std::memcpy(v, p.data(), sizeof v);
                const u8* mapping = p.data() + (r.cmd == 6 ? 0x18 : 0x10);
                LOG("hwopus: multistream decoder %u Hz, %u channels, %u streams (%u stereo)", v[0], v[1], v[2], v[3]);
                r.push_object(std::make_shared<OpusDecoderSession>(v[0], v[1], v[2], v[3], mapping));
            });
        }
        for (u32 c : {1u, 5u, 8u}) on(c, [](Request& r) { r.push<u32>(work_buffer_size(r.arg<u32>(4))); });
        for (u32 c : {3u, 7u, 9u}) {
            on(c, [](Request& r) {
                const auto p = r.read_in(0);
                u32 ch = 2;
                if (p.size() >= 8) std::memcpy(&ch, p.data() + 4, 4);
                r.push<u32>(work_buffer_size(ch));
            });
        }
    }
};

}  // namespace

void register_hwopus() {
    register_service("hwopus", [] { return std::make_shared<HwOpus>(); });
}

}  // namespace ipc
