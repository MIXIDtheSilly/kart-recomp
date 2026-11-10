#include "draw.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "../nvn_int.h"
#include "common/cityhash.h"
#include "format.h"
#include "nvn_map.h"
#include "pools.h"
#include "replay_async.h"
#include "replay_int.h"
#include "shaders.h"
#include "sparse.h"
#include "textures.h"
#include "upload.h"
#include "dump.h"
#include "hitch.h"
#include "../../watch.h"

namespace nvk {
namespace {

using nvn::prog_t;
using nvn::samp_par;
using nvn::tex_t;
using nvn::view_t;

// ---------------------------------------------------------------- statistics

enum Stat {
    ST_DRAWN, ST_DISPATCHED, ST_NO_PROGRAM, ST_NO_SHADER, ST_UNSUPPORTED, ST_MESH, ST_MULTI_DRAW, ST_TOPOLOGY,
    ST_NO_TARGET, ST_DISABLED, ST_NO_PIPELINE, ST_DRAW_TEX, ST_INDEX, ST_INDIRECT, ST_UPLOADS, ST_PIPELINES, ST_WARMED, ST_N
};
const char* const k_stat_names[ST_N] = {"drawn", "dispatched", "no program", "no shader", "shader unsupported", "mesh drawn",
                                         "indirect-count", "topology", "no target", "render disabled", "no pipeline",
                                         "draw texture", "index buffer", "indirect buffer", "uploads", "pipelines made",
                                         "pipelines from the warm-up"};
u64 g_stats[ST_N];
u64 g_drawn_total = 0;  // never reset (the dump trigger)
// Report / event writes replayed in this submission, by host address of each 32-bit word: they reach memory only when
// the submission completes, but render conditions later in the same stream must see them (agl clears once per frame:
// "clear if the report differs", then a report that makes it equal).
std::unordered_map<uintptr_t, u32> g_written;

// HOST_NVN_SKIP=cs,ind,draw: leave those out (bisecting GPU faults)
bool g_skip_cs = false, g_skip_ind = false, g_skip_draw = false, g_skip_mesh = false;

// ---------------------------------------------------------------- arena: per-submission upload memory and descriptor pools

struct Chunk {
    VkBuffer buf = VK_NULL_HANDLE;
    VmaAllocation alloc = VK_NULL_HANDLE;
    u8* ptr = nullptr;
    VkDeviceSize size = 0, used = 0;
};
struct Arena {
    std::vector<Chunk> chunks;
    size_t cur = 0;
    std::vector<VkDescriptorPool> pools;
    size_t pool_cur = 0;
    VkCommandPool cmd_pool = VK_NULL_HANDLE;
    VkCommandBuffer pre = VK_NULL_HANDLE;   // the uploads' command buffer being recorded
    VkCommandBuffer pre_first = VK_NULL_HANDLE;
    std::vector<VkCommandBuffer> pre_more;  // for the parts after partial flushes
    size_t pre_more_used = 0;
    bool pre_open = false;
    u64 value = 0;
};
constexpr VkDeviceSize ChunkSize = 8ull << 20;
std::deque<Arena*> g_arena_busy;
std::vector<Arena*> g_arena_free;
Arena* A = nullptr;

struct Slice {
    VkBuffer buf;
    VkDeviceSize off;
    u8* ptr;
};

Slice scratch(VkDeviceSize n, VkDeviceSize align = 256) {
    for (;;) {
        if (A->cur < A->chunks.size()) {
            Chunk& c = A->chunks[A->cur];
            const VkDeviceSize at = (c.used + align - 1) & ~(align - 1);
            if (at + n <= c.size) {
                c.used = at + n;
                return {c.buf, at, c.ptr + at};
            }
            A->cur++;
            continue;
        }
        Chunk c;
        c.size = std::max(ChunkSize, n);
        VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bi.size = c.size;
        bi.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                   VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
        VmaAllocationCreateInfo ai{};
        ai.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST;
        ai.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
        VmaAllocationInfo info;
        check(vmaCreateBuffer(ctx().vma, &bi, &ai, &c.buf, &c.alloc, &info), "arena chunk");
        c.ptr = static_cast<u8*>(info.pMappedData);
        A->chunks.push_back(c);
    }
}

VkCommandBuffer pre_cb();

// ---------------------------------------------------------------- constants kept in device memory
// The blocks this renderer makes for a descriptor set (the driver's constants, the support block, a shader's own
// constants, push constants) went to the arena, and every draw read them from system memory: about 6.5 KB a set, some
// 2,000 sets a frame. Most are the same from frame to frame, so each distinct block stays in device memory under a
// hash of what it is made from, copied there once (by the uploads' command buffer of the part that first uses it).
// When the store is full it starts again empty at the next submission. Off by default (it did not lower the measured bus traffic or raise fps); HOST_NVN_KEEP_CONSTANTS=1: on.
struct KeptBlock {
    VkBuffer buf = VK_NULL_HANDLE;
    VmaAllocation alloc = VK_NULL_HANDLE;
    VkDeviceSize used = 0;
};
struct KeptAt {
    u32 block, off;
};
struct KeptCopy {
    VkBuffer src, dst;
    VkBufferCopy r;
};
constexpr VkDeviceSize KeptBlockSize = 16ull << 20;
constexpr size_t KeptBlocks = 4;
std::vector<KeptBlock> g_kept_blocks;
size_t g_kept_cur = 0;
std::unordered_map<u64, KeptAt> g_kept;
std::vector<KeptCopy> g_kept_copies;   // not yet recorded
bool g_kept_on = [] {
    const char* e = getenv("HOST_NVN_KEEP_CONSTANTS");
    return e && e[0] == '1';
}();
bool g_kept_full = false;
u64 g_kept_hits = 0, g_kept_new = 0, g_kept_past = 0, g_kept_wipes = 0;

// Room for n bytes in the store; false when it has none (until the next submission).
bool kept_room(VkDeviceSize n, KeptAt& at) {
    for (;;) {
        if (g_kept_cur < g_kept_blocks.size()) {
            KeptBlock& b = g_kept_blocks[g_kept_cur];
            if (b.used + n <= KeptBlockSize) {
                at = {static_cast<u32>(g_kept_cur), static_cast<u32>(b.used)};
                b.used += n;
                return true;
            }
            g_kept_cur++;
            continue;
        }
        if (g_kept_blocks.size() == KeptBlocks) return false;
        KeptBlock b;
        VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bi.size = KeptBlockSize;
        bi.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        VmaAllocationCreateInfo ai{};
        ai.requiredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
        if (vmaCreateBuffer(ctx().vma, &bi, &ai, &b.buf, &b.alloc, nullptr) != VK_SUCCESS) {
            if (g_kept_blocks.empty()) g_kept_on = false;
            return false;
        }
        g_kept_blocks.push_back(b);
    }
}

// Records the copies of the new blocks (before the uploads' command buffer is closed).
void kept_flush() {
    if (g_kept_copies.empty()) return;
    thread_local std::vector<VkBufferCopy> r;
    const VkCommandBuffer cb = pre_cb();
    for (size_t i = 0; i < g_kept_copies.size();) {
        const KeptCopy& c = g_kept_copies[i];
        r.clear();
        for (; i < g_kept_copies.size() && g_kept_copies[i].src == c.src && g_kept_copies[i].dst == c.dst; i++) r.push_back(g_kept_copies[i].r);
        vkCmdCopyBuffer(cb, c.src, c.dst, static_cast<u32>(r.size()), r.data());
    }
    g_kept_copies.clear();
}

void kept_sub_begin() {
    if (!g_kept_full) return;
    g_kept_full = false;
    g_kept_wipes++;
    g_kept.clear();
    for (KeptBlock& b : g_kept_blocks) b.used = 0;
    g_kept_cur = 0;
}

// A block of `size` bytes that fill(u8*) writes, the same whenever the `from_n` bytes at `from` and `kind` are.
template <class F>
VkDescriptorBufferInfo constants(u32 kind, const void* from, size_t from_n, VkDeviceSize size, F&& fill) {
    u64 key = 0;
    if (g_kept_on) {
        key = Common::CityHash64(static_cast<const char*>(from), from_n) ^ ((size << 8 | kind) * 0x9E3779B97F4A7C15ull);
        if (const auto it = g_kept.find(key); it != g_kept.end()) {
            g_kept_hits++;
            return {g_kept_blocks[it->second.block].buf, it->second.off, size};
        }
    }
    const Slice sl = scratch(size);
    fill(sl.ptr);
    KeptAt at;
    if (!g_kept_on || g_kept_full || !kept_room((size + 255) & ~VkDeviceSize{255}, at)) {
        if (g_kept_on) g_kept_full = true, g_kept_past++;
        return {sl.buf, sl.off, size};
    }
    g_kept_new++;
    g_kept.emplace(key, at);
    g_kept_copies.push_back({sl.buf, g_kept_blocks[at.block].buf, {sl.off, at.off, size}});
    return {g_kept_blocks[at.block].buf, at.off, size};
}

VkDescriptorPool new_pool() {
    const u32 sets = 4096;
    const VkDescriptorPoolSize sizes[] = {
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, sets * 10},  {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, sets * 4},
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, sets * 12}, {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, sets},
        {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1024},         {VK_DESCRIPTOR_TYPE_SAMPLER, 1024},
        {VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, 1024},  {VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER, 1024},
    };
    VkDescriptorPoolCreateInfo ci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    ci.maxSets = sets;
    ci.poolSizeCount = static_cast<u32>(std::size(sizes));
    ci.pPoolSizes = sizes;
    VkDescriptorPool p;
    check(vkCreateDescriptorPool(ctx().dev, &ci, nullptr, &p), "vkCreateDescriptorPool");
    return p;
}

VkDescriptorSet alloc_set(VkDescriptorSetLayout layout) {
    for (;;) {
        if (A->pool_cur == A->pools.size()) A->pools.push_back(new_pool());
        VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        ai.descriptorPool = A->pools[A->pool_cur];
        ai.descriptorSetCount = 1;
        ai.pSetLayouts = &layout;
        VkDescriptorSet s;
        const VkResult r = vkAllocateDescriptorSets(ctx().dev, &ai, &s);
        if (r == VK_SUCCESS) return s;
        if (r != VK_ERROR_OUT_OF_POOL_MEMORY && r != VK_ERROR_FRAGMENTED_POOL) vk_fail(r, "vkAllocateDescriptorSets");
        A->pool_cur++;
    }
}

VkCommandBuffer pre_cb() {
    if (!A->pre_open) {
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(A->pre, &bi);
        A->pre_open = true;
    }
    return A->pre;
}

// ---------------------------------------------------------------- descriptor tables

struct TexEntry {
    tex_t t{};
    view_t v{};
    bool has_v = false, valid = false, image = false;
    u64 gen = 0;   // changes with every registration that changes the entry (stage_key)
};
std::mutex g_tab_mu;
std::vector<TexEntry> g_tex;
std::vector<std::pair<bool, samp_par>> g_samp;

std::unordered_map<std::string, VkSampler> g_samplers;
VkSampler g_default_sampler = VK_NULL_HANDLE;

// The Vulkan sampler of each sampler handle, made from its parameters at its first use (reset when the game registers
// the handle again): looking it up by parameters every binding cost several percent of the submitting thread.
// With the renderer on its own thread everything under g_tab_mu runs on that thread, several times a draw: no lock then.
struct TabLock {
    const bool on = !nvn::async_replay();
    TabLock() { if (on) g_tab_mu.lock(); }
    ~TabLock() { if (on) g_tab_mu.unlock(); }
    TabLock(const TabLock&) = delete;
};
std::vector<VkSampler> g_samp_vk;   // under g_tab_mu
std::vector<u64> g_samp_gen;        // as TexEntry::gen, per sampler handle (under g_tab_mu)
u64 g_reg_gen = 0;                  // the last generation handed out (under g_tab_mu)

VkSampler sampler_for(u32 id) {
    samp_par p;
    {
        TabLock lk;
        if (id >= g_samp.size() || !g_samp[id].first) return g_default_sampler;
        if (id < g_samp_vk.size() && g_samp_vk[id]) return g_samp_vk[id];
        p = g_samp[id].second;
    }
    std::string k(reinterpret_cast<const char*>(&p), sizeof p);
    VkSampler s = VK_NULL_HANDLE;
    if (auto it = g_samplers.find(k); it != g_samplers.end()) {
        s = it->second;
    } else {
        SamplerDesc d = sampler_desc(p);
        if (vkCreateSampler(ctx().dev, &d.ci, nullptr, &s) != VK_SUCCESS) s = g_default_sampler;
        g_samplers.emplace(std::move(k), s);
    }
    std::scoped_lock lk{g_tab_mu};
    // (unless the handle was registered again meanwhile)
    if (id < g_samp.size() && g_samp[id].first && std::memcmp(&g_samp[id].second, &p, sizeof p) == 0) {
        if (id >= g_samp_vk.size()) g_samp_vk.resize(id + 1);
        g_samp_vk[id] = s;
    }
    return s;
}

// ---------------------------------------------------------------- binding state (per queue)

struct BufB {
    u64 addr = 0, size = 0;
};
struct State {
    const Shader* sh[SH_N] = {};
    bool prog_bad = false;  // the bound program's shaders could not all be found
    nvn::blend_st blend[8];
    nvn::color_st color{0, NVN_LOGIC_OP_COPY, NVN_ALPHA_FUNC_ALWAYS};
    nvn::chmask_st chmask{0xffffffffu};
    nvn::ms_st ms{};
    nvn::poly_st poly{NVN_FACE_NONE, NVN_FRONT_FACE_CCW, NVN_POLYGON_MODE_FILL, 0};
    nvn::ds_st ds{0, 1, NVN_DEPTH_FUNC_LESS, 0, NVN_STENCIL_FUNC_ALWAYS, NVN_STENCIL_FUNC_ALWAYS, NVN_STENCIL_OP_KEEP,
                  NVN_STENCIL_OP_KEEP, NVN_STENCIL_OP_KEEP, NVN_STENCIL_OP_KEEP, NVN_STENCIL_OP_KEEP, NVN_STENCIL_OP_KEEP};
    nvn::va_st va[16] = {};
    nvn::vs_st vs[16] = {};
    NVNbufferRange vb[16] = {};
    BufB ubo[SH_N][18];
    BufB ssbo[SH_N][16];
    u32 tex[SH_N][256] = {};
    u32 samp[SH_N][128] = {};
    u32 img[SH_N][8] = {};
    float vp[16][4] = {};
    int nvp = 0;
    nvn::p_scissor sc{0, 0, 0, 0};
    bool has_sc = false;
    float dn = 0, df = 1;
    u32 st_vmask[2] = {0xff, 0xff}, st_wmask[2] = {0xff, 0xff}, st_ref[2] = {0, 0};
    nvn::p_poly_offset po{};
    float blend_color[4] = {};
    bool prim_restart = false;
    u32 restart_index = 0xffffffffu;
    bool depth_clamp = false, rast_discard = false;
    int render_mode = 2;
    u8 push[2][NVN_PUSH_SIZE] = {};
    u8 vp_sw[16][4] = {};
    State() {
        for (int i = 0; i < 8; i++)
            blend[i] = nvn::blend_st{static_cast<u64>(i), NVN_BLEND_FUNC_ONE, NVN_BLEND_FUNC_ZERO, NVN_BLEND_FUNC_ONE,
                                     NVN_BLEND_FUNC_ZERO, NVN_BLEND_EQUATION_ADD, NVN_BLEND_EQUATION_ADD};
    }
};
std::unordered_map<u64, std::unique_ptr<State>> g_states;
State* S = nullptr;

