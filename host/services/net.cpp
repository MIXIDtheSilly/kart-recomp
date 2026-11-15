// Networking for the game's LAN Play (Pia's LAN mode: UDP broadcast and unicast over nn::socket). The main menu's
// Wireless Play entry is LAN Play from the start when the PC has a network adapter (g_lan_play_default, a game patch:
// tools/patches.py); holding L+R and clicking the left stick switches it back, as on the console.
//   nifm   - accepts LAN Play's requests (connection confirmation "prohibited": no internet check) when the PC has a
//            LAN adapter, and reports its address; requests that need the internet stay rejected (Online Play asks
//            with the same request and preset, confirmation "required"), so online play stays off.
//   bsd:u  - nn::socket on Winsock (Linux: its own sockets). Guest layouts are FreeBSD's (sockaddr with sin_len, socket option and message
//            flag values) but errno values are Linux's (EAGAIN is 11: Pia's recvfrom took BSD's 35 as a fatal error,
//            0xe487, shown as 2318-6635), and O_NONBLOCK is 0x800. Host sockets stay non-blocking; a blocking guest call
//            waits in slices with its core given to other guest threads. Raw ICMP sockets (Pia opens one to ping the
//            gateway; Windows gives raw sockets to admins only) are loopback UDP sockets: echo requests sent on them
//            become IcmpSendEcho pings whose replies come back as IP + ICMP packets, as a BSD raw socket receives them.
//            (Linux: the pings go out on an unprivileged ICMP socket instead, which needs net.ipv4.ping_group_range to
//            take the user's group, as most distributions set it; socket options are translated from BSD's numbers.)
// HOST_LAN=0 hides the network. HOST_LAN_IP=<address> picks the adapter (default: the one the default route uses).
// HOST_NET_TRACE=1 logs nifm requests and socket calls; 2 also every bsd command, every IPC request from LAN Play's
// start and the results behind error messages (overrides.cpp); 3 also every packet sent.
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mstcpip.h>
#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif

#include <iphlpapi.h>
#include <icmpapi.h>
#else
#include <arpa/inet.h>
#include <errno.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <fstream>
#include <sstream>

// The Winsock names the code below uses, on POSIX sockets.
using SOCKET = int;
using SHORT = short;
using DWORD = uint32_t;
using BOOL = int;
using u_short = unsigned short;
using u_long = unsigned long;
using ULONG = unsigned long;
using WSAPOLLFD = pollfd;
constexpr SOCKET INVALID_SOCKET = -1;
constexpr int SOCKET_ERROR = -1;
constexpr int WSAEWOULDBLOCK = EWOULDBLOCK;
constexpr int WSAEMSGSIZE = EMSGSIZE;
static int closesocket(SOCKET s) { return close(s); }
static int WSAGetLastError() { return errno; }
static int WSAPoll(pollfd* p, unsigned long n, int ms) { return ::poll(p, static_cast<nfds_t>(n), ms); }
static int ioctlsocket(SOCKET s, unsigned long cmd, u_long* v) {
    int i = static_cast<int>(*v);
    const int r = ::ioctl(s, cmd, &i);
    *v = static_cast<u_long>(i);
    return r;
}
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>

#include "../ipc.h"

// The main menu's LAN Play switch as the menu sets it up (tools/patches.py lan_play_default): 1 = LAN Play.
uint8_t g_lan_play_default = 0;

namespace ipc {
namespace {

int net_trace_level() {
    static const int v = [] {
        const char* e = std::getenv("HOST_NET_TRACE");
        return e ? std::atoi(e) : 0;
    }();
    return v;
}
bool net_trace() { return net_trace_level() >= 1; }

// ------------------------------------------------------------------ the LAN adapter

struct Lan {
    bool up = false;
    u32 ip = 0, mask = 0, gateway = 0;  // network byte order, as nn::socket::InAddr

    Lan() {
#ifdef _WIN32
        WSADATA wd;
        WSAStartup(MAKEWORD(2, 2), &wd);
#endif
        if (const char* v = std::getenv("HOST_LAN"); v && std::strcmp(v, "0") == 0) {
            LOG("net: HOST_LAN=0, no network");
            return;
        }
        // The address the default route uses: a UDP socket "connected" to a public address (nothing is sent).
        u32 want = 0;
        if (const char* v = std::getenv("HOST_LAN_IP"); v && *v) {
            inet_pton(AF_INET, v, &want);
        } else {
            SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
            sockaddr_in to{};
            to.sin_family = AF_INET;
            to.sin_port = htons(9);
            inet_pton(AF_INET, "198.51.100.1", &to.sin_addr);
            sockaddr_in me{};
            socklen_t len = sizeof me;
            if (s != INVALID_SOCKET && connect(s, reinterpret_cast<sockaddr*>(&to), sizeof to) == 0 &&
                getsockname(s, reinterpret_cast<sockaddr*>(&me), &len) == 0) {
                want = me.sin_addr.s_addr;
            }
            if (s != INVALID_SOCKET) closesocket(s);
        }
#ifdef _WIN32
        SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        INTERFACE_INFO ifs[64];
        DWORD got = 0;
        if (s != INVALID_SOCKET && WSAIoctl(s, SIO_GET_INTERFACE_LIST, nullptr, 0, ifs, sizeof ifs, &got, nullptr, nullptr) == 0) {
            const size_t n = got / sizeof(INTERFACE_INFO);
            for (int pass = 0; pass < 2 && !up; pass++) {
                for (size_t i = 0; i < n && !up; i++) {
                    const INTERFACE_INFO& f = ifs[i];
                    if (!(f.iiFlags & IFF_UP) || (f.iiFlags & IFF_LOOPBACK) || f.iiAddress.AddressIn.sin_family != AF_INET) continue;
                    const u32 a = f.iiAddress.AddressIn.sin_addr.s_addr;
                    if (pass == 0 && a != want) continue;  // pass 1: the first adapter that is up
                    ip = a;
                    mask = f.iiNetmask.AddressIn.sin_addr.s_addr;
                    up = true;
                }
            }
        }
        if (s != INVALID_SOCKET) closesocket(s);
#else
        std::string ifname;
        if (ifaddrs* list = nullptr; getifaddrs(&list) == 0) {
            for (int pass = 0; pass < 2 && !up; pass++) {
                for (ifaddrs* f = list; f && !up; f = f->ifa_next) {
                    if (!f->ifa_addr || f->ifa_addr->sa_family != AF_INET || !f->ifa_netmask) continue;
                    if (!(f->ifa_flags & IFF_UP) || (f->ifa_flags & IFF_LOOPBACK)) continue;
                    const u32 a = reinterpret_cast<const sockaddr_in*>(f->ifa_addr)->sin_addr.s_addr;
                    if (pass == 0 && a != want) continue;  // pass 1: the first adapter that is up
                    ip = a;
                    mask = reinterpret_cast<const sockaddr_in*>(f->ifa_netmask)->sin_addr.s_addr;
                    ifname = f->ifa_name;
                    up = true;
                }
            }
            freeifaddrs(list);
        }
#endif
        if (!up) {
            LOG("net: no LAN adapter, no network");
            return;
        }
        // The default gateway (Pia pings it): the next hop of the route to a public address from this adapter.
#ifdef _WIN32
        MIB_IPFORWARDROW route{};
        in_addr pub{};
        inet_pton(AF_INET, "198.51.100.1", &pub);
        if (GetBestRoute(pub.s_addr, ip, &route) == NO_ERROR && route.dwForwardNextHop != 0) {
            gateway = route.dwForwardNextHop;
        } else {
            gateway = (ip & mask) | htonl(1);
        }
#else
        // /proc/net/route: the adapter's default route (destination 0); addresses as the bytes in memory, in hex.
        std::ifstream routes("/proc/net/route");
        std::string line;
        std::getline(routes, line);   // (the header)
        while (std::getline(routes, line)) {
            std::istringstream fields(line);
            std::string name, dst, via;
            if (fields >> name >> dst >> via && name == ifname && std::stoul(dst, nullptr, 16) == 0 && std::stoul(via, nullptr, 16) != 0) {
                gateway = static_cast<u32>(std::stoul(via, nullptr, 16));
                break;
            }
        }
        if (!gateway) gateway = (ip & mask) | htonl(1);
#endif
        char a[16], m[16], g[16];
        inet_ntop(AF_INET, &ip, a, sizeof a);
        inet_ntop(AF_INET, &mask, m, sizeof m);
        inet_ntop(AF_INET, &gateway, g, sizeof g);
        LOG("net: LAN %s mask %s gateway %s", a, m, g);
    }
};
Lan& lan() {
    static Lan l;
    return l;
}

// ------------------------------------------------------------------ nifm

std::atomic<int> g_local_accepted{0};  // live accepted local-network requests

struct NifmRequest : Service {
    // RequestState: 1 not submitted / error, 2 on hold, 3 accepted.
    u32 preset = 0, state = 1;
    // ConnectionConfirmationOption (command 11): 1 prohibited, 2 not required (local network: nn::nifm::
    // SetLocalNetworkMode(true) sets 1), 3 preferred, 4 required (internet: SetLocalNetworkMode(false)), 5 forced.
    u8 confirmation = 0;
    bool counted = false;
    kern::ServiceEvent ev1, ev2;

