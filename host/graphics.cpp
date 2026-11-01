// The graphics boundary: nv::, nn::vi:: and NVN are native. NVN functions the game looks up by
// name get guest-callable addresses in a reserved range; each address dispatches to a native function.
#include <windows.h>

#include <array>
#include <atomic>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <utility>

#include "graphics.h"
#include "host.h"
#include "kernel.h"

namespace gfx {
namespace {

// ------------------------------------------------------------------ native functions callable by the guest

constexpr u64 ThunkBase = 0x7f00000000ull;  // inside the guest address space, never mapped
constexpr size_t MaxThunks = 4096;

struct Thunk {
    std::string name;
    NativeFn fn;
    std::atomic<u64> calls{0};
};
std::array<Thunk, MaxThunks> g_thunks;
std::atomic<size_t> g_thunk_count{0};
std::mutex g_thunk_lock;
std::unordered_map<std::string, u64> g_thunk_by_name;

template <size_t I> void thunk_entry(Ctx* c) {
    Thunk& t = g_thunks[I];
    t.calls.fetch_add(1, std::memory_order_relaxed);
    t.fn(c, t.name);
}
template <size_t... I> constexpr std::array<GuestFn, sizeof...(I)> make_entries(std::index_sequence<I...>) {
    return {&thunk_entry<I>...};
}
const std::array<GuestFn, MaxThunks> g_entries = make_entries(std::make_index_sequence<MaxThunks>{});

}  // namespace

u64 native_function(const std::string& name, NativeFn fn) {
    std::scoped_lock lk{g_thunk_lock};
    auto it = g_thunk_by_name.find(name);
    if (it != g_thunk_by_name.end()) return it->second;
    const size_t i = g_thunk_count++;
    if (i >= MaxThunks) fatal("too many native functions");
    g_thunks[i].name = name;
    g_thunks[i].fn = std::move(fn);
    const u64 addr = ThunkBase + i * 16;
    host_override(addr, g_entries[i]);
    g_thunk_by_name.emplace(name, addr);
    return addr;
}

// ------------------------------------------------------------------ calling guest code from native code

u64 guest_call(Ctx* c, u64 fn, std::initializer_list<u64> args) {
    u64 saved[31];
    std::memcpy(saved, c->x, sizeof saved);
    const u64 sp = c->sp;
    size_t i = 0;
    for (u64 a : args) c->x[i++] = a;
    c->x[30] = 0;
    call_indirect(c, fn);
    const u64 r = c->x[0];
    std::memcpy(c->x, saved, sizeof saved);
    c->sp = sp;
    return r;
}

u64 sdk_fn(const char* name) {
    const u64 a = sdk_export(name);
    if (!a) fatal("sdk export %s not found", name);
    return a;
}

// Initializes a guest nn::os::SystemEventType around a kernel readable event.
void attach_system_event(Ctx* c, u64 event_type, u32 handle, bool auto_clear) {
    static const u64 attach = sdk_fn("_ZN2nn2os33AttachReadableHandleToSystemEventEPNS0_15SystemEventTypeEjbNS0_14EventClearModeE");
    guest_call(c, attach, {event_type, handle, 1, auto_clear ? 1u : 0u});
}

namespace {

// ------------------------------------------------------------------ nn::vi

kern::ServiceEvent& vsync_event() {
    static kern::ServiceEvent ev;
    static std::thread ticker([] {
        // Until presentation drives it, vsync is a 60 Hz timer.
        HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        LARGE_INTEGER due;
        due.QuadPart = -166667;
        SetWaitableTimer(timer, &due, 16, nullptr, nullptr, FALSE);
        for (;;) {
            WaitForSingleObject(timer, INFINITE);
            ev.signal();
        }
    });
    return ev;
}

constexpr u64 DisplayToken = 0x7e00000010ull, LayerToken = 0x7e00000020ull, NativeWindowToken = 0x7e00000030ull;

void ret(Ctx* c, u64 v = 0) { c->x[0] = v; }

void install_vi() {
    auto ov = [](const char* name, GuestFn fn) { host_override(sdk_fn(name), fn); };
    ov("_ZN2nn2vi10InitializeEv", [](Ctx* c) { ret(c); });
    ov("_ZN2nn2vi18OpenDefaultDisplayEPPNS0_7DisplayE", [](Ctx* c) { st<u64>(c->x[0], DisplayToken); ret(c); });
    ov("_ZN2nn2vi11CreateLayerEPPNS0_5LayerEPNS0_7DisplayE", [](Ctx* c) { st<u64>(c->x[0], LayerToken); ret(c); });
    ov("_ZN2nn2vi20GetDisplayVsyncEventEPNS_2os15SystemEventTypeEPNS0_7DisplayE", [](Ctx* c) {
        attach_system_event(c, c->x[0], vsync_event().new_handle(), true);
        ret(c);
    });
    ov("_ZN2nn2vi15GetNativeWindowEPPvPNS0_5LayerE", [](Ctx* c) { st<u64>(c->x[0], NativeWindowToken); ret(c); });
    ov("_ZN2nn2vi24QueryDisplayVrrSupportedEPbPKNS0_7DisplayE", [](Ctx* c) { st<u8>(c->x[0], 0); ret(c); });
    ov("_ZN2nn2vi23SetDisplayVrrCapabilityEPNS0_7DisplayEb", [](Ctx* c) { ret(c); });
    ov("_ZN2nn2vi21QueryDisplayHdrStatusEPbPNS0_15HdrOutputFormatEPtPKNS0_7DisplayE", [](Ctx* c) {
        st<u8>(c->x[0], 0);
        st<u32>(c->x[1], 0);
        st<u16>(c->x[2], 203);
        ret(c);
    });
    ov("_ZN2nn2vi25QueryDisplayHdrPaperWhiteEPtPKNS0_7DisplayE", [](Ctx* c) { st<u16>(c->x[0], 203); ret(c); });
    ov("_ZN2nn2vi52SetLayerNextHdrStandardContentLuminanceRangeMetadataEPNS0_5LayerEtt", [](Ctx* c) { ret(c); });
    ov("_ZN2nn2vi52SetLayerNextHdrExtendedContentLuminanceRangeMetadataEPNS0_5LayerEtt", [](Ctx* c) { ret(c); });
    ov("_ZN2nn2vi28SetLayerExtendedSwapIntervalEPNS0_5LayerEii", [](Ctx* c) { ret(c); });
    ov("_ZN2nn2vi30ClearLayerExtendedSwapIntervalEPNS0_5LayerE", [](Ctx* c) { ret(c); });
}

// ------------------------------------------------------------------ nv::

struct GraphicsAllocator {
    u64 alloc = 0, free = 0, realloc = 0, user = 0;
} g_allocator, g_devtools_allocator;

void install_nv() {
    auto ov = [](const char* name, GuestFn fn) { host_override(sdk_fn(name), fn); };
    ov("_ZN2nv20SetGraphicsAllocatorEPFPvmmS0_EPFvS0_S0_EPFS0_S0_mS0_ES0_", [](Ctx* c) {
        g_allocator = {c->x[0], c->x[1], c->x[2], c->x[3]};
    });
    ov("_ZN2nv28SetGraphicsDevtoolsAllocatorEPFPvmmS0_EPFvS0_S0_EPFS0_S0_mS0_ES0_", [](Ctx* c) {
        g_devtools_allocator = {c->x[0], c->x[1], c->x[2], c->x[3]};
    });
    ov("_ZN2nv18InitializeGraphicsEPvm", [](Ctx* c) {
        LOG("nv::InitializeGraphics(%#llx, %#llx)", c->x[0], c->x[1]);
    });
    ov("_ZN2nv9InitCheckEv", [](Ctx* c) { ret(c); });
    ov("_ZN2nv21GetGraphicsMemoryInfoEPNS_10MemoryInfoE", [](Ctx* c) {
        std::vector<u8> zero(0x40);
        std::memcpy(gptr(c->x[0]), zero.data(), zero.size());
    });
}

}  // namespace

void install() {
    install_vi();
    install_nv();
    install_nvn();
}

}  // namespace gfx
