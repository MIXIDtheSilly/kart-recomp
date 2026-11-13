#include "shaders.h"
#include "hitch.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

std::string host_exe_variant();  // localcfg.cpp: "1.8.0" for kart-1.8.0.exe, "" for kart.exe

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
    const std::string variant = host_exe_variant();  // kart-<version>.exe: that version's shader tables
    return p + "\\work\\nvn_shaders" + (variant.empty() ? "" : "-" + variant);
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

// The game's vertex shaders read the base vertex and base instance from driver constants, c[0] + 0x640 and 0x644, where
// the console's draw macro puts them. For an indirect draw only the GPU knows them: the culling shaders write them into
// the draw's record, and the CPU's copy says 0. Those reads become the built-ins, which hold the values the draw really
// used; a tree drawn with base instance 0 instead took another instance's transform for that frame.
void use_base_builtins(std::vector<u32>& w, const Shader& sh) {
    enum { OpCapability = 17, OpEntryPoint = 15, OpTypeInt = 21, OpTypePointer = 32, OpConstant = 43, OpFunction = 54, OpVariable = 59, OpLoad = 61,
           OpStore = 62, OpAccessChain = 65, OpInBoundsAccessChain = 66, OpDecorate = 71, OpBitcast = 124 };
    enum { DecBuiltIn = 11, DecBinding = 33, BuiltInBaseVertex = 4424, BuiltInBaseInstance = 4425, CapDrawParameters = 4427, Input = 1 };
    u32 c0_binding = UINT32_MAX;
    for (const Binding& b : sh.bindings)
        if (b.kind == BKind::Drv0) c0_binding = b.binding;
    if (c0_binding == UINT32_MAX || w[1] < 0x10300) return;   // (the built-ins are core from SPIR-V 1.3)

    u32 c0 = 0, int_ty = 0, ptr_ty = 0, vars[2] = {};
    bool has_cap = false;
    std::unordered_map<u32, u32> consts, chains;   // chains: access chain id -> 0 base vertex, 1 base instance
    for (size_t i = 5; i < w.size(); i += w[i] >> 16) {
        const u32 op = w[i] & 0xffff, n = w[i] >> 16;
        if (!n || i + n > w.size()) return;
        if (op == OpCapability && w[i + 1] == CapDrawParameters) has_cap = true;
        else if (op == OpDecorate && w[i + 2] == DecBinding && w[i + 3] == c0_binding) c0 = w[i + 1];
        else if (op == OpDecorate && w[i + 2] == DecBuiltIn && (w[i + 3] == BuiltInBaseVertex || w[i + 3] == BuiltInBaseInstance)) vars[w[i + 3] - BuiltInBaseVertex] = w[i + 1];
        else if (op == OpTypeInt && w[i + 2] == 32 && w[i + 3] == 1) int_ty = w[i + 1];
        else if (op == OpTypePointer && w[i + 2] == Input && w[i + 3] == int_ty && int_ty) ptr_ty = w[i + 1];
        else if (op == OpConstant && n == 4) consts[w[i + 2]] = w[i + 3];
        else if ((op == OpAccessChain || op == OpInBoundsAccessChain) && n == 7 && w[i + 3] == c0) {
            const auto a = consts.find(w[i + 5]), b = consts.find(w[i + 6]);
            if (a != consts.end() && b != consts.end() && a->second == 100 && b->second < 2) chains[w[i + 2]] = b->second;
        }
    }
    if (chains.empty() || !int_ty) return;
    for (size_t i = 5; i < w.size(); i += w[i] >> 16)
        if ((w[i] & 0xffff) == OpStore && chains.count(w[i + 1])) return;

    u32 bound = w[3];
    const bool want[2] = {std::any_of(chains.begin(), chains.end(), [](auto& c) { return c.second == 0; }),
                          std::any_of(chains.begin(), chains.end(), [](auto& c) { return c.second == 1; })};
    u32 added[2] = {};
    for (int k = 0; k < 2; k++)
        if (want[k] && !vars[k]) vars[k] = added[k] = bound++;
    const bool need_ptr = (added[0] || added[1]) && !ptr_ty;
    if (need_ptr) ptr_ty = bound++;

    std::vector<u32> o(w.begin(), w.begin() + 5);
    o.reserve(w.size() + 32);
    bool cap_done = has_cap, decs_done = false, vars_done = false;
    for (size_t i = 5; i < w.size(); i += w[i] >> 16) {
        const u32 op = w[i] & 0xffff, n = w[i] >> 16;
        if (!cap_done && op != OpCapability) {
            o.insert(o.end(), {OpCapability | 2u << 16, CapDrawParameters});
            cap_done = true;
        }
        // annotations come right before the first type, constant or global
        if (!decs_done && ((op >= 19 && op <= 39) || (op >= 41 && op <= 52) || op == OpVariable)) {
            for (int k = 0; k < 2; k++)
                if (added[k]) o.insert(o.end(), {OpDecorate | 4u << 16, added[k], DecBuiltIn, static_cast<u32>(BuiltInBaseVertex + k)});
            decs_done = true;
        }
        if (!vars_done && op == OpFunction) {
            if (need_ptr) o.insert(o.end(), {OpTypePointer | 4u << 16, ptr_ty, Input, int_ty});
            for (int k = 0; k < 2; k++)
                if (added[k]) o.insert(o.end(), {OpVariable | 4u << 16, ptr_ty, added[k], Input});
            vars_done = true;
        }
        if ((op == OpAccessChain || op == OpInBoundsAccessChain) && chains.count(w[i + 2])) continue;
        if (op == OpLoad && chains.count(w[i + 3])) {
            const u32 var = vars[chains[w[i + 3]]];
            if (w[i + 1] == int_ty) {
                o.insert(o.end(), {OpLoad | 4u << 16, int_ty, w[i + 2], var});
            } else {
                const u32 tmp = bound++;
                o.insert(o.end(), {OpLoad | 4u << 16, int_ty, tmp, var, OpBitcast | 4u << 16, w[i + 1], w[i + 2], tmp});
            }
            continue;
        }
        if (op == OpEntryPoint) {
            const u32 extra = (added[0] ? 1 : 0) + (added[1] ? 1 : 0);
            o.push_back(op | (n + extra) << 16);
            o.insert(o.end(), w.begin() + i + 1, w.begin() + i + n);
            for (int k = 0; k < 2; k++)
                if (added[k]) o.push_back(added[k]);
            continue;
        }
        o.insert(o.end(), w.begin() + i, w.begin() + i + n);
    }
    o[3] = bound;
    w = std::move(o);
}

