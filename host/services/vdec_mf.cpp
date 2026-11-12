#include "vdec_mf.h"

#include <windows.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mftransform.h>

#include <algorithm>
#include <mutex>

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfuuid.lib")

namespace vdec {
namespace {

template <class T> struct Com {
    T* p = nullptr;
    Com() = default;
    Com(const Com&) = delete;
    Com& operator=(const Com&) = delete;
    ~Com() { reset(); }
    void reset() {
        if (p) p->Release();
        p = nullptr;
    }
    T** put() {
        reset();
        return &p;
    }
    T* operator->() const { return p; }
    explicit operator bool() const { return p != nullptr; }
};

bool mf_started() {
    static const bool ok = [] {
        const HRESULT hr = MFStartup(MF_VERSION, MFSTARTUP_LITE);
        if (FAILED(hr)) LOG("vdec: Media Foundation did not start (%#lx)", static_cast<unsigned long>(hr));
        return SUCCEEDED(hr);
    }();
    return ok;
}

GUID subtype_of(std::string_view mime) {
    if (mime == "video/vp9") return MFVideoFormat_VP90;
    if (mime == "video/vp8") return MFVideoFormat_VP80;
    if (mime == "video/avc") return MFVideoFormat_H264;
    if (mime == "video/hevc") return MFVideoFormat_HEVC;
    return GUID_NULL;
}

}  // namespace

struct Decoder::Impl {
    Com<IMFTransform> mft;
    u32 width = 0, height = 0;            // the configured size (black frames)
    u32 out_w = 0, out_h = 0;             // the decoder's output frame
    u32 crop_w = 0, crop_h = 0;           // its visible part (the minimum display aperture)
    bool provides_samples = false;
    u32 out_size = 0;
    bool missing = false;                 // no decoder: black frames

    ~Impl() {
        if (mft) mft->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
    }

    bool open(const std::string& mime) {
        if (!mf_started()) return false;
        const GUID sub = subtype_of(mime);
        if (sub == GUID_NULL) return false;
        MFT_REGISTER_TYPE_INFO in{MFMediaType_Video, sub};
        IMFActivate** acts = nullptr;
        UINT32 n = 0;
        HRESULT hr = MFTEnumEx(MFT_CATEGORY_VIDEO_DECODER, MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_LOCALMFT | MFT_ENUM_FLAG_SORTANDFILTER,
                               &in, nullptr, &acts, &n);
        if (FAILED(hr) || !n) return false;
        for (UINT32 i = 0; i < n && !mft; i++) acts[i]->ActivateObject(IID_PPV_ARGS(mft.put()));
        for (UINT32 i = 0; i < n; i++) acts[i]->Release();
        CoTaskMemFree(acts);
        if (!mft) return false;

        Com<IMFMediaType> type;
        MFCreateMediaType(type.put());
        type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        type->SetGUID(MF_MT_SUBTYPE, sub);
        if (width && height) MFSetAttributeSize(type.p, MF_MT_FRAME_SIZE, width, height);
        hr = mft->SetInputType(0, type.p, 0);
        if (FAILED(hr)) {
            LOG("vdec: the %s decoder takes no %ux%u input (%#lx)", mime.c_str(), width, height, static_cast<unsigned long>(hr));
            mft.reset();
            return false;
        }
        if (!pick_output()) {
            mft.reset();
            return false;
        }
        mft->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
        mft->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
        return true;
    }

    // NV12 output, and what it says about the frame
    bool pick_output() {
        for (DWORD i = 0;; i++) {
            Com<IMFMediaType> t;
            if (FAILED(mft->GetOutputAvailableType(0, i, t.put()))) return false;
            GUID sub{};
            t->GetGUID(MF_MT_SUBTYPE, &sub);
            if (sub != MFVideoFormat_NV12) continue;
            if (FAILED(mft->SetOutputType(0, t.p, 0))) return false;
            UINT32 w = 0, h = 0;
            MFGetAttributeSize(t.p, MF_MT_FRAME_SIZE, &w, &h);
            out_w = w;
            out_h = h;
            crop_w = w;
            crop_h = h;
            MFVideoArea area{};
            if (SUCCEEDED(t->GetBlob(MF_MT_MINIMUM_DISPLAY_APERTURE, reinterpret_cast<UINT8*>(&area), sizeof area, nullptr))) {
                crop_w = std::min<u32>(w, static_cast<u32>(area.Area.cx));
                crop_h = std::min<u32>(h, static_cast<u32>(area.Area.cy));
            }
            MFT_OUTPUT_STREAM_INFO info{};
            mft->GetOutputStreamInfo(0, &info);
            provides_samples = (info.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)) != 0;
            out_size = info.cbSize ? info.cbSize : w * h * 3 / 2;
            return true;
        }
    }

