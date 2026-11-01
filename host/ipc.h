// HIPC messages (CMIF and TIPC framing, domains) dispatched to native service objects.
#pragma once
#include <functional>
#include <map>
#include <mutex>

#include "kernel.h"

namespace ipc {

struct Buffer {
    u64 addr = 0, size = 0;
};

struct Service;
using ServicePtr = std::shared_ptr<Service>;

struct Request {
    u32 cmd = 0;
    bool tipc = false;
    u64 pid = 0;
    std::vector<u32> copy_handles, move_handles, in_objects;
    std::vector<Buffer> x, a, b, w, c;
    const u8* data = nullptr;  // arguments after the CMIF header (or the TIPC header)
    size_t data_size = 0;

    // Reply.
    u32 result = ResultOk;
    std::vector<u8> out;
    std::vector<u32> out_copy, out_move;
    std::vector<ServicePtr> out_objects;

    template <class T> T arg(size_t off) const {
        T v{};
        if (off + sizeof(T) <= data_size) std::memcpy(&v, data + off, sizeof(T));
        return v;
    }
    template <class T> void push(const T& v) {
        const size_t at = out.size();
        out.resize(at + sizeof(T));
        std::memcpy(out.data() + at, &v, sizeof(T));
    }
    void push_bytes(const void* p, size_t n) {
        const size_t at = out.size();
        out.resize(at + n);
        std::memcpy(out.data() + at, p, n);
    }
    void align_out(size_t a) { out.resize((out.size() + a - 1) & ~(a - 1)); }
    void push_object(ServicePtr s) { out_objects.push_back(std::move(s)); }

    // Input buffer i: A (send) or, when that is empty, X (send static); "auto select" buffers.
    Buffer in_buf(size_t i = 0) const;
    // Output buffer i: B (receive) or, when that is empty, C (receive static).
    Buffer out_buf(size_t i = 0) const;
    std::vector<u8> read_in(size_t i = 0) const;
    size_t write_out(size_t i, const void* p, size_t n) const;
};

struct Service : std::enable_shared_from_this<Service> {
    explicit Service(std::string name_) : name(std::move(name_)) {}
    virtual ~Service() = default;
    // Default: log the command once per service and answer success with zeroed output.
    virtual void handle(Request& r);
    std::string name;
    std::mutex lock;  // serializes this object's requests

  protected:
    using Handler = std::function<void(Request&)>;
    std::map<u32, Handler> cmds;
    void on(u32 id, Handler h) { cmds[id] = std::move(h); }
};

// A service with a fixed reply for each command: set from the table, otherwise zero output.
ServicePtr stub(std::string name);

// Named services reachable through sm.
void register_service(const std::string& name, std::function<ServicePtr()> make);
ServicePtr make_service(const std::string& name);

u32 new_session(ServicePtr s);  // handle to a fresh session on `s`
u32 connect_named_port(const std::string& name, u32* out_handle);
u32 send_sync_request(u32 handle, u64 msg_addr);

void register_all_services();  // services/*.cpp

}  // namespace ipc