// Bytes of Workgroup (shared) variables the SPIR-V declares: scalars, vectors, arrays and structs of them.
u64 workgroup_bytes(const std::vector<u32>& w) {
    std::unordered_map<u32, u64> size;    // type id -> bytes
    std::unordered_map<u32, u64> konst;   // constant id -> value
    std::unordered_map<u32, u32> ptr;     // Workgroup pointer type id -> pointee type id
    u64 total = 0;
    for (size_t i = 5; i < w.size();) {
        const u32 n = w[i] >> 16, op = w[i] & 0xffff;
        if (!n || i + n > w.size()) break;
        const u32* a = &w[i + 1];
        switch (op) {
        case 21: case 22: size[a[0]] = a[1] / 8; break;                              // OpTypeInt / OpTypeFloat
        case 20: size[a[0]] = 4; break;                                              // OpTypeBool
        case 23: size[a[0]] = size[a[1]] * a[2]; break;                              // OpTypeVector
        case 28: size[a[0]] = size[a[1]] * konst[a[2]]; break;                       // OpTypeArray
        case 30: { u64 s = 0; for (u32 k = 1; k < n - 1; k++) s += size[a[k]]; size[a[0]] = s; break; }   // OpTypeStruct
        case 43: konst[a[1]] = a[2]; break;                                          // OpConstant
        case 32: if (a[1] == 4) ptr[a[0]] = a[2]; break;                             // OpTypePointer Workgroup
        case 59: if (a[2] == 4 && ptr.count(a[0])) total += size[ptr[a[0]]]; break;  // OpVariable Workgroup
        }
        i += n;
    }
    return total;
}

