// par_for_each: std::for_each(std::execution::par, ...) on the Windows thread pool, which is where MSVC's standard
// library runs it. libc++ (the llvm-mingw build) has no parallel algorithms.
#pragma once
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstddef>

template <class It, class F> void par_for_each(It first, It last, F f) {
    const size_t n = static_cast<size_t>(last - first);
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    const size_t helpers = std::min<size_t>(n ? n - 1 : 0, si.dwNumberOfProcessors ? si.dwNumberOfProcessors - 1 : 0);
    if (!helpers) {
        for (; first != last; ++first) f(*first);
        return;
    }
    struct Shared {
        It first;
        size_t n;
        F* f;
        std::atomic<size_t> next{0}, running{0};
        HANDLE done;
        void work() {
            for (size_t i; (i = next.fetch_add(1)) < n;) (*f)(first[i]);
        }
        void leave() {
            if (running.fetch_sub(1) == 1) SetEvent(done);
        }
    } s{first, n, &f};
    s.running = helpers;
    s.done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    for (size_t k = 0; k < helpers; k++) {
        const auto cb = [](PTP_CALLBACK_INSTANCE, void* p) {
            auto* sh = static_cast<Shared*>(p);
            sh->work();
            sh->leave();
        };
        if (!TrySubmitThreadpoolCallback(cb, &s, nullptr)) s.leave();
    }
    s.work();
    WaitForSingleObject(s.done, INFINITE);
    CloseHandle(s.done);
}
