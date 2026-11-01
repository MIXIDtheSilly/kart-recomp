// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// The nvdrv module (from Eglantine's nvdrv.cpp / nvdrv_interface.cpp) on the host: device nodes
// behind nx2:nvsrv, which gsv 1402 hands out. Requests use the nvdrv INvDrvServices protocol (CMIF).

#include <mutex>
#include <set>

#include "common/logging.h"
#include "core/core.h"
#include "ipc.h"
#include "nv/core/container.h"
#include "nv/core/syncpoint_manager.h"
#include "nv/devices/nvdevice.h"
#include "nv/devices/nvhost_as_gpu.h"
#include "nv/devices/nvhost_ctrl.h"
#include "nv/devices/nvhost_ctrl_gpu.h"
#include "nv/devices/nvhost_gpu.h"
#include "nv/devices/nvmap.h"
#include "nv/devices/nx2_stub_device.h"
#include "nv/kevent.h"
#include "nv/nvdrv.h"
#include "nv/nvsrv.h"
#include "video.h"
#include "video_core/gpu.h"
#include "video_core/nx2.h"

namespace Service::Nvidia {

EventInterface::EventInterface(Module& module_) : module{module_}, guard{}, on_signal{} {}

EventInterface::~EventInterface() = default;

Kernel::KEvent* EventInterface::CreateEvent(std::string) {
    return new Kernel::KEvent();
}

void EventInterface::FreeEvent(Kernel::KEvent* event) {
    delete event;
}

Module::Module(Core::System& system) : container{system.Host1x()}, events_interface{*this} {
    Devices::Nx2GpuMemoryRegistry::Get().Reset();
    Tegra::Nx2ClearSemaphoreRegions();
    Tegra::Nx2ClearSparseRegions();
    Devices::g_nx2_driver = false;
    Tegra::g_nx2_ampere = false;
    nx2_stub_builder = [this, &system](DeviceFD fd, const std::string& name) {
        auto device = std::make_shared<Devices::nx2_stub_device>(system, name);
        open_files.emplace(fd, device);
        return std::static_pointer_cast<Devices::nvdevice>(device);
    };
    builders["/dev/nvhost-as-gpu"] = [this, &system](DeviceFD fd) {
        auto device = std::make_shared<Devices::nvhost_as_gpu>(system, *this, container);
        return open_files.emplace(fd, std::move(device)).first;
    };
    builders["/dev/nvhost-gpu"] = [this, &system](DeviceFD fd) {
        auto device = std::make_shared<Devices::nvhost_gpu>(system, events_interface, container);
        return open_files.emplace(fd, std::move(device)).first;
    };
    builders["/dev/nvhost-ctrl-gpu"] = [this, &system](DeviceFD fd) {
        auto device = std::make_shared<Devices::nvhost_ctrl_gpu>(system, events_interface);
        return open_files.emplace(fd, std::move(device)).first;
    };
    builders["/dev/nvmap"] = [this, &system](DeviceFD fd) {
        auto device = std::make_shared<Devices::nvmap>(system, container);
        return open_files.emplace(fd, std::move(device)).first;
    };
    builders["/dev/nvhost-ctrl"] = [this, &system](DeviceFD fd) {
        auto device = std::make_shared<Devices::nvhost_ctrl>(system, events_interface, container);
        return open_files.emplace(fd, std::move(device)).first;
    };
}

Module::~Module() {}

NvResult Module::VerifyFD(DeviceFD fd) const {
    if (fd < 0) {
        return NvResult::InvalidState;
    }
    return open_files.find(fd) == open_files.end() ? NvResult::NotImplemented : NvResult::Success;
}

DeviceFD Module::Open(const std::string& device_name, NvCore::SessionId session_id) {
    auto it = builders.find(device_name);
    if (it == builders.end() && device_name.starts_with("/dev/nvhost-")) {
        // Newer nvgpu nodes used by Switch 2 drivers (e.g. /dev/nvhost-tsg-gpu).
        LOG("nvsrv: opening unknown device %s as a stub", device_name.c_str());
        const DeviceFD fd = next_fd++;
        auto device = nx2_stub_builder(fd, device_name);
        device->OnOpen(session_id, fd);
        return fd;
    }
    if (it == builders.end()) {
        LOG("nvsrv: unknown device %s", device_name.c_str());
        return INVALID_NVDRV_FD;
    }
    const DeviceFD fd = next_fd++;
    auto device = it->second(fd)->second;
    device->OnOpen(session_id, fd);
    return fd;
}

NvResult Module::Ioctl1(DeviceFD fd, Ioctl command, std::span<const u8> input, std::span<u8> output) {
    const auto itr = open_files.find(fd);
    if (fd < 0 || itr == open_files.end()) {
        return NvResult::NotImplemented;
    }
    return itr->second->Ioctl1(fd, command, input, output);
}

NvResult Module::Ioctl2(DeviceFD fd, Ioctl command, std::span<const u8> input,
                        std::span<const u8> inline_input, std::span<u8> output) {
    const auto itr = open_files.find(fd);
    if (fd < 0 || itr == open_files.end()) {
        return NvResult::NotImplemented;
    }
    return itr->second->Ioctl2(fd, command, input, inline_input, output);
}

NvResult Module::Ioctl3(DeviceFD fd, Ioctl command, std::span<const u8> input, std::span<u8> output,
                        std::span<u8> inline_output) {
    const auto itr = open_files.find(fd);
    if (fd < 0 || itr == open_files.end()) {
        return NvResult::NotImplemented;
    }
    return itr->second->Ioctl3(fd, command, input, output, inline_output);
}

NvResult Module::Close(DeviceFD fd) {
    const auto itr = open_files.find(fd);
    if (fd < 0 || itr == open_files.end()) {
        return NvResult::NotImplemented;
    }
    itr->second->OnClose(fd);
    open_files.erase(itr);
    return NvResult::Success;
}

NvResult Module::QueryEvent(DeviceFD fd, u32 event_id, Kernel::KEvent*& event) {
    const auto itr = open_files.find(fd);
    if (fd < 0 || itr == open_files.end()) {
        return NvResult::NotImplemented;
    }
    event = itr->second->QueryEvent(event_id);
    return event ? NvResult::Success : NvResult::BadParameter;
}

}  // namespace Service::Nvidia

