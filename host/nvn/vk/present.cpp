#include "present.h"
#include "replay_int.h"
#include "hitch.h"
#include "dump.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <vector>

#include "../../video.h"

namespace nvk {

std::atomic<u64> g_presents{0};

namespace {

struct Slot {
    VkCommandBuffer cb = VK_NULL_HANDLE;
    VkFence done = VK_NULL_HANDLE;
    VkSemaphore acquired = VK_NULL_HANDLE;
    // Frame capture (HOST_NVN_CAPTURE_EVERY): the frame scaled into `cap`, copied to `cap_buf`, written once `done` signals.
    VkImage cap = VK_NULL_HANDLE;
    VmaAllocation cap_alloc = VK_NULL_HANDLE;
    VkBuffer cap_buf = VK_NULL_HANDLE;
    VmaAllocation cap_buf_alloc = VK_NULL_HANDLE;
    void* cap_data = nullptr;
    VkFormat cap_format = VK_FORMAT_UNDEFINED;
    u32 cap_w = 0, cap_h = 0;
    s64 cap_pending = -1;  // present number to write, -1 none
};

struct Presenter {
    std::mutex lock;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkExtent2D extent{};
    std::vector<VkImage> images;
    std::vector<VkSemaphore> rendered;  // per swapchain image: the blit is done, present may go
    VkCommandPool pool = VK_NULL_HANDLE;
    Slot slots[2];
    u32 next_slot = 0;
    VkSemaphore timeline = VK_NULL_HANDLE;
    bool srgb = false;
} g;

bool is_srgb(VkFormat f) {
    switch (f) {
    case VK_FORMAT_R8G8B8A8_SRGB:
    case VK_FORMAT_B8G8R8A8_SRGB:
    case VK_FORMAT_A8B8G8R8_SRGB_PACK32:
    case VK_FORMAT_BC1_RGBA_SRGB_BLOCK:
    case VK_FORMAT_BC2_SRGB_BLOCK:
    case VK_FORMAT_BC3_SRGB_BLOCK:
    case VK_FORMAT_BC7_SRGB_BLOCK: return true;
    default: return false;
    }
}

VkPresentModeKHR present_mode() {
    const char* v = std::getenv("HOST_PRESENT_MODE");
    if (v && std::strcmp(v, "mailbox") == 0) return VK_PRESENT_MODE_MAILBOX_KHR;
    if (v && std::strcmp(v, "immediate") == 0) return VK_PRESENT_MODE_IMMEDIATE_KHR;
    return VK_PRESENT_MODE_FIFO_KHR;
}

void destroy_swapchain() {
    Device& c = ctx();
    vkDeviceWaitIdle(c.dev);
    for (VkSemaphore s : g.rendered) vkDestroySemaphore(c.dev, s, nullptr);
    g.rendered.clear();
    g.images.clear();
    if (g.swapchain) vkDestroySwapchainKHR(c.dev, g.swapchain, nullptr);
    g.swapchain = VK_NULL_HANDLE;
}

// Returns false while the window has no area (minimised).
bool create_swapchain() {
    Device& c = ctx();
    VkSurfaceCapabilitiesKHR caps;
    check(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(c.phys, g.surface, &caps), "surface capabilities");
    if (caps.currentExtent.width == 0 || caps.currentExtent.height == 0) return false;
    u32 n = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(c.phys, g.surface, &n, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(n);
    vkGetPhysicalDeviceSurfaceFormatsKHR(c.phys, g.surface, &n, formats.data());
    // The game's bytes go to the screen as they are: an sRGB-encoded texture into an sRGB
    // swapchain, a linear one into a UNORM one (a blit converts between the two otherwise).
    const VkFormat want = g.srgb ? VK_FORMAT_B8G8R8A8_SRGB : VK_FORMAT_B8G8R8A8_UNORM;
    g.format = formats.empty() ? want : formats[0].format;
    VkColorSpaceKHR space = formats.empty() ? VK_COLOR_SPACE_SRGB_NONLINEAR_KHR : formats[0].colorSpace;
    for (const auto& f : formats)
        if (f.format == want) g.format = f.format, space = f.colorSpace;
    u32 nm = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(c.phys, g.surface, &nm, nullptr);
    std::vector<VkPresentModeKHR> modes(nm);
    vkGetPhysicalDeviceSurfacePresentModesKHR(c.phys, g.surface, &nm, modes.data());
    VkPresentModeKHR mode = present_mode();
    if (std::find(modes.begin(), modes.end(), mode) == modes.end()) mode = VK_PRESENT_MODE_FIFO_KHR;
    g.extent = caps.currentExtent;
    VkSwapchainCreateInfoKHR sci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    sci.surface = g.surface;
    sci.minImageCount = std::max(caps.minImageCount, 3u);
    if (caps.maxImageCount) sci.minImageCount = std::min(sci.minImageCount, caps.maxImageCount);
    sci.imageFormat = g.format;
    sci.imageColorSpace = space;
    sci.imageExtent = g.extent;
    sci.imageArrayLayers = 1;
    sci.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    sci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    sci.preTransform = caps.currentTransform;
    sci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    sci.presentMode = mode;
    sci.clipped = VK_TRUE;
    check(vkCreateSwapchainKHR(c.dev, &sci, nullptr, &g.swapchain), "vkCreateSwapchainKHR");
    u32 ni = 0;
    vkGetSwapchainImagesKHR(c.dev, g.swapchain, &ni, nullptr);
    g.images.resize(ni);
    vkGetSwapchainImagesKHR(c.dev, g.swapchain, &ni, g.images.data());
    VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    g.rendered.resize(ni);
    for (auto& s : g.rendered) check(vkCreateSemaphore(c.dev, &si, nullptr, &s), "vkCreateSemaphore");
    LOG("nvk: swapchain %ux%u, format %d, %u images, present mode %d", g.extent.width, g.extent.height, g.format, ni, mode);
    return true;
}

void barrier(VkCommandBuffer cb, VkImage img, VkImageLayout from, VkImageLayout to, VkAccessFlags src, VkAccessFlags dst) {
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask = src;
    b.dstAccessMask = dst;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &b);
}

// HOST_NVN_CAPTURE=<dir>: presented images as <dir>/present_<n>.bmp (hidden test runs), every
// HOST_NVN_CAPTURE_EVERY (default 300) presents from HOST_NVN_CAPTURE_FROM to HOST_NVN_CAPTURE_TO, scaled to
// HOST_NVN_CAPTURE_WIDTH (default 480) wide. Recorded in the present's own command buffer and written to disk once
// that slot's fence has signalled, so capturing every frame does not stall the renderer.
struct CaptureCfg {
    const char* dir = std::getenv("HOST_NVN_CAPTURE");
    u64 every = env_u64("HOST_NVN_CAPTURE_EVERY", 300), from = env_u64("HOST_NVN_CAPTURE_FROM", 0),
        to = env_u64("HOST_NVN_CAPTURE_TO", ~0ull), width = env_u64("HOST_NVN_CAPTURE_WIDTH", 480);
    static u64 env_u64(const char* name, u64 def) {
        const char* v = std::getenv(name);
        return v && *v ? std::strtoull(v, nullptr, 10) : def;
    }
};
const CaptureCfg& capture_cfg() {
    static CaptureCfg c;
    return c;
}

// One-frame flashes among consecutive captures (HOST_NVN_CAPTURE_EVERY=1): 8x8-pixel cells where capture n differs from both
// neighbours by over 40 (of 255, same direction) while the neighbours agree within 15. Flashes of at least HOST_NVN_FLASH_CELLS
// (default 100) cells are logged; with HOST_NVN_DUMP_ON_FLASH=<dir>, two flashes 30 or 60 presents apart arm a dump of the next
// flash on that beat and the frame before it (the exploding-mesh frames come every 30 presents, the darker frames every 60).
s64 g_flashes[8] = {};   // the latest flashes
u32 g_nflashes = 0;

void check_flash(s64 num, const u8* bgra, u32 w, u32 h) {
    constexpr u32 C = 8;
    struct Cap { s64 num = -1; std::vector<float> cells; };
    static Cap ring[3];
    static const u64 min_cells = CaptureCfg::env_u64("HOST_NVN_FLASH_CELLS", 100);
    static const char* dump_dir = std::getenv("HOST_NVN_DUMP_ON_FLASH");
    static bool armed = false;
    const u32 cw = w / C, ch = h / C;
    Cap& cur = ring[num % 3];
    cur.num = num;
    cur.cells.assign(static_cast<size_t>(cw) * ch, 0.f);
    for (u32 y = 0; y < ch * C; y++)
        for (u32 x = 0; x < cw * C; x++) {
            const u8* p = bgra + (static_cast<size_t>(y) * w + x) * 4;
            cur.cells[(y / C) * cw + x / C] += (0.114f * p[0] + 0.587f * p[1] + 0.299f * p[2]) / (C * C);
        }
    const Cap& a = ring[(num + 1) % 3];   // num - 2
    const Cap& b = ring[(num + 2) % 3];   // num - 1, the frame tested
    if (a.num != num - 2 || b.num != num - 1 || a.cells.size() != cur.cells.size() || b.cells.size() != cur.cells.size()) return;
    u32 n = 0, dark = 0;
    for (size_t i = 0; i < cur.cells.size(); i++) {
        const float dp = b.cells[i] - a.cells[i], dn = b.cells[i] - cur.cells[i];
        if (std::abs(dp) > 40 && std::abs(dn) > 40 && (dp < 0) == (dn < 0) && std::abs(a.cells[i] - cur.cells[i]) < 15) n++, dark += dp < 0;
    }
    if (n < min_cells) return;
    LOG("flash: capture %lld, %u of %zu cells (%u darker)", static_cast<long long>(b.num), n, cur.cells.size(), dark);
    dump_ring_flash(b.num);
    g_flashes[g_nflashes++ % 8] = b.num;
    // armed once two flashes come exactly 30 or 60 presents apart: the next one on that beat and the frame before it
    static s64 seen[16] = {};
    static u32 nseen = 0;
    for (u32 i = 0; i < std::min<u32>(nseen, 16) && dump_dir && !armed; i++)
        if (const s64 d = b.num - seen[i]; d == 30 || d == 60) {
            armed = true;
            dump_arm(static_cast<u64>(b.num + d - 1), static_cast<u64>(b.num + d), dump_dir);
            LOG("flash: every %lld presents, dumping captures %lld-%lld", static_cast<long long>(d), static_cast<long long>(b.num + d - 1),
                static_cast<long long>(b.num + d));
        }
    seen[nseen++ % 16] = b.num;
}

void write_capture(Slot& s) {
    if (s.cap_pending < 0) return;
    vmaInvalidateAllocation(ctx().vma, s.cap_buf_alloc, 0, VK_WHOLE_SIZE);
    check_flash(s.cap_pending, static_cast<const u8*>(s.cap_data), s.cap_w, s.cap_h);
    char path[512];
    snprintf(path, sizeof path, "%s/present_%06lld.bmp", capture_cfg().dir, static_cast<long long>(s.cap_pending));
    const s64 num = s.cap_pending;
    s.cap_pending = -1;
    FILE* f = std::fopen(path, "wb");
    if (!f) return;
    const u32 w = s.cap_w, h = s.cap_h, bytes = w * h * 4;
    u8 hdr[54]{'B', 'M'};
    auto put = [&](int at, u32 v) { std::memcpy(hdr + at, &v, 4); };
    put(2, 54 + bytes), put(10, 54), put(14, 40), put(18, w), put(22, static_cast<u32>(-static_cast<s32>(h)));
    hdr[26] = 1, hdr[28] = 32;
    put(34, bytes);
    std::fwrite(hdr, 1, 54, f);
    std::fwrite(s.cap_data, 1, bytes, f);
    std::fclose(f);
    // HOST_NVN_DUMP_RING: only the captures around a flash stay.
    static const bool ring = std::getenv("HOST_NVN_DUMP_RING") != nullptr;
    if (const s64 old = num - 10; ring && old >= 0) {
        bool keep = false;
        for (u32 i = 0; i < std::min<u32>(g_nflashes, 8); i++) keep |= std::abs(old - g_flashes[i]) <= 3;
        snprintf(path, sizeof path, "%s/present_%06lld.bmp", capture_cfg().dir, static_cast<long long>(old));
        if (!keep) std::remove(path);
    }
}

// Records the scaled copy of `image` into the slot's capture buffer (after the present's blit).
void record_capture(Slot& s, VkCommandBuffer cb, VkImage image, VkFormat format, u32 w, u32 h) {
    const CaptureCfg& cfg = capture_cfg();
    const u64 num = g_presents.load() - 1;   // the same numbering as the dump's frames
    dump_note("present (capture %llu)", static_cast<unsigned long long>(num));
    if (!cfg.dir || !image || num < cfg.from || num > cfg.to || (num - cfg.from) % (cfg.every ? cfg.every : 1)) return;
    Device& c = ctx();
    const u32 cw = static_cast<u32>(std::min<u64>(cfg.width ? cfg.width : w, w));
    const u32 ch = std::max<u32>(1, static_cast<u32>(static_cast<u64>(h) * cw / w));
    const VkFormat cf = is_srgb(format) ? VK_FORMAT_B8G8R8A8_SRGB : VK_FORMAT_B8G8R8A8_UNORM;
    if (s.cap_w != cw || s.cap_h != ch || s.cap_format != cf) {
        if (s.cap) vmaDestroyImage(c.vma, s.cap, s.cap_alloc), s.cap = VK_NULL_HANDLE;
        if (s.cap_buf) vmaDestroyBuffer(c.vma, s.cap_buf, s.cap_buf_alloc), s.cap_buf = VK_NULL_HANDLE;
        VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ici.imageType = VK_IMAGE_TYPE_2D;
        ici.format = cf;
        ici.extent = {cw, ch, 1};
        ici.mipLevels = ici.arrayLayers = 1;
        ici.samples = VK_SAMPLE_COUNT_1_BIT;
        ici.tiling = VK_IMAGE_TILING_OPTIMAL;
        ici.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        VmaAllocationCreateInfo ai{};
        ai.usage = VMA_MEMORY_USAGE_AUTO;
        if (vmaCreateImage(c.vma, &ici, &ai, &s.cap, &s.cap_alloc, nullptr) != VK_SUCCESS) { s.cap = VK_NULL_HANDLE; return; }
        VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bci.size = static_cast<u64>(cw) * ch * 4;
        bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        ai.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST;
        ai.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
        VmaAllocationInfo info;
        if (vmaCreateBuffer(c.vma, &bci, &ai, &s.cap_buf, &s.cap_buf_alloc, &info) != VK_SUCCESS) { s.cap_buf = VK_NULL_HANDLE; return; }
        s.cap_data = info.pMappedData, s.cap_w = cw, s.cap_h = ch, s.cap_format = cf;
    }
    if (!s.cap || !s.cap_buf) return;
    barrier(cb, s.cap, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT);
    VkImageBlit blit{};
    blit.srcSubresource = blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    blit.srcOffsets[1] = {static_cast<s32>(w), static_cast<s32>(h), 1};
    if (g_origin_ll.load(std::memory_order_relaxed)) std::swap(blit.srcOffsets[0].y, blit.srcOffsets[1].y);
    blit.dstOffsets[1] = {static_cast<s32>(cw), static_cast<s32>(ch), 1};
    vkCmdBlitImage(cb, image, VK_IMAGE_LAYOUT_GENERAL, s.cap, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
    barrier(cb, s.cap, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_ACCESS_TRANSFER_READ_BIT);
    VkBufferImageCopy r{};
    r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    r.imageExtent = {cw, ch, 1};
    vkCmdCopyImageToBuffer(cb, s.cap, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, s.cap_buf, 1, &r);
    s.cap_pending = static_cast<s64>(num);
}

}  // namespace

VkSemaphore timeline() {
    static std::once_flag once;
    std::call_once(once, [] {
        VkSemaphoreTypeCreateInfo ti{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
        ti.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
        VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        si.pNext = &ti;
        check(vkCreateSemaphore(ctx().dev, &si, nullptr, &g.timeline), "timeline semaphore");
    });
    return g.timeline;
}

void present_init() {
    std::scoped_lock lk{g.lock};
    if (g.surface) return;
    Device& c = ctx();
    VkWin32SurfaceCreateInfoKHR wci{VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
    wci.hinstance = GetModuleHandleW(nullptr);
    wci.hwnd = video::main_window();
    check(vkCreateWin32SurfaceKHR(c.instance, &wci, nullptr, &g.surface), "vkCreateWin32SurfaceKHR");
    VkBool32 ok = VK_FALSE;
    vkGetPhysicalDeviceSurfaceSupportKHR(c.phys, c.family, g.surface, &ok);
    if (!ok) fatal("nvk: the queue cannot present to the window");
    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = c.family;
    check(vkCreateCommandPool(c.dev, &pci, nullptr, &g.pool), "present command pool");
    for (Slot& s : g.slots) {
        VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ai.commandPool = g.pool;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1;
        check(vkAllocateCommandBuffers(c.dev, &ai, &s.cb), "present command buffer");
        VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        check(vkCreateFence(c.dev, &fci, nullptr, &s.done), "present fence");
        VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        check(vkCreateSemaphore(c.dev, &si, nullptr, &s.acquired), "present semaphore");
    }
}

void present(VkImage image, VkFormat format, u32 w, u32 h, const int* crop, u64 ready) {
    HitchTimer ht{HI_PRESENT};
    Device& c = ctx();
    g_presents++;
    std::scoped_lock lk{g.lock};
    if (!g.surface) return;
    if (is_srgb(format) != g.srgb || !g.swapchain) {
        g.srgb = is_srgb(format);
        destroy_swapchain();
        if (!create_swapchain()) return;
    }
    Slot& s = g.slots[g.next_slot];
    g.next_slot = (g.next_slot + 1) % 2;
    vkWaitForFences(c.dev, 1, &s.done, VK_TRUE, UINT64_MAX);
    write_capture(s);
    u32 index = 0;
    VkResult r = vkAcquireNextImageKHR(c.dev, g.swapchain, UINT64_MAX, s.acquired, VK_NULL_HANDLE, &index);
    if (r == VK_ERROR_OUT_OF_DATE_KHR) {
        destroy_swapchain();
        create_swapchain();
        return;
    }
    if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) vk_fail(r, "vkAcquireNextImageKHR");
    vkResetFences(c.dev, 1, &s.done);

    VkCommandBuffer cb = s.cb;
    vkResetCommandBuffer(cb, 0);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cb, &bi);
    VkImage dst = g.images[index];
    barrier(cb, dst, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT);
    VkClearColorValue black{};
    VkImageSubresourceRange all{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(cb, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &all);
    barrier(cb, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
    if (image) {
        const int sx = crop ? crop[0] : 0, sy = crop ? crop[1] : 0;
        const int sw = crop && crop[2] ? crop[2] : static_cast<int>(w), sh = crop && crop[3] ? crop[3] : static_cast<int>(h);
        // Letterbox: the largest rectangle of the source's aspect inside the window.
        const double scale = std::min(double(g.extent.width) / sw, double(g.extent.height) / sh);
        const int dw = static_cast<int>(sw * scale), dh = static_cast<int>(sh * scale);
        const int dx = (static_cast<int>(g.extent.width) - dw) / 2, dy = (static_cast<int>(g.extent.height) - dh) / 2;
        VkImageBlit blit{};
        blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        blit.srcOffsets[0] = {sx, sy, 0};
        blit.srcOffsets[1] = {sx + sw, sy + sh, 1};
        if (g_origin_ll.load(std::memory_order_relaxed)) std::swap(blit.srcOffsets[0].y, blit.srcOffsets[1].y);  // stored bottom-up
        blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        blit.dstOffsets[0] = {dx, dy, 0};
        blit.dstOffsets[1] = {dx + dw, dy + dh, 1};
        vkCmdBlitImage(cb, image, VK_IMAGE_LAYOUT_GENERAL, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit,
                       VK_FILTER_LINEAR);
    }
    barrier(cb, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_ACCESS_TRANSFER_WRITE_BIT, 0);
    record_capture(s, cb, image, format, w, h);
    vkEndCommandBuffer(cb);

    const VkSemaphore waits[2] = {s.acquired, timeline()};
    const u64 wait_values[2] = {0, ready};
    const VkPipelineStageFlags stages[2] = {VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT};
    VkTimelineSemaphoreSubmitInfo ts{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
    ts.waitSemaphoreValueCount = 2;
    ts.pWaitSemaphoreValues = wait_values;
    const u64 signal_value = 0;
    ts.signalSemaphoreValueCount = 1;
    ts.pSignalSemaphoreValues = &signal_value;
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.pNext = &ts;
    si.waitSemaphoreCount = 2;
    si.pWaitSemaphores = waits;
    si.pWaitDstStageMask = stages;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cb;
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores = &g.rendered[index];
    VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &g.rendered[index];
    pi.swapchainCount = 1;
    pi.pSwapchains = &g.swapchain;
    pi.pImageIndices = &index;
    {
        std::scoped_lock qlk{c.queue_lock};
        check(vkQueueSubmit(c.queue, 1, &si, s.done), "vkQueueSubmit (present)");
        r = vkQueuePresentKHR(c.queue, &pi);
    }
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) {
        destroy_swapchain();
        create_swapchain();
    } else if (r != VK_SUCCESS) {
        vk_fail(r, "vkQueuePresentKHR");
    }
    // Presents a second, every 5 s
    static u64 t_last = host_ns(), n = 0;
    n++;
    if (const u64 now = host_ns(); now - t_last >= 5'000'000'000ull) {
        LOG("nvk: %.1f fps", static_cast<double>(n) * 1e9 / static_cast<double>(now - t_last));
        t_last = now, n = 0;
    }
}

}  // namespace nvk