// Shrinks the one Workgroup array a shader declares (OpVariable of an OpTypeArray of 32-bit words with a constant length)
// to `cap` bytes: a new OpConstant for the length goes in before the array type, so other uses of the old one stay.
// The size it had, or 0 when the shader has no such array (left as it is).
u64 shrink_workgroup(std::vector<u32>& w, u64 cap) {
    std::unordered_map<u32, u32> konst, int_width, ptr;   // constant -> value, int type -> width, Workgroup pointer -> type
    struct Arr { size_t at; u32 elem, len; };
    std::unordered_map<u32, Arr> arrays;
    u32 target = 0;
    for (size_t i = 5; i < w.size();) {
        const u32 n = w[i] >> 16, op = w[i] & 0xffff;
        if (!n || i + n > w.size()) return 0;
        const u32* a = &w[i + 1];
        if (op == 21) int_width[a[0]] = a[1];
        else if (op == 43) konst[a[1]] = a[2];
        else if (op == 28) arrays[a[0]] = {i, a[1], a[2]};
        else if (op == 32 && a[1] == 4) ptr[a[0]] = a[2];
        else if (op == 59 && a[2] == 4 && ptr.count(a[0])) {
            if (target) return 0;   // (more than one shared variable: not this kind of shader)
            target = ptr[a[0]];
        }
        i += n;
    }
    const auto it = arrays.find(target);
    if (it == arrays.end() || int_width[it->second.elem] != 32 || !konst.count(it->second.len)) return 0;
    const u64 had = static_cast<u64>(konst[it->second.len]) * 4;
    if (had <= cap) return had;
    const u32 id = w[3]++;   // a new id: the bound grows by one
    const u32 inst[4] = {(4u << 16) | 43u, it->second.elem, id, static_cast<u32>(cap / 4)};   // OpConstant %elem %id cap/4
    const size_t at = it->second.at;
    w[at + 3] = id;          // OpTypeArray %result %elem %length -> the new length
    w.insert(w.begin() + static_cast<std::ptrdiff_t>(at), inst, inst + 4);
    return had;
}

