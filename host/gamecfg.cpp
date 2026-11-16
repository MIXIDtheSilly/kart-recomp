// The game's own quality settings, changed from HOST_* settings as the romfs is built (services/fs.cpp).
//
// The game reads its performance settings from romfs System/GymlArchive/general_format.<version>.sarc.zs, a zstd'd SARC of
// BYML files. Two of them matter here:
//   Gyml/GamePerformanceGraphicsConfigGParam/*: LODDistanceRatio (2.0) and the 3D render heights: ResolutionLowFor3D
//     (handheld, 1080), ResolutionMiddleFor3D (docked, 1440), and their ForDiv / ForDiv4 forms (two / four player split screen).
//   Gyml/ProceduralGrassLodDistanceParam/Grass_Near, Grass_Mid: the grass's area and LOD distances.
// Their numbers are stored inside the BYML nodes, so they change in place; the archive goes back to the game as a zstd frame of
// stored blocks (only zstd's decoder is linked in), 9 MB instead of 1.3 MB.
//
//   HOST_RES_3D=<height>  the 3D render height in every mode (720, 900, 1080, 1440). The game builds its render targets at it and
//                         scales to the screen (handheld 1080, docked 1440). Above 1440 the game crashes on its first frame (its
//                         render graph lacks an object; main+0xa4128 in 1.8.0), so it is capped there.
//   HOST_LOD=<ratio>      the game's LODDistanceRatio (2 by default): higher keeps detailed models and grass further away.
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "common.h"
#include "os.h"
#include "zstd.h"