    void handle(Request& r) override {
        if (net_trace()) {
            LOG("nifm: IRequest::%u (preset %u, state %u, data %zu: %02x %08x)", r.cmd, preset, state, r.data_size,
                r.arg<u8>(0), r.arg<u32>(0));
        }
        Service::handle(r);
    }
    explicit NifmRequest(u32 preset_) : Service("IRequest"), preset(preset_) {
        on(0, [this](Request& r) { r.push<u32>(state); });  // GetRequestState
        on(1, [this](Request& r) {                         // GetResult
            if (state != 3) r.result = MakeResult(110, 3400);  // no connection
        });
        on(2, [this](Request& r) {  // GetSystemEventReadableHandles
            r.out_copy.push_back(ev1.new_handle());
            r.out_copy.push_back(ev2.new_handle());
        });
        on(3, [this](Request&) { set_state(1); });  // Cancel
        on(4, [this](Request&) {                    // Submit
            const bool local = confirmation == 1 || confirmation == 2;
            const bool ok = local && lan().up;
            if (ok && net_trace_level() >= 2) start_ipc_trace();
            LOG("nifm: request (preset %u, confirmation %u) %s", preset, confirmation,
                ok ? "accepted: LAN" : local ? "rejected: no LAN adapter" : "rejected: no internet");
            set_state(ok ? 3 : 1);
        });
        on(6, [this](Request& r) { preset = r.arg<u32>(0); });         // SetRequirementPreset
        on(11, [this](Request& r) { confirmation = r.arg<u8>(0); });  // SetConnectionConfirmationOption
        for (u32 id : {8u, 9u, 10u, 12u, 13u, 14u, 15u, 16u, 17u, 23u}) on(id, [](Request&) {});
    }
    ~NifmRequest() override {
        if (counted) g_local_accepted--;
    }
    void set_state(u32 s) {
        state = s;
        const bool c = s == 3;
        if (c != counted) g_local_accepted += c ? 1 : -1;
        counted = c;
        ev1.signal();
    }
};

struct GeneralService : Service {
    void handle(Request& r) override {
        if (net_trace() && r.cmd != 18) LOG("nifm: IGeneralService::%u (arg %#x)", r.cmd, r.arg<u32>(0));
        Service::handle(r);
    }
    GeneralService() : Service("IGeneralService") {
        on(1, [](Request& r) {  // GetClientId
            const u32 id = 1;
            r.write_out(0, &id, 4);
        });
        on(4, [](Request& r) { r.push_object(std::make_shared<NifmRequest>(r.arg<u32>(0))); });  // CreateRequest
        on(12, [](Request& r) {  // GetCurrentIpAddress
            if (!lan().up) r.result = MakeResult(110, 300);
            r.push<u32>(lan().ip);
        });
        on(15, [](Request& r) {  // GetCurrentIpConfigInfo: IpAddressSetting (0xd) + DnsSetting (9), packed
            if (!lan().up) r.result = MakeResult(110, 300);
            u8 b[0x16]{};
            b[0] = 1;
            std::memcpy(b + 1, &lan().ip, 4);
            std::memcpy(b + 5, &lan().mask, 4);
            std::memcpy(b + 9, &lan().gateway, 4);
            b[0xd] = 1;
            std::memcpy(b + 0xe, &lan().gateway, 4);
            r.push_bytes(b, sizeof b);
        });
        on(17, [](Request& r) { r.push<u8>(lan().up); });  // IsWirelessCommunicationEnabled
        on(18, [](Request& r) { r.push<u32>(0); });         // GetInternetConnectionStatus
        on(20, [](Request& r) { r.push<u8>(lan().up); });  // IsEthernetCommunicationEnabled
        on(21, [](Request& r) { r.push<u8>(0); });          // IsAnyInternetRequestAccepted
        on(22, [](Request& r) { r.push<u8>(g_local_accepted > 0); });  // IsAnyForegroundRequestAccepted
    }
};

struct Nifm : Service {
    explicit Nifm(std::string n) : Service(std::move(n)) {
        on(4, [](Request& r) { r.push_object(std::make_shared<GeneralService>()); });
        on(5, [](Request& r) { r.push_object(std::make_shared<GeneralService>()); });
    }
};

// ------------------------------------------------------------------ bsd: sockets

namespace E {  // Linux errno, as the console's socket service answers
constexpr s32 BADF = 9, INVAL = 22, MFILE = 24, AGAIN = 11, INPROGRESS = 115, NOTSOCK = 88, PROTONOSUPPORT = 93,
              NOPROTOOPT = 92, AFNOSUPPORT = 97, TIMEDOUT = 110;
}

#ifndef _WIN32
s32 bsd_errno(int e) { return e; }   // (Linux's own values)
#else
s32 bsd_errno(int wsa) {
    switch (wsa) {
    case 0: return 0;
    case WSAEINTR: return 4;
    case WSAEBADF: case WSA_INVALID_HANDLE: return E::BADF;
    case WSA_NOT_ENOUGH_MEMORY: return 12;
    case WSAEACCES: return 13;
    case WSAEFAULT: return 14;
    case WSAEINVAL: return E::INVAL;
    case WSAEMFILE: return E::MFILE;
    case WSAEWOULDBLOCK: return E::AGAIN;
    case WSAEINPROGRESS: return E::INPROGRESS;
    case WSAEALREADY: return 114;
    case WSAENOTSOCK: return E::NOTSOCK;
    case WSAEDESTADDRREQ: return 89;
    case WSAEMSGSIZE: return 90;
    case WSAEPROTOTYPE: return 91;
    case WSAENOPROTOOPT: return E::NOPROTOOPT;
    case WSAEPROTONOSUPPORT: return E::PROTONOSUPPORT;
    case WSAESOCKTNOSUPPORT: return 94;
    case WSAEOPNOTSUPP: return 95;
    case WSAEPFNOSUPPORT: return 96;
    case WSAEAFNOSUPPORT: return E::AFNOSUPPORT;
    case WSAEADDRINUSE: return 98;
    case WSAEADDRNOTAVAIL: return 99;
    case WSAENETDOWN: return 100;
    case WSAENETUNREACH: return 101;
    case WSAENETRESET: return 102;
    case WSAECONNABORTED: return 103;
    case WSAECONNRESET: return 104;
    case WSAENOBUFS: return 105;
    case WSAEISCONN: return 106;
    case WSAENOTCONN: return 107;
    case WSAESHUTDOWN: return 108;
    case WSAETIMEDOUT: return E::TIMEDOUT;
    case WSAECONNREFUSED: return 111;
    case WSAEHOSTDOWN: return 112;
    case WSAEHOSTUNREACH: return 113;
    default: return E::INVAL;
    }
}
#endif

#ifndef _WIN32
// A FreeBSD socket option (level 0xFFFF: SOL_SOCKET; 0: IPPROTO_IP) as Linux numbers it; false when Linux has none.
// Winsock numbers them as BSD does, so on Windows they pass unchanged.
bool host_sockopt(s32 level, s32 opt, int& hl, int& ho) {
    hl = level;
    ho = opt;
    if (level == 0xFFFF) {
        hl = SOL_SOCKET;
        switch (opt) {
        case 0x4: case 0x200: ho = SO_REUSEADDR; return true;   // SO_REUSEADDR; SO_REUSEPORT as on Windows
        case 0x8: ho = SO_KEEPALIVE; return true;
        case 0x10: ho = SO_DONTROUTE; return true;
        case 0x20: ho = SO_BROADCAST; return true;
        case 0x80: ho = SO_LINGER; return true;
        case 0x100: ho = SO_OOBINLINE; return true;
        case 0x1001: ho = SO_SNDBUF; return true;
        case 0x1002: ho = SO_RCVBUF; return true;
        case 0x1007: ho = SO_ERROR; return true;
        case 0x1008: ho = SO_TYPE; return true;
        default: return false;
        }
    }
    if (level == IPPROTO_IP) {
        switch (opt) {
        case 2: ho = IP_HDRINCL; return true;
        case 3: ho = IP_TOS; return true;
        case 4: ho = IP_TTL; return true;
        case 9: ho = IP_MULTICAST_IF; return true;
        case 10: ho = IP_MULTICAST_TTL; return true;
        case 11: ho = IP_MULTICAST_LOOP; return true;
        case 12: ho = IP_ADD_MEMBERSHIP; return true;
        case 13: ho = IP_DROP_MEMBERSHIP; return true;
        default: return false;
        }
    }
    return true;   // IPPROTO_TCP (TCP_NODELAY 1 and TCP_MAXSEG 2 alike) and the rest
}
#endif

struct Sock {
    SOCKET s = INVALID_SOCKET;
    int type = 0;
    bool icmp = false;  // emulated raw ICMP socket; `s` is a UDP socket on 127.0.0.1:icmp_port
    u16 icmp_port = 0;  // network order
    std::atomic<bool> nonblock{false}, closed{false};
    s64 rcv_timeout_ms = 0, snd_timeout_ms = 0;  // 0: forever
    ~Sock() {
        if (s != INVALID_SOCKET) closesocket(s);
    }
};
using SockPtr = std::shared_ptr<Sock>;

std::mutex g_fd_lock;
std::vector<SockPtr> g_fds;

s32 add_fd(SockPtr p) {
    std::scoped_lock lk{g_fd_lock};
    for (size_t i = 0; i < g_fds.size(); i++) {
        if (!g_fds[i]) {
            g_fds[i] = std::move(p);
            return static_cast<s32>(i);
        }
    }
    if (g_fds.size() >= 1024) return -1;
    g_fds.push_back(std::move(p));
    return static_cast<s32>(g_fds.size() - 1);
}
SockPtr get_fd(s32 fd) {
    std::scoped_lock lk{g_fd_lock};
    return fd >= 0 && static_cast<size_t>(fd) < g_fds.size() ? g_fds[fd] : nullptr;
}

std::string addr_str(const sockaddr_in& a) {
    char t[16];
    inet_ntop(AF_INET, &a.sin_addr, t, sizeof t);
    return std::string(t) + ":" + std::to_string(ntohs(a.sin_port));
}

// Guest sockaddr_in: u8 len, u8 family (AF_INET 2), u16 port, u32 addr, 8 zero bytes.
bool to_host_addr(const std::vector<u8>& g, sockaddr_in& out) {
    out = {};
    if (g.size() < 8 || g[1] != AF_INET) return false;
    out.sin_family = AF_INET;
    std::memcpy(&out.sin_port, g.data() + 2, 2);
    std::memcpy(&out.sin_addr, g.data() + 4, 4);
    return true;
}
std::vector<u8> to_guest_addr(const sockaddr_in& a) {
    std::vector<u8> g(16, 0);
    g[0] = 16;
    g[1] = AF_INET;
    std::memcpy(g.data() + 2, &a.sin_port, 2);
    std::memcpy(g.data() + 4, &a.sin_addr, 4);
    return g;
}

// poll events: FreeBSD <-> Winsock.
constexpr s16 PIn = 1, PPri = 2, POut = 4, PErr = 8, PHup = 0x10, PNval = 0x20, PRdNorm = 0x40, PWrNorm = 4;
SHORT to_win_events(s16 g) {
    SHORT w = 0;
    if (g & (PIn | PRdNorm)) w |= POLLRDNORM;
    if (g & (POut | PWrNorm)) w |= POLLWRNORM;
    return w;  // WSAPoll rejects POLLPRI on most providers
}
s16 to_guest_events(SHORT w, s16 asked) {
    s16 g = 0;
    if (w & (POLLRDNORM | POLLRDBAND)) g |= asked & (PIn | PRdNorm) ? asked & (PIn | PRdNorm) : PIn;
    if (w & POLLWRNORM) g |= asked & (POut | PWrNorm) ? asked & (POut | PWrNorm) : POut;
    if (w & POLLERR) g |= PErr;
    if (w & POLLHUP) g |= PHup;
    if (w & POLLNVAL) g |= PNval;
    return g;
}

using Clock = std::chrono::steady_clock;

// ------------------------------------------------------------------ emulated raw ICMP

u16 inet_checksum(const u8* p, size_t n) {
    u32 sum = 0;
    for (size_t i = 0; i + 1 < n; i += 2) sum += static_cast<u32>(p[i] << 8 | p[i + 1]);
    if (n & 1) sum += static_cast<u32>(p[n - 1] << 8);
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return static_cast<u16>(~sum);
}

// Pings `dst` for an ICMP echo request sent on an emulated raw socket and delivers the reply to it (127.0.0.1:port) as
// [u32 source address][IP header][ICMP echo reply]. Runs on a thread of its own: IcmpSendEcho blocks.
#ifndef _WIN32
// Linux: an unprivileged ICMP socket (SOCK_DGRAM, IPPROTO_ICMP) sends the echo request; the kernel sets its identifier
// and checksum and hands back the reply's ICMP part, from which the packet a BSD raw socket gets is put together.
void icmp_echo(u32 dst, u16 port, std::vector<u8> request) {
    std::thread([=] {
        const int s = socket(AF_INET, SOCK_DGRAM, IPPROTO_ICMP);
        if (s < 0) {
            static std::atomic<bool> said{false};
            if (!said.exchange(true))
                LOG("net: no ICMP socket (%s): the gateway pings go unanswered (sysctl net.ipv4.ping_group_range)", std::strerror(errno));
            return;
        }
        sockaddr_in to{};
        to.sin_family = AF_INET;
        to.sin_addr.s_addr = dst;
        std::vector<u8> reply(request.size() + 64);
        ssize_t got = -1;
        const auto t0 = Clock::now();
        if (sendto(s, request.data(), request.size(), 0, reinterpret_cast<sockaddr*>(&to), sizeof to) >= 0) {
            for (;;) {
                const auto left = 1000 - std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count();
                pollfd p{s, POLLIN, 0};
                if (left <= 0 || ::poll(&p, 1, static_cast<int>(left)) <= 0) break;
                got = ::recv(s, reply.data(), reply.size(), 0);
                if (got >= 8 && reply[0] == 0) break;   // an echo reply (the socket only gets replies to its own requests)
                got = -1;
            }
        }
        close(s);
        if (net_trace()) {
            char t[16];
            inet_ntop(AF_INET, &dst, t, sizeof t);
            LOG("net: icmp echo to %s: %s, %lld ms", t, got >= 8 ? "reply" : "no reply",
                static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count()));
        }
        if (got < 8) return;
        const size_t data = static_cast<size_t>(got) - 8;
        std::vector<u8> pkt(4 + 20 + 8 + data);
        std::memcpy(pkt.data(), &dst, 4);
        u8* ip = pkt.data() + 4;
        const u16 total = static_cast<u16>(20 + 8 + data);
        ip[0] = 0x45;
        ip[2] = static_cast<u8>(total >> 8);
        ip[3] = static_cast<u8>(total);
        ip[8] = 64;
        ip[9] = 1;  // ICMP
        std::memcpy(ip + 12, &dst, 4);
        std::memcpy(ip + 16, &lan().ip, 4);
        const u16 ipsum = inet_checksum(ip, 20);
        ip[10] = static_cast<u8>(ipsum >> 8);
        ip[11] = static_cast<u8>(ipsum);
        u8* icmp = ip + 20;
        std::memcpy(icmp + 4, request.data() + 4, 4);  // the guest's identifier and sequence (the kernel used its own)
        if (data) std::memcpy(icmp + 8, reply.data() + 8, data);
        const u16 sum = inet_checksum(icmp, 8 + data);
        icmp[2] = static_cast<u8>(sum >> 8);
        icmp[3] = static_cast<u8>(sum);
        const int u = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        sockaddr_in back{};
        back.sin_family = AF_INET;
        back.sin_port = port;
        back.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        sendto(u, pkt.data(), pkt.size(), 0, reinterpret_cast<sockaddr*>(&back), sizeof back);
        close(u);
    }).detach();
}
#else
void icmp_echo(u32 dst, u16 port, std::vector<u8> request) {
    std::thread([=] {
        const u8* payload = request.data() + 8;
        const DWORD plen = static_cast<DWORD>(request.size() - 8);
        HANDLE h = IcmpCreateFile();
        if (h == INVALID_HANDLE_VALUE) return;
        std::vector<u8> reply(sizeof(ICMP_ECHO_REPLY) + plen + 8 + 64);
        const DWORD n = IcmpSendEcho(h, dst, const_cast<u8*>(payload), static_cast<WORD>(plen), nullptr, reply.data(),
                                     static_cast<DWORD>(reply.size()), 1000);
        IcmpCloseHandle(h);
        const auto* e = reinterpret_cast<const ICMP_ECHO_REPLY*>(reply.data());
        if (net_trace()) {
            in_addr a{};
            a.s_addr = dst;
            char t[16];
            inet_ntop(AF_INET, &a, t, sizeof t);
            LOG("net: icmp echo to %s: %s, %lu ms", t, n && e->Status == IP_SUCCESS ? "reply" : "no reply",
                n ? e->RoundTripTime : 0ul);
        }
        if (n == 0 || e->Status != IP_SUCCESS) return;
        std::vector<u8> pkt(4 + 20 + 8 + e->DataSize);
        std::memcpy(pkt.data(), &e->Address, 4);
        u8* ip = pkt.data() + 4;
        const u16 total = static_cast<u16>(20 + 8 + e->DataSize);
        ip[0] = 0x45;
        ip[2] = static_cast<u8>(total >> 8);
        ip[3] = static_cast<u8>(total);
        ip[8] = e->Options.Ttl ? e->Options.Ttl : 64;
        ip[9] = 1;  // ICMP
        std::memcpy(ip + 12, &e->Address, 4);
        std::memcpy(ip + 16, &lan().ip, 4);
        const u16 ipsum = inet_checksum(ip, 20);
        ip[10] = static_cast<u8>(ipsum >> 8);
        ip[11] = static_cast<u8>(ipsum);
        u8* icmp = ip + 20;
        std::memcpy(icmp + 4, request.data() + 4, 4);  // identifier, sequence
        if (e->DataSize) std::memcpy(icmp + 8, e->Data, e->DataSize);
        const u16 sum = inet_checksum(icmp, 8 + e->DataSize);
        icmp[2] = static_cast<u8>(sum >> 8);
        icmp[3] = static_cast<u8>(sum);
        SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        sockaddr_in to{};
        to.sin_family = AF_INET;
        to.sin_port = port;
        to.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        sendto(s, reinterpret_cast<const char*>(pkt.data()), static_cast<int>(pkt.size()), 0,
               reinterpret_cast<sockaddr*>(&to), sizeof to);
        closesocket(s);
    }).detach();
}
#endif

// Waits until the socket is readable or writable or the deadline passes (or forever). The caller holds a HostWait.
// Returns false on timeout or when the socket was closed meanwhile.
bool wait_ready(Sock& k, bool write, Clock::time_point deadline, bool forever) {
    for (;;) {
        if (k.closed) return false;
        int slice = 100;
        if (!forever) {
            const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
            if (left <= 0) return false;
            slice = static_cast<int>(std::min<s64>(left, slice));
        }
        WSAPOLLFD p{k.s, static_cast<SHORT>(write ? POLLWRNORM : POLLRDNORM), 0};
        const int n = WSAPoll(&p, 1, slice);
        if (n != 0) return true;  // ready, or an error the retried call reports
    }
}

// Runs `op` (returns >= 0, or SOCKET_ERROR with WSAGetLastError) as a blocking or non-blocking guest call.
template <class Op> std::pair<s32, s32> run_io(Sock& k, bool write, bool dontwait, Op op) {
    int r = op();
    if (r != SOCKET_ERROR) return {r, 0};
    int e = WSAGetLastError();
    if (e != WSAEWOULDBLOCK || k.nonblock || dontwait) return {-1, bsd_errno(e)};
    const s64 t = write ? k.snd_timeout_ms : k.rcv_timeout_ms;
    const auto deadline = Clock::now() + std::chrono::milliseconds(t);
    kern::HostWait hw;
    for (;;) {
        if (!wait_ready(k, write, deadline, t == 0)) return {-1, k.closed ? E::BADF : E::AGAIN};
        r = op();
        if (r != SOCKET_ERROR) return {r, 0};
        e = WSAGetLastError();
        if (e != WSAEWOULDBLOCK) return {-1, bsd_errno(e)};
    }
}

struct Bsd : Service {
    void reply(Request& r, s32 ret, s32 err) {
        r.push(ret);
        r.push(err);
    }
    void bad_fd(Request& r) { reply(r, -1, E::BADF); }