// The storage buffer offsets (shaders.h DeltaBase): each access chain into a storage buffer the host binds from guest
// memory gets its stage's nibble for that buffer added to the element index, from a push constant block (a new one,
// or the mesh shaders' own with a member added). The bindings it covers (indexes in sh.bindings) go to
// sh.delta_binding. False: left as it was.
bool add_ssbo_deltas(std::vector<u32>& w, Shader& sh) {
    enum : u32 { OpEntryPointK = 15, OpFunction = 54, OpLoad = 61, OpAccessChain = 65, OpInBoundsAccessChain = 66, OpMemberDecorate = 72,
                 OpIAdd = 128, OpShiftRightLogical = 194, OpShiftLeftLogical = 196, OpBitwiseAnd = 199,
                 DecArrayStride = 6, DecOffset = 35, ScPushConstant = 9 };
    std::unordered_map<u32, u32> bind_of, stride, konst, ptr_pointee, var_ptr, array_elem, max_off;
    std::unordered_map<u32, std::vector<u32>> structs;   // struct id -> member types
    u32 uint_ty = 0, pc_var = 0;
    size_t first_decl = 0, first_fn = 0;
    for (size_t i = 5; i < w.size();) {
        const u32 op = w[i] & 0xffff, n = w[i] >> 16;
        if (!n || i + n > w.size()) return false;
        const bool decl = (op >= 19 && op <= 39) || (op >= 41 && op <= 52) || op == OpVariable;
        if (decl && !first_decl) first_decl = i;
        if (op == OpDecorate && n >= 4 && w[i + 2] == DecBinding) bind_of[w[i + 1]] = w[i + 3];
        else if (op == OpDecorate && n >= 4 && w[i + 2] == DecArrayStride) stride[w[i + 1]] = w[i + 3];
        else if (op == OpMemberDecorate && n >= 5 && w[i + 3] == DecOffset) max_off[w[i + 1]] = std::max(max_off[w[i + 1]], w[i + 4]);
        else if (op == OpTypeInt && w[i + 2] == 32 && w[i + 3] == 0) uint_ty = w[i + 1];
        else if (op == OpTypeArray || op == OpTypeRuntimeArray) array_elem[w[i + 1]] = w[i + 2];
        else if (op == OpTypeStruct) structs[w[i + 1]].assign(w.begin() + static_cast<ptrdiff_t>(i + 2), w.begin() + static_cast<ptrdiff_t>(i + n));
        else if (op == OpTypePointer) ptr_pointee[w[i + 1]] = w[i + 3];
        else if (op == OpConstant && n == 4) konst[w[i + 2]] = w[i + 3];
        else if (op == OpVariable && !first_fn) {
            var_ptr[w[i + 2]] = w[i + 1];
            if (w[i + 3] == ScPushConstant) pc_var = w[i + 2];
        } else if (op == OpFunction && !first_fn) first_fn = i;
        i += n;
    }
    if (!first_decl || !first_fn) return false;
    // the storage buffers: var -> (nibble index, log2 of the elements in 16 bytes)
    std::unordered_map<u32, std::pair<u32, u32>> ssbo;
    u32 k = 0;
    for (size_t bi = 0; bi < sh.bindings.size(); bi++) {
        const Binding& b = sh.bindings[bi];
        if (b.type != VK_DESCRIPTOR_TYPE_STORAGE_BUFFER || b.count != 1) continue;
        if (b.kind != BKind::Ssbo && b.kind != BKind::SsboUbo && b.kind != BKind::Ubo && b.kind != BKind::UboPtr) continue;
        u32 var = 0;
        for (auto& [id, bn] : bind_of)
            if (bn == b.binding && var_ptr.count(id)) var = id;
        if (!var) continue;
        const auto st = structs.find(ptr_pointee[var_ptr[var]]);
        if (st == structs.end() || st->second.size() != 1 || !array_elem.count(st->second[0])) continue;
        const u32 s = stride[st->second[0]];
        if (s != 4 && s != 8 && s != 16) continue;
        if (k == 16) {
            LOG("[RED] nvk: shader %s: more than 16 storage buffers, binding %u left without its offset", sh.name, b.binding);
            continue;
        }
        ssbo[var] = {k, s == 4 ? 2u : s == 8 ? 1u : 0u};
        sh.delta_binding[k++] = static_cast<u32>(bi);
    }
    if (ssbo.empty()) return false;
    // the push constant block: the shader's own (a member added after its last, which ends before DeltaBase) or a new one
    u32 pc_struct = 0, member = 0;
    if (pc_var) {
        pc_struct = ptr_pointee[var_ptr[pc_var]];
        if (!structs.count(pc_struct) || max_off[pc_struct] + 16 > DeltaBase) {
            LOG("[RED] nvk: shader %s: its push constants overlap the storage buffer offsets: left as it is", sh.name);
            return false;
        }
        member = static_cast<u32>(structs[pc_struct].size());
    }
    u32 bound = w[3];
    const u32 new_uint = uint_ty ? 0 : bound++;
    if (new_uint) uint_ty = new_uint;
    const u32 c_len = bound++, arr = bound++, ptr_u = bound++;
    const bool new_block = !pc_var;
    u32 pc_ptr = 0;
    if (new_block) pc_struct = bound++, pc_ptr = bound++, pc_var = bound++;
    std::unordered_map<u32, u32> cid;   // uint constant value -> id
    const auto want = [&](u32 v) { if (!cid.count(v)) cid[v] = bound++; };
    const u32 st = sh.stage;
    want(member), want(2 * st), want(2 * st + 1), want(15), want(1), want(2);
    for (u32 i = 0; i < 8; i++) want(4 * i);

    std::vector<u32> o(w.begin(), w.begin() + 5);
    o.reserve(w.size() + w.size() / 4);
    const auto emit = [&](std::initializer_list<u32> ins) { o.insert(o.end(), ins); };
    const auto head = [](u32 op, u32 n) { return op | n << 16; };
    const auto types_before = [&] {   // what the block's new member needs
        if (new_uint) emit({head(OpTypeInt, 4), new_uint, 32, 0});
        emit({head(OpConstant, 4), uint_ty, c_len, 2 * SH_N});
        emit({head(OpTypeArray, 4), arr, uint_ty, c_len});
    };
    u32 patched = 0;
    for (size_t i = 5; i < w.size();) {
        const u32 op = w[i] & 0xffff, n = w[i] >> 16;
        if (i == first_decl) {
            emit({head(OpDecorate, 4), arr, DecArrayStride, 4});
            emit({head(OpMemberDecorate, 5), pc_struct, member, DecOffset, DeltaBase});
            if (new_block) emit({head(OpDecorate, 3), pc_struct, DecBlock});
        }
        if (!new_block && op == OpTypeStruct && w[i + 1] == pc_struct) {
            types_before();
            o.push_back(head(OpTypeStruct, n + 1));
            o.insert(o.end(), w.begin() + static_cast<ptrdiff_t>(i + 1), w.begin() + static_cast<ptrdiff_t>(i + n));
            o.push_back(arr);
            i += n;
            continue;
        }
        if (i == first_fn) {
            if (new_block) {
                types_before();
                emit({head(OpTypeStruct, 3), pc_struct, arr});
                emit({head(OpTypePointer, 4), pc_ptr, ScPushConstant, pc_struct});
                emit({head(OpVariable, 4), pc_ptr, pc_var, ScPushConstant});
            }
            emit({head(OpTypePointer, 4), ptr_u, ScPushConstant, uint_ty});
            for (auto& [v, id] : cid) emit({head(OpConstant, 4), uint_ty, id, v});
        }
        if (op == OpEntryPointK && new_block && w[1] >= 0x10400) {   // (from SPIR-V 1.4 every global it uses is listed)
            o.push_back(head(op, n + 1));
            o.insert(o.end(), w.begin() + static_cast<ptrdiff_t>(i + 1), w.begin() + static_cast<ptrdiff_t>(i + n));
            o.push_back(pc_var);
            i += n;
            continue;
        }
        if ((op == OpAccessChain || op == OpInBoundsAccessChain) && n >= 6 && ssbo.count(w[i + 3]) && konst.count(w[i + 4]) && konst[w[i + 4]] == 0) {
            const auto [nib, mul] = ssbo[w[i + 3]];
            const u32 p = bound++, v = bound++, sh_ = bound++, nb = bound++, d = mul ? bound++ : nb, idx = bound++;
            emit({head(OpAccessChain, 6), ptr_u, p, pc_var, cid[member], cid[2 * st + nib / 8]});
            emit({head(OpLoad, 4), uint_ty, v, p});
            emit({head(OpShiftRightLogical, 5), uint_ty, sh_, v, cid[4 * (nib % 8)]});
            emit({head(OpBitwiseAnd, 5), uint_ty, nb, sh_, cid[15]});
            if (mul) emit({head(OpShiftLeftLogical, 5), uint_ty, d, nb, cid[mul]});
            emit({head(OpIAdd, 5), uint_ty, idx, w[i + 5], d});
            const size_t at = o.size();
            o.insert(o.end(), w.begin() + static_cast<ptrdiff_t>(i), w.begin() + static_cast<ptrdiff_t>(i + n));
            o[at + 5] = idx;
            patched++;
            i += n;
            continue;
        }
        o.insert(o.end(), w.begin() + static_cast<ptrdiff_t>(i), w.begin() + static_cast<ptrdiff_t>(i + n));
        i += n;
    }
    if (!patched) {
        for (u32 j = 0; j < k; j++) sh.delta_binding[j] = 0;
        return false;
    }
    o[3] = bound;
    w = std::move(o);
    sh.delta_n = static_cast<u8>(k);
    return true;
}

