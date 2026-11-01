// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// The Switch 2 graphics server as the SDK sees it (nx2:gsv and the sessions it hands out: syncpt,
// gpu, vi2, nvsrv). Ported from Eglantine's nx2_gsv.h; command layouts are documented there.
// GPU address == CPU address; GPU-only memory gets host-allocated backing in the alias region.

#include <windows.h>
#include <timeapi.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <map>
#include <optional>
#include <thread>

#include "common/settings.h"
#include "core/core.h"
#include "core/hle/service/nvnflinger/pixel_format.h"
#include "video_core/framebuffer_config.h"
#include "video_core/gpu.h"
#include "video_core/host1x/host1x.h"
#include "video_core/host1x/nx2_video_decoder.h"
#include "video_core/nx2.h"
#include "video_core/renderer_base.h"
#include "core/frontend/framebuffer_layout.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#include "../ipc.h"
#include "../memory.h"
#include "../video.h"
#include "../nv/devices/nx2_stub_device.h"
#include "../nv/nvsrv.h"

namespace ipc {
namespace {

using ::Service::Nvidia::Devices::Nx2GpuMemoryRegistry;

// Word i of a TIPC request as Eglantine reads it (cmd_buf[i]): the data words start at word 2.
u32 W(const Request& r, size_t i) { return r.arg<u32>((i - 2) * 4); }
u64 W64(const Request& r, size_t i) { return u64{W(r, i)} | u64{W(r, i + 1)} << 32; }

void reply(Request& r, std::initializer_list<u32> words) {
    for (u32 w : words) r.push(w);
}
void reply64(Request& r, std::initializer_list<u64> values) {
    for (u64 v : values) r.push(v);
}

// A session on `s` returned as a copy handle (word 3, result at word 4).
void return_session(Request& r, ServicePtr s) { r.out_copy.push_back(new_session(std::move(s))); }

Tegra::Host1x::SyncpointManager& syncpoints() { return video::system().Host1x().GetSyncpointManager(); }
Tegra::GPU& gpu() { return video::system().GPU(); }

constexpr u32 HostSyncpoints = 192;  // Tegra::Host1x::SyncpointManager

// The syncpoint values the game sees. Guest values count a GPU fence as reached once the GPU
// thread has queued it; host values once the host GPU ran everything before it and the buffers
// copied back for the CPU (reactive flushing) are in guest memory (video::host_fences(), which
// delays the fences). With guest values the game read GPU results it waited for before they were
// there, or half copied (camera placed from them: frames of the world stretched into streaks).
bool host_fences() { return video::host_fences(); }
u32 sync_value(u32 id) {
    return host_fences() ? syncpoints().GetHostSyncpointValue(id) : syncpoints().GetGuestSyncpointValue(id);
}
bool sync_ready(u32 id, u32 value) {
    return host_fences() ? syncpoints().IsReadyHost(id, value) : syncpoints().IsReadyGuest(id, value);
}
template <typename F>
Tegra::Host1x::SyncpointManager::ActionHandle on_sync(u32 id, u32 value, F&& f) {
    return host_fences() ? syncpoints().RegisterHostAction(id, value, std::forward<F>(f))
                         : syncpoints().RegisterGuestAction(id, value, std::forward<F>(f));
}
void cancel_sync(u32 id, const Tegra::Host1x::SyncpointManager::ActionHandle& h) {
    if (host_fences()) syncpoints().DeregisterHostAction(id, h);
    else syncpoints().DeregisterGuestAction(id, h);
}

// Events a session creates on demand by id.
struct EventMap {
    std::mutex m;
    std::map<u32, std::shared_ptr<kern::ServiceEvent>> events;
    std::shared_ptr<kern::ServiceEvent> get(u32 id) {
        std::scoped_lock lk{m};
        auto& e = events[id];
        if (!e) e = std::make_shared<kern::ServiceEvent>();
        return e;
    }
};

// ------------------------------------------------------------------ nx2:syncpt

struct Syncpt : Service {
    struct SlotRequest {
        u32 syncpoint = 0;
        std::atomic<bool> pending{false};
        Tegra::Host1x::SyncpointManager::ActionHandle handle{};
    };
    struct Shared {
        std::mutex m;
        bool alive = true;
    };
    EventMap events;
    std::map<u32, SlotRequest> requests;  // nodes are stable: actions hold references
    std::shared_ptr<Shared> shared = std::make_shared<Shared>();

    Syncpt() : Service("nx2:syncpt") {
        on(0, [](Request& r) { reply(r, {W(r, 2), W(r, 3)}); });  // handshake: echo the token
        on(1, [](Request& r) { reply(r, {32}); });
        on(22, [this](Request& r) { r.out_copy.push_back(events.get(W(r, 2))->new_handle()); });
        on(103, [](Request& r) {  // QueryCounter: the counter is ~syncpoint id
            const u32 id = ~W(r, 2);
            const u64 v = id < HostSyncpoints ? sync_value(id) : 0;
            reply(r, {static_cast<u32>(v), static_cast<u32>(v >> 32), 0});
        });
        on(201, [this](Request& r) {  // signal slot's event once ~counter reaches value
            const u32 slot = W(r, 2), id = ~W(r, 3);
            const u32 value = W(r, 4);
            auto ev = events.get(slot);
            if (id >= HostSyncpoints) {
                ev->signal();
                return;
            }
            auto& req = requests[slot];
            cancel(req);
            ev->clear();
            req.syncpoint = id;
            req.pending = true;
            req.handle = on_sync(id, value, [sh = shared, &req, ev] {
                std::scoped_lock lk{sh->m};
                if (!sh->alive) return;
                req.pending = false;
                ev->signal();
            });
        });
        on(202, [this](Request& r) {  // cancel: always signals (the SDK waits on it next)
            const u32 slot = W(r, 2);
            if (auto it = requests.find(slot); it != requests.end()) cancel(it->second);
            events.get(slot)->signal();
        });
    }
    ~Syncpt() override {
        {
            std::scoped_lock lk{shared->m};
            shared->alive = false;
        }
        for (auto& [slot, req] : requests) cancel(req);
    }
    static void cancel(SlotRequest& req) {
        if (req.pending.exchange(false)) cancel_sync(req.syncpoint, req.handle);
    }
};

// ------------------------------------------------------------------ nx2:gpu

// GPU-only memory: host-allocated and mapped so the SMMU can reach it, in the code region past the
// modules and below the stack region, as Eglantine does (alias code region). Not in the alias
// region: the game reserves ranges there and maps memory into them later (MapPhysicalMemory).
constexpr u64 BackingBase = 0x100000000ull, BackingEnd = mem::StackBase;
struct Backing {
    u64 addr = 0, size = 0;
};

std::mutex g_alias_lock;

std::optional<Backing> allocate_backing(u64 size) {
    std::scoped_lock lk{g_alias_lock};
    // The last GiB of the pool stays for the game's own allocations, which can't fail softly:
    // without it the sparse blocks (never freed while their region lives) end the process.
    constexpr u64 Reserve = 1ull << 30;
    if (mem::phys_free_bytes() < size + Reserve) {
        static std::atomic<int> warned{0};
        if (warned++ < 8) LOG("nx2:gpu: guest memory pool nearly full, no GPU-only memory for %#llx bytes", size);
        return std::nullopt;
    }
    const u64 addr = mem::find_free(BackingBase, BackingEnd - BackingBase, size, 0x200000);
    if (!addr) {
        LOG("nx2:gpu: no room for %#llx bytes of GPU-only memory", size);
        return std::nullopt;
    }
    mem::map(addr, size, mem::Io, mem::RW);
    return Backing{addr, size};
}

void free_backing(const Backing& b) {
    std::scoped_lock lk{g_alias_lock};
    mem::unmap(b.addr, b.size);
}

struct Gpu : Service {
    static constexpr u32 NumIds = 32;
    static constexpr u32 SparseRegionFlag = 0x200;
    static constexpr u64 SparseBlockSize = 4ull << 20;
    enum : u8 { BlockEmpty, BlockBacked, BlockFailed, BlockPending };