    void handle(Request& r) override {
        Service::handle(r);
        if (net_trace_level() >= 2 && r.cmd != 8 && r.cmd != 9 && r.cmd != 10 && r.cmd != 11) {
            LOG("net: bsd %u (%08x %08x %08x) -> %d errno %d (%zu bytes out)", r.cmd, r.arg<u32>(0), r.arg<u32>(4), r.arg<u32>(8),
                r.out.size() >= 4 ? *reinterpret_cast<const s32*>(r.out.data()) : 0,
                r.out.size() >= 8 ? *reinterpret_cast<const s32*>(r.out.data() + 4) : 0, r.out.size());
        }
    }

    explicit Bsd(std::string n) : Service(std::move(n)) {
        concurrent = true;  // blocking calls from several guest threads at once

        on(0, [](Request& r) { r.push<u64>(0); });  // RegisterClient
        on(1, [](Request&) {});                     // StartMonitoring
        auto make_socket = [this](Request& r) {     // Socket, SocketExempt, Open
            const s32 domain = r.arg<s32>(0), type_raw = r.arg<s32>(4), proto = r.arg<s32>(8);
            const s32 type = type_raw & 0xFFFF;
            if (domain != AF_INET) return reply(r, -1, E::AFNOSUPPORT);
            const bool icmp = type == SOCK_RAW && proto == IPPROTO_ICMP;
            if (type != SOCK_STREAM && type != SOCK_DGRAM && !icmp) return reply(r, -1, E::PROTONOSUPPORT);
            auto k = std::make_shared<Sock>();
            k->type = type;
            k->icmp = icmp;
            k->s = icmp ? socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP) : socket(AF_INET, type, proto);
            if (k->s == INVALID_SOCKET) return reply(r, -1, bsd_errno(WSAGetLastError()));
            if (icmp) {
                sockaddr_in a{};
                a.sin_family = AF_INET;
                a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
                socklen_t len = sizeof a;
                if (::bind(k->s, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0 ||
                    getsockname(k->s, reinterpret_cast<sockaddr*>(&a), &len) != 0) {
                    return reply(r, -1, bsd_errno(WSAGetLastError()));
                }
                k->icmp_port = a.sin_port;
            }
            u_long nb = 1;
            ioctlsocket(k->s, FIONBIO, &nb);
#ifdef _WIN32
            if (type == SOCK_DGRAM || icmp) {  // no WSAECONNRESET from recvfrom after an ICMP port unreachable
                BOOL off = FALSE;
                DWORD got = 0;
                WSAIoctl(k->s, SIO_UDP_CONNRESET, &off, sizeof off, nullptr, 0, &got, nullptr, nullptr);
            }
#endif
            k->nonblock = (type_raw & 0x20000000) != 0;  // SOCK_NONBLOCK
            const s32 fd = add_fd(k);
            if (net_trace()) LOG("net: socket(%d, %#x, %d) = %d", domain, type_raw, proto, fd);
            if (fd < 0) return reply(r, -1, E::MFILE);
            reply(r, fd, 0);
        };
        on(2, make_socket);
        on(3, make_socket);
        on(5, [this](Request& r) { select(r); });
        on(6, [this](Request& r) { poll(r); });
        on(7, [this](Request& r) { reply(r, -1, E::INVAL); });  // Sysctl
        on(8, [this](Request& r) { recv(r, r.arg<s32>(0), r.arg<s32>(4), false); });   // Recv
        on(9, [this](Request& r) { recv(r, r.arg<s32>(0), r.arg<s32>(4), true); });    // RecvFrom
        on(10, [this](Request& r) { send(r, r.arg<s32>(0), r.arg<s32>(4), false); });  // Send
        on(11, [this](Request& r) { send(r, r.arg<s32>(0), r.arg<s32>(4), true); });   // SendTo
        on(12, [this](Request& r) { accept(r); });
        on(13, [this](Request& r) {  // Bind
            auto k = get_fd(r.arg<s32>(0));
            if (!k) return bad_fd(r);
            sockaddr_in a;
            if (!to_host_addr(r.read_in(0), a)) return reply(r, -1, E::INVAL);
            if (k->icmp) return reply(r, 0, 0);  // the backing socket stays on loopback
            const int ok = bind(k->s, reinterpret_cast<sockaddr*>(&a), sizeof a);
            const s32 err = ok == 0 ? 0 : bsd_errno(WSAGetLastError());
            if (net_trace() || err) LOG("net: bind(%d, %s) = %d", r.arg<s32>(0), addr_str(a).c_str(), err);
            reply(r, ok == 0 ? 0 : -1, err);
        });
        on(14, [this](Request& r) { connect(r); });
        auto name = [this](Request& r, bool peer) {  // GetSockName / GetPeerName
            auto k = get_fd(r.arg<s32>(0));
            if (!k) return bad_fd(r), r.push<u32>(0);
            sockaddr_in a{};
            socklen_t len = sizeof a;
            const int ok = peer ? getpeername(k->s, reinterpret_cast<sockaddr*>(&a), &len)
                                : getsockname(k->s, reinterpret_cast<sockaddr*>(&a), &len);
            if (ok != 0) return reply(r, -1, bsd_errno(WSAGetLastError())), r.push<u32>(0);
            const auto g = to_guest_addr(a);
            r.write_out(0, g.data(), g.size());
            reply(r, 0, 0);
            r.push<u32>(16);
        };
        on(15, [name](Request& r) { name(r, true); });
        on(16, [name](Request& r) { name(r, false); });
        on(17, [this](Request& r) { getsockopt(r); });
        on(18, [this](Request& r) {  // Listen
            auto k = get_fd(r.arg<s32>(0));
            if (!k) return bad_fd(r);
            const int ok = listen(k->s, r.arg<s32>(4));
            reply(r, ok == 0 ? 0 : -1, ok == 0 ? 0 : bsd_errno(WSAGetLastError()));
        });
        on(19, [this](Request& r) { ioctl(r); });
        on(20, [this](Request& r) { fcntl(r); });
        on(21, [this](Request& r) { setsockopt(r); });
        on(22, [this](Request& r) {  // Shutdown
            auto k = get_fd(r.arg<s32>(0));
            if (!k) return bad_fd(r);
            const int ok = shutdown(k->s, r.arg<s32>(4));
            reply(r, ok == 0 ? 0 : -1, ok == 0 ? 0 : bsd_errno(WSAGetLastError()));
        });
        on(24, [this](Request& r) { send(r, r.arg<s32>(0), 0, false); });  // Write
        on(25, [this](Request& r) { recv(r, r.arg<s32>(0), 0, false); });  // Read
        on(26, [this](Request& r) {                                       // Close
            const s32 fd = r.arg<s32>(0);
            SockPtr k;
            {
                std::scoped_lock lk{g_fd_lock};
                if (fd >= 0 && static_cast<size_t>(fd) < g_fds.size()) k = std::move(g_fds[fd]);
            }
            if (!k) return bad_fd(r);
            k->closed = true;  // threads blocked on it give up within a poll slice; the socket closes with the last user
            if (net_trace()) LOG("net: close(%d)", fd);
            reply(r, 0, 0);
        });
        on(27, [this](Request& r) {  // DuplicateSocket
            auto k = get_fd(r.arg<s32>(0));
            if (!k) return bad_fd(r);
            const s32 fd = add_fd(k);
            reply(r, fd, fd < 0 ? E::MFILE : 0);
        });
    }

