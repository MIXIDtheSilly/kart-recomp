// Standalone host runtime: shared definitions.
#pragma once
#include <cstdarg>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "cpu.h"

using s8 = int8_t;
using s16 = int16_t;
using u8 = uint8_t;
using u16 = uint16_t;
using u32 = uint32_t;
using u64 = uint64_t;
using s32 = int32_t;
using s64 = int64_t;

// ------------------------------------------------------------------ results

constexpr u32 MakeResult(u32 module, u32 desc) { return module | desc << 9; }
constexpr u32 ResultOk = 0;
namespace KResult {
constexpr u32 OutOfSessions = MakeResult(1, 7);
constexpr u32 NotImplemented = MakeResult(1, 33);
constexpr u32 InvalidSize = MakeResult(1, 101);
constexpr u32 InvalidAddress = MakeResult(1, 102);
constexpr u32 OutOfResource = MakeResult(1, 103);
constexpr u32 OutOfMemory = MakeResult(1, 104);
constexpr u32 InvalidCurrentMemory = MakeResult(1, 106);
constexpr u32 InvalidNewMemoryPermission = MakeResult(1, 108);
constexpr u32 InvalidMemoryRegion = MakeResult(1, 110);
constexpr u32 InvalidPriority = MakeResult(1, 112);
constexpr u32 InvalidCoreId = MakeResult(1, 113);
constexpr u32 InvalidHandle = MakeResult(1, 114);
constexpr u32 InvalidPointer = MakeResult(1, 115);
constexpr u32 InvalidCombination = MakeResult(1, 116);
constexpr u32 TimedOut = MakeResult(1, 117);
constexpr u32 Cancelled = MakeResult(1, 118);
constexpr u32 OutOfRange = MakeResult(1, 119);
constexpr u32 InvalidEnumValue = MakeResult(1, 120);
constexpr u32 NotFound = MakeResult(1, 121);
constexpr u32 InvalidState = MakeResult(1, 125);
constexpr u32 SessionClosed = MakeResult(1, 123);
}  // namespace KResult

// ------------------------------------------------------------------ logging

void log_line(const char* fmt, ...);
[[noreturn]] void fatal(const char* fmt, ...);
#define LOG(...) log_line(__VA_ARGS__)
// HOST_TIMELINE=<file>: timestamped events for frame timelines (scratchpad timeline.py).
extern bool g_timeline_on;
void timeline(const char* fmt, ...);
#define TIMELINE(...) do { if (g_timeline_on) timeline(__VA_ARGS__); } while (0)

std::string describe_guest(u64 addr);  // "0x... (module+off)"
std::string host_symbol(u64 rip);      // host function containing rip
void dump_guest_stack(Ctx* c);

// ------------------------------------------------------------------ guest memory helpers

inline u8* gptr(u64 a) { return static_cast<u8*>(hp(a)); }
inline std::string read_cstr(u64 a, size_t max = 4096) {
    std::string s;
    for (size_t i = 0; i < max; i++) {
        char ch = static_cast<char>(*gptr(a + i));
        if (!ch) break;
        s += ch;
    }
    return s;
}

// Time base of the guest's system counter: 31.25 MHz on Switch 2 (the SDK converts ticks with that
// constant, nn::os::GetSystemTickFrequency; at 19.2 MHz its times were off by 1.63x).
constexpr u64 GuestTickHz = 31'250'000;
u64 guest_ticks();
u64 host_ns();

// Program the runtime was built for.
constexpr u64 ProgramId = 0x0400C3F00006E000ull;