State& state() {
    if (!S) {
        auto& p = g_states[0];
        if (!p) p = std::make_unique<State>();
        S = p.get();
    }
    return *S;
}

}  // namespace

VkRect2D draw_scissor() {
    const State& s = state();
    VkRect2D sc{{0, 0}, R.extent};
    if (s.has_sc) {
        const int x0 = std::max(0, s.sc.x), y0 = std::max(0, s.sc.y);
        const int x1 = std::min<int>(static_cast<int>(R.extent.width), s.sc.x + s.sc.w);
        const int y1 = std::min<int>(static_cast<int>(R.extent.height), s.sc.y + s.sc.h);
        sc = {{x0, y0}, {static_cast<u32>(std::max(0, x1 - x0)), static_cast<u32>(std::max(0, y1 - y0))}};
    }
    return sc;
}

namespace {

// ---------------------------------------------------------------- programs

struct Prog {
    const Shader* sh[SH_N] = {};
    bool bad = false;
};
std::mutex g_prog_mu;                   // programs are released on game threads
std::unordered_map<u64, Prog> g_progs;  // by prog_t::serial

Prog prog_of(const prog_t* p) {
    {
        std::scoped_lock lk{g_prog_mu};
        auto it = g_progs.find(p->serial);
        if (it != g_progs.end()) return it->second;
    }
    Prog r;
    for (u32 i = 0; i < p->nstage && i < 6; i++) {
        u8 key[8];
        if (!nvn::prog_key(p, i, key)) {
            r.bad = true;
            continue;
        }
        const Shader* sh = shader(key);
        if (!sh) {
            r.bad = true;
            continue;
        }
        r.sh[sh->stage] = sh;
    }
    std::scoped_lock lk{g_prog_mu};
    g_progs[p->serial] = r;
    return r;
}

// ---------------------------------------------------------------- pipelines

struct Fnv {
    u64 h = 1469598103934665603ull;
    void add(const void* p, size_t n) {
        const u8* b = static_cast<const u8*>(p);
        for (size_t i = 0; i < n; i++) h = (h ^ b[i]) * 1099511628211ull;
    }
    template <class T> void add(const T& v) { add(&v, sizeof v); }
};

VkDescriptorSetLayout g_empty_layout = VK_NULL_HANDLE;
std::mutex g_layout_mu;   // g_layouts (the warm-up makes layouts too)
std::unordered_map<u64, VkPipelineLayout> g_layouts;
std::unordered_map<u64, VkPipeline> g_pipelines;
VkPipelineCache g_pcache = VK_NULL_HANDLE;
std::string g_pcache_path;
u64 g_pcache_saved_at = 0;

VkPipelineLayout layout_for(const Shader* const* sets, u32 n, bool mesh_push = false) {
    Fnv f;
    f.add(mesh_push);
    for (u32 i = 0; i < n; i++) f.add(sets[i] ? sets[i]->layout : g_empty_layout);
    std::scoped_lock lk{g_layout_mu};
    if (auto it = g_layouts.find(f.h); it != g_layouts.end()) return it->second;
    VkDescriptorSetLayout l[5];
    for (u32 i = 0; i < n; i++) l[i] = sets[i] ? sets[i]->layout : g_empty_layout;
    VkPipelineLayoutCreateInfo ci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    ci.setLayoutCount = n;
    ci.pSetLayouts = l;
    const VkPushConstantRange pc{VK_SHADER_STAGE_TASK_BIT_EXT | VK_SHADER_STAGE_MESH_BIT_EXT, 0, 4};  // first_group (nvn_mesh.py)
    if (mesh_push) ci.pushConstantRangeCount = 1, ci.pPushConstantRanges = &pc;
    VkPipelineLayout pl;
    check(vkCreatePipelineLayout(ctx().dev, &ci, nullptr, &pl), "vkCreatePipelineLayout");
    g_layouts.emplace(f.h, pl);
    return pl;
}

void save_pcache_now();

// On a thread of its own (the cache is ~100 MB: writing it took 5% of the submitting thread); one save at a time.
void save_pcache() {
    if (!g_pcache || g_pcache_path.empty()) return;
    static std::atomic<bool> busy{false};
    if (busy.exchange(true)) return;
    std::thread([] {
        save_pcache_now();
        busy.store(false);
    }).detach();
}

void save_descs();

void save_pcache_now() {
    save_descs();
    size_t n = 0;
    vkGetPipelineCacheData(ctx().dev, g_pcache, &n, nullptr);
    std::vector<char> data(n);
    if (vkGetPipelineCacheData(ctx().dev, g_pcache, &n, data.data()) != VK_SUCCESS) return;
    // Written beside it and renamed over it: the game may be killed while this runs, and a torn cache gives the driver
    // broken shader binaries (device loss at the first draw of a pipeline from it)
    const std::string tmp = g_pcache_path + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        f.write(data.data(), static_cast<std::streamsize>(n));
        if (!f.good()) return;
    }
    std::error_code ec;
    std::filesystem::rename(tmp, g_pcache_path, ec);
}

// A pipeline as made: all it is made from (the rest is dynamic state). Every one made goes into <pipeline cache>.keys,
// and the next start makes them again on threads of its own (warm_up), before the game draws with them.
struct PipeDesc {
    u32 kind;         // 0 vertex, 1 mesh, 2 compute
    u32 ncolor;
    u64 ts, vs, ps;   // shader keys, 0 for none (vs: the mesh or compute shader for those kinds)
    VkFormat colors[8];
    VkFormat df, sf;
    u32 samples, _pad;
};
static_assert(sizeof(PipeDesc) == 80, "PipeDesc has no padding (hashed and stored as bytes)");
constexpr u32 KeysMagic = 0x4B50564E, KeysVersion = 1;   // "NVPK"

u64 desc_key(const PipeDesc& d) { return Common::CityHash64(reinterpret_cast<const char*>(&d), sizeof d); }

std::mutex g_warm_mu;
std::condition_variable g_warm_cv;
std::unordered_map<u64, VkPipeline> g_warm;   // by desc_key: made by the warm-up (null once taken, or made by the renderer)
std::unordered_set<u64> g_warm_busy;          // being made by the warm-up
std::vector<PipeDesc> g_descs;                // every pipeline made, this start or one before: the next start's list
std::unordered_set<u64> g_desc_keys;
u64 g_pipes_made = 0;                         // by the renderer (never reset: when to save the cache)

