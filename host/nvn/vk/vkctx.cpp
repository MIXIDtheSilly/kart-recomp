#include "vkctx.h"

#include "dump.h"
#include "hitch.h"
#include "vkq.h"

#include "../../memory.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <string>

namespace vkfn {

namespace {
HMODULE g_lib = nullptr;
}

bool load_global() {
    if (vkGetInstanceProcAddr) return true;
    g_lib = LoadLibraryW(L"vulkan-1.dll");
    if (!g_lib) return false;
    vkGetInstanceProcAddr = reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(g_lib, "vkGetInstanceProcAddr"));
    if (!vkGetInstanceProcAddr) return false;
#define X(name) name = reinterpret_cast<PFN_##name>(vkGetInstanceProcAddr(nullptr, #name));
    VKFN_GLOBAL(X)
#undef X
    return vkCreateInstance != nullptr;
}

void load_instance(VkInstance i) {
#define X(name) name = reinterpret_cast<PFN_##name>(vkGetInstanceProcAddr(i, #name));
    VKFN_INSTANCE(X)
#undef X
}

void load_device(VkDevice d) {
#define X(name) name = reinterpret_cast<PFN_##name>(vkGetDeviceProcAddr(d, #name));
    VKFN_DEVICE(X)
#undef X
}

}  // namespace vkfn