    // Every picture the decoder has ready
    void pull(std::vector<Picture>& pictures) {
        for (;;) {
            MFT_OUTPUT_DATA_BUFFER odb{};
            Com<IMFSample> own;
            if (!provides_samples) {
                Com<IMFMediaBuffer> buf;
                MFCreateMemoryBuffer(out_size, buf.put());
                MFCreateSample(own.put());
                own->AddBuffer(buf.p);
                odb.pSample = own.p;
            }
            DWORD status = 0;
            const HRESULT hr = mft->ProcessOutput(0, 1, &odb, &status);
            if (odb.pEvents) odb.pEvents->Release();
            if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
                if (!pick_output()) return;
                continue;
            }
            if (FAILED(hr)) {
                if (provides_samples && odb.pSample) odb.pSample->Release();
                return;   // MF_E_TRANSFORM_NEED_MORE_INPUT, or an error
            }
            if (odb.pSample) {
                pictures.push_back(read(odb.pSample));
                if (provides_samples) odb.pSample->Release();
            }
        }
    }

    Picture read(IMFSample* sample) {
        Picture p;
        LONGLONG t = 0;
        sample->GetSampleTime(&t);
        p.pts = static_cast<u64>(t / 10);
        p.width = crop_w;
        p.height = crop_h;
        p.nv12 = std::make_shared<std::vector<u8>>(size_t(crop_w) * crop_h * 3 / 2 + crop_w);
        Com<IMFMediaBuffer> buf;
        if (FAILED(sample->ConvertToContiguousBuffer(buf.put()))) return p;
        BYTE* data = nullptr;
        LONG pitch = static_cast<LONG>(out_w);
        Com<IMF2DBuffer> buf2d;
        bool locked2d = false;
        if (SUCCEEDED(buf->QueryInterface(IID_PPV_ARGS(buf2d.put()))) && SUCCEEDED(buf2d->Lock2D(&data, &pitch))) {
            locked2d = true;
        } else {
            DWORD len = 0;
            if (FAILED(buf->Lock(&data, nullptr, &len))) return p;
        }
        u8* dst = p.nv12->data();
        // Luma, then the chroma plane (it starts after the decoder's whole output height)
        for (u32 y = 0; y < crop_h; y++) std::memcpy(dst + size_t(y) * crop_w, data + size_t(y) * pitch, crop_w);
        const BYTE* uv = data + size_t(pitch) * out_h;
        u8* duv = dst + size_t(crop_w) * crop_h;
        for (u32 y = 0; y < (crop_h + 1) / 2; y++) std::memcpy(duv + size_t(y) * crop_w, uv + size_t(y) * pitch, crop_w);
        if (locked2d) buf2d->Unlock2D();
        else buf->Unlock();
        return p;
    }

    Picture black(u64 pts) const {
        Picture p;
        p.width = width;
        p.height = height;
        p.pts = pts;
        const size_t luma = size_t(width) * height;
        p.nv12 = std::make_shared<std::vector<u8>>(luma * 3 / 2 + width, u8{128});
        std::fill(p.nv12->begin(), p.nv12->begin() + luma, u8{16});
        return p;
    }
};

Decoder::Decoder(std::string_view mime, u32 width, u32 height) : name(mime), impl(std::make_unique<Impl>()) {
    impl->width = width;
    impl->height = height;
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (!impl->open(name)) {
        impl->missing = true;
        static std::once_flag once;
        std::call_once(once, [&] {
            LOG("vdec: Windows has no %s decoder (for VP9: the VP9 Video Extensions, Microsoft Store): movies show black",
                name.c_str());
        });
    }
}

Decoder::~Decoder() = default;