    struct SparseRegion {
        u64 size;
        std::vector<u8> blocks;
        bool warned = false;
    };
    struct Released {
        Backing b;
        std::shared_ptr<std::atomic<bool>> done;
    };

    EventMap events;
    std::map<u32, std::pair<u64, u64>> memory;  // handle -> (address, size)
    u32 next_handle = 1;

    std::recursive_mutex region_lock;
    std::condition_variable_any region_cv;
    std::map<u64, SparseRegion> sparse;  // by GPU address
    std::map<u64, Backing> gpu_only;     // by GPU address (whole regions and sparse blocks)
    std::vector<Released> released;
    u64 gpu_only_bytes = 0, gpu_only_peak = 0, sparse_blocks = 0;

    std::shared_ptr<kern::ServiceEvent> event(u32 id) { return events.get(id % NumIds); }

    Gpu() : Service("nx2:gpu") {
        on(11, [](Request& r) { reply(r, {0, NumIds - 1}); });
        on(12, [this](Request& r) { r.out_copy.push_back(event(W(r, 2))->new_handle()); });
        on(21, [this](Request& r) { event(W(r, 2))->signal(); });
        on(22, [this](Request& r) { return_session(r, shared_from_this()); });  // sub-session
        on(33, [](Request& r) { reply(r, {1}); });
        // Address space ranges: [0x8000000, 0xC000000000), as NvRm's AllocAsEx.
        on(201, [](Request& r) { reply64(r, {0x8000000ull, 0xC000000000ull - 0x8000000ull}); });
        on(202, [](Request& r) { reply64(r, {0, 0}); });
        on(203, [](Request& r) { reply64(r, {1, 0x8000000ull, 0xC000000000ull - 0x8000000ull}); });

        // 101 CreateGpuAccessibleMemory(start, last, ...): mapped at GPU address == CPU address.
        on(101, [this](Request& r) {
            const u64 start = W64(r, 2), last = W64(r, 4);
            if (last > start) {
                Nx2GpuMemoryRegistry::Get().Register(start, last - start + 1);
                if (last - start + 1 == 0x2000) Tegra::Nx2AddSemaphoreRegion(start);
            }
            const u32 h = next_handle++;
            memory[h] = {start, last - start + 1};
            reply(r, {h});
        });
        // 102 DestroyGpuAccessibleMemory(handle, slot).
        on(102, [this](Request& r) {
            const u32 h = W(r, 2);
            if (auto it = memory.find(h); it != memory.end()) {
                const auto [addr, size] = it->second;
                if (size) Nx2GpuMemoryRegistry::Get().Unmap(addr, size);
                if (size == 0x2000) Tegra::Nx2RemoveSemaphoreRegion(addr);
                memory.erase(it);
            } else {
                LOG("nx2:gpu 102: unknown memory handle %u", h);
            }
            signal_after_gpu(W(r, 3));
        });
        // 211 NormalMapGpuMemory4K: [first, last] GPU range, memory handle, offset, slot (word 17).
        on(211, [this](Request& r) {
            const u64 first = W64(r, 3), last = W64(r, 5), offset = W64(r, 8);
            const u32 h = W(r, 7);
            if (auto it = memory.find(h); it != memory.end() && last > first) {
                Nx2GpuMemoryRegistry::Get().MapAt(first, it->second.first + offset, last - first + 1);
            } else {
                LOG("nx2:gpu 211: map of unknown memory handle %u", h);
            }
            event(W(r, 17))->signal();
        });
        // 212 NormalModifyMemoryKind4K: kinds don't matter to the GPU; report the wanted one.
        on(212, [this](Request& r) {
            event(W(r, 10))->signal();
            reply(r, {W(r, 7) & 0xff});
        });
        // 213 NormalUnmapGpuMemory4K: [first, last], slot word 8.
        on(213, [this](Request& r) {
            const u64 first = W64(r, 3), last = W64(r, 5);
            if (last > first) Nx2GpuMemoryRegistry::Get().Unmap(first, last - first + 1);
            signal_after_gpu(W(r, 8));
        });
        on(214, [this](Request& r) { create_region(r); });
        on(215, [this](Request& r) { destroy_region(r); });

        Tegra::Nx2SetSparseBacking([this](u64 gpu_address, u64 size) { back_sparse_write(gpu_address, size); });
    }
    ~Gpu() override { Tegra::Nx2SetSparseBacking({}); }

    // Signalled once the GPU thread has run everything queued so far.
    void signal_after_gpu(u32 slot) {
        auto ev = event(slot);
        gpu().RunInOrder([ev] { ev->signal(); });
    }

    void add_backing(u64 gpu_address, const Backing& b) {
        gpu_only[gpu_address] = b;
        gpu_only_bytes += b.size;
        gpu_only_peak = std::max(gpu_only_peak, gpu_only_bytes);
    }

    // Unmaps destroyed regions' backing once the GPU thread is past their last use.
    void free_released() {
        std::erase_if(released, [&](Released& rel) {
            if (!rel.done->load(std::memory_order_acquire)) return false;
            free_backing(rel.b);
            gpu_only_bytes -= rel.b.size;
            return true;
        });
    }

    // 214: GPU-only region [start, last] (words 3-6), flags word 7, slot word 11.
    void create_region(Request& r) {
        const u64 start = W64(r, 3), last = W64(r, 5);
        const u32 flags = W(r, 7), slot = W(r, 11);
        if (last > start && ((last + 1 - start) % mem::PageSize) == 0) {
            const u64 size = last + 1 - start;
            std::scoped_lock lk{region_lock};
            free_released();
            if (flags & SparseRegionFlag) {
                // Backed block by block as the GPU writes it (back_sparse_write).
                sparse[start] = {size, std::vector<u8>((size + SparseBlockSize - 1) / SparseBlockSize)};
                Nx2GpuMemoryRegistry::Get().CreateSparse(start, size);
                Tegra::Nx2SparseRegionCreated(start, size, SparseBlockSize);
                Tegra::Nx2AddSparseSpan(start, size);
            } else if (auto b = allocate_backing(size)) {
                add_backing(start, *b);
                Nx2GpuMemoryRegistry::Get().MapAt(start, b->addr, size);
                LOG("nx2:gpu 214: GPU-only region %#llx+%#llx backed at %#llx (%llu MiB in use)", start, size,
                    b->addr, gpu_only_bytes >> 20);
            }
        } else {
            LOG("nx2:gpu 214: unexpected region [%#llx, %#llx]", start, last);
        }
        event(slot)->signal();
    }