// Workgroup barriers some threads may never reach: the game's compute shaders start with "if (id >= count) return;" and
// then wait at barriers in a loop. NVIDIA lets the others through; an Intel GPU waits for the returned threads forever
// (the handheld's device loss on a8310cf7..., dispatched as one workgroup of 64 with a count below 64). As Ryujinx does
// on Intel, those barriers become memory barriers: after a return in the function or in a function of its own
// (mode 2: also any inside a branch or loop). The number changed.
u32 drop_divergent_barriers(std::vector<u32>& w, int mode) {
    enum : u32 { OpFunctionK = 54, OpFunctionEnd = 56, OpLabel = 248, OpLoopMerge = 246, OpSelectionMerge = 247, OpReturn = 253,
                 OpReturnValue = 254, OpKill = 252, OpTerminateInvocation = 4416, OpControlBarrier = 224, OpMemoryBarrier = 225 };
    u32 entry_fn = 0, changed = 0;
    bool returned = false, is_entry = false;
    std::vector<u32> merges;
    for (size_t i = 5; i < w.size();) {
        const u32 op = w[i] & 0xffff, n = w[i] >> 16;
        if (!n || i + n > w.size()) return changed;
        if (op == 15 && !entry_fn) entry_fn = w[i + 2];   // OpEntryPoint
        else if (op == OpFunctionK) returned = false, is_entry = w[i + 2] == entry_fn, merges.clear();
        else if (op == OpReturn || op == OpReturnValue || op == OpKill || op == OpTerminateInvocation) returned = true;
        else if (op == OpSelectionMerge || op == OpLoopMerge) merges.push_back(w[i + 1]);
        else if (op == OpLabel) merges.erase(std::remove(merges.begin(), merges.end(), w[i + 1]), merges.end());
        else if (op == OpControlBarrier && n == 4 && (returned || !is_entry || (mode >= 2 && !merges.empty()))) {
            const u32 mem = w[i + 2], sem = w[i + 3];
            w[i] = OpMemoryBarrier | 3u << 16, w[i + 1] = mem, w[i + 2] = sem;
            w.erase(w.begin() + static_cast<ptrdiff_t>(i + 3));
            changed++;
            continue;   // (read again as the 3-word memory barrier it is now, then passed)
        }
        i += n;
    }
    return changed;
}

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
        sh.kinds |= 1u << static_cast<u32>(b.kind);
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
    // On Intel, compute / task / mesh shaders with workgroup barriers load <file>.bar.spv (tools/nvn_barfix.py): threads past the
    // early "return" stay for the barriers with their writes skipped, as Intel needs (NVIDIA lets them leave).
    // HOST_NVN_BARSPV=0: never, 1: on any GPU (to test on the desktop).
    Device& c = ctx();
    const bool intel = c.props.vendorID == 0x8086;
    static const int barspv = getenv("HOST_NVN_BARSPV") ? atoi(getenv("HOST_NVN_BARSPV")) : -1;
    const bool want_bar = barspv > 0 || (barspv < 0 && intel);
    bool bar = false;
    const auto bar_of = [&](const std::string& f) {
        const std::string b = f.substr(0, f.size() - 4) + ".bar.spv";
        if (want_bar && std::ifstream(b).good()) file = b, bar = true;
    };
    if (row.stage == SH_TS || row.stage == SH_MS) {
        const std::string flat = g_dir + "\\" + sh->name + ".meshflat.spv", mesh = g_dir + "\\" + sh->name + ".mesh.spv";
        file = std::ifstream(flat).good() ? flat : mesh;
        bar_of(file);
    } else {
        if (row.stage == SH_CS) bar_of(file);
        const std::string fix = g_dir + "\\" + sh->name + ".fix.spv";
        if (!bar && !getenv("HOST_NVN_NO_SPVFIX") && std::ifstream(fix).good())
            file = fix;   // one the driver miscompiles, restructured by tools/nvn_spvfix.py (HOST_NVN_NO_SPVFIX=1: the translation as it is)
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
    // HOST_NVN_BASE_BUILTINS=0: the shaders as translated
    static const bool base_builtins = !getenv("HOST_NVN_BASE_BUILTINS") || atoi(getenv("HOST_NVN_BASE_BUILTINS")) != 0;
    if (row.stage == SH_VS && base_builtins) use_base_builtins(words, *sh);
    if ((row.stage == SH_TS || row.stage == SH_MS) && !ctx().feat.mesh_shader) sh->unsupported = "no mesh shaders on this GPU";
    if (bar) {
        static std::atomic<int> logged{0};
        if (logged++ == 0) LOG("nvk: shaders with workgroup barriers loaded from their .bar.spv (tools/nvn_barfix.py; HOST_NVN_BARSPV=0: not), e.g. %s", sh->name);
    }
    // The fallback for a shader without a .bar.spv: barriers after an early return hang Intel GPUs, so they become memory barriers
    // (drop_divergent_barriers; their results can be wrong: the black screen of the first handheld runs). HOST_NVN_BARRIER_FIX=0:
    // as translated, 1: after a return, 2 (Intel's default, as Ryujinx): also inside branches and loops, which is where the .fix.spv
    // restructuring (tools/nvn_spvfix.py) puts the early return of a8310cf7 and its kin.
    static const int barrier_fix = getenv("HOST_NVN_BARRIER_FIX") ? atoi(getenv("HOST_NVN_BARRIER_FIX")) : intel ? 2 : 0;
    if (barrier_fix && !bar && (row.stage == SH_CS || row.stage == SH_TS || row.stage == SH_MS) && !sh->unsupported) {
        if (const u32 n = drop_divergent_barriers(words, barrier_fix)) {
            static std::atomic<int> logged{0};
            if (logged++ < 5) LOG("nvk: shader %s: %u workgroup barriers some threads may not reach made memory barriers (they hang Intel GPUs)", sh->name, n);
        }
    }
    // Compute shaders that declare all of the GPU's shared memory (~170 with 48 KB, one array of 12288 words indexed by
    // (workgroup * 64 + thread) * 48 bytes): the first guess at the Intel handheld's device loss, which was the barriers.
    // HOST_NVN_SHARED_CAP_KB=<n>: cut to n KB on Intel (HOST_NVN_SHARED_CAP_ANY: any GPU); HOST_NVN_SHARED_SKIP=1: skipped.
    if (row.stage == SH_CS && !sh->unsupported && (intel || getenv("HOST_NVN_SHARED_CAP_ANY"))) {
        const u64 limit = c.props.limits.maxComputeSharedMemorySize;
        const char* ce = getenv("HOST_NVN_SHARED_CAP_KB");
        const u64 cap = (ce && *ce ? std::strtoull(ce, nullptr, 0) : 0) << 10;
        if ((cap || getenv("HOST_NVN_SHARED_SKIP")) && workgroup_bytes(words) >= limit) {
            if (getenv("HOST_NVN_SHARED_SKIP")) {
                sh->unsupported = "skipped (declares all shared memory: hangs Intel GPUs; HOST_NVN_SHARED_SKIP)";
            } else if (const u64 had = shrink_workgroup(words, std::min(cap, limit - 1024)); had) {
                static std::atomic<int> logged{0};
                if (logged++ < 5)
                    LOG("nvk: compute shader %s: shared memory cut from %llu to %llu bytes (HOST_NVN_SHARED_CAP_KB)", sh->name,
                        static_cast<unsigned long long>(had), static_cast<unsigned long long>(workgroup_bytes(words)));
            } else {
                sh->unsupported = "skipped (declares all shared memory, not in a form that can be cut: hangs Intel GPUs)";
            }
        }
    }
    if (ssbo_deltas() && !sh->unsupported && add_ssbo_deltas(words, *sh)) {
        static std::atomic<int> logged{0};
        if (logged++ < 3) LOG("nvk: shader %s: %u storage buffers read with offsets from push constants (aligned to %llu bytes)", sh->name,
                              sh->delta_n, static_cast<unsigned long long>(ssbo_align()));
    }
    // HOST_NVN_DUMP_PATCHED=<folder>: the modules as given to the driver written there (to check with spirv-val)
    if (const char* d = getenv("HOST_NVN_DUMP_PATCHED"))
        std::ofstream(std::string(d) + "\\" + sh->name + ".spv", std::ios::binary).write(reinterpret_cast<const char*>(words.data()), static_cast<std::streamsize>(words.size() * 4));
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
    // Shaders the GPU faults on: the free-roam device loss ("instruction pointer invalid", 11-08 and 11-11) came, in every
    // crash with HOST_NVN_CHECKPOINTS=1, with vs 7bc86687f83f86ab + ps 443a31bcc678184a the oldest unfinished draw (a small
    // instanced object, 18 indices, drawn into a 960x540 target). Skipping it was the only change that got past the crash
    // spot; a fresh pipeline cache, a spirv-opt rewrite, no pipeline destruction, and blank cube / depth inputs did not, and
    // blank vertex-stage textures only delayed it (80 s -> 17 min): data dependent. A sibling vs (a7f9fb429f44f663, same
    // draw state) works. Skipped until the cause is found; HOST_NVN_KNOWN_BAD=0 draws it.
    static const bool skip_bad = !getenv("HOST_NVN_KNOWN_BAD") || atoi(getenv("HOST_NVN_KNOWN_BAD")) != 0;
    if (skip_bad && !sh->unsupported && std::strcmp(sh->name, "7bc86687f83f86ab") == 0) {
        sh->unsupported = "skipped (the GPU faults on it: HOST_NVN_KNOWN_BAD=0 draws it)";
        LOG("nvk: shader %s skipped: the GPU faults on it (HOST_NVN_KNOWN_BAD=0 draws it)", sh->name);
    }
    return sh;
}

}  // namespace

std::string shader_folder() { return shader_dir(); }

u64 ssbo_align() {
    static const u64 a = [] {
        u64 v = ctx().props.limits.minStorageBufferOffsetAlignment;
        if (const char* e = getenv("HOST_NVN_SSBO_ALIGN"); e && *e) v = std::max<u64>(v, std::strtoull(e, nullptr, 0));
        return v;
    }();
    return a;
}

bool ssbo_deltas() {
    static const bool on = ssbo_align() > 16 && ssbo_align() <= 256 && !(getenv("HOST_NVN_SSBO_DELTA") && atoi(getenv("HOST_NVN_SSBO_DELTA")) == 0);
    return on;
}

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
