// SPDX-FileCopyrightText: Copyright 2023 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// BC1/BC3 encoding of RGBA8 for formats the host GPU lacks (from Eden's video_core/textures/bcn.cpp; the blocks are
// stb_dxt's), and the transcode threads both codecs run on.
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <thread>

#include "codecs.h"
#include "stb_dxt.h"

namespace codec {
namespace {

// One parallel_for call: indices are handed out under the pool's lock, so once the caller has taken its job off the
// queue no thread can start another index of it.
struct Job {
    const std::function<void(u32)>* fn;
    u32 n;
    u32 next = 0;
    std::atomic<u32> done{0};
};

struct Pool {
    std::mutex lock;
    std::condition_variable work, finished;
    std::deque<Job*> queue;

    Pool() {
        u32 n = std::max(std::thread::hardware_concurrency() / 4, 2u);
        if (const char* v = std::getenv("HOST_TRANSCODE_THREADS")) n = std::max<u32>(std::strtoul(v, nullptr, 10), 1);
        for (u32 i = 0; i < n; i++) {
            std::thread([this, i] {
                SetThreadDescription(GetCurrentThread(), (L"transcode " + std::to_wstring(i)).c_str());
                run();
            }).detach();
        }
    }

    // The next index of the oldest job with any left, or nullptr when there is none.
    Job* take(u32& index) {
        while (!queue.empty()) {
            Job* j = queue.front();
            if (j->next < j->n) {
                index = j->next++;
                return j;
            }
            queue.pop_front();
        }
        return nullptr;
    }

    void finish(Job* j) {
        // Nothing of the job is touched after this: its caller may return as soon as done reaches n.
        if (j->done.fetch_add(1) + 1 == j->n) {
            std::lock_guard lk{lock};
            finished.notify_all();
        }
    }

    void run() {
        for (;;) {
            u32 index = 0;
            Job* j = nullptr;
            {
                std::unique_lock lk{lock};
                work.wait(lk, [&] { return (j = take(index)) != nullptr; });
            }
            (*j->fn)(index);
            finish(j);
        }
    }

    void parallel_for(u32 n, const std::function<void(u32)>& fn) {
        if (n == 0) return;
        if (n == 1) return fn(0);
        Job job{&fn, n};
        {
            std::lock_guard lk{lock};
            queue.push_back(&job);
        }
        work.notify_all();
        for (;;) {
            u32 index = 0;
            {
                std::lock_guard lk{lock};
                if (job.next >= job.n) {
                    std::erase(queue, &job);
                    break;
                }
                index = job.next++;
            }
            fn(index);
            finish(&job);
        }
        std::unique_lock lk{lock};
        finished.wait(lk, [&] { return job.done.load() == job.n; });
    }
};

Pool& pool() {
    static Pool p;
    return p;
}

using BlockEncoder = void (*)(u8* block_output, const u8* block_input, bool any_alpha);

template <u32 BytesPerBlock, bool ThresholdAlpha>
void encode(std::span<const u8> data, u32 width, u32 height, u32 depth, std::span<u8> output, BlockEncoder f) {
    constexpr u8 alpha_threshold = 128;
    constexpr u32 bytes_per_px = 4;
    const u32 plane_dim = width * height;
    const u32 bytes_per_row = BytesPerBlock * ((width + 3) / 4);
    const u32 bytes_per_plane = bytes_per_row * ((height + 3) / 4);

    for (u32 z = 0; z < depth; z++) {
        parallel_for((height + 3) / 4, [&](u32 row) {
            const u32 y = row * 4;
            for (u32 x = 0; x < width; x += 4) {
                // Gather 4x4 block of RGBA texels
                u8 input_colors[4][4][4];
                bool any_alpha = false;

                for (u32 j = 0; j < 4; j++) {
                    for (u32 i = 0; i < 4; i++) {
                        const size_t coord = (z * plane_dim + (y + j) * width + (x + i)) * bytes_per_px;

                        if ((x + i < width) && (y + j < height)) {
                            if constexpr (ThresholdAlpha) {
                                if (data[coord + 3] >= alpha_threshold) {
                                    input_colors[j][i][0] = data[coord + 0];
                                    input_colors[j][i][1] = data[coord + 1];
                                    input_colors[j][i][2] = data[coord + 2];
                                    input_colors[j][i][3] = 255;
                                } else {
                                    any_alpha = true;
                                    std::memset(input_colors[j][i], 0, bytes_per_px);
                                }
                            } else {
                                std::memcpy(input_colors[j][i], &data[coord], bytes_per_px);
                            }
                        } else {
                            std::memset(input_colors[j][i], 0, bytes_per_px);
                        }
                    }
                }

                f(output.data() + z * bytes_per_plane + row * bytes_per_row + (x / 4) * BytesPerBlock,
                  reinterpret_cast<u8*>(input_colors), any_alpha);
            }
        });
    }
}

}  // namespace

void parallel_for(u32 n, const std::function<void(u32)>& fn) {
    pool().parallel_for(n, fn);
}

void bc1_encode(std::span<const u8> data, u32 width, u32 height, u32 depth, std::span<u8> output) {
    encode<8, true>(data, width, height, depth, output, [](u8* out, const u8* in, bool any_alpha) {
        stb_compress_bc1_block(out, in, any_alpha, STB_DXT_NORMAL);
    });
}

void bc3_encode(std::span<const u8> data, u32 width, u32 height, u32 depth, std::span<u8> output) {
    encode<16, false>(data, width, height, depth, output, [](u8* out, const u8* in, bool) {
        stb_compress_bc3_block(out, in, STB_DXT_NORMAL);
    });
}

}  // namespace codec
