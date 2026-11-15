// par_for_each: std::for_each(std::execution::par, ...). MSVC builds use exactly that; libc++ (the llvm-mingw build)
// has no parallel algorithms, and libstdc++ (Linux) has them only with TBB, so there it runs on a thread pool
// (os::run_on_pool: on Windows the system thread pool, which is where MSVC's runs it too).
#pragma once
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <mutex>
#ifdef _MSC_VER
#include <execution>
#endif

#include "os.h"

template <class It, class F> void par_for_each(It first, It last, F f) {
#ifdef _MSC_VER
    std::for_each(std::execution::par, first, last, f);
#else
    const size_t n = static_cast<size_t>(last - first);
    const size_t helpers = std::min<size_t>(n ? n - 1 : 0, os::cpu_count() - 1);
    if (!helpers) {
        for (; first != last; ++first) f(*first);
        return;
    }
    struct Shared {
        It first;
        size_t n;
        F* f;
        std::atomic<size_t> next{0};
        size_t running = 0;
        std::mutex lock;
        std::condition_variable done;
        void work() {
            for (size_t i; (i = next.fetch_add(1)) < n;) (*f)(first[i]);
        }
        void leave() {
            std::scoped_lock lk{lock};
            if (--running == 0) done.notify_all();
        }
    } s{first, n, &f};
    s.running = helpers;
    for (size_t k = 0; k < helpers; k++) os::run_on_pool([&s] {
        s.work();
        s.leave();
    });
    s.work();
    std::unique_lock lk{s.lock};
    s.done.wait(lk, [&] { return s.running == 0; });
#endif
}
