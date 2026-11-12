// SPDX-License-Identifier: GPL-3.0-or-later
// What the audio renderer needs from its host, in place of the emulator core it came from:
// integer types, result codes, logging, thread helpers, guest memory access, the rendered event,
// the clock and the output volume. The host (services/audio.cpp) defines the out-of-line members.
#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <format>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using s8 = std::int8_t;
using s16 = std::int16_t;
using s32 = std::int32_t;
using s64 = std::int64_t;
using f32 = float;
using f64 = double;
using VAddr = u64;
using PAddr = u64;

namespace AudioCore {

// ------------------------------------------------------------------ results (module 153 = audio)

struct Result {
    u32 raw{};
    constexpr Result() = default;
    constexpr explicit Result(u32 raw_) : raw{raw_} {}
    constexpr Result(u32 module, u32 description) : raw{module | description << 9} {}
    [[nodiscard]] constexpr bool IsSuccess() const { return raw == 0; }
    [[nodiscard]] constexpr bool IsError() const { return raw != 0; }
    [[nodiscard]] constexpr bool IsFailure() const { return raw != 0; }
    constexpr bool operator==(const Result&) const = default;
};

constexpr Result ResultSuccess{0u};
constexpr u32 AudioModule = 153;
constexpr Result ResultNotFound{AudioModule, 1};
constexpr Result ResultOperationFailed{AudioModule, 2};
constexpr Result ResultInvalidSampleRate{AudioModule, 3};
constexpr Result ResultInsufficientBuffer{AudioModule, 4};
constexpr Result ResultOutOfSessions{AudioModule, 5};
constexpr Result ResultBufferCountReached{AudioModule, 8};
constexpr Result ResultInvalidChannelCount{AudioModule, 10};
constexpr Result ResultInvalidUpdateInfo{AudioModule, 41};
constexpr Result ResultInvalidAddressInfo{AudioModule, 42};
constexpr Result ResultNotSupported{AudioModule, 513};
constexpr Result ResultInvalidArgument{AudioModule, 900};
constexpr Result ResultInvalidHandle{AudioModule, 1536};
constexpr Result ResultInvalidRevision{AudioModule, 1537};

// Guest memory pages.
constexpr u64 GuestPageSize = 0x1000;

// The kernel's pseudo-handle for the calling process.
constexpr u32 CurrentProcessHandle = 0xFFFF8001;

// ------------------------------------------------------------------ logging

enum class LogLevel { Debug, Info, Warning, Error, Critical };

void LogMessage(LogLevel level, std::string_view text);  // host

namespace detail {
template <class T> decltype(auto) LogArg(const T& v) {
    if constexpr (std::is_enum_v<T>) return static_cast<std::underlying_type_t<T>>(v);
    else return (v);
}
}  // namespace detail

// std::format at run time; enums print as their value.
template <class... Args> std::string Format(std::string_view fmt, const Args&... args) {
    try {
        auto values = std::tuple{detail::LogArg(args)...};
        return std::apply([&](auto&... v) { return std::vformat(fmt, std::make_format_args(v...)); }, values);
    } catch (const std::exception&) {
        return std::string(fmt);
    }
}

template <class... Args> void Log(LogLevel level, std::string_view fmt, const Args&... args) {
    if (level != LogLevel::Debug) LogMessage(level, Format(fmt, args...));
}

#define LOG_TRACE(category, ...) ((void)0)
#define LOG_DEBUG(category, ...) ((void)0)
#define LOG_INFO(category, ...) ::AudioCore::Log(::AudioCore::LogLevel::Info, __VA_ARGS__)
#define LOG_WARNING(category, ...) ::AudioCore::Log(::AudioCore::LogLevel::Warning, __VA_ARGS__)
#define LOG_ERROR(category, ...) ::AudioCore::Log(::AudioCore::LogLevel::Error, __VA_ARGS__)
#define LOG_CRITICAL(category, ...) ::AudioCore::Log(::AudioCore::LogLevel::Critical, __VA_ARGS__)

[[noreturn]] void AssertFailed(const char* what, const char* file, int line);  // host
#define ASSERT(cond) \
    do { if (!(cond)) ::AudioCore::AssertFailed(#cond, __FILE__, __LINE__); } while (0)
#define ASSERT_MSG(cond, ...) ASSERT(cond)
#define UNREACHABLE() ::AudioCore::AssertFailed("unreachable", __FILE__, __LINE__)

// ------------------------------------------------------------------ small helpers

template <class T> constexpr T AlignUp(T value, std::size_t size) {
    const auto mod = static_cast<T>(value % size);
    return mod == 0 ? value : static_cast<T>(value + (static_cast<T>(size) - mod));
}
template <class T> constexpr bool IsAligned(T value, std::size_t alignment) {
    return (value & static_cast<T>(alignment - 1)) == 0;
}
constexpr u32 MakeMagic(char a, char b, char c, char d) {
    return u32(u8(a)) | u32(u8(b)) << 8 | u32(u8(c)) << 16 | u32(u8(d)) << 24;
}

enum class ThreadPriority { Low, Normal, High, VeryHigh, Critical };
void SetThreadName(const char* name);              // host.cpp
void SetThreadPriority(ThreadPriority priority);   // host.cpp

// An auto-reset event between threads.
class ThreadEvent {
public:
    void Set() {
        std::scoped_lock lk{mutex};
        if (!is_set) {
            is_set = true;
            condvar.notify_one();
        }
    }
    void Wait() {
        std::unique_lock lk{mutex};
        condvar.wait(lk, [&] { return is_set; });
        is_set = false;
    }
    bool WaitFor(std::chrono::nanoseconds time) {
        std::unique_lock lk{mutex};
        if (!condvar.wait_for(lk, time, [&] { return is_set; })) return false;
        is_set = false;
        return true;
    }
    void Reset() {
        std::scoped_lock lk{mutex};
        is_set = false;
    }

private:
    bool is_set = false;
    std::mutex mutex;
    std::condition_variable condvar;
};

// ------------------------------------------------------------------ the guest process

// The game's memory. Stateless: the host's members reach the one guest address space.
class GuestMemory {
public:
    bool ReadBlockUnsafe(u64 address, void* dest, std::size_t size);
    bool WriteBlockUnsafe(u64 address, const void* src, std::size_t size);
    bool ZeroBlock(u64 address, std::size_t size);
    // A host pointer to [address, address + size) when the range is contiguous, else nullptr.
    u8* GetSpan(u64 address, std::size_t size);
    void Write32(u64 address, u32 value) { WriteBlockUnsafe(address, &value, sizeof value); }
};

// Guest data to read: in place when contiguous, else a copy.
template <class T> class GuestRead {
public:
    GuestRead(GuestMemory& memory, u64 address, std::size_t count) : count_{count} {
        if (u8* p = memory.GetSpan(address, count * sizeof(T))) {
            data_ = reinterpret_cast<const T*>(p);
        } else {
            copy_.resize(count);
            memory.ReadBlockUnsafe(address, copy_.data(), count * sizeof(T));
            data_ = copy_.data();
        }
    }
    GuestRead(GuestMemory* memory, u64 address, std::size_t count) : GuestRead(*memory, address, count) {}
    const T& operator[](std::size_t i) const { return data_[i]; }
    const T* data() const { return data_; }
    std::size_t size() const { return count_; }
    const T* begin() const { return data_; }
    const T* end() const { return data_ + count_; }

private:
    const T* data_ = nullptr;
    std::size_t count_ = 0;
    std::vector<T> copy_;
};

class HostProcess {
public:
    GuestMemory& GetMemory() { return memory; }

private:
    GuestMemory memory;
};

struct HostTransferMemory {
    u64 address = 0;
    u64 GetSourceAddress() const { return address; }
};

// The event the game waits on for "a frame was rendered".
class HostEvent {
public:
    virtual ~HostEvent() = default;
    virtual void Signal() = 0;
    virtual void Clear() = 0;
};

// ------------------------------------------------------------------ the host system

class AudioCore;

// Time since the host started.
struct Clock {
    std::chrono::nanoseconds GetGlobalTimeNs() const;
    std::chrono::microseconds GetGlobalTimeUs() const {
        return std::chrono::duration_cast<std::chrono::microseconds>(GetGlobalTimeNs());
    }
};

class HostSystem {
public:
    const Clock& CoreTiming() const { return clock; }
    bool IsShuttingDown() const { return shutting_down.load(std::memory_order_relaxed); }
    bool IsPaused() const { return false; }
    void SetShuttingDown() { shutting_down = true; }
    class AudioCore& AudioCore();  // audio_core.cpp

private:
    Clock clock;
    std::atomic<bool> shutting_down{false};
};

HostSystem& GetHostSystem();  // audio_core.cpp

// The host's volume on top of the game's (1 = unchanged).
f32 OutputVolume();  // host.cpp
// HOST_AUDIO_MUTE=1: the output device plays silence (the mix still runs, so meters still see it).
bool OutputMuted();  // host.cpp

}  // namespace AudioCore