    void recv(Request& r, s32 fd, s32 flags, bool from) {
        auto k = get_fd(fd);
        if (!k) {
            bad_fd(r);
            if (from) r.push<u32>(0);
            return;
        }
        const Buffer out = r.out_buf(0);
        if (net_trace_level() >= 2) {
            LOG("net: recv%s(%d, %#llx bytes, flags %#x)%s", from ? "from" : "", fd, out.size, flags, k->icmp ? " icmp" : "");
        }
        if (k->icmp) return recv_icmp(r, *k, flags, from);
        std::vector<char> buf(out.size);
        const int wflags = (flags & 1 ? MSG_OOB : 0) | (flags & 2 ? MSG_PEEK : 0);
        sockaddr_in a{};
        socklen_t alen = sizeof a;
        auto [n, err] = run_io(*k, false, (flags & 0x80) != 0, [&] {
            alen = sizeof a;
            return from ? recvfrom(k->s, buf.data(), static_cast<int>(buf.size()), wflags, reinterpret_cast<sockaddr*>(&a), &alen)
                        : ::recv(k->s, buf.data(), static_cast<int>(buf.size()), wflags);
        });
        if (err == bsd_errno(WSAEMSGSIZE) && k->type == SOCK_DGRAM) {  // BSD truncates a datagram silently
            n = static_cast<s32>(buf.size());
            err = 0;
        }
        if (n > 0) r.write_out(0, buf.data(), n);
        reply(r, n, err);
        if (from) {
            if (n >= 0 && a.sin_family == AF_INET) {
                const auto g = to_guest_addr(a);
                r.write_out(1, g.data(), g.size());
                r.push<u32>(16);
            } else {
                r.push<u32>(0);
            }
        }
        if (net_trace() && n < 0 && err != E::AGAIN) LOG("net: recv%s(%d) errno %d", from ? "from" : "", fd, err);
        if (net_trace() && n >= 0) LOG("net: recv%s(%d) %d bytes%s%s", from ? "from" : "", fd, n, from ? " from " : "", from ? addr_str(a).c_str() : "");
    }

