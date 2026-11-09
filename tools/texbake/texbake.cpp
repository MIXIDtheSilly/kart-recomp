// texbake: offline tool for the ASTC textures the host dumps with HOST_TEXDUMP=<dir> (Eglantine
// texture_cache/util.cpp DumpAstcTexture). PC GPUs sample no ASTC, so the GPU re-compresses it to
// BC1 (opaque) or BC3 at every upload; this decodes each dump to find the smallest BC format that
// keeps it intact.
//   texbake analyze <dir> [csv]   per-texture alpha/colour classes and the host bytes per choice
//   texbake png <file.astc> <out.png>   level 0, layer 0 as RGBA (alpha shown separately on the right)
#define _CRT_SECURE_NO_WARNINGS
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"
#include "astc_decode.h"
#include "texture_transcode.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

namespace {

struct Copy {
    uint64_t offset, size;
    uint32_t row_length, image_height, level, base_layer, layers, width, height;
};

struct Dump {
    uint32_t bw, bh, srgb, width, height, levels, layers;
    uint64_t guest_bytes;
    std::vector<Copy> copies;
    std::vector<uint8_t> data;
};

bool Load(const fs::path& path, Dump& d) {
    std::FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f) return false;
    auto get32 = [f] { uint32_t v = 0; std::fread(&v, 4, 1, f); return v; };
    auto get64 = [f] { uint64_t v = 0; std::fread(&v, 8, 1, f); return v; };
    char magic[4] = {};
    std::fread(magic, 1, 4, f);
    if (std::memcmp(magic, "ASTD", 4) != 0 || get32() != 1) { std::fclose(f); return false; }
    d.bw = get32(); d.bh = get32(); d.srgb = get32(); d.width = get32(); d.height = get32();
    d.levels = get32(); d.layers = get32();
    const uint32_t count = get32();
    d.guest_bytes = get64();
    const uint64_t bytes = get64();
    d.copies.resize(count);
    for (Copy& c : d.copies) {
        c.offset = get64(); c.size = get64();
        c.row_length = get32(); c.image_height = get32(); c.level = get32(); c.base_layer = get32();
        c.layers = get32(); c.width = get32(); c.height = get32();
    }
    d.data.resize(bytes);
    const bool ok = std::fread(d.data.data(), 1, bytes, f) == bytes;
    std::fclose(f);
    return ok;
}

// Host bytes as 4x4 BC blocks of `block` bytes, every level and layer.
uint64_t BcBytes(const Dump& d, uint32_t block) {
    uint64_t total = 0;
    for (uint32_t level = 0; level < d.levels; ++level) {
        const uint32_t w = std::max(d.width >> level, 1u), h = std::max(d.height >> level, 1u);
        total += uint64_t{(w + 3) / 4} * ((h + 3) / 4) * block;
    }
    return total * d.layers;
}

struct Stats {
    uint64_t texels = 0, mid_alpha = 0, low_alpha = 0, errors = 0;
    int max_gray_diff = 0;  // largest |r-g| or |g-b| (0 = grey)
};

// Decodes level 0 of every layer.
Stats Analyze(const Dump& d) {
    Stats s;
    uint8_t px[12 * 12][4];
    for (const Copy& c : d.copies) {
        if (c.level != 0) continue;
        const uint32_t row = c.row_length ? c.row_length : c.width;
        const uint32_t rows = c.image_height ? c.image_height : c.height;
        const uint32_t bx = (row + d.bw - 1) / d.bw, by = (rows + d.bh - 1) / d.bh;
        const uint64_t layer_bytes = uint64_t{bx} * by * 16;
        for (uint32_t layer = 0; layer < c.layers; ++layer) {
            const uint64_t base = c.offset + layer * layer_bytes;
            for (uint32_t y = 0; y * d.bh < c.height; ++y) {
                for (uint32_t x = 0; x * d.bw < c.width; ++x) {
                    const uint64_t at = base + (uint64_t{y} * bx + x) * 16;
                    if (at + 16 > d.data.size()) return s;
                    if (!refract::astc::decode_block(&d.data[at], d.bw, d.bh, d.srgb != 0, px)) ++s.errors;
                    const uint32_t w = std::min(d.bw, c.width - x * d.bw), h = std::min(d.bh, c.height - y * d.bh);
                    for (uint32_t ty = 0; ty < h; ++ty) {
                        for (uint32_t tx = 0; tx < w; ++tx) {
                            const uint8_t* p = px[ty * d.bw + tx];
                            ++s.texels;
                            s.low_alpha += p[3] < 128;
                            s.mid_alpha += p[3] >= 8 && p[3] <= 247;
                            s.max_gray_diff = std::max({s.max_gray_diff, std::abs(p[0] - p[1]), std::abs(p[1] - p[2])});
                        }
                    }
                }
            }
        }
    }
    return s;
}