    // 215 DestroySparseRegion: [first, last] words 3-6, slot word 8.
    void destroy_region(Request& r) {
        const u64 first = W64(r, 3), last = W64(r, 5);
        {
            std::scoped_lock lk{region_lock};
            free_released();
            if (last > first) {
                if (sparse.erase(first)) {
                    Tegra::Nx2SparseRegionDestroyed(first);
                    Nx2GpuMemoryRegistry::Get().DestroySparse(first, last - first + 1);
                } else {
                    Nx2GpuMemoryRegistry::Get().Unmap(first, last - first + 1);
                }
                std::shared_ptr<std::atomic<bool>> done;
                for (auto it = gpu_only.lower_bound(first); it != gpu_only.end() && it->first <= last;) {
                    if (it->first + it->second.size - 1 > last) {
                        ++it;
                        continue;
                    }
                    Nx2GpuMemoryRegistry::Get().ReleaseBacking(it->second.addr, it->second.size);
                    if (!done) {
                        done = std::make_shared<std::atomic<bool>>(false);
                        gpu().RunInOrder([done] { done->store(true, std::memory_order_release); });
                    }
                    released.push_back({it->second, done});
                    it = gpu_only.erase(it);
                }
            }
        }
        signal_after_gpu(W(r, 8));
    }

    // Before a GPU write that may land in a sparse region (usually on the GPU thread): the blocks
    // it touches get cleared memory behind the region's device pages.
    void back_sparse_write(u64 gpu_address, u64 size) {
        struct Pending {
            u64 region, block;
        };
        std::vector<Pending> todo;
        const u64 end = gpu_address + size;
        auto for_regions = [&](auto&& fn) {
            auto it = sparse.upper_bound(gpu_address);
            if (it != sparse.begin()) --it;
            for (; it != sparse.end() && it->first < end; ++it) {
                auto& [start, region] = *it;
                if (start + region.size <= gpu_address) continue;
                fn(start, region, (std::max(gpu_address, start) - start) / SparseBlockSize,
                   (std::min(end, start + region.size) - 1 - start) / SparseBlockSize);
            }
        };
        bool waits = false;
        {
            std::scoped_lock lk{region_lock};
            for_regions([&](u64 start, SparseRegion& region, u64 a, u64 b) {
                for (u64 k = a; k <= b; k++) {
                    if (region.blocks[k] == BlockEmpty) {
                        region.blocks[k] = BlockPending;
                        todo.push_back({start, k});
                    } else if (region.blocks[k] == BlockPending) {
                        waits = true;
                    }
                }
            });
        }
        const auto t0 = std::chrono::steady_clock::now();
        struct Timer {
            std::chrono::steady_clock::time_point t0;
            size_t n;
            ~Timer() {
                const auto us = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count();
                static std::atomic<int> logged{0};
                if (n && us >= 5000 && logged++ < 100) LOG("nx2:gpu: backing %zu sparse blocks took %lld ms", n, us / 1000);
            }
        } timer{t0, todo.size()};
        for (const auto& [start, block] : todo) {
            u64 block_size = SparseBlockSize;
            {
                std::scoped_lock lk{region_lock};
                if (auto it = sparse.find(start); it != sparse.end())
                    block_size = std::min(SparseBlockSize, it->second.size - block * SparseBlockSize);
            }
            auto b = allocate_backing(block_size);
            bool stale = false;
            {
                std::scoped_lock lk{region_lock};
                auto it = sparse.find(start);
                if (it == sparse.end() || block >= it->second.blocks.size()) {
                    stale = true;
                } else if (!b) {
                    it->second.blocks[block] = BlockFailed;
                    if (!it->second.warned) {
                        it->second.warned = true;
                        LOG("nx2:gpu: sparse region %#llx: out of memory, its unbacked blocks drop writes", start);
                    }
                } else {
                    const u64 block_address = start + block * SparseBlockSize;
                    it->second.blocks[block] = BlockBacked;
                    add_backing(block_address, *b);
                    Nx2GpuMemoryRegistry::Get().BackSparse(block_address, b->addr, block_size);
                    Tegra::Nx2SparseBlockWritten(start, block);
                    if (++sparse_blocks == 1 || sparse_blocks % 64 == 0)
                        LOG("nx2:gpu: %llu sparse blocks backed; %llu MiB of GPU-only memory in use, peak %llu MiB",
                            sparse_blocks, gpu_only_bytes >> 20, gpu_only_peak >> 20);
                }
            }
            region_cv.notify_all();
            if (stale && b) free_backing(*b);
        }
        if (waits) {
            std::unique_lock lk{region_lock};
            region_cv.wait(lk, [&] {
                bool pending = false;
                for_regions([&](u64, SparseRegion& region, u64 a, u64 b) {
                    for (u64 k = a; k <= b; k++) pending |= region.blocks[k] == BlockPending;
                });
                return !pending;
            });
        }
    }
};

// ------------------------------------------------------------------ native memory and images

struct NativeObjects {
    struct Memory {
        u64 size = 0, address = 0;
    };
    struct Image {
        u32 memory = 0;
        std::array<u32, 16> preference{};
        u64 size = 0;
        u32 count = 1;
        u64 offset = 0;  // into its memory (the images of a decoder pool share one)
        bool window_pool = false;
        bool decoder = false;  // a video decoder output image (911)
    };
    std::mutex m;
    std::map<u32, Memory> memories;
    std::map<u32, Image> images;
    u32 next_memory = 1, next_image = 1;
};

// Block-linear layout of an InterModuleUserImagePreference (see Eglantine's Nx2CalculateImageLayout).
struct ImageLayout {
    u32 width, height;
    u64 pitch;
    u32 block_height_log2;
    u64 size, alignment;
};

ImageLayout image_layout(const u32* p) {
    ImageLayout l{};
    l.width = (p[2] & 0xFFFF) + 1;
    l.height = (p[2] >> 16) + 1;
    const u64 pitch_gobs = p[5];
    if (p[4] <= 8 && pitch_gobs >= (u64{l.width} * 4 + 63) / 64 && pitch_gobs <= 4096) {
        l.pitch = pitch_gobs * 64;
        l.block_height_log2 = p[4];
    } else {
        l.pitch = (u64{l.width} * 4 + 63) & ~u64{63};
        l.block_height_log2 = 7;
    }
    const u64 rows = u64{1} << l.block_height_log2;
    l.size = l.pitch * ((l.height + rows - 1) / rows * rows);
    l.alignment = std::max<u64>(64 * rows, 0x1000);
    return l;
}

std::array<u32, 16> preference_at(const Request& r, size_t word) {
    std::array<u32, 16> p{};
    for (size_t i = 0; i < 16; i++) p[i] = W(r, word + i);
    return p;
}

// Video decoder output images (991/911/961; Eglantine's Nx2CalculateDecoderImageLayout): word 1
// of the preference is the layout code (1/4 NV12, 2/5 BGRA, else RGBA), word 2 the size. Rows are
// pitch-linear, NV12's half-height chroma plane below the luma plane, planes padded to 128 rows.
struct DecoderImageLayout {
    u32 code, width, height;
    Tegra::Host1x::Nx2PictureFormat format;
    u64 pitch, chroma_offset, size;
};

DecoderImageLayout decoder_image_layout(const u32* p) {
    using Tegra::Host1x::Nx2PictureFormat;
    DecoderImageLayout l{};
    l.code = p[1];
    l.width = (p[2] & 0xFFFF) + 1;
    l.height = (p[2] >> 16) + 1;
    l.format = l.code == 1 || l.code == 4   ? Nx2PictureFormat::Nv12
               : l.code == 2 || l.code == 5 ? Nx2PictureFormat::Bgra8
                                            : Nx2PictureFormat::Rgba8;
    const bool nv12 = l.format == Nx2PictureFormat::Nv12;
    constexpr u64 BlockRows = 128;
    const auto align = [](u64 v, u64 a) { return (v + a - 1) / a * a; };
    l.pitch = align(u64{l.width} * (nv12 ? 1 : 4), 64);
    l.chroma_offset = l.pitch * align(l.height, BlockRows);
    l.size = l.chroma_offset + (nv12 ? l.pitch * align((u64{l.height} + 1) / 2, BlockRows) : 0);
    return l;
}

// ------------------------------------------------------------------ nx2:vi2

// Vsync events, signalled at 60 Hz by one host thread.
struct Vsync {
    std::mutex m;
    std::vector<std::shared_ptr<kern::ServiceEvent>> events;
    std::once_flag started;
    void add(std::shared_ptr<kern::ServiceEvent> ev) {
        {
            std::scoped_lock lk{m};
            events.push_back(std::move(ev));
        }
        std::call_once(started, [this] {
            std::thread([this] {
                timeBeginPeriod(1);
                const auto interval = std::chrono::nanoseconds{1'000'000'000LL / 60};
                auto next = std::chrono::steady_clock::now() + interval;
                for (;;) {
                    std::this_thread::sleep_until(next);
                    next += interval;
                    std::scoped_lock lk{m};
                    for (auto& e : events) e->signal();
                }
            }).detach();
        });
    }
};
Vsync g_vsync;

// Set by the GPU thread once it has run everything queued before it.
struct Marker {
    std::mutex m;
    std::condition_variable cv;
    bool done = false;
    void set() {
        {
            std::scoped_lock lk{m};
            done = true;
        }
        cv.notify_all();
    }
    void wait(std::chrono::nanoseconds timeout) {
        std::unique_lock lk{m};
        cv.wait_for(lk, timeout, [&] { return done; });
    }
};

struct Vi2 : Service {
    std::shared_ptr<NativeObjects> objects;
    std::vector<std::shared_ptr<kern::ServiceEvent>> held;
    u64 presents = 0;
    u32 previous_fence_id = ~0u, previous_fence_value = 0;
    u32 wait_timeouts = 0;
    u64 shown_layout = 0;
    std::chrono::steady_clock::time_point next_vblank{}, last_present{};
    std::shared_ptr<Marker> previous_marker;
    long long stats_frame = 0, stats_fence = 0;
    int stats_count = 0;
    std::chrono::steady_clock::time_point overlay_start{};
    long long overlay_fence = 0;
    int overlay_frames = 0;
    int hitches = 0;

