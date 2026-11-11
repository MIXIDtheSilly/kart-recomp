// Native NVN: the Vulkan renderer behind replay.h.
//
// A submission (rp_sub_begin .. rp_sub_commit) is one Vulkan command buffer, recorded on the game's
// thread while rp_exec walks the records, and submitted at commit with the next value of the timeline
// semaphore (fence value == timeline value: both count commits from 1). A completion thread waits for
// each value, applies the submission's CPU-visible effects (event values, counter reports, copies the GPU
// could not do), and only then publishes the fence (replay.h's completion rule).
//
// Every image is in VK_IMAGE_LAYOUT_GENERAL. Ordering inside a submission is kept with full memory
// barriers between passes, transfers and dispatches; each submission starts with one (presents and other
// submissions on the same queue).
#include <algorithm>
#include <condition_variable>
#include <deque>
#include <list>
#include <unordered_map>
#include <thread>

#include "../nvn_int.h"
#include "pools.h"
#include "copies.h"
#include "draw.h"
#include "hitch.h"
#include "dump.h"
#include "../../watch.h"
#include "present.h"
#include "replay_async.h"
#include "replay_int.h"
#include "sparse.h"
#include "textures.h"

namespace nvk { extern std::atomic<u32> g_dbg_subs; }
#include "vkctx.h"
#include "vkq.h"