    // An emulated raw ICMP socket's packets: [u32 source][IP header][ICMP], the source reported as the sender.
    void recv_icmp(Request& r, Sock& k, s32 flags, bool from) {
        std::vector<char> buf(r.out_buf(0).size + 4);
        auto [n, err] = run_io(k, false, (flags & 0x80) != 0, [&] {
            return ::recv(k.s, buf.data(), static_cast<int>(buf.size()), flags & 2 ? MSG_PEEK : 0);
        });
        if (err == bsd_errno(WSAEMSGSIZE)) n = static_cast<s32>(buf.size()), err = 0;
        sockaddr_in a{};
        a.sin_family = AF_INET;
        if (n >= 4) {
            std::memcpy(&a.sin_addr, buf.data(), 4);
            n -= 4;
            r.write_out(0, buf.data() + 4, n);
        } else if (n >= 0) {
            n = 0;
        }
        reply(r, n, err);
        if (from) {
            if (n >= 0) {
                const auto g = to_guest_addr(a);
                r.write_out(1, g.data(), g.size());
                r.push<u32>(16);
            } else {
                r.push<u32>(0);
            }
        }
    }

    void send(Request& r, s32 fd, s32 flags, bool to) {
        auto k = get_fd(fd);
        if (!k) return bad_fd(r);
        const auto data = r.read_in(0);
        sockaddr_in a{};
        if (to && !to_host_addr(r.read_in(1), a)) to = false;  // no address: as send
        if (k->icmp) {
            if (to && data.size() >= 8 && data[0] == 8) icmp_echo(a.sin_addr.s_addr, k->icmp_port, data);  // echo request
            else LOG("net: icmp packet type %d (%zu bytes) dropped", data.empty() ? -1 : data[0], data.size());
            return reply(r, static_cast<s32>(data.size()), 0);
        }
        const int wflags = flags & 1 ? MSG_OOB : 0;
        auto [n, err] = run_io(*k, true, (flags & 0x80) != 0, [&] {
            return to ? sendto(k->s, reinterpret_cast<const char*>(data.data()), static_cast<int>(data.size()), wflags,
                               reinterpret_cast<sockaddr*>(&a), sizeof a)
                      : ::send(k->s, reinterpret_cast<const char*>(data.data()), static_cast<int>(data.size()), wflags);
        });
        if (net_trace_level() >= 3) {
            std::string hex;
            for (size_t i = 0; i < data.size(); i++) {
                char b[4];
                std::snprintf(b, sizeof b, "%02x", data[i]);
                hex += b;
            }
            LOG("net: data %s", hex.c_str());
        }
        if (net_trace() || (err && err != E::AGAIN)) {
            LOG("net: send%s(%d, %zu bytes%s%s) = %d errno %d", to ? "to" : "", fd, data.size(), to ? " to " : "",
                to ? addr_str(a).c_str() : "", n, err);
        }
        reply(r, n, err);
    }