VkPipeline make_graphics(const PipeDesc& d, const Shader* ts, const Shader* vs, const Shader* ps, VkPipelineLayout layout) {
    const bool mesh = d.kind == 1;
    Device& c = ctx();
    VkPipelineShaderStageCreateInfo st[3]{};
    u32 nst = 0;
    for (const Shader* s : {ts, vs, ps}) {
        if (!s) continue;
        st[nst].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        st[nst].stage = s->vk_stage;
        st[nst].module = s->module;
        st[nst].pName = "main";
        nst++;
    }
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = static_cast<VkSampleCountFlagBits>(d.samples);
    VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    VkPipelineColorBlendAttachmentState att[8]{};
    for (auto& a : att) a.colorWriteMask = 0xf;
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = d.ncolor;
    cb.pAttachments = att;
    std::vector<VkDynamicState> dyn{
        VK_DYNAMIC_STATE_VIEWPORT_WITH_COUNT, VK_DYNAMIC_STATE_SCISSOR_WITH_COUNT, VK_DYNAMIC_STATE_LINE_WIDTH,
        VK_DYNAMIC_STATE_DEPTH_BIAS, VK_DYNAMIC_STATE_BLEND_CONSTANTS, VK_DYNAMIC_STATE_DEPTH_BOUNDS,
        VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK, VK_DYNAMIC_STATE_STENCIL_WRITE_MASK, VK_DYNAMIC_STATE_STENCIL_REFERENCE,
        VK_DYNAMIC_STATE_CULL_MODE, VK_DYNAMIC_STATE_FRONT_FACE,
        VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE, VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE, VK_DYNAMIC_STATE_DEPTH_COMPARE_OP,
        VK_DYNAMIC_STATE_DEPTH_BOUNDS_TEST_ENABLE, VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE, VK_DYNAMIC_STATE_STENCIL_OP,
        VK_DYNAMIC_STATE_RASTERIZER_DISCARD_ENABLE, VK_DYNAMIC_STATE_DEPTH_BIAS_ENABLE, VK_DYNAMIC_STATE_DEPTH_CLAMP_ENABLE_EXT, VK_DYNAMIC_STATE_POLYGON_MODE_EXT,
        VK_DYNAMIC_STATE_SAMPLE_MASK_EXT, VK_DYNAMIC_STATE_ALPHA_TO_COVERAGE_ENABLE_EXT,
        VK_DYNAMIC_STATE_COLOR_BLEND_ENABLE_EXT, VK_DYNAMIC_STATE_COLOR_BLEND_EQUATION_EXT, VK_DYNAMIC_STATE_COLOR_WRITE_MASK_EXT};
    if (c.feat.clip_control) dyn.push_back(VK_DYNAMIC_STATE_DEPTH_CLIP_NEGATIVE_ONE_TO_ONE_EXT);
    if (!mesh) {
        dyn.push_back(VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY);
        dyn.push_back(VK_DYNAMIC_STATE_PRIMITIVE_RESTART_ENABLE);
        dyn.push_back(VK_DYNAMIC_STATE_VERTEX_INPUT_EXT);
    }
    if (c.feat.clip_enable) dyn.push_back(VK_DYNAMIC_STATE_DEPTH_CLIP_ENABLE_EXT);
    VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dy.dynamicStateCount = static_cast<u32>(dyn.size());
    dy.pDynamicStates = dyn.data();
    VkPipelineRenderingCreateInfo pr{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    pr.colorAttachmentCount = d.ncolor;
    pr.pColorAttachmentFormats = d.colors;
    pr.depthAttachmentFormat = d.df;
    pr.stencilAttachmentFormat = d.sf;
    VkGraphicsPipelineCreateInfo ci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    ci.pNext = &pr;
    ci.stageCount = nst;
    ci.pStages = st;
    ci.pVertexInputState = mesh ? nullptr : &vi;
    ci.pInputAssemblyState = mesh ? nullptr : &ia;
    ci.pViewportState = &vp;
    ci.pRasterizationState = &rs;
    ci.pMultisampleState = &ms;
    ci.pDepthStencilState = &ds;
    ci.pColorBlendState = &cb;
    ci.pDynamicState = &dy;
    ci.layout = layout;
    VkPipeline p = VK_NULL_HANDLE;
    const u64 t0 = host_ns();
    HitchTimer ht{HI_PIPELINE};
    if (vkCreateGraphicsPipelines(c.dev, g_pcache, 1, &ci, nullptr, &p) != VK_SUCCESS) {
        LOG("[RED] nvk: graphics pipeline (vs %s, ps %s) could not be created", vs->name, ps ? ps->name : "-");
        p = VK_NULL_HANDLE;
    }
    const u64 ms_taken = (host_ns() - t0) / 1000000;
    if (ms_taken >= 50 && t_hitch) LOG("nvk: pipeline vs %s ps %s took %llu ms", vs->name, ps ? ps->name : "-", static_cast<unsigned long long>(ms_taken));
    return p;
}

VkPipeline make_compute(const Shader* cs, VkPipelineLayout layout) {
    VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    ci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    ci.stage.module = cs->module;
    ci.stage.pName = "main";
    ci.layout = layout;
    VkPipeline p = VK_NULL_HANDLE;
    HitchTimer ht{HI_PIPELINE};
    if (vkCreateComputePipelines(ctx().dev, g_pcache, 1, &ci, nullptr, &p) != VK_SUCCESS) {
        LOG("[RED] nvk: compute pipeline %s could not be created", cs->name);
        p = VK_NULL_HANDLE;
    }
    return p;
}

// The pipeline for key from the warm-up (waiting for it when it is being made), or null: then the caller makes it, and
// the warm-up leaves it alone.
VkPipeline take_warm(u64 key, const PipeDesc& d) {
    std::unique_lock lk{g_warm_mu};
    if (g_desc_keys.insert(key).second) g_descs.push_back(d);
    g_warm_cv.wait(lk, [&] { return !g_warm_busy.count(key); });
    auto [it, fresh] = g_warm.try_emplace(key, VK_NULL_HANDLE);
    return fresh ? VK_NULL_HANDLE : std::exchange(it->second, VK_NULL_HANDLE);
}

// The renderer's way to a pipeline: made once per start (or taken from the warm-up), then from g_pipelines.
VkPipeline pipeline_for(const PipeDesc& d, const Shader* ts, const Shader* vs, const Shader* ps, VkPipelineLayout layout) {
    const u64 key = desc_key(d);
    if (auto it = g_pipelines.find(key); it != g_pipelines.end()) return it->second;
    VkPipeline p = take_warm(key, d);
    if (p) {
        g_stats[ST_WARMED]++;
    } else {
        p = d.kind == 2 ? make_compute(vs, layout) : make_graphics(d, ts, vs, ps, layout);
        g_stats[ST_PIPELINES]++, g_pipes_made++;
    }
    g_pipelines.emplace(key, p);
    return p;
}

VkPipeline graphics_pipeline(const Shader* vs, const Shader* ps, VkPipelineLayout layout, const Shader* ts = nullptr) {
    PipeDesc d{};
    d.kind = vs->stage == SH_MS ? 1 : 0;
    d.ncolor = static_cast<u32>(R.ncolor);
    d.ts = ts ? ts->key : 0, d.vs = vs->key, d.ps = ps ? ps->key : 0;
    for (int i = 0; i < R.ncolor; i++) d.colors[i] = R.color[i].view ? R.color[i].format : VK_FORMAT_UNDEFINED;
    d.df = R.depth.img && (R.depth.img->aspect & VK_IMAGE_ASPECT_DEPTH_BIT) ? R.depth.format : VK_FORMAT_UNDEFINED;
    d.sf = R.depth.img && (R.depth.img->aspect & VK_IMAGE_ASPECT_STENCIL_BIT) ? R.depth.format : VK_FORMAT_UNDEFINED;
    d.samples = static_cast<u32>(R.samples);
    return pipeline_for(d, ts, vs, ps, layout);
}

VkPipeline compute_pipeline(const Shader* cs, VkPipelineLayout layout) {
    PipeDesc d{};
    d.kind = 2;
    d.vs = cs->key;
    return pipeline_for(d, nullptr, cs, nullptr, layout);
}

bool usable(const Shader* s) { return s && s->module && !s->unsupported; }

// One pipeline of the list, made into g_warm (any thread).
void warm_one(const PipeDesc& d) {
    const u64 key = desc_key(d);
    {
        std::scoped_lock lk{g_warm_mu};
        if (g_warm.count(key) || !g_warm_busy.insert(key).second) return;
    }
    VkPipeline p = VK_NULL_HANDLE;
    const Shader* ts = d.ts ? shader_by_key(d.ts) : nullptr;
    const Shader* vs = d.vs ? shader_by_key(d.vs) : nullptr;
    const Shader* ps = d.ps ? shader_by_key(d.ps) : nullptr;
    if (usable(vs) && (!d.ts || usable(ts)) && (!d.ps || usable(ps))) {
        if (d.kind == 2) {
            const Shader* sets[1] = {vs};
            p = make_compute(vs, layout_for(sets, 1));
        } else if (d.kind == 1) {
            const Shader* sets[5] = {nullptr, ps, nullptr, ts, vs};
            p = make_graphics(d, ts, vs, ps, layout_for(sets, 5, true));
        } else {
            const Shader* sets[2] = {vs, ps};
            p = make_graphics(d, nullptr, vs, ps, layout_for(sets, 2));
        }
    }
    std::scoped_lock lk{g_warm_mu};
    g_warm_busy.erase(key);
    if (p && !g_warm.try_emplace(key, p).second) vkDestroyPipeline(ctx().dev, p, nullptr);   // the renderer made it meanwhile
    g_warm_cv.notify_all();
}

// Makes the pipelines of the last starts' list, in the order they were first made, on threads of their own below the
// game's priority (HOST_NVN_WARMUP=0: not).
void warm_up(std::vector<PipeDesc> list) {
    if (const char* e = getenv("HOST_NVN_WARMUP"); (e && std::strcmp(e, "0") == 0) || list.empty()) return;
    std::thread([list = std::move(list)] {
        const u64 t0 = host_ns();
        std::atomic<size_t> next{0};
        const u32 n = std::clamp(std::thread::hardware_concurrency() / 4, 1u, 6u);
        std::vector<std::thread> threads;
        for (u32 i = 0; i < n; i++)
            threads.emplace_back([&] {
                SetThreadDescription(GetCurrentThread(), L"NVN pipeline warm-up");
                SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
                for (size_t k; (k = next++) < list.size();) warm_one(list[k]);
            });
        for (std::thread& t : threads) t.join();
        size_t made = 0;
        {
            std::scoped_lock lk{g_warm_mu};
            for (const auto& [k, p] : g_warm) made += p != VK_NULL_HANDLE;
        }
        LOG("nvk: pipeline warm-up: %zu pipelines in the list, done in %llu ms on %u threads (%zu not used yet)", list.size(),
            static_cast<unsigned long long>((host_ns() - t0) / 1'000'000), n, made);
        // Long only when the driver had to compile them (a new driver, or the layouts changed): keep what it made
        if (host_ns() - t0 > 10'000'000'000ull) save_pcache();
    }).detach();
}

// The list kept beside the cache: the pipelines of this start and the ones before.
std::vector<PipeDesc> load_descs() {
    std::vector<PipeDesc> list;
    std::ifstream f(g_pcache_path + ".keys", std::ios::binary);
    u32 h[4] = {};
    if (!f.read(reinterpret_cast<char*>(h), sizeof h) || h[0] != KeysMagic || h[1] != KeysVersion || h[2] > 200000) return list;
    list.resize(h[2]);
    if (!f.read(reinterpret_cast<char*>(list.data()), static_cast<std::streamsize>(list.size() * sizeof(PipeDesc)))) list.clear();
    std::scoped_lock lk{g_warm_mu};
    for (const PipeDesc& d : list)
        if (g_desc_keys.insert(desc_key(d)).second) g_descs.push_back(d);
    return list;
}

void save_descs() {
    std::vector<PipeDesc> list;
    {
        std::scoped_lock lk{g_warm_mu};
        list = g_descs;
    }
    const std::string file = g_pcache_path + ".keys", tmp = file + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        const u32 h[4] = {KeysMagic, KeysVersion, static_cast<u32>(list.size()), 0};
        f.write(reinterpret_cast<const char*>(h), sizeof h);
        f.write(reinterpret_cast<const char*>(list.data()), static_cast<std::streamsize>(list.size() * sizeof(PipeDesc)));
        if (!f.good()) return;
    }
    std::error_code ec;
    std::filesystem::rename(tmp, file, ec);
}

// ---------------------------------------------------------------- descriptors

constexpr u32 C0Size = 0xA00, C2Size = 0xA00, SupportSize = 1392;
const u32 k_ssbo_base[SH_N] = {0x110, 0x510, 0x410, 0x670, 0x770, 0x310};

VkImageViewType view_type_of(const Binding& b) {
    switch (b.dim) {
    case 0: return b.arrayed ? VK_IMAGE_VIEW_TYPE_1D_ARRAY : VK_IMAGE_VIEW_TYPE_1D;
    case 2: return VK_IMAGE_VIEW_TYPE_3D;
    case 3: return b.arrayed ? VK_IMAGE_VIEW_TYPE_CUBE_ARRAY : VK_IMAGE_VIEW_TYPE_CUBE;
    default: return b.arrayed ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
    }
}

// A buffer range of guest memory for a descriptor; null buffer (reads 0) when no pool holds it.
VkDescriptorBufferInfo guest_buffer(u64 addr, u64 size, bool uniform, bool written = false) {
    VkDescriptorBufferInfo bi{VK_NULL_HANDLE, 0, VK_WHOLE_SIZE};
    if (!addr || !size) return bi;
    if (written) pool_gpu_write(addr, size);
    BufferRef r;
    if (!pool_find(addr, 1, r, size)) return bi;
    if (pool_stats_on()) pool_note(addr, size, uniform ? 2 : written ? 6 : 3);
    const VkPhysicalDeviceLimits& lim = ctx().props.limits;
    const u64 align = uniform ? lim.minUniformBufferOffsetAlignment : lim.minStorageBufferOffsetAlignment;
    u64 range = std::min<u64>(size, r.size);
    if (uniform) range = std::min<u64>(range, lim.maxUniformBufferRange);
    if (r.offset % align) {
        if (uniform) {
            // Unaligned uniform data: a copy of the guest bytes as they are now.
            Slice s = scratch(range, align);
            nvn::gpu_read(addr, s.ptr, range);
            return {s.buf, s.off, range};
        }
        static int once;
        if (!once++) LOG("[RED] nvk: storage buffer at %#llx is not %llu-byte aligned in its pool buffer: bound from the aligned address below",
                         static_cast<unsigned long long>(addr), static_cast<unsigned long long>(align));
        const u64 back = r.offset % align;
        return {r.buffer, r.offset - back, range + back};
    }
    return {r.buffer, r.offset, range};
}

// upload_from_storage with statistics: first uploads and repeats (the CPU wrote the texture again), their time, and the
// slowest one, logged every 10 s.
void upload_timed(Image* img, const tex_t* t) {
    static u64 n[2], ms_x1000[2], worst_us = 0, last = 0;
    static std::string worst;
    const bool again = img->upload_count++ > 0;
    const u64 t0 = host_ns();
    if (upload_from_storage(img, t, pre_cb())) g_stats[ST_UPLOADS]++;
    const u64 us = (host_ns() - t0) / 1000;
    static std::mutex m;
    std::scoped_lock lk{m};
    n[again]++, ms_x1000[again] += us;
    if (us > worst_us) {
        worst_us = us;
        char b[160];
        snprintf(b, sizeof b, "image %llu %ux%u x%u, %u levels, NVN format %#x, upload %u", static_cast<unsigned long long>(img->serial),
                 img->width, img->height, img->layers, img->levels, img->nvn_format, img->upload_count);
        worst = b;
    }
    if (const u64 now = host_ns(); now - last > 10'000'000'000ull) {
        if (last)
            LOG("nvk: uploads from storage: %llu first (%llu ms), %llu repeated (%llu ms); slowest %llu ms: %s", static_cast<unsigned long long>(n[0]),
                static_cast<unsigned long long>(ms_x1000[0] / 1000), static_cast<unsigned long long>(n[1]),
                static_cast<unsigned long long>(ms_x1000[1] / 1000), static_cast<unsigned long long>(worst_us / 1000), worst.c_str());
        last = now, n[0] = n[1] = ms_x1000[0] = ms_x1000[1] = worst_us = 0;
    }
}

bool g_used_placeholder = false;   // texture_view gave a placeholder (bind_stage does not keep the set)

// What a shader samples while its texture's content is still being made on the upload worker: transparent black,
// as a view of type `want` (1x1 images: 1D, 2D with 6 layers for the cube and array types, 3D).
VkImageView placeholder_view(VkImageViewType want) {
    static VkImageView views[VK_IMAGE_VIEW_TYPE_CUBE_ARRAY + 1];
    if (want > VK_IMAGE_VIEW_TYPE_CUBE_ARRAY) want = VK_IMAGE_VIEW_TYPE_2D;
    if (views[want]) return views[want];
    static VkImage images[3];
    const int which = want == VK_IMAGE_VIEW_TYPE_1D || want == VK_IMAGE_VIEW_TYPE_1D_ARRAY ? 0 : want == VK_IMAGE_VIEW_TYPE_3D ? 2 : 1;
    if (!images[which]) {
        VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ci.flags = which == 1 ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0;
        ci.imageType = which == 0 ? VK_IMAGE_TYPE_1D : which == 1 ? VK_IMAGE_TYPE_2D : VK_IMAGE_TYPE_3D;
        ci.format = VK_FORMAT_R8G8B8A8_UNORM;
        ci.extent = {1, 1, 1};
        ci.mipLevels = 1;
        ci.arrayLayers = which == 1 ? 6 : 1;
        ci.samples = VK_SAMPLE_COUNT_1_BIT;
        ci.tiling = VK_IMAGE_TILING_OPTIMAL;
        ci.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        VmaAllocationCreateInfo ai{};
        ai.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
        VmaAllocation alloc;
        check(vmaCreateImage(ctx().vma, &ci, &ai, &images[which], &alloc, nullptr), "vmaCreateImage (placeholder)");
        const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, ci.arrayLayers};
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = images[which];
        b.subresourceRange = range;
        vkCmdPipelineBarrier(pre_cb(), VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
        const VkClearColorValue black{};
        vkCmdClearColorImage(pre_cb(), images[which], VK_IMAGE_LAYOUT_GENERAL, &black, 1, &range);
    }
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = images[which];
    vi.viewType = want;
    vi.format = VK_FORMAT_R8G8B8A8_UNORM;
    const bool six = want == VK_IMAGE_VIEW_TYPE_CUBE || want == VK_IMAGE_VIEW_TYPE_CUBE_ARRAY;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, six ? 6u : 1u};
    check(vkCreateImageView(ctx().dev, &vi, nullptr, &views[want]), "vkCreateImageView (placeholder)");
    return views[want];
}

// img's content from storage when it has none yet (or the CPU wrote the storage again); false while an upload on the
// worker is still being prepared (`wait`: never, the upload finishes here).
bool ensure_content(Image* img, const tex_t* t, bool wait) {
    if (img->sparse) {
        sparse_image_use(img, pre_cb());
        return true;
    }
    if (!img->gpu_written && !img->uploaded) {
        img->uploaded = true;
        upload_async_cancel(img);   // of storage written since
        if (!upload_async_start(img, t)) upload_timed(img, t);
    }
    // While a new upload of storage written again is prepared, the image shows the previous one.
    return upload_async_poll(img, pre_cb(), wait) || img->has_content;
}

// The texture behind handle id: its image (created / uploaded as needed) and a view of type `want`.
// Views by texture handle, for the submission being recorded (cleared at draw_sub_begin: an image the game releases is
// destroyed only after the submissions that may use it, so within one a view stays valid): the draws of a frame bind
// the same handles again and again. Valid while the handle's registration (TexEntry::gen) and g_bind_epoch are.
struct ViewHit {
    u64 gen = 0, epoch = 0;
    VkImageView view = VK_NULL_HANDLE;
};
std::unordered_map<u64, ViewHit> g_view_cache;   // (renderer thread)

VkImageView texture_view(u32 id, VkImageViewType want, bool storage) {
    const u64 epoch = g_bind_epoch.load(std::memory_order_relaxed);
    const u64 ck = static_cast<u64>(id) | static_cast<u64>(want) << 32 | static_cast<u64>(storage) << 40;
    TexEntry e;
    {
        TabLock lk;
        if (id >= g_tex.size() || !g_tex[id].valid) return VK_NULL_HANDLE;
        if (auto it = g_view_cache.find(ck); it != g_view_cache.end() && it->second.gen == g_tex[id].gen && it->second.epoch == epoch)
            return it->second.view;
        e = g_tex[id];
    }
    Image* img = image_of(&e.t, pre_cb());
    if (!img) return VK_NULL_HANDLE;
    if (storage && img->sparse) {
        static bool once = false;   // (sparse images are made for sampling)
        if (!once) once = true, LOG("[RED] nvk: sparse image %llu as a storage image: not bound", static_cast<unsigned long long>(img->serial));
        return VK_NULL_HANDLE;
    }
    if (storage) img->gpu_written = true;
    if (storage && img->type == VK_IMAGE_TYPE_3D) {
        static std::unordered_map<u64, int> n3d;
        if (n3d[img->serial]++ < 1)
            LOG("nvk: 3D storage image: image %llu %ux%ux%u fmt %#x want %d", static_cast<unsigned long long>(img->serial),
                img->width, img->height, img->depth, img->nvn_format, static_cast<int>(want));
    }
    if (!ensure_content(img, &e.t, storage)) {
        g_used_placeholder = true;
        return placeholder_view(want);
    }
    const VkImageView v = view_of(img, e.has_v ? &e.v : nullptr, false, want);
    if (!dump_active() && !dump_tracing()) g_view_cache[ck] = {e.gen, epoch, v};
    return v;
}

// HOST_NVN_CHECKPOINTS=1: a marker before every draw and dispatch (VK_NV_device_diagnostic_checkpoints); at device loss
// vk_fail logs the last markers the GPU reached with their shaders (checkpoint_desc).
struct Mark { u64 a, b, c, sub; u32 kind; };
constexpr size_t MarkN = 1 << 20;
std::vector<Mark> g_marks;
u64 g_mark_n = 0;   // under the submission lock
void checkpoint(VkCommandBuffer cb, u32 kind, const Shader* a, const Shader* b, const Shader* c) {
    if (!ctx().feat.checkpoints) return;
    if (g_marks.empty()) g_marks.resize(MarkN);
    const u64 n = g_mark_n++;
    g_marks[n % MarkN] = {a ? a->key : 0, b ? b->key : 0, c ? c->key : 0, g_drawn_total, kind};
    vkCmdSetCheckpointNV(cb, reinterpret_cast<void*>(static_cast<uintptr_t>(n + 1)));
}

struct DrawArgs {
    u32 dispatch[3] = {};
    int base_vertex = 0, base_instance = 0;
    float vp_w = 1, vp_h = 1;
};

