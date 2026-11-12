// Small system services: lm, pctl, time, acc, aoc, nifm, friend, prepo.
#include <ctime>

#include "../ipc.h"

namespace ipc {
namespace {

constexpr u8 UserId[16] = {0x4b, 0x61, 0x72, 0x74, 0x50, 0x6c, 0x61, 0x79, 0x65, 0x72, 0x30, 0x30, 0x30, 0x30, 0x30, 0x31};

// ------------------------------------------------------------------ lm

struct Logger : Service {
    Logger() : Service("ILogger") {
        on(0, [](Request& r) {
            auto in = r.read_in(0);
            // LogPacketHeader (0x18 bytes) followed by TLV fields; print the text fields.
            std::string text;
            for (size_t i = 0x18; i + 2 <= in.size();) {
                const u8 type = in[i];
                size_t len = in[i + 1];
                size_t hdr = 2;
                if (len & 0x80) { len = (len & 0x7F) | (in[i + 2] << 7); hdr = 3; }
                if (i + hdr + len > in.size()) break;
                if (type == 2) text.assign(reinterpret_cast<const char*>(in.data() + i + hdr), len);
                i += hdr + len;
            }
            if (!text.empty()) LOG("guest log: %s", text.c_str());
        });
        on(1, [](Request&) {});
    }
};
struct LM : Service {
    LM() : Service("lm") {
        on(0, [](Request& r) { r.push_object(std::make_shared<Logger>()); });
    }
};

// ------------------------------------------------------------------ pctl

struct ParentalControl : Service {
    ParentalControl() : Service("IParentalControlService") {
        on(1, [](Request&) {});
        on(1001, [](Request&) {});  // CheckFreeCommunicationPermission
        on(1013, [](Request&) {});
        on(1031, [](Request& r) { r.push<u8>(0); });  // IsRestrictionEnabled
    }
};
struct Pctl : Service {
    explicit Pctl(std::string n) : Service(std::move(n)) {
        auto create = [](Request& r) { r.push_object(std::make_shared<ParentalControl>()); };
        on(0, create);
        on(1, create);
    }
};

// ------------------------------------------------------------------ time

struct TimeShared {
    std::shared_ptr<kern::KSharedMemory> shm = std::make_shared<kern::KSharedMemory>(0x1000);
    u8 source_id[16] = {0x53, 0x74, 0x65, 0x61, 0x64, 0x79, 0x43, 0x6c, 0x6f, 0x63, 0x6b, 0x48, 0x6f, 0x73, 0x74, 0x31};

    // LockFreeAtomicType<T>: u32 counter, then two copies of T (8-byte aligned).
    template <class T> void write(size_t off, const T& v) {
        u8* p = shm->data() + off;
        u32 counter;
        std::memcpy(&counter, p, 4);
        counter++;
        std::memcpy(p + 8 + sizeof(T) * (counter & 1), &v, sizeof(T));
        std::atomic_thread_fence(std::memory_order_release);
        std::memcpy(p, &counter, 4);
    }