    explicit Vi2(std::shared_ptr<NativeObjects> o) : Service("nx2:vi2"), objects(std::move(o)) {
        on(1, [](Request& r) { reply(r, {1}); });
        on(3, [](Request& r) { reply(r, {1}); });  // display count
        on(5, [](Request& r) { reply(r, {7}); });  // timestamp recorders per layer (< 8)
        on(11, [this](Request& r) {
            auto ev = std::make_shared<kern::ServiceEvent>();
            held.push_back(ev);
            r.out_copy.push_back(ev->new_handle());
        });
        auto vsync = [this](Request& r) {
            auto ev = std::make_shared<kern::ServiceEvent>();
            held.push_back(ev);
            g_vsync.add(ev);
            r.out_copy.push_back(ev->new_handle());
        };
        on(31, vsync);
        on(32, vsync);
        on(104, [](Request&) {});
        on(105, [](Request&) {});
        on(228, [](Request& r) { reply(r, {0x7fff, 0, 0, 0, 0, 0}); });  // CancelClientBuffer
        on(229, [this](Request& r) { present(r); });
    }

    // 229(u64 token, u32 slot, u32 image, ... fence ~word 22 / word 24): waits for the previous
    // frame's fence (one frame in flight), composites the image, paces to 60 Hz, hands every slot back.
    void present(Request& r) {
        presents++;
        const u32 image = W(r, 5), fence_id = ~W(r, 22), fence_value = W(r, 24);
        if (presents <= 4 || presents % 600 == 0)
            LOG("nx2:vi2 present #%llu image %u fence (%u, %u)", presents, image, fence_id, fence_value);
        // Waits for this frame's fence. HOST_FRAMES_AHEAD=1 waits for the previous frame's instead
        // (Eglantine's default, CPU and GPU overlap): with the native CPU ahead of video_core the
        // game then rewrites command memory the GPU hasn't read yet ("changed between submit and
        // execution" with EDEN_NX2_PB_CHECK=1) and the GPU runs other data as commands. Some
        // completion the game waits for is answered too early (all buffer slots are always free).
        static const bool ahead = [] {
            const char* v = std::getenv("HOST_FRAMES_AHEAD");
            return v && v[0] == '1';
        }();
        const auto entered = std::chrono::steady_clock::now();
        if (ahead) {
            wait_for_frame(previous_fence_id, previous_fence_value);
            previous_fence_id = fence_id;
            previous_fence_value = fence_value;
            // The fence covers the frame's graphics work only. Everything the game submitted
            // before the previous present (other channels too) must have been read by the GPU
            // thread before the game reuses its command memory: the console's GPU is far ahead of
            // the CPU, here the native CPU can outrun video_core.
            auto marker = std::make_shared<Marker>();
            gpu().RunInOrder([marker] { marker->set(); });
            if (previous_marker) previous_marker->wait(std::chrono::seconds{4});
            previous_marker = std::move(marker);
        } else {
            wait_for_frame(fence_id, fence_value);
        }
        const auto waited = std::chrono::steady_clock::now();
        // Hitches: a present long after the previous one (game, GPU or host stalls).
        if (last_present != std::chrono::steady_clock::time_point{}) {
            const auto gap = std::chrono::duration_cast<std::chrono::milliseconds>(waited - last_present).count();
            if (gap >= 100 && hitches++ < 200)
                LOG("nx2:vi2 hitch: %lld ms since the previous present (%lld ms of it on the GPU fence)", gap,
                    static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(waited - entered).count()));
        }
        composite(image, fence_id, fence_value);
        pace_to_vblank();
        // Where the present thread's time goes, per 600 presents.
        using us = std::chrono::microseconds;
        stats_fence += std::chrono::duration_cast<us>(waited - entered).count();
        if (last_present != std::chrono::steady_clock::time_point{})
            stats_frame += std::chrono::duration_cast<us>(entered - last_present).count();
        last_present = entered;
        // The overlay's numbers, over half a second.
        overlay_frames++;
        overlay_fence += std::chrono::duration_cast<us>(waited - entered).count();
        if (overlay_start == std::chrono::steady_clock::time_point{}) overlay_start = entered;
        if (const auto span = std::chrono::duration<double>(entered - overlay_start).count(); span >= 0.5) {
            video::report_frame_stats(overlay_frames / span, span * 1000.0 / overlay_frames,
                                      overlay_fence / 1000.0 / overlay_frames);
            overlay_start = entered;
            overlay_frames = 0;
            overlay_fence = 0;
        }
        if (++stats_count == 600) {
            LOG("nx2:vi2 last 600 presents: frame %lld us (%.1f fps), blocked on the GPU fence %lld us", stats_frame / 599,
                599e6 / std::max<long long>(stats_frame, 1), stats_fence / 600);
            stats_count = 0;
            stats_frame = stats_fence = 0;
        }
        reply(r, {0x7fff, 0, 0, 0, 0, 0});
    }