// Descriptor sets by what they are made from (stage_key), for the submission being recorded (its arena's pools and
// scratch hold them): the draws of a pass mostly bind the same buffers and textures, and making a set (resolving
// handles to views, addresses to buffers, vkUpdateDescriptorSets) cost a fifth of the submitting thread.
// g_bind_epoch changes whenever what a handle or address resolves to may have (textures.h). HOST_NVN_NO_SET_CACHE=1: off.
// Open addressing, emptied by stepping g_set_gen at draw_sub_begin: a key at most once (bind_stage looks before it keeps).
struct SetSlot {
    u64 key = 0, gen = 0;
    VkDescriptorSet set = VK_NULL_HANDLE;
};
constexpr size_t SetSlots = 1 << 15;
std::vector<SetSlot> g_set_cache(SetSlots);
u64 g_set_gen = 1;
size_t g_set_kept = 0;
u64 g_set_hits = 0, g_set_made = 0;

VkDescriptorSet set_find(u64 key) {
    for (size_t i = key & (SetSlots - 1);; i = (i + 1) & (SetSlots - 1)) {
        const SetSlot& e = g_set_cache[i];
        if (e.gen != g_set_gen) return VK_NULL_HANDLE;
        if (e.key == key) return e.set;
    }
}

void set_keep(u64 key, VkDescriptorSet set) {
    if (g_set_kept >= SetSlots / 2) return;
    g_set_kept++;
    size_t i = key & (SetSlots - 1);
    while (g_set_cache[i].gen == g_set_gen) i = (i + 1) & (SetSlots - 1);
    g_set_cache[i] = {key, g_set_gen, set};
}

// What stage sh's descriptor set is made from in state s; 0 = not to be cached.
u64 stage_key(const State& s, const Shader* sh, const DrawArgs& da) {
    static const bool off = getenv("HOST_NVN_NO_SET_CACHE") != nullptr;
    if (off || dump_active() || dump_tracing()) return 0;
    const u32 st = sh->stage;
    thread_local std::vector<u8> k(1 << 14);
    size_t used = 0;
    auto put = [&used](const void* p, size_t n) {
        if (used + n > k.size()) k.resize(std::max(k.size() * 2, used + n));
        std::memcpy(k.data() + used, p, n);
        used += n;
    };
    const u64 head[2] = {reinterpret_cast<u64>(sh), g_bind_epoch.load(std::memory_order_relaxed)};
    put(head, sizeof head);
    std::optional<TabLock> tab;   // from the first texture binding on
    for (const Binding& b : sh->bindings) {
        switch (b.kind) {
        case BKind::Support: put(&da.vp_w, 4), put(&da.vp_h, 4); break;
        case BKind::Ubo:
        case BKind::UboPtr:
            if (b.reg < 18) put(&s.ubo[st][b.reg], sizeof(BufB));
            break;
        case BKind::Drv0:
            put(s.ssbo[st], sizeof s.ssbo[st]);
            if (st == SH_CS) put(s.ubo[st], sizeof(BufB) * 16), put(da.dispatch, 12);
            else put(&da.base_vertex, 4), put(&da.base_instance, 4);
            break;
        case BKind::Drv2: put(s.tex[st], sizeof s.tex[st]), put(s.img[st], sizeof s.img[st]), put(s.samp[st], sizeof s.samp[st]); break;
        case BKind::Push: put(s.push[st == SH_CS ? 1 : 0], NVN_PUSH_SIZE); break;
        case BKind::Ssbo:
            if (b.reg < 16) put(&s.ssbo[st][b.reg], sizeof(BufB));
            break;
        case BKind::SsboUbo:
            if (b.reg < 18 && s.ubo[st][b.reg].addr) {
                u64 rec[2] = {};
                nvn::gpu_read(s.ubo[st][b.reg].addr + b.off, rec, 12);
                put(rec, 12);
            }
            break;
        case BKind::Tex:
        case BKind::Img: {
            // the handle, and the registrations of its texture and sampler it resolves to
            if (b.kind == BKind::Tex ? b.reg >= 256 : b.reg >= 8) break;
            const u32 h = b.kind == BKind::Tex ? s.tex[st][b.reg] : s.img[st][b.reg];
            u64 g[3] = {h, 0, 0};
            const u32 tid = h & 0xfffff, sid = h >> 20;
            if (!tab) tab.emplace();
            if (tid < g_tex.size()) g[1] = g_tex[tid].gen;
            if (b.kind == BKind::Tex && sid < g_samp_gen.size()) g[2] = g_samp_gen[sid];
            put(g, sizeof g);
            break;
        }
        default: break;
        }
    }
    const u64 h = Common::CityHash64(reinterpret_cast<const char*>(k.data()), used);
    return h ? h : 1;
}

// Fills and binds the descriptor set of one stage.
u32 fnv(const void* p, size_t n) {
    u32 h = 2166136261u;
    for (size_t i = 0; i < n; i++) h = (h ^ static_cast<const u8*>(p)[i]) * 16777619u;
    return h;
}

// A dump note of a uniform buffer as the CPU sees it now: a hash, and every word for the shaders in HOST_NVN_DUMP_HEX (keys).
// (storage buffers: `kind` 's', the first 16 KiB in hex)
void note_ubo(const Shader* sh, u32 reg, u64 addr, u64 size, char kind = 'c') {
    static const char* hex = getenv("HOST_NVN_DUMP_HEX");
    thread_local std::vector<u8> buf;
    // (HOST_NVN_DUMP_RING_DETAIL: what many draws share is hashed once a frame, or the game crawls)
    thread_local u64 hashed_frame = 0;
    thread_local std::unordered_map<u64, u32> hashed;
    const bool once = dump_brief() && !hex;
    if (once) {
        if (const u64 f = nvn::g_frame.load(std::memory_order_relaxed); f != hashed_frame) hashed_frame = f, hashed.clear();
        if (const auto it = hashed.find(addr * 31 ^ size); it != hashed.end()) {
            dump_note("  %s %s %c%u %llx+%llx fnv %08x", sh->name, kind == 'c' ? "ubo" : "ssbo", kind, reg, static_cast<unsigned long long>(addr),
                      static_cast<unsigned long long>(size), it->second);
            return;
        }
    }
    u64 n = std::min<u64>(size, 65536);
    while (n > 256 && !nvn::host_readable(addr, n)) n /= 2;   // (a storage buffer's size may run past mapped memory)
    if (!addr || !nvn::host_readable(addr, n)) n = 0;
    buf.assign(static_cast<size_t>(n), 0);
    if (n) nvn::gpu_read(addr, buf.data(), n);
    if (once) hashed[addr * 31 ^ size] = fnv(buf.data(), buf.size());
    dump_note("  %s %s %c%u %llx+%llx fnv %08x", sh->name, kind == 'c' ? "ubo" : "ssbo", kind, reg, static_cast<unsigned long long>(addr),
              static_cast<unsigned long long>(size), fnv(buf.data(), buf.size()));
    if (!hex || !std::strstr(hex, sh->name)) return;
    if (kind == 's') dump_recheck(addr, buf.data(), buf.size());
    for (size_t at = 0; at + 32 <= std::min<size_t>(buf.size(), 16384); at += 32) {
        u32 w[8];
        std::memcpy(w, buf.data() + at, 32);
        dump_note("    %c%u+%04zx: %08x %08x %08x %08x %08x %08x %08x %08x", kind, reg, at, w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7]);
    }
}

bool bind_stage(VkCommandBuffer cb, VkPipelineBindPoint bp, VkPipelineLayout layout, const Shader* sh, const DrawArgs& da) {
    State& s = state();
    const u32 st = sh->stage;
    const u64 key = stage_key(s, sh, da);
    if (key && !(dump_active() && !dump_brief()))   // (a dump notes every binding of every draw)
        if (const VkDescriptorSet kept = set_find(key)) {
            g_set_hits++;
            vkCmdBindDescriptorSets(cb, bp, layout, sh->set, 1, &kept, 0, nullptr);
            return true;
        }
    g_set_made++;
    g_used_placeholder = false;
    VkDescriptorSet set = alloc_set(sh->layout);
    size_t nimg = 0, nbuf = 0;
    for (const Binding& b : sh->bindings) (b.type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER || b.type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER ? nbuf : nimg) += b.count;
    // (kept between calls: three heap allocations a stage were 4% of the submitting thread)
    thread_local std::vector<VkDescriptorImageInfo> imgs;
    imgs.clear();
    thread_local std::vector<VkDescriptorBufferInfo> bufs;
    bufs.clear();
    thread_local std::vector<VkWriteDescriptorSet> writes;
    writes.clear();
    imgs.reserve(nimg);
    bufs.reserve(nbuf);
    writes.reserve(sh->bindings.size());
    VkDescriptorBufferInfo c0{}, c2{};
    bool have_c0 = false, have_c2 = false;
    for (const Binding& b : sh->bindings) {
        VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstSet = set;
        w.dstBinding = b.binding;
        w.descriptorCount = b.count;
        w.descriptorType = b.type;
        const bool uniform = b.type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        if (uniform || b.type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER) {
            VkDescriptorBufferInfo bi{VK_NULL_HANDLE, 0, VK_WHOLE_SIZE};
            switch (b.kind) {
            case BKind::Support: {
                const float from[2] = {da.vp_w, da.vp_h};
                bi = constants(1, from, sizeof from, SupportSize, [&](u8* p) {
                    std::memset(p, 0, SupportSize);
                    float* f = reinterpret_cast<float*>(p);
                    // viewport_inverse at 0x90, viewport_size at 0xa0, frag_scale_count at 0xb0, render_scale from 0xc0
                    f[0x90 / 4] = 2.0f / da.vp_w, f[0x94 / 4] = 2.0f / da.vp_h, f[0x98 / 4] = 1.0f;
                    f[0xa0 / 4] = da.vp_w, f[0xa4 / 4] = da.vp_h, f[0xa8 / 4] = 1.0f;
                    for (u32 i = 0; i < 73; i++) f[(0xc0 + 16 * i) / 4] = 1.0f;
                });
                break;
            }
            case BKind::Ubo:
                if (b.reg < 18) bi = guest_buffer(s.ubo[st][b.reg].addr, s.ubo[st][b.reg].size, uniform);
                if (b.reg < 18 && dump_buffers()) note_ubo(sh, b.reg, s.ubo[st][b.reg].addr, s.ubo[st][b.reg].size);
                if (b.reg < 18 && s.ubo[st][b.reg].addr && watch::wanted(sh->name, 100 + b.reg))   // (uniform slot n is watch slot 100 + n)
                    watch::add(s.ubo[st][b.reg].addr, s.ubo[st][b.reg].size), watch::bound(s.ubo[st][b.reg].addr);
                break;
            case BKind::C1:
                if (!sh->c1.empty())
                    bi = constants(2, sh->c1.data(), sh->c1.size(), sh->c1.size(), [&](u8* p) { std::memcpy(p, sh->c1.data(), sh->c1.size()); });
                break;
            case BKind::Drv0:
                if (!have_c0) {
                    for (u32 i = 0; i < 16; i++)
                        if (s.ssbo[st][i].addr && !watch::wanted(sh->name, i)) watch::gpu_access(st == SH_CS ? "cs ssbo" : "gfx ssbo", sh->name, i, s.ssbo[st][i].addr, s.ssbo[st][i].size);
                    for (u32 i = 0; i < 16; i++)
                        if (s.ssbo[st][i].addr && watch::wanted(sh->name, i)) watch::add(s.ssbo[st][i].addr, s.ssbo[st][i].size), watch::bound(s.ssbo[st][i].addr);
                    if (dump_buffers())
                        for (u32 i = 0; i < 16; i++)
                            if (s.ssbo[st][i].addr) note_ubo(sh, i, s.ssbo[st][i].addr, s.ssbo[st][i].size, 's');
                    // (what the block is made from: the stage, its storage buffers, and the dispatch or the draw's bases)
                    u8 from[4 + sizeof s.ssbo[0] + sizeof(BufB) * 16 + 16];
                    size_t fn = 0;
                    const auto add = [&](const void* p, size_t n) { std::memcpy(from + fn, p, n), fn += n; };
                    add(&st, 4), add(s.ssbo[st], sizeof s.ssbo[st]);
                    if (st == SH_CS) add(s.ubo[st], sizeof(BufB) * 16), add(da.dispatch, 12);
                    else add(&da.base_vertex, 4), add(&da.base_instance, 4);
                    c0 = constants(3, from, fn, C0Size, [&](u8* c) {
                        std::memset(c, 0, C0Size);
                        auto put = [&](u32 off, u64 addr, u64 size) {
                            std::memcpy(c + off, &addr, 8);
                            const u32 n = static_cast<u32>(std::min<u64>(size, UINT32_MAX));
                            std::memcpy(c + off + 8, &n, 4);
                        };
                        for (u32 i = 0; i < 16; i++) put(k_ssbo_base[st] + 0x10 * i, s.ssbo[st][i].addr, s.ssbo[st][i].size);
                        if (st == SH_CS) {
                            for (u32 i = 0; i < 16; i++) put(0x210 + 0x10 * i, s.ubo[st][i].addr, s.ubo[st][i].size);
                            std::memcpy(c + 4, da.dispatch, 12);
                            std::memcpy(c + 0x200, da.dispatch, 12);
                        } else {
                            std::memcpy(c + 0x640, &da.base_vertex, 4);
                            std::memcpy(c + 0x644, &da.base_instance, 4);
                            const float point = 64.0f;
                            std::memcpy(c + 0x610, &point, 4);
                        }
                    });
                    have_c0 = true;
                }
                bi = c0;
                break;
            case BKind::Drv2:
                if (!have_c2) {
                    // (made from the stage's texture, image and sampler handles)
                    u32 from[32 + 8 + 128 + 128];
                    std::memcpy(from, s.tex[st], 32 * 4), std::memcpy(from + 32, s.img[st], 8 * 4);
                    std::memcpy(from + 40, s.tex[st] + 128, 128 * 4), std::memcpy(from + 168, s.samp[st], 128 * 4);
                    c2 = constants(4, from, sizeof from, C2Size, [&](u8* c) {
                        std::memset(c, 0, C2Size);
                        for (u32 i = 0; i < 32; i++) std::memcpy(c + 0x20 + 8 * i, &s.tex[st][i], 4);
                        for (u32 i = 0; i < 8; i++) std::memcpy(c + 0x120 + 8 * i, &s.img[st][i], 4);
                        for (u32 i = 0; i < 128; i++) std::memcpy(c + 0x168 + 8 * i, &s.tex[st][128 + i], 4);
                        for (u32 i = 0; i < 128; i++) {
                            const u32 h = s.samp[st][i] << 20;
                            std::memcpy(c + 0x568 + 8 * i, &h, 4);
                        }
                    });
                    have_c2 = true;
                }
                bi = c2;
                break;
            case BKind::Push: {
                const u8* push = s.push[st == SH_CS ? 1 : 0];
                bi = constants(5, push, NVN_PUSH_SIZE, NVN_PUSH_SIZE, [&](u8* p) { std::memcpy(p, push, NVN_PUSH_SIZE); });
                break;
            }
            case BKind::Ssbo:
                if (b.reg < 16) bi = guest_buffer(s.ssbo[st][b.reg].addr, s.ssbo[st][b.reg].size, uniform, b.aux != 0);
                break;
            case BKind::UboPtr:
                if (b.reg < 18) bi = guest_buffer(s.ubo[st][b.reg].addr, s.ubo[st][b.reg].size, uniform);
                break;
            case BKind::SsboUbo:
                if (b.reg < 18 && s.ubo[st][b.reg].addr) {
                    u64 rec[2] = {};
                    nvn::gpu_read(s.ubo[st][b.reg].addr + b.off, rec, 12);
                    bi = guest_buffer(rec[0], static_cast<u32>(rec[1]), uniform, b.aux != 0);
                }
                break;
            default: break;
            }
            for (u32 i = 0; i < b.count; i++) bufs.push_back(bi);
            w.pBufferInfo = &bufs[bufs.size() - b.count];
        } else if (b.type == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER || b.type == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE ||
                   b.type == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE) {
            VkDescriptorImageInfo ii{g_default_sampler, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_GENERAL};
            const VkImageViewType want = view_type_of(b);
            if (b.kind == BKind::Tex && b.reg < 256) {
                const u32 h = s.tex[st][b.reg];
                const u32 tid = h & 0xfffff, sid = h >> 20;
                if (h) {
                    ii.imageView = texture_view(tid, want, false);
                    ii.sampler = sampler_for(sid);
                }
                if (dump_active() || dump_tracing()) {
                    TexEntry e;
                    {
                        std::scoped_lock lk{g_tab_mu};
                        if (tid < g_tex.size()) e = g_tex[tid];
                    }
                    Image* im = e.valid ? image_find(e.t.serial) : nullptr;
                    // the storage's hash for images the CPU fills (as it is now)
                    char fh[24] = "";
                    if (im && !im->gpu_written && !im->sparse && !(e.t.d.flags & NVN_TEXTURE_FLAGS_SPARSE) && e.t.mem && e.t.L.size <= (4u << 20) &&
                        !dump_brief())   // (a sparse pool's storage is mapped only in part)
                        std::snprintf(fh, sizeof fh, " fnv %08x", fnv(e.t.mem, e.t.L.size));
                    if (!dump_brief() || (dump_buffers() && st == SH_CS)) dump_note("  %s slot %u: tex %u (serial %llu) samp %u -> %s %ux%ux%u x%u fmt %#x target %d flags %#x mem %p%s%s%s%s view %p", sh->name, b.reg, tid,
                              static_cast<unsigned long long>(e.t.serial), sid, e.valid ? "image" : "EMPTY", e.t.d.width, e.t.d.height, e.t.d.depth, e.t.L.layers,
                              e.t.d.format, e.t.d.target, e.t.d.flags, (void*)e.t.mem, im && im->gpu_written ? " gpu" : "", im && im->uploaded ? " uploaded" : "",
                              e.valid && !im ? " NO IMAGE" : "", fh, (void*)ii.imageView);
                }
            } else if (b.kind == BKind::Img && b.reg < 8) {
                const u32 h = s.img[st][b.reg];
                if (h) ii.imageView = texture_view(h & 0xfffff, want, true);
            }
            if (b.type == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE) ii.sampler = VK_NULL_HANDLE;
            for (u32 i = 0; i < b.count; i++) imgs.push_back(ii);
            w.pImageInfo = &imgs[imgs.size() - b.count];
        } else if (b.type == VK_DESCRIPTOR_TYPE_SAMPLER) {
            VkDescriptorImageInfo ii{g_default_sampler, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_GENERAL};
            for (u32 i = 0; i < b.count; i++) imgs.push_back(ii);
            w.pImageInfo = &imgs[imgs.size() - b.count];
        } else {
            // texel buffers: none in this game's shaders; a null view reads 0
            static const VkBufferView null_view = VK_NULL_HANDLE;
            std::vector<VkBufferView>* views = new std::vector<VkBufferView>(b.count, null_view);
            w.pTexelBufferView = views->data();
            vkUpdateDescriptorSets(ctx().dev, 1, &w, 0, nullptr);
            delete views;
            continue;
        }
        writes.push_back(w);
    }
    if (!writes.empty()) vkUpdateDescriptorSets(ctx().dev, static_cast<u32>(writes.size()), writes.data(), 0, nullptr);
    vkCmdBindDescriptorSets(cb, bp, layout, sh->set, 1, &set, 0, nullptr);
    if (key && !g_used_placeholder) set_keep(key, set);
    return true;
}

