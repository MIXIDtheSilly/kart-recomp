// The video decoder on Linux (vdec_mf.h): libvpx for VP9 and VP8, the codecs of the game's movies. It is loaded at run time
// (dlopen, the soname of any libvpx since 1.6), so the build needs no libvpx headers; the few declarations it uses are
// below, with the layout of libvpx's ABI (vpx_image_t: VPX_IMAGE_ABI_VERSION 5, decoder ABI 12, unchanged since 1.8).
// Without libvpx the frames come out black and the movies still run their course, as on Windows without a decoder.
#ifndef _WIN32
#include <dlfcn.h>

#include <algorithm>
#include <cstring>
#include <mutex>

#include "vdec_mf.h"

namespace vdec {
namespace {

// ------------------------------------------------------------------ libvpx's ABI

struct VpxImage {
    int fmt, cs, range;
    unsigned w, h, bit_depth, d_w, d_h, r_w, r_h, x_chroma_shift, y_chroma_shift;
    unsigned char* planes[4];
    int stride[4];
    int bps;
    void* user_priv;
    // (private fields follow)
};
struct VpxDecCfg {
    unsigned threads, w, h;
};
constexpr int VpxDecoderAbi = 12;
constexpr int VpxImgFmtPlanar = 0x100, VpxImgFmtHighBitDepth = 0x800, VpxImgFmtI420 = VpxImgFmtPlanar | 2;

struct Vpx {
    using Iface = void*;
    Iface (*vp9_dx)() = nullptr;
    Iface (*vp8_dx)() = nullptr;
    int (*dec_init_ver)(void* ctx, Iface iface, const VpxDecCfg* cfg, long flags, int ver) = nullptr;
    int (*decode)(void* ctx, const u8* data, unsigned size, void* user_priv, long deadline) = nullptr;
    VpxImage* (*get_frame)(void* ctx, const void** iter) = nullptr;
    int (*destroy)(void* ctx) = nullptr;
    const char* (*error)(void* ctx) = nullptr;
    const char* (*version)() = nullptr;
    bool ok = false;

    Vpx() {
        void* lib = nullptr;
        for (const char* name : {"libvpx.so.12", "libvpx.so.11", "libvpx.so.10", "libvpx.so.9", "libvpx.so.8", "libvpx.so.7", "libvpx.so.6", "libvpx.so"})
            if ((lib = dlopen(name, RTLD_NOW | RTLD_LOCAL))) break;
        if (!lib) return;
        const auto get = [&](auto& fn, const char* sym) { fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(dlsym(lib, sym)); };
        get(vp9_dx, "vpx_codec_vp9_dx");
        get(vp8_dx, "vpx_codec_vp8_dx");
        get(dec_init_ver, "vpx_codec_dec_init_ver");
        get(decode, "vpx_codec_decode");
        get(get_frame, "vpx_codec_get_frame");
        get(destroy, "vpx_codec_destroy");
        get(error, "vpx_codec_error");
        get(version, "vpx_codec_version_str");
        ok = dec_init_ver && decode && get_frame && destroy && (vp9_dx || vp8_dx);
        if (ok) LOG("vdec: libvpx %s", version ? version() : "?");
    }
};

const Vpx& vpx() {
    static const Vpx v;
    return v;
}

}  // namespace

struct Decoder::Impl {
    alignas(16) u8 ctx[512]{};   // vpx_codec_ctx_t (64 bytes in every version: room to spare)
    bool open = false, missing = false;
    u32 width = 0, height = 0;   // the configured size (black frames)

    // A black frame of the configured size, in NV12.
    Picture black(u64 pts) const {
        Picture p{width, height, pts, nullptr};
        const size_t luma = size_t(width) * height;
        p.nv12 = std::make_shared<std::vector<u8>>(luma * 3 / 2 + width, u8{128});
        std::fill(p.nv12->begin(), p.nv12->begin() + luma, u8{16});
        return p;
    }

