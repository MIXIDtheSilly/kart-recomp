// Native NVN: programs (shader stage sets) and their shader keys.
//
// A program is the game's NVNshaderData per stage: `control` (the shader's control section, guest memory) and `data` (the code area, a GPU address). The renderer finds the
// translated shader by KEY: the first 8 bytes of sha256(control section prefix + code area). Offline shader builds are keyed by it, so the format must match nxnative's exactly:
//   NVN (Switch 1):  +0x704 of the control section is the code area's length; +0x20 holds the section's own length. Sections of at least 0x880 bytes hash the first 0x880;
//                    shorter ones (SDK 10's 3D World has 0x850-byte ones followed by other data) hash only their own length (shader_split.py ctl_len).
//   NVN2 (Switch 2, magic 0x98761234, version 0xc9 at +4): the code area's length at +0x44; the key takes the first 0x48 bytes (through the shader's own hash at +0x20 and the
//                    code lengths), with the 4 bytes at +0x30 hashed as zero: Donkey Kong Bananza's BNSH files hold the section's length there (0xd8, 0xd0 in effects), agl's BAHS 0, also in memory.
// followed by the code area (ncode bytes) when it is mapped.
#include <unordered_map>

#include "nvn_int.h"

namespace nvn {

// ---------- SHA-256 (FIPS 180-4) ----------
namespace {
struct Sha256 {
    uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    uint8_t buf[64]; size_t n = 0; uint64_t total = 0;
    static uint32_t rotr(uint32_t x, int k) { return x >> k | x << (32 - k); }
    void block(const uint8_t* p) {
        static const uint32_t K[64] = {
            0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
            0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
            0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
            0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
        uint32_t w[64];
        for (int i = 0; i < 16; i++) w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 | (uint32_t)p[4 * i + 2] << 8 | p[4 * i + 3];
        for (int i = 16; i < 64; i++) {
            const uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3), s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 64; i++) {
            const uint32_t t1 = hh + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + ((e & f) ^ (~e & g)) + K[i] + w[i];
            const uint32_t t2 = (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
            hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
    }
    void update(const void* data, size_t len) {
        const uint8_t* p = static_cast<const uint8_t*>(data); total += len;
        if (n) { const size_t k = std::min(len, 64 - n); memcpy(buf + n, p, k); n += k; p += k; len -= k; if (n == 64) { block(buf); n = 0; } }
        for (; len >= 64; p += 64, len -= 64) block(p);
        if (len) { memcpy(buf, p, len); n = len; }
    }
    void final(uint8_t out[32]) {
        const uint64_t bits = total * 8; const uint8_t one = 0x80, zero = 0;
        update(&one, 1); while (n != 56) update(&zero, 1);
        uint8_t len[8]; for (int i = 0; i < 8; i++) len[i] = (uint8_t)(bits >> (56 - 8 * i));
        update(len, 8);
        for (int i = 0; i < 8; i++) for (int k = 0; k < 4; k++) out[4 * i + k] = (uint8_t)(h[i] >> (24 - 8 * k));
    }
};

constexpr uint32_t NVN2_KEY_CTL = 0x48;
bool nvn2_ctl(const uint8_t* c) { uint32_t magic, ver; memcpy(&magic, c, 4); memcpy(&ver, c + 4, 4); return magic == 0x98761234u && ver == 0xc9; }
// A control section's bytes in the shader key and its code area's length (NVN or NVN2)
void ctl_lens(const uint8_t* c, uint32_t* nctl, uint32_t* ncode) {
    uint32_t own;
    if (nvn2_ctl(c)) { memcpy(ncode, c + 0x44, 4); *nctl = NVN2_KEY_CTL; return; }
    memcpy(ncode, c + 0x704, 4);
    memcpy(&own, c + 0x20, 4);
    *nctl = own >= 0x760 && own < 0x880 ? own : 0x880;
}
// mapped: computed only when the code area is readable (at SetShaders: Mario sets some programs with an unmapped address first and fills it in before the first draw).
// sd->control and sd->data are guest addresses. The control section is only guaranteed valid during SetShaders (NVN lets the caller free it once it returns, and Unity does).
bool keys_hash(const NVNshaderData& sd, bool mapped, uint8_t out[8]) {
    const u64 control = (u64)(uintptr_t)sd.control;
    if (!control) return false;
    const uint8_t* ctl = static_cast<const uint8_t*>(hp(control));
    uint32_t ncode = 0, nctl;
    ctl_lens(ctl, &nctl, &ncode);
    bool code = sd.data && ncode && ncode < (1u << 24);
    const uint8_t* cp = nullptr;
    if (code) { cp = host_readable(sd.data, ncode) ? gpu_ptr(sd.data, ncode) : nullptr; if (!cp) code = false; }
    if (mapped && !code) return false;
    Sha256 h;
    if (nvn2_ctl(ctl)) {
        uint8_t c[NVN2_KEY_CTL]; memcpy(c, ctl, NVN2_KEY_CTL); memset(c + 0x30, 0, 4);
        h.update(c, NVN2_KEY_CTL);
    } else h.update(ctl, nctl);
    if (code) h.update(cp, ncode);
    uint8_t md[32]; h.final(md); memcpy(out, md, 8);
    return true;
}

// a program's per-stage keys (side table: program object address -> keys; bit i of ok = stage i computed). Not in the object: the game's real NVNprogram may be smaller than prog_t's neighbours allow
struct prog_keys_t { uint8_t key[6][8]; uint8_t ok; };
std::mutex g_pk_mu;
std::unordered_map<const void*, prog_keys_t*> g_pk;
prog_keys_t* pk_get(const void* p) {
    std::scoped_lock lk{g_pk_mu};
    prog_keys_t*& k = g_pk[p];
    if (!k) k = new prog_keys_t{};
    return k;
}
}  // namespace

void pk_drop(const void* p) {
    std::scoped_lock lk{g_pk_mu};
    if (auto it = g_pk.find(p); it != g_pk.end()) { delete it->second; g_pk.erase(it); }
}
bool prog_key(const prog_t* p, unsigned st, uint8_t key[8]) {
    if (st >= p->nstage) return false;
    prog_keys_t* k = pk_get(p);
    std::scoped_lock lk{g_pk_mu};
    if (!(k->ok >> st & 1)) { keys_hash(p->sd[st], false, k->key[st]); k->ok |= (uint8_t)(1u << st); }
    memcpy(key, k->key[st], 8);
    return true;
}

NVN(nvnProgramInitialize, NVNboolean, NVNprogram* p, NVNdevice* d) {
    (void)d; memset(p, 0, sizeof(prog_t)); ((prog_t*)p)->magic = NVN_MAGIC('P', 'R', 'O', 'G'); return 1;
}
NVN(nvnProgramFinalize, void, NVNprogram* p) {
    { std::scoped_lock lk{g_sub_mu}; rp_prog_forget((const prog_t*)p); }
    rp_prog_release((prog_t*)p); ((prog_t*)p)->magic = 0; pk_drop(p);
}
NVN(nvnProgramSetShaders, NVNboolean, NVNprogram* p, int n, const NVNshaderData* sd) {
    prog_t* o = (prog_t*)p;
    if (n < 0 || n > 6) { LOG("[RED] nvn: ProgramSetShaders stage count %d", n); return 0; }
    rp_prog_release(o); o->nstage = n; memcpy(o->sd, sd, n * sizeof *sd); o->serial = new_serial();
    // The keys are computed here (see prog_key): the control section is only guaranteed valid during this call; the code area lives in a memory pool and stays valid while the program is in use
    prog_keys_t* k = pk_get(p);
    { std::scoped_lock lk{g_pk_mu}; k->ok = 0; for (int i = 0; i < n; i++) if (keys_hash(sd[i], true, k->key[i])) k->ok |= (uint8_t)(1u << i); }
    OLOG("ProgramSetShaders %p n=%d data0=%#llx", (void*)p, n, n ? (unsigned long long)sd[0].data : 0ull);
    rp_prog_prefetch(o);   // its shaders load in the background now, not at its first draw
    return 1;
}

// nvnProgramSetShadersExt (NVN 55): each stage is 0x18 bytes = NVNshaderData{data, control} + one 8-byte extension field.
// The host identifies stages by the control section and does not use the extension field: convert to plain NVNshaderData and hand it to SetShaders.
struct shader_data_ext { NVNbufferAddress data; const void* control; uint64_t ext; };
NVN_NOPROTO(nvnProgramSetShadersExt, NVNboolean, NVNprogram* p, int n, const shader_data_ext* sd) {
    if (n < 0 || n > 6) { LOG("[RED] nvn: ProgramSetShadersExt stage count %d", n); return 0; }
    NVNshaderData plain[6];
    for (int i = 0; i < n; i++) { plain[i].data = sd[i].data; plain[i].control = sd[i].control; if (sd[i].ext) ONCE_LOG("ProgramSetShadersExt extension field nonzero (%#llx): unused by the host", (unsigned long long)sd[i].ext); }
    return h_nvnProgramSetShaders(p, n, plain);
}

// NVN2 (Switch 2) creates programs from a builder: SetDefaults, SetDevice, SetShaders(count, NVNshaderData[]) and then nvnProgramInitializeFromBuilder. The stages are
// 16-byte {data, control} pairs as in NVN, so initializing from the builder is nvnProgramInitialize + nvnProgramSetShaders. The builder only keeps the caller's array (a host
// pointer into the game's memory), which the game keeps until it initializes the program (its stack frame), as other NVN builders keep their pointers
struct prog_builder_t { NVNdevice* dev; const NVNshaderData* sd; int n; };
NVN_NOPROTO(nvnProgramBuilderSetDefaults, void, void* b) { memset(b, 0, sizeof(prog_builder_t)); }
NVN_NOPROTO(nvnProgramBuilderSetDevice, void, void* b, NVNdevice* d) { ((prog_builder_t*)b)->dev = d; }
NVN_NOPROTO(nvnProgramBuilderSetShaders, void, void* b, int n, const NVNshaderData* sd) { ((prog_builder_t*)b)->n = n; ((prog_builder_t*)b)->sd = sd; }
NVN_NOPROTO(nvnProgramInitializeFromBuilder, NVNboolean, NVNprogram* p, const void* b) {
    const prog_builder_t* o = (const prog_builder_t*)b;
    return h_nvnProgramInitialize(p, o->dev) && (!o->n || h_nvnProgramSetShaders(p, o->n, o->sd));
}
NVN(nvnProgramSetDebugLabel, void, NVNprogram* p, const char* s) { (void)p; (void)s; }
// nvnProgramSetSampleShading (NVN 53+): whether the program's fragment shader runs once per sample of a multisampled target. The renderer decides per shader
// (a fragment function that reads the sample index runs per sample); the mode is not applied here
NVN_NOPROTO(nvnProgramSetSampleShading, NVNboolean, NVNprogram* p, int mode) { (void)p; ONCE_LOG("nvnProgramSetSampleShading mode %d: shading per sample only where the shader reads the sample index", mode); return 1; }

}  // namespace nvn