    void wait_for_frame(u32 id, u32 value) {
        if (id >= HostSyncpoints) return;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{4};
        if (!sync_ready(id, value)) {
            struct Wake {
                std::mutex m;
                std::condition_variable cv;
                bool done = false;
            };
            auto wake = std::make_shared<Wake>();
            on_sync(id, value, [wake] {
                {
                    std::scoped_lock lk{wake->m};
                    wake->done = true;
                }
                wake->cv.notify_all();
            });
            std::unique_lock lk{wake->m};
            while (!wake->done && std::chrono::steady_clock::now() < deadline)
                wake->cv.wait_for(lk, std::chrono::milliseconds{50});
        }
        if (!sync_ready(id, value) && wait_timeouts++ < 64)
            LOG("nx2:vi2 present #%llu: fence (%u, %u) not reached in 4 s (now %u)", presents, id, value,
                sync_value(id));
    }

    void pace_to_vblank() {
        const auto interval = std::chrono::nanoseconds{1'000'000'000LL / 60};
        const auto now = std::chrono::steady_clock::now();
        if (now > next_vblank + interval) next_vblank = now;
        else if (now < next_vblank) std::this_thread::sleep_until(next_vblank);
        next_vblank += interval;
    }

    void composite(u32 image_id, u32 fence_id, u32 fence_value) {
        u64 address = 0;
        ImageLayout layout{};
        {
            std::scoped_lock lk{objects->m};
            auto img = objects->images.find(image_id);
            if (img == objects->images.end()) return;
            auto memo = objects->memories.find(img->second.memory);
            if (memo == objects->memories.end() || !memo->second.address) return;
            address = memo->second.address;
            layout = image_layout(img->second.preference.data());
        }
        const auto device = Nx2GpuMemoryRegistry::Get().DeviceAddressOf(address);
        if (!device) {
            LOG("nx2:vi2 present: image %u at %#llx has no device mapping", image_id, address);
            return;
        }
        // Docked, the game draws 2560x1440 into the top-left of its 3840x2160 images.
        u32 crop_w = layout.width, crop_h = layout.height;
        if (layout.width == 3840 && layout.height == 2160) {
            crop_w = 2560;
            crop_h = 1440;
        }
        const u64 shown = u64{layout.width} << 48 | u64{layout.height} << 32 | u64{crop_w} << 16 | crop_h;
        if (shown != shown_layout) {
            shown_layout = shown;
            LOG("nx2:vi2 composite image %u: %ux%u, crop %ux%u", image_id, layout.width, layout.height, crop_w, crop_h);
        }
        std::vector<Tegra::FramebufferConfig> layers{Tegra::FramebufferConfig{
            .address = *device,
            .offset = 0,
            .width = layout.width,
            .height = layout.height,
            .stride = static_cast<u32>(layout.pitch / 4),
            .pixel_format = ::Service::android::PixelFormat::Rgba8888,
            .transform_flags = ::Service::android::BufferTransformFlags::Unset,
            .crop_rect = {0, 0, static_cast<int>(crop_w), static_cast<int>(crop_h)},
            .blending = Tegra::BlendMode::Opaque,
            .native_tiling = true,
        }};
        std::vector<::Service::Nvidia::NvFence> fences;
        if (fence_id < HostSyncpoints) fences.push_back({static_cast<s32>(fence_id), fence_value});
        gpu().RequestComposite(std::move(layers), std::move(fences));
        capture_frame();
    }

    // HOST_FRAME_CAPTURE=<dir>: every 300th present, save what the renderer shows as
    // frame-<n>.png in <dir> (to check the output of hidden test runs). HOST_FRAME_CAPTURE_EVERY=<n>
    // changes the interval (1 = every frame a capture is not still being written: flicker hunts),
    // HOST_FRAME_CAPTURE_FROM/_TO=<present> limit them to a range, HOST_FRAME_CAPTURE_WIDTH=<w> (16:9) shrinks them.
    std::shared_ptr<std::atomic<bool>> capture_pending = std::make_shared<std::atomic<bool>>(false);
    void capture_frame() {
        static const char* const dir = std::getenv("HOST_FRAME_CAPTURE");
        static const auto env_u64 = [](const char* name, u64 fallback) {
            const char* v = std::getenv(name);
            return v ? std::strtoull(v, nullptr, 10) : fallback;
        };
        static const u64 every = std::max<u64>(env_u64("HOST_FRAME_CAPTURE_EVERY", 300), 1);
        static const u64 from = env_u64("HOST_FRAME_CAPTURE_FROM", 0);
        static const u64 to = env_u64("HOST_FRAME_CAPTURE_TO", ~0ull);
        if (presents >= to) return;
        static const u32 width = static_cast<u32>(std::clamp<u64>(env_u64("HOST_FRAME_CAPTURE_WIDTH", 1280), 160, 2560));
        if (!dir || presents < from || (presents - from) % every != (every == 1 ? 0 : 1) ||
            capture_pending->exchange(true))
            return;
        const auto layout = Layout::DefaultFrameLayout(width, width * 9 / 16);
        auto pixels = std::make_shared<std::vector<u8>>(size_t{layout.width} * layout.height * 4);
        char path[512];
        std::snprintf(path, sizeof path, "%s/frame-%05llu.png", dir, presents);
        auto pending = capture_pending;
        gpu().Renderer().RequestScreenshot(
            pixels->data(),
            [pixels, layout, file = std::string{path}, pending](bool invert_y) {
                const int w = static_cast<int>(layout.width), h = static_cast<int>(layout.height);
                // The renderer writes BGRA; PNG wants RGBA with an opaque alpha.
                for (size_t i = 0; i < pixels->size(); i += 4) {
                    std::swap((*pixels)[i], (*pixels)[i + 2]);
                    (*pixels)[i + 3] = 0xff;
                }
                stbi_flip_vertically_on_write(invert_y ? 1 : 0);
                stbi_write_png(file.c_str(), w, h, 4, pixels->data(), w * 4);
                pending->store(false);
            },
            layout);
    }
};

// ------------------------------------------------------------------ video decoder

// The Switch 2 video decoder (nn::codec::video::detail::ipc), ported from Eglantine's
// INx2VideoSession (the command layouts are documented there). gsv 902/906/1002 hand out a session
// whose 1 creates a decoder service, another session of this kind. FFmpeg (Eglantine's
// Nx2VideoDecoder) decodes on a thread of the session's own into the output images the client
// registers (311) and gives back (314); each finished frame increments a syncpoint of the
// session's own, which the client waits on through the NativeSyncHandle it got for that image.
struct VideoSession : Service {
    struct Input {
        std::vector<u8> data;
        u64 pts;  // us
        bool end;
    };
    struct Decoded {
        std::optional<Tegra::Host1x::Nx2Picture> picture;  // none: an empty end-of-stream frame
        u64 pts;
        bool end;
    };
    struct Output {
        u32 image, width, height;
        u64 pts;
        bool end;
    };
    struct Fence {
        u32 syncpoint, value;
    };
    static constexpr u32 NoSyncpoint = ~0U;
    static constexpr size_t MaxQueuedInputs = 8;
    static constexpr size_t MaxDecodedAhead = 2;  // decoded frames held while no image is free

    std::shared_ptr<NativeObjects> objects;
    std::shared_ptr<kern::ServiceEvent> ready_event;
    std::atomic<bool> room{true};  // the input queue takes more frames
    u64 submitted = 0;
    u32 syncpoint = NoSyncpoint;  // counts finished frames (allocated at the first register)
    u32 sync_base = 0;