namespace nvk {

void vk_fail(VkResult r, const char* what) {
    Device& c = ctx();
    if (r == VK_ERROR_DEVICE_LOST && c.feat.device_fault && vkGetDeviceFaultInfoEXT) {
        VkDeviceFaultCountsEXT n{VK_STRUCTURE_TYPE_DEVICE_FAULT_COUNTS_EXT};
        if (vkGetDeviceFaultInfoEXT(c.dev, &n, nullptr) == VK_SUCCESS) {
            std::vector<VkDeviceFaultAddressInfoEXT> addr(n.addressInfoCount);
            std::vector<VkDeviceFaultVendorInfoEXT> vendor(n.vendorInfoCount);
            VkDeviceFaultInfoEXT fi{VK_STRUCTURE_TYPE_DEVICE_FAULT_INFO_EXT};
            fi.pAddressInfos = addr.data();
            fi.pVendorInfos = vendor.data();
            n.vendorBinarySize = 0;
            if (const VkResult fr = vkGetDeviceFaultInfoEXT(c.dev, &n, &fi); fr == VK_SUCCESS || fr == VK_INCOMPLETE) {
                LOG("[RED] vulkan: device lost: %s", fi.description);
                static const char* kinds[] = {"none", "read invalid", "write invalid", "execute invalid", "instruction pointer unknown",
                                              "instruction pointer invalid", "instruction pointer fault"};
                for (const auto& a : addr)
                    LOG("[RED] vulkan: fault: %s at %#llx (precision %#llx)", a.addressType <= 6 ? kinds[a.addressType] : "?",
                        static_cast<unsigned long long>(a.reportedAddress), static_cast<unsigned long long>(a.addressPrecision));
                for (const auto& v : vendor)
                    LOG("[RED] vulkan: fault vendor info: %s (code %#llx, data %#llx)", v.description,
                        static_cast<unsigned long long>(v.vendorFaultCode), static_cast<unsigned long long>(v.vendorFaultData));
            }
        }
    }
    if (r == VK_ERROR_DEVICE_LOST && c.feat.checkpoints && vkGetQueueCheckpointDataNV) {
        u32 n = 0;
        vkGetQueueCheckpointDataNV(c.queue, &n, nullptr);
        std::vector<VkCheckpointDataNV> cp(n, VkCheckpointDataNV{VK_STRUCTURE_TYPE_CHECKPOINT_DATA_NV});
        vkGetQueueCheckpointDataNV(c.queue, &n, cp.data());
        for (const auto& d : cp)
            LOG("[RED] vulkan: checkpoint at stage %#x: %s", d.stage, checkpoint_desc(reinterpret_cast<uintptr_t>(d.pCheckpointMarker)).c_str());
    }
    if (r == VK_ERROR_DEVICE_LOST) dump_trace_print();
    fatal("vulkan: %s failed (VkResult %d)", what, static_cast<int>(r));
}

Device& ctx() {
    static Device c;
    return c;
}

namespace {

VKAPI_ATTR VkBool32 VKAPI_CALL debug_cb(VkDebugUtilsMessageSeverityFlagBitsEXT sev, VkDebugUtilsMessageTypeFlagsEXT,
                                        const VkDebugUtilsMessengerCallbackDataEXT* d, void*) {
    static std::atomic<int> count{0};
    if (count++ < 2000) LOG("vulkan %s: %s", sev >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT ? "error" : "warning",
                            d->pMessage);
    return VK_FALSE;
}

bool has_ext(const std::vector<VkExtensionProperties>& exts, const char* name) {
    for (const auto& e : exts)
        if (std::strcmp(e.extensionName, name) == 0) return true;
    return false;
}

void create_instance(Device& c) {
    if (!load_global()) fatal("vulkan: vulkan-1.dll not found");
    u32 n = 0;
    vkEnumerateInstanceExtensionProperties(nullptr, &n, nullptr);
    std::vector<VkExtensionProperties> exts(n);
    vkEnumerateInstanceExtensionProperties(nullptr, &n, exts.data());
    std::vector<const char*> want{VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_WIN32_SURFACE_EXTENSION_NAME};
    const bool debug_utils = has_ext(exts, VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    if (debug_utils) want.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    // HOST_VK_VALIDATION=1: the Khronos validation layer, its messages in the log.
    std::vector<const char*> layers;
    const char* v = std::getenv("HOST_VK_VALIDATION");
    if (v && v[0] == '1') layers.push_back("VK_LAYER_KHRONOS_validation");
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "kart";
    app.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ci.pApplicationInfo = &app;
    ci.enabledExtensionCount = static_cast<u32>(want.size());
    ci.ppEnabledExtensionNames = want.data();
    ci.enabledLayerCount = static_cast<u32>(layers.size());
    ci.ppEnabledLayerNames = layers.data();
    check(vkCreateInstance(&ci, nullptr, &c.instance), "vkCreateInstance");
    load_instance(c.instance);
    if (!layers.empty() && debug_utils) {
        VkDebugUtilsMessengerCreateInfoEXT di{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
        di.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        di.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                         VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
        di.pfnUserCallback = debug_cb;
        VkDebugUtilsMessengerEXT m;
        vkCreateDebugUtilsMessengerEXT(c.instance, &di, nullptr, &m);
    }
}

// HOST_VULKAN_DEVICE=<index> picks a GPU; by default the discrete GPU with the most device memory.
VkPhysicalDevice pick_device(Device& c) {
    u32 n = 0;
    vkEnumeratePhysicalDevices(c.instance, &n, nullptr);
    std::vector<VkPhysicalDevice> devs(n);
    vkEnumeratePhysicalDevices(c.instance, &n, devs.data());
    if (devs.empty()) fatal("vulkan: no GPU");
    if (const char* v = std::getenv("HOST_VULKAN_DEVICE"); v && std::strcmp(v, "auto") != 0) {
        const u32 i = static_cast<u32>(std::atoi(v));
        if (i < devs.size()) return devs[i];
    }
    VkPhysicalDevice best = devs[0];
    u64 best_score = 0;
    for (VkPhysicalDevice d : devs) {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(d, &p);
        VkPhysicalDeviceMemoryProperties m;
        vkGetPhysicalDeviceMemoryProperties(d, &m);
        u64 vram = 0;
        for (u32 h = 0; h < m.memoryHeapCount; h++)
            if (m.memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) vram = std::max<u64>(vram, m.memoryHeaps[h].size);
        const u64 score = vram + (p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? (1ull << 50) : 0);
        if (score > best_score) best = d, best_score = score;
    }
    return best;
}

void create_device(Device& c) {
    c.phys = pick_device(c);
    vkGetPhysicalDeviceProperties(c.phys, &c.props);
    vkGetPhysicalDeviceMemoryProperties(c.phys, &c.mem_props);
    if (c.props.apiVersion < VK_API_VERSION_1_3) fatal("vulkan: %s has Vulkan %u.%u, 1.3 is needed", c.props.deviceName,
                                                       VK_API_VERSION_MAJOR(c.props.apiVersion),
                                                       VK_API_VERSION_MINOR(c.props.apiVersion));
    u32 n = 0;
    vkEnumerateDeviceExtensionProperties(c.phys, nullptr, &n, nullptr);
    std::vector<VkExtensionProperties> exts(n);
    vkEnumerateDeviceExtensionProperties(c.phys, nullptr, &n, exts.data());

    std::vector<const char*> want{VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    auto opt = [&](const char* name) {
        if (!has_ext(exts, name)) return false;
        want.push_back(name);
        return true;
    };
    c.feat.host_import = opt(VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME);
    const bool memory_budget = opt(VK_EXT_MEMORY_BUDGET_EXTENSION_NAME);
    opt(VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME);
    c.feat.mesh_shader = opt(VK_EXT_MESH_SHADER_EXTENSION_NAME);
    c.feat.eds3 = opt(VK_EXT_EXTENDED_DYNAMIC_STATE_3_EXTENSION_NAME);
    c.feat.vertex_input_dynamic = opt(VK_EXT_VERTEX_INPUT_DYNAMIC_STATE_EXTENSION_NAME);
    c.feat.conditional_rendering = opt(VK_EXT_CONDITIONAL_RENDERING_EXTENSION_NAME);
    const bool robustness2 = opt(VK_EXT_ROBUSTNESS_2_EXTENSION_NAME);
    const bool border = opt(VK_EXT_CUSTOM_BORDER_COLOR_EXTENSION_NAME);
    const bool clip_control = opt(VK_EXT_DEPTH_CLIP_CONTROL_EXTENSION_NAME);
    const bool clip_enable = opt(VK_EXT_DEPTH_CLIP_ENABLE_EXTENSION_NAME);
    const bool uint8 = opt(VK_KHR_INDEX_TYPE_UINT8_EXTENSION_NAME) || opt(VK_EXT_INDEX_TYPE_UINT8_EXTENSION_NAME);
    const bool fault = opt(VK_EXT_DEVICE_FAULT_EXTENSION_NAME);
    c.feat.checkpoints = getenv("HOST_NVN_CHECKPOINTS") && opt(VK_NV_DEVICE_DIAGNOSTIC_CHECKPOINTS_EXTENSION_NAME);
    opt(VK_EXT_PROVOKING_VERTEX_EXTENSION_NAME);
    opt(VK_EXT_SHADER_STENCIL_EXPORT_EXTENSION_NAME);
    opt(VK_EXT_SHADER_VIEWPORT_INDEX_LAYER_EXTENSION_NAME);
    opt(VK_NV_VIEWPORT_SWIZZLE_EXTENSION_NAME);
    opt(VK_EXT_CONSERVATIVE_RASTERIZATION_EXTENSION_NAME);
    opt(VK_EXT_LINE_RASTERIZATION_EXTENSION_NAME);
    opt(VK_EXT_DEPTH_RANGE_UNRESTRICTED_EXTENSION_NAME);
    opt(VK_EXT_SHADER_ATOMIC_FLOAT_EXTENSION_NAME);
    opt(VK_EXT_TRANSFORM_FEEDBACK_EXTENSION_NAME);
    opt(VK_EXT_FRAGMENT_SHADER_INTERLOCK_EXTENSION_NAME);
    opt(VK_KHR_FRAGMENT_SHADER_BARYCENTRIC_EXTENSION_NAME);

    // Every feature the GPU has is enabled: the game's shaders and state need many of them, and
    // which ones is only known per draw.
    VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    VkPhysicalDeviceVulkan11Features f11{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
    VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceMeshShaderFeaturesEXT fmesh{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_EXT};
    VkPhysicalDeviceExtendedDynamicState3FeaturesEXT feds3{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_3_FEATURES_EXT};
    VkPhysicalDeviceVertexInputDynamicStateFeaturesEXT fvi{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_INPUT_DYNAMIC_STATE_FEATURES_EXT};
    VkPhysicalDeviceConditionalRenderingFeaturesEXT fcr{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CONDITIONAL_RENDERING_FEATURES_EXT};
    VkPhysicalDeviceRobustness2FeaturesEXT frob{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT};
    VkPhysicalDeviceCustomBorderColorFeaturesEXT fbc{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_BORDER_COLOR_FEATURES_EXT};
    VkPhysicalDeviceDepthClipControlFeaturesEXT fdcc{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_CLIP_CONTROL_FEATURES_EXT};
    VkPhysicalDeviceDepthClipEnableFeaturesEXT fdce{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_CLIP_ENABLE_FEATURES_EXT};
    VkPhysicalDeviceIndexTypeUint8FeaturesKHR fu8{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_INDEX_TYPE_UINT8_FEATURES_KHR};
    VkPhysicalDeviceFaultFeaturesEXT ffault{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FAULT_FEATURES_EXT};
    void** tail = &f2.pNext;
    auto chain = [&](auto& s) {
        *tail = &s;
        tail = &s.pNext;
    };
    chain(f11);
    chain(f12);
    chain(f13);
    if (c.feat.mesh_shader) chain(fmesh);
    if (c.feat.eds3) chain(feds3);
    if (c.feat.vertex_input_dynamic) chain(fvi);
    if (c.feat.conditional_rendering) chain(fcr);
    if (robustness2) chain(frob);
    if (border) chain(fbc);
    if (clip_control) chain(fdcc);
    if (clip_enable) chain(fdce);
    if (uint8) chain(fu8);
    if (fault) chain(ffault);
    vkGetPhysicalDeviceFeatures2(c.phys, &f2);
    // Multiview and shading-rate features of mesh shaders need features the device may lack.
    fmesh.multiviewMeshShader = VK_FALSE;
    fmesh.primitiveFragmentShadingRateMeshShader = VK_FALSE;
    fmesh.meshShaderQueries = VK_FALSE;
    c.feat.mesh_shader = c.feat.mesh_shader && fmesh.meshShader && fmesh.taskShader;
    c.feat.index_uint8 = uint8 && fu8.indexTypeUint8;
    c.feat.device_fault = fault && ffault.deviceFault;
    ffault.deviceFaultVendorBinary = VK_FALSE;
    c.feat.clip_control = clip_control && fdcc.depthClipControl && feds3.extendedDynamicState3DepthClipNegativeOneToOne;
    c.feat.clip_enable = clip_enable && fdce.depthClipEnable && feds3.extendedDynamicState3DepthClipEnable;

    u32 nq = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(c.phys, &nq, nullptr);
    std::vector<VkQueueFamilyProperties> qf(nq);
    vkGetPhysicalDeviceQueueFamilyProperties(c.phys, &nq, qf.data());
    c.family = ~0u;
    for (u32 i = 0; i < nq; i++)
        if ((qf[i].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) == (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) {
            c.family = i;
            break;
        }
    if (c.family == ~0u) fatal("vulkan: no graphics queue");
    c.feat.sparse = (qf[c.family].queueFlags & VK_QUEUE_SPARSE_BINDING_BIT) && f2.features.sparseBinding &&
                    f2.features.sparseResidencyImage2D;
    c.feat.sparse_strict = c.props.sparseProperties.residencyNonResidentStrict;
    const float prio = 1.0f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = c.family;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.pNext = &f2;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = static_cast<u32>(want.size());
    dci.ppEnabledExtensionNames = want.data();
    check(vkCreateDevice(c.phys, &dci, nullptr, &c.dev), "vkCreateDevice");
    load_device(c.dev);
    vkq::install_cmd_proxy();   // (HOST_NVN_CMD_PROXY: recording on the driver thread too)
    vkGetDeviceQueue(c.dev, c.family, 0, &c.queue);

    if (c.feat.host_import) {
        VkPhysicalDeviceExternalMemoryHostPropertiesEXT hp{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_MEMORY_HOST_PROPERTIES_EXT};
        VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
        p2.pNext = &hp;
        vkGetPhysicalDeviceProperties2(c.phys, &p2);
        c.feat.import_alignment = hp.minImportedHostPointerAlignment;
    }

    VmaVulkanFunctions vf{};
    vf.vkGetInstanceProcAddr = vkGetInstanceProcAddr;
    vf.vkGetDeviceProcAddr = vkGetDeviceProcAddr;
    VmaAllocatorCreateInfo ai{};
    ai.flags = VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT;
    if (memory_budget) ai.flags |= VMA_ALLOCATOR_CREATE_EXT_MEMORY_BUDGET_BIT;   // (without it VMA guesses 80% of the heap)
    ai.physicalDevice = c.phys;
    ai.device = c.dev;
    ai.instance = c.instance;
    ai.vulkanApiVersion = VK_API_VERSION_1_3;
    // 64 MB blocks (VMA's default 256): a block goes back to the driver only once empty, and freed textures left
    // ~1.4 GB of 256 MB blocks unused but held, pushing new ones into system memory; anything over 32 MB gets its own.
    ai.preferredLargeHeapBlockSize = 64ull << 20;
    ai.pVulkanFunctions = &vf;
    check(vmaCreateAllocator(&ai, &c.vma), "vmaCreateAllocator");

    std::string list;
    for (const char* e : want) list += std::string(" ") + e;
    LOG("nvk: %s (Vulkan %u.%u.%u, driver %#x), queue family %u, mesh shaders %d, dynamic state 3 %d, host import %d "
        "(alignment %llu), sparse images %d (strict %d); extensions:%s",
        c.props.deviceName, VK_API_VERSION_MAJOR(c.props.apiVersion), VK_API_VERSION_MINOR(c.props.apiVersion),
        VK_API_VERSION_PATCH(c.props.apiVersion), c.props.driverVersion, c.family, c.feat.mesh_shader, c.feat.eds3,
        c.feat.host_import, static_cast<unsigned long long>(c.feat.import_alignment), c.feat.sparse, c.feat.sparse_strict, list.c_str());
}

}  // namespace

void init() {
    static std::once_flag once;
    std::call_once(once, [] {
        Device& c = ctx();
        create_instance(c);
        create_device(c);
    });
}

// ------------------------------------------------------------------ guest memory as GPU buffers

bool import_guest(u64 addr, u64 size, Imported& out, const char** why) {
    const u64 align = ctx().feat.import_alignment;
    const u64 lo = addr & ~(align - 1), hi = (addr + size + align - 1) & ~(align - 1);
    if (!import_host(hp(lo), hi - lo, out, why)) return false;
    out.addr = lo;
    return true;
}

bool import_host(void* host, u64 size, Imported& out, const char** why) {
    HitchTimer ht{HI_IMPORT};
    Device& c = ctx();
    const char* dummy;
    if (!why) why = &dummy;
    *why = "no host import";
    if (!c.feat.host_import || !size) return false;
    const u64 align = c.feat.import_alignment;
    if (reinterpret_cast<uintptr_t>(host) & (align - 1) || size & (align - 1)) {
        *why = "unaligned";
        return false;
    }
    const u64 lo = 0, hi = size;
    VkMemoryHostPointerPropertiesEXT hpp{VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT};
    if (vkGetMemoryHostPointerPropertiesEXT(c.dev, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, host, &hpp) !=
            VK_SUCCESS ||
        !hpp.memoryTypeBits) {
        *why = "vkGetMemoryHostPointerPropertiesEXT refused the memory";
        return false;
    }
    // The host-coherent type without caching first: the GPU sees CPU stores without flushes.
    u32 type = ~0u;
    for (int pass = 0; pass < 2 && type == ~0u; pass++)
        for (u32 i = 0; i < c.mem_props.memoryTypeCount; i++) {
            if (!(hpp.memoryTypeBits & (1u << i))) continue;
            const auto fl = c.mem_props.memoryTypes[i].propertyFlags;
            if (!(fl & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) continue;
            if (pass == 0 && (fl & VK_MEMORY_PROPERTY_HOST_CACHED_BIT)) continue;
            type = i;
            break;
        }
    if (type == ~0u) {
        *why = "no coherent memory type";
        return false;
    }
    VkImportMemoryHostPointerInfoEXT imp{VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT};
    imp.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
    imp.pHostPointer = host;
    VkMemoryAllocateFlagsInfo flags{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO};
    flags.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
    flags.pNext = &imp;
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.pNext = &flags;
    mai.allocationSize = hi - lo;
    mai.memoryTypeIndex = type;
    Imported im;
    im.addr = lo;
    im.size = hi - lo;
    if (const VkResult r = vkAllocateMemory(c.dev, &mai, nullptr, &im.memory); r != VK_SUCCESS) {
        *why = r == VK_ERROR_INVALID_EXTERNAL_HANDLE ? "vkAllocateMemory: invalid external handle (pages not mapped?)"
               : r == VK_ERROR_OUT_OF_DEVICE_MEMORY ? "vkAllocateMemory: out of device memory"
                                                     : "vkAllocateMemory failed";
        return false;
    }
    VkExternalMemoryBufferCreateInfo ext{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO};
    ext.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.pNext = &ext;
    bci.size = im.size;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT |
                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
                VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT |
                VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
                VK_BUFFER_USAGE_CONDITIONAL_RENDERING_BIT_EXT;
    if (vkCreateBuffer(c.dev, &bci, nullptr, &im.buffer) != VK_SUCCESS) {
        *why = "vkCreateBuffer failed";
        vkFreeMemory(c.dev, im.memory, nullptr);
        return false;
    }
    if (vkBindBufferMemory(c.dev, im.buffer, im.memory, 0) != VK_SUCCESS) {
        release_import(im);
        return false;
    }
    VkBufferDeviceAddressInfo bda{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
    bda.buffer = im.buffer;
    im.gpu = vkGetBufferDeviceAddress(c.dev, &bda);
    out = im;
    return true;
}

void release_import(Imported& im) {
    Device& c = ctx();
    if (im.buffer) vkDestroyBuffer(c.dev, im.buffer, nullptr);
    if (im.memory) vkFreeMemory(c.dev, im.memory, nullptr);
    im = {};
}

// ------------------------------------------------------------------ one-off work

namespace {
std::mutex g_oneoff_lock;
VkCommandPool g_oneoff_pool = VK_NULL_HANDLE;
}  // namespace

VkCommandBuffer begin_oneoff() {
    Device& c = ctx();
    vkq::sync();   // (a one-off is waited for: whatever the driver thread still has goes first)
    g_oneoff_lock.lock();
    if (!g_oneoff_pool) {
        VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pci.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT | VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pci.queueFamilyIndex = c.family;
        check(vkCreateCommandPool(c.dev, &pci, nullptr, &g_oneoff_pool), "vkCreateCommandPool");
    }
    VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool = g_oneoff_pool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    VkCommandBuffer cb;
    check(vkAllocateCommandBuffers(c.dev, &ai, &cb), "vkAllocateCommandBuffers");
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cb, &bi);
    return cb;
}

void end_oneoff(VkCommandBuffer cb) {
    Device& c = ctx();
    vkEndCommandBuffer(cb);
    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence fence;
    check(vkCreateFence(c.dev, &fci, nullptr, &fence), "vkCreateFence");
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cb;
    vkq::sync();
    {
        std::scoped_lock lk{c.queue_lock};
        check(vkQueueSubmit(c.queue, 1, &si, fence), "vkQueueSubmit (one-off)");
    }
    vkWaitForFences(c.dev, 1, &fence, VK_TRUE, UINT64_MAX);
    vkDestroyFence(c.dev, fence, nullptr);
    vkFreeCommandBuffers(c.dev, g_oneoff_pool, 1, &cb);
    g_oneoff_lock.unlock();
}

}  // namespace nvk