// ---------------------------------------------------------------- dynamic state

// The dynamic state last set in the command buffer being recorded (reset at each submission's start): draws mostly
// repeat it, and each of the ~40 vkCmdSet* calls a draw is a driver call. HOST_NVN_NO_DYN_SHADOW=1 sets everything always.
enum DynSlot {
    D_VP, D_SC, D_LW, D_CULL, D_FF, D_TOPO, D_PMODE, D_DBE, D_DB, D_RD, D_DCLAMP, D_DCLIP, D_DCLIPNEG, D_RESTART, D_DTE, D_DWE,
    D_DCMP, D_DBTE, D_DBOUNDS, D_STE, D_SOPF, D_SOPB, D_SCMF, D_SWMF, D_SREFF, D_SCMB, D_SWMB, D_SREFB, D_RS, D_SMASK, D_A2C,
    D_BLENDC, D_CBE, D_CBEQ, D_CWM, D_VI, D_N
};
struct DynShadow {
    std::vector<u8> v[D_N];
    bool known[D_N] = {};
    bool mesh = false;
} g_dyn;

void dyn_reset() {
    for (bool& k : g_dyn.known) k = false;
}

// True when slot already holds the n bytes at p; otherwise it is recorded as holding them (the caller sets the state).
bool dyn_same(int slot, const void* p, size_t n) {
    static const bool off = getenv("HOST_NVN_NO_DYN_SHADOW") != nullptr;
    if (off) return false;
    std::vector<u8>& v = g_dyn.v[slot];
    if (g_dyn.known[slot] && v.size() == n && std::memcmp(v.data(), p, n) == 0) return true;
    v.assign(static_cast<const u8*>(p), static_cast<const u8*>(p) + n);
    g_dyn.known[slot] = true;
    return false;
}
template <class... T> bool dyn_same_v(int slot, const T&... a) {
    u8 b[64];
    size_t n = 0;
    ((std::memcpy(b + n, &a, sizeof a), n += sizeof a), ...);
    return dyn_same(slot, b, n);
}

void set_dynamic(VkCommandBuffer cb, const State& s, const Shader* ps, VkPrimitiveTopology topo, DrawArgs& da, bool mesh = false) {
    Device& c = ctx();
    // A mesh pipeline has no vertex input or input assembly: binding one leaves those states undefined for the next.
    if (g_dyn.mesh != mesh) g_dyn.known[D_TOPO] = g_dyn.known[D_RESTART] = g_dyn.known[D_VI] = false, g_dyn.mesh = mesh;
    const float tw = static_cast<float>(R.extent.width), th = static_cast<float>(R.extent.height);
    // viewports
    VkViewport vps[16];
    const int nvp = std::max(1, std::min(s.nvp, 16));
    const bool flip = !g_origin_ll.load(std::memory_order_relaxed);
    for (int i = 0; i < nvp; i++) {
        VkViewport v = s.nvp ? VkViewport{s.vp[i][0], s.vp[i][1], s.vp[i][2], s.vp[i][3], s.dn, s.df}
                             : VkViewport{0, 0, tw, th, s.dn, s.df};
        if (v.width <= 0) v.width = 1;
        if (v.height == 0) v.height = 1;
        u32 sw;
        std::memcpy(&sw, s.vp_sw[i], 4);
        const bool neg_y = sw == 0x100;
        if (flip != neg_y) {
            v.y += v.height;
            v.height = -v.height;
        }
        v.minDepth = std::clamp(v.minDepth, 0.0f, 1.0f);
        v.maxDepth = std::clamp(v.maxDepth, 0.0f, 1.0f);
        vps[i] = v;
    }
    da.vp_w = vps[0].width, da.vp_h = std::abs(vps[0].height);
    if (!dyn_same(D_VP, vps, sizeof(VkViewport) * nvp)) vkCmdSetViewportWithCount(cb, static_cast<u32>(nvp), vps);
    const VkRect2D sc = draw_scissor();
    VkRect2D scs[16];
    for (int i = 0; i < nvp; i++) scs[i] = sc;
    if (!dyn_same(D_SC, scs, sizeof(VkRect2D) * nvp)) vkCmdSetScissorWithCount(cb, static_cast<u32>(nvp), scs);
    if (!dyn_same_v(D_LW, 1.0f)) vkCmdSetLineWidth(cb, 1.0f);
    // rasterizer
    if (const VkCullModeFlags cm = cull_mode(s.poly.cull); !dyn_same_v(D_CULL, cm)) vkCmdSetCullMode(cb, cm);
    const bool ccw = (s.poly.front == NVN_FRONT_FACE_CCW) != (g_queue_origin_ll.load(std::memory_order_relaxed) != 0);
    if (!dyn_same_v(D_FF, ccw)) vkCmdSetFrontFace(cb, ccw ? VK_FRONT_FACE_COUNTER_CLOCKWISE : VK_FRONT_FACE_CLOCKWISE);
    if (!mesh && !dyn_same_v(D_TOPO, topo)) vkCmdSetPrimitiveTopology(cb, topo);
    if (const VkPolygonMode pm = polygon_mode(s.poly.mode); !dyn_same_v(D_PMODE, pm)) vkCmdSetPolygonModeEXT(cb, pm);
    const u32 mode_bit = s.poly.mode == NVN_POLYGON_MODE_FILL ? 4 : s.poly.mode == NVN_POLYGON_MODE_LINE ? 2 : 1;
    const bool bias = (s.poly.offset_en & mode_bit) != 0;
    if (!dyn_same_v(D_DBE, bias)) vkCmdSetDepthBiasEnable(cb, bias);
    if (!dyn_same_v(D_DB, s.po.units, s.po.clamp, s.po.factor)) vkCmdSetDepthBias(cb, s.po.units, s.po.clamp, s.po.factor);
    if (!dyn_same_v(D_RD, s.rast_discard)) vkCmdSetRasterizerDiscardEnable(cb, s.rast_discard);
    if (!dyn_same_v(D_DCLAMP, s.depth_clamp)) vkCmdSetDepthClampEnableEXT(cb, s.depth_clamp);
    if (c.feat.clip_enable && !dyn_same_v(D_DCLIP, s.depth_clamp)) vkCmdSetDepthClipEnableEXT(cb, !s.depth_clamp);
    if (const bool neg = !g_depth_zero.load(std::memory_order_relaxed); c.feat.clip_control && !dyn_same_v(D_DCLIPNEG, neg))
        vkCmdSetDepthClipNegativeOneToOneEXT(cb, neg);
    if (!mesh && !dyn_same_v(D_RESTART, s.prim_restart)) vkCmdSetPrimitiveRestartEnable(cb, s.prim_restart);
    // depth / stencil
    const bool has_depth = R.depth.img && (R.depth.img->aspect & VK_IMAGE_ASPECT_DEPTH_BIT);
    const bool has_stencil = R.depth.img && (R.depth.img->aspect & VK_IMAGE_ASPECT_STENCIL_BIT);
    if (const bool dte = has_depth && s.ds.dt; !dyn_same_v(D_DTE, dte)) vkCmdSetDepthTestEnable(cb, dte);
    if (const bool dwe = has_depth && s.ds.dw; !dyn_same_v(D_DWE, dwe)) vkCmdSetDepthWriteEnable(cb, dwe);
    // EQUAL against a depth pre-pass needs the two passes' vertex shaders to give bit-identical depth, which the
    // translated shaders do not guarantee on this GPU (the whole main pass was rejected): LEQUAL draws the same
    // surfaces when the pass does not write depth.
    VkCompareOp dop = compare_op_depth(s.ds.df);
    if (dop == VK_COMPARE_OP_EQUAL && !s.ds.dw) dop = VK_COMPARE_OP_LESS_OR_EQUAL;
    if (!dyn_same_v(D_DCMP, dop)) vkCmdSetDepthCompareOp(cb, dop);
    if (!dyn_same_v(D_DBTE, 0)) vkCmdSetDepthBoundsTestEnable(cb, VK_FALSE);
    if (!dyn_same_v(D_DBOUNDS, 0)) vkCmdSetDepthBounds(cb, 0.0f, 1.0f);
    if (const bool ste = has_stencil && s.ds.st; !dyn_same_v(D_STE, ste)) vkCmdSetStencilTestEnable(cb, ste);
    {
        const VkStencilOp f[3] = {stencil_op(s.ds.fail_f), stencil_op(s.ds.zpass_f), stencil_op(s.ds.zfail_f)};
        const VkCompareOp fc = compare_op_stencil(s.ds.sf_f);
        if (!dyn_same_v(D_SOPF, f, fc)) vkCmdSetStencilOp(cb, VK_STENCIL_FACE_FRONT_BIT, f[0], f[1], f[2], fc);
        const VkStencilOp b[3] = {stencil_op(s.ds.fail_b), stencil_op(s.ds.zpass_b), stencil_op(s.ds.zfail_b)};
        const VkCompareOp bc = compare_op_stencil(s.ds.sf_b);
        if (!dyn_same_v(D_SOPB, b, bc)) vkCmdSetStencilOp(cb, VK_STENCIL_FACE_BACK_BIT, b[0], b[1], b[2], bc);
    }
    for (int f = 0; f < 2; f++) {
        const VkStencilFaceFlags face = f ? VK_STENCIL_FACE_BACK_BIT : VK_STENCIL_FACE_FRONT_BIT;
        if (!dyn_same_v(D_SCMF + 3 * f, s.st_vmask[f])) vkCmdSetStencilCompareMask(cb, face, s.st_vmask[f]);
        if (!dyn_same_v(D_SWMF + 3 * f, s.st_wmask[f])) vkCmdSetStencilWriteMask(cb, face, s.st_wmask[f]);
        if (!dyn_same_v(D_SREFF + 3 * f, s.st_ref[f])) vkCmdSetStencilReference(cb, face, s.st_ref[f]);
    }
    // multisample
    if (!dyn_same_v(D_RS, R.samples)) vkCmdSetRasterizationSamplesEXT(cb, R.samples);
    const VkSampleMask mask[2] = {~0u, ~0u};
    if (!dyn_same_v(D_SMASK, R.samples)) vkCmdSetSampleMaskEXT(cb, R.samples, mask);
    if (const bool a2c = s.ms.a2c && R.samples != VK_SAMPLE_COUNT_1_BIT; !dyn_same_v(D_A2C, a2c)) vkCmdSetAlphaToCoverageEnableEXT(cb, a2c);
    // blend
    if (!dyn_same(D_BLENDC, s.blend_color, sizeof s.blend_color)) vkCmdSetBlendConstants(cb, s.blend_color);
    if (R.ncolor) {
        VkBool32 en[8];
        VkColorBlendEquationEXT eq[8];
        VkColorComponentFlags wm[8];
        for (int i = 0; i < R.ncolor; i++) {
            bool adv = false, dual = false;
            blend_equation(s.blend[i], &eq[i], &adv, &dual);
            en[i] = (s.color.blend_en >> i & 1) && !adv;
            wm[i] = (s.chmask.mask >> (4 * i)) & 0xf;
            if (ps && !(ps->outputs >> i & 1)) wm[i] = 0;
            if (!ps) wm[i] = 0;
        }
        if (!dyn_same(D_CBE, en, sizeof en[0] * R.ncolor)) vkCmdSetColorBlendEnableEXT(cb, 0, static_cast<u32>(R.ncolor), en);
        if (!dyn_same(D_CBEQ, eq, sizeof eq[0] * R.ncolor)) vkCmdSetColorBlendEquationEXT(cb, 0, static_cast<u32>(R.ncolor), eq);
        if (!dyn_same(D_CWM, wm, sizeof wm[0] * R.ncolor)) vkCmdSetColorWriteMaskEXT(cb, 0, static_cast<u32>(R.ncolor), wm);
    }
    if (s.color.logic_op != NVN_LOGIC_OP_COPY) ONCE_RED("logic op %u: drawn without it", static_cast<unsigned>(s.color.logic_op));
    if (s.color.alpha_func != NVN_ALPHA_FUNC_ALWAYS) ONCE_RED("alpha test %u: drawn without it", static_cast<unsigned>(s.color.alpha_func));
}