int AnalyzeDir(const fs::path& dir, const char* csv_path) {
    std::vector<fs::path> files;
    for (const auto& e : fs::directory_iterator(dir))
        if (e.path().extension() == ".astc") files.push_back(e.path());
    std::sort(files.begin(), files.end());
    struct Row { std::string name; Dump info; Stats s; };
    std::vector<Row> rows(files.size());
    std::atomic<size_t> next{0};
    std::vector<std::thread> threads;
    for (unsigned t = 0; t < std::max(1u, std::thread::hardware_concurrency()); ++t) {
        threads.emplace_back([&] {
            for (size_t i; (i = next.fetch_add(1)) < files.size();) {
                Dump d;
                if (!Load(files[i], d)) { rows[i].name.clear(); continue; }
                rows[i].name = files[i].stem().string();
                rows[i].s = Analyze(d);
                d.data.clear(); d.data.shrink_to_fit();
                rows[i].info = std::move(d);
            }
        });
    }
    for (auto& t : threads) t.join();

    // Classes: opaque (alpha 255 everywhere: BC1), cutout (alpha only near 0/255: BC1 with 1-bit
    // alpha), grey+alpha (two channels, e.g. normal maps: BC5, same size as BC3), alpha (BC3).
    enum { Opaque, Cutout, GreyAlpha, Alpha, Classes };
    static const char* names[Classes] = {"opaque", "cutout", "grey+alpha", "alpha"};
    uint64_t count[Classes] = {}, guest[Classes] = {}, bc1[Classes] = {}, bc3[Classes] = {};
    std::FILE* csv = csv_path ? std::fopen(csv_path, "w") : nullptr;
    if (csv) std::fprintf(csv, "hash,width,height,levels,layers,block,srgb,class,low_alpha,mid_alpha,gray_diff,errors,guest_bytes\n");
    size_t loaded = 0;
    for (const Row& r : rows) {
        if (r.name.empty()) continue;
        ++loaded;
        const Stats& s = r.s;
        const int cls = s.low_alpha == 0 && s.mid_alpha == 0 ? Opaque
                        : s.mid_alpha * 200 <= s.texels      ? Cutout  // under 0.5% partial alpha
                        : s.max_gray_diff <= 2               ? GreyAlpha
                                                             : Alpha;
        ++count[cls];
        guest[cls] += r.info.guest_bytes;
        bc1[cls] += BcBytes(r.info, 8);
        bc3[cls] += BcBytes(r.info, 16);
        if (csv)
            std::fprintf(csv, "%s,%u,%u,%u,%u,%ux%u,%u,%s,%llu,%llu,%d,%llu,%llu\n", r.name.c_str(), r.info.width,
                         r.info.height, r.info.levels, r.info.layers, r.info.bw, r.info.bh, r.info.srgb, names[cls],
                         (unsigned long long)s.low_alpha, (unsigned long long)s.mid_alpha, s.max_gray_diff,
                         (unsigned long long)s.errors, (unsigned long long)r.info.guest_bytes);
    }
    if (csv) std::fclose(csv);
    const auto mib = [](uint64_t b) { return static_cast<double>(b) / (1 << 20); };
    std::printf("%zu textures\n%-11s %6s %10s %10s %10s\n", loaded, "class", "count", "ASTC MiB", "BC1 MiB", "BC3 MiB");
    for (int c = 0; c < Classes; ++c)
        std::printf("%-11s %6llu %10.1f %10.1f %10.1f\n", names[c], (unsigned long long)count[c], mib(guest[c]),
                    mib(bc1[c]), mib(bc3[c]));
    return 0;
}

int WritePng(const fs::path& in, const char* out) {
    Dump d;
    if (!Load(in, d)) { std::fprintf(stderr, "cannot read %s\n", in.string().c_str()); return 1; }
    const auto it = std::find_if(d.copies.begin(), d.copies.end(), [](const Copy& c) { return c.level == 0; });
    if (it == d.copies.end()) return 1;
    const Copy& c = *it;
    const uint32_t row = c.row_length ? c.row_length : c.width;
    const uint32_t bx = (row + d.bw - 1) / d.bw;
    // Colour on the left, alpha as grey on the right.
    std::vector<uint8_t> image(size_t{c.width} * 2 * c.height * 3);
    uint8_t px[12 * 12][4];
    for (uint32_t y = 0; y * d.bh < c.height; ++y) {
        for (uint32_t x = 0; x * d.bw < c.width; ++x) {
            refract::astc::decode_block(&d.data[c.offset + (uint64_t{y} * bx + x) * 16], d.bw, d.bh, d.srgb != 0, px);
            for (uint32_t ty = 0; ty < d.bh && y * d.bh + ty < c.height; ++ty) {
                for (uint32_t tx = 0; tx < d.bw && x * d.bw + tx < c.width; ++tx) {
                    const uint8_t* p = px[ty * d.bw + tx];
                    uint8_t* o = &image[((size_t{y * d.bh + ty}) * c.width * 2 + x * d.bw + tx) * 3];
                    o[0] = p[0]; o[1] = p[1]; o[2] = p[2];
                    o += size_t{c.width} * 3;
                    o[0] = o[1] = o[2] = p[3];
                }
            }
        }
    }
    return stbi_write_png(out, c.width * 2, c.height, 3, image.data(), c.width * 2 * 3) ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc >= 3 && std::strcmp(argv[1], "analyze") == 0) return AnalyzeDir(argv[2], argc >= 4 ? argv[3] : nullptr);
    if (argc >= 4 && std::strcmp(argv[1], "png") == 0) return WritePng(argv[2], argv[3]);
    std::fprintf(stderr, "usage: texbake analyze <dir> [csv] | png <file.astc> <out.png>\n");
    return 1;
}