namespace nvn {
namespace {

using namespace nvk;

// ---------------------------------------------------------------- effects after completion

struct Post {
    enum Kind { Event, Counter, Copy, Update, Fill } kind;
    volatile uint32_t* val = nullptr;
    uint32_t mode = 0, v = 0, type = 0;
    u64 addr = 0, src = 0, n = 0;
    std::vector<uint8_t> data;
};

struct Submission {
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer cb = VK_NULL_HANDLE;     // the one being recorded
    VkCommandBuffer first = VK_NULL_HANDLE;  // its first; then more[] for the parts after each partial flush
    std::vector<VkCommandBuffer> more;
    size_t more_used = 0;
    u64 value = 0;  // timeline value it signals (0: free)
    std::vector<Post> posts;
};

std::mutex g_mu;                  // g_inflight, g_free, g_done
std::condition_variable g_cv;
std::deque<Submission*> g_inflight;
std::vector<Submission*> g_free;
std::atomic<u64> g_submitted{0}, g_done{0};
Submission* g_cur = nullptr;      // the submission being recorded (submission lock)
bool g_thread_started = false;
std::vector<std::pair<u64, std::function<void()>>> g_deferred;  // (submitted value it waits for, fn), under g_mu

void apply(Post& p) {
    switch (p.kind) {
    case Post::Event: event_apply(p.val, p.mode, p.v); break;
    case Post::Counter: {
        const uint64_t rec[2] = {0, gpu_ticks()};
        gpu_write(p.addr, rec, sizeof rec);
        break;
    }
    case Post::Copy: {
        std::vector<uint8_t> tmp(p.n);
        gpu_read(p.src, tmp.data(), p.n);
        gpu_write(p.addr, tmp.data(), p.n);
        break;
    }
    case Post::Update: gpu_write(p.addr, p.data.data(), p.data.size()); break;
    case Post::Fill: {
        std::vector<uint32_t> tmp((p.n + 3) / 4, p.v);
        gpu_write(p.addr, tmp.data(), p.n);
        break;
    }
    }
}

void completion_loop() {
    Device& c = ctx();
    for (;;) {
        Submission* s;
        {
            std::unique_lock lk{g_mu};
            g_cv.wait(lk, [] { return !g_inflight.empty(); });
            s = g_inflight.front();
        }
        VkSemaphoreWaitInfo wi{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
        const VkSemaphore sem = timeline();
        wi.semaphoreCount = 1;
        wi.pSemaphores = &sem;
        wi.pValues = &s->value;
        const VkResult r = vkWaitSemaphores(c.dev, &wi, UINT64_MAX);
        if (r != VK_SUCCESS) vk_fail(r, "vkWaitSemaphores (completion; device lost?)");
        for (Post& p : s->posts) apply(p);
        s->posts.clear();
        TIMELINE("gpudone fence=%llu", static_cast<unsigned long long>(s->value));
        std::vector<std::function<void()>> run;
        {
            std::scoped_lock lk{g_mu};
            g_inflight.pop_front();
            g_done.store(s->value, std::memory_order_release);
            g_free.push_back(s);
            for (size_t i = 0; i < g_deferred.size();) {
                if (g_deferred[i].first <= s->value) {
                    run.push_back(std::move(g_deferred[i].second));
                    g_deferred[i] = std::move(g_deferred.back());
                    g_deferred.pop_back();
                } else {
                    i++;
                }
            }
        }
        g_cv.notify_all();
        for (auto& fn : run) fn();
    }
}

Submission* take_submission() {
    Device& c = ctx();
    Submission* s = nullptr;
    {
        std::scoped_lock lk{g_mu};
        if (!g_thread_started) {
            g_thread_started = true;
            std::thread(completion_loop).detach();
        }
        if (!g_free.empty()) {
            s = g_free.back();
            g_free.pop_back();
        }
    }
    if (!s) {
        s = new Submission;
        nvk::g_dbg_subs++;
        VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pci.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        pci.queueFamilyIndex = c.family;
        check(vkCreateCommandPool(c.dev, &pci, nullptr, &s->pool), "submission command pool");
        VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ai.commandPool = s->pool;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1;
        check(vkAllocateCommandBuffers(c.dev, &ai, &s->first), "submission command buffer");
    } else {
        vkResetCommandPool(c.dev, s->pool, 0);
    }
    s->cb = s->first;
    s->more_used = 0;
    s->value = 0;
    return s;
}

void full_barrier(VkCommandBuffer cb) {
    VkMemoryBarrier b{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    b.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &b, 0, nullptr,
                         0, nullptr);
}

// ---------------------------------------------------------------- render targets


// A linear target's storage is ordinary memory the CPU can map, and games read the pixels straight from the pool: the kart game renders
// its baked animation images (10 x 184 RGBA16F, flag LINEAR_RENDER_TARGET) and samples bone keys from them on the CPU. The image is the
// renderer's own, so each pass's result is stored back; without it the CPU read whatever the pool held before (garbage bones, flashing meshes).
bool store_linear_target(VkCommandBuffer cb, const Target& t) {
    const Image* img = t.img;
    if (!img || !img->mem || t.level || t.layer || img->samples != VK_SAMPLE_COUNT_1_BIT) return false;
    if (!(img->desc.flags & (NVN_TEXTURE_FLAGS_LINEAR | NVN_TEXTURE_FLAGS_LINEAR_RENDER_TARGET))) return false;
    int bw = 1, bh = 1;
    const int bpb = ::nvn_fmt_bpb(static_cast<int>(img->nvn_format), &bw, &bh);
    if (!bpb || bw != 1 || bh != 1) return false;
    const u64 pitch = static_cast<u64>(img->width) * bpb, stride = img->desc.stride ? img->desc.stride : pitch;
    const u64 bytes = stride * (img->height - 1) + pitch;
    BufferRef br;
    pool_gpu_write(img->mem, bytes);
    if (pool_stats_on()) pool_note(img->mem, bytes, 5);
    if (stride % bpb || !host_readable(img->mem, bytes) || !pool_find(img->mem, bytes, br) || br.offset % 4 || br.offset % bpb) {
        ONCE_RED("linear render target %ux%u format %#x at %#llx cannot be stored back to its pool", img->width, img->height, img->nvn_format,
                 static_cast<unsigned long long>(img->mem));
        return false;
    }
    VkBufferImageCopy bic{};
    bic.bufferOffset = br.offset;
    bic.bufferRowLength = static_cast<u32>(stride / bpb);
    bic.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    bic.imageExtent = {img->width, img->height, 1};
    vkCmdCopyImageToBuffer(cb, img->image, VK_IMAGE_LAYOUT_GENERAL, br.buffer, 1, &bic);
    if (pool_gpu_owned(img->mem, bytes)) {
        full_barrier(cb);
        pool_write_through(cb, img->mem, bytes);
    }
    return true;
}

void end_rendering(VkCommandBuffer cb) {
    if (!R.active) return;
    vkCmdEndRendering(cb);
    R.active = false;
    full_barrier(cb);
    bool stored = false;
    for (int i = 0; i < R.ncolor; i++) stored |= store_linear_target(cb, R.color[i]);
    if (stored) full_barrier(cb);
    if (dump_active()) dump_targets(cb);
}

void set_targets(VkCommandBuffer cb, const p_set_rt* rt) {
    end_rendering(cb);
    R = RenderState{};
    R.ncolor = std::min(rt->n, 8);
    u32 w = UINT32_MAX, h = UINT32_MAX, layers = UINT32_MAX;
    auto bind = [&](Target& t, const tex_t* tex, const view_t* v) {
        t.img = image_of(tex, draw_pre_cb());
        dump_note("  target: tex %p serial %llu mem %p %ux%u fmt %#x -> image %llu", (const void*)tex,
                  static_cast<unsigned long long>(tex->serial), (void*)tex->mem, tex->d.width, tex->d.height, tex->d.format,
                  t.img ? static_cast<unsigned long long>(t.img->serial) : 0ull);
        if (!t.img) return;
        if (t.img->sparse) {
            // (sparse images are made for sampling: textures.cpp makes one only for a texture partly or hugely mapped)
            static bool once = false;
            if (!once) once = true, LOG("[RED] nvk: sparse image %llu (%ux%u NVN format %#x) as a render target: not drawn to",
                                        static_cast<unsigned long long>(t.img->serial), t.img->width, t.img->height, t.img->nvn_format);
            t.img = nullptr;
            return;
        }
        t.view = view_of(t.img, v, true);
        t.format = view_format(t.img, v);
        R.samples = t.img->samples;
        t.level = v && v->set_levels ? std::min<u32>(v->base_level, t.img->levels - 1) : 0;
        t.layer = v && v->set_layers ? v->min_layer : 0;
        t.img->gpu_written = true;
        if (t.img->type == VK_IMAGE_TYPE_3D) {
            static std::unordered_map<u64, int> n3d;
            if (n3d[t.img->serial]++ < 1)
                LOG("nvk: 3D target (frame %llu): image %llu %ux%ux%u fmt %#x view set_layers %d min %u n %u set_levels %d base %u set_target %d target %d",
                    static_cast<unsigned long long>(nvn::g_frame.load()), static_cast<unsigned long long>(t.img->serial),
                    t.img->width, t.img->height, t.img->depth, t.img->nvn_format, v ? v->set_layers : -1, v ? v->min_layer : 0, v ? v->nlayers : 0, v ? v->set_levels : -1, v ? v->base_level : 0,
                    v ? v->set_target : -1, v ? v->target : 0);
        }
        w = std::min(w, std::max(t.img->width >> t.level, 1u));
        h = std::min(h, std::max(t.img->height >> t.level, 1u));
        u32 nl = v && v->set_layers && v->nlayers ? v->nlayers
                 : t.img->type == VK_IMAGE_TYPE_3D ? std::max(t.img->depth >> t.level, 1u) : t.img->layers;
        layers = std::min(layers, nl);
    };
    for (int i = 0; i < R.ncolor; i++)
        if (rt->color[i]) bind(R.color[i], rt->color[i], rt->has_cview[i] ? &rt->cview[i] : nullptr);
    if (rt->depth) bind(R.depth, rt->depth, rt->has_dview ? &rt->dview : nullptr);
    R.extent = {w == UINT32_MAX ? 1 : w, h == UINT32_MAX ? 1 : h};
    R.layers = layers == UINT32_MAX ? 1 : layers;
}

void begin_rendering(VkCommandBuffer cb) {
    if (R.active) return;
    VkRenderingAttachmentInfo col[8]{};
    for (int i = 0; i < R.ncolor; i++) {
        col[i].sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        col[i].imageView = R.color[i].view;
        col[i].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        col[i].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        col[i].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    }
    VkRenderingAttachmentInfo dep{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    dep.imageView = R.depth.view;
    dep.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    dep.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    dep.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea = {{0, 0}, R.extent};
    ri.layerCount = R.layers;
    ri.colorAttachmentCount = static_cast<u32>(R.ncolor);
    ri.pColorAttachments = col;
    const bool has_depth = R.depth.view != VK_NULL_HANDLE;
    ri.pDepthAttachment = has_depth && (R.depth.img->aspect & VK_IMAGE_ASPECT_DEPTH_BIT) ? &dep : nullptr;
    ri.pStencilAttachment = has_depth && (R.depth.img->aspect & VK_IMAGE_ASPECT_STENCIL_BIT) ? &dep : nullptr;
    vkCmdBeginRendering(cb, &ri);
    R.active = true;
}

void clear_color(VkCommandBuffer cb, const p_clear_color* c) {
    if (c->index < 0 || c->index >= R.ncolor || !R.color[c->index].view) return;
    if ((c->mask & 0xf) != 0xf && (c->mask & 0xf)) ONCE_RED("colour clear with a partial channel mask %#x: cleared whole", c->mask);
    if (!(c->mask & 0xf)) return;
    if (!draw_render_enabled()) { dump_note("clear colour %d: rendering off, skipped", c->index); return; }
    begin_rendering(cb);
    VkClearAttachment a{};
    a.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    a.colorAttachment = static_cast<u32>(c->index);
    std::memcpy(a.clearValue.color.float32, c->rgba, 16);  // integer targets: the same bits
    VkClearRect rect{draw_scissor(), 0, R.layers};
    dump_note("clear colour %d (%g %g %g %g) mask %#x rect %d,%d %ux%u of %ux%u", c->index, c->rgba[0], c->rgba[1], c->rgba[2],
              c->rgba[3], c->mask, rect.rect.offset.x, rect.rect.offset.y, rect.rect.extent.width, rect.rect.extent.height,
              R.extent.width, R.extent.height);
    if (!rect.rect.extent.width || !rect.rect.extent.height) return;
    vkCmdClearAttachments(cb, 1, &a, 1, &rect);
}

void clear_ds(VkCommandBuffer cb, const p_clear_ds* c) {
    if (!R.depth.view) return;
    VkImageAspectFlags aspects = 0;
    if (c->depth_mask && (R.depth.img->aspect & VK_IMAGE_ASPECT_DEPTH_BIT)) aspects |= VK_IMAGE_ASPECT_DEPTH_BIT;
    if ((c->stencil_mask & 0xff) && (R.depth.img->aspect & VK_IMAGE_ASPECT_STENCIL_BIT)) aspects |= VK_IMAGE_ASPECT_STENCIL_BIT;
    if (!aspects) return;
    if (!draw_render_enabled()) { dump_note("clear depth: rendering off, skipped"); return; }
    begin_rendering(cb);
    VkClearAttachment a{};
    a.aspectMask = aspects;
    a.clearValue.depthStencil = {c->depth, static_cast<u32>(c->stencil)};
    VkClearRect rect{draw_scissor(), 0, R.layers};
    dump_note("clear depth %g stencil %d aspects %#x rect %d,%d %ux%u", c->depth, c->stencil, aspects, rect.rect.offset.x,
              rect.rect.offset.y, rect.rect.extent.width, rect.rect.extent.height);
    if (!rect.rect.extent.width || !rect.rect.extent.height) return;
    vkCmdClearAttachments(cb, 1, &a, 1, &rect);
}

void clear_tex(VkCommandBuffer cb, const p_clear_tex* c) {
    Image* img = image_of(c->tex, draw_pre_cb());
    if (!img) return;
    end_rendering(cb);
    img->gpu_written = true;
    const view_t* v = c->has_view ? &c->view : nullptr;
    dump_note("clear texture: image %llu %ux%u", static_cast<unsigned long long>(img->serial), img->width, img->height);
    VkImageSubresourceRange range = full_range(img);
    if (v && v->set_levels) range.baseMipLevel = std::min<u32>(v->base_level, img->levels - 1), range.levelCount = 1;
    else range.levelCount = 1;
    if (v && v->set_layers && img->type != VK_IMAGE_TYPE_3D) {
        range.baseArrayLayer = std::min<u32>(v->min_layer, img->layers - 1);
        range.layerCount = v->nlayers ? std::min<u32>(v->nlayers, img->layers - range.baseArrayLayer) : 1;
    }
    if (v && v->set_layers && img->type == VK_IMAGE_TYPE_3D && img->aspect == VK_IMAGE_ASPECT_COLOR_BIT) {
        // Some slices of a 3D image: a clear-on-load rendering scope over them (a whole-image clear would
        // erase the slices already made).
        const u32 level = range.baseMipLevel;
        const u32 d = std::max(img->depth >> level, 1u);
        const u32 nl = v->nlayers ? std::min<u32>(v->nlayers, d - std::min<u32>(v->min_layer, d - 1)) : 1;
        VkRenderingAttachmentInfo at{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        at.imageView = view_of(img, v, true);
        at.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        at.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        at.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        std::memcpy(at.clearValue.color.uint32, c->c.u, 16);
        VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
        ri.renderArea.extent = {std::max(img->width >> level, 1u), std::max(img->height >> level, 1u)};
        ri.layerCount = nl;
        ri.colorAttachmentCount = 1;
        ri.pColorAttachments = &at;
        vkCmdBeginRendering(cb, &ri);
        vkCmdEndRendering(cb);
    } else if (img->aspect & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) {
        VkClearDepthStencilValue ds{c->c.f[0], c->c.u[1]};
        vkCmdClearDepthStencilImage(cb, img->image, VK_IMAGE_LAYOUT_GENERAL, &ds, 1, &range);
    } else {
        VkClearColorValue col;
        std::memcpy(col.uint32, c->c.u, 16);
        vkCmdClearColorImage(cb, img->image, VK_IMAGE_LAYOUT_GENERAL, &col, 1, &range);
    }
    full_barrier(cb);
}

// ---------------------------------------------------------------- buffer operations (GPU when both ends are in imported pools)

void copy_b2b(VkCommandBuffer cb, u64 src, u64 dst, u64 n) {
    watch::gpu_access("copy to", nullptr, 0, dst, n);
    if (!n) return;
    BufferRef s, d;
    pool_gpu_write(dst, n);
    if (pool_stats_on()) pool_note(dst, n, 5);
    const bool gpu = pool_find(src, n, s) && pool_find(dst, n, d);
    if (!gpu) pool_gpu_write(dst, n, true);
    dump_note("copy buffer %#llx -> %#llx +%#llx%s", static_cast<unsigned long long>(src), static_cast<unsigned long long>(dst),
              static_cast<unsigned long long>(n), gpu ? "" : " (on the CPU after the submission)");
    if (gpu) {
        end_rendering(cb);
        VkBufferCopy r{s.offset, d.offset, n};
        vkCmdCopyBuffer(cb, s.buffer, d.buffer, 1, &r);
        full_barrier(cb);
        return;
    }
    static int logged = 0;
    if (logged < 20) logged++, LOG("nvk: buffer copy %#llx -> %#llx +%#llx done on the CPU after its submission (not in imported pools)",
                                   static_cast<unsigned long long>(src), static_cast<unsigned long long>(dst), static_cast<unsigned long long>(n));
    Post p{Post::Copy};
    p.src = src, p.addr = dst, p.n = n;
    g_cur->posts.push_back(std::move(p));
}

void update(VkCommandBuffer cb, const void* data, u64 dst, u64 n) {
    watch::gpu_access("update", nullptr, 0, dst, n);
    if (!n) return;
    BufferRef d;
    pool_gpu_write(dst, n);
    if (pool_stats_on()) pool_note(dst, n, 5);
    const bool gpu = !(dst & 3) && !(n & 3) && pool_find(dst, n, d);
    if (!gpu) pool_gpu_write(dst, n, true);
    dump_note("update buffer %#llx +%#llx%s", static_cast<unsigned long long>(dst), static_cast<unsigned long long>(n),
              gpu ? "" : " (on the CPU after the submission)");
    if (gpu) {
        end_rendering(cb);
        for (u64 off = 0; off < n; off += 65536) {
            const u64 k = std::min<u64>(65536, n - off);
            vkCmdUpdateBuffer(cb, d.buffer, d.offset + off, k, static_cast<const u8*>(data) + off);
        }
        full_barrier(cb);
        return;
    }
    static int logged = 0;
    if (logged < 20) logged++, LOG("nvk: buffer update %#llx +%#llx done on the CPU after its submission", static_cast<unsigned long long>(dst),
                                   static_cast<unsigned long long>(n));
    Post p{Post::Update};
    p.addr = dst;
    p.data.assign(static_cast<const u8*>(data), static_cast<const u8*>(data) + n);
    g_cur->posts.push_back(std::move(p));
}

void fill(VkCommandBuffer cb, u64 dst, u64 n, uint32_t value) {
    watch::gpu_access("fill", nullptr, 0, dst, n);
    if (!n) return;
    BufferRef d;
    pool_gpu_write(dst, n);
    if (pool_stats_on()) pool_note(dst, n, 5);
    const bool gpu = !(dst & 3) && !(n & 3) && pool_find(dst, n, d);
    if (!gpu) pool_gpu_write(dst, n, true);
    dump_note("fill buffer %#llx +%#llx with %#x%s", static_cast<unsigned long long>(dst), static_cast<unsigned long long>(n), value,
              gpu ? "" : " (on the CPU after the submission)");
    if (gpu) {
        end_rendering(cb);
        vkCmdFillBuffer(cb, d.buffer, d.offset, n, value);
        full_barrier(cb);
        return;
    }
    static int logged = 0;
    if (logged < 20) logged++, LOG("nvk: buffer fill %#llx +%#llx done on the CPU after its submission", static_cast<unsigned long long>(dst),
                                   static_cast<unsigned long long>(n));
    Post p{Post::Fill};
    p.addr = dst, p.n = n, p.v = value;
    g_cur->posts.push_back(std::move(p));
}

constexpr int MaxDepth = 64;

// ---------------------------------------------------------------- partial submissions

// A long submission goes to the GPU in parts (every HOST_NVN_FLUSH_RECORDS records, default 3000; 0 = whole): the GPU
// starts on the first part while the rest is recorded, instead of after the whole frame's recording. Only the last part
// signals the fence value. Each part is [its uploads, its commands], like a whole submission.
u32 g_since_flush = 0;

u32 flush_records() {
    static const u32 n = [] {
        const char* e = getenv("HOST_NVN_FLUSH_RECORDS");
        return e ? static_cast<u32>(std::strtoul(e, nullptr, 10)) : 3000u;
    }();
    return n ? n : UINT32_MAX;
}

void flush_partial() {
    g_since_flush = 0;
    if (dump_active() || dump_tracing()) return;
    Device& c = ctx();
    VkCommandBuffer cb = g_cur->cb;
    end_rendering(cb);   // (rendering always loads and stores: the next draw opens it again)
    vkEndCommandBuffer(cb);
    const VkCommandBuffer pre = draw_sub_flush();
    VkSemaphore bind_sem = VK_NULL_HANDLE;
    const u64 bind_value = sparse_commit(&bind_sem);
    {
        HitchTimer ht{HI_SUBMIT};
        // (on the driver thread when it is on: everything the submit needs, by value)
        vkq::run_now([pre, cb, bind_sem, bind_value] {
            Device& c = ctx();
            const VkCommandBuffer cbs[2] = {pre, cb};
            VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
            si.commandBufferCount = pre ? 2 : 1;
            si.pCommandBuffers = pre ? cbs : &cb;
            VkTimelineSemaphoreSubmitInfo ts{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
            const VkPipelineStageFlags bind_stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
            if (bind_value) {
                ts.waitSemaphoreValueCount = 1;
                ts.pWaitSemaphoreValues = &bind_value;
                si.pNext = &ts;
                si.waitSemaphoreCount = 1;
                si.pWaitSemaphores = &bind_sem;
                si.pWaitDstStageMask = &bind_stage;
            }
            std::scoped_lock lk{c.queue_lock};
            check(vkQueueSubmit(c.queue, 1, &si, VK_NULL_HANDLE), "vkQueueSubmit (part)");
            TIMELINE("vkpart");
        });
    }
    if (g_cur->more_used == g_cur->more.size()) {
        VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ai.commandPool = g_cur->pool;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1;
        VkCommandBuffer n;
        check(vkAllocateCommandBuffers(c.dev, &ai, &n), "submission command buffer (part)");
        g_cur->more.push_back(n);
    }
    g_cur->cb = g_cur->more[g_cur->more_used++];
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(g_cur->cb, &bi);
    full_barrier(g_cur->cb);
    pool_part_begin(false);
}

}  // namespace

}  // namespace nvn

namespace nvk {
RenderState R;
std::atomic<int> g_origin_ll{0}, g_queue_origin_ll{0}, g_depth_zero{0};
void begin_rendering(VkCommandBuffer cb) { nvn::begin_rendering(cb); }
void end_rendering(VkCommandBuffer cb) { nvn::end_rendering(cb); }
void full_barrier(VkCommandBuffer cb) { nvn::full_barrier(cb); }
}  // namespace nvk

void nvk::defer(std::function<void()> fn) {
    using namespace nvn;
    std::scoped_lock lk{g_mu};
    // The submission being recorded counts too: it may already reference the object. So does work still waiting for
    // the driver thread (vkq.h; a queued present of the window image, say): the next submission is appended after it.
    const u64 after = g_submitted.load() + (g_cur || !vkq::idle() ? 1 : 0);
    if (g_done.load() >= after) {
        fn();
        return;
    }
    g_deferred.emplace_back(after, std::move(fn));
}

namespace nvn {

// ---------------------------------------------------------------- device state
void vk_rp_set_origin_lower_left(int on) {
    if (g_origin_ll.exchange(on != 0) != (on != 0)) LOG("nvk: window origin %s", on ? "lower left" : "upper left");
}
void vk_rp_set_depth_zero(int on) {
    if (g_depth_zero.exchange(on != 0) != (on != 0)) LOG("nvk: depth mode %s", on ? "near 0 (D3D / Vulkan)" : "near -w (GL)");
}
void vk_rp_set_queue_origin_lower_left(int on) { g_queue_origin_ll.store(on != 0); }

// ---------------------------------------------------------------- memory pools
void vk_rp_pool_flushed(u64 gpu, u64 size) { pool_flushed(gpu, size); }
void vk_rp_pool_invalidated(u64 gpu, u64 size) { pool_invalidated(gpu, size); }
bool vk_rp_pool_gpu_owned(u64 gpu, u64 size) { return pool_gpu_owned(gpu, size); }
void vk_rp_pool_add(u64 gpu, u64 size, u32 flags) {
    init();
    pool_add(gpu, size, flags);
}
void vk_rp_pool_del(u64 gpu) { pool_del(gpu); }
void vk_rp_alias_drop_pool(const pool_t*) {}
void vk_rp_vpool_create(u64, u64) {}
void vk_rp_vpool_map(u64 base, u64 voff, u64 size, u64) { nvk::sparse_note_map(base + voff, size); }
void vk_rp_vpool_destroy(u64) {}

// ---------------------------------------------------------------- descriptors
void vk_rp_set_tex_pool_n(int n) { draw_set_tex_pool_n(n); }
void vk_rp_reg_tex(int id, const tex_t* t, const view_t* v) { draw_reg_tex(id, t, v, false); }
void vk_rp_reg_image(int id, const tex_t* t, const view_t* v) { draw_reg_tex(id, t, v, true); }
void vk_rp_reg_samp(int id, const samp_par* p) { draw_reg_samp(id, p); }

// ---------------------------------------------------------------- textures
void vk_rp_tex_init(const tex_t*) {}
void vk_rp_tex_release(tex_t* t) { image_release(t->serial); }
void vk_rp_tex_cpu_written(const tex_t* t) { draw_tex_cpu_written(t); }
int vk_rp_tex_gpu_owned(uint64_t serial) {
    Image* img = image_find(serial);
    return img && img->gpu_written;
}
void vk_rp_read_texels(const tex_t* t, unsigned, unsigned, const NVNcopyRegion* r, uint8_t* dst, size_t row, size_t img) {
    int bw, bh;
    const int bpb = nvn_fmt_bpb(t->d.format, &bw, &bh);
    if (!bpb || !dst) return;
    ONCE_RED("ReadTexels: GPU texture readback is not implemented yet (zeros)");
    const size_t wb = static_cast<size_t>((r->width + bw - 1) / bw) * bpb, hb = static_cast<size_t>(r->height + bh - 1) / bh;
    const int nz = r->depth ? r->depth : 1;
    if (!row) row = wb;
    if (!img) img = row * hb;
    for (int z = 0; z < nz; z++)
        for (size_t y = 0; y < hb; y++) memset(dst + static_cast<size_t>(z) * img + y * row, 0, wb);
}

// ---------------------------------------------------------------- programs
void vk_rp_prog_release(prog_t* p) { draw_prog_release(p); }
void vk_rp_prog_prefetch(const prog_t*) {}
void vk_rp_prog_forget(const prog_t* p) { draw_prog_forget(p); }

// ---------------------------------------------------------------- queues / submissions
void vk_rp_queue_activate(uint64_t serial) { draw_queue_activate(serial); }
void vk_rp_queue_forget(uint64_t serial) { draw_queue_forget(serial); }
void vk_rp_prepare() {
    static bool once = [] {
        init();
        present_init();
        draw_init();
        LOG("nvn: Vulkan renderer ready");
        return true;
    }();
    (void)once;
}

void vk_rp_sub_begin() {
    g_cur = take_submission();
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(g_cur->cb, &bi);
    full_barrier(g_cur->cb);
    R = RenderState{};
    g_since_flush = 0;
    draw_sub_begin(g_done.load());
    sparse_tick(draw_pre_cb);
    image_evict_cold();
    pool_part_begin(true);
    dump_sub_begin();
}

void vk_rp_exec_rest(const uint64_t*, int) {}

namespace {

// The records of a recording in execution order, following JUMP and CALL; ev (if not null) gets the indices in out of its
// event waits and signals.
void collect(const uint8_t* p, std::vector<const uint8_t*>& out, int depth = 0, std::vector<u32>* ev = nullptr) {
    if (depth >= MaxDepth) {
        LOG("[RED] nvn: command stream calls nest deeper than %d; ignored", MaxDepth);
        return;
    }
    for (;;) {
        op_hdr h;
        memcpy(&h, p, sizeof h);
        if (h.size < 8 || h.op >= OP_COUNT_ || (h.size & 7)) {
            LOG("[RED] nvn: corrupt command record at %p (op %u size %u): the recording is cut here", (const void*)p, h.op, h.size);
            return;
        }
        if (h.op == OP_END) return;
        if (h.op == OP_JUMP) {
            const uint8_t* t;
            memcpy(&t, p + 8, 8);
            if (!t) { LOG("[RED] nvn: JUMP to null"); return; }
            p = t;
            continue;
        }
        if (h.op == OP_CALL) {
            uint64_t t;
            memcpy(&t, p + 8, 8);
            if (t) collect(static_cast<const uint8_t*>(hp(t)), out, depth + 1, ev);
        } else {
            if (ev && (h.op == OP_WAIT_EVENT || h.op == OP_SIGNAL_EVENT)) ev->push_back(static_cast<u32>(out.size()));
            out.push_back(p);
        }
        p += h.size;
    }
}

void exec_record(const uint8_t* p, const op_hdr& h) {
    VkCommandBuffer cb = g_cur->cb;   // (each record: a partial flush changes it)
    const uint8_t* a = p + 8;
    switch (h.op) {
    case OP_SIGNAL_EVENT: {
        volatile uint32_t* val;
        uint32_t m, v;
        memcpy(&val, a, 8), memcpy(&m, a + 8, 4), memcpy(&v, a + 12, 4);
        dump_note("event write %p mode %u value %#x", (const void*)val, m, v);
        if (val) {
            const u32 nv = m == NVN_EVENT_SIGNAL_MODE_ADD ? draw_written_word(val) + v : v;
            draw_note_write(val, &nv, 4);
        }
        rp_sub_event(val, m, v);
        break;
    }
    case OP_WAIT_EVENT: {
        volatile uint32_t* val;
        uint32_t m, v;
        memcpy(&val, a, 8), memcpy(&m, a + 8, 4), memcpy(&v, a + 12, 4);
        dump_note("event wait %p mode %u value %#x now %#x", (const void*)val, m, v, val ? draw_written_word(val) : 0);
        break;
    }
    case OP_REPORT_COUNTER: {
        uint64_t t, ad;
        memcpy(&t, a, 8), memcpy(&ad, a + 8, 8);
        dump_note("counter report type %llu to %#llx", static_cast<unsigned long long>(t), static_cast<unsigned long long>(ad));
        if (u8* h = gpu_ptr(ad, 16)) {
            const uint64_t rec[2] = {0, 0};  // the value the completion writes (the timestamp half is not compared)
            draw_note_write(h, rec, 8);
        }
        rp_sub_counter(ad, static_cast<uint32_t>(t));
        break;
    }
    case OP_FENCE_SYNC: {
        sync_t* s;
        memcpy(&s, a, 8);
        // (on the renderer thread the sync was fenced when the submission was queued: the game may have
        // fenced it again since)
        if (!async_replay()) {
            s->fence = g_submitted.load() + 1;
            s->fenced = 1;
        }
        break;
    }
    case OP_CLEAR_BUF: {
        const auto* c = reinterpret_cast<const p_clear_buf*>(a);
        fill(cb, c->addr, c->size, c->value);
        break;
    }
    case OP_COPY_B2B: {
        uint64_t s_, d, n;
        memcpy(&s_, a, 8), memcpy(&d, a + 8, 8), memcpy(&n, a + 16, 8);
        copy_b2b(cb, s_, d, n);
        break;
    }
    case OP_UPDATE_UB: {
        uint64_t d, n;
        memcpy(&d, a, 8), memcpy(&n, a + 8, 8);
        update(cb, a + 16, d, n);
        break;
    }
    case OP_SET_RT: set_targets(cb, reinterpret_cast<const p_set_rt*>(a)); break;
    case OP_COPY_STRIDE: copy_stride(reinterpret_cast<const p_copy_stride*>(a)); break;
    case OP_COPY_B2T: copy_buffer_texture(cb, reinterpret_cast<const p_copy_b2t*>(a), true); break;
    case OP_COPY_T2B: copy_buffer_texture(cb, reinterpret_cast<const p_copy_b2t*>(a), false); break;
    case OP_COPY_T2T: copy_texture_texture(cb, reinterpret_cast<const p_copy_t2t*>(a)); break;
    case OP_CLEAR_COLOR: clear_color(cb, reinterpret_cast<const p_clear_color*>(a)); break;
    case OP_CLEAR_DS: clear_ds(cb, reinterpret_cast<const p_clear_ds*>(a)); break;
    case OP_CLEAR_TEX: clear_tex(cb, reinterpret_cast<const p_clear_tex*>(a)); break;
    case OP_BARRIER:
        end_rendering(cb);
        full_barrier(cb);
        break;
    case OP_DRAW:
    case OP_DRAW_IDX:
    case OP_DISPATCH:
    case OP_DRAW_TEX: draw(cb, h.op, a, h.size, p + h.size); break;
    default: draw_state(h.op, a, h.size); break;
    }
    if (++g_since_flush >= flush_records()) flush_partial();
}

}  // namespace

void vk_rp_exec(const uint8_t* p) {
    std::vector<const uint8_t*> recs;
    collect(p, recs);
    for (const uint8_t* r : recs) {
        op_hdr h;
        memcpy(&h, r, sizeof h);
        exec_record(r, h);
    }
}

uint64_t vk_rp_sub_commit() {
    VkCommandBuffer cb = g_cur->cb;
    end_rendering(cb);
    vkEndCommandBuffer(cb);
    const u64 value = g_submitted.load() + 1;
    g_cur->value = value;
    if (g_timeline_on) {
        u64 made, binds, gens;
        draw_counts(&binds, &made, &gens);
        TIMELINE("rcommit fence=%llu made=%llu binds=%llu gens=%llu", static_cast<unsigned long long>(value), static_cast<unsigned long long>(made),
                 static_cast<unsigned long long>(binds), static_cast<unsigned long long>(gens));
    }
    const VkCommandBuffer pre = draw_sub_commit(value);
    const VkSemaphore sem = timeline();
    // Sparse images' tile binds of this submission go first
    VkSemaphore bind_sem = VK_NULL_HANDLE;
    const u64 bind_value = sparse_commit(&bind_sem);
    {
        HitchTimer ht{HI_SUBMIT};
        // (on the driver thread when it is on. The completion thread may wait for `value` before it is submitted: a
        // timeline wait for a value not yet submitted just waits.)
        vkq::run_now([pre, cb, sem, value, bind_sem, bind_value] {
            Device& c = ctx();
            const VkCommandBuffer cbs[2] = {pre, cb};
            VkTimelineSemaphoreSubmitInfo ts{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
            ts.signalSemaphoreValueCount = 1;
            ts.pSignalSemaphoreValues = &value;
            VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
            si.pNext = &ts;
            si.commandBufferCount = pre ? 2 : 1;
            si.pCommandBuffers = pre ? cbs : &cb;
            si.signalSemaphoreCount = 1;
            si.pSignalSemaphores = &sem;
            const VkPipelineStageFlags bind_stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
            if (bind_value) {
                ts.waitSemaphoreValueCount = 1;
                ts.pWaitSemaphoreValues = &bind_value;
                si.waitSemaphoreCount = 1;
                si.pWaitSemaphores = &bind_sem;
                si.pWaitDstStageMask = &bind_stage;
            }
            std::scoped_lock lk{c.queue_lock};
            check(vkQueueSubmit(c.queue, 1, &si, VK_NULL_HANDLE), "vkQueueSubmit");
            TIMELINE("vksubmit fence=%llu", static_cast<unsigned long long>(value));
        });
    }
    g_submitted.store(value);
    if (dump_active()) dump_flush(value);
    {
        std::scoped_lock lk{g_mu};
        g_inflight.push_back(g_cur);
    }
    g_cv.notify_all();
    g_cur = nullptr;
    static u64 last_log = 0;
    if (value - last_log >= 1800) {
        last_log = value;
        LOG("nvk: %llu submissions", static_cast<unsigned long long>(value));
        draw_log_stats();
    }
    return value;
}

// ---------------------------------------------------------------- submissions in the order their events allow
// The game's compute and graphics queues take turns through events, five rounds a frame: compute submits first and
// waits for what graphics signals (GPU-side, nvnCommandBufferWaitEvent), graphics waits for what compute signals. One
// Vulkan queue runs both, so a submission that cannot get past a wait is held until the submission that signals it
// has come; then they run together in one Vulkan submission, each up to its next unmet wait. Replayed one after the
// other, compute read what graphics had left from the previous frame and the culling let trees through for a frame.
//
// Held work is never waited for: a game wait on a held fence, HoldMs without a new step (replay_async.cpp) or too many
// held submissions run it anyway, past the waits nothing will signal.
namespace {

constexpr size_t MaxHeld = 4;

struct Stream {
    u64 queue = 0;
    int origin = 0;
    u64 fence = 0;
    std::vector<const uint8_t*> recs;
    std::vector<u32> ev;   // indices of its event waits and signals in recs
    size_t at = 0;
    RenderState r{};  // its render targets while another stream runs
};

// feasible() looks only at the event records (Stream::ev) instead of walking every record: HOST_NVN_FAST=0 off.
bool events_only() {
    static const bool on = [] {
        const char* e = getenv("HOST_NVN_FAST");
        return !(e && *e == '0');
    }();
    return on;
}
std::list<Stream> g_streams;       // renderer thread; oldest first
Stream* g_active = nullptr;
std::atomic<u64> g_hold_first{0};  // the fence of the oldest held submission (0: none)

bool wait_met(u32 now, uint32_t mode, uint32_t v) {
    return mode == NVN_EVENT_WAIT_MODE_EQUAL ? now == v : static_cast<int32_t>(now - v) >= 0;
}

// Takes s from `at` to its end or its next unmet wait; on_rec for each record. True when it moved.
template <class Now, class Rec> bool advance(const Stream& s, size_t& at, Now now, Rec on_rec) {
    const size_t from = at;
    for (; at < s.recs.size(); at++) {
        const uint8_t* p = s.recs[at];
        op_hdr h;
        memcpy(&h, p, sizeof h);
        if (h.op == OP_WAIT_EVENT) {
            volatile uint32_t* val;
            uint32_t m, v;
            memcpy(&val, p + 8, 8), memcpy(&m, p + 16, 4), memcpy(&v, p + 20, 4);
            if (val && !wait_met(now(val), m, v)) break;
        }
        on_rec(p, h);
    }
    return at != from;
}

// A queue runs its submissions in order: only its oldest unfinished one may move.
template <class At> bool blocked_by_older(const Stream& s, At at_of) {
    for (const Stream& o : g_streams) {
        if (&o == &s) return false;
        if (o.queue == s.queue && at_of(o) < o.recs.size()) return true;
    }
    return false;
}

// advance() for feasible(): only the event records, from `at` (on_signal for each signal); at ends at the first unmet wait or
// at the stream's end, as advance() would.
template <class Now, class Sig> bool advance_events(const Stream& s, size_t& at, Now now, Sig on_signal) {
    const size_t from = at;
    for (auto it = std::lower_bound(s.ev.begin(), s.ev.end(), static_cast<u32>(at)); it != s.ev.end(); ++it) {
        const uint8_t* p = s.recs[*it];
        op_hdr h;
        memcpy(&h, p, sizeof h);
        if (h.op == OP_WAIT_EVENT) {
            volatile uint32_t* val;
            uint32_t m, v;
            memcpy(&val, p + 8, 8), memcpy(&m, p + 16, 4), memcpy(&v, p + 20, 4);
            if (val && !wait_met(now(val), m, v)) {
                at = *it;
                return at != from;
            }
        } else {
            on_signal(p, h);
        }
    }
    at = s.recs.size();
    return at != from;
}

// Whether the streams can all reach their ends with the events they signal themselves. Runs nothing.
bool feasible() {
    std::unordered_map<const volatile uint32_t*, u32> vals;
    std::unordered_map<const Stream*, size_t> ats;
    for (const Stream& s : g_streams) ats[&s] = s.at;
    const auto at_of = [&](const Stream& s) { return ats[&s]; };
    const auto now = [&](volatile uint32_t* v) {
        auto it = vals.find(v);
        return it != vals.end() ? it->second : *v;
    };
    for (bool moved = true; moved;) {
        moved = false;
        for (const Stream& s : g_streams) {
            if (blocked_by_older(s, at_of)) continue;
            const auto signal = [&](const uint8_t* p, const op_hdr& h) {
                if (h.op != OP_SIGNAL_EVENT) return;
                volatile uint32_t* val;
                uint32_t m, v;
                memcpy(&val, p + 8, 8), memcpy(&m, p + 16, 4), memcpy(&v, p + 20, 4);
                if (val) vals[val] = m == NVN_EVENT_SIGNAL_MODE_ADD ? now(val) + v : v;
            };
            moved |= events_only() ? advance_events(s, ats[&s], now, signal) : advance(s, ats[&s], now, signal);
        }
    }
    for (const Stream& s : g_streams)
        if (ats[&s] < s.recs.size()) return false;
    return true;
}

void switch_to(Stream& s) {
    if (g_active) {
        end_rendering(g_cur->cb);
        g_active->r = R;
        flush_partial();
    }
    R = s.r;
    g_queue_origin_ll.store(s.origin);
    draw_queue_activate(s.queue);
    draw_cb_changed();
    g_active = &s;
}

void run_ready() {
    const auto at_of = [](const Stream& s) { return s.at; };
    const auto now = [](volatile uint32_t* v) { return draw_written_word(v); };
    for (bool moved = true; moved;) {
        moved = false;
        for (Stream& s : g_streams) {
            if (blocked_by_older(s, at_of)) continue;
            moved |= advance(s, s.at, now, [&](const uint8_t* p, const op_hdr& h) {
                if (g_active != &s) switch_to(s);
                exec_record(p, h);
            });
        }
    }
}

void run_streams() {
    static u64 forced = 0, log_at = 0;
    vk_rp_sub_begin();
    g_active = nullptr;
    for (;;) {
        run_ready();
        // a wait nothing here signals: past it, as if it had been met
        Stream* stuck = nullptr;
        for (Stream& s : g_streams)
            if (s.at < s.recs.size() && !blocked_by_older(s, [](const Stream& o) { return o.at; })) { stuck = &s; break; }
        if (!stuck) break;
        forced++;
        stuck->at++;
    }
    if (const u64 now = host_ns(); forced && now - log_at > 10'000'000'000ull) {
        LOG("nvk: event waits nothing signalled in time, passed anyway: %llu so far", static_cast<unsigned long long>(forced));
        log_at = now;
    }
    const u64 last = g_streams.back().fence;
    g_active = nullptr;
    g_streams.clear();
    g_hold_first.store(0);
    R = RenderState{};
    g_submitted.store(last - 1);   // held submissions' fence values are covered by this one
    vk_rp_sub_commit();
}

}  // namespace

void vk_rp_submit(u64 queue, std::vector<const uint8_t*> handles, u64 fence) {
    Stream& s = g_streams.emplace_back();
    s.queue = queue, s.origin = g_queue_origin_ll.load(), s.fence = fence;
    static size_t typical = 0;
    s.recs.reserve(typical);
    for (const uint8_t* h : handles) collect(h, s.recs, 0, &s.ev);
    typical = std::max(typical, s.recs.size());
    if (!feasible() && g_streams.size() < MaxHeld) {
        if (!g_hold_first.load()) g_hold_first.store(g_streams.front().fence);
        return;
    }
    run_streams();
}

bool vk_rp_holding() { return !g_streams.empty(); }
void vk_rp_force_held() {
    if (!g_streams.empty()) run_streams();
}

void rp_sub_copy(u64 src, u64 dst, u64 n) { copy_b2b(g_cur->cb, src, dst, n); }
void rp_sub_update(const void* data, u64 dst, u64 n) { update(g_cur->cb, data, dst, n); }
void rp_sub_fill(u64 dst, u64 n, uint32_t value) { fill(g_cur->cb, dst, n, value); }
void rp_sub_event(volatile uint32_t* val, uint32_t mode, uint32_t v) {
    Post p{Post::Event};
    p.val = val, p.mode = mode, p.v = v;
    g_cur->posts.push_back(std::move(p));
}
void rp_sub_counter(u64 addr, uint32_t type) {
    pool_gpu_write(addr, 16, true);
    Post p{Post::Counter};
    p.addr = addr, p.type = type;
    g_cur->posts.push_back(std::move(p));
}

// ---------------------------------------------------------------- fences / waiting
uint64_t vk_rp_last_fence() { return g_submitted.load(); }
bool rp_wait(uint64_t fence, uint64_t timeout_ns) {
    if (g_done.load(std::memory_order_acquire) >= fence) return true;
    if (!timeout_ns) return false;
    if (const u64 h = g_hold_first.load(); h && fence >= h) rp_force_held();
    std::unique_lock lk{g_mu};
    auto ready = [&] { return g_done.load(std::memory_order_acquire) >= fence; };
    // The game's waits for the GPU: how many, how long in all (logged every 10 s)
    static u64 n_waits = 0, waited_ns = 0, behind = 0, stats_at = 0;
    const u64 t0 = host_ns();
    behind += g_submitted.load() - g_done.load();
    bool ok = true;
    if (timeout_ns == UINT64_MAX) g_cv.wait(lk, ready);
    else ok = g_cv.wait_for(lk, std::chrono::nanoseconds(timeout_ns), ready);
    const u64 t1 = host_ns();
    n_waits++, waited_ns += t1 - t0;
    TIMELINE("gpuwait %llu us fence=%llu", static_cast<unsigned long long>((t1 - t0) / 1000), static_cast<unsigned long long>(fence));
    if (t1 - stats_at > 10'000'000'000ull) {
        if (stats_at)
            LOG("nvk: game waits for the GPU: %llu (%llu ms in all; %.1f submissions not done at a wait on average)",
                static_cast<unsigned long long>(n_waits), static_cast<unsigned long long>(waited_ns / 1'000'000),
                n_waits ? static_cast<double>(behind) / n_waits : 0.0);
        stats_at = t1, n_waits = waited_ns = behind = 0;
    }
    return ok;
}
bool rp_wait_acq(uint64_t fence, uint64_t timeout_ns) { return rp_wait(fence, timeout_ns); }
void vk_rp_finish() { rp_wait(g_submitted.load(), UINT64_MAX); }
void vk_rp_drain() {
    vkq::sync();
    vk_rp_finish();
    vkDeviceWaitIdle(ctx().dev);
}

// ---------------------------------------------------------------- present
// The whole present (acquire, blit, submit, vkQueuePresentKHR) runs on the driver thread when it is on, after the
// submissions before it. It waits on the GPU for the last submitted value, taken here.
uint64_t vk_rp_present(const tex_t* t, double, const int* crop) {
    static std::atomic<u64> n{0};
    Image* img = t ? image_find(t->serial) : nullptr;
    struct Args {
        VkImage image = VK_NULL_HANDLE;
        VkFormat format = VK_FORMAT_B8G8R8A8_UNORM;
        u32 w = 1, h = 1;
        bool has_crop = false;
        int crop[4]{};
        u64 ready = 0;
    } a;
    if (img) {
        a.image = img->image, a.format = img->format, a.w = img->width, a.h = img->height;
        if (crop) a.has_crop = true, std::memcpy(a.crop, crop, sizeof a.crop);
    }
    a.ready = g_submitted.load();
    vkq::run_now([a] { present(a.image, a.format, a.w, a.h, a.has_crop ? a.crop : nullptr, a.ready); });
    return ++n;
}
void rp_present_wait(uint64_t) {}
void rp_window_tex(const tex_t*) {}

}  // namespace nvn