    void connect(Request& r) {
        auto k = get_fd(r.arg<s32>(0));
        if (!k) return bad_fd(r);
        sockaddr_in a;
        if (!to_host_addr(r.read_in(0), a)) return reply(r, -1, E::INVAL);
        if (::connect(k->s, reinterpret_cast<sockaddr*>(&a), sizeof a) == 0) return reply(r, 0, 0);
        const int e = WSAGetLastError();
        if (e != WSAEWOULDBLOCK) return reply(r, -1, bsd_errno(e));
        if (k->nonblock) return reply(r, -1, E::INPROGRESS);
        {
            kern::HostWait hw;
            if (!wait_ready(*k, true, Clock::now() + std::chrono::milliseconds(k->snd_timeout_ms), k->snd_timeout_ms == 0)) {
                return reply(r, -1, E::TIMEDOUT);
            }
        }
        int so = 0;
        socklen_t len = sizeof so;
        ::getsockopt(k->s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&so), &len);
        reply(r, so ? -1 : 0, bsd_errno(so));
    }

    void accept(Request& r) {
        auto k = get_fd(r.arg<s32>(0));
        if (!k) return bad_fd(r), r.push<u32>(0);
        sockaddr_in a{};
        socklen_t alen = sizeof a;
        SOCKET got = INVALID_SOCKET;
        auto [n, err] = run_io(*k, false, false, [&] {
            alen = sizeof a;
            got = ::accept(k->s, reinterpret_cast<sockaddr*>(&a), &alen);
            return got == INVALID_SOCKET ? SOCKET_ERROR : 0;
        });
        if (err) return reply(r, -1, err), r.push<u32>(0);
        auto c = std::make_shared<Sock>();
        c->s = got;
        c->type = SOCK_STREAM;
        u_long nb = 1;
        ioctlsocket(got, FIONBIO, &nb);
        const s32 fd = add_fd(c);
        const auto g = to_guest_addr(a);
        r.write_out(0, g.data(), g.size());
        reply(r, fd, fd < 0 ? E::MFILE : 0);
        r.push<u32>(16);
    }

