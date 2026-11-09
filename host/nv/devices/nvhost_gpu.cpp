#pragma GCC diagnostic ignored "-Wunused-variable"
// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <cstdlib>
#include <cstring>
#include <vector>
#include <mutex>
#include <map>
#include "common/assert.h"
#include "common/logging.h"
#include "core/core.h"
#include "nv/kevent.h"
#include "nv/core/container.h"
#include "nv/core/nvmap.h"
#include "nv/core/syncpoint_manager.h"
#include "nv/devices/ioctl_serialization.h"
#include "nv/devices/nvhost_gpu.h"
#include "nv/devices/nx2_stub_device.h"
#include "nv/nvdrv.h"
#include "nv/nvsrv.h"
#include "core/memory.h"
#include "video_core/control/channel_state.h"
#include "video_core/engines/puller.h"
#include "video_core/memory_manager.h"
#include "video_core/gpu.h"
#include "video_core/host1x/host1x.h"

extern std::atomic<unsigned long long> g_host_present_count;

namespace {
std::mutex g_channel_fences_lock;
std::map<s32, std::pair<u32, u32>> g_channel_fences;  // bind id -> latest fence
}  // namespace

namespace nv {
std::vector<std::pair<u32, u32>> latest_channel_fences() {
    std::scoped_lock lk{g_channel_fences_lock};
    std::vector<std::pair<u32, u32>> fences;
    for (const auto& [channel, fence] : g_channel_fences) fences.push_back(fence);
    return fences;
}
}  // namespace nv