void Decoder::Decode(std::span<const u8> data, u64 pts, std::vector<Picture>& pictures) {
    if (impl->missing) {
        if (impl->width && impl->height) pictures.push_back(impl->black(pts));
        return;
    }
    Com<IMFMediaBuffer> buf;
    if (FAILED(MFCreateMemoryBuffer(static_cast<DWORD>(data.size()), buf.put()))) return;
    BYTE* p = nullptr;
    buf->Lock(&p, nullptr, nullptr);
    std::memcpy(p, data.data(), data.size());
    buf->Unlock();
    buf->SetCurrentLength(static_cast<DWORD>(data.size()));
    Com<IMFSample> sample;
    MFCreateSample(sample.put());
    sample->AddBuffer(buf.p);
    sample->SetSampleTime(static_cast<LONGLONG>(pts) * 10);
    HRESULT hr = impl->mft->ProcessInput(0, sample.p, 0);
    if (hr == MF_E_NOTACCEPTING) {
        impl->pull(pictures);
        hr = impl->mft->ProcessInput(0, sample.p, 0);
    }
    if (FAILED(hr)) LOG("vdec: %s input of %zu bytes refused (%#lx)", name.c_str(), data.size(), static_cast<unsigned long>(hr));
    impl->pull(pictures);
}

void Decoder::Drain(std::vector<Picture>& pictures) {
    if (impl->missing) return;
    impl->mft->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0);
    impl->pull(pictures);
    impl->mft->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
}

void Decoder::Reset() {
    if (impl->missing) return;
    impl->mft->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
}

void Decoder::Render(const Picture& pic, Format format, u32 width, u32 height, u64 pitch, u64 chroma_offset,
                     std::span<u8> out) {
    if (!pic.nv12 || !pic.width || !pic.height || !width || !height) return;
    const u8* y_plane = pic.nv12->data();
    const u8* uv_plane = y_plane + size_t(pic.width) * pic.height;
    // Nearest source sample of each output pixel (the sizes match but for a frame larger than the image)
    const auto sx = [&](u32 x) { return width == pic.width ? x : static_cast<u32>(u64(x) * pic.width / width); };
    const auto sy = [&](u32 y) { return height == pic.height ? y : static_cast<u32>(u64(y) * pic.height / height); };
    if (format == Format::Nv12) {
        for (u32 y = 0; y < height; y++) {
            u8* row = out.data() + y * pitch;
            const u8* src = y_plane + size_t(sy(y)) * pic.width;
            if (width == pic.width) std::memcpy(row, src, width);
            else for (u32 x = 0; x < width; x++) row[x] = src[sx(x)];
        }
        for (u32 y = 0; y < (height + 1) / 2; y++) {
            u8* row = out.data() + chroma_offset + y * pitch;
            const u8* src = uv_plane + size_t(sy(y * 2) / 2) * pic.width;
            if (width == pic.width) std::memcpy(row, src, (width + 1) & ~1u);
            else for (u32 x = 0; x < width / 2; x++) {
                const u32 s = sx(x * 2) / 2;
                row[x * 2] = src[s * 2];
                row[x * 2 + 1] = src[s * 2 + 1];
            }
        }
        return;
    }
    // BT.709, limited range
    const bool bgra = format == Format::Bgra8;
    for (u32 y = 0; y < height; y++) {
        u8* row = out.data() + y * pitch;
        const u32 py = sy(y);
        const u8* ys = y_plane + size_t(py) * pic.width;
        const u8* uvs = uv_plane + size_t(py / 2) * pic.width;
        for (u32 x = 0; x < width; x++) {
            const u32 px = sx(x);
            const int c = (ys[px] - 16) * 298;
            const int d = uvs[px & ~1u] - 128, e = uvs[(px & ~1u) + 1] - 128;
            const int r = std::clamp((c + 459 * e + 128) >> 8, 0, 255);
            const int g = std::clamp((c - 55 * d - 136 * e + 128) >> 8, 0, 255);
            const int b = std::clamp((c + 541 * d + 128) >> 8, 0, 255);
            u8* o = row + x * 4;
            o[0] = static_cast<u8>(bgra ? b : r);
            o[1] = static_cast<u8>(g);
            o[2] = static_cast<u8>(bgra ? r : b);
            o[3] = 255;
        }
    }
}

}  // namespace vdec