void set_vertex_input(VkCommandBuffer cb, const State& s, const Shader* vs) {
    VkVertexInputBindingDescription2EXT binds[17];
    VkVertexInputAttributeDescription2EXT attrs[32];
    u32 nb = 0, na = 0, streams = 0;
    bool zero = false;
    for (u32 loc = 0; loc < 32; loc++) {
        if (!(vs->inputs >> loc & 1)) continue;
        VkVertexInputAttributeDescription2EXT a{VK_STRUCTURE_TYPE_VERTEX_INPUT_ATTRIBUTE_DESCRIPTION_2_EXT};
        a.location = loc;
        VkFormat f = VK_FORMAT_UNDEFINED;
        if (loc < 16 && s.va[loc].fmt && s.va[loc].stream < 16) f = vertex_format(s.va[loc].fmt);
        if (f != VK_FORMAT_UNDEFINED) {
            a.binding = s.va[loc].stream;
            a.format = f;
            a.offset = s.va[loc].offset;
            streams |= 1u << a.binding;
        } else {
            a.binding = 16;
            a.format = vs->input_type[loc] == 1 ? VK_FORMAT_R32G32B32A32_SINT
                       : vs->input_type[loc] == 2 ? VK_FORMAT_R32G32B32A32_UINT : VK_FORMAT_R32G32B32A32_SFLOAT;
            a.offset = 0;
            zero = true;
        }
        attrs[na++] = a;
    }
    VkBuffer bufs[17];
    VkDeviceSize offs[17], sizes[17];
    for (u32 i = 0; i < 16; i++) {
        if (!(streams >> i & 1)) continue;
        VkVertexInputBindingDescription2EXT b{VK_STRUCTURE_TYPE_VERTEX_INPUT_BINDING_DESCRIPTION_2_EXT};
        b.binding = i;
        b.stride = s.vs[i].stride;
        b.inputRate = s.vs[i].divisor ? VK_VERTEX_INPUT_RATE_INSTANCE : VK_VERTEX_INPUT_RATE_VERTEX;
        b.divisor = 1;
        if (s.vs[i].divisor > 1) ONCE_RED("vertex stream divisor %u: drawn with divisor 1", s.vs[i].divisor);
        binds[nb++] = b;
        BufferRef r;
        if (s.vb[i].address && pool_find(s.vb[i].address, 1, r, s.vb[i].size)) {
            bufs[i] = r.buffer, offs[i] = r.offset, sizes[i] = std::min<u64>(r.size, s.vb[i].size ? s.vb[i].size : r.size);
        } else {
            bufs[i] = VK_NULL_HANDLE, offs[i] = 0, sizes[i] = VK_WHOLE_SIZE;
        }
        vkCmdBindVertexBuffers2(cb, i, 1, &bufs[i], &offs[i], &sizes[i], nullptr);
    }
    if (zero) {
        VkVertexInputBindingDescription2EXT b{VK_STRUCTURE_TYPE_VERTEX_INPUT_BINDING_DESCRIPTION_2_EXT};
        b.binding = 16;
        b.stride = 0;
        b.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
        b.divisor = 1;
        binds[nb++] = b;
        const VkBuffer nb_ = VK_NULL_HANDLE;
        const VkDeviceSize zo = 0, zs = VK_WHOLE_SIZE;
        vkCmdBindVertexBuffers2(cb, 16, 1, &nb_, &zo, &zs, nullptr);
    }
    // (write watch: the attributes of a watched shader's vertex buffers, once per buffer: format, shader input type, byte ranges)
    if (watch::wanted(vs->name, 2)) {
        static std::mutex mu;
        static std::unordered_set<u64> seen;
        std::scoped_lock lk{mu};
        for (u32 i = 0; i < na; i++) {
            const u32 bi = attrs[i].binding;
            if (bi >= 16 || !s.vb[bi].address || !seen.insert(s.vb[bi].address * 33 + attrs[i].location).second) continue;
            const u64 size = std::min<u64>(s.vb[bi].size, 1u << 20), stride = s.vs[bi].stride;
            if (!stride || !nvn::host_readable(s.vb[bi].address, size)) continue;
            std::vector<u8> v(size);
            nvn::gpu_read(s.vb[bi].address, v.data(), size);
            u32 lo[4] = {255, 255, 255, 255}, hi[4] = {}, n = 0;
            for (u64 at = attrs[i].offset; at + 4 <= size; at += stride, n++)
                for (int c = 0; c < 4; c++) lo[c] = std::min<u32>(lo[c], v[at + c]), hi[c] = std::max<u32>(hi[c], v[at + c]);
            LOG("watch: %s location %u: format %d (input type %u) stream %u %#llx+%#llx stride %llu offset %u, %u vertices, bytes %u-%u %u-%u %u-%u %u-%u",
                vs->name, attrs[i].location, attrs[i].format, vs->input_type[attrs[i].location], bi,
                static_cast<unsigned long long>(s.vb[bi].address), static_cast<unsigned long long>(size), static_cast<unsigned long long>(stride),
                attrs[i].offset, n, lo[0], hi[0], lo[1], hi[1], lo[2], hi[2], lo[3], hi[3]);
        }
    }
    for (u32 i = 0; i < na && !dump_brief(); i++)
        dump_note("  vertex attribute %u: binding %u format %d offset %u", attrs[i].location, attrs[i].binding, attrs[i].format, attrs[i].offset);
    for (u32 i = 0; i < nb && !dump_brief(); i++)
        dump_note("  vertex binding %u: stride %u rate %d buffer %#llx size %#llx", binds[i].binding, binds[i].stride, binds[i].inputRate,
                  binds[i].binding < 16 ? static_cast<unsigned long long>(s.vb[binds[i].binding].address) : 0ull,
                  binds[i].binding < 16 ? static_cast<unsigned long long>(s.vb[binds[i].binding].size) : 0ull);
    {
        thread_local std::vector<u8> k;
        k.assign(reinterpret_cast<const u8*>(binds), reinterpret_cast<const u8*>(binds + nb));
        k.insert(k.end(), reinterpret_cast<const u8*>(attrs), reinterpret_cast<const u8*>(attrs + na));
        if (!dyn_same(D_VI, k.data(), k.size())) vkCmdSetVertexInputEXT(cb, nb, binds, na, attrs);
    }
}

// ---------------------------------------------------------------- draws

bool render_enabled(const State& s) { return s.render_mode != 3; }

}  // namespace

bool draw_render_enabled() { return render_enabled(state()); }
u64 draw_total() { return g_drawn_total; }

