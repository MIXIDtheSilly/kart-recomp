// HOST_NVN_DUMP=<frame>[,<dir>]: a picture of the renderer's work in one frame, for finding the pass that goes
// wrong without a graphics debugger. Every draw and dispatch of the submissions between present <frame> and
// the next is logged, and the colour targets are saved as <dir>/dump_<seq>_c<i>.bmp each time a rendering
// scope ends (blitted to RGBA8, so float targets are clamped to 0..1).
#include "dump.h"

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

#include "../nvn_int.h"
#include "draw.h"
#include "present.h"
#include "replay_int.h"
#include "vkq.h"

namespace nvk {
namespace {

struct Pending {
    VkImage img = VK_NULL_HANDLE;
    VmaAllocation img_alloc = VK_NULL_HANDLE;
    VkBuffer buf = VK_NULL_HANDLE;
    VmaAllocation buf_alloc = VK_NULL_HANDLE;
    void* ptr = nullptr;
    u32 w = 0, h = 0;
    std::string path;
};
std::vector<Pending> g_pending;
bool g_active = false;
bool g_trace = false;   // this draw is traced (HOST_NVN_TRACE_PS): its notes go to g_ring, logged at device loss
std::mutex g_ring_mu;
std::deque<std::string> g_ring;
int g_seq = 0;
long long g_frame_want = -1, g_frame_last = -1;   // <a>[-<b>]: frames a..b
double g_time_want = -1;   // HOST_NVN_DUMP=t<seconds>: the first whole frame after that time
std::string g_dir = ".";
// HOST_NVN_DUMP_NOTARGETS=1: no images, and the notes go to <dir>/dump_notes.txt through a big buffer (the log's line-by-line
// writes made a frame of notes take seconds)
FILE* notes_file() {
    static FILE* f = [] {
        FILE* f = std::fopen((g_dir + "/dump_notes.txt").c_str(), "w");
        if (f) setvbuf(f, nullptr, _IOFBF, 64 << 20);
        return f;
    }();
    return f;
}
bool g_no_targets = false;
std::atomic<s64> g_arm_from{-1}, g_arm_to{-1};   // dump_arm: frames by present number

// HOST_NVN_DUMP_RING: frames by present number. A flash is written with this many frames on each side of it: the game's
// passes repeat every 6 frames, so the frames to compare it with are 6 away, and a flash is only found 2 or 3 frames late.
struct RingFrame { s64 num; std::string text; };
constexpr s64 kRingSide = 7;
constexpr size_t kRingFiles = 64, kRingFrames = 32;
const char* const g_ring_dir = getenv("HOST_NVN_DUMP_RING");
const bool g_ring_detail = getenv("HOST_NVN_DUMP_RING_DETAIL") != nullptr;   // every binding of every draw (hashes of its buffers) too
std::mutex g_ring_frames_mu;
std::deque<RingFrame> g_ring_frames;
std::deque<s64> g_ring_wanted;
size_t g_ring_written = 0;

// (g_ring_frames_mu held)
void ring_write(s64 flash) {
    char path[512];
    snprintf(path, sizeof path, "%s/ring_%06lld.txt", g_ring_dir, static_cast<long long>(flash));
    FILE* f = std::fopen(path, "wb");
    if (!f) return;
    for (const RingFrame& r : g_ring_frames) {
        if (r.num < flash - kRingSide || r.num > flash + kRingSide) continue;
        std::fprintf(f, "frame %lld\n", static_cast<long long>(r.num));
        std::fwrite(r.text.data(), 1, r.text.size(), f);
    }
    std::fclose(f);
}

void ring_sub_begin() {
    g_active = g_no_targets = true;
    const s64 f = static_cast<s64>(g_presents.load());
    std::scoped_lock lk{g_ring_frames_mu};
    if (!g_ring_frames.empty() && g_ring_frames.back().num == f) return;
    while (!g_ring_wanted.empty() && f > g_ring_wanted.front() + kRingSide) {
        ring_write(g_ring_wanted.front());
        g_ring_wanted.pop_front();
    }
    g_ring_frames.push_back({f, {}});
    g_ring_frames.back().text.reserve(1 << 20);
    if (g_ring_frames.size() > kRingFrames) g_ring_frames.pop_front();
}

void barrier(VkCommandBuffer cb) { full_barrier(cb); }

constexpr u32 kQueries = 32768;
VkQueryPool g_queries = VK_NULL_HANDLE;
u32 g_query_next = 0, g_query_read = 0;
struct Recheck { u64 addr; std::vector<u8> was; };
std::vector<Recheck> g_recheck;

}  // namespace

bool dump_active() { return g_active; }
bool dump_brief() { return g_no_targets; }
bool dump_buffers() { return g_active && (!g_no_targets || g_ring_detail); }

void dump_arm(u64 from, u64 to, const char* dir) {
    g_dir = dir;
    g_arm_to = static_cast<s64>(to);
    g_arm_from = static_cast<s64>(from);
}

void dump_ring_flash(s64 num) {
    if (!g_ring_dir) return;
    std::scoped_lock lk{g_ring_frames_mu};
    static s64 last = -1000;   // (flashes come in runs: the first of each)
    if (num - last < 20) return;
    last = num;
    if (g_ring_written++ < kRingFiles) g_ring_wanted.push_back(num);
}

void dump_sub_begin() {
    if (g_ring_dir) return ring_sub_begin();
    static bool parsed = false;
    if (!parsed) {
        parsed = true;
        if (const char* e = getenv("HOST_NVN_DUMP")) {
            if (e[0] == 't') g_time_want = atof(e + 1);
            else {
                g_frame_want = atoll(e);
                if (const char* d = strchr(e, '-')) g_frame_last = atoll(d + 1);
            }
            if (const char* c = strchr(e, ',')) g_dir = c + 1;
        }
    }
    static const u64 t0 = host_ns();  // the first submission
    // t<seconds>: the first whole frame after that time that follows a frame of over 2500 draws (a race frame, not a menu)
    static long long cur = -1;
    static u64 at_start = 0, last_draws = 0;
    if (const long long f = static_cast<long long>(nvn::g_frame.load()); f != cur) {
        const u64 total = draw_total();
        last_draws = total - at_start;
        at_start = total, cur = f;
    }
    if (g_time_want >= 0 && g_frame_want < 0 && host_ns() - t0 >= static_cast<u64>(g_time_want * 1e9) && last_draws > 2500)
        g_frame_want = static_cast<long long>(nvn::g_frame.load()) + 1;
    long long f = static_cast<long long>(nvn::g_frame.load());
    g_active = g_frame_want >= 0 && f >= g_frame_want && f <= std::max(g_frame_want, g_frame_last);
    if (const s64 a = g_arm_from.load(); a >= 0) {   // the renderer's own present count: g_frame runs ahead of the replay
        f = static_cast<long long>(g_presents.load());
        g_active = f >= a && f <= g_arm_to.load();
    }
    static long long logged = -1;
    static const bool no_targets = getenv("HOST_NVN_DUMP_NOTARGETS") != nullptr;
    g_no_targets = no_targets;
    if (g_active && f != logged) {
        logged = f;
        if ((g_no_targets || g_arm_from >= 0) && notes_file()) std::fprintf(notes_file(), "frame %lld\n", f);
        else LOG("dump: frame %lld", f);
    }
    static bool closed = false;
    if (!g_active && (g_no_targets || g_arm_from >= 0) && logged >= 0 && !closed) closed = true, notes_file() && std::fflush(notes_file());
}

void dump_trace(bool on) { g_trace = on; }
bool dump_tracing() { return g_trace; }

void dump_note(const char* fmt, ...) {
    if (!g_active && !g_trace) return;
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (g_active && g_ring_dir) {
        std::scoped_lock lk{g_ring_frames_mu};
        if (!g_ring_frames.empty()) g_ring_frames.back().text.append(buf).push_back('\n');
        return;
    }
    if (g_active) {
        if ((g_no_targets || g_arm_from >= 0) && notes_file()) std::fprintf(notes_file(), "%s\n", buf);
        else LOG("dump: %s", buf);
        return;
    }
    std::scoped_lock lk{g_ring_mu};
    g_ring.emplace_back(buf);
    if (g_ring.size() > 1500) g_ring.pop_front();
}

void dump_trace_print() {
    std::scoped_lock lk{g_ring_mu};
    for (const std::string& l : g_ring) LOG("trace: %s", l.c_str());
}

void dump_targets(VkCommandBuffer cb) {
    if (!g_active || g_no_targets) return;
    Device& c = ctx();
    const int seq = g_seq++;
    for (int i = 0; i < R.ncolor; i++) {
        const Target& t = R.color[i];
        if (!t.img || !t.view) continue;
        const u32 w = std::max(t.img->width >> t.level, 1u), h = std::max(t.img->height >> t.level, 1u);
        Pending p;
        p.w = w, p.h = h;
        VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ci.imageType = VK_IMAGE_TYPE_2D;
        ci.format = VK_FORMAT_R8G8B8A8_UNORM;
        ci.extent = {w, h, 1};
        ci.mipLevels = 1, ci.arrayLayers = 1;
        ci.samples = VK_SAMPLE_COUNT_1_BIT;
        ci.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        VmaAllocationCreateInfo ai{};
        ai.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
        if (vmaCreateImage(c.vma, &ci, &ai, &p.img, &p.img_alloc, nullptr) != VK_SUCCESS) continue;
        VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bci.size = static_cast<u64>(w) * h * 4;
        bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        VmaAllocationCreateInfo bai{};
        bai.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST;
        bai.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
        VmaAllocationInfo info;
        if (vmaCreateBuffer(c.vma, &bci, &bai, &p.buf, &p.buf_alloc, &info) != VK_SUCCESS) continue;
        p.ptr = info.pMappedData;
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = p.img;
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
        VkImageBlit bl{};
        const bool is3d = t.img->type == VK_IMAGE_TYPE_3D;
        bl.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, t.level, is3d ? 0 : t.layer, 1};
        bl.srcOffsets[0] = {0, 0, is3d ? static_cast<int>(t.layer) : 0};
        bl.srcOffsets[1] = {static_cast<int>(w), static_cast<int>(h), is3d ? static_cast<int>(t.layer) + 1 : 1};
        bl.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        bl.dstOffsets[1] = {static_cast<int>(w), static_cast<int>(h), 1};
        vkCmdBlitImage(cb, t.img->image, VK_IMAGE_LAYOUT_GENERAL, p.img, VK_IMAGE_LAYOUT_GENERAL, 1, &bl, VK_FILTER_NEAREST);
        barrier(cb);
        VkBufferImageCopy r{};
        r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        r.imageExtent = {w, h, 1};
        vkCmdCopyImageToBuffer(cb, p.img, VK_IMAGE_LAYOUT_GENERAL, p.buf, 1, &r);
        barrier(cb);
        char name[512];
        snprintf(name, sizeof name, "%s/dump_%03d_c%d.bmp", g_dir.c_str(), seq, i);
        p.path = name;
        LOG("dump: scope %d target %d: %ux%u layer %u vk format %d (image %llu, NVN format %#x)", seq, i, w, h, t.layer, t.format,
            static_cast<unsigned long long>(t.img->serial), t.img->nvn_format);
        g_pending.push_back(std::move(p));
    }
}

