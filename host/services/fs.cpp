// fsp-srv: romfs (a RomFS image built over the dump's loose romfs/ directory) and save data
// (a host directory).
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <list>
#include <unordered_map>

#include "../host.h"
#include "../ipc.h"

namespace fs = std::filesystem;

namespace ipc {
namespace {

constexpr u32 ResultPathNotFound = MakeResult(2, 1);
constexpr u32 ResultPathAlreadyExists = MakeResult(2, 2);
constexpr u32 ResultOutOfRange = MakeResult(2, 3002);

// ------------------------------------------------------------------ RomFS image

class RomFs {
  public:
    explicit RomFs(const fs::path& root) { build(root); }

    u64 size() const { return image_size; }

    void read(u64 off, u8* out, u64 len) {
        std::memset(out, 0, len);
        // Header and metadata.
        auto copy_from = [&](const std::vector<u8>& src, u64 src_off) {
            const u64 lo = std::max(off, src_off), hi = std::min(off + len, src_off + src.size());
            if (lo < hi) std::memcpy(out + (lo - off), src.data() + (lo - src_off), hi - lo);
        };
        copy_from(header, 0);
        copy_from(meta, meta_offset);
        // File data.
        auto it = std::upper_bound(files.begin(), files.end(), off, [](u64 v, const File& f) { return v < f.offset; });
        if (it != files.begin()) --it;
        for (; it != files.end() && it->offset < off + len; ++it) {
            const u64 lo = std::max(off, it->offset), hi = std::min(off + len, it->offset + it->size);
            if (lo >= hi) continue;
            std::ifstream& f = open(*it);
            f.seekg(static_cast<std::streamoff>(lo - it->offset));
            f.read(reinterpret_cast<char*>(out + (lo - off)), static_cast<std::streamsize>(hi - lo));
            f.clear();
        }
    }

  private:
    struct File {
        u64 offset, size;  // absolute in the image
        fs::path path;
    };
    struct DirNode {
        std::string name;
        u32 parent = 0;
        std::vector<u32> dirs;   // indices into dir_nodes
        std::vector<u32> files;  // indices into file_nodes
        u32 meta_off = 0;
    };
    struct FileNode {
        std::string name;
        u32 parent;
        u64 data_off, size;
        u32 meta_off = 0;
    };

    std::vector<u8> header, meta;
    u64 meta_offset = 0, image_size = 0;
    std::vector<File> files;
    std::unordered_map<size_t, std::ifstream> open_files;
    std::list<size_t> open_order;

    std::ifstream& open(const File& f) {
        const size_t idx = static_cast<size_t>(&f - files.data());
        auto it = open_files.find(idx);
        if (it != open_files.end()) return it->second;
        if (open_files.size() >= 64) {
            open_files.erase(open_order.front());
            open_order.pop_front();
        }
        open_order.push_back(idx);
        return open_files.emplace(idx, std::ifstream(f.path, std::ios::binary)).first->second;
    }

    static u32 path_hash(u32 parent, const std::string& name) {
        u32 h = parent ^ 123456789;
        for (unsigned char ch : name) {
            h = (h >> 5) | (h << 27);
            h ^= ch;
        }
        return h;
    }
    static u32 bucket_count(u32 n) {
        if (n < 3) return 3;
        if (n < 19) return n | 1;
        u32 c = n;
        while (c % 2 == 0 || c % 3 == 0 || c % 5 == 0 || c % 7 == 0 || c % 11 == 0 || c % 13 == 0 || c % 17 == 0) c++;
        return c;
    }