namespace {

// A GPU hang surfaces about 5 s late (Windows' timeout), long after the draw that caused it was recorded. What a device loss needs is the
// first use of each pipeline, the likely culprits: these draws are traced, and vk_fail logs the ring.
void trace_first_use(VkPipeline pipe, const Shader* a, const Shader* b) {
    static VkPipeline last = VK_NULL_HANDLE;
    if (pipe == last) return;
    last = pipe;
    static std::unordered_set<VkPipeline> seen;
    if (!seen.insert(pipe).second) return;
    dump_trace(true);
    dump_note("first use of pipeline at frame %llu: %s %s, %d colour targets", static_cast<unsigned long long>(nvn::g_frame.load()), a ? a->name : "-", b ? b->name : "-", R.ncolor);
    for (int i = 0; i < R.ncolor; i++)
        if (R.color[i].img)
            dump_note("  colour %d: image %llu %ux%u NVN format %#x layer %u", i, static_cast<unsigned long long>(R.color[i].img->serial), R.color[i].img->width,
                      R.color[i].img->height, R.color[i].img->nvn_format, R.color[i].layer);
    if (R.depth.img) dump_note("  depth: image %llu %ux%u NVN format %#x", static_cast<unsigned long long>(R.depth.img->serial), R.depth.img->width, R.depth.img->height, R.depth.img->nvn_format);
}

// NVN2 mesh draw: count task (or, without a task stage, mesh) workgroups from `first`.
void do_mesh(VkCommandBuffer cb, const nvn::p_draw* d) {
    State& s = state();
    const Shader* ts = s.sh[SH_TS];
    const Shader* ms = s.sh[SH_MS];
    const Shader* ps = s.sh[SH_PS];
    if (s.prog_bad) { g_stats[ST_NO_SHADER]++; return; }
    if (!ms) { g_stats[ST_NO_PROGRAM]++; return; }
    if (g_skip_mesh) { g_stats[ST_DISABLED]++; return; }
    if (!ms->module || ms->unsupported || (ts && (!ts->module || ts->unsupported)) || (ps && (!ps->module || ps->unsupported))) {
        g_stats[ST_UNSUPPORTED]++;
        return;
    }
    if (!R.ncolor && !R.depth.view) { g_stats[ST_NO_TARGET]++; return; }
    if (d->ind) { g_stats[ST_INDIRECT]++; return; }
    const Shader* sets[5] = {nullptr, ps, nullptr, ts, ms};
    VkPipelineLayout layout = layout_for(sets, 5, true);
    VkPipeline pipe = graphics_pipeline(ms, ps, layout, ts);
    if (!pipe) { g_stats[ST_NO_PIPELINE]++; return; }
    trace_first_use(pipe, ms, ps);
    if (g_skip_draw) { g_stats[ST_DISABLED]++; return; }
    DrawArgs da;
    begin_rendering(cb);
    set_dynamic(cb, s, ps, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, da, true);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    checkpoint(cb, 1, ts, ms, ps);
    if (ts) bind_stage(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, ts, da);
    bind_stage(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, ms, da);
    if (ps) bind_stage(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, ps, da);
    const u32 first = static_cast<u32>(d->first);
    vkCmdPushConstants(cb, layout, VK_SHADER_STAGE_TASK_BIT_EXT | VK_SHADER_STAGE_MESH_BIT_EXT, 0, 4, &first);
    vkCmdDrawMeshTasksEXT(cb, static_cast<u32>(d->count), 1, 1);
    g_stats[ST_MESH]++;
    dump_note("mesh ts %s ms %s ps %s groups %d+%d", ts ? ts->name : "-", ms->name, ps ? ps->name : "-", d->first, d->count);
}

void do_draw(VkCommandBuffer cb, u32 op, const u8* a, u32 size, const u8* next) {
    // HOST_NVN_SKIP_DRAWS=1 (measuring only): no draw is recorded, to see how fast the game runs without their cost
    static const bool skip = getenv("HOST_NVN_SKIP_DRAWS") != nullptr;
    if (skip) return;
    State& s = state();
    {
        // HOST_NVN_TRACE_PS=<pixel shader key>: the draws with that pixel shader are logged as in a dump (first 20)
        static const u64 trace = getenv("HOST_NVN_TRACE_PS") ? std::strtoull(getenv("HOST_NVN_TRACE_PS"), nullptr, 16) : 0;
        const bool on = trace && s.sh[SH_PS] && s.sh[SH_PS]->key == trace;
        dump_trace(on);
        if (on) {
            dump_note("draw %llu with ps %016llx: %d colour targets", static_cast<unsigned long long>(g_drawn_total),
                      static_cast<unsigned long long>(trace), R.ncolor);
            for (int i = 0; i < R.ncolor; i++)
                if (R.color[i].img)
                    dump_note("  colour %d: image %llu %ux%u NVN format %#x view format %d layer %u", i, static_cast<unsigned long long>(R.color[i].img->serial),
                              R.color[i].img->width, R.color[i].img->height, R.color[i].img->nvn_format, R.color[i].format, R.color[i].layer);
            if (R.depth.img)
                dump_note("  depth: image %llu %ux%u NVN format %#x", static_cast<unsigned long long>(R.depth.img->serial), R.depth.img->width,
                          R.depth.img->height, R.depth.img->nvn_format);
        }
    }
    if (!render_enabled(s)) { g_stats[ST_DISABLED]++; return; }
    nvn::p_draw dtmp;
    nvn::p_draw_idx itmp;
    const nvn::p_draw* d = op == nvn::OP_DRAW ? nvn::draw_rec(a, size, &dtmp) : nullptr;
    const nvn::p_draw_idx* di = op == nvn::OP_DRAW_IDX ? nvn::draw_idx_rec(a, size, &itmp) : nullptr;
    const int prim = d ? d->prim : di->prim;
    if (prim == NVN_PRIM_MESH) {
        if (d) do_mesh(cb, d);
        return;
    }
    // An indirect-count multi-draw was recorded as max draws, draw i drawn while i < *cnt (cmdbuf.cpp): the first issues one
    // Vulkan indirect-count draw over the records that follow it with the same count, the others are covered by it.
    const u64 mdi_cnt = d ? d->mdi_cnt : di->mdi_cnt;
    u32 mdi_max = 0, mdi_stride = di ? 20 : 16;
    BufferRef cnt{};
    if (mdi_cnt) {
        if ((d ? d->mdi_i : di->mdi_i) != 0) return;
        const u64 ind0 = d ? d->ind : di->ind;
        mdi_max = 1;
        for (const u8* q = next; q;) {
            nvn::op_hdr h;
            std::memcpy(&h, q, sizeof h);
            if (h.op != op || h.size < 8) break;
            nvn::p_draw t1;
            nvn::p_draw_idx t2;
            const u64 c = op == nvn::OP_DRAW ? nvn::draw_rec(q + 8, h.size, &t1)->mdi_cnt : nvn::draw_idx_rec(q + 8, h.size, &t2)->mdi_cnt;
            const int i = op == nvn::OP_DRAW ? nvn::draw_rec(q + 8, h.size, &t1)->mdi_i : nvn::draw_idx_rec(q + 8, h.size, &t2)->mdi_i;
            const u64 ind = op == nvn::OP_DRAW ? nvn::draw_rec(q + 8, h.size, &t1)->ind : nvn::draw_idx_rec(q + 8, h.size, &t2)->ind;
            if (c != mdi_cnt || i != static_cast<int>(mdi_max)) break;
            if (mdi_max == 1) mdi_stride = static_cast<u32>(ind - ind0);
            mdi_max++;
            q += h.size;
        }
        if (!pool_find(mdi_cnt, 4, cnt)) { g_stats[ST_INDIRECT]++; return; }
        g_stats[ST_MULTI_DRAW]++;
    }
    if (s.prog_bad) { g_stats[ST_NO_SHADER]++; return; }
    const Shader* vs = s.sh[SH_VS];
    const Shader* ps = s.sh[SH_PS];
    if (!vs) { g_stats[ST_NO_PROGRAM]++; return; }
    if (!vs->module || vs->unsupported || (ps && (!ps->module || ps->unsupported)) || s.sh[SH_GS]) {
        g_stats[ST_UNSUPPORTED]++;
        dump_note("draw skipped: vs %s ps %s unsupported", vs->name, ps ? ps->name : "-");
        return;
    }
    if (!R.ncolor && !R.depth.view) { g_stats[ST_NO_TARGET]++; return; }
    const Topology t = topology(static_cast<u32>(prim));
    const bool quads = t.quads && !(d ? d->ind : di->ind) && !(d ? d->mdi_cnt : di->mdi_cnt);
    if (!t.supported || (t.quads && !quads) || t.quad_strip || t.line_loop || t.patch_vertices || t.topo == VK_PRIMITIVE_TOPOLOGY_PATCH_LIST) {
        g_stats[ST_TOPOLOGY]++;
        return;
    }
    const Shader* sets[2] = {vs, ps};
    VkPipelineLayout layout = layout_for(sets, 2);
    VkPipeline pipe = graphics_pipeline(vs, ps, layout);
    if (!pipe) { g_stats[ST_NO_PIPELINE]++; return; }
    trace_first_use(pipe, vs, ps);
    // index / indirect buffers first: a missing one skips the draw before anything is recorded
    BufferRef ib{}, ind{};
    VkIndexType itype = VK_INDEX_TYPE_UINT16;
    if (di) {
        bool u8 = false;
        itype = index_type(static_cast<u32>(di->itype), &u8);
        if ((u8 && !ctx().feat.index_uint8) || !pool_find(di->ibuf, 1, ib)) { g_stats[ST_INDEX]++; return; }
    }
    const u64 ind_addr = d ? d->ind : di->ind;
    if (ind_addr && (g_skip_ind || !pool_find(ind_addr, di ? 20 : 16, ind))) { g_stats[ST_INDIRECT]++; return; }
    if (g_skip_draw) { g_stats[ST_DISABLED]++; return; }
    if (pool_stats_on()) {
        // (what the GPU fetches at most: the bound size stands in where the GPU decides the count)
        const u64 n = di ? static_cast<u64>(di->count) : static_cast<u64>(d->count);
        const u64 inst = std::max<u64>(1, di ? di->instances : d->instances);
        for (int i = 0; i < 16; i++) {
            if (!s.vb[i].address || !s.vs[i].stride) continue;
            const u64 want = ind_addr ? s.vb[i].size : (s.vs[i].divisor ? inst : n) * s.vs[i].stride;
            pool_note(s.vb[i].address, s.vb[i].size ? std::min<u64>(want, s.vb[i].size) : want, 0);
        }
        if (di) pool_note(di->ibuf, ind_addr ? 0 : n * (itype == VK_INDEX_TYPE_UINT32 ? 4 : 2), 1);
        if (ind_addr) pool_note(ind_addr, di ? 20 : 16, 4);
    }

    DrawArgs da;
    da.base_vertex = di ? di->base_vertex : (d ? d->first : 0);
    da.base_instance = d ? d->base_instance : di->base_instance;
    begin_rendering(cb);
    set_dynamic(cb, s, ps, quads ? VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST : t.topo, da);
    set_vertex_input(cb, s, vs);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    checkpoint(cb, 0, vs, ps, nullptr);
    bind_stage(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, vs, da);
    if (ps) bind_stage(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, ps, da);
    const u32 query = dump_query_begin(cb);
    if (quads) {
        // quads as triangles: 0 1 2, 0 2 3 per quad, from the draw's own indices when it has them
        const u32 n = static_cast<u32>(d ? d->count : di->count) / 4;
        if (!n) { dump_query_end(cb, query); return; }
        std::vector<u32> src(n * 4);
        if (di) {
            const u32 isz = index_size(static_cast<u32>(di->itype));
            std::vector<u8> raw(static_cast<size_t>(n) * 4 * isz);
            nvn::gpu_read(di->ibuf, raw.data(), raw.size());
            for (u32 i = 0; i < n * 4; i++)
                src[i] = isz == 1 ? raw[i] : isz == 2 ? reinterpret_cast<const uint16_t*>(raw.data())[i] : reinterpret_cast<const u32*>(raw.data())[i];
        } else {
            for (u32 i = 0; i < n * 4; i++) src[i] = i;
        }
        Slice sl = scratch(static_cast<VkDeviceSize>(n) * 24, 4);
        u32* o = reinterpret_cast<u32*>(sl.ptr);
        for (u32 q = 0; q < n; q++) {
            const u32* v = &src[4 * q];
            o[6 * q] = v[0], o[6 * q + 1] = v[1], o[6 * q + 2] = v[2], o[6 * q + 3] = v[0], o[6 * q + 4] = v[2], o[6 * q + 5] = v[3];
        }
        vkCmdBindIndexBuffer(cb, sl.buf, sl.off, VK_INDEX_TYPE_UINT32);
        vkCmdDrawIndexed(cb, n * 6, static_cast<u32>(std::max(d ? d->instances : di->instances, 1)), 0,
                         d ? d->first : di->base_vertex, static_cast<u32>(d ? d->base_instance : di->base_instance));
    } else if (di) {
        vkCmdBindIndexBuffer(cb, ib.buffer, ib.offset, itype);
        if (mdi_cnt) vkCmdDrawIndexedIndirectCount(cb, ind.buffer, ind.offset, cnt.buffer, cnt.offset, mdi_max, mdi_stride);
        else if (ind_addr) vkCmdDrawIndexedIndirect(cb, ind.buffer, ind.offset, 1, 20);
        else vkCmdDrawIndexed(cb, static_cast<u32>(di->count), static_cast<u32>(std::max(di->instances, 1)), 0, di->base_vertex,
                              static_cast<u32>(di->base_instance));
    } else {
        if (mdi_cnt) vkCmdDrawIndirectCount(cb, ind.buffer, ind.offset, cnt.buffer, cnt.offset, mdi_max, mdi_stride);
        else if (ind_addr) vkCmdDrawIndirect(cb, ind.buffer, ind.offset, 1, 16);
        else vkCmdDraw(cb, static_cast<u32>(d->count), static_cast<u32>(std::max(d->instances, 1)), static_cast<u32>(d->first),
                       static_cast<u32>(d->base_instance));
    }
    dump_query_end(cb, query);
    g_stats[ST_DRAWN]++;
    g_drawn_total++;
    if (dump_active() || dump_tracing()) {
        // the vertex streams the shader reads: address+size/stride, "i" when per instance
        char vb[512];
        int at = 0;
        u32 used = 0;
        for (u32 loc = 0; loc < 16; loc++)
            if (vs->inputs >> loc & 1 && s.va[loc].fmt && s.va[loc].stream < 16) used |= 1u << s.va[loc].stream;
        vb[0] = 0;
        for (u32 i = 0; i < 16 && at < 440; i++)
            if (used >> i & 1)
                at += snprintf(vb + at, sizeof vb - at, " s%u=%llx+%llx/%u%s", i, (unsigned long long)s.vb[i].address,
                               (unsigned long long)s.vb[i].size, s.vs[i].stride, s.vs[i].divisor ? "i" : "");
        dump_note("draw vs %s ps %s prim %d count %d inst %d first %d base %d bi %d%s%s idx %d ib %llx targets %d (%ux%u) depth %d blend %#x mask %#x ds %d%d%d%s q%d",
                  vs->name, ps ? ps->name : "-", prim, d ? d->count : di->count, d ? d->instances : di->instances,
                  d ? d->first : 0, di ? di->base_vertex : 0, d ? d->base_instance : di->base_instance,
                  ind_addr ? " indirect" : "", mdi_cnt ? " multi" : "", di != nullptr, di ? (unsigned long long)di->ibuf : 0ull,
                  R.ncolor, R.extent.width, R.extent.height,
                  R.depth.view != VK_NULL_HANDLE, s.color.blend_en, s.chmask.mask, (int)s.ds.dt, (int)s.ds.dw, (int)s.ds.df, vb, static_cast<int>(query));
    }
}

void do_dispatch(VkCommandBuffer cb, const nvn::p_dispatch* d) {
    State& s = state();
    const Shader* cs = s.sh[SH_CS];
    if (s.prog_bad) { g_stats[ST_NO_SHADER]++; return; }
    if (!cs) { g_stats[ST_NO_PROGRAM]++; return; }
    if (!cs->module || cs->unsupported) { g_stats[ST_UNSUPPORTED]++; return; }
    if (g_skip_cs) { g_stats[ST_DISABLED]++; return; }
    BufferRef ind{};
    if (d->ind && (g_skip_ind || !pool_find(d->ind, 12, ind))) { g_stats[ST_INDIRECT]++; return; }
    const Shader* sets[1] = {cs};
    VkPipelineLayout layout = layout_for(sets, 1);
    VkPipeline pipe = compute_pipeline(cs, layout);
    if (!pipe) { g_stats[ST_NO_PIPELINE]++; return; }
    end_rendering(cb);
    dump_trace(false);
    trace_first_use(pipe, cs, nullptr);
    DrawArgs da;
    da.dispatch[0] = static_cast<u32>(d->x), da.dispatch[1] = static_cast<u32>(d->y), da.dispatch[2] = static_cast<u32>(d->z);
    if (d->ind) {
        u32 g[3] = {};
        // as the CPU sees it now: for shaders that read the counts from c0. Checked against the counts the GPU dispatches with
        // (2026-11-07): the same in all but a handful of a race's ~4000 indirect dispatches.
        nvn::gpu_read(d->ind, g, 12);
        std::memcpy(da.dispatch, g, 12);
    }
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
    checkpoint(cb, 2, cs, nullptr, nullptr);
    bind_stage(cb, VK_PIPELINE_BIND_POINT_COMPUTE, layout, cs, da);
    if (d->ind) vkCmdDispatchIndirect(cb, ind.buffer, ind.offset);
    else vkCmdDispatch(cb, da.dispatch[0], da.dispatch[1], da.dispatch[2]);
    full_barrier(cb);
    g_stats[ST_DISPATCHED]++;
    if (dump_active()) dump_note("dispatch cs %s %u %u %u%s", cs->name, da.dispatch[0], da.dispatch[1], da.dispatch[2], d->ind ? " indirect" : "");
}

}  // namespace

// ---------------------------------------------------------------- interface

