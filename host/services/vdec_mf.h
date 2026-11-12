// The video decoder behind nn::codec::video (host/services/gsv.cpp, VideoSession): Windows' Media Foundation
// decodes the elementary stream the game demuxed (its movies are VP9, which needs the VP9 Video Extensions;
// without a decoder the frames come out black and the movies still run their course).
#pragma once
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "../common.h"

namespace vdec {

// A decoded picture, in display order: NV12, rows `width` bytes apart, the interleaved chroma rows after the
// luma rows.
struct Picture {
    u32 width = 0, height = 0;
    u64 pts = 0;   // as submitted with the compressed frame (us)
    std::shared_ptr<std::vector<u8>> nv12;
};

// Pixel layouts of an output image.
enum class Format : u32 {
    Nv12,   // luma rows, then interleaved Cb/Cr rows at chroma_offset
    Rgba8,
    Bgra8,
};

// Used from one thread at a time.
class Decoder {
public:
    // mime: the codec name nn::codec sends with Configure ("video/vp9", "video/vp8", ...); width x height: the size
    // it configures, for the black frames when there is no decoder.
    Decoder(std::string_view mime, u32 width, u32 height);
    ~Decoder();
    const std::string& Name() const { return name; }

    // Decodes one compressed frame and appends the pictures it completes.
    void Decode(std::span<const u8> data, u64 pts, std::vector<Picture>& pictures);
    // End of stream: appends the pictures the decoder still holds, then starts over.
    void Drain(std::vector<Picture>& pictures);
    // Drops everything queued in the decoder (the game flushed it to seek).
    void Reset();

    // Writes `picture` as `format` into `out`: rows `pitch` bytes apart, NV12's chroma plane at `chroma_offset`,
    // scaled to width x height.
    static void Render(const Picture& picture, Format format, u32 width, u32 height, u64 pitch, u64 chroma_offset,
                       std::span<u8> out);

private:
    struct Impl;
    std::string name;
    std::unique_ptr<Impl> impl;
};

}  // namespace vdec
