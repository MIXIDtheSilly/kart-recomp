#include "os.h"

#include <algorithm>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#else
#include <dirent.h>
#include <execinfo.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <ucontext.h>

#include <atomic>
#include <fstream>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>
#endif

namespace os {

#ifdef _WIN32

void set_thread_name(const char* name) {
    wchar_t wide[128];
    if (MultiByteToWideChar(CP_UTF8, 0, name, -1, wide, 128)) SetThreadDescription(GetCurrentThread(), wide);
}

void set_thread_priority(Priority p) {
    static constexpr int levels[] = {THREAD_PRIORITY_LOWEST, THREAD_PRIORITY_BELOW_NORMAL, THREAD_PRIORITY_NORMAL,
                                     THREAD_PRIORITY_ABOVE_NORMAL};
    SetThreadPriority(GetCurrentThread(), levels[static_cast<int>(p)]);
}

void timer_sleep_ns(int64_t ns) {
    thread_local HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    LARGE_INTEGER due;
    due.QuadPart = -std::max<int64_t>(1, ns / 100);
    if (timer && SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) {
        WaitForSingleObject(timer, INFINITE);
        return;
    }
    Sleep(static_cast<DWORD>(ns / 1000000));
}

void yield() { SwitchToThread(); }

std::filesystem::path exe_path() {
    wchar_t exe[MAX_PATH];
    const DWORD n = GetModuleFileNameW(nullptr, exe, MAX_PATH);
    return std::filesystem::path(std::wstring(exe, n));
}

FILE* open_file(const std::filesystem::path& path, const char* mode) {
    wchar_t wmode[8]{};
    for (int i = 0; i < 7 && mode[i]; i++) wmode[i] = static_cast<wchar_t>(mode[i]);
    return _wfopen(path.c_str(), wmode);
}

void set_env(const char* key, const char* value) { _putenv_s(key, value); }

void* alloc_pages(size_t bytes) { return VirtualAlloc(nullptr, bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE); }

uint64_t thread_id() { return GetCurrentThreadId(); }

unsigned cpu_count() {
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return si.dwNumberOfProcessors ? si.dwNumberOfProcessors : 1;
}

void run_on_pool(std::function<void()> f) {
    auto* job = new std::function<void()>(std::move(f));
    const auto cb = [](PTP_CALLBACK_INSTANCE, void* p) {
        auto* j = static_cast<std::function<void()>*>(p);
        (*j)();
        delete j;
    };
    if (!TrySubmitThreadpoolCallback(cb, job, nullptr)) {
        (*job)();
        delete job;
    }
}

#else  // Linux

void set_thread_name(const char* name) {
    char shortname[16];   // the kernel keeps 15 characters
    std::snprintf(shortname, sizeof shortname, "%s", name);
    pthread_setname_np(pthread_self(), shortname);
}

void set_thread_priority(Priority p) {
    static constexpr int nice[] = {10, 5, 0, -5};
    setpriority(PRIO_PROCESS, static_cast<id_t>(syscall(SYS_gettid)), nice[static_cast<int>(p)]);
}

void timer_sleep_ns(int64_t ns) {
    if (ns <= 0) return;
    timespec ts{static_cast<time_t>(ns / 1000000000), static_cast<long>(ns % 1000000000)};
    while (clock_nanosleep(CLOCK_MONOTONIC, 0, &ts, &ts) == EINTR) {}
}

void yield() { sched_yield(); }

std::filesystem::path exe_path() {
    std::error_code ec;
    auto p = std::filesystem::read_symlink("/proc/self/exe", ec);
    return ec ? std::filesystem::path() : p;
}

FILE* open_file(const std::filesystem::path& path, const char* mode) { return std::fopen(path.c_str(), mode); }

void set_env(const char* key, const char* value) { setenv(key, value, 1); }

void* alloc_pages(size_t bytes) {
    void* p = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return p == MAP_FAILED ? nullptr : p;
}

uint64_t thread_id() { return static_cast<uint64_t>(syscall(SYS_gettid)); }

unsigned cpu_count() {
    const unsigned n = std::thread::hardware_concurrency();
    return n ? n : 1;
}

// One pool for the process: a thread per logical processor (less one), started on first use.
void run_on_pool(std::function<void()> f) {
    struct Pool {
        std::mutex lock;
        std::condition_variable cv;
        std::deque<std::function<void()>> jobs;
        Pool() {
            for (unsigned i = 0, n = std::max(1u, cpu_count() - 1); i < n; i++)
                std::thread([this, i] {
                    char name[16];
                    std::snprintf(name, sizeof name, "pool %u", i);
                    set_thread_name(name);
                    for (;;) {
                        std::function<void()> job;
                        {
                            std::unique_lock lk{lock};
                            cv.wait(lk, [this] { return !jobs.empty(); });
                            job = std::move(jobs.front());
                            jobs.pop_front();
                        }
                        job();
                    }
                }).detach();
        }
    };
    static Pool* pool = new Pool;   // (never destroyed: its threads run until the process ends)
    {
        std::scoped_lock lk{pool->lock};
        pool->jobs.push_back(std::move(f));
    }
    pool->cv.notify_one();
}

// ------------------------------------------------------------------ thread samples

namespace {

// One sample at a time: the requester fills `want`, signals the thread, and its handler writes the addresses.
constexpr int SampleMax = 48;
struct Sample {
    std::atomic<int> want{0};    // the tid asked
    std::atomic<int> done{0};    // set by the handler
    uint64_t pcs[SampleMax];
    int n = 0;
};
Sample g_sample;
std::mutex g_sample_lock;
const int SampleSignal = SIGRTMIN + 3;

void sample_handler(int, siginfo_t*, void* context) {
    const int saved = errno;
    if (g_sample.want.load() == static_cast<int>(syscall(SYS_gettid))) {
        const auto* uc = static_cast<const ucontext_t*>(context);
        g_sample.n = backtrace_from(static_cast<uint64_t>(uc->uc_mcontext.gregs[REG_RIP]), g_sample.pcs, SampleMax);
        g_sample.done.store(1);
    }
    errno = saved;
}

}  // namespace

int backtrace_from(uint64_t pc, uint64_t* pcs, int max) {
    // backtrace from here: the signal handler's frames, the signal trampoline, then the interrupted function and its
    // callers. The unwinder steps through the trampoline's frame exactly: the interrupted function's entry is `pc`.
    void* frames[SampleMax + 16];
    const int got = backtrace(frames, SampleMax + 16);
    int from = got;
    for (int i = 0; i < got; i++)
        if (reinterpret_cast<uint64_t>(frames[i]) == pc) {
            from = i;
            break;
        }
    int n = 0;
    if (max > 0) pcs[n++] = pc;
    for (int i = from + 1; i < got && n < max; i++) pcs[n++] = reinterpret_cast<uint64_t>(frames[i]);
    return n;
}

int sample_thread(int tid, uint64_t* pcs, int max, int wait_ms) {
    static const bool installed = [] {
        void* warm[2];
        backtrace(warm, 2);   // loads the unwinder now: the first call allocates, which a signal handler must not
        struct sigaction sa{};
        sa.sa_sigaction = sample_handler;
        sa.sa_flags = SA_SIGINFO | SA_RESTART | SA_ONSTACK;
        sigemptyset(&sa.sa_mask);
        return sigaction(SampleSignal, &sa, nullptr) == 0;
    }();
    if (!installed || tid == static_cast<int>(syscall(SYS_gettid))) return 0;
    std::scoped_lock lk{g_sample_lock};
    g_sample.done = 0;
    g_sample.n = 0;
    g_sample.want = tid;
    int n = 0;
    if (syscall(SYS_tgkill, getpid(), tid, SampleSignal) == 0) {
        for (int waited = 0; waited < wait_ms * 10 && !g_sample.done.load(); waited++) usleep(100);
        if (g_sample.done.load()) {
            n = std::min(g_sample.n, max);
            std::copy(g_sample.pcs, g_sample.pcs + n, pcs);
        }
    }
    g_sample.want = 0;   // (a late answer finds nobody asking)
    return n;
}

std::vector<ThreadInfo> list_threads() {
    std::vector<ThreadInfo> out;
    if (DIR* d = opendir("/proc/self/task")) {
        while (dirent* e = readdir(d)) {
            if (e->d_name[0] < '0' || e->d_name[0] > '9') continue;
            ThreadInfo t{std::atoi(e->d_name), {}};
            std::ifstream comm(std::string("/proc/self/task/") + e->d_name + "/comm");
            std::getline(comm, t.name);
            out.push_back(std::move(t));
        }
        closedir(d);
    }
    return out;
}

#endif

}  // namespace os
