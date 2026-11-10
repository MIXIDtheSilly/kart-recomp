// Native NVN: memory pools (including virtual / sparse pools), descriptor pools, buffers, and the services the renderer
// uses to resolve GPU addresses (gpu_ptr, sparse_*).
//
// GPU address == guest virtual address. A normal pool is guest memory the game gave us (pool_t::mem, a host pointer);
// its GPU address is ga(mem). A virtual pool (flags 0x200) is a GPU address range with no storage of its own: its range is
// reserved in the guest address space (never mapped), and MapVirtual shows pages of physical pools in it. The CPU never
// touches a virtual pool (nvnMemoryPoolMap returns NULL); the renderer resolves its addresses through the 4 KB page table below.
#include <map>

#include "nvn_int.h"

namespace nvn {

#define PL(p) ((pool_t*)(p))
#define CPL(p) ((const pool_t*)(p))

// ---------- Flushed ranges ----------
// What the CPU published with nvnMemoryPoolFlushMappedRange (every game), as disjoint [lo, hi) guest spans sorted by start (touching spans merge), dropped with their pool.
// Until the GPU first uses a texture it has not written it, so storage the game flushed before that first use is the texture's content: the renderer fills a new GPU texture
// from it once, as it does for a pool the CPU cannot access. Breath of the Wild copies its precomputed sky scattering tables into CPU_CACHED pools of their own with memcpy
// and flushes them; as GPU textures of zeros the sky had no scattered light and came out brownish grey instead of blue (nxnative issue #52).
namespace {
struct fl_span { u64 lo, hi; };
std::vector<fl_span> g_fl;
std::mutex g_fl_mu;
size_t fl_find(u64 a, bool touch) {   // the first span that ends after a (touch: or at a); g_fl_mu held
    size_t l = 0, h = g_fl.size();
    while (l < h) { size_t m = (l + h) / 2; if (g_fl[m].hi < a || (!touch && g_fl[m].hi == a)) l = m + 1; else h = m; }
    return l;
}
}  // namespace
void flushed_add(u64 a, u64 n) {
    if (!a || !n) return;
    u64 b = a + n;
    std::scoped_lock lk{g_fl_mu};
    size_t i = fl_find(a, true), j = i;
    if (i < g_fl.size() && g_fl[i].lo <= a && b <= g_fl[i].hi) return;   // already covered (a buffer flushed every frame)
    while (j < g_fl.size() && g_fl[j].lo <= b) { if (g_fl[j].lo < a) a = g_fl[j].lo; if (g_fl[j].hi > b) b = g_fl[j].hi; j++; }   // spans i .. j-1 overlap or touch [a, b): merged into one
    if (i == j) g_fl.insert(g_fl.begin() + i, fl_span{a, b});
    else { g_fl[i] = fl_span{a, b}; g_fl.erase(g_fl.begin() + i + 1, g_fl.begin() + j); }
}
void flushed_drop(u64 a, u64 n) {   // a pool goes: its part of every span (spans of neighbouring pools may have merged with it)
    const u64 b = a + n;
    std::scoped_lock lk{g_fl_mu};
    for (size_t i = fl_find(a, false); i < g_fl.size() && g_fl[i].lo < b;) {
        fl_span s = g_fl[i];
        if (s.lo < a && s.hi > b) {   // the pool lies inside the span: split it
            g_fl[i].hi = a; g_fl.insert(g_fl.begin() + i + 1, fl_span{b, s.hi}); break;
        }
        if (s.lo < a) { g_fl[i].hi = a; i++; }
        else if (s.hi > b) { g_fl[i].lo = b; break; }
        else g_fl.erase(g_fl.begin() + i);
    }
}
bool flushed(u64 a, u64 n) {   // whether the CPU flushed any of [a, a + n)
    std::scoped_lock lk{g_fl_mu};
    const size_t i = fl_find(a, false);
    return i < g_fl.size() && g_fl[i].lo < a + n;
}

// ---------- Sparse virtual pools ----------
// Monster Hunter Rise and Mario Kart World build GPU heaps from separately allocated physical pools mapped at offsets of their own into one huge virtual pool (this game: the
// terrain caches, 8192x8192 arrays of 68 layers in sparse pools, written by the CPU into physical pools, then mapped; no GPU copy writes them). Every virtual pool gets a reserved
// address range at Initialize, since the game may create its textures and buffers before mapping anything; its first mapping request then decides (MapVirtual):
// one that maps the whole pool from offset 0 keeps the alias (the range is given back; Super Mario Bros. Wonder's pool), anything else makes the pool sparse.
// OCTOPATH TRAVELER (Unreal Engine 4) gives each streamed texture a virtual pool of its own (0x20000 to 0x560000 bytes, flags 0x2a1), creates the texture in it, maps its mip
// tail from a 4 MB physical pool and later the higher mips from other pages of it, after writing their texels there on the CPU: aliased, the texture had no storage (created
// before any mapping) and the later mappings were refused. A pool larger than the application pool (app_pool 0x265100000) is sparse from the start.
// NVN maps at 4 KB: every request goes into a page table (the guest address of the physical page behind each virtual page, and the mapping generation that set it) which the
// renderer reads the sparse pool's textures and buffers through (sparse_copy, gpu_read, ...); unmapped pages read as 0.
namespace {
constexpr u64 AppPool = 0x265100000ull;
constexpr u64 VpRegionBase = 0x4000000000ull, VpRegionSize = 0x3000000000ull;   // guest address range the reservations come from (above the heap / alias / stack regions)
constexpr u64 VpAlign = 0x200000;
constexpr int VPT_LEAF = 512;   // a page-table leaf covers 2 MB
struct vpte { u64 phys; u64 gen; };   // phys: guest address of the physical page, 0 = unmapped
}  // namespace
struct vpool {
    pool_t* pool; u64 base, size, top;   // base: reserved guest address range (size bytes, rounded up to 2 MB); top: highest mapped end so far (logged per 128 MB)
    std::vector<vpte*> pt;
    std::unordered_map<u64, u64> rev;    // physical page -> the virtual page (offset) that mapped it last (see vpool_map)
    u64 stale_dropped = 0;               // virtual pages whose physical page was mapped again elsewhere (stats)
};
namespace {
std::map<u64, vpool*> g_vps;   // base -> pool, under g_vp_mu
std::mutex g_vp_mu;
std::mutex g_va_mu;
std::atomic<uint64_t> g_vp_gen{0};   // bumped per mapping request

// The pool whose reserved range holds a; caller holds g_vp_mu
vpool* vpool_find(u64 a) {
    auto it = g_vps.upper_bound(a);
    if (it == g_vps.begin()) return nullptr;
    --it;
    return a < it->second->base + it->second->size ? it->second : nullptr;
}
const vpte* vpt_get(const vpool* v, u64 o) { const vpte* l = v->pt[o >> 21]; return l ? &l[(o >> 12) & (VPT_LEAF - 1)] : nullptr; }
void vpt_set(vpool* v, u64 voff, u64 size, u64 phys) {
    const u64 gen = g_vp_gen.fetch_add(1) + 1;
    for (u64 o = 0; o < size; o += 0x1000) {
        vpte*& l = v->pt[(voff + o) >> 21];
        if (!l && !phys) continue;   // never mapped: stays unmapped
        if (!l) l = new vpte[VPT_LEAF]();
        l[((voff + o) >> 12) & (VPT_LEAF - 1)] = vpte{phys ? phys + o : 0, gen};
    }
}
// Guest address of the physical memory behind [off, off+n) of a sparse pool when it is one contiguous run of mapped pages, else 0. Caller holds g_vp_mu
u64 vrun_phys(const vpool* v, u64 off, u64 n) {
    u64 first = 0;
    const u64 pg0 = off & ~0xFFFull, end = off + (n ? n : 1);
    for (u64 at = pg0; at < end; at += 0x1000) {
        const vpte* e = at < v->pool->size ? vpt_get(v, at) : nullptr;
        if (!e || !e->phys) return 0;
        if (at == pg0) first = e->phys; else if (e->phys != first + (at - pg0)) return 0;
    }
    return first + (off & 0xFFF);
}
void vpool_drop(pool_t* o) {   // caller holds g_vp_mu
    vpool* v = o->vp; if (!v) return;
    rp_vpool_destroy(v->base);
    g_vps.erase(v->base);
    mem::set_state(v->base, v->size, mem::Free, mem::None);
    for (vpte* l : v->pt) delete[] l;
    delete v; o->vp = nullptr;
}
}  // namespace

void vpool_reserve(pool_t* o) {
    if (!(o->flags & 0x200)) return;
    const u64 len = (o->size + VpAlign - 1) & ~(VpAlign - 1);
    u64 base;
    {
        std::scoped_lock lk{g_va_mu};
        base = mem::find_free(VpRegionBase, VpRegionSize, len, VpAlign);
        if (base) mem::set_state(base, len, mem::Inaccessible, mem::None);   // reserved: nothing else is placed there; no backing
    }
    if (!base) { LOG("[RED] nvn: virtual pool %p: cannot reserve %#llx bytes of address space", (const void*)o, (unsigned long long)len); return; }
    vpool* v = new vpool{o, base, len, 0, {}};
    v->pt.assign((o->size + (2u << 20) - 1) >> 21, nullptr);
    {
        std::scoped_lock lk{g_vp_mu};
        g_vps.emplace(base, v);
        o->vp = v;
        o->vmode = o->size > AppPool ? 2 : 0;
    }
    o->mem = reinterpret_cast<uint8_t*>(g_membase + base);   // the pool's GPU addresses; never dereferenced
    rp_vpool_create(base, o->size);
    if (o->vmode == 2) LOG("nvn: virtual pool %p (%#llx bytes, flags %#x): sparse, own address range %#llx, physical pages are mapped into it", (const void*)o, (unsigned long long)o->size, o->flags, (unsigned long long)base);
}
void vpool_release(pool_t* o) { std::scoped_lock lk{g_vp_mu}; vpool_drop(o); }

// A mapping request {physical pool, physical offset, virtual offset, size, storage class}, 0x28 bytes. The pool pointer is the game's (a guest address).
namespace {
struct map_req { u64 phys; int64_t poff, voff; uint64_t size; int32_t storage_class, _pad; };
static_assert(sizeof(map_req) == 0x28, "NVNmappingRequest");

bool vpool_map(vpool* v, const map_req& r) {   // caller holds g_vp_mu; -> false on failure
    if (r.voff < 0 || (uint64_t)r.voff + r.size > v->pool->size) { LOG("[RED] nvn: virtual pool %p: request at %#llx+%#llx is beyond the pool's %#llx", (const void*)v->pool, (unsigned long long)r.voff, (unsigned long long)r.size, (unsigned long long)v->pool->size); return false; }
    if (((uint64_t)r.voff | r.size | (uint64_t)r.poff) & 0xFFF) { LOG("[RED] nvn: virtual pool %p: request %#llx+%#llx (physical offset %#llx) is not 4 KB aligned", (const void*)v->pool, (unsigned long long)r.voff, (unsigned long long)r.size, (unsigned long long)r.poff); return false; }
    const pool_t* ph = gp<pool_t>(r.phys);
    u64 phys = 0;
    if (ph) {
        if (!ph->mem || r.poff < 0 || (uint64_t)r.poff + r.size > ph->size) { LOG("[RED] nvn: virtual pool %p: physical pool %p (%#llx bytes, storage %p) cannot supply %#llx+%#llx", (const void*)v->pool, (const void*)ph, (unsigned long long)ph->size, (void*)ph->mem, (unsigned long long)r.poff, (unsigned long long)r.size); return false; }
        phys = ga(ph->mem) + (u64)r.poff;
    }
    // Which virtual page mapped each physical page last (sparse_stale). Mario Kart World's terrain caches recycle a
    // physical page by mapping it at another tile without unmapping the tile that had it; the page table keeps the old
    // tile as an alias, as the console's does (other pools alias on purpose: render targets sharing memory), and the
    // old tiles are told to the renderer, which drops such tiles from its sampled-only sparse images (sparse.cpp).
    std::vector<u64> stale;
    const u64 lo = (u64)r.voff, hi = lo + r.size;
    for (u64 o = 0; o < r.size; o += 0x1000) {
        const u64 vp = lo + o;
        if (const vpte* e = vpt_get(v, vp); e && e->phys) {   // this page's previous physical page is no longer here
            auto it = v->rev.find(e->phys);
            if (it != v->rev.end() && it->second == vp) v->rev.erase(it);
        }
        if (!phys) continue;
        auto [it, fresh] = v->rev.try_emplace(phys + o, vp);
        if (fresh) continue;
        const u64 old = it->second;
        it->second = vp;
        if (old >= lo && old < hi) continue;   // remapped by this same request
        if (const vpte* e = vpt_get(v, old); e && e->phys == phys + o) stale.push_back(old);
    }
    vpt_set(v, (u64)r.voff, r.size, phys);
    rp_vpool_map(v->base, (u64)r.voff, r.size, phys);
    if (!stale.empty()) {
        std::sort(stale.begin(), stale.end());
        for (size_t i = 0; i < stale.size();) {
            size_t j = i + 1;
            while (j < stale.size() && stale[j] == stale[j - 1] + 0x1000) j++;
            rp_vpool_map(v->base, stale[i], (j - i) * 0x1000, vpt_get(v, stale[i])->phys);   // (unchanged: the renderer looks again)
            i = j;
        }
        v->stale_dropped += stale.size();
    }
    const u64 end = (u64)r.voff + r.size;
    if (end >> 27 > v->top >> 27) LOG("nvn: virtual pool %p: mapped up to %#llx", (const void*)v->pool, (unsigned long long)end);
    if (end > v->top) v->top = end;
    return true;
}
}  // namespace

NVN(nvnMemoryPoolMapVirtual, NVNboolean, NVNmemoryPool* p, int n, const NVNmappingRequest* rq) {
    pool_t* o = PL(p); const map_req* r = reinterpret_cast<const map_req*>(rq);
    std::unique_lock lk{g_vp_mu};
    if (o->vp && o->vmode == 0 && n > 0) {   // the first mapping decides (see the sparse pools above)
        const pool_t* ph0 = gp<pool_t>(r[0].phys);
        if (ph0 && r[0].voff == 0 && r[0].size == o->size) { vpool_drop(o); o->mem = nullptr; o->vmode = 1; }
        else o->vmode = 2;
    }
    vpool* v = o->vmode == 2 ? o->vp : nullptr; bool ok = true;
    for (int i = 0; v && i < n && ok; i++) ok = vpool_map(v, r[i]);
    lk.unlock();
    if (v) return (NVNboolean)ok;
    // Aliased onto physical pools: the virtual pool's storage is the physical pool's memory (all requests must share one base)
    for (int i = 0; i < n; i++) {
        const pool_t* ph = gp<pool_t>(r[i].phys);
        if (!ph) continue;   // Unmapping (null physical pool): the host keeps the storage as is
        if (!ph->mem) { LOG("[RED] nvn: MapVirtual: physical pool %p has no storage", (const void*)ph); return 0; }
        uint8_t* base = ph->mem + r[i].poff - r[i].voff;
        if (r[i].voff < 0 || (uint64_t)r[i].voff + r[i].size > o->size || (uint64_t)r[i].poff + r[i].size > ph->size) { LOG("[RED] nvn: MapVirtual: request out of range (virtual %#llx+%#llx / pool %#llx, physical %#llx / pool %#llx)", (unsigned long long)r[i].voff, (unsigned long long)r[i].size, (unsigned long long)o->size, (unsigned long long)r[i].poff, (unsigned long long)ph->size); return 0; }
        if (o->mem && o->mem != base) { LOG("[RED] nvn: MapVirtual: non-contiguous mapping on a pool that was aliased: virtual pool %p already maps to %p, this request wants %p", (void*)o, (void*)o->mem, (void*)base); return 0; }
        if (!o->mem) LOG("nvn: MapVirtual: the storage of virtual pool %p (%#llx bytes) points at physical pool %p + %#llx", (void*)o, (unsigned long long)o->size, (const void*)ph, (unsigned long long)(r[i].poff - r[i].voff));
        o->mem = base;
    }
    return 1;
}

// ---------- GPU address resolution (replay.h) ----------
bool sparse_pool(const pool_t* pool) { return pool && pool->vp; }
uint64_t sparse_gen() { return g_vp_gen.load(); }
bool sparse_alias(u64 lo, u64 hi) {
    if (hi <= lo) return false;
    std::scoped_lock lk{g_vp_mu};
    for (auto& [b, v] : g_vps) if (lo < b + v->size && b < hi) return true;
    return false;
}
uint64_t sparse_copy(const pool_t* pool, void* dst, u64 gpu, u64 n) {
    std::scoped_lock lk{g_vp_mu};
    vpool* v = pool ? pool->vp : nullptr;
    if (!v || gpu < v->base || gpu >= v->base + v->size) { memset(dst, 0, n); return n; }
    uint64_t miss = 0;
    for (u64 p = 0, o = gpu - v->base; p < n;) {
        const u64 at = o + p; u64 k = 0x1000 - (at & 0xFFF); if (k > n - p) k = n - p;
        const vpte* e = at < v->pool->size ? vpt_get(v, at) : nullptr;
        if (e && e->phys) memcpy((uint8_t*)dst + p, hp(e->phys + (at & 0xFFF)), k); else { memset((uint8_t*)dst + p, 0, k); miss += k; }
        p += k;
    }
    return miss;
}
bool sparse_mapped(const pool_t* pool, u64 gpu, u64 n) {   // every page of [gpu, gpu+n) has memory behind it
    std::scoped_lock lk{g_vp_mu};
    vpool* v = pool ? pool->vp : nullptr;
    if (!v || gpu < v->base || gpu >= v->base + v->size) return false;
    const u64 end = gpu - v->base + n;
    for (u64 o = (gpu - v->base) & ~0xFFFull; o < end; o += 0x1000) { const vpte* e = o < v->pool->size ? vpt_get(v, o) : nullptr; if (!e || !e->phys) return false; }
    return true;
}
u64 sparse_phys(const pool_t* pool, u64 gpu, u64 n) {
    std::scoped_lock lk{g_vp_mu};
    vpool* v = pool ? pool->vp : nullptr;
    if (!v || gpu < v->base || gpu >= v->base + v->size) return 0;
    return vrun_phys(v, gpu - v->base, n);
}
bool sparse_changed_since(const pool_t* pool, u64 gpu, u64 n, uint64_t gen) {
    if (gen == ~0ull) return false;
    std::scoped_lock lk{g_vp_mu};
    vpool* v = pool ? pool->vp : nullptr;
    if (!v || gpu < v->base || gpu >= v->base + v->size) return true;
    const u64 o0 = gpu - v->base;
    for (u64 at = o0 & ~0xFFFull; at < o0 + n && at < v->pool->size; at += 0x1000) {
        const vpte* e = vpt_get(v, at);
        if (!e) { at |= (2u << 20) - 1; at -= 0xFFF; continue; }   // a leaf never mapped: skip its 2 MB
        if (e->gen > gen) return true;
    }
    return false;
}

bool sparse_stale(const pool_t* pool, u64 gpu, u64 n) {   // [gpu, gpu+n) has mapped pages, and each one's physical page was mapped again elsewhere since
    std::scoped_lock lk{g_vp_mu};
    vpool* v = pool ? pool->vp : nullptr;
    if (!v || gpu < v->base || gpu >= v->base + v->size) return false;
    bool any = false;
    const u64 end = gpu - v->base + n;
    for (u64 o = (gpu - v->base) & ~0xFFFull; o < end && o < v->pool->size; o += 0x1000) {
        const vpte* e = vpt_get(v, o);
        if (!e || !e->phys) continue;
        const auto it = v->rev.find(e->phys);
        if (it != v->rev.end() && it->second == o) return false;   // this page holds its physical page's latest mapping
        any = true;
    }
    return any;
}

// HOST_NVN_ALIAS_STATS=1 (renderer tick, every 30 s): per sparse pool, mapped memory against the distinct physical memory behind it.
// "stale" = virtual pages whose physical page is also mapped at another virtual page by a newer request (remapped without an unmap).
void sparse_alias_report() {
    std::scoped_lock lk{g_vp_mu};
    for (auto& [b, v] : g_vps) {
        if (v->pool->vmode != 2) continue;
        std::unordered_map<u64, std::pair<u64, u32>> newest;   // physical page -> newest generation mapping it, virtual pages mapping it
        u64 mapped = 0;
        for (const vpte* l : v->pt)
            if (l) for (int i = 0; i < VPT_LEAF; i++)
                if (l[i].phys) { mapped++; auto& g = newest[l[i].phys]; if (l[i].gen > g.first) g.first = l[i].gen; g.second++; }
        if (!mapped) continue;
        u64 stale = 0;
        u32 most = 0;
        for (const auto& [p, g] : newest) most = std::max(most, g.second);
        for (const vpte* l : v->pt)
            if (l) for (int i = 0; i < VPT_LEAF; i++)
                if (l[i].phys && newest[l[i].phys].first != l[i].gen) stale++;
        LOG("nvn: sparse pool %p (%#llx bytes): %llu MB mapped, %llu MB distinct physical, %llu MB stale aliases (most aliased page: %u virtual pages); %llu MB remapped so far",
            (const void*)v->pool, (unsigned long long)v->pool->size, (unsigned long long)(mapped >> 8),
            (unsigned long long)(newest.size() >> 8), (unsigned long long)(stale >> 8), most, (unsigned long long)(v->stale_dropped >> 8));
    }
}

uint8_t* gpu_ptr(u64 addr, u64 n) {
    {
        std::scoped_lock lk{g_vp_mu};
        if (!g_vps.empty()) if (vpool* v = vpool_find(addr)) {
            const u64 p = vrun_phys(v, addr - v->base, n);
            return p ? static_cast<uint8_t*>(hp(p)) : nullptr;
        }
    }
    return static_cast<uint8_t*>(hp(addr));
}
namespace {
template <class F> uint64_t gpu_pieces(u64 addr, u64 n, F f) {   // f(offset into the range, host pointer or null, length); returns the unmapped bytes
    uint64_t miss = 0;
    std::scoped_lock lk{g_vp_mu};
    vpool* v = g_vps.empty() ? nullptr : vpool_find(addr);
    if (!v) { f(0, static_cast<uint8_t*>(hp(addr)), n); return 0; }
    for (u64 p = 0, o = addr - v->base; p < n;) {
        const u64 at = o + p; u64 k = 0x1000 - (at & 0xFFF); if (k > n - p) k = n - p;
        const vpte* e = at < v->pool->size ? vpt_get(v, at) : nullptr;
        if (e && e->phys) f(p, static_cast<uint8_t*>(hp(e->phys + (at & 0xFFF))), k); else { f(p, nullptr, k); miss += k; }
        p += k;
    }
    return miss;
}
}  // namespace
uint64_t gpu_read(u64 addr, void* dst, u64 n) {
    return gpu_pieces(addr, n, [&](u64 off, uint8_t* src, u64 k) { if (src) memcpy((uint8_t*)dst + off, src, k); else memset((uint8_t*)dst + off, 0, k); });
}
uint64_t gpu_write(u64 addr, const void* src, u64 n) {
    return gpu_pieces(addr, n, [&](u64 off, uint8_t* d, u64 k) { if (d) memmove(d, (const uint8_t*)src + off, k); });
}
bool host_readable(u64 addr, u64 n) {
    {
        std::scoped_lock lk{g_vp_mu};
        if (!g_vps.empty()) if (vpool* v = vpool_find(addr)) {
            const u64 end = addr - v->base + n;
            for (u64 o = (addr - v->base) & ~0xFFFull; o < end; o += 0x1000) { const vpte* e = o < v->pool->size ? vpt_get(v, o) : nullptr; if (!e || !e->phys) return false; }
            return true;
        }
    }
    for (u64 a = addr; a < addr + n;) {   // guest memory is mapped there
        const mem::Info i = mem::query(a);
        if (i.state == mem::Free || i.state == mem::Inaccessible) return false;
        a = i.addr + i.size;
    }
    return true;
}

// ---------- Memory pools ----------
NVN(nvnMemoryPoolBuilderSetDefaults, void, NVNmemoryPoolBuilder* b) { memset(b, 0, sizeof(pool_builder_t)); }
NVN(nvnMemoryPoolBuilderSetDevice, void, NVNmemoryPoolBuilder* b, NVNdevice* d) { ((pool_builder_t*)b)->dev = d; }
NVN(nvnMemoryPoolBuilderSetStorage, void, NVNmemoryPoolBuilder* b, void* m, size_t n) { ((pool_builder_t*)b)->mem = m; ((pool_builder_t*)b)->size = n; }
NVN(nvnMemoryPoolBuilderSetFlags, void, NVNmemoryPoolBuilder* b, int f) { ((pool_builder_t*)b)->flags = f; }
NVN(nvnMemoryPoolInitialize, NVNboolean, NVNmemoryPool* p, const NVNmemoryPoolBuilder* b) {
    const pool_builder_t* s = (const pool_builder_t*)b; pool_t* o = PL(p);
    o->magic = NVN_MAGIC('P', 'O', 'O', 'L'); o->flags = s->flags; o->mem = (uint8_t*)s->mem; o->size = s->size; o->vp = nullptr; o->vmode = 0;
    if (!o->mem && (o->flags & 0x200)) vpool_reserve(o);   // a virtual pool's own address range until its first mapping decides (see MapVirtual)
    else if (o->mem) rp_pool_add(ga(o->mem), o->size, o->flags);
    if (!o->mem && !(o->flags & 0x200)) LOG("[RED] nvn: MemoryPoolInitialize: non-virtual pool has no storage flags=%#x size=%#llx", o->flags, (unsigned long long)o->size);
    OLOG("MemoryPool %p mem=%p size=%#llx flags=%#x", (void*)p, (void*)o->mem, (unsigned long long)o->size, o->flags);
    return 1;
}
NVN(nvnMemoryPoolFinalize, void, NVNmemoryPool* p) {
    pool_t* o = PL(p);
    // Dead to the renderer first (its tables dropped, the magic cleared), then torn down: a texture resolving on another thread must not find the page table or the storage already gone
    rp_alias_drop_pool(o); o->magic = 0;
    if ((o->flags & 0x200) && o->vp) vpool_release(o);
    if (o->mem && !(o->flags & 0x200)) {   // A virtual pool's storage is borrowed from a physical pool; do not unregister it
        rp_pool_del(ga(o->mem)); flushed_drop(ga(o->mem), o->size);
    }
}
NVN(nvnMemoryPoolFlushMappedRange, void, const NVNmemoryPool* p, ptrdiff_t off, size_t n) {
    const pool_t* o = CPL(p);
    if (!o->mem) return;
    if (!(o->flags & 0x200)) {
        flushed_add(ga(o->mem) + off, n);
        if (o->flags & NVN_MEMORY_POOL_FLAGS_CPU_CACHED) rp_pool_flushed(ga(o->mem) + off, n);
    }
}
NVN(nvnMemoryPoolGetBufferAddress, NVNbufferAddress, const NVNmemoryPool* p) { return (NVNbufferAddress)ga(CPL(p)->mem); }

// Memory pool queries
NVN(nvnMemoryPoolGetFlags, NVNmemoryPoolFlags, const NVNmemoryPool* p) { return (NVNmemoryPoolFlags)CPL(p)->flags; }
// NULL without CPU access (nvnBufferMap), and for a sparse virtual pool: its address range is not mapped for the CPU
NVN(nvnMemoryPoolMap, void*, const NVNmemoryPool* p) { const pool_t* o = CPL(p); return (o->flags & NVN_MEMORY_POOL_FLAGS_CPU_NO_ACCESS) || o->vmode == 2 ? nullptr : o->mem; }
NVN(nvnMemoryPoolGetSize, size_t, const NVNmemoryPool* p) { return (size_t)CPL(p)->size; }
NVN(nvnMemoryPoolInvalidateMappedRange, void, const NVNmemoryPool* p, ptrdiff_t off, size_t n) {
    const pool_t* o = CPL(p);
    if (o->mem && (o->flags & NVN_MEMORY_POOL_FLAGS_CPU_CACHED) && !(o->flags & 0x200)) rp_pool_invalidated(ga(o->mem) + off, n);
}
NVN(nvnMemoryPoolSetDebugLabel, void, NVNmemoryPool* p, const char* s) { (void)p; (void)s; }

// ---------- Descriptor pools ----------
// One ID space per pool. In NVN a texture or sampler ID indexes the pool the command buffer has bound (SetTexturePool / SetSamplerPool), and two pools may use the same IDs. The renderer keeps
// one table for each kind, so every pool gets a base in it, in creation order: registrations go to base + ID, and binds recorded on a command buffer get the base of the pool that command
// buffer last bound (cmdbuf.cpp bind_handles). A game with one pool of each kind is unchanged (base 0).
// Fire Emblem: Three Houses: a 12,544-entry texture pool and a 3,856-entry sampler pool for the scene, and 4,096-entry ones its 2D UI command buffer binds. Both fill IDs from 256 up; the scene's
// default 8x8 white texture overwrote the UI's textures, and the title menu's labels drew as plain bars or not at all.
namespace {
constexpr int NDPOOL = 16;
struct dpool_base_t { const void* pool; int base; };
struct DPools { dpool_base_t t[NDPOOL]; int nt = 0, next = 0; };
DPools g_tp, g_sp;
std::mutex g_dpool_mu;
int dpool_add(DPools& d, const void* pool, int n, const char* kind) {   // -> the pool's base (assigned at its first Initialize)
    std::scoped_lock lk{g_dpool_mu};
    int i = 0; while (i < d.nt && d.t[i].pool != pool) i++;
    if (i == d.nt) {
        if (d.nt == NDPOOL) { LOG("[RED] nvn: %s pools exceed %d: later ones share the first pool's numbering", kind, NDPOOL); return 0; }
        d.t[i] = dpool_base_t{pool, d.next}; d.nt++; d.next += (n + 255) & ~255;
        if (i) LOG("nvn: pool #%d (%s, %d entries): numbered from %d in the host table, separate from the earlier pools", i + 1, kind, n, d.t[i].base);
    }
    return d.t[i].base;
}
int dpool_base_of(const DPools& d, const void* pool) { std::scoped_lock lk{g_dpool_mu}; for (int i = 0; i < d.nt; i++) if (d.t[i].pool == pool) return d.t[i].base; return 0; }
}  // namespace
int dpool_base(const void* pool, bool tex) { return dpool_base_of(tex ? g_tp : g_sp, pool); }

// The pools each command buffer last bound are kept in its own host state (cmd_t tex_base / samp_base). They used to be a 64-entry list of command buffers that was never cleared:
// Dark Souls Remastered binds one of 12 sampler pools in more than 64 command buffers, and every later one read its samplers through the first pool's numbering
NVN(nvnTexturePoolInitialize, NVNboolean, NVNtexturePool* tp, const NVNmemoryPool* p, ptrdiff_t off, int n) {
    *(dpool_t*)tp = dpool_t{CPL(p), off, n}; OLOG("TexturePool n=%d", n);
    dpool_add(g_tp, tp, n, "texture");
    { std::scoped_lock lk{g_dpool_mu}; rp_set_tex_pool_n(g_tp.next); }
    return 1;
}
NVN(nvnTexturePoolFinalize, void, NVNtexturePool* tp) { (void)tp; }
NVN(nvnTexturePoolRegisterTexture, void, const NVNtexturePool* tp, int id, const NVNtexture* t, const NVNtextureView* v) {
    rp_reg_tex(dpool_base(tp, true) + id, (const tex_t*)t, (const view_t*)v);
}
// Image descriptors live in the same pool index space as texture descriptors, and image handles are pool indices (nvnDeviceGetImageHandle; BindImages), so an image
// registers into the same table. Hyrule Warriors: Age of Calamity registers the images its compute shaders write
NVN(nvnTexturePoolRegisterImage, void, const NVNtexturePool* tp, int id, const NVNtexture* t, const NVNtextureView* v) {
    rp_reg_image(dpool_base(tp, true) + id, (const tex_t*)t, (const view_t*)v);
}
NVN(nvnSamplerPoolInitialize, NVNboolean, NVNsamplerPool* sp, const NVNmemoryPool* p, ptrdiff_t off, int n) {
    *(dpool_t*)sp = dpool_t{CPL(p), off, n}; OLOG("SamplerPool n=%d", n); dpool_add(g_sp, sp, n, "sampler"); return 1;
}
NVN(nvnSamplerPoolFinalize, void, NVNsamplerPool* sp) { (void)sp; }
NVN(nvnSamplerPoolRegisterSampler, void, const NVNsamplerPool* sp, int id, const NVNsampler* s) { rp_reg_samp(dpool_base(sp, false) + id, &((const samp_t*)s)->p); }
// straight from a builder, without a sampler object (FINAL FANTASY TACTICS - The Ivalice Chronicles registers its samplers this way)
NVN(nvnSamplerPoolRegisterSamplerBuilder, void, const NVNsamplerPool* sp, int id, const NVNsamplerBuilder* b) { rp_reg_samp(dpool_base(sp, false) + id, &((const samp_builder_t*)b)->p); }
// Unreal Engine reads a descriptor pool's size back at device setup (Octopath Traveler 0)
NVN(nvnTexturePoolGetMemoryPool, const NVNmemoryPool*, const NVNtexturePool* tp) { return (const NVNmemoryPool*)((const dpool_t*)tp)->pool; }
NVN(nvnTexturePoolGetMemoryOffset, ptrdiff_t, const NVNtexturePool* tp) { return (ptrdiff_t)((const dpool_t*)tp)->off; }
NVN(nvnTexturePoolGetSize, int, const NVNtexturePool* tp) { return ((const dpool_t*)tp)->n; }
NVN(nvnSamplerPoolGetMemoryPool, const NVNmemoryPool*, const NVNsamplerPool* sp) { return (const NVNmemoryPool*)((const dpool_t*)sp)->pool; }
NVN(nvnSamplerPoolGetMemoryOffset, ptrdiff_t, const NVNsamplerPool* sp) { return (ptrdiff_t)((const dpool_t*)sp)->off; }
NVN(nvnSamplerPoolGetSize, int, const NVNsamplerPool* sp) { return ((const dpool_t*)sp)->n; }
// Debug labels: only for NVN debugging tools, unused by the host
NVN(nvnTexturePoolSetDebugLabel, void, NVNtexturePool* p, const char* s) { (void)p; (void)s; }
NVN(nvnSamplerPoolSetDebugLabel, void, NVNsamplerPool* p, const char* s) { (void)p; (void)s; }

// ---------- Buffers ----------
// Buffer object = pool + offset + size; the GPU address and the CPU mapping are both the pool's address + offset (as MemoryPoolGetBufferAddress / Map)
struct buf_builder_t { NVNdevice* dev; const pool_t* pool; int64_t off; uint64_t size; };   NVN_FITS(buf_builder_t, NVNbufferBuilder);
// Guest ring allocators write offset/size directly at +0x18/+0x20 (nxnative issue #723: an engine retargets an initialized buffer with direct 64-bit stores, then calls
// nvnBufferGetAddress / GetSize). The intervening driver bytes have no host meaning; keep them reserved.
struct buf_t { uint32_t magic; const pool_t* pool; uint8_t reserved[8]; int64_t off; uint64_t size; };   NVN_FITS(buf_t, NVNbuffer);
static_assert(offsetof(buf_t, off) == 0x18, "NVNbuffer guest offset");
static_assert(offsetof(buf_t, size) == 0x20, "NVNbuffer guest size");
#define BB(b) ((buf_builder_t*)(b))
#define BF(b) ((const buf_t*)(b))
NVN(nvnBufferBuilderSetDefaults, void, NVNbufferBuilder* b) { NVNdevice* d = BB(b)->dev; memset(b, 0, sizeof(buf_builder_t)); BB(b)->dev = d; }
NVN(nvnBufferBuilderSetDevice, void, NVNbufferBuilder* b, NVNdevice* d) { BB(b)->dev = d; }
NVN(nvnBufferBuilderSetStorage, void, NVNbufferBuilder* b, NVNmemoryPool* p, ptrdiff_t off, size_t n) { BB(b)->pool = CPL(p); BB(b)->off = off; BB(b)->size = n; }
NVN(nvnBufferInitialize, NVNboolean, NVNbuffer* buf, const NVNbufferBuilder* b) {
    const buf_builder_t* bb = (const buf_builder_t*)b;
    if (!bb->pool || !bb->pool->mem) { LOG("[RED] nvn: BufferInitialize: pool has no storage (pool %p flags %#x offset %#llx size %#llx)", (const void*)bb->pool, bb->pool ? bb->pool->flags : 0, (unsigned long long)bb->off, (unsigned long long)bb->size); return 0; }
    // The NVN driver does not check a buffer's range against its pool, and the host's own GPU access never goes by the buffer: it takes an address and a length from the command.
    // Breath of the Wild creates a 256 KB buffer over a 144 KB pool for a screen readback whose one use lies inside the pool. Not an error by itself: one line.
    if ((uint64_t)bb->off + bb->size > bb->pool->size) ONCE_LOG("BufferInitialize: a buffer reaches past the end of its pool (offset %#llx size %#llx pool %#llx); only an access that leaves the pool is an error, and it is reported where it happens", (unsigned long long)bb->off, (unsigned long long)bb->size, (unsigned long long)bb->pool->size);
    buf_t* o = (buf_t*)buf; memset(o, 0, sizeof *o);
    o->magic = NVN_MAGIC('B', 'U', 'F', 0); o->pool = bb->pool; o->off = bb->off; o->size = bb->size;
    OLOG("BufferInitialize %p pool=%p off=%#llx size=%#llx", (void*)buf, (const void*)bb->pool, (unsigned long long)bb->off, (unsigned long long)bb->size);
    return 1;
}
NVN(nvnBufferFinalize, void, NVNbuffer* buf) { ((buf_t*)buf)->magic = 0; }
NVN(nvnBufferSetDebugLabel, void, NVNbuffer* buf, const char* s) { (void)buf; (void)s; }
NVN(nvnBufferGetAddress, NVNbufferAddress, const NVNbuffer* buf) { return (NVNbufferAddress)(ga(BF(buf)->pool->mem) + BF(buf)->off); }
// A buffer or pool the CPU has no access to (NVN_MEMORY_POOL_FLAGS_CPU_NO_ACCESS) maps to NULL, as in NVN, though the host's storage is CPU memory: EA SPORTS FC 27's Frostbite
// renderer takes a buffer's GPU address as GetAddress + (its data pointer - Map), with a null data pointer for such a buffer, so a real pointer made the copy's destination address 0.
NVN(nvnBufferMap, void*, const NVNbuffer* buf) { const buf_t* o = BF(buf); return (o->pool->flags & NVN_MEMORY_POOL_FLAGS_CPU_NO_ACCESS) || o->pool->vmode == 2 ? nullptr : o->pool->mem + o->off; }
NVN(nvnBufferGetSize, size_t, const NVNbuffer* buf) { return (size_t)BF(buf)->size; }
NVN(nvnBufferGetMemoryPool, NVNmemoryPool*, const NVNbuffer* buf) { return (NVNmemoryPool*)BF(buf)->pool; }
NVN(nvnBufferGetMemoryOffset, ptrdiff_t, const NVNbuffer* buf) { return (ptrdiff_t)BF(buf)->off; }
NVN(nvnBufferFlushMappedRange, void, const NVNbuffer* buf, ptrdiff_t off, size_t n) {
    const buf_t* o = BF(buf);
    if (o->pool->mem && (o->pool->flags & NVN_MEMORY_POOL_FLAGS_CPU_CACHED) && !(o->pool->flags & 0x200)) rp_pool_flushed(ga(o->pool->mem) + o->off + off, n);
}        // unified memory: nothing to flush
NVN(nvnBufferInvalidateMappedRange, void, const NVNbuffer* buf, ptrdiff_t off, size_t n) {
    const buf_t* o = BF(buf);
    if (o->pool->mem && (o->pool->flags & NVN_MEMORY_POOL_FLAGS_CPU_CACHED) && !(o->pool->flags & 0x200)) rp_pool_invalidated(ga(o->pool->mem) + o->off + off, n);
}
NVN(nvnBufferBuilderGetSize, size_t, const NVNbufferBuilder* b) { return (size_t)((const buf_builder_t*)b)->size; }
NVN(nvnBufferBuilderGetMemoryOffset, ptrdiff_t, const NVNbufferBuilder* b) { return (ptrdiff_t)((const buf_builder_t*)b)->off; }
// The storage set with SetStorage (EA SPORTS FC 27's Frostbite renderer reads it back from its builders before it initializes the objects). The pool getters return the
// pool's address; the header the prototypes come from declares the builders' by value, so they get no signature check
NVN_NOPROTO(nvnBufferBuilderGetMemoryPool, NVNmemoryPool*, const NVNbufferBuilder* b) { return (NVNmemoryPool*)((const buf_builder_t*)b)->pool; }

}  // namespace nvn