    // Every picture the decoder has ready, as NV12 (rows `d_w` bytes apart, interleaved chroma after the luma).
    void pull(std::vector<Picture>& pictures) {
        const void* iter = nullptr;
        while (const VpxImage* img = vpx().get_frame(ctx, &iter)) {
            const u32 w = img->d_w, h = img->d_h;
            if (!w || !h || !(img->fmt & VpxImgFmtPlanar)) continue;
            const bool high = (img->fmt & VpxImgFmtHighBitDepth) != 0;
            const unsigned shift = high ? img->bit_depth - 8 : 0;
            const auto sample = [&](int plane, u32 x, u32 y) -> u8 {
                const u8* row = img->planes[plane] + size_t(y) * img->stride[plane];
                return high ? static_cast<u8>(reinterpret_cast<const u16*>(row)[x] >> shift) : row[x];
            };
            Picture p{w, h, reinterpret_cast<u64>(img->user_priv), nullptr};
            p.nv12 = std::make_shared<std::vector<u8>>(size_t(w) * h + size_t(w) * ((h + 1) / 2) + w, u8{128});
            u8* const y_out = p.nv12->data();
            u8* const uv_out = y_out + size_t(w) * h;
            for (u32 y = 0; y < h; y++) {
                if (!high) std::memcpy(y_out + size_t(y) * w, img->planes[0] + size_t(y) * img->stride[0], w);
                else for (u32 x = 0; x < w; x++) y_out[size_t(y) * w + x] = sample(0, x, y);
            }
            // 4:2:0 as the movies are; 4:2:2 and 4:4:4 (profile 1) are sampled down by their chroma shifts
            for (u32 y = 0; y < (h + 1) / 2; y++) {
                u8* row = uv_out + size_t(y) * w;
                const u32 sy = (y * 2) >> img->y_chroma_shift;
                for (u32 x = 0; x < w / 2; x++) {
                    const u32 sx = (x * 2) >> img->x_chroma_shift;
                    row[x * 2] = sample(1, sx, sy);
                    row[x * 2 + 1] = sample(2, sx, sy);
                }
            }
            pictures.push_back(std::move(p));
        }
    }
};

Decoder::Decoder(std::string_view mime, u32 width, u32 height) : name(mime), impl(std::make_unique<Impl>()) {
    impl->width = width;
    impl->height = height;
    const Vpx& v = vpx();
    void* const iface = !v.ok ? nullptr : name == "video/vp9" && v.vp9_dx ? v.vp9_dx() : name == "video/vp8" && v.vp8_dx ? v.vp8_dx() : nullptr;
    const VpxDecCfg cfg{4, width, height};
    impl->open = iface && v.dec_init_ver(impl->ctx, iface, &cfg, 0, VpxDecoderAbi) == 0;
    if (!impl->open) {
        impl->missing = true;
        static std::once_flag once;
        std::call_once(once, [&] {
            LOG("vdec: no %s decoder (%s): movies show black", name.c_str(),
                !v.ok ? "libvpx is not installed" : iface ? v.error(impl->ctx) : "libvpx has none");
        });
    }
}

Decoder::~Decoder() {
    if (impl->open) vpx().destroy(impl->ctx);
}

void Decoder::Decode(std::span<const u8> data, u64 pts, std::vector<Picture>& pictures) {
    if (impl->missing) {
        if (impl->width && impl->height) pictures.push_back(impl->black(pts));
        return;
    }
    // The pts travels with the frame as its user data: libvpx hands it back on the picture that frame shows.
    if (vpx().decode(impl->ctx, data.data(), static_cast<unsigned>(data.size()), reinterpret_cast<void*>(pts), 0) != 0)
        LOG("vdec: %s input of %zu bytes refused (%s)", name.c_str(), data.size(), vpx().error(impl->ctx));
    impl->pull(pictures);
}

void Decoder::Drain(std::vector<Picture>& pictures) {
    if (impl->missing) return;
    vpx().decode(impl->ctx, nullptr, 0, nullptr, 0);   // (flush: frames held back come out)
    impl->pull(pictures);
}

void Decoder::Reset() {
    if (impl->missing) return;
    // libvpx keeps no queue of frames to drop: a flush and reading out what it returns
    vpx().decode(impl->ctx, nullptr, 0, nullptr, 0);
    std::vector<Picture> dropped;
    impl->pull(dropped);
}

}  // namespace vdec
#endif
