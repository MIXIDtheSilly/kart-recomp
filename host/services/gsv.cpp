// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// The Switch 2 graphics server as the SDK sees it (nx2:gsv and the sessions it hands out: syncpt, gpu, nvsrv, the
// video decoder). Ported from Eglantine's nx2_gsv.h; command layouts are documented there. With the native renderer
// (nvn/) the game uses it for its syncpoints, native memory and images, and the video decoder.

#include <windows.h>

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <map>
#include <optional>
#include <thread>

#include "../ipc.h"
#include "../memory.h"
#include "../syncpoints.h"
#include "vdec_mf.h"

namespace ipc {
namespace {

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

constexpr u32 HostSyncpoints = sync::Count;

u32 sync_value(u32 id) { return sync::value(id); }
bool sync_ready(u32 id, u32 value) { return sync::ready(id, value); }
template <typename F> sync::Handle on_sync(u32 id, u32 value, F&& f) { return sync::on_reach(id, value, std::forward<F>(f)); }
void cancel_sync(u32 id, sync::Handle h) { sync::cancel(id, h); }

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

// ------------------------------------------------------------------ nx2:chat:gsv

// GameChat's graphics side (nn::chat::gsv), from gsv 1202; opened by 1.8.0 at boot. As Eglantine's INx2ChatGsvSession:
// 1() returns an event id (the SDK aborts with 2162-0001 over 0x3ff), 11(u32 id) that id's event, never signalled.
// As a plain stub, 11 returned no handle and the game waited on handle 0 in a loop.
struct ChatGsv : Service {
    EventMap events;
    ChatGsv() : Service("nx2:chat:gsv") {
        on(1, [](Request& r) { reply(r, {0}); });
        on(11, [this](Request& r) { r.out_copy.push_back(events.get(W(r, 2))->new_handle()); });
    }
};

// ------------------------------------------------------------------ nx2:syncpt

struct Syncpt : Service {
    struct SlotRequest {
        u32 syncpoint = 0;
        std::atomic<bool> pending{false};
        sync::Handle handle = 0;
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
            TIMELINE("syncwait slot=%u fence=%u:%u now=%u", slot, id, value, sync_value(id));
            req.handle = on_sync(id, value, [sh = shared, &req, ev, slot, id, value] {
                TIMELINE("syncdone slot=%u fence=%u:%u", slot, id, value);
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

// The game opens it during the console graphics boot and asks for its id range, events and a sub-session; GPU
// memory and channels are the native renderer's (nvn/).
struct Gpu : Service {
    static constexpr u32 NumIds = 32;
    EventMap events;
    std::shared_ptr<kern::ServiceEvent> event(u32 id) { return events.get(id % NumIds); }

    Gpu() : Service("nx2:gpu") {
        on(11, [](Request& r) { reply(r, {0, NumIds - 1}); });
        on(12, [this](Request& r) { r.out_copy.push_back(event(W(r, 2))->new_handle()); });
        on(21, [this](Request& r) { event(W(r, 2))->signal(); });
        on(22, [this](Request& r) { return_session(r, shared_from_this()); });  // sub-session
    }
};

// nvsrv (the nvdrv session the boot opens): only 13, SetGraphicsFirmwareMemoryMarginEnabled.
struct Nvsrv : Service {
    Nvsrv() : Service("nx2:nvsrv") { on(13, [](Request&) {}); }
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
    vdec::Format format;
    u64 pitch, chroma_offset, size;
};

DecoderImageLayout decoder_image_layout(const u32* p) {
    using Nx2PictureFormat = vdec::Format;
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
        std::optional<vdec::Picture> picture;  // none: an empty end-of-stream frame
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
    u64 last_pts = 0;  // of the newest frame written
    std::deque<Input> inputs;
    std::deque<Decoded> decoded;
    std::deque<u32> free_images;  // in the order the client acquires them
    std::map<u32, Fence> release_fences;
    std::deque<Output> outputs;  // finished, for 213
    bool flush = false, quit = false;
    bool at_end = false;  // the frame marked end of stream is out and no input came since
    std::thread worker, ticker;

    VideoSession(std::shared_ptr<NativeObjects> objects_, const char* name)
        : Service(name), objects(std::move(objects_)) {
        on(1, [this](Request& r) { return_session(r, std::make_shared<VideoSession>(objects, "nx2:vdecsvc")); });
        on(3, [this](Request& r) { get_event(r); });
        for (u32 c : {6u, 115u, 200u}) on(c, [](Request&) {});  // start, finalize, initialize
        on(8, [this](Request& r) { flush_queue(r, "flush"); });
        on(207, [this](Request& r) { flush_queue(r, "stop"); });
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
        if (syncpoint != NoSyncpoint) sync::release(syncpoint);
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
            syncpoint = sync::allocate();
            sync_base = sync_value(syncpoint);
            LOG("%s counts decoded frames on syncpoint %u from %u", name.c_str(), syncpoint, sync_base);
        }
        u32 value;
        {
            std::scoped_lock lk{m};
            free_images.push_back(image);
            if (release) release_fences[image] = *release;
            value = sync_base + ++handed;
            release_unfilled();
        }
        cv.notify_all();
        reply(r, {~syncpoint, 0, value, 0});
    }

    // At the end of the stream (nothing left to decode), the images handed out that no frame will fill: their values
    // signalled all the same, without an output (213 answers "nothing finished", which the client skips). When the
    // movie stops, the client drains its ring of handed-out images and aborts on a value not reached
    // (subsdk0+0x4540 after nn::gs::TryWaitNativeSync: svcBreak after IApplicationFunctions::22); stopping after the
    // last frame with images still out did that now and then. (m held)
    // Also at a flush or stop (any_time), where nothing queued will fill them either. Returns how many.
    u32 release_unfilled(bool any_time = false) {
        if ((!any_time && (!at_end || !inputs.empty() || !decoded.empty())) || syncpoint == NoSyncpoint) return 0;
        u32 n = 0;
        for (; !free_images.empty(); free_images.pop_front(), n++) {
            release_fences.erase(free_images.front());
            sync::increment(syncpoint);
        }
        return n;
    }

    // 109(A: one compressed frame, u64 pts in us, u8 end of stream): reply a u64.
    void submit(Request& r) {
        Input input{r.read_in(0), r.arg<u64>(0), r.arg<u8>(8) != 0};
        {
            std::scoped_lock lk{m};
            inputs.push_back(std::move(input));
            room = inputs.size() < MaxQueuedInputs;
            at_end = false;
        }
        cv.notify_all();
        reply64(r, {++submitted});
    }

    // 8 flush / 207 stop: drop queued input and frames not yet in an image. The images handed out that are not being
    // written now get their values without a frame (as at the end of the stream: 213 answers "nothing finished"), so the
    // sync values stay in step with the client's ring: a movie stopped mid-stream drains its ring and aborted on a value
    // never reached (subsdk0+0x4540, svcBreak after IApplicationFunctions::22, with no end of stream in the log). An
    // image the decode loop is writing now was already taken off free_images and gets its value when written.
    void flush_queue(Request&, const char* what) {
        u32 released, queued;
        u64 frames;
        {
            std::scoped_lock lk{m};
            frames = frames_out;
            queued = static_cast<u32>(inputs.size() + decoded.size());
            inputs.clear();
            decoded.clear();
            flush = true;
            room = true;
            released = release_unfilled(true);
        }
        cv.notify_all();
        LOG("%s %s after %llu frames: %u queued dropped, %u handed-out images given their values", name.c_str(), what,
            static_cast<unsigned long long>(frames), queued, released);
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
        std::unique_ptr<vdec::Decoder> decoder;
        std::vector<vdec::Picture> pictures;
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
                last_pts = frame.pts;
                sync::increment(syncpoint);
                ++frames_out;
                if (frame.end) at_end = true, release_unfilled();
                if (frames_out == 1 || frames_out % 600 == 0 || frame.end)
                    LOG("%s frame %llu (%ux%u, pts %llu us%s) in image %u", name.c_str(), frames_out, w, h,
                        frame.pts, frame.end ? ", end of stream" : "", image);
                continue;
            }
            Input input = std::move(inputs.front());
            inputs.pop_front();
            room = inputs.size() < MaxQueuedInputs;
            const std::string want = codec;
            const u32 want_w = width, want_h = height;
            lk.unlock();
            if (!decoder || decoder->Name() != want) decoder = std::make_unique<vdec::Decoder>(want, want_w, want_h);
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
                // The client stops at the frame marked end of stream. Media Foundation often hands out
                // the last picture before the (empty) end-of-stream input comes, so the newest frame the
                // client has not fetched yet (213) is marked, written or not. Only when it has fetched
                // them all does it get an empty frame, with the last frame's time: the input's own pts is
                // ~0, and an extra frame with it made the 1.8.0 movie module abort (once in 5 runs).
                if (!decoded.empty()) decoded.back().end = true;
                else if (!outputs.empty()) {
                    outputs.back().end = true;
                    at_end = true;
                    release_unfilled();
                    LOG("%s frame %llu marked end of stream (the decoder had nothing left)", name.c_str(), frames_out);
                } else {
                    decoded.push_back({std::nullopt, input.pts == ~0ull ? last_pts : input.pts, true});
                }
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
    std::pair<u32, u32> write_image(vdec::Decoder* decoder, u32 image_id, const Decoded& frame,
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
        vdec::Decoder::Render(*frame.picture, l.format, w, h, l.pitch, l.chroma_offset, bytes);
        std::memcpy(gptr(address), bytes.data(), l.pitch * h);
        if (l.format == vdec::Format::Nv12)
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
        on(305, [](Request& r) { return_session(r, stub("nx2:vi2")); });
        on(497, [](Request& r) { return_session(r, std::make_shared<Gpu>()); });
        on(1402, [](Request& r) { return_session(r, std::make_shared<Nvsrv>()); });
        on(602, [](Request& r) { return_session(r, stub("nx2:camera")); });
        on(1202, [](Request& r) { return_session(r, std::make_shared<ChatGsv>()); });
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
            using Nx2PictureFormat = vdec::Format;
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
        // The request event is the client's to signal (as Eglantine: the writable side). 1.8.0's movie module signals it
        // from nn::syncpt::sdk::TryWaitSyncPoint; with a readable handle that failed (invalid handle) and the game aborted.
        r.out_copy.push_back(syncpt_request->new_writable_handle());
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