namespace Service::Nvidia::Devices {
namespace {
Tegra::CommandHeader BuildFenceAction(Tegra::Engines::Puller::FenceOperation op, u32 syncpoint_id) {
    Tegra::Engines::Puller::FenceAction result{};
    result.op.Assign(op);
    result.syncpoint_id.Assign(syncpoint_id);
    return {result.raw};
}
} // namespace

nvhost_gpu::nvhost_gpu(Core::System& system_, EventInterface& events_interface_,
                       NvCore::Container& core_)
    : nvdevice{system_}, events_interface{events_interface_}, core{core_},
      syncpoint_manager{core_.GetSyncpointManager()}, nvmap{core.GetNvMapFile()},
      channel_state{system.GPU().AllocateChannel()} {
    channel_syncpoint = syncpoint_manager.AllocateSyncpoint(false);
    sm_exception_breakpoint_int_report_event =
        events_interface.CreateEvent("GpuChannelSMExceptionBreakpointInt");
    sm_exception_breakpoint_pause_report_event =
        events_interface.CreateEvent("GpuChannelSMExceptionBreakpointPause");
    error_notifier_event = events_interface.CreateEvent("GpuChannelErrorNotifier");
}

nvhost_gpu::~nvhost_gpu() {
    events_interface.FreeEvent(sm_exception_breakpoint_int_report_event);
    events_interface.FreeEvent(sm_exception_breakpoint_pause_report_event);
    events_interface.FreeEvent(error_notifier_event);
    for (auto& [id, event] : nx2_events) {
        events_interface.FreeEvent(event);
    }
    syncpoint_manager.FreeSyncpoint(channel_syncpoint);
}

NvResult nvhost_gpu::Ioctl1(DeviceFD fd, Ioctl command, std::span<const u8> input,
                            std::span<u8> output) {
    switch (command.group) {
    case 0x0:
        switch (command.cmd) {
        case 0x3:
            return WrapFixed(this, &nvhost_gpu::GetWaitbase, input, output);
        default:
            break;
        }
        break;
    case 'H':
        switch (command.cmd) {
        case 0x1:
            return WrapFixed(this, &nvhost_gpu::SetNVMAPfd, input, output);
        case 0x3:
            return WrapFixed(this, &nvhost_gpu::ChannelSetTimeout, input, output);
        case 0x8:
            return WrapFixedVariable(this, &nvhost_gpu::SubmitGPFIFOBase1, input, output, fd, false);
        case 0x9:
            return WrapFixed(this, &nvhost_gpu::AllocateObjectContext, input, output);
        case 0xb:
            return WrapFixed(this, &nvhost_gpu::ZCullBind, input, output);
        case 0xc:
            return WrapFixed(this, &nvhost_gpu::SetErrorNotifier, input, output);
        case 0xd:
            return WrapFixed(this, &nvhost_gpu::SetChannelPriority, input, output);
        case 0x18:
            return WrapFixed(this, &nvhost_gpu::AllocGPFIFOEx, input, output, fd);
        case 0x1a:
            return WrapFixed(this, &nvhost_gpu::AllocGPFIFOEx2, input, output, fd);
        case 0x1b:
            return WrapFixedVariable(this, &nvhost_gpu::SubmitGPFIFOBase1, input, output, fd, true);
        case 0x1d:
            return WrapFixed(this, &nvhost_gpu::ChannelSetTimeslice, input, output);
        default:
            break;
        }
        break;
    case 'G':
        switch (command.cmd) {
        case 0x14:
            return WrapFixed(this, &nvhost_gpu::SetClientData, input, output);
        case 0x15:
            return WrapFixed(this, &nvhost_gpu::GetClientData, input, output);
        default:
            break;
        }
        break;
    }
    if (g_nx2_driver) {
        return Nx2UnknownIoctl("nvhost-gpu", command, input, output);
    }
    UNIMPLEMENTED_MSG("Unimplemented ioctl={:08X}", command.raw);
    return NvResult::NotImplemented;
};

NvResult nvhost_gpu::Ioctl2(DeviceFD fd, Ioctl command, std::span<const u8> input,
                            std::span<const u8> inline_input, std::span<u8> output) {
    switch (command.group) {
    case 'H':
        switch (command.cmd) {
        case 0x1b:
            return WrapFixedInlIn(this, &nvhost_gpu::SubmitGPFIFOBase2, input, inline_input,
                                  output);
        }
        break;
    }
    if (g_nx2_driver) {
        return Nx2UnknownIoctl("nvhost-gpu", command, input, output);
    }
    UNIMPLEMENTED_MSG("Unimplemented ioctl={:08X}", command.raw);
    return NvResult::NotImplemented;
}

NvResult nvhost_gpu::Ioctl3(DeviceFD fd, Ioctl command, std::span<const u8> input,
                            std::span<u8> output, std::span<u8> inline_output) {
    if (g_nx2_driver) {
        std::fill(inline_output.begin(), inline_output.end(), u8{0});
        return Nx2UnknownIoctl("nvhost-gpu", command, input, output);
    }
    UNIMPLEMENTED_MSG("Unimplemented ioctl={:08X}", command.raw);
    return NvResult::NotImplemented;
}

void nvhost_gpu::OnOpen(NvCore::SessionId session_id, DeviceFD fd) {
    sessions[fd] = session_id;
}

void nvhost_gpu::OnClose(DeviceFD fd) {
    sessions.erase(fd);
}

NvResult nvhost_gpu::SetNVMAPfd(IoctlSetNvmapFD& params) {
    LOG_DEBUG(Service_NVDRV, "called, fd={}", params.nvmap_fd);

    nvmap_fd = params.nvmap_fd;
    return NvResult::Success;
}

NvResult nvhost_gpu::SetClientData(IoctlClientData& params) {
    LOG_DEBUG(Service_NVDRV, "called");
    user_data = params.data;
    return NvResult::Success;
}

NvResult nvhost_gpu::GetClientData(IoctlClientData& params) {
    LOG_DEBUG(Service_NVDRV, "called");
    params.data = user_data;
    return NvResult::Success;
}

NvResult nvhost_gpu::ZCullBind(IoctlZCullBind& params) {
    zcull_params = params;
    LOG_DEBUG(Service_NVDRV, "called, gpu_va={:X}, mode={:X}", zcull_params.gpu_va,
              zcull_params.mode);
    return NvResult::Success;
}

NvResult nvhost_gpu::SetErrorNotifier(IoctlSetErrorNotifier& params) {
    LOG_WARNING(Service_NVDRV, "(STUBBED) called, offset={:X}, size={:X}, mem={:X}", params.offset,
                params.size, params.mem);
    return NvResult::Success;
}

NvResult nvhost_gpu::SetChannelPriority(IoctlChannelSetPriority& params) {
    channel_priority = params.priority;
    LOG_INFO(Service_NVDRV, "called, priority={:X}", channel_priority);

    switch (static_cast<ChannelPriority>(channel_priority)) {
    case ChannelPriority::Low: channel_timeslice = 1300; break;
    case ChannelPriority::Medium: channel_timeslice = 2600; break;
    case ChannelPriority::High: channel_timeslice = 5200; break;
    default : return NvResult::BadParameter;
    }

    return NvResult::Success;
}

NvResult nvhost_gpu::AllocGPFIFOEx(IoctlAllocGpfifoEx& params, DeviceFD fd) {
    LOG_DEBUG(Service_NVDRV, "called, num_entries={:X}, flags={:X}, reserved1={:X}, "
              "reserved2={:X}, reserved3={:X}",
              params.num_entries, params.flags, params.reserved[0], params.reserved[1],
              params.reserved[2]);

    if (channel_state->initialized) {
        LOG_DEBUG(Service_NVDRV, "Channel already initialized; AllocGPFIFOEx returning AlreadyAllocated");
        return NvResult::AlreadyAllocated;
    }

    u64 program_id{};
    if (auto* const session = core.GetSession(sessions[fd]); session != nullptr) {
        program_id = ProgramId;
    }

    // Store program id for later lazy initialization
    channel_state->program_id = program_id;

    // If address space is not yet bound, defer channel initialization.
    if (!channel_state->memory_manager) {
        params.fence_out = syncpoint_manager.GetSyncpointFence(channel_syncpoint);
        return NvResult::Success;
    }

    system.GPU().InitChannel(*channel_state, program_id);

    params.fence_out = syncpoint_manager.GetSyncpointFence(channel_syncpoint);

    return NvResult::Success;
}

NvResult nvhost_gpu::AllocGPFIFOEx2(IoctlAllocGpfifoEx& params, DeviceFD fd) {
    LOG_DEBUG(Service_NVDRV,
              "called, num_entries={:X}, flags={:X}, reserved1={:X}, "
              "reserved2={:X}, reserved3={:X}",
              params.num_entries, params.flags, params.reserved[0], params.reserved[1],
              params.reserved[2]);

    if (channel_state->initialized) {
        LOG_DEBUG(Service_NVDRV, "Channel already initialized; AllocGPFIFOEx2 returning AlreadyAllocated");
        return NvResult::AlreadyAllocated;
    }

    u64 program_id{};
    if (auto* const session = core.GetSession(sessions[fd]); session != nullptr) {
        program_id = ProgramId;
    }

    // Store program id for later lazy initialization
    channel_state->program_id = program_id;

    // If address space is not yet bound, defer channel initialization.
    if (!channel_state->memory_manager) {
        params.fence_out = syncpoint_manager.GetSyncpointFence(channel_syncpoint);
        return NvResult::Success;
    }

    system.GPU().InitChannel(*channel_state, program_id);

    params.fence_out = syncpoint_manager.GetSyncpointFence(channel_syncpoint);

    return NvResult::Success;
}

s32_le nvhost_gpu::GetObjectContextClassNumberIndex(CtxClasses class_number) {
    constexpr s32_le invalid_class_number_index = -1;
    switch (class_number) {
    case CtxClasses::Ctx2D: return 0;
    case CtxClasses::Ctx3D: return 1;
    case CtxClasses::CtxCompute: return 2;
    case CtxClasses::CtxKepler: return 3;
    case CtxClasses::CtxDMA: return 4;
    case CtxClasses::CtxChannelGPFIFO: return 5;
    default: return invalid_class_number_index;
    }
}

NvResult nvhost_gpu::AllocateObjectContext(IoctlAllocObjCtx& params) {
    LOG_DEBUG(Service_NVDRV, "called, class_num={:#x}, flags={:#x}, obj_id={:#x}", params.class_num,
              params.flags, params.obj_id);

    // Do not require channel initialization here: some clients allocate contexts before binding.
    if (!channel_state) {
        LOG_ERROR(Service_NVDRV, "No channel state available!");
        return NvResult::InvalidState;
    }

    std::scoped_lock lk(channel_mutex);

    if (params.flags) {
        LOG_WARNING(Service_NVDRV, "non-zero flags={:#x} for class={:#x}", params.flags,
                    params.class_num);

        constexpr u32 allowed_mask{};
        params.flags = allowed_mask;
    }

    if (g_nx2_driver) {
        // Switch 2 NvRm asks for Ampere classes, or class 0 (it reads the class numbers from a
        // characteristics layout we don't know yet). Map them onto the Maxwell engine slots.
        u32 mapped = params.class_num;
        switch (params.class_num) {
        case 0xC797: case 0xC697: case 0xC597: mapped = static_cast<u32>(CtxClasses::Ctx3D); break;
        case 0xC7C0: case 0xC6C0: case 0xC5C0: mapped = static_cast<u32>(CtxClasses::CtxCompute); break;
        case 0xC7B5: case 0xC6B5: case 0xC5B5: mapped = static_cast<u32>(CtxClasses::CtxDMA); break;
        case 0xC56F: case 0xC76F: mapped = static_cast<u32>(CtxClasses::CtxChannelGPFIFO); break;
        case 0: {
            for (const auto candidate : {CtxClasses::Ctx3D, CtxClasses::CtxCompute, CtxClasses::CtxDMA,
                                         CtxClasses::Ctx2D, CtxClasses::CtxKepler}) {
                const auto index = GetObjectContextClassNumberIndex(candidate);
                if (index >= 0 && !ctxObjs[index].has_value()) {
                    mapped = static_cast<u32>(candidate);
                    break;
                }
            }
            break;
        }
        default:
            break;
        }
        if (mapped != params.class_num) {
            LOG_WARNING(Service_NVDRV, "Switch 2 object class {:#x} -> {:#x}", params.class_num, mapped);
            params.class_num = mapped;
        }
    }

    s32_le ctx_class_number_index =
        GetObjectContextClassNumberIndex(static_cast<CtxClasses>(params.class_num));
    if (ctx_class_number_index < 0) {
        LOG_ERROR(Service_NVDRV, "Invalid class number for object context: {:#x}",
                  params.class_num);
        return NvResult::BadParameter;
    }

    if (ctxObjs[ctx_class_number_index].has_value()) {
        LOG_WARNING(Service_NVDRV, "Object context for class {:#x} already allocated on this channel",
                    params.class_num);
        return NvResult::AlreadyAllocated;
    }

    // Defer actual hardware context binding until channel is initialized.
    ctxObjs[ctx_class_number_index] = params;

    return NvResult::Success;
}

static boost::container::small_vector<Tegra::CommandHeader, 512> BuildWaitCommandList(
    NvFence fence) {
    return {
        Tegra::BuildCommandHeader(Tegra::BufferMethods::SyncpointPayload, 1,
                                  Tegra::SubmissionMode::Increasing),
        {fence.value},
        Tegra::BuildCommandHeader(Tegra::BufferMethods::SyncpointOperation, 1,
                                  Tegra::SubmissionMode::Increasing),
        BuildFenceAction(Tegra::Engines::Puller::FenceOperation::Acquire, fence.id),
    };
}

static boost::container::small_vector<Tegra::CommandHeader, 512> BuildIncrementCommandList(
    NvFence fence) {
    boost::container::small_vector<Tegra::CommandHeader, 512> result{
        Tegra::BuildCommandHeader(Tegra::BufferMethods::SyncpointPayload, 1,
                                  Tegra::SubmissionMode::Increasing),
        {}};

    for (u32 count = 0; count < 2; ++count) {
        result.push_back(Tegra::BuildCommandHeader(Tegra::BufferMethods::SyncpointOperation, 1,
                                                   Tegra::SubmissionMode::Increasing));
        result.push_back(
            BuildFenceAction(Tegra::Engines::Puller::FenceOperation::Increment, fence.id));
    }

    return result;
}

// Switch 2 pushbuffers signal completion with semaphore releases, never host1x syncpoint methods,
// but the driver still has the kernel raise the syncpoint max by the submission's increment value
// and then waits for that value. Append the increments after its commands.
static boost::container::small_vector<Tegra::CommandHeader, 512> BuildIncrementCountCommandList(
    NvFence fence, u32 count) {
    boost::container::small_vector<Tegra::CommandHeader, 512> result{
        Tegra::BuildCommandHeader(Tegra::BufferMethods::WaitForIdle, 1,
                                  Tegra::SubmissionMode::Increasing),
        {},
        Tegra::BuildCommandHeader(Tegra::BufferMethods::SyncpointPayload, 1,
                                  Tegra::SubmissionMode::Increasing),
        {}};
    for (u32 i = 0; i < count; ++i) {
        result.push_back(Tegra::BuildCommandHeader(Tegra::BufferMethods::SyncpointOperation, 1,
                                                   Tegra::SubmissionMode::Increasing));
        result.push_back(
            BuildFenceAction(Tegra::Engines::Puller::FenceOperation::Increment, fence.id));
    }
    return result;
}

static boost::container::small_vector<Tegra::CommandHeader, 512> BuildIncrementWithWfiCommandList(
    NvFence fence) {
    boost::container::small_vector<Tegra::CommandHeader, 512> result{
        Tegra::BuildCommandHeader(Tegra::BufferMethods::WaitForIdle, 1,
                                  Tegra::SubmissionMode::Increasing),
        {}};
    auto increment_list{BuildIncrementCommandList(fence)};
    result.insert(result.end(), increment_list.begin(), increment_list.end());
    return result;
}

namespace {
thread_local bool g_logged_small_submit = false;
}

NvResult nvhost_gpu::SubmitGPFIFOImpl(IoctlSubmitGpfifo& params, Tegra::CommandList&& entries) {
    LOG_TRACE(Service_NVDRV, "called, gpfifo={:X}, num_entries={:X}, flags={:X}", params.address,
              params.num_entries, params.flags.raw);

    auto& gpu = system.GPU();

    std::scoped_lock lock(channel_mutex);

    // Lazily initialize channel when address space is available
    if (!channel_state->initialized && channel_state->memory_manager) {
        system.GPU().InitChannel(*channel_state, channel_state->program_id);
    }

    const auto bind_id = channel_state->bind_id;

    if (g_nx2_driver) {
        for (const auto& entry : entries.command_lists) {
            LOG_TRACE(Service_NVDRV, "nx2 GPFIFO entry raw={:#018x} addr={:#x} size={:#x}", entry.raw,
                     entry.addr.Value(), entry.size.Value());
        }
        static const bool check = std::getenv("EDEN_NX2_PB_CHECK") != nullptr;
        if (check) {
            std::vector<u32> words;
            for (const auto& entry : entries.command_lists) {
                words.resize(entry.size);
                system.ApplicationMemory().ReadBlock(entry.addr.Value(), words.data(),
                                                     words.size() * sizeof(u32));
                entries.submit_hashes.push_back(Tegra::HashPushbufferWords(words));
                // Small entries (NVN's queue-internal commands) with their words, for the first few.
                static std::atomic<int> small_logged{0};
                if (entry.size <= 8 && small_logged++ < 6000) {
                    g_logged_small_submit = true;
                    std::string text;
                    for (u32 w : words) text += fmt::format(" {:08x}", w);
                    LOG("nx2: submit entry %#llx+%#x (of %zu, flags %#x, fence id %u):%s", entry.addr.Value(),
                        static_cast<u32>(entry.size.Value()), entries.command_lists.size(), params.flags.raw,
                        channel_syncpoint, text.c_str());
                }
                // Command buffers (big entries) with the present they were submitted after, to
                // see how many frames apart the game reuses their memory.
                static std::atomic<int> big_logged{0};
                if (entry.size >= 0x100 && big_logged++ < 30000) {
                    LOG("nx2: submit cmdbuf %#llx+%#x after present %llu (fence id %u)", entry.addr.Value(),
                        static_cast<u32>(entry.size.Value()), g_host_present_count.load(), channel_syncpoint);
                }
                // Host: does the GPU see the same bytes at this address as the CPU right now?
                if (channel_state->memory_manager) {
                    std::vector<u32> gpu_words(entry.size);
                    channel_state->memory_manager->ReadBlockUnsafe(entry.addr.Value(), gpu_words.data(),
                                                                    gpu_words.size() * sizeof(u32));
                    static std::atomic<int> mismatches{0};
                    if (gpu_words != words && mismatches++ < 16) {
                        const auto device = channel_state->memory_manager->GpuToCpuAddress(entry.addr.Value());
                        LOG("nx2: pushbuffer %#llx+%#x differs between the CPU and GPU views at submit (device %#llx, cpu page phys %p)",
                            entry.addr.Value(), static_cast<u32>(entry.size.Value()),
                            device ? static_cast<unsigned long long>(*device) : 0ull,
                            static_cast<void*>(mem::backing_ptr(entry.addr.Value())));
                    }
                }
            }
        }
        // With frames ahead the present returns before the GPU thread reaches this frame's work,
        // and the game then refills command memory it already submitted. Keep the words as
        // submitted (video_core dma_pusher: lists that waited behind an acquire are copied again
        // at the resume, as the game writes them just before releasing it; upstream cce6d44af2).
        // Opt-in, EDEN_NX2_PB_SNAPSHOT=1: waiting for each frame, memory is current at the parse,
        // and with HOST_FRAMES_AHEAD the game still broke right after running ahead started, with
        // or without the copies (2026-11-06).
        static const bool snapshot = [] {
            const char* value = std::getenv("EDEN_NX2_PB_SNAPSHOT");
            return value != nullptr && value[0] != '0';
        }();
        if (snapshot && channel_state->memory_manager && channel_state->payload) {
            // Before the copy: a suspension during it must count as one after it.
            entries.snapshot_suspensions =
                channel_state->payload->dma_pusher.suspensions.load(std::memory_order_acquire);
            size_t words = 0;
            for (const auto& entry : entries.command_lists) {
                words += entry.size;
            }
            entries.submitted_words.resize(words);
            entries.submitted_offsets.reserve(entries.command_lists.size());
            size_t offset = 0;
            for (const auto& entry : entries.command_lists) {
                entries.submitted_offsets.push_back(static_cast<u32>(offset));
                channel_state->memory_manager->ReadBlockUnsafe(
                    entry.addr, entries.submitted_words.data() + offset, entry.size * sizeof(u32));
                offset += entry.size;
            }
        }
    }

    auto& flags = params.flags;

    if (flags.fence_wait.Value()) {
        if (flags.increment_value.Value()) {
            return NvResult::BadParameter;
        }

        if (!syncpoint_manager.IsFenceSignalled(params.fence)) {
            gpu.PushGPUEntries(bind_id, Tegra::CommandList{BuildWaitCommandList(params.fence)});
        }
    }

    params.fence.id = channel_syncpoint;

    u32 increment{(flags.fence_increment.Value() != 0 ? 2 : 0) +
                  (flags.increment_value.Value() != 0 ? params.fence.value : 0)};
    params.fence.value = syncpoint_manager.IncrementSyncpointMaxExt(channel_syncpoint, increment);
    if (g_logged_small_submit) {
        g_logged_small_submit = false;
        LOG("nx2: submit fence out (%u, %u), increment %u, guest value now %u", channel_syncpoint, params.fence.value,
            increment, system.Host1x().GetSyncpointManager().GetGuestSyncpointValue(channel_syncpoint));
    }
    // Host: CPU writes to GPU-modified memory are collected (video::on_cpu_write_fault) and applied
    // when the GPU syncs, which video_core does at fences only; these commands may read them
    // before the next one (seen as frames with stale vertex/constant data). Apply them first, in
    // order with the commands, as the console's GPU sees CPU writes made before a submit.
    gpu.RunInOrder([&gpu] { gpu.InvalidateGPUCache(); });
    gpu.PushGPUEntries(bind_id, std::move(entries));

    if (g_nx2_driver && !flags.fence_increment.Value() && increment > 0) {
        gpu.PushGPUEntries(bind_id,
                           Tegra::CommandList{BuildIncrementCountCommandList(params.fence, increment)});
    }

    if (flags.fence_increment.Value()) {
        if (flags.suppress_wfi.Value()) {
            gpu.PushGPUEntries(bind_id,
                               Tegra::CommandList{BuildIncrementCommandList(params.fence)});
        } else {
            gpu.PushGPUEntries(bind_id,
                               Tegra::CommandList{BuildIncrementWithWfiCommandList(params.fence)});
        }
    }

    if (g_nx2_driver) {
        LOG_TRACE(Service_NVDRV,
                 "nx2 SubmitGPFIFO entries={} flags={:#x} fence_out=({}, {}) guest_value={}",
                 params.num_entries, flags.raw, params.fence.id, params.fence.value,
                 system.Host1x().GetSyncpointManager().GetGuestSyncpointValue(params.fence.id));
    }
    if (increment > 0) {
        std::scoped_lock lk{g_channel_fences_lock};
        g_channel_fences[bind_id] = {params.fence.id, params.fence.value};
    }
    if (g_timeline_on) {
        TIMELINE("submit ch=%d n=%u fence=%u:%u", bind_id, static_cast<u32>(params.num_entries), params.fence.id,
                 params.fence.value);
        if (increment > 0) {
            system.Host1x().GetSyncpointManager().RegisterGuestAction(params.fence.id, params.fence.value,
                                                  [bind_id, id = params.fence.id, value = params.fence.value] {
                                                      TIMELINE("fence ch=%d fence=%u:%u", bind_id, id, value);
                                                  });
        }
    }
    flags.raw = 0;

    return NvResult::Success;
}

Core::Memory::Memory& nvhost_gpu::GetSessionMemory(DeviceFD fd) {
    // One process: the application's memory.
    return system.ApplicationMemory();
}

NvResult nvhost_gpu::SubmitGPFIFOBase1(IoctlSubmitGpfifo& params,
                                       std::span<Tegra::CommandListHeader> commands, DeviceFD fd,
                                       bool kickoff) {
    if (params.num_entries > commands.size()) {
        UNIMPLEMENTED();
        return NvResult::InvalidSize;
    }

    Tegra::CommandList entries(params.num_entries);
    if (kickoff) {
        this->GetSessionMemory(fd).ReadBlock(params.address, entries.command_lists.data(),
                                             params.num_entries * sizeof(Tegra::CommandListHeader));
    } else {
        std::memcpy(entries.command_lists.data(), commands.data(),
                    params.num_entries * sizeof(Tegra::CommandListHeader));
    }

    return SubmitGPFIFOImpl(params, std::move(entries));
}

NvResult nvhost_gpu::SubmitGPFIFOBase2(IoctlSubmitGpfifo& params,
                                       std::span<const Tegra::CommandListHeader> commands) {
    if (params.num_entries > commands.size()) {
        UNIMPLEMENTED();
        return NvResult::InvalidSize;
    }

    Tegra::CommandList entries(params.num_entries);
    std::memcpy(entries.command_lists.data(), commands.data(),
                params.num_entries * sizeof(Tegra::CommandListHeader));
    return SubmitGPFIFOImpl(params, std::move(entries));
}

NvResult nvhost_gpu::GetWaitbase(IoctlGetWaitbase& params) {
    LOG_INFO(Service_NVDRV, "called, unknown={:#x}", params.unknown);

    params.value = 0; // Seems to be hard coded at 0
    return NvResult::Success;
}

NvResult nvhost_gpu::ChannelSetTimeout(IoctlChannelSetTimeout& params) {
    LOG_INFO(Service_NVDRV, "called, timeout={:#x}", params.timeout);

    return NvResult::Success;
}

NvResult nvhost_gpu::ChannelSetTimeslice(IoctlSetTimeslice& params) {
    LOG_INFO(Service_NVDRV, "called, timeslice={:#x}", params.timeslice);

    if (params.timeslice < 1000 || params.timeslice > 5000) {
        return NvResult::BadParameter;
    }

    channel_timeslice = params.timeslice;

    return NvResult::Success;
}

Kernel::KEvent* nvhost_gpu::QueryEvent(u32 event_id) {
    switch (event_id) {
    case 1:
        return sm_exception_breakpoint_int_report_event;
    case 2:
        return sm_exception_breakpoint_pause_report_event;
    case 3:
        return error_notifier_event;
    default:
        if (g_nx2_driver) {
            // Switch 2 NVN also asks for event 7 and multi-waits on it with its syncpoint events.
            // Nothing signals it yet.
            auto*& event = nx2_events[event_id];
            if (event == nullptr) {
                event = events_interface.CreateEvent(fmt::format("GpuChannelNx2Event{}", event_id));
            }
            LOG_WARNING(Service_NVDRV, "(STUBBED) Switch 2 channel event {}", event_id);
            return event;
        }
        LOG_CRITICAL(Service_NVDRV, "Unknown Ctrl GPU Event {}", event_id);
        return nullptr;
    }
}

} // namespace Service::Nvidia::Devices