namespace gamecfg {
namespace {

struct Settings {
    int res3d = 0;    // 0: the game's own
    float lod = 0;    // 0: the game's own
    bool any() const { return res3d || lod > 0; }
};

const Settings& settings() {
    static const Settings s = [] {
        Settings s;
        if (const char* e = std::getenv("HOST_RES_3D"); e && *e) {
            const int v = std::atoi(e);
            if (v > 0) {
                s.res3d = std::clamp(v, 360, 1440);
                if (s.res3d != v) LOG("gamecfg: HOST_RES_3D=%d: capped to %d (the game crashes above 1440)", v, s.res3d);
            }
        }
        if (const char* e = std::getenv("HOST_LOD"); e && *e) {
            const float v = static_cast<float>(std::atof(e));
            if (v > 0) s.lod = std::clamp(v, 0.5f, 8.0f);
        }
        return s;
    }();
    return s;
}

u32 rd32(const std::vector<u8>& d, size_t o) { u32 v; std::memcpy(&v, d.data() + o, 4); return v; }
u32 rd24(const std::vector<u8>& d, size_t o) { return d[o] | d[o + 1] << 8 | d[o + 2] << 16; }
u16 rd16(const std::vector<u8>& d, size_t o) { return static_cast<u16>(d[o] | d[o + 1] << 8); }

// The scalars of a little-endian BYML file, as (path, type, offset of the 4-byte value). Paths: "A.B", arrays "A[2]".
struct Scalar { std::string path; u8 type; size_t at; };

std::vector<Scalar> byml_scalars(const std::vector<u8>& d, size_t base, size_t size) {
    std::vector<Scalar> out;
    if (size < 16 || d[base] != 'Y' || d[base + 1] != 'B') return out;
    const size_t keys_off = rd32(d, base + 4), root = rd32(d, base + 12);
    std::vector<std::string> keys;
    if (keys_off && keys_off + 4 <= size && d[base + keys_off] == 0xC2) {
        const u32 n = rd24(d, base + keys_off + 1);
        for (u32 i = 0; i < n; i++) {
            const size_t s = base + keys_off + rd32(d, base + keys_off + 4 + 4 * i);
            size_t e = s;
            while (e < base + size && d[e]) e++;
            keys.emplace_back(reinterpret_cast<const char*>(d.data() + s), e - s);
        }
    }
    auto walk = [&](auto& self, size_t off, const std::string& pfx, int depth) -> void {
        if (depth > 16 || off + 4 > size) return;
        const u8 t = d[base + off];
        const u32 n = rd24(d, base + off + 1);
        auto value = [&](const std::string& path, u8 type, size_t at) {
            if (type == 0xC0 || type == 0xC1) self(self, rd32(d, at), path, depth + 1);
            else if (type >= 0xD0 && type <= 0xD3) out.push_back({path, type, at});
        };
        if (t == 0xC1) {
            for (u32 i = 0; i < n; i++) {
                const size_t e = base + off + 4 + 8 * i;
                const u32 k = rd24(d, e);
                value((pfx.empty() ? "" : pfx + ".") + (k < keys.size() ? keys[k] : "?"), d[e + 3], e + 4);
            }
        } else if (t == 0xC0) {
            const size_t vals = base + off + 4 + ((n + 3) & ~3u);
            for (u32 i = 0; i < n; i++) value(pfx + "[" + std::to_string(i) + "]", d[base + off + 4 + i], vals + 4 * i);
        }
    };
    if (root) walk(walk, root, "", 0);
    return out;
}

float get_f(const std::vector<u8>& d, size_t at) { float f; std::memcpy(&f, d.data() + at, 4); return f; }
void set_f(std::vector<u8>& d, size_t at, float f) { std::memcpy(d.data() + at, &f, 4); }
void set_i(std::vector<u8>& d, size_t at, int v) { std::memcpy(d.data() + at, &v, 4); }

// Changes the files of one SARC in place; the number of values changed.
int patch_sarc(std::vector<u8>& d) {
    if (d.size() < 0x20 || std::memcmp(d.data(), "SARC", 4) != 0 || rd16(d, 6) != 0xFEFF) return 0;
    const size_t hdr = rd16(d, 4), data_off = rd32(d, 12);
    if (std::memcmp(d.data() + hdr, "SFAT", 4) != 0) return 0;
    const u32 n = rd16(d, hdr + 6);
    const size_t names = hdr + 12 + 16 * static_cast<size_t>(n) + 8;
    const Settings& s = settings();
    int changed = 0;
    for (u32 i = 0; i < n; i++) {
        const size_t node = hdr + 12 + 16 * i;
        const u32 attr = rd32(d, node + 4);
        const size_t lo = data_off + rd32(d, node + 8), hi = data_off + rd32(d, node + 12);
        if (!(attr >> 24) || hi > d.size() || lo >= hi) continue;
        const char* name = reinterpret_cast<const char*>(d.data() + names + (attr & 0xFFFFFF) * 4);
        const bool graphics = std::strncmp(name, "Gyml/GamePerformanceGraphicsConfigGParam/", 41) == 0;
        const bool grass = std::strncmp(name, "Gyml/ProceduralGrassLodDistanceParam/", 37) == 0;
        if (!graphics && !grass) continue;
        int here = 0;
        for (const Scalar& v : byml_scalars(d, lo, hi - lo)) {
            if (graphics && s.res3d && v.type == 0xD1 && v.path.rfind("Resolution", 0) == 0 && v.path.find("For3D") != std::string::npos)
                set_i(d, v.at, s.res3d), here++;
            else if (graphics && s.lod > 0 && v.type == 0xD2 && v.path == "LODDistanceRatio")
                set_f(d, v.at, s.lod), here++;
            else if (grass && s.lod > 0 && v.type == 0xD2 && (v.path == "AreaDist" || v.path.rfind("LodDist[", 0) == 0))
                set_f(d, v.at, get_f(d, v.at) * s.lod / 2.0f), here++;   // the game's ratio is 2: grass scales by the change
        }
        if (here) LOG("gamecfg: %s: %d values changed", name, here);
        changed += here;
    }
    return changed;
}

// One zstd frame of stored blocks, recording its content size.
std::vector<u8> zstd_store(const std::vector<u8>& raw) {
    std::vector<u8> out{0x28, 0xB5, 0x2F, 0xFD, 0xE0};   // magic; single segment, 8-byte content size
    const u64 size = raw.size();
    for (int i = 0; i < 8; i++) out.push_back(static_cast<u8>(size >> (8 * i)));
    constexpr size_t kBlock = 128 * 1024;
    size_t at = 0;
    do {
        const size_t n = std::min(kBlock, raw.size() - at);
        const u32 h = (n + at == raw.size() ? 1u : 0u) | static_cast<u32>(n) << 3;   // last flag, raw block, size
        out.push_back(static_cast<u8>(h)), out.push_back(static_cast<u8>(h >> 8)), out.push_back(static_cast<u8>(h >> 16));
        out.insert(out.end(), raw.begin() + static_cast<std::ptrdiff_t>(at), raw.begin() + static_cast<std::ptrdiff_t>(at + n));
        at += n;
    } while (at < raw.size());
    return out;
}

}  // namespace

// The bytes the game gets for the romfs file at `rel` (forward slashes), when a setting changes it; else nothing.
std::optional<std::vector<u8>> patched(const std::string& rel, const std::filesystem::path& disk) {
    const Settings& s = settings();
    if (!s.any()) return std::nullopt;
    const std::string dir = "System/GymlArchive/general_format.";
    if (rel.rfind(dir, 0) != 0 || rel.size() < 8 || rel.compare(rel.size() - 8, 8, ".sarc.zs") != 0) return std::nullopt;
    std::vector<u8> packed;
    if (FILE* f = os::open_file(disk, "rb")) {
        std::error_code ec;
        packed.resize(static_cast<size_t>(std::filesystem::file_size(disk, ec)));
        if (std::fread(packed.data(), 1, packed.size(), f) != packed.size()) packed.clear();
        std::fclose(f);
    }
    const unsigned long long size = packed.empty() ? ZSTD_CONTENTSIZE_ERROR : ZSTD_getFrameContentSize(packed.data(), packed.size());
    if (size == ZSTD_CONTENTSIZE_ERROR || size == ZSTD_CONTENTSIZE_UNKNOWN) {
        LOG("gamecfg: [RED] %s does not unpack: settings not applied", rel.c_str());
        return std::nullopt;
    }
    std::vector<u8> raw(static_cast<size_t>(size));
    if (const size_t n = ZSTD_decompress(raw.data(), raw.size(), packed.data(), packed.size()); ZSTD_isError(n) || n != raw.size()) {
        LOG("gamecfg: [RED] %s does not unpack: settings not applied", rel.c_str());
        return std::nullopt;
    }
    const int changed = patch_sarc(raw);
    LOG("gamecfg: 3D resolution %s, LOD ratio %s: %d values changed in %s", s.res3d ? std::to_string(s.res3d).c_str() : "the game's",
        s.lod > 0 ? std::to_string(s.lod).substr(0, 4).c_str() : "the game's", changed, rel.c_str());
    if (!changed) return std::nullopt;
    return zstd_store(raw);
}

}  // namespace gamecfg
