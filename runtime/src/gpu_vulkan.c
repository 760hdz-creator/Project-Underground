/* Host renderer for NV2A Kelvin state: native Vulkan 1.3 backend (Project Underground).
 *
 * - Guest color/zeta surfaces map to host images keyed by physical address (scaled by UG_SCALE).
 * - Vertices arrive decoded (KVertex, 16 x float4) with a triangle/line/point index list; the vertex shader pulls
 *   them from a storage buffer and either interprets the guest's NV2A vertex program or does the fixed-function
 *   composite transform (runtime/shaders/nv2a.vert). The fragment shader interprets the register combiners.
 *   Both are precompiled SPIR-V (shaders_spv.h), so no shader compiler is needed at run time.
 * - Textures are decoded on the CPU (unswizzle, palettes; DXT stays BC1-3) and cached by address, format and hash.
 * - Dynamic rendering + extended dynamic state; pipelines are keyed by blend state, write mask, topology and depth.
 * - gpu_flip() blits a surface into the swapchain (4:3 letterbox) and submits the frame (2 frames in flight).
 * All gpu_* calls are serialized (pusher thread and DPC thread).
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#define VK_USE_PLATFORM_WIN32_KHR
#include "volk.h"
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include "rt.h"
#include "kelvin.h"
#include "shaders_spv.h"
#define XXH_INLINE_ALL
#include "xxhash.h"

static int g_scale = 2;   /* UG_SCALE=1..8: host render-target scale over the guest resolution */

static HWND g_hwnd;
static int gpu_ok;
static uint32_t frame_no;

/* ================================================================ window */

static void toggle_fullscreen(HWND h) {
    static WINDOWPLACEMENT saved = {sizeof saved};
    LONG st = GetWindowLongW(h, GWL_STYLE);
    if (st & WS_OVERLAPPEDWINDOW) {
        MONITORINFO mi = {sizeof mi};
        GetWindowPlacement(h, &saved);
        GetMonitorInfoW(MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST), &mi);
        SetWindowLongW(h, GWL_STYLE, (st & ~WS_OVERLAPPEDWINDOW) | WS_POPUP);
        SetWindowPos(h, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
                     mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    } else {
        SetWindowLongW(h, GWL_STYLE, (st & ~WS_POPUP) | WS_OVERLAPPEDWINDOW);
        SetWindowPlacement(h, &saved);
        SetWindowPos(h, NULL, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    }
}

static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_CLOSE: LOG("[gpu] window closed\n"); ExitProcess(0);
    case WM_KEYDOWN:
        if (w == VK_ESCAPE) { LOG("[gpu] escape\n"); ExitProcess(0); }
        if (w == VK_F11) { toggle_fullscreen(h); return 0; }
        break;
    case WM_SYSKEYDOWN:
        if (w == VK_RETURN && (l & (1 << 29))) { toggle_fullscreen(h); return 0; }
        break;
    case WM_SYSCHAR: if (w == VK_RETURN) return 0; break;
    default: break;
    }
    return DefWindowProcW(h, m, w, l);
}

/* UG_MONITOR=N picks monitor N; default: console (dev/test) runs open on the first non-primary monitor without
 * taking focus, the installed game (no console) on the primary monitor. */
typedef struct { int want, idx; RECT pick; int found; } MonPick;
static BOOL CALLBACK mon_enum(HMONITOR m, HDC dc, LPRECT rc, LPARAM lp) {
    MonPick *mp = (MonPick *)lp;
    MONITORINFO mi = {sizeof mi};
    (void)dc; (void)rc;
    GetMonitorInfoW(m, &mi);
    int hit = mp->want >= 0 ? mp->idx == mp->want : !(mi.dwFlags & MONITORINFOF_PRIMARY);
    if (hit && !mp->found) { mp->pick = mi.rcWork; mp->found = 1; }
    mp->idx++;
    return TRUE;
}

static DWORD WINAPI window_thread(LPVOID ev) {
    WNDCLASSW wc = {0};
    wc.lpfnWndProc = wndproc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = L"UndergroundRecomp";
    RegisterClassW(&wc);
    const char *me = getenv("UG_MONITOR");
    int dev_run = GetConsoleWindow() != NULL;
    MonPick mp = {me && *me ? atoi(me) : (dev_run ? -1 : -2), 0, {0}, 0};
    if (mp.want != -2) EnumDisplayMonitors(NULL, NULL, mon_enum, (LPARAM)&mp);
    POINT origin = {0, 0};
    HMONITOR hm = mp.found ? MonitorFromRect(&mp.pick, MONITOR_DEFAULTTONEAREST) : MonitorFromPoint(origin, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi = {sizeof mi};
    GetMonitorInfoW(hm, &mi);
    int mh = mi.rcMonitor.bottom - mi.rcMonitor.top;
    int wh = mh * 2 / 3, ww = wh * 4 / 3;
    RECT r = {0, 0, ww, wh};
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    int x = mi.rcWork.left + 32, y = mi.rcWork.top + 32;
    g_hwnd = CreateWindowExW(0, wc.lpszClassName, L"Tony Hawk's Underground (recomp)", WS_OVERLAPPEDWINDOW,
                             x, y, r.right - r.left, r.bottom - r.top, NULL, NULL, wc.hInstance, NULL);
    if (dev_run) ShowWindow(g_hwnd, SW_SHOWNOACTIVATE);
    else { ShowWindow(g_hwnd, SW_SHOW); SetForegroundWindow(g_hwnd); }
    if (getenv("UG_FULLSCREEN") && *getenv("UG_FULLSCREEN") == '1') toggle_fullscreen(g_hwnd);
    SetEvent((HANDLE)ev);
    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    return 0;
}

/* ================================================================ Vulkan core */

#define VKC(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { LOG("[gpu] %s failed: %d\n", #x, r_); return 0; } } while (0)
#define FRAMES 2

static VkInstance inst;
static VkSurfaceKHR surface;
static VkPhysicalDevice pdev;
static VkPhysicalDeviceProperties pprops;
static VkPhysicalDeviceMemoryProperties memprops;
static VkDevice dev;
static VkQueue queue;
static uint32_t qfam;
static VkFormat depth_fmt;

static VkSwapchainKHR swapchain;
static VkFormat sc_fmt;
static VkExtent2D sc_ext;
static uint32_t sc_count;
static VkImage sc_img[8];
static VkSemaphore sc_done[8];   /* render finished, per swapchain image */

static VkDescriptorSetLayout dsl;
static VkPipelineLayout plal;
static VkShaderModule vs_mod, fs_mod;

typedef struct { VkBuffer buf; VkDeviceMemory mem; uint8_t *map; VkDeviceSize size, pos; } Ring;

typedef struct {
    VkCommandPool pool;
    VkCommandBuffer cmd, up;
    VkFence fence;
    VkSemaphore acquired;
    VkDescriptorPool dpool;
    Ring vtx, idx, ubo, stage;
    VkImage gimg[512]; VkImageView gview[512]; VkDeviceMemory gmem[512]; int ngarbage;
    int up_used;
} Frame;
static Frame frames[FRAMES];
static int fi;   /* current frame slot */

static uint32_t mem_type(uint32_t bits, VkMemoryPropertyFlags want) {
    for (uint32_t i = 0; i < memprops.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (memprops.memoryTypes[i].propertyFlags & want) == want) return i;
    return UINT32_MAX;
}

static int ring_create(Ring *r, VkDeviceSize size, VkBufferUsageFlags usage) {
    VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, 0, 0, size, usage, VK_SHARING_MODE_EXCLUSIVE};
    VKC(vkCreateBuffer(dev, &bi, 0, &r->buf));
    VkMemoryRequirements mr;
    vkGetBufferMemoryRequirements(dev, r->buf, &mr);
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, 0, mr.size,
                               mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)};
    VKC(vkAllocateMemory(dev, &ai, 0, &r->mem));
    VKC(vkBindBufferMemory(dev, r->buf, r->mem, 0));
    VKC(vkMapMemory(dev, r->mem, 0, size, 0, (void **)&r->map));
    r->size = size;
    r->pos = 0;
    return 1;
}

/* bump allocation in a per-frame ring; NULL when full */
static void *ring_alloc(Ring *r, VkDeviceSize bytes, VkDeviceSize align, VkDeviceSize *off) {
    VkDeviceSize p = (r->pos + align - 1) & ~(align - 1);
    if (p + bytes > r->size) return NULL;
    r->pos = p + bytes;
    *off = p;
    return r->map + p;
}

/* ---- device-local image memory: 64 MB blocks, first-fit free lists (stays under maxMemoryAllocationCount) ---- */
typedef struct { VkDeviceSize off, size; } Span;
typedef struct { VkDeviceMemory mem; uint32_t type; VkDeviceSize size; Span *free; int nfree, capfree; } Block;
static Block blocks[64];
static int nblocks;
#define BLOCK_SIZE (64ull << 20)

typedef struct { VkDeviceMemory mem; VkDeviceSize off, size; int block; } Alloc;

static int img_alloc(VkMemoryRequirements mr, Alloc *out) {
    uint32_t type = mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    for (int pass = 0; pass < 2; pass++) {
        for (int b = 0; b < nblocks; b++) {
            Block *bl = &blocks[b];
            if (bl->type != type) continue;
            for (int i = 0; i < bl->nfree; i++) {
                Span *s = &bl->free[i];
                VkDeviceSize a = (s->off + mr.alignment - 1) & ~(mr.alignment - 1);
                if (a + mr.size > s->off + s->size) continue;
                VkDeviceSize end = s->off + s->size;
                out->mem = bl->mem; out->off = a; out->size = mr.size; out->block = b;
                /* split: [s->off, a) stays free (alignment gap), [a + size, end) stays free */
                VkDeviceSize head = a - s->off;
                if (head) { s->size = head; if (end > a + mr.size) { if (bl->nfree == bl->capfree) { bl->capfree *= 2; bl->free = realloc(bl->free, bl->capfree * sizeof(Span)); } bl->free[bl->nfree++] = (Span){a + mr.size, end - a - mr.size}; } }
                else if (end > a + mr.size) { s->off = a + mr.size; s->size = end - a - mr.size; }
                else { bl->free[i] = bl->free[--bl->nfree]; }
                return 1;
            }
        }
        if (pass == 0) {   /* new block */
            if (nblocks == 64) break;
            VkDeviceSize sz = mr.size > BLOCK_SIZE ? mr.size : BLOCK_SIZE;
            VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, 0, sz, type};
            Block *bl = &blocks[nblocks];
            if (vkAllocateMemory(dev, &ai, 0, &bl->mem) != VK_SUCCESS) break;
            bl->type = type; bl->size = sz; bl->capfree = 64; bl->free = malloc(64 * sizeof(Span));
            bl->free[0] = (Span){0, sz}; bl->nfree = 1;
            nblocks++;
        }
    }
    LOG("[gpu] out of image memory\n");
    return 0;
}

