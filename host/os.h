// What the host needs from the operating system beyond the C++ library, for Windows and Linux (os.cpp): thread names
// and priorities, timer sleeps, the exe's path, files by path, environment variables, whole pages.
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

// The path separator as a string literal, for building paths by concatenation: "dir" OS_SEP "file".
#ifdef _WIN32
#define OS_SEP "\\"
#else
#define OS_SEP "/"
#endif

namespace os {

// The calling thread's name, as debuggers, the stall dump and (Linux) top show it.
void set_thread_name(const char* name);

// The calling thread's priority. On Linux a thread can always lower its own priority, but raising it (AboveNormal,
// or back up after lowering it) needs CAP_SYS_NICE: without it the call changes nothing.
enum class Priority { Lowest, BelowNormal, Normal, AboveNormal };
void set_thread_priority(Priority p);

// A timer sleep of `ns` (at least 100 ns): Windows' high-resolution waitable timer (the default timer tick is 15.6 ms),
// Linux's clock_nanosleep on the monotonic clock.
void timer_sleep_ns(int64_t ns);
// Gives the rest of the time slice to another ready thread (SwitchToThread, sched_yield).
void yield();

// The running executable, and the folder it is in.
std::filesystem::path exe_path();
inline std::filesystem::path exe_dir() { return exe_path().parent_path(); }

// fopen by path: Windows paths are wide (_wfopen), Linux paths are bytes.
FILE* open_file(const std::filesystem::path& path, const char* mode);

// Sets an environment variable of this process (getenv sees it from then on).
void set_env(const char* key, const char* value);

// `bytes` of zeroed read-write memory in whole pages, reserved and committed at once; nullptr when there is none.
void* alloc_pages(size_t bytes);

// The calling thread's system id (what the stall dump names threads by).
uint64_t thread_id();

// Number of logical processors.
unsigned cpu_count();

// Runs `f` on a pool thread (Linux: par_for_each's helpers; Windows uses its own thread pool).
void run_on_pool(std::function<void()> f);

#ifndef _WIN32
// Linux: the code addresses on thread `tid`'s stack (a thread of this process), innermost first: where it runs, then
// the return addresses the unwinder finds. A signal makes the thread read them itself; 0 when it did not answer within
// `wait_ms` (blocked with signals masked, or in the kernel without returning). For stall and crash reports.
int sample_thread(int tid, uint64_t* pcs, int max, int wait_ms = 500);
// Inside a signal handler: `pc` (where the signal interrupted the thread) and the return addresses above it.
int backtrace_from(uint64_t pc, uint64_t* pcs, int max);
// The ids and names of this process's threads (/proc/self/task).
struct ThreadInfo {
    int tid;
    std::string name;
};
std::vector<ThreadInfo> list_threads();
#endif

}  // namespace os