namespace nv {
namespace {

using namespace Service::Nvidia;

// One module for the process; the devices aren't written for concurrent requests (Eglantine
// serves them all from one service thread), so requests take the module lock.
std::mutex g_module_lock;
Module* g_module = nullptr;
NvCore::SessionId g_session{};

Module& module() {
    if (!g_module) {
        g_module = new Module(video::system());
        g_session = g_module->GetContainer().OpenSession(nullptr);
    }
    return *g_module;
}

bool trace_ioctls() {
    static const bool on = std::getenv("HOST_NV_TRACE") != nullptr;
    return on;
}

struct Nvsrv : ipc::Service {
    Nvsrv() : Service("nx2:nvsrv") {
        on(0, [](ipc::Request& r) {  // Open
            const auto name_bytes = r.read_in(0);
            const std::string name(reinterpret_cast<const char*>(name_bytes.data()),
                                   strnlen(reinterpret_cast<const char*>(name_bytes.data()), name_bytes.size()));
            std::scoped_lock lk{g_module_lock};
            const DeviceFD fd = module().Open(name, g_session);
            LOG("nvsrv: open %s -> fd %d", name.c_str(), fd);
            r.push<s32>(fd);
            r.push<u32>(static_cast<u32>(fd != INVALID_NVDRV_FD ? NvResult::Success : NvResult::FileOperationFailed));
        });
        on(1, [](ipc::Request& r) { ioctl(r, 1); });
        on(11, [](ipc::Request& r) { ioctl(r, 2); });
        on(12, [](ipc::Request& r) { ioctl(r, 3); });
        on(2, [](ipc::Request& r) {  // Close
            std::scoped_lock lk{g_module_lock};
            r.push<u32>(static_cast<u32>(module().Close(r.arg<s32>(0))));
        });
        on(3, [](ipc::Request& r) { r.push<u32>(0); });  // Initialize (done for the process)
        on(4, [](ipc::Request& r) {                      // QueryEvent
            const s32 fd = r.arg<s32>(0);
            const u32 id = r.arg<u32>(4);
            Kernel::KEvent* event = nullptr;
            NvResult result;
            {
                std::scoped_lock lk{g_module_lock};
                result = module().QueryEvent(fd, id, event);
            }
            if (result == NvResult::Success) r.out_copy.push_back(event->NewHandle());
            else LOG("nvsrv: QueryEvent fd %d event %#x failed (%#x)", fd, id, static_cast<u32>(result));
            r.push<u32>(static_cast<u32>(result));
        });
        on(6, [](ipc::Request& r) { r.push<u32>(0); });  // GetStatus
        on(8, [](ipc::Request& r) { r.push<u32>(0); });  // SetAruid
        on(9, [](ipc::Request&) {});                     // DumpGraphicsMemoryInfo
        on(13, [](ipc::Request&) {});                    // SetGraphicsFirmwareMemoryMarginEnabled
    }

    // Ioctl (1), Ioctl2 (11: a second input buffer, inlined), Ioctl3 (12: a second output buffer).
    static void ioctl(ipc::Request& r, int kind) {
        const DeviceFD fd = r.arg<s32>(0);
        Ioctl command{};
        command.raw = r.arg<u32>(4);
        const auto input = r.read_in(0);
        std::vector<u8> output(r.out_buf(0).size);
        std::vector<u8> inline_output;
        NvResult result;
        {
            std::scoped_lock lk{g_module_lock};
            if (kind == 1) {
                result = module().Ioctl1(fd, command, input, output);
            } else if (kind == 2) {
                const auto inline_input = r.read_in(1);
                result = module().Ioctl2(fd, command, input, inline_input, output);
            } else {
                inline_output.resize(r.out_buf(1).size);
                result = module().Ioctl3(fd, command, input, output, inline_output);
            }
        }
        if (trace_ioctls()) {
            LOG("nvsrv: ioctl%d fd %d %08x in %zu out %zu -> %#x", kind, fd, command.raw, input.size(),
                output.size(), static_cast<u32>(result));
        }
        if (result == NvResult::NotImplemented) {
            static std::set<u32> seen;
            std::scoped_lock lk{g_module_lock};
            if (seen.insert(command.raw).second) LOG("nvsrv: unimplemented ioctl %08x on fd %d", command.raw, fd);
        }
        if (command.is_out) {
            r.write_out(0, output.data(), output.size());
            if (kind == 3) r.write_out(1, inline_output.data(), inline_output.size());
        }
        r.push<u32>(static_cast<u32>(result));
    }
};

}  // namespace

ipc::ServicePtr make_nvsrv() {
    return std::make_shared<Nvsrv>();
}

u32 allocate_syncpoint() {
    std::scoped_lock lk{g_module_lock};
    return module().GetContainer().GetSyncpointManager().AllocateSyncpoint(false);
}

void free_syncpoint(u32 id) {
    std::scoped_lock lk{g_module_lock};
    module().GetContainer().GetSyncpointManager().FreeSyncpoint(id);
}

}  // namespace nv