    void poll(Request& r) {
        const u32 nfds = r.arg<u32>(0);
        const s32 timeout = r.arg<s32>(4);
        auto in = r.read_in(0);
        struct GPoll { s32 fd; s16 events, revents; };
        const size_t n = std::min<size_t>(nfds, in.size() / sizeof(GPoll));
        std::vector<GPoll> g(n);
        if (n) std::memcpy(g.data(), in.data(), n * sizeof(GPoll));
        std::vector<SockPtr> keep(n);
        std::vector<WSAPOLLFD> w;
        std::vector<size_t> map;
        s32 ready = 0;
        for (size_t i = 0; i < n; i++) {
            g[i].revents = 0;
            if (g[i].fd < 0) continue;
            keep[i] = get_fd(g[i].fd);
            if (!keep[i]) {
                g[i].revents = PNval;
                ready++;
                continue;
            }
            w.push_back({keep[i]->s, to_win_events(g[i].events), 0});
            map.push_back(i);
        }
        auto run = [&](int ms) {
            if (w.empty()) {
                if (ms > 0) std::this_thread::sleep_for(std::chrono::milliseconds(ms));
                return 0;
            }
            for (auto& p : w) p.revents = 0;
            return WSAPoll(w.data(), static_cast<ULONG>(w.size()), ms);
        };
        int got = ready ? run(0) : 0;
        if (!ready && got == 0) got = run(0);
        if (!ready && got == 0 && timeout != 0) {
            const auto deadline = Clock::now() + std::chrono::milliseconds(timeout);
            kern::HostWait hw;
            for (;;) {
                int slice = 100;
                if (timeout > 0) {
                    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
                    if (left <= 0) break;
                    slice = static_cast<int>(std::min<s64>(left, slice));
                }
                got = run(slice);
                if (got != 0) break;
                bool closed = false;
                for (size_t m : map) closed |= keep[m]->closed.load();
                if (closed) break;
            }
        }
        if (got == SOCKET_ERROR) {
            return reply(r, -1, bsd_errno(WSAGetLastError()));
        }
        for (size_t j = 0; j < w.size(); j++) {
            GPoll& p = g[map[j]];
            if (keep[map[j]]->closed) p.revents = PNval;
            else p.revents = to_guest_events(w[j].revents, p.events);
            if (p.revents) ready++;
        }
        if (n) r.write_out(0, g.data(), n * sizeof(GPoll));
        reply(r, ready, 0);
    }