static void img_free(Alloc a) {
    Block *bl = &blocks[a.block];
    if (bl->nfree == bl->capfree) { bl->capfree *= 2; bl->free = realloc(bl->free, bl->capfree * sizeof(Span)); }
    bl->free[bl->nfree++] = (Span){a.off, a.size};
    /* merge neighbours (small lists: quadratic is fine) */
    for (int i = 0; i < bl->nfree; i++)
        for (int j = 0; j < bl->nfree; j++)
            if (i != j && bl->free[i].off + bl->free[i].size == bl->free[j].off) {
                bl->free[i].size += bl->free[j].size;
                bl->free[j] = bl->free[--bl->nfree];
                j = -1;
                if (i >= bl->nfree) break;
            }
}

typedef struct { VkImage img; VkImageView view; Alloc mem; VkImageLayout layout; } Img;

static int img_create(Img *im, uint32_t w, uint32_t h, uint32_t levels, VkFormat fmt, VkImageUsageFlags usage,
                      VkImageAspectFlags aspect) {
    VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_2D; ci.format = fmt; ci.extent = (VkExtent3D){w, h, 1};
    ci.mipLevels = levels; ci.arrayLayers = 1; ci.samples = VK_SAMPLE_COUNT_1_BIT; ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = usage; ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VKC(vkCreateImage(dev, &ci, 0, &im->img));
    VkMemoryRequirements mr;
    vkGetImageMemoryRequirements(dev, im->img, &mr);
    if (!img_alloc(mr, &im->mem)) { vkDestroyImage(dev, im->img, 0); im->img = 0; return 0; }
    VKC(vkBindImageMemory(dev, im->img, im->mem.mem, im->mem.off));
    VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, 0, 0, im->img, VK_IMAGE_VIEW_TYPE_2D, fmt,
                                {0}, {aspect, 0, levels, 0, 1}};
    VKC(vkCreateImageView(dev, &vi, 0, &im->view));
    im->layout = VK_IMAGE_LAYOUT_UNDEFINED;
    return 1;
}

/* deferred destruction: the frame slot that used it may still be in flight */
static Alloc gallocs[FRAMES][512];
static void img_destroy_later(Img *im) {
    Frame *f = &frames[fi];
    if (!im->img) return;
    if (f->ngarbage == 512) {   /* flush: wait for the GPU (rare) */
        vkDeviceWaitIdle(dev);
        for (int i = 0; i < f->ngarbage; i++) {
            vkDestroyImageView(dev, f->gview[i], 0); vkDestroyImage(dev, f->gimg[i], 0); img_free(gallocs[fi][i]);
        }
        f->ngarbage = 0;
    }
    f->gimg[f->ngarbage] = im->img; f->gview[f->ngarbage] = im->view; gallocs[fi][f->ngarbage] = im->mem;
    f->ngarbage++;
    memset(im, 0, sizeof *im);
}

static void barrier(VkCommandBuffer cb, Img *im, VkImageAspectFlags aspect, VkImageLayout to) {
    if (im->layout == to) return;
    VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    b.oldLayout = im->layout; b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = im->img;
    b.subresourceRange = (VkImageSubresourceRange){aspect, 0, VK_REMAINING_MIP_LEVELS, 0, 1};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, 0, 0, 0, 1, &b);
    im->layout = to;
}

static void raw_barrier(VkCommandBuffer cb, VkImage img, VkImageLayout from, VkImageLayout to) {
    VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    b.oldLayout = from; b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img;
    b.subresourceRange = (VkImageSubresourceRange){VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, 0, 0, 0, 1, &b);
}