    TimeShared() {
        struct SteadyPoint { s64 time_point; u8 id[16]; };
        struct SystemContext { s64 offset; SteadyPoint steady; };
        struct Adjust { s64 rtc_offset, diff_scale, shift, lower, upper; u8 id[16]; };
        SteadyPoint steady{0, {}};
        std::memcpy(steady.id, source_id, 16);
        write(0x0, steady);
        // System clocks: posix time = offset + steady clock seconds.
        const s64 uptime_s = static_cast<s64>(guest_ticks() / GuestTickHz);
        SystemContext ctx{static_cast<s64>(std::time(nullptr)) - uptime_s, {0, {}}};
        std::memcpy(ctx.steady.id, source_id, 16);
        write(0x38, ctx);
        write(0x80, ctx);
        write(0xC8, u8{0});
        Adjust adj{0, 0, 0, 0, 0, {}};
        std::memcpy(adj.id, source_id, 16);
        write(0xD0, adj);
    }
};
TimeShared& time_shared() {
    static TimeShared t;
    return t;
}

struct TimeZone : Service {
    TimeZone() : Service("ITimeZoneService") {
        on(0, [](Request& r) {  // GetDeviceLocationName
            char name[0x24] = "UTC";
            r.push_bytes(name, sizeof name);
        });
        on(1, [](Request&) {});
        on(4, [](Request& r) {  // LoadTimeZoneRule: the rule stays opaque to the guest
            std::vector<u8> zero(r.out_buf(0).size);
            r.write_out(0, zero.data(), zero.size());
        });
        auto to_calendar = [](Request& r) {
            const time_t t = static_cast<time_t>(r.arg<s64>(0));
            std::tm tm{};
            gmtime_s(&tm, &t);
            struct {
                s16 year; s8 month, day, hour, minute, second; s8 pad;
            } cal{static_cast<s16>(tm.tm_year + 1900), static_cast<s8>(tm.tm_mon + 1), static_cast<s8>(tm.tm_mday),
                  static_cast<s8>(tm.tm_hour), static_cast<s8>(tm.tm_min), static_cast<s8>(tm.tm_sec), 0};
            struct {
                u32 day_of_week, day_of_year;
                char tz_name[8];
                u32 is_dst;
                s32 utc_offset;
            } info{static_cast<u32>(tm.tm_wday), static_cast<u32>(tm.tm_yday), "UTC", 0, 0};
            r.push(cal);
            r.push(info);
        };
        on(100, to_calendar);
        on(101, to_calendar);
        on(201, [](Request& r) {  // ToPosixTimeWithMyRule
            struct { s16 year; s8 month, day, hour, minute, second; s8 pad; } cal;
            std::memcpy(&cal, r.data, sizeof cal);
            std::tm tm{};
            tm.tm_year = cal.year - 1900; tm.tm_mon = cal.month - 1; tm.tm_mday = cal.day;
            tm.tm_hour = cal.hour; tm.tm_min = cal.minute; tm.tm_sec = cal.second;
            const s64 t = _mkgmtime(&tm);
            r.push<u32>(1);
            r.write_out(0, &t, 8);
        });
    }
};

struct SystemClock : Service {
    SystemClock() : Service("ISystemClock") {
        on(0, [](Request& r) { r.push<s64>(static_cast<s64>(std::time(nullptr))); });  // GetCurrentTime
        on(2, [](Request& r) {  // GetSystemClockContext
            u8 ctx[0x20]{};
            const s64 offset = static_cast<s64>(std::time(nullptr)) - static_cast<s64>(guest_ticks() / GuestTickHz);
            std::memcpy(ctx, &offset, 8);
            std::memcpy(ctx + 16, time_shared().source_id, 16);
            r.push_bytes(ctx, sizeof ctx);
        });
    }
};

struct SteadyClock : Service {
    SteadyClock() : Service("ISteadyClock") {
        on(0, [](Request& r) {  // GetCurrentTimePoint
            const s64 s = static_cast<s64>(guest_ticks() / GuestTickHz);
            r.push(s);
            r.push_bytes(time_shared().source_id, 16);
        });
    }
};

struct TimeU : Service {
    explicit TimeU(std::string n) : Service(std::move(n)) {
        on(0, [](Request& r) { r.push_object(std::make_shared<SystemClock>()); });
        on(1, [](Request& r) { r.push_object(std::make_shared<SystemClock>()); });
        on(2, [](Request& r) { r.push_object(std::make_shared<SteadyClock>()); });
        on(3, [](Request& r) { r.push_object(std::make_shared<TimeZone>()); });
        on(4, [](Request& r) { r.push_object(std::make_shared<SystemClock>()); });
        on(5, [](Request& r) { r.push_object(std::make_shared<SystemClock>()); });
        on(20, [](Request& r) { r.out_copy.push_back(kern::add_handle(time_shared().shm)); });
        on(100, [](Request& r) { r.push<u8>(0); });
        on(200, [](Request& r) { r.push<u8>(0); });
    }
};

// ------------------------------------------------------------------ acc

struct Profile : Service {
    Profile() : Service("IProfile") {
        auto base = [](Request& r) {
            u8 b[0x38]{};
            std::memcpy(b, UserId, 16);
            const u64 ts = 1700000000;
            std::memcpy(b + 0x10, &ts, 8);
            std::strcpy(reinterpret_cast<char*>(b + 0x18), "Player");
            r.push_bytes(b, sizeof b);
        };
        on(0, [base](Request& r) {
            base(r);
            std::vector<u8> user_data(r.out_buf(0).size);
            r.write_out(0, user_data.data(), user_data.size());
        });
        on(1, base);
        on(10, [](Request& r) { r.push<u32>(0); });  // GetImageSize
        on(11, [](Request& r) { r.push<u32>(0); });
    }
};

struct ManagerForApplication : Service {
    ManagerForApplication() : Service("IManagerForApplication") {
        on(0, [](Request&) {});
        on(1, [](Request& r) { r.push<u64>(0x0123456789abcdefull); });  // GetAccountId
    }
};

struct Acc : Service {
    explicit Acc(std::string n) : Service(std::move(n)) {
        on(0, [](Request& r) { r.push<u32>(1); });  // GetUserCount
        on(1, [](Request& r) { r.push<u8>(1); });   // GetUserExistence
        auto list = [](Request& r) {
            u8 ids[16 * 8]{};
            std::memcpy(ids, UserId, 16);
            r.write_out(0, ids, sizeof ids);
        };
        on(2, list);
        on(3, list);
        on(4, [](Request& r) { r.push_bytes(UserId, 16); });  // GetLastOpenedUser
        on(5, [](Request& r) { r.push_object(std::make_shared<Profile>()); });
        on(100, [](Request&) {});
        on(101, [](Request& r) { r.push_object(std::make_shared<ManagerForApplication>()); });
        on(140, list);  // ListQualifiedUsers
        on(150, [](Request& r) { r.push<u8>(0); });
        on(160, [](Request&) {});
    }
};

// ------------------------------------------------------------------ nifm: no network

struct NifmRequest : Service {
    NifmRequest() : Service("IRequest") {
        on(0, [](Request& r) { r.push<u32>(1); });  // GetRequestState: not submitted / error
        on(1, [](Request& r) { r.result = MakeResult(110, 3400); });  // GetResult: no internet connection
        on(2, [this](Request& r) {
            r.out_copy.push_back(ev1.new_handle());
            r.out_copy.push_back(ev2.new_handle());
        });
        for (u32 id : {3u, 4u, 6u, 11u}) on(id, [](Request&) {});
    }
    kern::ServiceEvent ev1, ev2;
};

struct GeneralService : Service {
    GeneralService() : Service("IGeneralService") {
        on(4, [](Request& r) { r.push_object(std::make_shared<NifmRequest>()); });
        on(18, [](Request& r) { r.push<u32>(0); });  // GetInternetConnectionStatus
        on(21, [](Request& r) { r.push<u8>(0); });   // IsAnyInternetRequestAccepted
    }
};

struct Nifm : Service {
    explicit Nifm(std::string n) : Service(std::move(n)) {
        on(4, [](Request& r) { r.push_object(std::make_shared<GeneralService>()); });
        on(5, [](Request& r) { r.push_object(std::make_shared<GeneralService>()); });
    }
};

// ------------------------------------------------------------------ friends

struct FriendService : Service {
    FriendService() : Service("IFriendService") {
        on(10101, [](Request& r) { r.push<u32>(0); });  // GetFriendList: none
        on(10120, [](Request& r) { r.push<u8>(1); });   // CheckFriendListAvailability
        on(10700, [](Request& r) {                       // GetPlayHistoryRegistrationKey
            std::vector<u8> key(r.out_buf(0).size);
            r.write_out(0, key.data(), key.size());
        });
    }
};

struct Friend : Service {
    explicit Friend(std::string n) : Service(std::move(n)) {
        on(0, [](Request& r) { r.push_object(std::make_shared<FriendService>()); });
        on(1, [](Request& r) { r.push_object(stub("INotificationService")); });
    }
};

// ------------------------------------------------------------------ chat:u: GameChat (Switch 2), opened by 1.8.0 at boot

// The session chat:u 0 hands out. Command 1 returns an event the game waits on: never signalled (as Eglantine's
// IChatSession). Other commands answer as unknown ones do.
struct ChatSession : Service {
    ChatSession() : Service("IChatSession") {
        on(1, [this](Request& r) { r.out_copy.push_back(event.new_handle()); });
    }
    kern::ServiceEvent event;
};

// Without a session object from command 0 the sdk's chat client read through a null pointer at boot.
struct Chat : Service {
    Chat() : Service("chat:u") {
        on(0, [](Request& r) { r.push_object(std::make_shared<ChatSession>()); });
    }
};

}  // namespace

void register_misc() {
    register_service("lm", [] { return std::make_shared<LM>(); });
    for (const char* n : {"pctl", "pctl:s", "pctl:a", "pctl:r"}) {
        register_service(n, [n] { return std::make_shared<Pctl>(n); });
    }
    for (const char* n : {"time:u", "time:a", "time:s"}) {
        register_service(n, [n] { return std::make_shared<TimeU>(n); });
    }
    for (const char* n : {"acc:u0", "acc:u1"}) {
        register_service(n, [n] { return std::make_shared<Acc>(n); });
    }
    for (const char* n : {"nifm:u", "nifm:a", "nifm:s"}) {
        register_service(n, [n] { return std::make_shared<Nifm>(n); });
    }
    for (const char* n : {"friend:u", "friend:a"}) {
        register_service(n, [n] { return std::make_shared<Friend>(n); });
    }
    register_service("chat:u", [] { return std::make_shared<Chat>(); });
}

}  // namespace ipc