void draw_init() {
    static bool done = false;
    if (done) return;
    done = true;
    if (const char* e = getenv("HOST_NVN_SKIP")) {
        const std::string v = e;
        g_skip_cs = v.find("cs") != std::string::npos;
        g_skip_ind = v.find("ind") != std::string::npos;
        g_skip_draw = v.find("draw") != std::string::npos;
        g_skip_mesh = v.find("mesh") != std::string::npos;
    }
    Device& c = ctx();
    VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    si.magFilter = si.minFilter = VK_FILTER_LINEAR;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.maxLod = VK_LOD_CLAMP_NONE;
    check(vkCreateSampler(c.dev, &si, nullptr, &g_default_sampler), "default sampler");
    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    check(vkCreateDescriptorSetLayout(c.dev, &li, nullptr, &g_empty_layout), "empty layout");
    // pipeline cache, kept next to the shaders
    std::vector<char> data;
    if (const char* e = getenv("HOST_NVN_PIPELINE_CACHE")) g_pcache_path = e;
    else g_pcache_path = "nvn_pipelines.bin";
    {
        std::ifstream f(g_pcache_path, std::ios::binary);
        if (f) data.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    }
    VkPipelineCacheCreateInfo pci{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
    pci.initialDataSize = data.size();
    pci.pInitialData = data.empty() ? nullptr : data.data();
    if (vkCreatePipelineCache(c.dev, &pci, nullptr, &g_pcache) != VK_SUCCESS) {
        pci.initialDataSize = 0, pci.pInitialData = nullptr;
        check(vkCreatePipelineCache(c.dev, &pci, nullptr, &g_pcache), "vkCreatePipelineCache");
    }
    LOG("nvk: draw path ready: %zu shaders, pipeline cache %s (%zu bytes)", shader_count(), g_pcache_path.c_str(), data.size());
    warm_up(load_descs());
}

void draw_note_write(volatile void* host, const void* data, u32 size) {
    for (u32 i = 0; i + 4 <= size; i += 4) {
        u32 w;
        std::memcpy(&w, static_cast<const u8*>(data) + i, 4);
        g_written[reinterpret_cast<uintptr_t>(host) + i] = w;
    }
}

u32 draw_written_word(volatile void* host) {
    if (auto it = g_written.find(reinterpret_cast<uintptr_t>(host)); it != g_written.end()) return it->second;
    return *static_cast<volatile u32*>(host);
}

void draw_sub_begin(u64 done) {
    draw_init();
    g_written.clear();
    g_set_gen++, g_set_kept = 0;
    kept_sub_begin();
    g_view_cache.clear();
    dyn_reset();
    Device& c = ctx();
    while (!g_arena_busy.empty() && g_arena_busy.front()->value <= done) {
        Arena* a = g_arena_busy.front();
        g_arena_busy.pop_front();
        for (Chunk& ch : a->chunks) ch.used = 0;
        a->cur = 0;
        for (VkDescriptorPool p : a->pools) vkResetDescriptorPool(c.dev, p, 0);
        a->pool_cur = 0;
        vkResetCommandPool(c.dev, a->cmd_pool, 0);
        a->pre_open = false;
        a->pre = a->pre_first;
        a->pre_more_used = 0;
        g_arena_free.push_back(a);
    }
    if (g_arena_free.empty()) {
        Arena* a = new Arena;
        VkCommandPoolCreateInfo pi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pi.queueFamilyIndex = c.family;
        check(vkCreateCommandPool(c.dev, &pi, nullptr, &a->cmd_pool), "arena command pool");
        VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ai.commandPool = a->cmd_pool;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1;
        check(vkAllocateCommandBuffers(c.dev, &ai, &a->pre), "arena command buffer");
        a->pre_first = a->pre;
        g_arena_free.push_back(a);
    }
    A = g_arena_free.back();
    g_arena_free.pop_back();
}

VkCommandBuffer draw_sub_flush() {
    dyn_reset();
    kept_flush();
    if (!A->pre_open) return VK_NULL_HANDLE;
    full_barrier(A->pre);
    vkEndCommandBuffer(A->pre);
    const VkCommandBuffer pre = A->pre;
    if (A->pre_more_used == A->pre_more.size()) {
        VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ai.commandPool = A->cmd_pool;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1;
        VkCommandBuffer n;
        check(vkAllocateCommandBuffers(ctx().dev, &ai, &n), "arena command buffer (part)");
        A->pre_more.push_back(n);
    }
    A->pre = A->pre_more[A->pre_more_used++];
    A->pre_open = false;
    return pre;
}

VkCommandBuffer draw_sub_commit(u64 value) {
    A->value = value;
    kept_flush();
    VkCommandBuffer pre = VK_NULL_HANDLE;
    if (A->pre_open) {
        full_barrier(A->pre);
        vkEndCommandBuffer(A->pre);
        pre = A->pre;
    }
    g_arena_busy.push_back(A);
    A = nullptr;
    if (g_pipes_made != g_pcache_saved_at && value % 600 == 0) {
        g_pcache_saved_at = g_pipes_made;
        save_pcache();
    }
    return pre;
}

void draw_cb_changed() { dyn_reset(); }

void draw_queue_activate(u64 serial) {
    auto& p = g_states[serial];
    if (!p) p = std::make_unique<State>();
    S = p.get();
}

void draw_queue_forget(u64 serial) {
    auto it = g_states.find(serial);
    if (it == g_states.end()) return;
    if (S == it->second.get()) S = nullptr;
    g_states.erase(it);
}

bool draw_state(u32 op, const u8* a, u32 size) {
    using namespace nvn;
    State& s = state();
    switch (op) {
    case OP_BIND_PROGRAM: {
        const auto* b = reinterpret_cast<const p_bind_program*>(a);
        if (!b->prog) {
            for (int i = 0; i < SH_N; i++)
                if (b->stages >> i & 1) s.sh[i] = nullptr;
            break;
        }
        const Prog p = prog_of(b->prog);
        s.prog_bad = p.bad;
        for (int i = 0; i < SH_N; i++)
            if (b->stages >> i & 1) s.sh[i] = p.sh[i];
        break;
    }
    case OP_BIND_STATE: {
        const auto* b = reinterpret_cast<const p_bind_state*>(a);
        const size_t n = size - 8 - offsetof(p_bind_state, bytes);
        switch (b->kind) {
        case 0: {
            blend_st bl{};
            std::memcpy(&bl, b->bytes, std::min(n, sizeof bl));
            s.blend[bl.target & 7] = bl;
            break;
        }
        case 1: std::memcpy(&s.color, b->bytes, std::min(n, sizeof s.color)); break;
        case 2: std::memcpy(&s.chmask, b->bytes, std::min(n, sizeof s.chmask)); break;
        case 3: std::memcpy(&s.ms, b->bytes, std::min(n, sizeof s.ms)); break;
        case 4: std::memcpy(&s.poly, b->bytes, std::min(n, sizeof s.poly)); break;
        case 5: std::memcpy(&s.ds, b->bytes, std::min(n, sizeof s.ds)); break;
        default: break;
        }
        break;
    }
    case OP_BIND_VATTR: {
        const auto* b = reinterpret_cast<const p_bind_vattr*>(a);
        for (int i = 0; i < 16; i++) s.va[i] = va_st{};
        for (int i = 0; i < b->n && b->first + i < 16; i++) std::memcpy(&s.va[b->first + i], &b->st[i], 4);
        break;
    }
    case OP_BIND_VSTREAM: {
        const auto* b = reinterpret_cast<const p_bind_vstream*>(a);
        for (int i = 0; i < b->n && b->first + i < 16; i++) s.vs[b->first + i] = b->st[i];
        break;
    }
    case OP_BIND_VBUF:
    case OP_BIND_VBUF1: {
        p_bind_vbuf t;
        const p_bind_vbuf* b = vbuf_rec(op, a, &t);
        for (int i = 0; i < b->n && b->first + i < 16; i++) s.vb[b->first + i] = b->r[i];
        break;
    }
    case OP_BIND_UBO:
    case OP_BIND_UBO_SMALL: {
        p_bind_buf t;
        const p_bind_buf* b = bind_buf_rec(op, a, &t);
        if (b->stage >= 0 && b->stage < SH_N && b->index >= 0 && b->index < 18) s.ubo[b->stage][b->index] = {b->addr, b->size};
        break;
    }
    case OP_BIND_SSBO: {
        const auto* b = reinterpret_cast<const p_bind_buf*>(a);
        if (b->stage >= 0 && b->stage < SH_N && b->index >= 0 && b->index < 16) s.ssbo[b->stage][b->index] = {b->addr, b->size};
        break;
    }
    case OP_BIND_TEX:
    case OP_BIND_TEX1:
    case OP_BIND_SAMP:
    case OP_BIND_IMG: {
        p_bind_handles t;
        const p_bind_handles* b = bind_rec(op, a, &t);
        bh_fresh(b);
        if (b->stage < 0 || b->stage >= SH_N) break;
        for (int i = 0; i < b->n; i++) {
            const int slot = b->first + i;
            const u32 h = static_cast<u32>(b->h[i]);
            if (op == OP_BIND_SAMP) { if (slot >= 0 && slot < 128) s.samp[b->stage][slot] = h >> 20; }
            else if (op == OP_BIND_IMG) { if (slot >= 0 && slot < 8) s.img[b->stage][slot] = h; }
            else if (slot >= 0 && slot < 256) s.tex[b->stage][slot] = h;
        }
        break;
    }
    case OP_VIEWPORTS: {
        const auto* b = reinterpret_cast<const p_viewports*>(a);
        for (int i = 0; i < b->n && b->first + i < 16; i++) std::memcpy(s.vp[b->first + i], b->v[i], 16);
        s.nvp = std::max(s.nvp, std::min(16, b->first + b->n));
        break;
    }
    case OP_SCISSOR: std::memcpy(&s.sc, a, sizeof s.sc); s.has_sc = true; break;
    case OP_DEPTH_RANGE: {
        const auto* b = reinterpret_cast<const p_depth_range*>(a);
        s.dn = b->n, s.df = b->f;
        break;
    }
    case OP_STENCIL: {
        const auto* b = reinterpret_cast<const p_stencil*>(a);
        for (int f = 0; f < 2; f++) {
            if (!(b->face >> f & 1)) continue;
            if (b->which == 0) s.st_vmask[f] = static_cast<u32>(b->value_mask);
            else if (b->which == 1) s.st_wmask[f] = static_cast<u32>(b->mask);
            else s.st_ref[f] = static_cast<u32>(b->ref);
        }
        break;
    }
    case OP_POLY_OFFSET: std::memcpy(&s.po, a, sizeof s.po); break;
    case OP_BLEND_COLOR: std::memcpy(s.blend_color, a, 16); break;
    case OP_PRIM_RESTART: {
        const auto* b = reinterpret_cast<const p_prim_restart*>(a);
        s.prim_restart = b->on != 0;
        s.restart_index = b->index;
        if (b->on && b->index != 0xffff && b->index != 0xffffffffu) ONCE_RED("primitive restart index %#x: Vulkan restarts at all ones", b->index);
        break;
    }
    case OP_DEPTH_CLAMP: s.depth_clamp = reinterpret_cast<const p_depth_clamp*>(a)->on != 0; break;
    case OP_RAST_DISCARD: s.rast_discard = reinterpret_cast<const p_rast_discard*>(a)->on != 0; break;
    case OP_RENDER_COND: {
        const auto* b = reinterpret_cast<const p_render_cond*>(a);
        s.render_mode = b->mode;
        if (b->mode < 2) {
            // Render if the 64-bit counters at addr and addr + 16 are equal (mode 0) / differ (mode 1), as memory
            // will be at this point of the stream.
            u64 v[3] = {};
            if (u8* h = nvn::gpu_ptr(b->addr, 24)) {
                u32 w[6];
                for (int i = 0; i < 6; i++) w[i] = draw_written_word(h + 4 * i);
                std::memcpy(v, w, 24);
            } else {
                ONCE_RED("render condition at %#llx: not backed by memory; rendered", static_cast<unsigned long long>(b->addr));
            }
            const bool eq = v[0] == v[2];
            s.render_mode = eq == (b->mode == 0) ? 2 : 3;
            dump_note("render condition: mode %d addr %#llx counters %#llx %#llx -> %s", b->mode, static_cast<unsigned long long>(b->addr),
                      static_cast<unsigned long long>(v[0]), static_cast<unsigned long long>(v[2]), s.render_mode == 2 ? "render" : "skip");
        }
        break;
    }
    case OP_VP_SWIZZLE: {
        const auto* b = reinterpret_cast<const p_vp_swizzle*>(a);
        for (int i = 0; i < b->n && b->first + i < 16; i++) std::memcpy(s.vp_sw[b->first + i], b->sw[i], 4);
        break;
    }
    case OP_PUSH_CONST: {
        const auto* b = reinterpret_cast<const p_push_const*>(a);
        if (b->offset < NVN_PUSH_SIZE) std::memcpy(s.push[b->compute ? 1 : 0] + b->offset, b->data, std::min<u32>(b->size, NVN_PUSH_SIZE - b->offset));
        break;
    }
    case OP_ALPHA_REF:
    case OP_SET_TEX_POOL:
    case OP_SET_SAMP_POOL: break;
    default: return false;
    }
    return true;
}

void draw(VkCommandBuffer cb, u32 op, const u8* a, u32 size, const u8* next) {
    if (op == nvn::OP_DISPATCH) do_dispatch(cb, reinterpret_cast<const nvn::p_dispatch*>(a));
    else if (op == nvn::OP_DRAW_TEX) g_stats[ST_DRAW_TEX]++;
    else do_draw(cb, op, a, size, next);
}

void draw_set_tex_pool_n(int n) {
    std::scoped_lock lk{g_tab_mu};
    if (n > 0 && static_cast<size_t>(n) > g_tex.size()) g_tex.resize(static_cast<size_t>(n));
}

// The game registers the same texture and sampler handles again and again: only a change makes what a handle resolves
// to new (bind_epoch_bump: the descriptor sets made so far may not be used again).
u64 g_reg_changes = 0, g_reg_same = 0;

void draw_reg_tex(int id, const tex_t* t, const view_t* v, bool image) {
    if (id < 0 || id >= (1 << 20)) return;
    std::scoped_lock lk{g_tab_mu};
    if (static_cast<size_t>(id) >= g_tex.size()) g_tex.resize(static_cast<size_t>(id) + 1);
    TexEntry& e = g_tex[static_cast<size_t>(id)];
    const bool valid = t && t->serial;
    const bool same = e.valid == valid && (!valid || (e.t.serial == t->serial && e.t.mem == t->mem && e.image == image &&
                                                      e.has_v == (v != nullptr) && (!v || std::memcmp(&e.v, v, sizeof *v) == 0)));
    if (same) {
        g_reg_same++;
        return;
    }
    g_reg_changes++;
    e.gen = ++g_reg_gen;
    e.valid = valid;
    if (!e.valid) return;
    e.t = *t;
    e.has_v = v != nullptr;
    if (v) e.v = *v;
    e.image = image;
}

void draw_reg_samp(int id, const samp_par* p) {
    if (id < 0 || id >= (1 << 12)) return;
    std::scoped_lock lk{g_tab_mu};
    if (static_cast<size_t>(id) >= g_samp.size()) g_samp.resize(static_cast<size_t>(id) + 1);
    auto& e = g_samp[static_cast<size_t>(id)];
    if (e.first == (p != nullptr) && (!p || std::memcmp(&e.second, p, sizeof *p) == 0)) {
        g_reg_same++;
        return;
    }
    g_reg_changes++;
    if (static_cast<size_t>(id) >= g_samp_gen.size()) g_samp_gen.resize(static_cast<size_t>(id) + 1);
    g_samp_gen[static_cast<size_t>(id)] = ++g_reg_gen;
    e = {p != nullptr, p ? *p : samp_par{}};
    if (static_cast<size_t>(id) < g_samp_vk.size()) g_samp_vk[static_cast<size_t>(id)] = VK_NULL_HANDLE;
}

void draw_prog_release(const prog_t* p) {
    std::scoped_lock lk{g_prog_mu};
    g_progs.erase(p->serial);
}

void draw_prog_forget(const prog_t* p) {
    // Its shaders stay bound as resolved (the Shader objects live for the whole run).
    std::scoped_lock lk{g_prog_mu};
    g_progs.erase(p->serial);
}

void draw_tex_cpu_written(const tex_t* t) {
    bind_epoch_bump();
    if (Image* img = image_find(t->serial)) img->uploaded = false;
}

VkCommandBuffer draw_pre_cb() { return pre_cb(); }

void draw_ensure_content(Image* img, const tex_t* t) { ensure_content(img, t, true); }

std::string checkpoint_desc(u64 marker) {
    if (!marker || g_marks.empty() || marker > g_mark_n || g_mark_n - marker >= MarkN) return "?";
    const Mark& m = g_marks[(marker - 1) % MarkN];
    static const char* kinds[] = {"draw vs/ps", "mesh draw ts/ms/ps", "dispatch cs"};
    char b[160];
    snprintf(b, sizeof b, "%s %016llx %016llx %016llx (draw %llu; %llu markers after it)", kinds[m.kind % 3], static_cast<unsigned long long>(m.a),
             static_cast<unsigned long long>(m.b), static_cast<unsigned long long>(m.c), static_cast<unsigned long long>(m.sub),
             static_cast<unsigned long long>(g_mark_n - marker));
    return b;
}

void draw_log_stats() {
    std::string line;
    for (int i = 0; i < ST_N; i++) {
        if (!g_stats[i]) continue;
        line += " ";
        line += k_stat_names[i];
        line += " " + std::to_string(g_stats[i]);
        g_stats[i] = 0;
    }
    static u64 last_epoch = 0;
    const u64 epoch = g_bind_epoch.load();
    LOG("nvk: draws:%s; descriptor sets %llu made, %llu reused; handles registered %llu changed, %llu the same; bind epoch +%llu",
        line.c_str(), static_cast<unsigned long long>(g_set_made), static_cast<unsigned long long>(g_set_hits),
        static_cast<unsigned long long>(g_reg_changes), static_cast<unsigned long long>(g_reg_same),
        static_cast<unsigned long long>(epoch - last_epoch));
    if (g_kept_on)
        LOG("nvk: constant blocks: %llu found in device memory, %llu new, %llu left in system memory (store full); %zu kept, %zu MB; store emptied %llu times",
            static_cast<unsigned long long>(g_kept_hits), static_cast<unsigned long long>(g_kept_new), static_cast<unsigned long long>(g_kept_past),
            g_kept.size(), g_kept_blocks.size() * static_cast<size_t>(KeptBlockSize >> 20), static_cast<unsigned long long>(g_kept_wipes));
    last_epoch = epoch;
    g_set_made = g_set_hits = g_reg_changes = g_reg_same = 0;
    g_kept_hits = g_kept_new = g_kept_past = 0;
}

}  // namespace nvk