void dump_recheck(u64 addr, const u8* now, size_t n) {
    if (g_active && n) g_recheck.push_back({addr, std::vector<u8>(now, now + n)});
}

u32 dump_query_begin(VkCommandBuffer cb) {
    if (!g_active || g_no_targets) return ~0u;
    Device& c = ctx();
    if (!g_queries) {
        VkQueryPoolCreateInfo qi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        qi.queryType = VK_QUERY_TYPE_OCCLUSION;
        qi.queryCount = kQueries;
        if (vkCreateQueryPool(c.dev, &qi, nullptr, &g_queries) != VK_SUCCESS) return ~0u;
    }
    if (g_query_next >= kQueries) return ~0u;
    const u32 q = g_query_next++;
    vkResetQueryPool(c.dev, g_queries, q, 1);
    vkCmdBeginQuery(cb, g_queries, q, VK_QUERY_CONTROL_PRECISE_BIT);
    return q;
}

void dump_query_end(VkCommandBuffer cb, u32 q) {
    if (q != ~0u) vkCmdEndQuery(cb, g_queries, q);
}

void dump_flush(u64 value) {
    if (g_pending.empty() && g_query_read == g_query_next && g_recheck.empty()) return;
    vkq::sync();   // (the dump modes keep the driver thread off; in case one does not)
    Device& c = ctx();
    VkSemaphoreWaitInfo wi{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
    const VkSemaphore sem = timeline();
    wi.semaphoreCount = 1;
    wi.pSemaphores = &sem;
    wi.pValues = &value;
    vkWaitSemaphores(c.dev, &wi, UINT64_MAX);
    if (g_query_read < g_query_next) {
        std::vector<u64> r(g_query_next - g_query_read);
        if (vkGetQueryPoolResults(c.dev, g_queries, g_query_read, static_cast<u32>(r.size()), r.size() * 8, r.data(), 8,
                                  VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT) == VK_SUCCESS)
            for (size_t i = 0; i < r.size(); i++)
                dump_note("samples q%u %llu", g_query_read + static_cast<u32>(i), static_cast<unsigned long long>(r[i]));
        g_query_read = g_query_next;
    }
    for (const Recheck& r : g_recheck) {
        std::vector<u8> now(r.was.size());
        nvn::gpu_read(r.addr, now.data(), now.size());
        u32 rows = 0;
        for (size_t at = 0; at + 32 <= now.size(); at += 32)
            if (std::memcmp(now.data() + at, r.was.data() + at, 32)) {
                if (rows++ < 64) {
                    u32 w[8];
                    std::memcpy(w, now.data() + at, 32);
                    dump_note("    after %llx+%04zx: %08x %08x %08x %08x %08x %08x %08x %08x", static_cast<unsigned long long>(r.addr), at, w[0],
                              w[1], w[2], w[3], w[4], w[5], w[6], w[7]);
                }
            }
        dump_note("recheck %llx+%zx: %u rows changed after the submission", static_cast<unsigned long long>(r.addr), now.size(), rows);
    }
    g_recheck.clear();
    for (Pending& p : g_pending) {
        if (FILE* f = std::fopen(p.path.c_str(), "wb")) {
            const u32 bytes = p.w * p.h * 4;
            u8 hdr[54]{'B', 'M'};
            auto put = [&](int at, u32 v) { std::memcpy(hdr + at, &v, 4); };
            put(2, 54 + bytes), put(10, 54), put(14, 40), put(18, p.w), put(22, static_cast<u32>(-static_cast<s32>(p.h)));
            hdr[26] = 1, hdr[28] = 32;
            put(34, bytes);
            std::fwrite(hdr, 1, 54, f);
            std::vector<u8> row(p.w * 4);
            const u8* src = static_cast<const u8*>(p.ptr);
            for (u32 y = 0; y < p.h; y++) {
                std::memcpy(row.data(), src + static_cast<size_t>(y) * p.w * 4, row.size());
                for (u32 x = 0; x < p.w; x++) {
                    std::swap(row[x * 4], row[x * 4 + 2]);
                    row[x * 4 + 3] = 255;
                }
                std::fwrite(row.data(), 1, row.size(), f);
            }
            std::fclose(f);
        }
        vmaDestroyBuffer(c.vma, p.buf, p.buf_alloc);
        vmaDestroyImage(c.vma, p.img, p.img_alloc);
    }
    g_pending.clear();
}

}  // namespace nvk