    // Shared with the decode and ready threads.
    std::mutex m;
    std::condition_variable cv;
    std::string codec;
    u32 width = 0, height = 0;
    u32 handed = 0;  // output images handed out (registered or released) so far
    u64 frames_out = 0;
    std::deque<Input> inputs;
    std::deque<Decoded> decoded;
    std::deque<u32> free_images;  // in the order the client acquires them
    std::map<u32, Fence> release_fences;
    std::deque<Output> outputs;  // finished, for 213
    bool flush = false, quit = false;
    std::thread worker, ticker;

    VideoSession(std::shared_ptr<NativeObjects> objects_, const char* name)
        : Service(name), objects(std::move(objects_)) {
        on(1, [this](Request& r) { return_session(r, std::make_shared<VideoSession>(objects, "nx2:vdecsvc")); });
        on(3, [this](Request& r) { get_event(r); });
        for (u32 c : {6u, 115u, 200u}) on(c, [](Request&) {});  // start, finalize, initialize
        on(8, [this](Request& r) { flush_queue(r); });
        on(207, [this](Request& r) { flush_queue(r); });
        on(101, [this](Request& r) { configure(r); });
        on(109, [this](Request& r) { submit(r); });
        on(212, [this](Request& r) {
            std::scoped_lock lk{m};
            const u32 image = W(r, 2);
            std::erase(free_images, image);
            release_fences.erase(image);
        });
        on(213, [this](Request& r) { output_info(r); });
        on(311, [this](Request& r) { hand_out(r, W(r, 2), std::nullopt); });
        on(314, [this](Request& r) { hand_out(r, W(r, 2), Fence{~W(r, 4), W(r, 6)}); });
    }

    ~VideoSession() override {
        {
            std::scoped_lock lk{m};
            quit = true;
        }
        cv.notify_all();
        if (worker.joinable()) worker.join();
        if (ticker.joinable()) ticker.join();
        if (syncpoint != NoSyncpoint) nv::free_syncpoint(syncpoint);
    }