static int swapchain_create(void) {
    VkSurfaceCapabilitiesKHR caps;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(pdev, surface, &caps);
    if (caps.currentExtent.width == 0 || caps.currentExtent.height == 0) return 0;   /* minimized */
    uint32_t n = 16;
    VkSurfaceFormatKHR fmts[16];
    vkGetPhysicalDeviceSurfaceFormatsKHR(pdev, surface, &n, fmts);
    VkSurfaceFormatKHR pick = fmts[0];
    for (uint32_t i = 0; i < n; i++)
        if (fmts[i].format == VK_FORMAT_B8G8R8A8_UNORM || fmts[i].format == VK_FORMAT_R8G8B8A8_UNORM) { pick = fmts[i]; break; }
    sc_fmt = pick.format;
    sc_ext = caps.currentExtent;
    uint32_t want = caps.minImageCount + 1;
    if (caps.maxImageCount && want > caps.maxImageCount) want = caps.maxImageCount;
    VkPresentModeKHR modes[8];
    uint32_t nm = 8;
    vkGetPhysicalDeviceSurfacePresentModesKHR(pdev, surface, &nm, modes);
    VkPresentModeKHR pm = VK_PRESENT_MODE_FIFO_KHR;   /* the game paces itself on vblank; FIFO is always there */
    const char *vs = getenv("UG_VSYNC");
    if (vs && *vs == '0')
        for (uint32_t i = 0; i < nm; i++) if (modes[i] == VK_PRESENT_MODE_IMMEDIATE_KHR || modes[i] == VK_PRESENT_MODE_MAILBOX_KHR) pm = modes[i];
    VkSwapchainCreateInfoKHR ci = {VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    ci.surface = surface; ci.minImageCount = want; ci.imageFormat = sc_fmt; ci.imageColorSpace = pick.colorSpace;
    ci.imageExtent = sc_ext; ci.imageArrayLayers = 1;
    ci.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE; ci.preTransform = caps.currentTransform;
    ci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR; ci.presentMode = pm; ci.clipped = VK_TRUE;
    ci.oldSwapchain = swapchain;
    VkSwapchainKHR sc;
    VKC(vkCreateSwapchainKHR(dev, &ci, 0, &sc));
    if (swapchain) vkDestroySwapchainKHR(dev, swapchain, 0);
    swapchain = sc;
    sc_count = 8;
    vkGetSwapchainImagesKHR(dev, swapchain, &sc_count, sc_img);
    for (uint32_t i = 0; i < sc_count; i++) {
        if (!sc_done[i]) {
            VkSemaphoreCreateInfo si = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
            vkCreateSemaphore(dev, &si, 0, &sc_done[i]);
        }
    }
    LOG("[gpu] swapchain %ux%u, %u images, format %d, present mode %d\n", sc_ext.width, sc_ext.height, sc_count, sc_fmt, pm);
    return 1;
}

/* ================================================================ surfaces */

typedef struct {
    uint32_t addr; int w, h, hw, hh;
    Img color;
    uint32_t last_frame;
} Surface;
typedef struct { uint32_t addr; int w, h; Img depth; } ZSurface;
static Surface rts[16]; static int n_rts;
static ZSurface zss[8]; static int n_zss;
static Surface *cur_rt;
static ZSurface *cur_zs;
static int rendering;                 /* inside vkCmdBeginRendering */
static Surface *r_rt; static ZSurface *r_zs;   /* what the active rendering pass targets */

static VkCommandBuffer CMD(void) { return frames[fi].cmd; }

static void end_rendering(void) {
    if (rendering) { vkCmdEndRendering(CMD()); rendering = 0; }
}

static void begin_rendering(void) {
    if (rendering && r_rt == cur_rt && r_zs == cur_zs) return;
    end_rendering();
    barrier(CMD(), &cur_rt->color, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    if (cur_zs) barrier(CMD(), &cur_zs->depth, VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT,
                        VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
    VkRenderingAttachmentInfo ca = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    ca.imageView = cur_rt->color.view; ca.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    ca.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD; ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingAttachmentInfo za = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    if (cur_zs) {
        za.imageView = cur_zs->depth.view; za.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        za.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD; za.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    }
    VkRenderingInfo ri = {VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea = (VkRect2D){{0, 0}, {(uint32_t)cur_rt->hw, (uint32_t)cur_rt->hh}};
    ri.layerCount = 1; ri.colorAttachmentCount = 1; ri.pColorAttachments = &ca;
    if (cur_zs) { ri.pDepthAttachment = &za; ri.pStencilAttachment = &za; }
    vkCmdBeginRendering(CMD(), &ri);
    rendering = 1; r_rt = cur_rt; r_zs = cur_zs;
    VkViewport vp = {0, 0, (float)cur_rt->hw, (float)cur_rt->hh, 0, 1};
    vkCmdSetViewport(CMD(), 0, 1, &vp);
    VkRect2D sc = {{0, 0}, {(uint32_t)cur_rt->hw, (uint32_t)cur_rt->hh}};
    vkCmdSetScissor(CMD(), 0, 1, &sc);
}

static Surface *find_rt(uint32_t addr) {
    for (int i = 0; i < n_rts; i++) if (rts[i].addr == addr && rts[i].color.img) return &rts[i];
    return NULL;
}

static void clear_image(Img *im, VkImageAspectFlags aspect) {
    barrier(CMD(), im, aspect, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkImageSubresourceRange rg = {aspect, 0, 1, 0, 1};
    if (aspect & VK_IMAGE_ASPECT_COLOR_BIT) {
        VkClearColorValue cc = {{0, 0, 0, 1}};
        vkCmdClearColorImage(CMD(), im->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &cc, 1, &rg);
    } else {
        VkClearDepthStencilValue dz = {1.0f, 0};
        vkCmdClearDepthStencilImage(CMD(), im->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &dz, 1, &rg);
    }
}

static Surface *get_rt(uint32_t addr, int w, int h) {
    Surface *s = find_rt(addr);
    if (s && s->w == w && s->h == h) return s;
    end_rendering();
    if (!s) {
        if (n_rts == 16) {
            int o = 0;
            for (int i = 1; i < n_rts; i++) if (rts[i].last_frame < rts[o].last_frame) o = i;
            s = &rts[o];
        } else s = &rts[n_rts++];
    }
    img_destroy_later(&s->color);
    s->addr = addr; s->w = w; s->h = h; s->hw = w * g_scale; s->hh = h * g_scale;
    if (!img_create(&s->color, s->hw, s->hh, 1, VK_FORMAT_R8G8B8A8_UNORM,
                    VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                    VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_ASPECT_COLOR_BIT)) return NULL;
    clear_image(&s->color, VK_IMAGE_ASPECT_COLOR_BIT);
    LOG("[gpu] render target %08X %dx%d\n", addr, w, h);
    return s;
}

static ZSurface *get_zs(uint32_t addr, int w, int h) {
    for (int i = 0; i < n_zss; i++) if (zss[i].addr == addr && zss[i].w == w && zss[i].h == h && zss[i].depth.img) return &zss[i];
    end_rendering();
    ZSurface *z = NULL;
    for (int i = 0; i < n_zss; i++) if (zss[i].addr == addr) z = &zss[i];
    if (!z) z = n_zss < 8 ? &zss[n_zss++] : &zss[0];
    img_destroy_later(&z->depth);
    z->addr = addr; z->w = w; z->h = h;
    if (!img_create(&z->depth, w * g_scale, h * g_scale, 1, depth_fmt,
                    VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                    VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) return NULL;
    clear_image(&z->depth, VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT);
    LOG("[gpu] depth surface %08X %dx%d\n", addr, w, h);
    return z;
}

static void surface_size(int *w, int *h) {
    uint32_t ch = KREG(K_CLIP_H), cv = KREG(K_CLIP_V);
    *w = (int)((ch & 0xFFFF) + (ch >> 16));
    *h = (int)((cv & 0xFFFF) + (cv >> 16));
    if (*w <= 0 || *w > 4096) *w = 640;
    if (*h <= 0 || *h > 4096) *h = 480;
}

/* select the guest's current color/zeta surfaces (does not begin rendering) */
static int select_targets(void) {
    int w, h;
    surface_size(&w, &h);
    uint32_t caddr = (nv2a_dma_base(KREG(K_DMA_COLOR)) + KREG(K_COLOR_OFFSET)) & 0x03FFFFFF;
    cur_rt = get_rt(caddr, w, h);
    if (!cur_rt) return 0;
    cur_rt->last_frame = frame_no;
    uint32_t zfmt = (KREG(K_SURF_FORMAT) >> 4) & 0xF;
    cur_zs = NULL;
    if (zfmt) {
        uint32_t zaddr = (nv2a_dma_base(KREG(K_DMA_ZETA)) + KREG(K_ZETA_OFFSET)) & 0x03FFFFFF;
        cur_zs = get_zs(zaddr, w, h);
    }
    return 1;
}

/* ================================================================ textures */

static uint64_t hash_mem(uint32_t a, uint32_t n, uint64_t h) {
    a &= 0x03FFFFFF;
    if (a + n > RAM_SIZE) n = a < RAM_SIZE ? RAM_SIZE - a : 0;
    return XXH3_64bits_withSeed(g_mem + a, n, h);
}

static void swizzle_tables(int w, int h, uint32_t *xs, uint32_t *ys) {
    for (int x = 0; x < w; x++) {
        uint32_t o = 0, bit = 1;
        for (int i = 0; (1 << i) < w || (1 << i) < h; i++) {
            if ((1 << i) < w) { if (x & (1 << i)) o |= bit; bit <<= 1; }
            if ((1 << i) < h) bit <<= 1;
        }
        xs[x] = o;
    }
    for (int y = 0; y < h; y++) {
        uint32_t o = 0, bit = 1;
        for (int i = 0; (1 << i) < w || (1 << i) < h; i++) {
            if ((1 << i) < w) bit <<= 1;
            if ((1 << i) < h) { if (y & (1 << i)) o |= bit; bit <<= 1; }
        }
        ys[y] = o;
    }
}

enum { TF_NONE, TF_Y8, TF_AY8, TF_A8, TF_A8Y8, TF_1555, TF_X555, TF_4444, TF_565, TF_8888, TF_X888, TF_I8,
       TF_ABGR, TF_BGRA, TF_RGBA, TF_DXT1, TF_DXT3, TF_DXT5 };
typedef struct { int kind, bpp, swizzled; } TexFmt;

static TexFmt tex_format(uint32_t c) {
    TexFmt f = {TF_NONE, 0, 1};
    switch (c) {
    case 0x00: f.kind = TF_Y8; f.bpp = 1; break;
    case 0x01: f.kind = TF_AY8; f.bpp = 1; break;
    case 0x02: f.kind = TF_1555; f.bpp = 2; break;
    case 0x03: f.kind = TF_X555; f.bpp = 2; break;
    case 0x04: f.kind = TF_4444; f.bpp = 2; break;
    case 0x05: f.kind = TF_565; f.bpp = 2; break;
    case 0x06: f.kind = TF_8888; f.bpp = 4; break;
    case 0x07: f.kind = TF_X888; f.bpp = 4; break;
    case 0x0B: f.kind = TF_I8; f.bpp = 1; break;
    case 0x0C: f.kind = TF_DXT1; f.swizzled = 0; break;
    case 0x0E: f.kind = TF_DXT3; f.swizzled = 0; break;
    case 0x0F: f.kind = TF_DXT5; f.swizzled = 0; break;
    case 0x10: f.kind = TF_1555; f.bpp = 2; f.swizzled = 0; break;
    case 0x11: f.kind = TF_565; f.bpp = 2; f.swizzled = 0; break;
    case 0x12: f.kind = TF_8888; f.bpp = 4; f.swizzled = 0; break;
    case 0x13: f.kind = TF_Y8; f.bpp = 1; f.swizzled = 0; break;
    case 0x19: f.kind = TF_A8; f.bpp = 1; break;
    case 0x1A: f.kind = TF_A8Y8; f.bpp = 2; break;
    case 0x1B: f.kind = TF_AY8; f.bpp = 1; f.swizzled = 0; break;
    case 0x1C: f.kind = TF_X555; f.bpp = 2; f.swizzled = 0; break;
    case 0x1D: f.kind = TF_4444; f.bpp = 2; f.swizzled = 0; break;
    case 0x1E: f.kind = TF_X888; f.bpp = 4; f.swizzled = 0; break;
    case 0x1F: f.kind = TF_A8; f.bpp = 1; f.swizzled = 0; break;
    case 0x20: f.kind = TF_A8Y8; f.bpp = 2; f.swizzled = 0; break;
    case 0x3A: f.kind = TF_ABGR; f.bpp = 4; break;
    case 0x3B: f.kind = TF_BGRA; f.bpp = 4; break;
    case 0x3C: f.kind = TF_RGBA; f.bpp = 4; break;
    case 0x3F: f.kind = TF_ABGR; f.bpp = 4; f.swizzled = 0; break;
    case 0x40: f.kind = TF_BGRA; f.bpp = 4; f.swizzled = 0; break;
    case 0x41: f.kind = TF_RGBA; f.bpp = 4; f.swizzled = 0; break;
    default: break;
    }
    return f;
}

static uint32_t c5(uint32_t v) { return (v << 3) | (v >> 2); }
static uint32_t c6(uint32_t v) { return (v << 2) | (v >> 4); }

/* one texel -> RGBA8 (R in the low byte) */
static uint32_t texel(int kind, const uint8_t *p, const uint32_t *pal) {
    uint32_t v, r, g, b, a;
    switch (kind) {
    case TF_Y8: v = p[0]; return v | v << 8 | v << 16 | 0xFF000000u;
    case TF_AY8: v = p[0]; return v | v << 8 | v << 16 | v << 24;
    case TF_A8: return (uint32_t)p[0] << 24;
    case TF_A8Y8: v = p[0]; return v | v << 8 | v << 16 | (uint32_t)p[1] << 24;
    case TF_1555: case TF_X555:
        v = p[0] | p[1] << 8;
        r = c5((v >> 10) & 31); g = c5((v >> 5) & 31); b = c5(v & 31);
        a = kind == TF_X555 ? 255 : (v & 0x8000) ? 255 : 0;
        return r | g << 8 | b << 16 | a << 24;
    case TF_4444:
        v = p[0] | p[1] << 8;
        r = ((v >> 8) & 15) * 17; g = ((v >> 4) & 15) * 17; b = (v & 15) * 17; a = (v >> 12) * 17;
        return r | g << 8 | b << 16 | a << 24;
    case TF_565:
        v = p[0] | p[1] << 8;
        return c5(v >> 11) | c6((v >> 5) & 63) << 8 | c5(v & 31) << 16 | 0xFF000000u;
    case TF_8888: return p[2] | p[1] << 8 | p[0] << 16 | (uint32_t)p[3] << 24;
    case TF_X888: return p[2] | p[1] << 8 | p[0] << 16 | 0xFF000000u;
    case TF_ABGR: return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
    case TF_BGRA: return p[1] | p[2] << 8 | p[3] << 16 | (uint32_t)p[0] << 24;
    case TF_RGBA: return p[3] | p[2] << 8 | p[1] << 16 | (uint32_t)p[0] << 24;
    case TF_I8: return pal[p[0]];
    default: return 0;
    }
}

typedef struct {
    uint32_t addr, fmt, pitch, pal_addr, pal_len;
    uint64_t hash;
    uint32_t last_frame;
    int w, h, levels;
    Img im;
} TexEntry;
#define TEX_CACHE 4096
static TexEntry tcache[TEX_CACHE];
#define ADDR_SLOTS 4096
static TexEntry *addr_slot[ADDR_SLOTS];   /* latest entry per texture address (in-place re-upload) */
static Img white_tex;
static uint32_t st_draws, st_verts, st_newtex;

static uint32_t tex_bytes(TexFmt f, int w, int h, int levels, uint32_t pitch) {
    if (!f.swizzled && f.kind < TF_DXT1) return (pitch ? pitch : (uint32_t)w * f.bpp) * h;
    uint32_t n = 0;
    for (int l = 0; l < levels; l++) {
        int lw = w >> l, lh = h >> l;
        if (lw < 1) lw = 1;
        if (lh < 1) lh = 1;
        if (f.kind >= TF_DXT1) n += ((lw + 3) / 4) * ((lh + 3) / 4) * (f.kind == TF_DXT1 ? 8 : 16);
        else n += (uint32_t)lw * lh * f.bpp;
    }
    return n;
}

/* decode into the staging ring and record the upload into the frame's upload command buffer */
static int create_texture(TexEntry *e, TexFmt f, int levels, const uint32_t *pal) {
    int w = e->w, h = e->h, dxt = f.kind >= TF_DXT1;
    if (dxt && ((w & 3) || (h & 3))) return 0;
    if (!f.swizzled && !dxt) levels = 1;
    int nl = 0;
    VkBufferImageCopy regions[16];
    Frame *fr = &frames[fi];
    uint32_t src = e->addr;
    for (int l = 0; l < levels && l < 16; l++) {
        int lw = w >> l, lh = h >> l;
        if (lw < 1) lw = 1;
        if (lh < 1) lh = 1;
        VkDeviceSize off;
        if (dxt) {
            if (l > 0 && (lw < 4 || lh < 4)) break;
            uint32_t bsz = f.kind == TF_DXT1 ? 8 : 16, bw = (lw + 3) / 4, bh = (lh + 3) / 4, sz = bw * bh * bsz;
            uint8_t *p = ring_alloc(&fr->stage, sz, 16, &off);
            if (!p) return 0;
            memcpy(p, g_mem + (src & 0x03FFFFFF), sz);
            src += sz;
        } else {
            uint32_t *out = ring_alloc(&fr->stage, (VkDeviceSize)lw * lh * 4, 16, &off);
            if (!out) return 0;
            const uint8_t *base = g_mem + (src & 0x03FFFFFF);
            if (f.swizzled) {
                uint32_t *xs = malloc(lw * 4), *ys = malloc(lh * 4);
                swizzle_tables(lw, lh, xs, ys);
                for (int y = 0; y < lh; y++)
                    for (int x = 0; x < lw; x++) out[y * lw + x] = texel(f.kind, base + (xs[x] | ys[y]) * f.bpp, pal);
                free(xs); free(ys);
                src += (uint32_t)lw * lh * f.bpp;
            } else {
                uint32_t pitch = e->pitch ? e->pitch : (uint32_t)lw * f.bpp;
                for (int y = 0; y < lh; y++)
                    for (int x = 0; x < lw; x++) out[y * lw + x] = texel(f.kind, base + y * pitch + x * f.bpp, pal);
            }
        }
        regions[nl] = (VkBufferImageCopy){off, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, (uint32_t)l, 0, 1}, {0, 0, 0},
                                          {(uint32_t)lw, (uint32_t)lh, 1}};
        nl++;
    }
    VkFormat vf = f.kind == TF_DXT1 ? VK_FORMAT_BC1_RGBA_UNORM_BLOCK : f.kind == TF_DXT3 ? VK_FORMAT_BC2_UNORM_BLOCK :
                  f.kind == TF_DXT5 ? VK_FORMAT_BC3_UNORM_BLOCK : VK_FORMAT_R8G8B8A8_UNORM;
    if (!e->im.img && !img_create(&e->im, w, h, nl, vf, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                  VK_IMAGE_ASPECT_COLOR_BIT)) return 0;   /* an existing image is re-uploaded in place */
    barrier(fr->up, &e->im, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    vkCmdCopyBufferToImage(fr->up, fr->stage.buf, e->im.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, nl, regions);
    barrier(fr->up, &e->im, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    fr->up_used = 1;
    st_newtex++;
    return 1;
}

/* stage i -> image view (or 0); *rect = linear texture (unnormalized coordinates) */
static VkImageView get_texture(int i, int *rect, int *tw, int *th) {
    uint32_t fmtw = KREG(K_TEX_FORMAT(i));
    uint32_t dma = KREG((fmtw & 3) == 2 ? K_DMA_B : K_DMA_A);
    uint32_t addr = (nv2a_dma_base(dma) + KREG(K_TEX_OFFSET(i))) & 0x03FFFFFF;
    uint32_t color = (fmtw >> 8) & 0xFF;
    TexFmt f = tex_format(color);
    *rect = 0;
    if (fmtw & 4) {
        static int warned;
        if (!warned++) LOG("[gpu] cube map textures not supported yet (format %08X)\n", fmtw);
        return 0;
    }
    if (f.kind == TF_NONE) {
        static uint32_t warned[8]; static int nw;
        int seen = 0;
        for (int k = 0; k < nw; k++) seen |= warned[k] == color;
        if (!seen && nw < 8) { warned[nw++] = color; LOG("[gpu] unsupported texture format %02X\n", color); }
        return 0;
    }
    int w, h, levels;
    uint32_t pitch = 0;
    if (f.swizzled || f.kind >= TF_DXT1) {
        w = 1 << ((fmtw >> 20) & 15); h = 1 << ((fmtw >> 24) & 15);
        levels = (fmtw >> 16) & 15;
        if (levels < 1) levels = 1;
    } else {
        uint32_t r = KREG(K_TEX_RECT(i));
        w = r >> 16; h = r & 0xFFFF; levels = 1;
        pitch = KREG(K_TEX_CONTROL1(i)) >> 16;
        *rect = 1;
    }
    if (w <= 0 || h <= 0 || w > 4096 || h > 4096) return 0;
    *tw = w; *th = h;
    /* render-to-texture: sample the host surface (not while it is the bound target) */
    Surface *s = find_rt(addr);
    if (s) {
        if (s == cur_rt) return 0;
        end_rendering();
        barrier(CMD(), &s->color, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        return s->color.view;
    }
    uint32_t pal_addr = 0, pal_len = 0, palette[256];
    if (f.kind == TF_I8) {
        uint32_t pw = KREG(K_TEX_PALETTE(i));
        pal_len = 256u >> ((pw >> 2) & 3);
        pal_addr = (nv2a_dma_base(KREG((pw & 1) ? K_DMA_B : K_DMA_A)) + (pw & ~0x3Fu)) & 0x03FFFFFF;
    }
    uint32_t nbytes = tex_bytes(f, w, h, levels, pitch);
    uint64_t hsh = hash_mem(addr, nbytes, 0xCBF29CE484222325ull);
    if (pal_len) hsh = hash_mem(pal_addr, pal_len * 4, hsh);
    uint32_t slot = (uint32_t)((hsh ^ addr * 0x9E3779B1u ^ fmtw) % TEX_CACHE);
    for (int probe = 0; probe < 8; probe++) {
        TexEntry *e = &tcache[(slot + probe) % TEX_CACHE];
        if (e->im.img && e->hash == hsh && e->addr == addr && e->fmt == fmtw && e->pitch == pitch && e->w == w &&
            e->h == h && e->pal_addr == pal_addr && e->pal_len == pal_len) {
            e->last_frame = frame_no;
            return e->im.view;
        }
    }
    /* same texture slot rewritten (movie frames, render-to-memory): re-upload into the previous image, unless that
     * image was already sampled this frame (the upload command buffer runs before this frame's draws) */
    uint32_t aslot = ((addr * 0x9E3779B1u) ^ (fmtw * 0x85EBCA77u)) >> 20 & (ADDR_SLOTS - 1);
    TexEntry *old_e = addr_slot[aslot];
    if (old_e && old_e->im.img && old_e->addr == addr && old_e->fmt == fmtw && old_e->pitch == pitch && old_e->w == w &&
        old_e->h == h && old_e->pal_addr == pal_addr && old_e->pal_len == pal_len && old_e->last_frame != frame_no &&
        old_e->levels == levels) {
        if (pal_len)
            for (uint32_t k = 0; k < 256; k++) {
                uint32_t c = k < pal_len ? RD32(pal_addr + k * 4) : 0;
                palette[k] = ((c >> 16) & 0xFF) | (c & 0xFF00) | ((c & 0xFF) << 16) | (c & 0xFF000000u);
            }
        old_e->hash = hsh; old_e->last_frame = frame_no;
        if (!create_texture(old_e, f, levels, palette)) return 0;
        return old_e->im.view;
    }
    TexEntry *v = &tcache[slot];
    for (int probe = 0; probe < 8; probe++) {
        TexEntry *e = &tcache[(slot + probe) % TEX_CACHE];
        if (!e->im.img) { v = e; break; }
        if (e->last_frame < v->last_frame) v = e;
    }
    img_destroy_later(&v->im);
    if (pal_len)
        for (uint32_t k = 0; k < 256; k++) {
            uint32_t c = k < pal_len ? RD32(pal_addr + k * 4) : 0;   /* A8R8G8B8 */
            palette[k] = ((c >> 16) & 0xFF) | (c & 0xFF00) | ((c & 0xFF) << 16) | (c & 0xFF000000u);
        }
    v->addr = addr; v->fmt = fmtw; v->pitch = pitch; v->w = w; v->h = h; v->pal_addr = pal_addr; v->pal_len = pal_len;
    v->hash = hsh; v->last_frame = frame_no; v->levels = levels;
    if (!create_texture(v, f, levels, palette)) return 0;
    addr_slot[aslot] = v;
    return v->im.view;
}

/* ---- samplers ---- */
typedef struct { uint32_t address, filter, border; VkSampler s; } SamplerEntry;
static SamplerEntry samplers[128]; static int n_samplers;
static VkSampler blit_sampler;

static VkSamplerAddressMode addr_mode(uint32_t m) {
    switch (m) {
    case 1: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
    case 2: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
    case 4: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    default: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    }
}

static VkSampler get_sampler(int i, int rect) {
    uint32_t a = KREG(K_TEX_ADDRESS(i)), f = (KREG(K_TEX_FILTER(i)) & 0x0FFF0000u) | (uint32_t)rect, b = KREG(K_TEX_BORDER(i));
    for (int k = 0; k < n_samplers; k++)
        if (samplers[k].address == a && samplers[k].filter == f && samplers[k].border == b) return samplers[k].s;
    uint32_t mn = (f >> 16) & 0xFF, mg = (f >> 24) & 0xF;
    int min_lin = mn == 2 || mn == 4 || mn == 6 || mn == 7, mag_lin = mg != 1, mip_lin = mn == 5 || mn == 6, mips = mn >= 3 && mn <= 6;
    VkSamplerCreateInfo ci = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    ci.magFilter = mag_lin ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    ci.minFilter = min_lin ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    ci.mipmapMode = mip_lin ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
    ci.addressModeU = addr_mode(a & 0xF); ci.addressModeV = addr_mode((a >> 8) & 0xF); ci.addressModeW = addr_mode((a >> 16) & 0xF);
    ci.maxLod = mips && !rect ? 16.0f : 0.0f;
    ci.borderColor = (b >> 24) < 0x80 ? VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK :
                     (b & 0xFFFFFF) > 0x808080 ? VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE : VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
    VkSampler s = 0;
    vkCreateSampler(dev, &ci, 0, &s);
    if (n_samplers < 128) { samplers[n_samplers].address = a; samplers[n_samplers].filter = f; samplers[n_samplers].border = b; samplers[n_samplers++].s = s; }
    return s;
}

/* ================================================================ pipelines */

typedef struct {
    uint32_t blend_en, sf, df, eq, cmask, topo, has_z;
} PipeKey;
typedef struct { PipeKey key; VkPipeline p; } PipeEntry;
static PipeEntry pipes[512]; static int n_pipes;

static VkBlendFactor blend_factor(uint32_t v, int alpha) {
    switch (v) {
    case 0x0000: return VK_BLEND_FACTOR_ZERO;
    case 0x0001: return VK_BLEND_FACTOR_ONE;
    case 0x0300: return alpha ? VK_BLEND_FACTOR_SRC_ALPHA : VK_BLEND_FACTOR_SRC_COLOR;
    case 0x0301: return alpha ? VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA : VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
    case 0x0302: return VK_BLEND_FACTOR_SRC_ALPHA;
    case 0x0303: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case 0x0304: return VK_BLEND_FACTOR_DST_ALPHA;
    case 0x0305: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    case 0x0306: return alpha ? VK_BLEND_FACTOR_DST_ALPHA : VK_BLEND_FACTOR_DST_COLOR;
    case 0x0307: return alpha ? VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA : VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
    case 0x0308: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
    case 0x8001: return alpha ? VK_BLEND_FACTOR_CONSTANT_ALPHA : VK_BLEND_FACTOR_CONSTANT_COLOR;
    case 0x8002: return alpha ? VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA : VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
    case 0x8003: return VK_BLEND_FACTOR_CONSTANT_ALPHA;
    case 0x8004: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
    default: return VK_BLEND_FACTOR_ONE;
    }
}

static VkBlendOp blend_op(uint32_t v) {
    switch (v) {
    case 0x800A: return VK_BLEND_OP_SUBTRACT;
    case 0x800B: return VK_BLEND_OP_REVERSE_SUBTRACT;
    case 0x8007: return VK_BLEND_OP_MIN;
    case 0x8008: return VK_BLEND_OP_MAX;
    default: return VK_BLEND_OP_ADD;
    }
}

static VkCompareOp cmp_op(uint32_t v) { return (VkCompareOp)((v - 0x200) & 7); }

static VkStencilOp stencil_op(uint32_t v) {
    switch (v) {
    case 0x0000: return VK_STENCIL_OP_ZERO;
    case 0x1E01: return VK_STENCIL_OP_REPLACE;
    case 0x1E02: return VK_STENCIL_OP_INCREMENT_AND_CLAMP;
    case 0x1E03: return VK_STENCIL_OP_DECREMENT_AND_CLAMP;
    case 0x150A: return VK_STENCIL_OP_INVERT;
    case 0x8507: return VK_STENCIL_OP_INCREMENT_AND_WRAP;
    case 0x8508: return VK_STENCIL_OP_DECREMENT_AND_WRAP;
    default: return VK_STENCIL_OP_KEEP;
    }
}

static VkPipeline get_pipeline(const PipeKey *k) {
    for (int i = 0; i < n_pipes; i++) if (!memcmp(&pipes[i].key, k, sizeof *k)) return pipes[i].p;
    VkPipelineShaderStageCreateInfo st[2] = {
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, 0, 0, VK_SHADER_STAGE_VERTEX_BIT, vs_mod, "main"},
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, 0, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fs_mod, "main"}};
    VkPipelineVertexInputStateCreateInfo vi = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = k->topo == KTOPO_TRIS ? VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST :
                  k->topo == KTOPO_LINES ? VK_PRIMITIVE_TOPOLOGY_LINE_LIST : VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
    VkPipelineViewportStateCreateInfo vp = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, 0, 0, 1, 0, 1, 0};
    VkPipelineRasterizationStateCreateInfo rs = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL; rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo ds = {VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    VkPipelineColorBlendAttachmentState ba = {0};
    ba.blendEnable = k->blend_en;
    ba.srcColorBlendFactor = blend_factor(k->sf, 0); ba.dstColorBlendFactor = blend_factor(k->df, 0);
    ba.srcAlphaBlendFactor = blend_factor(k->sf, 1); ba.dstAlphaBlendFactor = blend_factor(k->df, 1);
    ba.colorBlendOp = ba.alphaBlendOp = blend_op(k->eq);
    uint32_t m = k->cmask;   /* NV097 color mask: A 0x01000000, R 0x00010000, G 0x00000100, B 0x00000001 */
    ba.colorWriteMask = ((m >> 16) & 1 ? VK_COLOR_COMPONENT_R_BIT : 0) | ((m >> 8) & 1 ? VK_COLOR_COMPONENT_G_BIT : 0) |
                        (m & 1 ? VK_COLOR_COMPONENT_B_BIT : 0) | ((m >> 24) & 1 ? VK_COLOR_COMPONENT_A_BIT : 0);
    VkPipelineColorBlendStateCreateInfo cb = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = 1; cb.pAttachments = &ba;
    VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_CULL_MODE,
                            VK_DYNAMIC_STATE_FRONT_FACE, VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE, VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE,
                            VK_DYNAMIC_STATE_DEPTH_COMPARE_OP, VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE, VK_DYNAMIC_STATE_STENCIL_OP,
                            VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK, VK_DYNAMIC_STATE_STENCIL_WRITE_MASK,
                            VK_DYNAMIC_STATE_STENCIL_REFERENCE, VK_DYNAMIC_STATE_BLEND_CONSTANTS};
    VkPipelineDynamicStateCreateInfo dy = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, 0, 0,
                                           sizeof dyn / sizeof dyn[0], dyn};
    VkFormat cf = VK_FORMAT_R8G8B8A8_UNORM;
    VkPipelineRenderingCreateInfo pr = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    pr.colorAttachmentCount = 1; pr.pColorAttachmentFormats = &cf;
    pr.depthAttachmentFormat = k->has_z ? depth_fmt : VK_FORMAT_UNDEFINED;
    pr.stencilAttachmentFormat = k->has_z ? depth_fmt : VK_FORMAT_UNDEFINED;
    VkGraphicsPipelineCreateInfo ci = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, &pr};
    ci.stageCount = 2; ci.pStages = st; ci.pVertexInputState = &vi; ci.pInputAssemblyState = &ia; ci.pViewportState = &vp;
    ci.pRasterizationState = &rs; ci.pMultisampleState = &ms; ci.pDepthStencilState = &ds; ci.pColorBlendState = &cb;
    ci.pDynamicState = &dy; ci.layout = plal;
    VkPipeline p = 0;
    if (vkCreateGraphicsPipelines(dev, 0, 1, &ci, 0, &p) != VK_SUCCESS) { LOG("[gpu] pipeline creation failed\n"); return 0; }
    if (n_pipes < 512) { pipes[n_pipes].key = *k; pipes[n_pipes].p = p; n_pipes++; }
    return p;
}

/* ================================================================ init / frames */

typedef struct {
    uint32_t prog[136][4];
    float cst[192][4];
    float comp[4][4];
    float texm[16][4];
    float vpo[4], surf[4], zr[4];
    uint32_t mode[4];
    uint32_t comb[9][4];
    float cfac[16][4];
    float fogc[4], alpha[4];
    float tsz[4][4];
    uint32_t tmode[4];
    float sfac[2][4];            /* final combiner c0, c1 (SET_SPECULAR_FOG_FACTOR) */
    float fogp[4];               /* fog params 0, 1; function (0 off, 1 linear, 2 exp, 3 exp2); abs */
} DrawUBO;

#define VTX_RING (96ull << 20)
#define IDX_RING (16ull << 20)
#define UBO_RING (24ull << 20)
#define STAGE_RING (64ull << 20)

static void frame_begin(void) {
    Frame *f = &frames[fi];
    vkWaitForFences(dev, 1, &f->fence, VK_TRUE, UINT64_MAX);
    vkResetFences(dev, 1, &f->fence);
    for (int i = 0; i < f->ngarbage; i++) {
        vkDestroyImageView(dev, f->gview[i], 0); vkDestroyImage(dev, f->gimg[i], 0); img_free(gallocs[fi][i]);
    }
    f->ngarbage = 0;
    vkResetCommandPool(dev, f->pool, 0);
    vkResetDescriptorPool(dev, f->dpool, 0);
    f->vtx.pos = f->idx.pos = f->ubo.pos = f->stage.pos = 0;
    f->up_used = 0;
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, 0, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    vkBeginCommandBuffer(f->cmd, &bi);
    vkBeginCommandBuffer(f->up, &bi);
}

static int vk_init(void) {
    if (volkInitialize() != VK_SUCCESS) { LOG("[gpu] no Vulkan loader (vulkan-1.dll): install a GPU driver with Vulkan\n"); return 0; }
    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO, 0, "Project Underground", 1, "underground", 1, VK_API_VERSION_1_3};
    const char *exts[] = {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_WIN32_SURFACE_EXTENSION_NAME};
    VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, 0, 0, &app, 0, 0, 2, exts};
    const char *layer = "VK_LAYER_KHRONOS_validation";
    if (getenv("UG_VKDEBUG")) { ici.enabledLayerCount = 1; ici.ppEnabledLayerNames = &layer; }
    VKC(vkCreateInstance(&ici, 0, &inst));
    volkLoadInstance(inst);
    VkWin32SurfaceCreateInfoKHR sci = {VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR, 0, 0, GetModuleHandleW(NULL), g_hwnd};
    VKC(vkCreateWin32SurfaceKHR(inst, &sci, 0, &surface));
    /* device: prefer discrete GPUs with Vulkan 1.3 and a graphics queue that can present */
    uint32_t n = 8;
    VkPhysicalDevice pds[8];
    vkEnumeratePhysicalDevices(inst, &n, pds);
    int best = -1, best_score = -1;
    for (uint32_t i = 0; i < n; i++) {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(pds[i], &p);
        if (p.apiVersion < VK_API_VERSION_1_3) continue;
        uint32_t nq = 16;
        VkQueueFamilyProperties qp[16];
        vkGetPhysicalDeviceQueueFamilyProperties(pds[i], &nq, qp);
        for (uint32_t q = 0; q < nq; q++) {
            VkBool32 pres = 0;
            vkGetPhysicalDeviceSurfaceSupportKHR(pds[i], q, surface, &pres);
            if ((qp[q].queueFlags & VK_QUEUE_GRAPHICS_BIT) && pres) {
                int score = p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 2 : 1;
                if (score > best_score) { best_score = score; best = (int)i; qfam = q; }
                break;
            }
        }
    }
    if (best < 0) { LOG("[gpu] no Vulkan 1.3 GPU that can present to the window\n"); return 0; }
    pdev = pds[best];
    vkGetPhysicalDeviceProperties(pdev, &pprops);
    vkGetPhysicalDeviceMemoryProperties(pdev, &memprops);
    VkPhysicalDeviceVulkan13Features f13 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    f13.dynamicRendering = VK_TRUE; f13.synchronization2 = VK_TRUE;
    VkPhysicalDeviceFeatures2 f2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &f13};
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, 0, 0, qfam, 1, &prio};
    const char *dexts[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &f2, 0, 1, &qci, 0, 0, 1, dexts, 0};
    VKC(vkCreateDevice(pdev, &dci, 0, &dev));
    volkLoadDevice(dev);
    vkGetDeviceQueue(dev, qfam, 0, &queue);
    /* depth: D24S8 is not on every GPU (AMD); D32S8 is */
    depth_fmt = VK_FORMAT_D32_SFLOAT_S8_UINT;
    {
        VkFormatProperties fp;
        vkGetPhysicalDeviceFormatProperties(pdev, VK_FORMAT_D24_UNORM_S8_UINT, &fp);
        if (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) depth_fmt = VK_FORMAT_D24_UNORM_S8_UINT;
    }
    if (!swapchain_create()) return 0;

    /* descriptors: 0 draw UBO (dynamic), 1 vertices SSBO (dynamic), 2..5 textures */
    VkDescriptorSetLayoutBinding b[6] = {
        {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0},
        {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, 1, VK_SHADER_STAGE_VERTEX_BIT, 0},
        {2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, 0},
        {3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, 0},
        {4, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, 0},
        {5, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, 0}};
    VkDescriptorSetLayoutCreateInfo dli = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, 0, 0, 6, b};
    VKC(vkCreateDescriptorSetLayout(dev, &dli, 0, &dsl));
    VkPipelineLayoutCreateInfo pli = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, 0, 0, 1, &dsl};
    VKC(vkCreatePipelineLayout(dev, &pli, 0, &plal));
    VkShaderModuleCreateInfo smi = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, 0, 0, sizeof spv_nv2a_vert, spv_nv2a_vert};
    VKC(vkCreateShaderModule(dev, &smi, 0, &vs_mod));
    smi.codeSize = sizeof spv_nv2a_frag; smi.pCode = spv_nv2a_frag;
    VKC(vkCreateShaderModule(dev, &smi, 0, &fs_mod));

    for (int i = 0; i < FRAMES; i++) {
        Frame *f = &frames[i];
        VkCommandPoolCreateInfo pci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, 0, VK_COMMAND_POOL_CREATE_TRANSIENT_BIT, qfam};
        VKC(vkCreateCommandPool(dev, &pci, 0, &f->pool));
        VkCommandBuffer cbs[2];
        VkCommandBufferAllocateInfo cai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, 0, f->pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 2};
        VKC(vkAllocateCommandBuffers(dev, &cai, cbs));
        f->cmd = cbs[0]; f->up = cbs[1];
        VkFenceCreateInfo fci = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, 0, VK_FENCE_CREATE_SIGNALED_BIT};
        VKC(vkCreateFence(dev, &fci, 0, &f->fence));
        VkSemaphoreCreateInfo sei = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        VKC(vkCreateSemaphore(dev, &sei, 0, &f->acquired));
        VkDescriptorPoolSize ps[3] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 8192}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, 8192},
                                      {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 32768}};
        VkDescriptorPoolCreateInfo dpi = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, 0, 0, 8192, 3, ps};
        VKC(vkCreateDescriptorPool(dev, &dpi, 0, &f->dpool));
        if (!ring_create(&f->vtx, VTX_RING, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT)) return 0;
        if (!ring_create(&f->idx, IDX_RING, VK_BUFFER_USAGE_INDEX_BUFFER_BIT)) return 0;
        if (!ring_create(&f->ubo, UBO_RING, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT)) return 0;
        if (!ring_create(&f->stage, STAGE_RING, VK_BUFFER_USAGE_TRANSFER_SRC_BIT)) return 0;
    }
    VkSamplerCreateInfo sci2 = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sci2.magFilter = sci2.minFilter = VK_FILTER_NEAREST;
    sci2.addressModeU = sci2.addressModeV = sci2.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    VKC(vkCreateSampler(dev, &sci2, 0, &blit_sampler));
    fi = 0;
    frame_begin();
    /* 1x1 white texture for unused stages */
    if (!img_create(&white_tex, 1, 1, 1, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                    VK_IMAGE_ASPECT_COLOR_BIT)) return 0;
    barrier(CMD(), &white_tex, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkClearColorValue one = {{1, 1, 1, 1}};
    VkImageSubresourceRange rg = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(CMD(), white_tex.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &one, 1, &rg);
    barrier(CMD(), &white_tex, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    LOG("[gpu] Vulkan ready: %s (API %u.%u), depth %s, scale %d\n", pprops.deviceName, VK_API_VERSION_MAJOR(pprops.apiVersion),
        VK_API_VERSION_MINOR(pprops.apiVersion), depth_fmt == VK_FORMAT_D24_UNORM_S8_UINT ? "D24S8" : "D32S8", g_scale);
    return 1;
}

void gpu_init(void) {
    if (gpu_ok) return;
    gpu_ok = -1;
    if (getenv("UG_HEADLESS") && *getenv("UG_HEADLESS") == '1') { LOG("[gpu] headless: no window, nothing drawn\n"); return; }
    if (getenv("UG_SCALE")) { g_scale = atoi(getenv("UG_SCALE")); if (g_scale < 1) g_scale = 1; if (g_scale > 8) g_scale = 8; }
    HANDLE ev = CreateEventW(NULL, TRUE, FALSE, NULL);
    CreateThread(NULL, 0, window_thread, ev, 0, NULL);
    WaitForSingleObject(ev, INFINITE);
    CloseHandle(ev);
    if (vk_init()) gpu_ok = 1;
}

static int ready(void) {
    if (!gpu_ok) gpu_init();
    return gpu_ok == 1;
}

/* ================================================================ draw */

static void argb_to_f4(uint32_t c, float *o) {
    o[0] = ((c >> 16) & 0xFF) / 255.f; o[1] = ((c >> 8) & 0xFF) / 255.f; o[2] = (c & 0xFF) / 255.f; o[3] = (c >> 24) / 255.f;
}

static void draw_impl(int topo, const KVertex *v, uint32_t nv, const uint32_t *idx, uint32_t ni) {
    st_draws++; st_verts += nv;
    if (!ready() || !ni) return;
    {   /* UG_SKIPFMT=<hex>: drop draws whose texture 0 has this colour format (debug) */
        static int skip = -2;
        if (skip == -2) { const char *e = getenv("UG_SKIPFMT"); skip = e ? (int)strtol(e, NULL, 16) : -1; }
        if (skip >= 0 && (KREG(K_TEX_CONTROL0(0)) >> 30 & 1) && (int)(KREG(K_TEX_FORMAT(0)) >> 8 & 0xFF) == skip) return;
        /* UG_SKIPMODE=<transform mode>: drop those draws in odd 300-frame windows (A/B shots in one run, debug) */
        static int skipm = -2;
        if (skipm == -2) { const char *e = getenv("UG_SKIPMODE"); skipm = e ? atoi(e) : -1; }
        if (skipm >= 0 && (frame_no / 300 & 1) && (int)(KREG(K_TRANSFORM_MODE) & 3) == skipm) return;
    }
    if (!select_targets()) return;
    Frame *f = &frames[fi];

    /* uniforms */
    VkDeviceSize uoff, voff, ioff;
    DrawUBO *u = ring_alloc(&f->ubo, sizeof(DrawUBO), pprops.limits.minUniformBufferOffsetAlignment, &uoff);
    void *vp = ring_alloc(&f->vtx, (VkDeviceSize)nv * sizeof(KVertex), pprops.limits.minStorageBufferOffsetAlignment, &voff);
    void *ip = ring_alloc(&f->idx, (VkDeviceSize)ni * 4, 4, &ioff);
    if (!u || !vp || !ip) {
        static int warned;
        if (!warned++) LOG("[gpu] per-frame buffers full: draws dropped\n");
        return;
    }
    memcpy(vp, v, (size_t)nv * sizeof(KVertex));
    memcpy(ip, idx, (size_t)ni * 4);
    memcpy(u->prog, g_vp_prog, sizeof u->prog);
    memcpy(u->cst, g_vp_const, sizeof u->cst);
    for (int i = 0; i < 16; i++) u->comp[i / 4][i % 4] = kf(K_COMPOSITE + i * 4);
    for (int s = 0; s < 4; s++)
        for (int j = 0; j < 16; j++) u->texm[s * 4 + j / 4][j % 4] = kf(K_TEXTURE_MATRIX + s * 64 + j * 4);
    u->vpo[0] = kf(K_VIEWPORT_OFFSET); u->vpo[1] = kf(K_VIEWPORT_OFFSET + 4); u->vpo[2] = u->vpo[3] = 0;
    u->surf[0] = (float)cur_rt->w; u->surf[1] = (float)cur_rt->h; u->surf[2] = 1.0f / cur_rt->w; u->surf[3] = 1.0f / cur_rt->h;
    uint32_t zfmt = (KREG(K_SURF_FORMAT) >> 4) & 0xF;
    u->zr[0] = kf(K_CLIP_MIN); u->zr[1] = kf(K_CLIP_MAX); u->zr[2] = 1.0f / (zfmt == 1 ? 65535.0f : 16777215.0f); u->zr[3] = 0;
    u->mode[0] = KREG(K_TRANSFORM_MODE) & 3; u->mode[1] = g_vp_start < 136 ? g_vp_start : 0;
    u->mode[2] = 0;
    { static int dbg = -1; if (dbg < 0) { const char *e = getenv("UG_DBGCOLOR"); dbg = e ? atoi(e) : 0; } u->mode[3] = (uint32_t)dbg; }
    for (int s = 0; s < 4; s++) if (KREG(K_TEXMAT_EN + s * 4) & 1) u->mode[2] |= 1u << s;
    uint32_t ctl = KREG(K_COMB_CONTROL);
    u->comb[0][0] = ctl; u->comb[0][1] = KREG(K_COMB_SPECFOG_CW0); u->comb[0][2] = KREG(K_COMB_SPECFOG_CW1); u->comb[0][3] = 0;
    for (int st = 0; st < 8; st++) {
        u->comb[1 + st][0] = KREG(K_COMB_COLOR_ICW + st * 4); u->comb[1 + st][1] = KREG(K_COMB_COLOR_OCW + st * 4);
        u->comb[1 + st][2] = KREG(K_COMB_ALPHA_ICW + st * 4); u->comb[1 + st][3] = KREG(K_COMB_ALPHA_OCW + st * 4);
        argb_to_f4(KREG(K_COMB_FACTOR0 + st * 4), u->cfac[st]);
        argb_to_f4(KREG(K_COMB_FACTOR1 + st * 4), u->cfac[8 + st]);
    }
    argb_to_f4(KREG(K_SPECFOG_FACTOR0), u->sfac[0]);
    argb_to_f4(KREG(K_SPECFOG_FACTOR1), u->sfac[1]);
    {   /* fog function (as xemu): FOG_MODE 0x2601 linear, 0x800 exp, 0x801 exp2, 0x802/0x803 abs, 0x804 linear abs */
        uint32_t fm = KREG(K_FOG_MODE);
        memcpy(&u->fogp[0], &g_kreg[(K_FOG_PARAMS >> 2) + 0], 4);
        memcpy(&u->fogp[1], &g_kreg[(K_FOG_PARAMS >> 2) + 1], 4);
        u->fogp[2] = !(KREG(K_FOG_ENABLE) & 1) ? 0.f : fm == 0x800 || fm == 0x802 ? 2.f : fm == 0x801 || fm == 0x803 ? 3.f : 1.f;
        u->fogp[3] = (fm == 0x802 || fm == 0x803 || fm == 0x804) ? 1.f : 0.f;
    }
    { uint32_t fc = KREG(K_FOG_COLOR); u->fogc[0] = (fc & 0xFF) / 255.f; u->fogc[1] = ((fc >> 8) & 0xFF) / 255.f; u->fogc[2] = ((fc >> 16) & 0xFF) / 255.f; u->fogc[3] = 1; }
    u->alpha[0] = (float)(KREG(K_ALPHA_REF) & 0xFF);
    u->alpha[1] = (float)((KREG(K_ALPHA_FUNC) - 0x200) & 7);
    u->alpha[2] = (KREG(K_ALPHA_TEST_EN) & 1) ? 1.f : 0.f;
    u->alpha[3] = 1.f;

    /* textures (may end rendering for render-target-to-texture transitions) */
    VkDescriptorImageInfo ii[4];
    uint32_t prog = KREG(K_SHADER_STAGE_PROG);
    for (int t = 0; t < 4; t++) {
        ii[t] = (VkDescriptorImageInfo){blit_sampler, white_tex.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        u->tmode[t] = 0;
        u->tsz[t][0] = u->tsz[t][1] = 1; u->tsz[t][2] = u->tsz[t][3] = 0;
        uint32_t mode = (prog >> (t * 5)) & 31;
        if (!(KREG(K_TEX_CONTROL0(t)) & 0x40000000u) || mode == 0) continue;
        if (mode == 4) { u->tmode[t] = 3; continue; }
        if (mode != 1) {
            static uint32_t warned;
            if (!(warned & (1u << mode))) { warned |= 1u << mode; LOG("[gpu] shader stage mode %u not implemented\n", mode); }
            continue;
        }
        int rect = 0, tw = 1, th = 1;
        VkImageView view = get_texture(t, &rect, &tw, &th);
        if (!view) continue;
        ii[t].imageView = view;
        ii[t].sampler = get_sampler(t, rect);
        u->tmode[t] = rect ? 2 : 1;
        u->tsz[t][0] = 1.0f / tw; u->tsz[t][1] = 1.0f / th;
    }

    {   /* UG_DRAWLOG=N: log every draw of frame N (state, first vertices, vertex program) */
        static int dl = -1;
        if (dl < 0) { const char *e = getenv("UG_DRAWLOG"); dl = e ? atoi(e) : 0; }
        if (dl && (int)frame_no == dl) {
            LOG("[draw] f%u topo %d nv %u ni %u mode %u start %u surf %dx%d clip %g..%g comb %X fcw %08X %08X tm %u/%u\n",
                frame_no, topo, nv, ni, u->mode[0], u->mode[1], cur_rt->w, cur_rt->h, u->zr[0], u->zr[1], ctl,
                u->comb[0][1], u->comb[0][2], u->tmode[0], u->tmode[1]);
            LOG("[draw]   blend %X %X/%X eq %X cmask %08X ztest %X zmask %X atest %X func %X ref %X tex0 fmt %08X ctl0 %08X filter %08X\n",
                KREG(K_BLEND_EN), KREG(K_BLEND_SFACTOR), KREG(K_BLEND_DFACTOR), KREG(K_BLEND_EQUATION), KREG(K_COLOR_MASK),
                KREG(K_DEPTH_TEST_EN), KREG(K_DEPTH_MASK), KREG(K_ALPHA_TEST_EN), KREG(K_ALPHA_FUNC), KREG(K_ALPHA_REF),
                KREG(K_TEX_FORMAT(0)), KREG(K_TEX_CONTROL0(0)), KREG(K_TEX_FILTER(0)));
            {
                float lo[4] = {1e30f, 1e30f, 1e30f, 1e30f}, hi[4] = {-1e30f, -1e30f, -1e30f, -1e30f};
                for (uint32_t i = 0; i < nv; i++)
                    for (int k = 0; k < 4; k++) {
                        if (v[i].a[0][k] < lo[k]) lo[k] = v[i].a[0][k];
                        if (v[i].a[0][k] > hi[k]) hi[k] = v[i].a[0][k];
                    }
                LOG("[draw]   fog en %X mode %X gen %X color %08X params %g %g sfac %08X %08X fogp.z %g\n", KREG(K_FOG_ENABLE),
                    KREG(K_FOG_MODE), KREG(K_FOG_GEN_MODE), KREG(K_FOG_COLOR), u->fogp[0], u->fogp[1],
                    KREG(K_SPECFOG_FACTOR0), KREG(K_SPECFOG_FACTOR1), u->fogp[2]);
                LOG("[draw]   texgen0 %X %X %X %X texgen1 %X %X %X %X tex en %X %X %X %X texmat en %u light %X\n",
                    g_kreg[0x3C0 >> 2], g_kreg[0x3C4 >> 2], g_kreg[0x3C8 >> 2], g_kreg[0x3CC >> 2], g_kreg[0x3D0 >> 2],
                    g_kreg[0x3D4 >> 2], g_kreg[0x3D8 >> 2], g_kreg[0x3DC >> 2], KREG(K_TEX_CONTROL0(0)) >> 30 & 1,
                    KREG(K_TEX_CONTROL0(1)) >> 30 & 1, KREG(K_TEX_CONTROL0(2)) >> 30 & 1, KREG(K_TEX_CONTROL0(3)) >> 30 & 1,
                    u->mode[2], g_kreg[0x314 >> 2]);
                LOG("[draw]   pos range x %g..%g y %g..%g z %g..%g w %g..%g\n", lo[0], hi[0], lo[1], hi[1], lo[2], hi[2],
                    lo[3], hi[3]);
            }
            for (uint32_t i = 0; i < nv && i < 4; i++)
                LOG("[draw]   v%u pos %g %g %g %g d %g %g %g %g t0 %g %g %g %g\n", i, v[i].a[0][0], v[i].a[0][1], v[i].a[0][2],
                    v[i].a[0][3], v[i].a[3][0], v[i].a[3][1], v[i].a[3][2], v[i].a[3][3], v[i].a[9][0], v[i].a[9][1],
                    v[i].a[9][2], v[i].a[9][3]);
            for (int st = 0; st < (int)(ctl & 0xFF) && st < 8; st++)
                LOG("[draw]   st%d %08X %08X %08X %08X f0 %08X f1 %08X\n", st, u->comb[1 + st][0], u->comb[1 + st][1],
                    u->comb[1 + st][2], u->comb[1 + st][3], KREG(K_COMB_FACTOR0 + st * 4), KREG(K_COMB_FACTOR1 + st * 4));
            if (u->mode[0] == 2)
                for (uint32_t pc = u->mode[1]; pc < 136; pc++) {
                    LOG("[draw]   vp%03u %08X %08X %08X %08X\n", pc, g_vp_prog[pc][0], g_vp_prog[pc][1], g_vp_prog[pc][2], g_vp_prog[pc][3]);
                    if (g_vp_prog[pc][3] & 1) break;
                }
            for (int c = 0; c < 192; c++)
                if (g_vp_const[c][0] != 0 || g_vp_const[c][1] != 0 || g_vp_const[c][2] != 0 || g_vp_const[c][3] != 0)
                    LOG("[draw]   c%d %g %g %g %g\n", c, g_vp_const[c][0], g_vp_const[c][1], g_vp_const[c][2], g_vp_const[c][3]);
        }
    }

    begin_rendering();

    /* descriptor set */
    VkDescriptorSet set;
    VkDescriptorSetAllocateInfo dai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, 0, f->dpool, 1, &dsl};
    if (vkAllocateDescriptorSets(dev, &dai, &set) != VK_SUCCESS) {
        static int warned;
        if (!warned++) LOG("[gpu] descriptor pool exhausted: draws dropped\n");
        return;
    }
    VkDescriptorBufferInfo ub = {f->ubo.buf, 0, sizeof(DrawUBO)}, sb = {f->vtx.buf, 0, (VkDeviceSize)nv * sizeof(KVertex)};
    VkWriteDescriptorSet w[6];
    memset(w, 0, sizeof w);
    for (int i = 0; i < 6; i++) { w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[i].dstSet = set; w[i].dstBinding = i; w[i].descriptorCount = 1; }
    w[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC; w[0].pBufferInfo = &ub;
    w[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC; w[1].pBufferInfo = &sb;
    for (int t = 0; t < 4; t++) { w[2 + t].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w[2 + t].pImageInfo = &ii[t]; }
    vkUpdateDescriptorSets(dev, 6, w, 0, 0);

    /* pipeline + dynamic state */
    int has_z = cur_zs != NULL;
    PipeKey k = {KREG(K_BLEND_EN) & 1, KREG(K_BLEND_SFACTOR), KREG(K_BLEND_DFACTOR), KREG(K_BLEND_EQUATION),
                 KREG(K_COLOR_MASK), (uint32_t)topo, (uint32_t)has_z};
    if (!k.blend_en) k.sf = k.df = k.eq = 0;
    VkPipeline p = get_pipeline(&k);
    if (!p) return;
    VkCommandBuffer cb = CMD();
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
    uint32_t dyn[2] = {(uint32_t)uoff, (uint32_t)voff};
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, plal, 0, 1, &set, 2, dyn);
    vkCmdBindIndexBuffer(cb, f->idx.buf, ioff, VK_INDEX_TYPE_UINT32);
    uint32_t ff = KREG(K_FRONT_FACE) ? KREG(K_FRONT_FACE) : 0x900;
    vkCmdSetCullMode(cb, !(KREG(K_CULL_EN) & 1) ? VK_CULL_MODE_NONE : KREG(K_CULL_FACE) == 0x404 ? VK_CULL_MODE_FRONT_BIT :
                         KREG(K_CULL_FACE) == 0x408 ? VK_CULL_MODE_FRONT_AND_BACK : VK_CULL_MODE_BACK_BIT);
    /* NV097 front face 0x900 = CW, 0x901 = CCW in screen space (y down); Vulkan's winding is in framebuffer space,
     * which is the same orientation here */
    vkCmdSetFrontFace(cb, ff == 0x901 ? VK_FRONT_FACE_COUNTER_CLOCKWISE : VK_FRONT_FACE_CLOCKWISE);
    vkCmdSetDepthTestEnable(cb, has_z && (KREG(K_DEPTH_TEST_EN) & 1));
    vkCmdSetDepthWriteEnable(cb, has_z && (KREG(K_DEPTH_MASK) & 1));
    vkCmdSetDepthCompareOp(cb, cmp_op(KREG(K_DEPTH_FUNC)));
    vkCmdSetStencilTestEnable(cb, has_z && (KREG(K_STENCIL_TEST_EN) & 1));
    vkCmdSetStencilOp(cb, VK_STENCIL_FACE_FRONT_AND_BACK, stencil_op(KREG(K_STENCIL_OP_FAIL)), stencil_op(KREG(K_STENCIL_OP_ZPASS)),
                      stencil_op(KREG(K_STENCIL_OP_ZFAIL)), cmp_op(KREG(K_STENCIL_FUNC)));
    vkCmdSetStencilCompareMask(cb, VK_STENCIL_FACE_FRONT_AND_BACK, KREG(K_STENCIL_FMASK) & 0xFF);
    vkCmdSetStencilWriteMask(cb, VK_STENCIL_FACE_FRONT_AND_BACK, KREG(K_STENCIL_MASK) & 0xFF);
    vkCmdSetStencilReference(cb, VK_STENCIL_FACE_FRONT_AND_BACK, KREG(K_STENCIL_REF) & 0xFF);
    float bc[4];
    argb_to_f4(KREG(K_BLEND_COLOR), bc);
    vkCmdSetBlendConstants(cb, bc);
    vkCmdDrawIndexed(cb, ni, 1, 0, 0, 0);
}

/* ================================================================ clear / flip */

static void clear_impl(uint32_t flags) {
    if (!ready()) return;
    if (!select_targets()) return;
    begin_rendering();
    uint32_t h = KREG(K_CLEAR_RECT_H), v = KREG(K_CLEAR_RECT_V);
    int x0 = h & 0xFFF, x1 = (h >> 16) & 0xFFF, y0 = v & 0xFFF, y1 = (v >> 16) & 0xFFF;
    if (x1 >= cur_rt->w) x1 = cur_rt->w - 1;
    if (y1 >= cur_rt->h) y1 = cur_rt->h - 1;
    if (x1 < x0 || y1 < y0) return;
    VkClearRect rect = {{{x0 * g_scale, y0 * g_scale}, {(uint32_t)((x1 - x0 + 1) * g_scale), (uint32_t)((y1 - y0 + 1) * g_scale)}}, 0, 1};
    VkClearAttachment ca[2];
    int n = 0;
    if (flags & 0xF0) {
        float c[4];
        argb_to_f4(KREG(K_COLOR_CLEAR), c);
        ca[n].aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; ca[n].colorAttachment = 0;
        memcpy(ca[n].clearValue.color.float32, c, 16);
        n++;
    }
    if ((flags & 3) && cur_zs) {
        uint32_t zv = KREG(K_ZSTENCIL_CLEAR);
        uint32_t zfmt = (KREG(K_SURF_FORMAT) >> 4) & 0xF;
        ca[n].aspectMask = ((flags & 1) ? VK_IMAGE_ASPECT_DEPTH_BIT : 0) | ((flags & 2) ? VK_IMAGE_ASPECT_STENCIL_BIT : 0);
        ca[n].colorAttachment = 0;
        ca[n].clearValue.depthStencil.depth = zfmt == 1 ? (zv & 0xFFFF) / 65535.f : (zv >> 8) / 16777215.f;
        ca[n].clearValue.depthStencil.stencil = zv & 0xFF;
        n++;
    }
    if (n) vkCmdClearAttachments(CMD(), (uint32_t)n, ca, 1, &rect);
}

static void submit_and_present(Surface *s) {
    Frame *f = &frames[fi];
    end_rendering();
    uint32_t img = UINT32_MAX;
    VkResult ar = swapchain ? vkAcquireNextImageKHR(dev, swapchain, UINT64_MAX, f->acquired, 0, &img) : VK_ERROR_OUT_OF_DATE_KHR;
    if (ar == VK_ERROR_OUT_OF_DATE_KHR) { vkDeviceWaitIdle(dev); swapchain_create(); img = UINT32_MAX; }
    else if (ar != VK_SUCCESS && ar != VK_SUBOPTIMAL_KHR) img = UINT32_MAX;
    VkCommandBuffer cb = f->cmd;
    if (img != UINT32_MAX) {
        raw_barrier(cb, sc_img[img], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkClearColorValue black = {{0, 0, 0, 1}};
        VkImageSubresourceRange rg = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdClearColorImage(cb, sc_img[img], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &rg);
        if (s) {
            barrier(cb, &s->color, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
            /* letterbox the guest surface's aspect into the window */
            float sa = (float)s->w / s->h, wa = (float)sc_ext.width / sc_ext.height;
            int dw = (int)sc_ext.width, dh = (int)sc_ext.height;
            if (wa > sa) dw = (int)(dh * sa + 0.5f); else dh = (int)(dw / sa + 0.5f);
            int dx = ((int)sc_ext.width - dw) / 2, dy = ((int)sc_ext.height - dh) / 2;
            VkImageBlit bl = {{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {{0, 0, 0}, {s->hw, s->hh, 1}},
                              {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {{dx, dy, 0}, {dx + dw, dy + dh, 1}}};
            vkCmdBlitImage(cb, s->color.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, sc_img[img],
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bl, VK_FILTER_LINEAR);
        }
        raw_barrier(cb, sc_img[img], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    }
    /* UG_SHOT=N: every N frames, read the presented surface back and write out/shot_<frame>.ppm (debug) */
    static int shot_every = -1;
    static Ring shot;
    if (shot_every < 0) { const char *e = getenv("UG_SHOT"); shot_every = e ? atoi(e) : 0; }
    int shoot = s && shot_every > 0 && frame_no % (uint32_t)shot_every == 0;
    if (shoot) {
        VkDeviceSize need = (VkDeviceSize)s->hw * s->hh * 4;
        if (shot.size < need) {
            if (shot.buf) { vkDestroyBuffer(dev, shot.buf, 0); vkFreeMemory(dev, shot.mem, 0); memset(&shot, 0, sizeof shot); }
            if (!ring_create(&shot, need, VK_BUFFER_USAGE_TRANSFER_DST_BIT)) shoot = 0;
        }
        if (shoot) {
            barrier(cb, &s->color, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
            VkBufferImageCopy rg = {0, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {(uint32_t)s->hw, (uint32_t)s->hh, 1}};
            vkCmdCopyImageToBuffer(cb, s->color.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, shot.buf, 1, &rg);
        }
    }
    vkEndCommandBuffer(f->up);
    vkEndCommandBuffer(cb);
    VkCommandBuffer cbs[2] = {f->up, cb};
    VkPipelineStageFlags wst = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 2; si.pCommandBuffers = cbs;
    if (img != UINT32_MAX) {
        si.waitSemaphoreCount = 1; si.pWaitSemaphores = &f->acquired; si.pWaitDstStageMask = &wst;
        si.signalSemaphoreCount = 1; si.pSignalSemaphores = &sc_done[img];
    }
    VkResult sr = vkQueueSubmit(queue, 1, &si, f->fence);
    if (sr != VK_SUCCESS) { LOG("[gpu] vkQueueSubmit failed %d\n", sr); gpu_ok = -1; return; }
    if (shoot) {
        vkQueueWaitIdle(queue);
        char path[64];
        snprintf(path, sizeof path, "out/shot_%05u.ppm", frame_no);
        FILE *fp = fopen(path, "wb");
        if (fp) {
            fprintf(fp, "P6 %d %d 255\n", s->hw, s->hh);
            for (int i = 0; i < s->hw * s->hh; i++) fwrite(shot.map + i * 4, 1, 3, fp);
            fclose(fp);
            LOG("[gpu] screenshot %s\n", path);
        }
    }
    if (img != UINT32_MAX) {
        VkPresentInfoKHR pi = {VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, 0, 1, &sc_done[img], 1, &swapchain, &img, 0};
        VkResult pr = vkQueuePresentKHR(queue, &pi);
        if (pr == VK_ERROR_OUT_OF_DATE_KHR || pr == VK_SUBOPTIMAL_KHR) { vkDeviceWaitIdle(dev); swapchain_create(); }
    }
    fi = (fi + 1) % FRAMES;
    frame_begin();
}

static void flip_impl(uint32_t addr) {
    if (!ready()) return;
    frame_no++;
    if (frame_no % 60 == 0)   /* evict textures idle for 10 s */
        for (int i = 0; i < TEX_CACHE; i++)
            if (tcache[i].im.img && frame_no - tcache[i].last_frame > 600) img_destroy_later(&tcache[i].im);
    /* XDK 5659 swaps without naming the front buffer (4242 left it in 1D8C): present the surface just rendered */
    Surface *s = find_rt(addr);
    if (!s) s = cur_rt;
    submit_and_present(s);
    if (frame_no % 300 == 0) {
        static DWORD t0;
        DWORD t = GetTickCount();
        LOG("[gpu] frame %u: %.1f fps, per frame: draws %u verts %u new textures %u, %d pipelines, %d targets\n", frame_no,
            t0 ? 300000.0 / (double)(t - t0) : 0.0, st_draws / 300, st_verts / 300, st_newtex / 300, n_pipes, n_rts);
        st_draws = st_verts = st_newtex = 0;
        t0 = t;
    }
}

/* ================================================================ public entry points */

static SRWLOCK gpu_lock = SRWLOCK_INIT;

void gpu_draw(int topo, const KVertex *v, uint32_t nv, const uint32_t *idx, uint32_t ni) {
    AcquireSRWLockExclusive(&gpu_lock); draw_impl(topo, v, nv, idx, ni); ReleaseSRWLockExclusive(&gpu_lock);
}
void gpu_clear(uint32_t flags) {
    AcquireSRWLockExclusive(&gpu_lock); clear_impl(flags); ReleaseSRWLockExclusive(&gpu_lock);
}
void gpu_flip(uint32_t addr) {
    AcquireSRWLockExclusive(&gpu_lock); flip_impl(addr); ReleaseSRWLockExclusive(&gpu_lock);
}
void gpu_vblank(void) {}
