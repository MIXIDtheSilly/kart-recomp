#include "shaders.h"
#include "hitch.h"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

namespace nvk {
namespace {

struct Row {
    ShStage stage;
    std::vector<u8> c1;
    std::vector<Binding> bindings;  // kind, reg, off, aux from the table; types from the SPIR-V
};

std::string g_dir;
std::unordered_map<u64, Row> g_rows;
std::unordered_map<u64, std::unique_ptr<Shader>> g_shaders;  // loaded (or failed: module null)
std::mutex g_mu;

u64 key_u64(const u8 k[8]) {
    u64 v = 0;
    for (int i = 0; i < 8; i++) v = v << 8 | k[i];  // big-endian: the hex name reads the same
    return v;
}

std::string shader_dir() {
    if (const char* e = getenv("HOST_NVN_SHADERS")) return e;
    char exe[MAX_PATH];
    const DWORD n = GetModuleFileNameA(nullptr, exe, MAX_PATH);
    std::string p(exe, n);
    p = p.substr(0, p.find_last_of("\\/"));    // ...\host\out
    p = p.substr(0, p.find_last_of("\\/"));    // ...\host
    p = p.substr(0, p.find_last_of("\\/"));    // ...\recomp
    return p + "\\work\\nvn_shaders";
}

ShStage stage_of(const std::string& s) {
    if (s == "vs") return SH_VS;
    if (s == "ps") return SH_PS;
    if (s == "gs") return SH_GS;
    if (s == "ts") return SH_TS;
    if (s == "ms") return SH_MS;
    return SH_CS;
}

BKind kind_of(const std::string& k) {
    static const std::unordered_map<std::string, BKind> m{
        {"u", BKind::Ubo},      {"c1", BKind::C1},       {"d0", BKind::Drv0},     {"d2", BKind::Drv2},
        {"p", BKind::Push},     {"s", BKind::Ssbo},      {"su", BKind::SsboUbo},  {"up", BKind::UboPtr},
        {"gm", BKind::GmemMap}, {"gl", BKind::GmemLog},  {"t", BKind::Tex},       {"i", BKind::Img}};
    auto it = m.find(k);
    return it == m.end() ? BKind::Unknown : it->second;
}

void load_table() {
    static bool done = false;
    if (done) return;
    done = true;
    g_dir = shader_dir();
    std::ifstream f(g_dir + "\\refl.tsv");
    if (!f) {
        LOG("[RED] nvk: no shader reflection table at %s\\refl.tsv (run tools/nvn_shaders.py and tools/nvn_refl.py): nothing is drawn",
            g_dir.c_str());
        return;
    }
    std::string line;
    while (std::getline(f, line)) {
        if (line.empty() || line[0] == '#') continue;
        size_t t1 = line.find('\t'), t2 = line.find('\t', t1 + 1), t3 = line.find('\t', t2 + 1);
        if (t1 == std::string::npos || t2 == std::string::npos || t3 == std::string::npos) continue;
        const u64 key = std::stoull(line.substr(0, t1), nullptr, 16);
        Row r;
        r.stage = stage_of(line.substr(t1 + 1, t2 - t1 - 1));
        const std::string c1 = line.substr(t2 + 1, t3 - t2 - 1);
        if (c1 != "-")
            for (size_t i = 0; i + 1 < c1.size(); i += 2) r.c1.push_back(static_cast<u8>(std::stoul(c1.substr(i, 2), nullptr, 16)));
        size_t p = t3 + 1;
        while (p < line.size()) {
            size_t e = line.find(' ', p);
            if (e == std::string::npos) e = line.size();
            const std::string item = line.substr(p, e - p);
            p = e + 1;
            unsigned b = 0, reg = 0, off = 0, aux = 0;
            char kind[8] = {};
            if (sscanf(item.c_str(), "%u:%7[a-z0-9]:%u:%u:%u", &b, kind, &reg, &off, &aux) != 5) continue;
            Binding bi;
            bi.binding = b;
            bi.kind = kind_of(kind);
            bi.reg = reg, bi.off = off, bi.aux = aux;
            r.bindings.push_back(bi);
        }
        g_rows.emplace(key, std::move(r));
    }
    LOG("nvk: %zu translated shaders in %s", g_rows.size(), g_dir.c_str());
}

// ---------------------------------------------------------------- SPIR-V

enum : u32 {
    OpName = 5, OpEntryPoint = 15, OpExecutionMode = 16, OpTypeInt = 21, OpTypeFloat = 22, OpTypeVector = 23, OpTypeImage = 25,
    OpTypeSampler = 26, OpTypeSampledImage = 27, OpTypeArray = 28, OpTypeRuntimeArray = 29, OpTypeStruct = 30,
    OpTypePointer = 32, OpConstant = 43, OpVariable = 59, OpDecorate = 71,
    DecBlock = 2, DecBufferBlock = 3, DecBuiltIn = 11, DecLocation = 30, DecBinding = 33, DecSet = 34,
    ScUniformConstant = 0, ScInput = 1, ScUniform = 2, ScOutput = 3, ScStorageBuffer = 12,
    ExecLocalSize = 17,
};

struct Var {
    u32 type = 0, sc = 0;
    int binding = -1, location = -1;
    bool builtin = false;
};

// Reads the SPIR-V's resources into sh (descriptor types, counts, vertex inputs, colour outputs) and
// moves its descriptor set to sh.set. False when it is malformed.
bool reflect(std::vector<u32>& w, Shader& sh, const Row& row) {
    if (w.size() < 5 || w[0] != 0x07230203) return false;
    std::unordered_map<u32, std::vector<u32>> types;  // id -> the type instruction's words (opcode first)
    std::unordered_map<u32, u32> consts;
    std::unordered_map<u32, Var> vars;
    std::unordered_map<u32, u32> block;               // struct id -> 2 Block / 3 BufferBlock
    std::vector<size_t> set_words;
    std::unordered_map<u32, int> bind_of, loc_of;
    std::unordered_map<u32, bool> builtin_of;
    std::unordered_map<u32, std::string> name_of;
    for (size_t i = 5; i < w.size();) {
        const u32 op = w[i] & 0xffff, n = w[i] >> 16;
        if (!n || i + n > w.size()) return false;
        switch (op) {
        case OpName:
            if (n >= 3) name_of[w[i + 1]] = reinterpret_cast<const char*>(&w[i + 2]);
            break;
        case OpDecorate:
            if (n >= 3) {
                const u32 id = w[i + 1], d = w[i + 2];
                if (d == DecSet && n >= 4) set_words.push_back(i + 3);
                if (d == DecBinding && n >= 4) bind_of[id] = static_cast<int>(w[i + 3]);
                if (d == DecLocation && n >= 4) loc_of[id] = static_cast<int>(w[i + 3]);
                if (d == DecBuiltIn) builtin_of[id] = true;
                if (d == DecBlock || d == DecBufferBlock) block[id] = d;
            }
            break;
        case OpExecutionMode:
            if (n >= 6 && w[i + 2] == ExecLocalSize) sh.local[0] = w[i + 3], sh.local[1] = w[i + 4], sh.local[2] = w[i + 5];
            break;
        case OpTypeInt: case OpTypeFloat: case OpTypeVector: case OpTypeImage: case OpTypeSampler: case OpTypeSampledImage:
        case OpTypeArray: case OpTypeRuntimeArray: case OpTypeStruct: case OpTypePointer:
            types[w[i + 1]] = std::vector<u32>(w.begin() + static_cast<ptrdiff_t>(i), w.begin() + static_cast<ptrdiff_t>(i + n));
            break;
        case OpConstant:
            if (n >= 4) consts[w[i + 2]] = w[i + 3];
            break;
        case OpVariable:
            if (n >= 4) vars[w[i + 2]] = Var{w[i + 1], w[i + 3]};
            break;
        default: break;
        }
        i += n;
    }
    for (size_t at : set_words) w[at] = sh.set;
    auto type = [&](u32 id) -> const std::vector<u32>* {
        auto it = types.find(id);
        return it == types.end() ? nullptr : &it->second;
    };
    // the scalar kind under vectors / arrays: 0 float, 1 sint, 2 uint
    auto scalar_kind = [&](u32 id) -> u8 {
        for (int depth = 0; depth < 8; depth++) {
            const auto* t = type(id);
            if (!t) return 0;
            const u32 op = (*t)[0] & 0xffff;
            if (op == OpTypeFloat) return 0;
            if (op == OpTypeInt) return (*t)[3] ? 1 : 2;
            if (op == OpTypeVector || op == OpTypeArray) id = (*t)[2];
            else return 0;
        }
        return 0;
    };
    std::unordered_map<u32, const Binding*> meta;
    for (const Binding& b : row.bindings) meta[b.binding] = &b;
    for (auto& [id, v] : vars) {
        const auto* pt = type(v.type);
        if (!pt || ((*pt)[0] & 0xffff) != OpTypePointer) continue;
        u32 pointee = (*pt)[3];
        if (auto it = loc_of.find(id); it != loc_of.end() && !builtin_of.count(id)) {
            const int loc = it->second;
            if (v.sc == ScInput && sh.stage == SH_VS && loc < 32) {
                sh.inputs |= 1u << loc;
                sh.input_type[loc] = scalar_kind(pointee);
            }
            if (v.sc == ScOutput && sh.stage == SH_PS && loc < 8) sh.outputs |= 1u << loc;
            continue;
        }
        auto bit = bind_of.find(id);
        if (bit == bind_of.end()) continue;
        if (v.sc != ScUniformConstant && v.sc != ScUniform && v.sc != ScStorageBuffer) continue;
        Binding b;
        b.binding = static_cast<u32>(bit->second);
        if (auto m = meta.find(b.binding); m != meta.end()) b = *m->second;
        else if (b.binding == 0 && v.sc == ScUniform) b.kind = BKind::Support;
        else if (auto nm = name_of.find(id); nm != name_of.end() && v.sc == ScUniform) {
            // a constant buffer the reflection leaves out (the translator's c0 reads): named <stage>_c<bank>
            const std::string& s = nm->second;
            const size_t u = s.find("_c");
            if (u != std::string::npos && u + 2 < s.size() && isdigit(static_cast<unsigned char>(s[u + 2]))) {
                const int bank = atoi(s.c_str() + u + 2);
                if (bank == 0) b.kind = BKind::Drv0;
                else if (bank == 1) b.kind = BKind::C1;
                else if (bank == 2) b.kind = BKind::Drv2;
                else if (bank >= 3 && bank <= 16) b.kind = BKind::Ubo, b.reg = static_cast<u32>(bank - 3);
                else if (bank == 24) b.kind = BKind::Push;
            }
        }
        // arrays of descriptors
        const auto* t = type(pointee);
        while (t && (((*t)[0] & 0xffff) == OpTypeArray || ((*t)[0] & 0xffff) == OpTypeRuntimeArray)) {
            if (((*t)[0] & 0xffff) == OpTypeArray) {
                auto c = consts.find((*t)[3]);
                b.count *= c == consts.end() ? 1 : c->second;
            }
            pointee = (*t)[2];
            t = type(pointee);
        }
        if (!t) return false;
        const u32 op = (*t)[0] & 0xffff;
        if (v.sc == ScStorageBuffer) b.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        else if (v.sc == ScUniform) {
            auto bl = block.find(pointee);
            b.type = bl != block.end() && bl->second == DecBufferBlock ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER : VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        } else if (op == OpTypeSampler) b.type = VK_DESCRIPTOR_TYPE_SAMPLER;
        else {
            const std::vector<u32>* img = t;
            if (op == OpTypeSampledImage) img = type((*t)[2]);
            if (!img || ((*img)[0] & 0xffff) != OpTypeImage) return false;
            b.dim = static_cast<u8>((*img)[3]);
            b.arrayed = (*img)[5] != 0;
            b.ms = (*img)[6] != 0;
            const bool storage = (*img)[7] == 2;
            if (b.dim == 5) b.type = storage ? VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER : VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER;
            else if (storage) b.type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            else b.type = op == OpTypeSampledImage ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        }
        if (b.kind == BKind::Unknown && !sh.unsupported) sh.unsupported = "a binding the reflection does not name";
        if (b.kind == BKind::GmemLog && !sh.unsupported) sh.unsupported = "global memory write log";
        if (b.kind == BKind::GmemMap && !sh.unsupported) sh.unsupported = "raw global memory (pool table not built yet)";
        sh.bindings.push_back(b);
    }
    std::sort(sh.bindings.begin(), sh.bindings.end(), [](const Binding& a, const Binding& b) { return a.binding < b.binding; });
    return true;
}

VkShaderStageFlagBits vk_stage(ShStage s) {
    switch (s) {
    case SH_VS: return VK_SHADER_STAGE_VERTEX_BIT;
    case SH_PS: return VK_SHADER_STAGE_FRAGMENT_BIT;
    case SH_GS: return VK_SHADER_STAGE_GEOMETRY_BIT;
    case SH_TS: return VK_SHADER_STAGE_TASK_BIT_EXT;
    case SH_MS: return VK_SHADER_STAGE_MESH_BIT_EXT;
    default: return VK_SHADER_STAGE_COMPUTE_BIT;
    }
}

std::unique_ptr<Shader> load(u64 key, const Row& row) {
    auto sh = std::make_unique<Shader>();
    sh->key = key;
    snprintf(sh->name, sizeof sh->name, "%016llx", static_cast<unsigned long long>(key));
    sh->stage = row.stage;
    sh->set = stage_set(row.stage);
    sh->vk_stage = vk_stage(row.stage);
    sh->c1 = row.c1;
    // Task and mesh shaders: the translator made compute kernels of them; tools/nvn_mesh.py turns those into
    // TaskEXT / MeshEXT modules (<key>.mesh.spv), and per-primitive outputs into flat per-vertex ones where they fit
    // (<key>.meshflat.spv), which the translated fragment shaders read without PerPrimitiveEXT.
    std::string file = g_dir + "\\" + sh->name + ".spv";
    if (row.stage == SH_TS || row.stage == SH_MS) {
        const std::string flat = g_dir + "\\" + sh->name + ".meshflat.spv", mesh = g_dir + "\\" + sh->name + ".mesh.spv";
        file = std::ifstream(flat).good() ? flat : mesh;
    } else if (const std::string fix = g_dir + "\\" + sh->name + ".fix.spv"; std::ifstream(fix).good()) {
        file = fix;   // one the driver miscompiles, restructured by tools/nvn_spvfix.py
    }
    std::ifstream f(file, std::ios::binary);
    if (!f) {
        sh->unsupported = "no SPIR-V file";
        return sh;
    }
    std::vector<char> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    std::vector<u32> words(bytes.size() / 4);
    std::memcpy(words.data(), bytes.data(), words.size() * 4);
    if (!reflect(words, *sh, row)) {
        sh->unsupported = "malformed SPIR-V";
        return sh;
    }
    if ((row.stage == SH_TS || row.stage == SH_MS) && !ctx().feat.mesh_shader) sh->unsupported = "no mesh shaders on this GPU";
    Device& c = ctx();
    VkShaderModuleCreateInfo mi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    mi.codeSize = words.size() * 4;
    mi.pCode = words.data();
    if (vkCreateShaderModule(c.dev, &mi, nullptr, &sh->module) != VK_SUCCESS) {
        sh->module = VK_NULL_HANDLE;
        sh->unsupported = "vkCreateShaderModule failed";
        return sh;
    }
    std::vector<VkDescriptorSetLayoutBinding> lb;
    for (const Binding& b : sh->bindings)
        lb.push_back(VkDescriptorSetLayoutBinding{b.binding, b.type, b.count, static_cast<VkShaderStageFlags>(sh->vk_stage), nullptr});
    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    li.bindingCount = static_cast<u32>(lb.size());
    li.pBindings = lb.data();
    check(vkCreateDescriptorSetLayout(c.dev, &li, nullptr, &sh->layout), "vkCreateDescriptorSetLayout");
    // HOST_NVN_SKIP_SHADERS=<key>,<key>...: draws with these shaders are skipped (to find one the GPU faults on)
    if (const char* e = getenv("HOST_NVN_SKIP_SHADERS"); e && std::strstr(e, sh->name)) sh->unsupported = "skipped (HOST_NVN_SKIP_SHADERS)";
    return sh;
}

}  // namespace

u32 stage_set(ShStage s) {
    switch (s) {
    case SH_VS: case SH_CS: return 0;
    case SH_PS: return 1;
    case SH_GS: return 2;
    case SH_TS: return 3;
    default: return 4;
    }
}

size_t shader_count() {
    std::scoped_lock lk{g_mu};
    load_table();
    return g_rows.size();
}

const Shader* shader(const u8 k[8]) { return shader_by_key(key_u64(k)); }

// Loaded outside g_mu (file reads, the driver): the warm-up threads load shaders while the renderer looks them up.
const Shader* shader_by_key(u64 key) {
    const Row* row;
    {
        std::scoped_lock lk{g_mu};
        load_table();
        if (auto it = g_shaders.find(key); it != g_shaders.end()) return it->second.get();
        auto r = g_rows.find(key);
        if (r == g_rows.end()) {
            LOG("[RED] nvk: shader %016llx has no translation: its draws are skipped", static_cast<unsigned long long>(key));
            g_shaders.emplace(key, nullptr);
            return nullptr;
        }
        row = &r->second;   // g_rows is not changed after load_table
    }
    HitchTimer ht{HI_SHADER};
    auto sh = load(key, *row);
    std::scoped_lock lk{g_mu};
    if (auto it = g_shaders.find(key); it != g_shaders.end()) {   // loaded by another thread meanwhile
        if (sh->module) vkDestroyShaderModule(ctx().dev, sh->module, nullptr);
        if (sh->layout) vkDestroyDescriptorSetLayout(ctx().dev, sh->layout, nullptr);
        return it->second.get();
    }
    if (sh->unsupported) LOG("[RED] nvk: shader %s (%s): %s; its draws are skipped", sh->name,
                             row->stage == SH_PS ? "ps" : row->stage == SH_VS ? "vs" : "other", sh->unsupported);
    const Shader* out = sh.get();
    g_shaders.emplace(key, std::move(sh));
    return out;
}

}  // namespace nvk
