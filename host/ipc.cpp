#include "ipc.h"

#include <cstdlib>
#include <set>
#include <unordered_map>

namespace ipc {
namespace {

constexpr u32 MagicIn = 0x49434653;   // "SFCI"
constexpr u32 MagicOut = 0x4F434653;  // "SFCO"
constexpr u16 PointerBufferSize = 0x8000;

// Domain state; clones of a session share it with the original.
struct Domain {
    std::mutex lock;
    bool domain = false;
    std::vector<ServicePtr> objects;  // domain object id i + 1

    u32 add_object(ServicePtr s) {
        for (size_t i = 0; i < objects.size(); i++) {
            if (!objects[i]) {
                objects[i] = std::move(s);
                return static_cast<u32>(i + 1);
            }
        }
        objects.push_back(std::move(s));
        return static_cast<u32>(objects.size());
    }
    ServicePtr object(u32 id) {
        return id >= 1 && id <= objects.size() ? objects[id - 1] : nullptr;
    }
};

struct KSession : kern::KObject {
    static constexpr const char* Type = "session";
    const char* type() const override { return Type; }
    ServicePtr root;
    std::shared_ptr<Domain> d = std::make_shared<Domain>();
};

u32 clone_session(const KSession& s) {
    auto c = std::make_shared<KSession>();
    c->root = s.root;
    c->d = s.d;
    return kern::add_handle(c);
}

std::mutex g_registry_lock;
std::map<std::string, std::function<ServicePtr()>> g_registry;

bool ipc_trace() {
    static const bool on = std::getenv("HOST_IPC_TRACE") != nullptr;
    return on;
}

struct Parsed {
    u32 type = 0;
    u32 data_word = 0, data_words = 0;  // index and count of the raw data words
};

Parsed parse(const u32* w, Request& r) {
    Parsed p;
    const u32 h0 = w[0], h1 = w[1];
    p.type = h0 & 0xFFFF;
    const u32 nx = (h0 >> 16) & 0xF, na = (h0 >> 20) & 0xF, nb = (h0 >> 24) & 0xF, nw = (h0 >> 28) & 0xF;
    p.data_words = h1 & 0x3FF;
    const u32 cmode = (h1 >> 10) & 0xF;
    u32 i = 2;
    if (h1 >> 31) {
        const u32 sh = w[i++];
        const u32 ncopy = (sh >> 1) & 0xF, nmove = (sh >> 5) & 0xF;
        if (sh & 1) {
            r.pid = w[i] | static_cast<u64>(w[i + 1]) << 32;
            i += 2;
        }
        for (u32 k = 0; k < ncopy; k++) r.copy_handles.push_back(w[i++]);
        for (u32 k = 0; k < nmove; k++) r.move_handles.push_back(w[i++]);
    }
    for (u32 k = 0; k < nx; k++, i += 2) {
        const u32 packed = w[i];
        const u64 addr = w[i + 1] | static_cast<u64>((packed >> 12) & 0xF) << 32 | static_cast<u64>((packed >> 6) & 0x3F) << 36;
        r.x.push_back({addr, packed >> 16});
    }
    auto buffers = [&](u32 n, std::vector<Buffer>& out) {
        for (u32 k = 0; k < n; k++, i += 3) {
            const u32 p2 = w[i + 2];
            const u64 addr = w[i + 1] | static_cast<u64>((p2 >> 28) & 0xF) << 32 | static_cast<u64>((p2 >> 2) & 0x3FFFFF) << 36;
            const u64 size = w[i] | static_cast<u64>((p2 >> 24) & 0xF) << 32;
            out.push_back({addr, size});
        }
    };
    buffers(na, r.a);
    buffers(nb, r.b);
    buffers(nw, r.w);
    p.data_word = i;
    u32 nc = cmode == 2 ? 1 : cmode > 2 ? cmode - 2 : 0;
    for (u32 k = 0, ci = i + p.data_words; k < nc; k++, ci += 2) {
        r.c.push_back({w[ci] | static_cast<u64>(w[ci + 1] & 0xFFFF) << 32, w[ci + 1] >> 16});
    }
    return p;
}

// Writes the reply over the request buffer.
void write_reply(u32* w, Request& r, bool tipc, bool domain, const std::vector<u32>& domain_ids) {
    std::vector<u32> move = r.out_move;
    const bool special = !r.out_copy.empty() || !move.empty();
    u32 i = 2;
    if (special) {
        w[i++] = static_cast<u32>(r.out_copy.size()) << 1 | static_cast<u32>(move.size()) << 5;
        for (u32 h : r.out_copy) w[i++] = h;
        for (u32 h : move) w[i++] = h;
    }
    const u32 data_word = i;
    u8* base = reinterpret_cast<u8*>(w);
    if (tipc) {
        u8* d = base + data_word * 4;
        std::memcpy(d, &r.result, 4);
        if (r.result == ResultOk && !r.out.empty()) std::memcpy(d + 4, r.out.data(), r.out.size());
        const u32 words = 1 + static_cast<u32>((r.out.size() + 3) / 4);
        w[0] = 0;
        w[1] = words | (special ? 1u << 31 : 0);
        return;
    }
    size_t off = (data_word * 4 + 15) & ~size_t{15};
    const size_t start = off;
    if (domain) {
        const u32 hdr[4] = {static_cast<u32>(domain_ids.size()), 0, 0, 0};
        std::memcpy(base + off, hdr, 16);
        off += 16;
    }
    const u32 out_hdr[4] = {MagicOut, 0, r.result, 0};
    std::memcpy(base + off, out_hdr, 16);
    off += 16;
    if (!r.out.empty()) {
        std::memcpy(base + off, r.out.data(), r.out.size());
        off += (r.out.size() + 3) & ~size_t{3};
    }
    for (u32 id : domain_ids) {
        std::memcpy(base + off, &id, 4);
        off += 4;
    }
    const u32 words = static_cast<u32>((off - start + 16) / 4);
    w[0] = 0;
    w[1] = words | (special ? 1u << 31 : 0);
}

void control(KSession& s, u32 cmd, Request& r) {
    switch (cmd) {
    case 0:  // ConvertCurrentObjectToDomain
        s.d->domain = true;
        s.d->objects = {s.root};
        r.push<u32>(1);
        break;
    case 1: {  // CopyFromCurrentDomain
        auto obj = s.d->object(r.arg<u32>(0));
        if (!obj) { r.result = KResult::InvalidHandle; break; }
        r.out_move.push_back(new_session(obj));
        break;
    }
    case 2:  // CloneCurrentObject
    case 4:  // CloneCurrentObjectEx
        r.out_move.push_back(clone_session(s));
        break;
    case 3:  // QueryPointerBufferSize
        r.push<u16>(PointerBufferSize);
        break;
    default:
        LOG("ipc: unknown control command %u on %s", cmd, s.root->name.c_str());
        r.result = KResult::NotImplemented;
    }
}

}  // namespace

// ------------------------------------------------------------------ Request

Buffer Request::in_buf(size_t i) const {
    if (i < a.size() && a[i].size) return a[i];
    if (i < x.size()) return x[i];
    return i < a.size() ? a[i] : Buffer{};
}

Buffer Request::out_buf(size_t i) const {
    if (i < b.size() && b[i].size) return b[i];
    if (i < c.size()) return c[i];
    return i < b.size() ? b[i] : Buffer{};
}

std::vector<u8> Request::read_in(size_t i) const {
    const Buffer bf = in_buf(i);
    std::vector<u8> v(bf.size);
    if (bf.size) std::memcpy(v.data(), gptr(bf.addr), bf.size);
    return v;
}

size_t Request::write_out(size_t i, const void* p, size_t n) const {
    const Buffer bf = out_buf(i);
    n = std::min<size_t>(n, bf.size);
    if (n) std::memcpy(gptr(bf.addr), p, n);
    return n;
}

// ------------------------------------------------------------------ Service

void Service::handle(Request& r) {
    auto it = cmds.find(r.cmd);
    if (it != cmds.end()) {
        it->second(r);
        return;
    }
    static std::mutex m;
    static std::set<std::pair<std::string, u32>> seen;
    {
        std::scoped_lock lk{m};
        if (seen.insert({name, r.cmd}).second) LOG("ipc: stubbed %s::%u (%s)", name.c_str(), r.cmd, r.tipc ? "tipc" : "cmif");
    }
    // TIPC: the result and one zero word, as Eglantine's auto stub replies; the rest of the message
    // keeps the request's words, which the Switch 2 SDK reads back (vi2 212 polled forever on zeros).
    if (r.tipc) r.out.assign(4, 0);
    else r.out.assign(0x40, 0);
}

ServicePtr stub(std::string name) { return std::make_shared<Service>(std::move(name)); }

void register_service(const std::string& name, std::function<ServicePtr()> make) {
    std::scoped_lock lk{g_registry_lock};
    g_registry[name] = std::move(make);
}

ServicePtr make_service(const std::string& name) {
    std::function<ServicePtr()> make;
    {
        std::scoped_lock lk{g_registry_lock};
        auto it = g_registry.find(name);
        if (it != g_registry.end()) make = it->second;
    }
    return make ? make() : nullptr;
}

u32 new_session(ServicePtr s) {
    auto sess = std::make_shared<KSession>();
    sess->root = std::move(s);
    return kern::add_handle(sess);
}

u32 connect_named_port(const std::string& name, u32* out_handle) {
    ServicePtr s = make_service(name);
    if (!s) {
        LOG("ConnectToNamedPort: no port %s", name.c_str());
        return KResult::NotFound;
    }
    *out_handle = new_session(s);
    return ResultOk;
}

u32 send_sync_request(u32 handle, u64 msg_addr) {
    auto sess = kern::get<KSession>(handle);
    if (!sess) {
        LOG("SendSyncRequest: handle %#x is not a session", handle);
        return KResult::InvalidHandle;
    }
    u32* w = reinterpret_cast<u32*>(gptr(msg_addr));
    Request r;
    const Parsed p = parse(w, r);
    const u8* raw = reinterpret_cast<const u8*>(w);

    if (p.type == 2) {  // Close (CMIF): the client closes the handle next
        return ResultOk;
    }
    if (p.type == 15) {
        // TIPC close: the server drops the session without a reply, so the client's request ends with "session
        // closed" (2001-0123), and the SDK checks exactly that before closing its handle: nn::codec's video decoder
        // service, destroyed at the end of each of 1.8.0's movies, aborted on 0.
        return 0xf601;
    }
    if (p.type >= 16) {  // TIPC
        r.tipc = true;
        r.cmd = p.type - 16;
        r.data = raw + p.data_word * 4;
        r.data_size = p.data_words * 4;
        ServicePtr svc = sess->root;
        if (ipc_trace()) LOG("tipc %s::%u", svc->name.c_str(), r.cmd);
        {
            std::scoped_lock lk{svc->lock};
            svc->handle(r);
        }
        for (auto& o : r.out_objects) r.out_move.insert(r.out_move.begin(), new_session(o));
        write_reply(w, r, true, false, {});
        return ResultOk;
    }

    const size_t off = (p.data_word * 4 + 15) & ~size_t{15};
    const u8* cmif = raw + off;
    ServicePtr target = sess->root;
    bool domain = false;
    if (sess->d->domain && (p.type == 4 || p.type == 6)) {
        domain = true;
        const u8 dtype = cmif[0];
        const u8 nin = cmif[1];
        const u16 dsize = static_cast<u16>(cmif[2] | cmif[3] << 8);
        u32 object_id;
        std::memcpy(&object_id, cmif + 4, 4);
        {
            std::scoped_lock lk{sess->d->lock};
            if (dtype == 2) {  // close a domain object
                if (object_id >= 1 && object_id <= sess->d->objects.size()) sess->d->objects[object_id - 1].reset();
                Request done;
                write_reply(w, done, false, true, {});
                return ResultOk;
            }
            target = sess->d->object(object_id);
        }
        for (u32 k = 0; k < nin; k++) {
            u32 id;
            std::memcpy(&id, cmif + 16 + dsize + 4 * k, 4);
            r.in_objects.push_back(id);
        }
        if (!target) {
            LOG("ipc: domain object %u does not exist", object_id);
            return KResult::InvalidHandle;
        }
        cmif += 16;
        r.data_size = dsize >= 16 ? dsize - 16 : 0;
    } else {
        r.data_size = p.data_words * 4 > (off - p.data_word * 4) + 16 ? p.data_words * 4 - (off - p.data_word * 4) - 16 : 0;
    }
    u32 magic;
    std::memcpy(&magic, cmif, 4);
    std::memcpy(&r.cmd, cmif + 8, 4);
    r.data = cmif + 16;
    if (magic != MagicIn) {
        LOG("ipc: %s request without SFCI (type %u, data word %u, %u words):", target->name.c_str(), p.type, p.data_word,
            p.data_words);
        for (int k = 0; k < 16; k += 4) LOG("  %08x %08x %08x %08x", w[k], w[k + 1], w[k + 2], w[k + 3]);
    }

    if (p.type == 5 || p.type == 7) {
        if (ipc_trace()) LOG("ipc %s control %u", target->name.c_str(), r.cmd);
        std::scoped_lock lk{sess->d->lock};
        control(*sess, r.cmd, r);
        write_reply(w, r, false, false, {});
        return ResultOk;
    }
    if (p.type != 4 && p.type != 6) {
        LOG("ipc: unsupported message type %u to %s", p.type, target->name.c_str());
        return KResult::NotImplemented;
    }

    if (ipc_trace()) LOG("ipc %s::%u", target->name.c_str(), r.cmd);
    {
        std::scoped_lock lk{target->lock};
        target->handle(r);
    }
    if (ipc_trace() && r.result) LOG("ipc %s::%u -> %#x", target->name.c_str(), r.cmd, r.result);

    std::vector<u32> ids;
    if (domain) {
        std::scoped_lock lk{sess->d->lock};
        for (auto& o : r.out_objects) ids.push_back(sess->d->add_object(o));
    } else {
        std::vector<u32> handles;
        for (auto& o : r.out_objects) handles.push_back(new_session(o));
        r.out_move.insert(r.out_move.begin(), handles.begin(), handles.end());
    }
    write_reply(w, r, false, domain, ids);
    return ResultOk;
}

}  // namespace ipc
