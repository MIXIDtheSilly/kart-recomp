// Native NVN: declarations shared by the interface-layer sources (device.cpp, memory.cpp, texture.cpp, ...).
// Not for the renderer: that is replay.h.
#pragma once
#include <atomic>
#include <cstring>
#include <mutex>
#include <vector>

#include "../common.h"
#include "../host.h"
#include "../memory.h"
#include "abi.h"
#include "nvn_host.h"
#include "nvn_obj.h"
#include "replay.h"

// Boolean parameters: guest callers leave the upper bits of a bool register undefined, only the low 8 bits count.
#define B(x) ((x) & 0xff)

#define OLOG(...) do { if (::nvn::g_log) LOG("nvn: " __VA_ARGS__); } while (0)
#define ONCE_RED(...) do { static int once_; if (!once_++) LOG("[RED] nvn: " __VA_ARGS__); } while (0)
#define ONCE_LOG(...) do { static int once_; if (!once_++) LOG("nvn: " __VA_ARGS__); } while (0)

namespace nvn {

extern bool g_log;                    // HOST_NVN_LOG=1: log object creation
extern std::mutex g_sub_mu;           // one submission at a time (queue.cpp); also guards the renderer's per-queue bindings
extern std::atomic<uint64_t> g_frame; // presents so far (window.cpp)

// The guest return address of the NVN call being served (x30), for diagnostics and the dispatch record's call-site field.
inline uint64_t call_site() { Ctx* c = guest_ctx(); return c ? c->x[30] : 0; }

inline uint64_t now_ns() { return host_ns(); }   // monotonic host clock, for pacing

// ---- device.cpp
// The NVN2 constants of the game's own sdk (nvnDeviceGetInteger's switch): item -> *out, false when the sdk has no constant case.
bool sdk_nvn2_item(unsigned item, int* out);

// ---- memory.cpp
void flushed_add(u64 gpu, u64 n);     // MemoryPoolFlushMappedRange (the renderer asks nvn::flushed)
void flushed_drop(u64 gpu, u64 n);
void vpool_reserve(pool_t* o);        // sparse virtual pools, see MapVirtual
void vpool_release(pool_t* o);

// ---- cmdbuf.cpp
// Appends a record of `payload` bytes (zeroed) with opcode op to the command buffer; returns the payload.
void* rec(NVNcommandBuffer* c, uint32_t op, size_t payload);
// Snapshot of texture object t into slot k after record r at offset off (or the live object when invalid); null stays null.
const tex_t* tex_snap(void* r, size_t off, int k, const NVNtexture* t, uint64_t ra);
void copy_view(view_t* dst, uint8_t* has, const NVNtextureView* v);
#define SNAP_OFF(T) ((sizeof(T) + 7) & ~(size_t)7)
#define REC(c, OP, T) (static_cast<T*>(::nvn::rec((c), (OP), sizeof(T))))
// Binding records with arrays are allocated by the actual count: prefix fields + n elements (the reader reads only the first n)
#define RECN(c, OP, T, ARR, N) (static_cast<T*>(::nvn::rec((c), (OP), offsetof(T, ARR) + sizeof(static_cast<T*>(nullptr)->ARR[0]) * (size_t)(N))))
#define REC_SNAP(c, OP, T, K) (static_cast<T*>(::nvn::rec((c), (OP), SNAP_OFF(T) + (size_t)(K) * sizeof(tex_t))))

// ---- program.cpp
void pk_drop(const void* p);

}  // namespace nvn