    // Select on top of poll: in {s32 nfds, pad, timeval{s64 sec, s64 usec}, bool null_timeout}, three fd_set buffers.
    void select(Request& r) {
        const s32 nfds = std::clamp(r.arg<s32>(0), 0, 1024);
        const s64 sec = r.arg<s64>(8), usec = r.arg<s64>(16);
        const bool forever = r.arg<u8>(24) != 0;
        std::vector<u8> sets[3];
        for (int i = 0; i < 3; i++) {
            sets[i] = r.read_in(i);
            sets[i].resize(128, 0);
        }
        auto isset = [&](int s, int fd) { return r.in_buf(s).size && (sets[s][fd / 8] >> (fd % 8) & 1); };
        std::vector<WSAPOLLFD> w;
        std::vector<int> fds;
        std::vector<SockPtr> keep;
        for (int fd = 0; fd < nfds; fd++) {
            const bool rd = isset(0, fd), wr = isset(1, fd), ex = isset(2, fd);
            if (!rd && !wr && !ex) continue;
            auto k = get_fd(fd);
            if (!k) return reply(r, -1, E::BADF);
            w.push_back({k->s, static_cast<SHORT>((rd ? POLLRDNORM : 0) | (wr ? POLLWRNORM : 0)), 0});
            fds.push_back(fd);
            keep.push_back(k);
        }
        const s64 timeout_ms = forever ? -1 : sec * 1000 + usec / 1000;
        auto run = [&](int ms) {
            if (w.empty()) {
                if (ms > 0) std::this_thread::sleep_for(std::chrono::milliseconds(ms));
                return 0;
            }
            for (auto& p : w) p.revents = 0;
            return WSAPoll(w.data(), static_cast<ULONG>(w.size()), ms);
        };
        int got = run(0);
        if (got == 0 && timeout_ms != 0) {
            const auto deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);
            kern::HostWait hw;
            for (;;) {
                int slice = 100;
                if (timeout_ms > 0) {
                    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
                    if (left <= 0) break;
                    slice = static_cast<int>(std::min<s64>(left, slice));
                }
                got = run(slice);
                if (got != 0) break;
            }
        }
        if (got == SOCKET_ERROR) return reply(r, -1, bsd_errno(WSAGetLastError()));
        for (auto& s : sets) std::fill(s.begin(), s.end(), 0);
        s32 count = 0;
        for (size_t j = 0; j < w.size(); j++) {
            const int fd = fds[j];
            const SHORT ev = w[j].revents;
            if (isset(0, fd) && (ev & (POLLRDNORM | POLLRDBAND | POLLHUP | POLLERR))) sets[0][fd / 8] |= 1 << (fd % 8), count++;
            if (isset(1, fd) && (ev & (POLLWRNORM | POLLERR))) sets[1][fd / 8] |= 1 << (fd % 8), count++;
            if (isset(2, fd) && (ev & POLLERR)) sets[2][fd / 8] |= 1 << (fd % 8), count++;
        }
        for (int i = 0; i < 3; i++) r.write_out(i, sets[i].data(), sets[i].size());
        reply(r, count, 0);
    }

    // Socket options: BSD values; the ones whose layout differs on Winsock are converted.
    void setsockopt(Request& r) {
        auto k = get_fd(r.arg<s32>(0));
        if (!k) return bad_fd(r);
        const s32 level = r.arg<s32>(4), opt = r.arg<s32>(8);
        auto v = r.read_in(0);
        v.resize(std::max<size_t>(v.size(), 16), 0);
        s32 iv;
        std::memcpy(&iv, v.data(), 4);
        if (k->icmp && level != 0xFFFF) return reply(r, 0, 0);  // IP options of the emulated raw socket
        int wlevel = level, wopt = opt;
        std::vector<char> wv(v.begin(), v.end());
        int wlen = static_cast<int>(r.in_buf(0).size);
        if (level == 0xFFFF) {
            switch (opt) {
            case 0x1005:  // SO_SNDTIMEO / SO_RCVTIMEO: timeval, kept here for the blocking emulation
            case 0x1006: {
                s64 sec, usec;
                std::memcpy(&sec, v.data(), 8);
                std::memcpy(&usec, v.data() + 8, 8);
                (opt == 0x1005 ? k->snd_timeout_ms : k->rcv_timeout_ms) = std::max<s64>(sec * 1000 + usec / 1000, 0);
                return reply(r, 0, 0);
            }
            case 0x200: wopt = SO_REUSEADDR; break;  // SO_REUSEPORT
            case 0x80: {                              // SO_LINGER: {int, int} -> {u_short, u_short}
                linger l{static_cast<u_short>(iv != 0), 0};
                s32 secs;
                std::memcpy(&secs, v.data() + 4, 4);
                l.l_linger = static_cast<u_short>(std::clamp(secs, 0, 65535));
                wv.assign(reinterpret_cast<char*>(&l), reinterpret_cast<char*>(&l) + sizeof l);
                wlen = sizeof l;
                break;
            }
            case 0x1100: k->nonblock = iv != 0; return reply(r, 0, 0);  // SO_NONBLOCK
            case 0x1003: case 0x1004: return reply(r, 0, 0);            // SO_SNDLOWAT / SO_RCVLOWAT
            default: break;
            }
        } else if (level == IPPROTO_IP && (opt == 10 || opt == 11) && wlen < 4) {  // IP_MULTICAST_TTL/LOOP: u_char
            const DWORD d = v[0];
            wv.assign(reinterpret_cast<const char*>(&d), reinterpret_cast<const char*>(&d) + 4);
            wlen = 4;
        }
#ifdef _WIN32
        const int ok = ::setsockopt(k->s, wlevel, wopt, wv.data(), wlen);
        const s32 err = ok == 0 ? 0 : bsd_errno(WSAGetLastError());
#else
        int hl, ho;
        const bool known = host_sockopt(wlevel, opt == 0x200 && level == 0xFFFF ? 0x4 : opt, hl, ho);
        const int ok = known ? ::setsockopt(k->s, hl, ho, wv.data(), static_cast<socklen_t>(wlen)) : -1;
        const s32 err = ok == 0 ? 0 : known ? bsd_errno(errno) : E::NOPROTOOPT;
#endif
        if (net_trace() || err) LOG("net: setsockopt(%d, %#x, %#x, %d) = %d", r.arg<s32>(0), level, opt, iv, err);
        // Options Winsock lacks (ENOPROTOOPT) are accepted: the game only tunes with them.
        const s32 reported = err == E::NOPROTOOPT ? 0 : err;
        reply(r, reported ? -1 : 0, reported);
    }

    void getsockopt(Request& r) {
        auto k = get_fd(r.arg<s32>(0));
        if (!k) return bad_fd(r), r.push<u32>(0);
        const s32 level = r.arg<s32>(4), opt = r.arg<s32>(8);
        if (level == 0xFFFF && (opt == 0x1005 || opt == 0x1006)) {
            const s64 ms = opt == 0x1005 ? k->snd_timeout_ms : k->rcv_timeout_ms;
            const s64 tv[2] = {ms / 1000, ms % 1000 * 1000};
            r.write_out(0, tv, sizeof tv);
            return reply(r, 0, 0), r.push<u32>(16);
        }
        int wopt = level == 0xFFFF && opt == 0x200 ? SO_REUSEADDR : opt;
        char buf[16]{};
        socklen_t len = sizeof buf;
#ifdef _WIN32
        if (::getsockopt(k->s, level, wopt, buf, &len) != 0) {
            return reply(r, -1, bsd_errno(WSAGetLastError())), r.push<u32>(0);
        }
#else
        int hl, ho;
        if (!host_sockopt(level, opt, hl, ho)) return reply(r, -1, E::NOPROTOOPT), r.push<u32>(0);
        if (::getsockopt(k->s, hl, ho, buf, &len) != 0) return reply(r, -1, bsd_errno(errno)), r.push<u32>(0);
        (void)wopt;
#endif
        s32 iv = 0;
        std::memcpy(&iv, buf, std::min<size_t>(len, 4));
        if (level == 0xFFFF && opt == 0x1007) iv = bsd_errno(iv);  // SO_ERROR
        if (len <= 4) {
            r.write_out(0, &iv, 4);
            len = 4;
        } else {
            r.write_out(0, buf, len);
        }
        reply(r, 0, 0);
        r.push<u32>(static_cast<u32>(len));
    }

    void ioctl(Request& r) {
        auto k = get_fd(r.arg<s32>(0));
        if (!k) return bad_fd(r);
        const u32 req = r.arg<u32>(4);
        if (req == 0x8004667e) {  // FIONBIO
            const auto v = r.read_in(0);
            s32 on = 0;
            if (v.size() >= 4) std::memcpy(&on, v.data(), 4);
            k->nonblock = on != 0;
            return reply(r, 0, 0);
        }
        if (req == 0x4004667f) {  // FIONREAD
            u_long n = 0;
            if (ioctlsocket(k->s, FIONREAD, &n) != 0) return reply(r, -1, bsd_errno(WSAGetLastError()));
            const s32 v = static_cast<s32>(n);
            r.write_out(0, &v, 4);
            return reply(r, 0, 0);
        }
        LOG("net: ioctl %#x not supported", req);
        reply(r, -1, E::INVAL);
    }

    void fcntl(Request& r) {
        auto k = get_fd(r.arg<s32>(0));
        if (!k) return bad_fd(r);
        const s32 cmd = r.arg<s32>(4), arg = r.arg<s32>(8);
        constexpr s32 NonBlock = 0x800;
        if (cmd == 3) return reply(r, k->nonblock ? NonBlock : 0, 0);  // F_GETFL
        if (cmd == 4) {                                                // F_SETFL
            k->nonblock = (arg & NonBlock) != 0;
            return reply(r, 0, 0);
        }
        reply(r, 0, 0);
    }
};

}  // namespace

void register_net() {
    g_lan_play_default = lan().up ? 1 : 0;
    for (const char* n : {"nifm:u", "nifm:a", "nifm:s"}) {
        register_service(n, [n] { return std::make_shared<Nifm>(n); });
    }
    for (const char* n : {"bsd:u", "bsd:s", "bsd:a"}) {
        register_service(n, [n] { return std::make_shared<Bsd>(n); });
    }
}

}  // namespace ipc