    // 101(A: codec name; u32 width, u32 height, u32 rate, u32).
    void configure(Request& r) {
        std::string name;
        if (!r.a.empty()) {
            const auto bytes = r.read_in(0);
            name.assign(reinterpret_cast<const char*>(bytes.data()),
                        strnlen(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
        }
        LOG("%s configure codec \"%s\" %ux%u rate %u", this->name.c_str(), name.c_str(), r.arg<u32>(0),
            r.arg<u32>(4), r.arg<u32>(8));
        {
            std::scoped_lock lk{m};
            codec = std::move(name);
            width = r.arg<u32>(0);
            height = r.arg<u32>(4);
        }
        if (!worker.joinable()) worker = std::thread([this] { decode_loop(); });
    }

    // 3: the submission-ready event. The client attaches it auto-clearing, so while the input
    // queue has room it is signalled again every 4 ms (a spurious wake only makes it check again).
    void get_event(Request& r) {
        if (!ready_event) {
            ready_event = std::make_shared<kern::ServiceEvent>();
            ticker = std::thread([this] {
                SetThreadDescription(GetCurrentThread(), L"vdec ready");
                std::unique_lock lk{m};
                while (!cv.wait_for(lk, std::chrono::milliseconds{4}, [this] { return quit; })) {
                    if (room.load(std::memory_order_relaxed)) ready_event->signal();
                }
            });
        }
        ready_event->signal();
        r.out_copy.push_back(ready_event->new_handle());
    }

    // 311 / 314: queues an output image for the next decoded frame; replies the NativeSyncHandle
    // {~syncpoint, mode, u64 value} that signals once that frame is in it.
    void hand_out(Request& r, u32 image, std::optional<Fence> release) {
        if (syncpoint == NoSyncpoint) {
            syncpoint = nv::allocate_syncpoint();
            sync_base = sync_value(syncpoint);
            LOG("%s counts decoded frames on syncpoint %u from %u", name.c_str(), syncpoint, sync_base);
        }
        u32 value;
        {
            std::scoped_lock lk{m};
            free_images.push_back(image);
            if (release) release_fences[image] = *release;
            value = sync_base + ++handed;
        }
        cv.notify_all();
        reply(r, {~syncpoint, 0, value, 0});
    }

    // 109(A: one compressed frame, u64 pts in us, u8 end of stream): reply a u64.
    void submit(Request& r) {
        Input input{r.read_in(0), r.arg<u64>(0), r.arg<u8>(8) != 0};
        {
            std::scoped_lock lk{m};
            inputs.push_back(std::move(input));
            room = inputs.size() < MaxQueuedInputs;
        }
        cv.notify_all();
        reply64(r, {++submitted});
    }

    // 8 flush / 207 stop: drop queued input and frames not yet in an image. Images already handed
    // out keep their place, so the sync values stay in step with the client's ring.
    void flush_queue(Request&) {
        {
            std::scoped_lock lk{m};
            inputs.clear();
            flush = true;
            room = true;
        }
        cv.notify_all();
    }

    // 213: the next finished frame: image, width, height, width, height, colour space, pts (ns),
    // end of stream, padding to 16 words.
    void output_info(Request& r) {
        std::optional<Output> out;
        {
            std::scoped_lock lk{m};
            if (!outputs.empty()) {
                out = outputs.front();
                outputs.pop_front();
            }
        }
        if (!out) {
            r.result = 0x1c286f;  // nothing finished: the code the client's own Acquire uses
            return;
        }
        const u64 pts_ns = out->pts * 1000;
        reply(r, {out->image, out->width, out->height, out->width, out->height, 0, static_cast<u32>(pts_ns),
                  static_cast<u32>(pts_ns >> 32), out->end ? 1u : 0u, 0, 0, 0, 0, 0, 0, 0});
    }

    void decode_loop() {
        SetThreadDescription(GetCurrentThread(), L"vdec");
        std::unique_ptr<Tegra::Host1x::Nx2VideoDecoder> decoder;
        std::vector<Tegra::Host1x::Nx2Picture> pictures;
        std::vector<u8> bytes;
        std::unique_lock lk{m};
        for (;;) {
            cv.wait(lk, [&] {
                return quit || flush || (!decoded.empty() && !free_images.empty()) ||
                       (!inputs.empty() && decoded.size() < MaxDecodedAhead);
            });
            if (quit) return;
            if (flush) {
                flush = false;
                decoded.clear();
                if (decoder) {
                    lk.unlock();
                    decoder->Reset();
                    lk.lock();
                }
                continue;
            }
            if (!decoded.empty() && !free_images.empty()) {
                Decoded frame = std::move(decoded.front());
                decoded.pop_front();
                const u32 image = free_images.front();
                free_images.pop_front();
                std::optional<Fence> fence;
                if (auto it = release_fences.find(image); it != release_fences.end()) {
                    fence = it->second;
                    release_fences.erase(it);
                }
                lk.unlock();
                if (fence) wait_for_release(*fence);
                const auto [w, h] = write_image(decoder.get(), image, frame, bytes);
                lk.lock();
                // Even an image written during a flush counts: the client waits for its value.
                outputs.push_back({image, w, h, frame.pts, frame.end});
                syncpoints().IncrementGuest(syncpoint);
                syncpoints().IncrementHost(syncpoint);
                ++frames_out;
                if (frames_out == 1 || frames_out % 600 == 0 || frame.end)
                    LOG("%s frame %llu (%ux%u, pts %llu us%s) in image %u", name.c_str(), frames_out, w, h,
                        frame.pts, frame.end ? ", end of stream" : "", image);
                continue;
            }
            Input input = std::move(inputs.front());
            inputs.pop_front();
            room = inputs.size() < MaxQueuedInputs;
            const std::string want = codec;
            lk.unlock();
            if (!decoder || decoder->Name() != want) decoder = std::make_unique<Tegra::Host1x::Nx2VideoDecoder>(want);
            pictures.clear();
            decoder->Decode(input.data, input.pts, pictures);
            if (input.end) decoder->Drain(pictures);
            lk.lock();
            if (flush) continue;
            for (auto& p : pictures) {
                const u64 pts = p.pts;
                decoded.push_back({std::move(p), pts, false});
            }
            if (input.end) {
                // The client stops at the frame marked end of stream; give it one even when the
                // last input held no picture.
                if (decoded.empty()) decoded.push_back({std::nullopt, input.pts, true});
                else decoded.back().end = true;
            }
        }
    }

    // Waits (bounded) until the client's reads of an image it gave back have passed.
    void wait_for_release(const Fence& f) {
        if (f.syncpoint >= HostSyncpoints) return;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds{100};
        while (!sync_ready(f.syncpoint, f.value) && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }

    // Writes a decoded frame into an output image (through the view, so pages the GPU caches are
    // invalidated by the write fault); returns the size the frame reports.
    std::pair<u32, u32> write_image(Tegra::Host1x::Nx2VideoDecoder* decoder, u32 image_id, const Decoded& frame,
                                    std::vector<u8>& bytes) {
        u64 address = 0;
        DecoderImageLayout l{};
        {
            std::scoped_lock lk{objects->m};
            const auto img = objects->images.find(image_id);
            if (img == objects->images.end()) return {width, height};
            l = decoder_image_layout(img->second.preference.data());
            if (auto mm = objects->memories.find(img->second.memory); mm != objects->memories.end() && mm->second.address)
                address = mm->second.address + img->second.offset;
        }
        if (!frame.picture || !decoder || !address) return {std::min(width, l.width), std::min(height, l.height)};
        // A frame larger than the image is scaled down to it.
        const u32 w = std::min(frame.picture->width, l.width);
        const u32 h = std::min(frame.picture->height, l.height);
        bytes.resize(l.size);
        decoder->Render(*frame.picture, l.format, w, h, l.pitch, l.chroma_offset, bytes);
        std::memcpy(gptr(address), bytes.data(), l.pitch * h);
        if (l.format == Tegra::Host1x::Nx2PictureFormat::Nv12)
            std::memcpy(gptr(address + l.chroma_offset), bytes.data() + l.chroma_offset, l.pitch * ((h + 1) / 2));
        return {w, h};
    }
};

// ------------------------------------------------------------------ nx2:gsv

struct Gsv : Service {
    std::shared_ptr<NativeObjects> objects = std::make_shared<NativeObjects>();

    // Syncpoint memory (204): RO page header + values, RW armed bits; polled every 500 us.
    static constexpr u32 SyncptValuesOffset = 0x40, SyncptRoSummaryOffset = 0x1040, SyncptRoBitsOffset = 0x1100;
    static constexpr u32 SyncptRwSummaryOffset = 0x0, SyncptRwBitsOffset = 0x100;
    std::shared_ptr<kern::ServiceEvent> syncpt_request, syncpt_notify;
    std::atomic<u64> syncpt_rw{0}, syncpt_ro{0};
    std::atomic<u32> syncpt_count{0};

    Gsv() : Service("nx2:gsv") {
        for (u32 c : {4u, 5u, 101u, 901u, 1001u, 601u, 1201u}) on(c, [](Request&) {});
        on(202, [](Request& r) { return_session(r, std::make_shared<Syncpt>()); });
        on(305, [this](Request& r) { return_session(r, std::make_shared<Vi2>(objects)); });
        on(497, [](Request& r) { return_session(r, std::make_shared<Gpu>()); });
        on(1402, [](Request& r) { return_session(r, nv::make_nvsrv()); });
        on(602, [](Request& r) { return_session(r, stub("nx2:camera")); });
        on(1202, [](Request& r) { return_session(r, stub("nx2:chat:gsv")); });
        for (u32 c : {902u, 906u, 1002u}) {
            on(c, [this](Request& r) {
                return_session(r, std::make_shared<VideoSession>(objects, r.cmd == 1002 ? "nx2:vdec2" : "nx2:vdec"));
            });
        }
        // 991 CalculateDecoderImagePoolBuffer(preference, u32 count): size, alignment.
        on(991, [](Request& r) {
            const auto p = preference_at(r, 2);
            const auto l = decoder_image_layout(p.data());
            const u32 count = std::max<u32>(W(r, 18), 1);
            LOG("nx2:gsv 991 decoder image pool code %u %ux%u count %u -> size %#llx", l.code, l.width, l.height,
                count, l.size * count);
            reply64(r, {l.size * count, 64 * 128});
        });
        // 911 CreateDecoderImage(preference, u32 count, u32 memory id): the first of `count` images,
        // one image size apart in the memory.
        on(911, [this](Request& r) {
            const u32 count = std::max<u32>(W(r, 18), 1);
            const auto p = preference_at(r, 2);
            const auto l = decoder_image_layout(p.data());
            std::scoped_lock lk{objects->m};
            const u32 first = objects->next_image;
            objects->next_image += count;
            for (u32 i = 0; i < count; i++) {
                auto& img = objects->images[first + i];
                img.memory = W(r, 19);
                img.preference = p;
                img.size = l.size;
                img.offset = l.size * i;
                img.decoder = true;
            }
            LOG("nx2:gsv 911 decoder images %u..%u code %u %ux%u in memory %u", first, first + count - 1, l.code,
                l.width, l.height, W(r, 19));
            reply(r, {first});
        });
        // 912(image): destroy a decoder output image; 903 / 1003 finalize the decoder sessions.
        on(912, [this](Request& r) {
            std::scoped_lock lk{objects->m};
            objects->images.erase(W(r, 2));
        });
        for (u32 c : {903u, 1003u}) on(c, [](Request&) {});
        // 961 GetCpuUserImageDescriptor(image): the 128-byte descriptor nn::codec reads output
        // images with: u8 kind (8 RGBA, 9 BGRA, 10 NV12), plane offset (+0x8) and pitch (+0x10),
        // NV12's chroma plane at +0x18 / +0x20.
        on(961, [this](Request& r) {
            using Tegra::Host1x::Nx2PictureFormat;
            std::array<u32, 32> w{};
            std::scoped_lock lk{objects->m};
            if (auto it = objects->images.find(W(r, 2)); it != objects->images.end() && it->second.decoder) {
                const auto l = decoder_image_layout(it->second.preference.data());
                w[0] = l.format == Nx2PictureFormat::Nv12 ? 0xa : l.format == Nx2PictureFormat::Bgra8 ? 0x9 : 0x8;
                w[2] = static_cast<u32>(it->second.offset);
                w[4] = static_cast<u32>(l.pitch);
                if (l.format == Nx2PictureFormat::Nv12) {
                    w[6] = static_cast<u32>(it->second.offset + l.chroma_offset);
                    w[8] = static_cast<u32>(l.pitch);
                }
            }
            for (u32 x : w) r.push(x);
        });
        on(204, [this](Request& r) { attach_syncpt_memory(r); });

        // 1510 CalculateRequirements(preference) / 1291: size, alignment.
        on(1510, [](Request& r) {
            const auto p = preference_at(r, 2);
            const auto l = image_layout(p.data());
            reply64(r, {l.size, l.alignment});
        });
        on(1291, [](Request& r) {
            const auto p = preference_at(r, 2);
            const auto l = image_layout(p.data());
            reply64(r, {l.size * std::max<u32>(W(r, 18), 1), l.alignment});
        });
        // 435 CalculateNativeImageSize(preference, count).
        on(435, [](Request& r) {
            const auto p = preference_at(r, 2);
            reply64(r, {image_layout(p.data()).size * std::max<u32>(W(r, 18), 1)});
        });
        // 111 CreateNativeMemory(u8 mode, u64 size) -> id.
        on(111, [this](Request& r) {
            std::scoped_lock lk{objects->m};
            const u32 id = objects->next_memory++;
            objects->memories[id] = {W64(r, 3), 0};
            LOG("nx2:gsv 111 CreateNativeMemory size %#llx -> %u", W64(r, 3), id);
            reply(r, {id});
        });
        on(112, [this](Request& r) {
            std::scoped_lock lk{objects->m};
            objects->memories.erase(W(r, 2));
        });
        // 114 BindNativeMemory / 423 BindNativeGpuMemory(id, u64 address, u64 size, ...).
        auto bind = [this](Request& r) {
            std::scoped_lock lk{objects->m};
            auto& m = objects->memories[W(r, 2)];
            m.address = W64(r, 3);
            if (!m.size) m.size = W64(r, 5);
            LOG("nx2:gsv %u bind native memory %u at %#llx+%#llx", r.cmd, W(r, 2), m.address, W64(r, 5));
        };
        on(114, bind);
        on(423, bind);
        // 429 CreateNativeImage(preference, count, memory id) / 1211 -> id.
        auto create_image = [this](Request& r) {
            std::scoped_lock lk{objects->m};
            const u32 id = objects->next_image;
            auto& img = objects->images[id];
            img.count = std::max<u32>(W(r, 18), 1);
            objects->next_image += img.count;
            img.memory = W(r, 19);
            img.preference = preference_at(r, 2);
            img.size = image_layout(img.preference.data()).size;
            LOG("nx2:gsv %u CreateNativeImage memory %u count %u size %#llx -> %u", r.cmd, img.memory, img.count,
                img.size, id);
            reply(r, {id});
        };
        on(429, create_image);
        on(1211, create_image);
        on(430, [this](Request& r) {
            std::scoped_lock lk{objects->m};
            objects->images.erase(W(r, 2));
        });
        // 315 / 314: add / remove a window alias image pool.
        auto pool = [this](Request& r) {
            std::scoped_lock lk{objects->m};
            if (auto it = objects->images.find(W(r, 2)); it != objects->images.end()) it->second.window_pool = r.cmd == 315;
        };
        on(314, pool);
        on(315, pool);
        on(426, [this](Request& r) { query_image(r); });
        on(428, [this](Request& r) { query_image(r); });
    }

    // 426 MapNativeImage / 428 QueryNativeImage(handle): GPU address (426), offset, descriptor.
    void query_image(Request& r) {
        const u32 handle = W(r, 2);
        std::array<u32, 16> pref{};
        u64 offset = 0, address = 0;
        {
            std::scoped_lock lk{objects->m};
            auto it = objects->images.upper_bound(handle);
            if (it != objects->images.begin()) {
                --it;
                if (handle - it->first < it->second.count) {
                    pref = it->second.preference;
                    offset = it->second.offset + (handle - it->first) * it->second.size;
                    if (auto m = objects->memories.find(it->second.memory); m != objects->memories.end())
                        address = m->second.address;
                }
            }
        }
        const auto l = image_layout(pref.data());
        const u64 gpu_address = r.cmd == 426 ? address + offset : 0;
        std::array<u32, 37> w{};
        w[0] = static_cast<u32>(gpu_address);
        w[1] = static_cast<u32>(gpu_address >> 32);
        w[2] = static_cast<u32>(offset);
        w[3] = static_cast<u32>(offset >> 32);
        u32* d = w.data() + 4;
        d[0] = 1;
        d[1] = pref[1];
        d[2] = pref[2];
        d[4] = l.block_height_log2;
        d[5] = static_cast<u32>(l.pitch / 64);
        d[8] = 1;
        d[9] = 1;
        d[11] = static_cast<u32>(l.size);
        for (u32 x : w) r.push(x);
    }

    void attach_syncpt_memory(Request& r) {
        const u32 max_syncpoints = std::min<u32>(W(r, 3), (0x1000 - 0x40) / 8);
        auto rw = r.copy_handles.size() > 1 ? kern::get<kern::KTransferMemory>(r.copy_handles[0]) : nullptr;
        auto ro = r.copy_handles.size() > 1 ? kern::get<kern::KTransferMemory>(r.copy_handles[1]) : nullptr;
        if (rw && ro && ro->size >= 0x2000) {
            syncpt_rw = rw->addr;
            syncpt_ro = ro->addr;
            syncpt_count = max_syncpoints;
            const u64 base = ro->addr;
            const u32 header[8] = {1, 0, max_syncpoints, SyncptValuesOffset, SyncptRwSummaryOffset,
                                   SyncptRoSummaryOffset, SyncptRwBitsOffset, SyncptRoBitsOffset};
            std::memcpy(mem::backing_ptr(base), header, sizeof header);
            if (!syncpt_request) {
                syncpt_request = std::make_shared<kern::ServiceEvent>();
                syncpt_notify = std::make_shared<kern::ServiceEvent>();
                std::thread([self = std::static_pointer_cast<Gsv>(shared_from_this())] {
                    for (;;) {
                        std::this_thread::sleep_for(std::chrono::microseconds{500});
                        self->poll_syncpts();
                    }
                }).detach();
            }
        } else {
            LOG("nx2:gsv 204: missing syncpt transfer memory");
            if (!syncpt_request) {
                syncpt_request = std::make_shared<kern::ServiceEvent>();
                syncpt_notify = std::make_shared<kern::ServiceEvent>();
            }
        }
        r.out_copy.push_back(syncpt_request->new_handle());
        r.out_copy.push_back(syncpt_notify->new_handle());
    }

    static u64& at64(u64 addr) { return *reinterpret_cast<u64*>(mem::backing_ptr(addr & ~(mem::PageSize - 1)) + (addr & (mem::PageSize - 1))); }

    // Mirrors the guest syncpoint values into the RO page and acknowledges every armed wait.
    void poll_syncpts() {
        const u64 ro = syncpt_ro, rw = syncpt_rw;
        const u32 count = syncpt_count;
        if (!ro) return;
        for (u32 id = 0; id < std::min(count, HostSyncpoints); id++) at64(ro + SyncptValuesOffset + id * 8) = sync_value(id);
        bool acked = false;
        for (u32 w = 0; w < (count + 63) / 64; w++) {
            const u64 bits = at64(rw + SyncptRwBitsOffset + w * 8);
            u64& ack = at64(ro + SyncptRoBitsOffset + w * 8);
            if (ack != bits) {
                ack = bits;
                acked = true;
            }
        }
        at64(ro + SyncptRoSummaryOffset) = at64(rw + SyncptRwSummaryOffset);
        if (acked) syncpt_notify->signal();
    }
};

}  // namespace

void register_gsv() {
    register_service("nx2:gsv", [] { return std::make_shared<Gsv>(); });
}

}  // namespace ipc