    void build(const fs::path& root) {
        std::vector<DirNode> dirs(1);
        std::vector<FileNode> fnodes;
        std::vector<fs::path> fpaths;
        std::vector<std::pair<fs::path, u32>> stack{{root, 0}};
        while (!stack.empty()) {
            auto [path, idx] = stack.back();
            stack.pop_back();
            std::vector<fs::directory_entry> entries;
            for (auto& e : fs::directory_iterator(path)) entries.push_back(e);
            std::sort(entries.begin(), entries.end(), [](auto& a, auto& b) { return a.path().filename() < b.path().filename(); });
            for (auto& e : entries) {
                const auto u8name = e.path().filename().u8string();
                const std::string name(u8name.begin(), u8name.end());
                if (e.is_directory()) {
                    const u32 d = static_cast<u32>(dirs.size());
                    dirs.push_back({name, idx, {}, {}, 0});
                    dirs[idx].dirs.push_back(d);
                    stack.push_back({e.path(), d});
                } else {
                    const u32 f = static_cast<u32>(fnodes.size());
                    fnodes.push_back({name, idx, 0, static_cast<u64>(e.file_size()), 0});
                    fpaths.push_back(e.path());
                    dirs[idx].files.push_back(f);
                }
            }
        }

        // Data: at 0x200, each file 16-byte aligned.
        const u64 data_offset = 0x200;
        u64 cur = 0;
        for (size_t i = 0; i < fnodes.size(); i++) {
            cur = (cur + 15) & ~15ull;
            fnodes[i].data_off = cur;
            files.push_back({data_offset + cur, fnodes[i].size, fpaths[i]});
            cur += fnodes[i].size;
        }
        std::sort(files.begin(), files.end(), [](auto& a, auto& b) { return a.offset < b.offset; });

        // Metadata entry offsets.
        u32 off = 0;
        for (auto& d : dirs) {
            d.meta_off = off;
            off += 0x18 + ((static_cast<u32>(d.name.size()) + 3) & ~3u);
        }
        const u32 dir_meta_size = off;
        off = 0;
        for (auto& f : fnodes) {
            f.meta_off = off;
            off += 0x20 + ((static_cast<u32>(f.name.size()) + 3) & ~3u);
        }
        const u32 file_meta_size = off;
        const u32 dir_buckets = bucket_count(static_cast<u32>(dirs.size()));
        const u32 file_buckets = bucket_count(static_cast<u32>(fnodes.size()));
        std::vector<u32> dir_hash(dir_buckets, 0xFFFFFFFF), file_hash(file_buckets, 0xFFFFFFFF);
        std::vector<u8> dir_meta(dir_meta_size), file_meta(file_meta_size);
        auto put32 = [](std::vector<u8>& v, u32 at, u32 x) { std::memcpy(v.data() + at, &x, 4); };
        auto put64 = [](std::vector<u8>& v, u32 at, u64 x) { std::memcpy(v.data() + at, &x, 8); };

        for (size_t i = 0; i < dirs.size(); i++) {
            const DirNode& d = dirs[i];
            const u32 parent_off = dirs[d.parent].meta_off;
            u32 sibling = 0xFFFFFFFF;
            if (i != 0) {
                const auto& sibs = dirs[d.parent].dirs;
                auto pos = std::find(sibs.begin(), sibs.end(), static_cast<u32>(i));
                if (pos + 1 != sibs.end()) sibling = dirs[*(pos + 1)].meta_off;
            }
            const u32 h = path_hash(parent_off, d.name) % dir_buckets;
            put32(dir_meta, d.meta_off + 0x00, parent_off);
            put32(dir_meta, d.meta_off + 0x04, sibling);
            put32(dir_meta, d.meta_off + 0x08, d.dirs.empty() ? 0xFFFFFFFF : dirs[d.dirs[0]].meta_off);
            put32(dir_meta, d.meta_off + 0x0C, d.files.empty() ? 0xFFFFFFFF : fnodes[d.files[0]].meta_off);
            put32(dir_meta, d.meta_off + 0x10, dir_hash[h]);
            put32(dir_meta, d.meta_off + 0x14, static_cast<u32>(d.name.size()));
            std::memcpy(dir_meta.data() + d.meta_off + 0x18, d.name.data(), d.name.size());
            dir_hash[h] = d.meta_off;
        }
        for (auto& d : dirs) {
            for (size_t k = 0; k < d.files.size(); k++) {
                const FileNode& f = fnodes[d.files[k]];
                const u32 parent_off = d.meta_off;
                const u32 h = path_hash(parent_off, f.name) % file_buckets;
                put32(file_meta, f.meta_off + 0x00, parent_off);
                put32(file_meta, f.meta_off + 0x04, k + 1 < d.files.size() ? fnodes[d.files[k + 1]].meta_off : 0xFFFFFFFF);
                put64(file_meta, f.meta_off + 0x08, f.data_off);
                put64(file_meta, f.meta_off + 0x10, f.size);
                put32(file_meta, f.meta_off + 0x18, file_hash[h]);
                put32(file_meta, f.meta_off + 0x1C, static_cast<u32>(f.name.size()));
                std::memcpy(file_meta.data() + f.meta_off + 0x20, f.name.data(), f.name.size());
                file_hash[h] = f.meta_off;
            }
        }

        meta_offset = (data_offset + cur + 3) & ~3ull;
        u64 o = meta_offset;
        u64 hdr[10];
        hdr[0] = 0x50;
        auto section = [&](size_t bytes, int slot) {
            hdr[slot] = o;
            hdr[slot + 1] = bytes;
            o += bytes;
        };
        section(dir_hash.size() * 4, 1);
        section(dir_meta.size(), 3);
        section(file_hash.size() * 4, 5);
        section(file_meta.size(), 7);
        hdr[9] = data_offset;
        header.assign(reinterpret_cast<u8*>(hdr), reinterpret_cast<u8*>(hdr) + sizeof hdr);
        auto append = [&](const void* p, size_t n) {
            meta.insert(meta.end(), static_cast<const u8*>(p), static_cast<const u8*>(p) + n);
        };
        append(dir_hash.data(), dir_hash.size() * 4);
        append(dir_meta.data(), dir_meta.size());
        append(file_hash.data(), file_hash.size() * 4);
        append(file_meta.data(), file_meta.size());
        image_size = o;
        LOG("romfs: %zu directories, %zu files, %.1f MiB of data", dirs.size(), fnodes.size(), cur / 1048576.0);
    }
};

RomFs& romfs() {
    static RomFs r(g_config.game_dir / "romfs");
    return r;
}

struct RomStorage : Service {
    RomStorage() : Service("IStorage") {
        on(0, [](Request& r) {  // Read(offset, size)
            const u64 off = r.arg<u64>(0), size = r.arg<u64>(8);
            const Buffer out = r.out_buf(0);
            const u64 n = std::min(size, out.size);
            std::scoped_lock lk{lock()};
            romfs().read(off, gptr(out.addr), n);
        });
        on(4, [](Request& r) { r.push<u64>(romfs().size()); });
        on(5, [](Request& r) {  // OperateRange: query range info
            u8 info[0x40]{};
            r.push_bytes(info, sizeof info);
        });
    }
    static std::mutex& lock() {
        static std::mutex m;
        return m;
    }
};

// ------------------------------------------------------------------ host directory file systems

std::string path_arg(const Request& r, size_t i = 0) {
    auto v = r.read_in(i);
    std::string s(reinterpret_cast<const char*>(v.data()), strnlen(reinterpret_cast<const char*>(v.data()), v.size()));
    return s;
}

struct HostFile : Service {
    HostFile(fs::path p, u32 mode_) : Service("IFile"), path(std::move(p)), mode(mode_) {
        f.open(path, std::ios::binary | std::ios::in | ((mode & 2) ? std::ios::out : std::ios::openmode{}));
        on(0, [this](Request& r) {  // Read(option, offset, size)
            const s64 off = r.arg<s64>(8), size = r.arg<s64>(16);
            const Buffer out = r.out_buf(0);
            f.clear();
            f.seekg(off);
            f.read(reinterpret_cast<char*>(gptr(out.addr)), std::min<s64>(size, static_cast<s64>(out.size)));
            r.push<s64>(f.gcount());
        });
        on(1, [this](Request& r) {  // Write(option, offset, size)
            const s64 off = r.arg<s64>(8), size = r.arg<s64>(16);
            const Buffer in = r.in_buf(0);
            f.clear();
            f.seekp(off);
            f.write(reinterpret_cast<const char*>(gptr(in.addr)), std::min<s64>(size, static_cast<s64>(in.size)));
        });
        on(2, [this](Request&) { f.flush(); });
        on(3, [this](Request& r) {  // SetSize
            f.flush();
            std::error_code ec;
            fs::resize_file(path, r.arg<u64>(0), ec);
        });
        on(4, [this](Request& r) {
            std::error_code ec;
            r.push<s64>(static_cast<s64>(fs::file_size(path, ec)));
        });
    }
    fs::path path;
    u32 mode;
    std::fstream f;
};

struct HostDirectory : Service {
    explicit HostDirectory(const fs::path& p) : Service("IDirectory") {
        std::error_code ec;
        for (auto& e : fs::directory_iterator(p, ec)) entries.push_back(e);
        on(0, [this](Request& r) {
            const Buffer out = r.out_buf(0);
            s64 n = 0;
            for (; next < entries.size() && (n + 1) * 0x310 <= static_cast<s64>(out.size); next++, n++) {
                u8 e[0x310]{};
                const std::string name = entries[next].path().filename().string();
                std::memcpy(e, name.data(), std::min<size_t>(name.size(), 0x300));
                e[0x304] = entries[next].is_directory() ? 0 : 1;
                std::error_code ec;
                const s64 size = entries[next].is_directory() ? 0 : static_cast<s64>(entries[next].file_size(ec));
                std::memcpy(e + 0x308, &size, 8);
                std::memcpy(gptr(out.addr + n * 0x310), e, sizeof e);
            }
            r.push<s64>(n);
        });
        on(1, [this](Request& r) { r.push<s64>(static_cast<s64>(entries.size())); });
    }
    std::vector<fs::directory_entry> entries;
    size_t next = 0;
};

struct HostFileSystem : Service {
    explicit HostFileSystem(fs::path root_) : Service("IFileSystem"), root(std::move(root_)) {
        fs::create_directories(root);
        auto full = [this](const Request& r, size_t i = 0) { return root / fs::u8path(path_arg(r, i)).relative_path(); };
        on(0, [full](Request& r) {  // CreateFile(option, size)
            const fs::path p = full(r);
            if (fs::exists(p)) { r.result = ResultPathAlreadyExists; return; }
            std::ofstream(p, std::ios::binary).close();
            std::error_code ec;
            fs::resize_file(p, r.arg<u64>(8), ec);
        });
        on(1, [full](Request& r) {
            std::error_code ec;
            if (!fs::remove(full(r), ec)) r.result = ResultPathNotFound;
        });
        on(2, [full](Request& r) {
            std::error_code ec;
            if (!fs::create_directory(full(r), ec)) r.result = ResultPathAlreadyExists;
        });
        on(3, [full](Request& r) { std::error_code ec; fs::remove(full(r), ec); });
        on(4, [full](Request& r) { std::error_code ec; fs::remove_all(full(r), ec); });
        on(5, [full](Request& r) { std::error_code ec; fs::rename(full(r, 0), full(r, 1), ec); });
        on(6, [full](Request& r) { std::error_code ec; fs::rename(full(r, 0), full(r, 1), ec); });
        on(7, [full](Request& r) {  // GetEntryType
            const fs::path p = full(r);
            std::error_code ec;
            if (!fs::exists(p, ec)) { r.result = ResultPathNotFound; return; }
            r.push<u32>(fs::is_directory(p, ec) ? 0 : 1);
        });
        on(8, [full](Request& r) {  // OpenFile(mode)
            const fs::path p = full(r);
            std::error_code ec;
            if (!fs::is_regular_file(p, ec)) { r.result = ResultPathNotFound; return; }
            r.push_object(std::make_shared<HostFile>(p, r.arg<u32>(0)));
        });
        on(9, [full](Request& r) {
            const fs::path p = full(r);
            std::error_code ec;
            if (!fs::is_directory(p, ec)) { r.result = ResultPathNotFound; return; }
            r.push_object(std::make_shared<HostDirectory>(p));
        });
        on(10, [](Request&) {});
        on(11, [](Request& r) { r.push<s64>(1ll << 30); });
        on(12, [](Request& r) { r.push<s64>(1ll << 30); });
        on(13, [full](Request& r) {
            std::error_code ec;
            for (auto& e : fs::directory_iterator(full(r), ec)) fs::remove_all(e.path(), ec);
        });
    }
    fs::path root;
};

struct FspSrv : Service {
    FspSrv() : Service("fsp-srv") {
        on(1, [](Request&) {});
        on(51, [](Request& r) {  // OpenSaveDataFileSystem(space, attribute)
            r.push_object(std::make_shared<HostFileSystem>(g_config.user_dir / "save"));
        });
        on(52, [](Request& r) { r.push_object(std::make_shared<HostFileSystem>(g_config.user_dir / "save")); });
        on(200, [](Request& r) { r.push_object(std::make_shared<RomStorage>()); });
        on(203, [](Request& r) { r.push_object(std::make_shared<RomStorage>()); });
        on(1005, [](Request& r) { r.push<u32>(0); });
        on(1006, [](Request&) {});  // OutputAccessLogToSdCard
    }
};

}  // namespace

void register_fs() {
    romfs();  // build the image up front
    register_service("fsp-srv", [] { return std::make_shared<FspSrv>(); });
}

}  // namespace ipc
