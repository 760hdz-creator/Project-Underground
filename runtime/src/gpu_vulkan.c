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
#ifdef _WIN32
#define VK_USE_PLATFORM_WIN32_KHR
#endif
#include "volk.h"
#ifndef _WIN32
#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#endif
#include <math.h>
#include <stdarg.h>
#include <stdlib.h>
#include <limits.h>
#include <string.h>
#include "rt.h"
#include "hdtex.h"
#include "kelvin.h"
#include "shaders_spv.h"
#define XXH_INLINE_ALL
#include "xxhash.h"

static int has_depth_clamp;
volatile float g_aniso;   /* max sampler anisotropy, 0 = off (UG_ANISO, menu.c) */
static int has_aniso;
static int has_bc = 1;   /* textureCompressionBC: most Mali and older Adreno GPUs lack it (UG_NOBC=1 forces the CPU path) */
static VkSurfaceTransformFlagBitsKHR sc_transform;
static int g_scale = 2;   /* UG_SCALE=1..8: host render-target scale over the guest resolution */

static HWND g_hwnd;
static int gpu_ok;
static uint32_t frame_no;
static uint32_t g_draw_idx;
/* F9 (debug capture): logs every draw of the next frame ([draw] lines), saves it (out/shot_N.ppm), then saves the
 * frame after it painted with draw indices (UG_DBGCOLOR=10 colours, out/shot_N+1.ppm). 1 = requested. */
static volatile int g_cap;
static uint32_t g_cap_log_frame;   /* frame whose draws are logged */
static int g_cap_dbg, g_cap_shoot;   /* draw index in the current frame (UG_DBGCOLOR=10, UG_DRAWLOG) */

/* ================================================================ window */

#ifdef _WIN32

static volatile int g_creating;
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
    case WM_KEYDOWN: {
        extern volatile int g_menu_req;
        extern int menu_is_open(void);
        extern void menu_key(int);
        if (menu_is_open()) {   /* in-game menu (menu.c): arrows, Enter/Space = ok, Esc/F1/Backspace = back */
            int k = w == VK_UP ? 1 : w == VK_DOWN ? 2 : w == VK_LEFT ? 3 : w == VK_RIGHT ? 4
                  : w == VK_RETURN || w == VK_SPACE ? 5 : w == VK_ESCAPE || w == VK_F1 || w == VK_BACK ? 6 : 0;
            if (k && (!(l & (1 << 30)) || k <= 4)) menu_key(k);   /* only the arrows autorepeat */
            if (w != VK_F11) return 0;
        } else if ((w == VK_ESCAPE || w == VK_F1) && !(l & (1 << 30))) {   /* first press, not autorepeat */
            g_menu_req = 1;
            return 0;
        }
        if (w == VK_F9 && !g_cap) { g_cap = 1; LOG("[capture] F9\n"); return 0; }
        if (w == VK_F11) { toggle_fullscreen(h); return 0; }
        break;
    }
    case WM_SYSKEYDOWN:
        if (w == VK_RETURN && (l & (1 << 29))) { toggle_fullscreen(h); return 0; }
        if (w == VK_F10) {   /* HD texture pack on/off (compare) */
            extern volatile int g_hd_hidden;
            g_hd_hidden = !g_hd_hidden;
            LOG("[hdtex] HD textures %s (F10)\n", g_hd_hidden ? "off" : "on");
            return 0;
        }
        break;
    case WM_SYSCHAR: if (w == VK_RETURN) return 0; break;
    case WM_APP + 1: toggle_fullscreen(h); return 0;   /* in-game menu (plat_fullscreen_toggle) */
    case WM_CHAR: { extern void kbd_text_push(int); if (getenv("UG_KBDLOG")) LOG("[input] WM_CHAR %02X\n", (unsigned)w); kbd_text_push((int)w); return 0; }
    case 0x02E0: {   /* WM_DPICHANGED (moved to a monitor with another scale): take Windows' suggested rect */
        const RECT *s = (const RECT *)l;
        if (!g_creating && !(GetWindowLongW(h, GWL_STYLE) & WS_POPUP))
            SetWindowPos(h, NULL, s->left, s->top, s->right - s->left, s->bottom - s->top, SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }
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
    {   /* per-monitor DPI aware: else Windows scales a smaller swapchain up (blurry at 125-200% display scaling) and
         * UG_WINDOW sizes would be in scaled units, not pixels */
        typedef BOOL(WINAPI * SetCtx)(HANDLE);
        SetCtx f = (SetCtx)(void *)GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetProcessDpiAwarenessContext");
        if (f && !f((HANDLE)-4)) LOG("[gpu] DPI awareness not set (error %lu)\n", GetLastError());   /* PER_MONITOR_AWARE_V2 */
        /* this thread already exists: it keeps the old context unless it is set for the thread too */
        typedef HANDLE(WINAPI * SetThr)(HANDLE);
        SetThr t = (SetThr)(void *)GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetThreadDpiAwarenessContext");
        if (t) t((HANDLE)-4);
    }
    WNDCLASSW wc = {0};
    wc.lpfnWndProc = wndproc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = L"UndergroundRecomp";
    {   /* window/taskbar icon: underground.ico, which the installer puts next to the exe (dev runs have none) */
        wchar_t ico[MAX_PATH];
        DWORD n = GetModuleFileNameW(NULL, ico, MAX_PATH);
        while (n && ico[n - 1] != L'\\') n--;
        if (n && n + 16 < MAX_PATH) {
            wcscpy(ico + n, L"underground.ico");
            wc.hIcon = (HICON)LoadImageW(NULL, ico, IMAGE_ICON, 0, 0, LR_LOADFROMFILE | LR_DEFAULTSIZE);
        }
    }
    RegisterClassW(&wc);
    const char *me = getenv("UG_MONITOR");
    int dev_run = GetConsoleWindow() != NULL;
    MonPick mp = {me && *me ? atoi(me) : (dev_run ? -1 : -2), 0, {0}, 0};
    if (mp.want != -2) EnumDisplayMonitors(NULL, NULL, mon_enum, (LPARAM)&mp);
    POINT origin = {0, 0};
    HMONITOR hm = mp.found ? MonitorFromRect(&mp.pick, MONITOR_DEFAULTTONEAREST) : MonitorFromPoint(origin, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi = {sizeof mi};
    GetMonitorInfoW(hm, &mi);
    /* client size: UG_WINDOW=WxH, else 2/3 of the monitor's height at the game's aspect; centred on the monitor */
    int mh = mi.rcMonitor.bottom - mi.rcMonitor.top;
    int wh = mh * 2 / 3, ww = ug_widescreen() ? wh * 16 / 9 : wh * 4 / 3;
    const char *wsz = getenv("UG_WINDOW");
    int qw = 0, qh = 0;
    if (wsz && sscanf(wsz, "%dx%d", &qw, &qh) == 2 && qw >= 320 && qh >= 240) { ww = qw; wh = qh; } {   /* larger than the monitor: scaled down to fit, same aspect */
        int mw = (int)(mi.rcWork.right - mi.rcWork.left) - 16, mh = (int)(mi.rcWork.bottom - mi.rcWork.top) - 64;
        if (ww > mw) { wh = wh * mw / ww; ww = mw; }
        if (wh > mh) { ww = ww * mh / wh; wh = mh; }
    }
    /* the frame at the monitor's own DPI (per-monitor aware: sizes are physical pixels) */
    RECT r = {0, 0, ww, wh};
    typedef BOOL(WINAPI * AdjForDpi)(RECT *, DWORD, BOOL, DWORD, UINT);
    typedef HRESULT(WINAPI * MonDpi)(HMONITOR, int, UINT *, UINT *);
    AdjForDpi adj = (AdjForDpi)(void *)GetProcAddress(GetModuleHandleW(L"user32.dll"), "AdjustWindowRectExForDpi");
    HMODULE shc = LoadLibraryW(L"shcore.dll");
    MonDpi mdpi = shc ? (MonDpi)(void *)GetProcAddress(shc, "GetDpiForMonitor") : NULL;
    UINT dx = 96, dy = 96;
    if (adj && mdpi && mdpi(hm, 0, &dx, &dy) == 0) adj(&r, WS_OVERLAPPEDWINDOW, FALSE, 0, dx);
    else AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    int fw = r.right - r.left, fh = r.bottom - r.top;
    int fx = (int)(mi.rcWork.right - mi.rcWork.left - fw) / 2, fy = (int)(mi.rcWork.bottom - mi.rcWork.top - fh) / 2;
    int x = mi.rcWork.left + (fx > 0 ? fx : 0), y = mi.rcWork.top + (fy > 0 ? fy : 0);
    g_creating = 1;   /* the WM_DPICHANGED sent while the window lands on its monitor must not rescale it */
    g_hwnd = CreateWindowExW(0, wc.lpszClassName, L"Tony Hawk's Underground (recomp)", WS_OVERLAPPEDWINDOW,
                             x, y, fw, fh, NULL, NULL, wc.hInstance, NULL);
    SetWindowPos(g_hwnd, NULL, x, y, fw, fh, SWP_NOZORDER | SWP_NOACTIVATE);
    g_creating = 0;
    if (dev_run) ShowWindow(g_hwnd, SW_SHOWNOACTIVATE);
    else { ShowWindow(g_hwnd, SW_SHOW); SetForegroundWindow(g_hwnd); }
    if (getenv("UG_FULLSCREEN") && *getenv("UG_FULLSCREEN") == '1') toggle_fullscreen(g_hwnd);
    SetEvent((HANDLE)ev);
    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    return 0;
}
#endif   /* Linux: linux/sdl_plat.c makes the window; sched_run pumps its events on the main thread */

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
static int shot_out;   /* UG_SHOTOUT=1: UG_SHOT saves the window's image, not the render target (debug) */
static VkImage sc_img[8];
static VkImageView sc_view[8];   /* for the touch overlay pass (the frame itself is blitted) */
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
    if (sc_ext.width == UINT32_MAX) {   /* Wayland: the surface takes the swapchain's size; use the window's */
        RECT cr;
        GetClientRect(g_hwnd, &cr);
        sc_ext.width = (uint32_t)(cr.right > 0 ? cr.right : 640);
        sc_ext.height = (uint32_t)(cr.bottom > 0 ? cr.bottom : 480);
    }
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
    shot_out = getenv("UG_SHOTOUT") && (caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    if (shot_out) ci.imageUsage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE; ci.preTransform = caps.currentTransform;
    sc_transform = caps.currentTransform;
    ci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR; ci.presentMode = pm; ci.clipped = VK_TRUE;
    ci.oldSwapchain = swapchain;
    VkSwapchainKHR sc;
    VKC(vkCreateSwapchainKHR(dev, &ci, 0, &sc));
    for (uint32_t i = 0; i < 8; i++) if (sc_view[i]) { vkDestroyImageView(dev, sc_view[i], 0); sc_view[i] = 0; }
    if (swapchain) vkDestroySwapchainKHR(dev, swapchain, 0);
    swapchain = sc;
    sc_count = 8;
    vkGetSwapchainImagesKHR(dev, swapchain, &sc_count, sc_img);
    for (uint32_t i = 0; i < sc_count; i++) {
        VkImageViewCreateInfo vci = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, 0, 0, sc_img[i], VK_IMAGE_VIEW_TYPE_2D, sc_fmt};
        vci.subresourceRange = (VkImageSubresourceRange){VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        if (vkCreateImageView(dev, &vci, 0, &sc_view[i]) != VK_SUCCESS) sc_view[i] = 0;
        if (!sc_done[i]) {
            VkSemaphoreCreateInfo si = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
            vkCreateSemaphore(dev, &si, 0, &sc_done[i]);
        }
    }
    LOG("[gpu] swapchain %ux%u, %u images, format %d, present mode %d, transform %X\n", sc_ext.width, sc_ext.height,
        sc_count, sc_fmt, pm, (unsigned)sc_transform);
    return 1;
}

/* ================================================================ surfaces */

typedef struct {
    uint32_t addr; int w, h, hw, hh;   /* active guest size (clip) and its host size */
    int aw, ah;                        /* allocated guest size: grows only; a smaller clip renders into the top-left */
    Img color;
    uint32_t last_frame;
} Surface;
typedef struct { uint32_t addr; int w, h; Img depth; VkImageView dview; } ZSurface;   /* w, h: allocated (grows only) */   /* dview: depth aspect only (shadow maps) */
static Surface rts[16]; static int n_rts;
static ZSurface zss[8]; static int n_zss;
static Surface *cur_rt;
static ZSurface *cur_zs;
static int rendering;                 /* inside vkCmdBeginRendering */
static Surface *r_rt; static ZSurface *r_zs;   /* what the active rendering pass targets */
static int r_hw, r_hh;                         /* and its render area */

static VkCommandBuffer CMD(void) { return frames[fi].cmd; }

static void end_rendering(void) {
    if (rendering) { vkCmdEndRendering(CMD()); rendering = 0; }
}

static void begin_rendering(void) {
    if (rendering && r_rt == cur_rt && r_zs == cur_zs && r_hw == cur_rt->hw && r_hh == cur_rt->hh) return;
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
    rendering = 1; r_rt = cur_rt; r_zs = cur_zs; r_hw = cur_rt->hw; r_hh = cur_rt->hh;
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
    if (s && w <= s->aw && h <= s->ah) {   /* clip changed within the allocation (cutscene letterbox 640x420/480) */
        s->w = w; s->h = h; s->hw = w * g_scale; s->hh = h * g_scale;
        return s;
    }
    end_rendering();
    if (s) { if (w < s->aw) w = s->aw; if (h < s->ah) h = s->ah; }
    if (!s) {
        if (n_rts == 16) {
            int o = 0;
            for (int i = 1; i < n_rts; i++) if (rts[i].last_frame < rts[o].last_frame) o = i;
            s = &rts[o];
        } else s = &rts[n_rts++];
    }
    img_destroy_later(&s->color);
    s->addr = addr; s->w = s->aw = w; s->h = s->ah = h; s->hw = w * g_scale; s->hh = h * g_scale;
    if (!img_create(&s->color, s->hw, s->hh, 1, VK_FORMAT_R8G8B8A8_UNORM,
                    VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                    VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_ASPECT_COLOR_BIT)) return NULL;
    clear_image(&s->color, VK_IMAGE_ASPECT_COLOR_BIT);
    LOG("[gpu] render target %08X %dx%d\n", addr, w, h);
    return s;
}

static ZSurface *get_zs(uint32_t addr, int w, int h) {
    for (int i = 0; i < n_zss; i++) if (zss[i].addr == addr && w <= zss[i].w && h <= zss[i].h && zss[i].depth.img) return &zss[i];
    end_rendering();
    ZSurface *z = NULL;
    for (int i = 0; i < n_zss; i++) if (zss[i].addr == addr) z = &zss[i];
    if (z) { if (w < z->w) w = z->w; if (h < z->h) h = z->h; }
    if (!z) z = n_zss < 8 ? &zss[n_zss++] : &zss[0];
    if (z->dview) { vkDeviceWaitIdle(dev); vkDestroyImageView(dev, z->dview, 0); z->dview = 0; }   /* rare: grows only */
    img_destroy_later(&z->depth);
    z->addr = addr; z->w = w; z->h = h;
    if (!img_create(&z->depth, w * g_scale, h * g_scale, 1, depth_fmt,
                    VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                    VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) return NULL;
    VkImageViewCreateInfo dvi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, 0, 0, z->depth.img, VK_IMAGE_VIEW_TYPE_2D, depth_fmt,
                                 {0}, {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1}};
    VKC(vkCreateImageView(dev, &dvi, 0, &z->dview));
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
       TF_ABGR, TF_BGRA, TF_RGBA, TF_DXT1, TF_DXT3, TF_DXT5, TF_DEPTH };
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
    case 0x2A: f.kind = TF_DEPTH; f.bpp = 4; break;                   /* depth X8_Y24 fixed (shadow maps) */
    case 0x2C: f.kind = TF_DEPTH; f.bpp = 2; break;                   /* depth Y16 fixed */
    case 0x2E: f.kind = TF_DEPTH; f.bpp = 4; f.swizzled = 0; break;
    case 0x30: f.kind = TF_DEPTH; f.bpp = 2; f.swizzled = 0; break;
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
/* DXT1/3/5 block -> 4x4 RGBA8 texels (texel() byte order: r | g << 8 | b << 16 | a << 24), for GPUs without BC */
static uint32_t rgb565(uint16_t v) { return c5(v >> 11) | c6((v >> 5) & 63) << 8 | c5(v & 31) << 16; }
static void dxt_block(int kind, const uint8_t *b, uint32_t out[16]) {
    const uint8_t *cb = kind == TF_DXT1 ? b : b + 8;
    uint16_t c0 = (uint16_t)(cb[0] | cb[1] << 8), c1 = (uint16_t)(cb[2] | cb[3] << 8);
    uint32_t p[4];
    p[0] = rgb565(c0) | 0xFF000000u;
    p[1] = rgb565(c1) | 0xFF000000u;
    uint32_t r0 = p[0] & 255, g0 = (p[0] >> 8) & 255, b0 = (p[0] >> 16) & 255;
    uint32_t r1 = p[1] & 255, g1 = (p[1] >> 8) & 255, b1 = (p[1] >> 16) & 255;
    if (kind != TF_DXT1 || c0 > c1) {
        p[2] = (2 * r0 + r1) / 3 | (2 * g0 + g1) / 3 << 8 | (2 * b0 + b1) / 3 << 16 | 0xFF000000u;
        p[3] = (r0 + 2 * r1) / 3 | (g0 + 2 * g1) / 3 << 8 | (b0 + 2 * b1) / 3 << 16 | 0xFF000000u;
    } else {
        p[2] = (r0 + r1) / 2 | (g0 + g1) / 2 << 8 | (b0 + b1) / 2 << 16 | 0xFF000000u;
        p[3] = 0;   /* transparent black */
    }
    uint32_t idx = (uint32_t)(cb[4] | cb[5] << 8 | cb[6] << 16 | (uint32_t)cb[7] << 24);
    for (int i = 0; i < 16; i++) out[i] = p[(idx >> (2 * i)) & 3];
    if (kind == TF_DXT3) {
        for (int i = 0; i < 16; i++) {
            uint32_t a = (b[i / 2] >> (4 * (i & 1))) & 15;
            out[i] = (out[i] & 0x00FFFFFFu) | (a * 17) << 24;
        }
    } else if (kind == TF_DXT5) {
        uint32_t a[8], a0 = b[0], a1 = b[1];
        a[0] = a0; a[1] = a1;
        if (a0 > a1) for (int k = 1; k < 7; k++) a[k + 1] = ((7 - k) * a0 + k * a1) / 7;
        else { for (int k = 1; k < 5; k++) a[k + 1] = ((5 - k) * a0 + k * a1) / 5; a[6] = 0; a[7] = 255; }
        uint64_t bits = 0;
        for (int k = 0; k < 6; k++) bits |= (uint64_t)b[2 + k] << (8 * k);
        for (int i = 0; i < 16; i++) out[i] = (out[i] & 0x00FFFFFFu) | a[(bits >> (3 * i)) & 7] << 24;
    }
}

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
    int hd;           /* HD pack texture: 1 + pack index once loaded, -(1 + index) while waiting for upload budget */
    Img im;
} TexEntry;
#define TEX_CACHE 4096
static TexEntry tcache[TEX_CACHE];
#define ADDR_SLOTS 4096
static TexEntry *addr_slot[ADDR_SLOTS];   /* latest entry per texture address (in-place re-upload) */
static Img white_tex;
static uint32_t st_draws, st_verts, st_newtex, st_worst, st_slow;   /* st_worst: 0.1 ms */

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
            if (has_bc) {
                uint8_t *p = ring_alloc(&fr->stage, sz, 16, &off);
                if (!p) return 0;
                memcpy(p, g_mem + (src & 0x03FFFFFF), sz);
            } else {   /* no BC on this GPU: decode to RGBA8 */
                uint32_t *out = ring_alloc(&fr->stage, (VkDeviceSize)lw * lh * 4, 16, &off), blk[16];
                if (!out) return 0;
                const uint8_t *base = g_mem + (src & 0x03FFFFFF);
                for (uint32_t by = 0; by < bh; by++)
                    for (uint32_t bx = 0; bx < bw; bx++) {
                        dxt_block(f.kind, base + (by * bw + bx) * bsz, blk);
                        for (int y = 0; y < 4 && (int)(by * 4) + y < lh; y++)
                            for (int x = 0; x < 4 && (int)(bx * 4) + x < lw; x++)
                                out[(by * 4 + y) * lw + bx * 4 + x] = blk[y * 4 + x];
                    }
            }
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
    VkFormat vf = dxt && !has_bc ? VK_FORMAT_R8G8B8A8_UNORM : f.kind == TF_DXT1 ? VK_FORMAT_BC1_RGBA_UNORM_BLOCK : f.kind == TF_DXT3 ? VK_FORMAT_BC2_UNORM_BLOCK :
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

/* ---- HD texture packs (hdtex.c): the guest texture's level 0 as RGBA8 is matched by picture; results are kept per
 * content hash, so a texture seen again (other level, re-upload) costs nothing ---- */
/* worker thread (hdtex.c): level 0 of a guest texture -> RGBA8 (R low), plus the unswizzled indices for I8 */
static void hd_decode(const uint8_t *raw, int kind, int bpp, int w, int h, const uint32_t *pal, uint32_t *rgba, uint8_t *idx) {
    if (kind >= TF_DXT1) {
        uint32_t bsz = kind == TF_DXT1 ? 8 : 16, bw = (uint32_t)w / 4, blk[16];
        for (int by = 0; by < h / 4; by++)
            for (int bx = 0; bx < w / 4; bx++) {
                dxt_block(kind, raw + (by * bw + bx) * bsz, blk);
                for (int k = 0; k < 16; k++) rgba[(by * 4 + k / 4) * w + bx * 4 + k % 4] = blk[k];
            }
        return;
    }
    uint32_t *xs = malloc(w * 4), *ys = malloc(h * 4);
    if (!xs || !ys) { free(xs); free(ys); memset(rgba, 0, (size_t)w * h * 4); return; }
    swizzle_tables(w, h, xs, ys);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            if (idx) {   /* I8 with the guest palette (A8R8G8B8) */
                uint8_t v = raw[xs[x] | ys[y]];
                uint32_t c = pal[v];
                idx[y * w + x] = v;
                rgba[y * w + x] = (c >> 16 & 255) | (c & 0xFF00) | (c & 255) << 16 | (c & 0xFF000000u);
            } else rgba[y * w + x] = texel(kind, raw + (xs[x] | ys[y]) * bpp, NULL);
        }
    free(xs); free(ys);
}
typedef struct { uint64_t hash; int idx; } HdMemo;   /* idx: pack index, -1 no match; hash 0 = empty slot */
static HdMemo hd_memo[1 << 14];
#define HD_WAIT (-2)   /* hd_match: still being matched */
static int hd_match(const TexEntry *e, TexFmt f, uint64_t hsh) {
    int i8 = f.kind == TF_I8;   /* palettized: the exact PS2 hashes (hdtex_find_indexed); others: by picture */
    if (!hdtex_enabled() || (!f.swizzled && f.kind < TF_DXT1) || e->w > 4096 || e->h > 4096 ||
        (i8 ? (e->w < 4 || e->h < 4 || !e->pal_len) : (e->w < 32 || e->h < 32)))
        return -1;
    uint32_t s = (uint32_t)(hsh ^ hsh >> 29) & ((1 << 14) - 1);
    for (int p = 0; p < 16; p++) {
        HdMemo *m = &hd_memo[(s + p) & ((1 << 14) - 1)];
        if (m->hash == hsh) return m->idx;
        if (!m->hash) break;
    }
    int r = hdtex_result(hsh);   /* the match runs on hdtex.c's worker thread */
    if (r >= -1) {
        for (int p = 0; p < 16; p++) {
            HdMemo *m = &hd_memo[(s + p) & ((1 << 14) - 1)];
            if (!m->hash || p == 15) { m->hash = hsh; m->idx = r; break; }
        }
        return r;
    }
    if (r == -2) return HD_WAIT;
    /* copy level 0 (a memcpy) and let the worker decode and match it */
    int w = e->w, h = e->h;
    size_t n = f.kind >= TF_DXT1 ? (size_t)(w / 4) * (h / 4) * (f.kind == TF_DXT1 ? 8 : 16) : (size_t)w * h * f.bpp;
    uint8_t *raw = malloc(n);
    if (!raw) return -1;
    memcpy(raw, g_mem + (e->addr & 0x03FFFFFF), n);
    uint32_t pal[256] = {0};
    if (i8) for (uint32_t k = 0; k < e->pal_len && k < 256; k++) pal[k] = RD32(e->pal_addr + k * 4);
    hdtex_submit_raw(hsh, raw, f.kind, f.bpp, i8, w, h, i8 ? pal : NULL, i8 ? e->pal_len : 0, hd_decode);
    return HD_WAIT;
}

/* Replaces e's image with pack texture idx (all its levels down to 4x4). 0 = not now (this frame's HD upload budget
 * is spent, or the file is unusable): the caller keeps the original texture. */
static uint32_t hd_budget_frame, hd_budget_bytes;
static uint64_t hd_vram;   /* bytes of HD images created (budget: UG_HDVRAM MB, default 2048) */
/* HD images, one per pack texture, shared by every cache entry that shows it (a texture seen again costs nothing);
 * freed after 30 s unused. Slots keep their key (no deletion), so the table only grows to the pack textures used. */
typedef struct { int key; Img im; uint32_t bytes, last; } HdImg;   /* key = pack index + 1, 0 = empty */
#define HD_CACHE (1 << 14)
static HdImg hd_imgs[HD_CACHE];
static HdImg *hd_img(int idx, int add) {
    uint32_t i = (uint32_t)idx * 2654435761u & (HD_CACHE - 1);
    for (int n = 0; n < HD_CACHE; n++, i = (i + 1) & (HD_CACHE - 1)) {
        if (hd_imgs[i].key == idx + 1) return &hd_imgs[i];
        if (!hd_imgs[i].key) {
            if (!add) return NULL;
            hd_imgs[i].key = idx + 1;
            return &hd_imgs[i];
        }
    }
    return NULL;
}
static int create_hd(TexEntry *e, int idx) {
    HdImg *hc = hd_img(idx, 1);
    if (!hc) return 0;
    if (hc->im.img) { hc->last = frame_no; e->hd = 1 + idx; return 1; }   /* already on the GPU */
    int w, h, levels;
    uint32_t fourcc;
    if (!hdtex_info(idx, &w, &h, &levels, &fourcc)) return 0;
    int kind = fourcc == 0x31545844u ? TF_DXT1 : fourcc == 0x33545844u ? TF_DXT3 : TF_DXT5;
    uint32_t bsz = kind == TF_DXT1 ? 8 : 16;
    uint64_t total = 0;
    for (int l = 0; l < levels && l < 16; l++) {
        int lw = w >> l, lh = h >> l;
        if (lw < 4 || lh < 4) break;
        total += (uint64_t)(lw / 4) * (lh / 4) * (has_bc ? bsz : 64);
    }
    static uint64_t vram_cap;
    if (!vram_cap) { const char *v = getenv("UG_HDVRAM"); vram_cap = (uint64_t)(v ? atoi(v) : 2048) << 20; }
    if (hd_vram + total > vram_cap) return 0;
    if (hd_budget_frame != frame_no) { hd_budget_frame = frame_no; hd_budget_bytes = 0; }
    if (hd_budget_bytes && hd_budget_bytes + total > (4u << 20)) return 0;   /* rest on later frames: no hitch */
    size_t len;
    uint8_t *file;
    if (!hdtex_file(idx, &file, &len)) return 0;   /* read ahead on the worker thread; ask again next frame */
    Frame *fr = &frames[fi];
    VkBufferImageCopy regions[16];
    int nl = 0;
    size_t src = 128;
    for (int l = 0; l < levels && l < 16; l++) {
        int lw = w >> l, lh = h >> l;
        if (lw < 4 || lh < 4) break;
        uint32_t bw = (uint32_t)lw / 4, bh = (uint32_t)lh / 4, sz = bw * bh * bsz;
        if (src + sz > len) break;
        if (kind == TF_DXT5)   /* PS2 packs keep GS alpha (0x80 = opaque): double the endpoints of each alpha block */
            for (uint32_t k = 0; k < bw * bh; k++) {
                uint8_t *b = file + src + k * 16;
                int a0 = b[0] * 2, a1 = b[1] * 2, eight = b[0] > b[1];
                if (a0 > 255) a0 = 255;
                if (a1 > 255) a1 = 255;
                if (eight && a0 <= a1) a1 = a0 - 1;   /* keep the 8-value mode (6-value mode codes 6/7 as 0/255) */
                b[0] = (uint8_t)a0; b[1] = (uint8_t)a1;
            }
        VkDeviceSize off;
        if (has_bc) {
            uint8_t *p = ring_alloc(&fr->stage, sz, 16, &off);
            if (!p) break;
            memcpy(p, file + src, sz);
        } else {
            uint32_t *out = ring_alloc(&fr->stage, (VkDeviceSize)lw * lh * 4, 16, &off), blk[16];
            if (!out) break;
            for (uint32_t by = 0; by < bh; by++)
                for (uint32_t bx = 0; bx < bw; bx++) {
                    dxt_block(kind, file + src + (by * bw + bx) * bsz, blk);
                    for (int k = 0; k < 16; k++) out[(by * 4 + k / 4) * lw + bx * 4 + k % 4] = blk[k];
                }
        }
        regions[nl] = (VkBufferImageCopy){off, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, (uint32_t)l, 0, 1}, {0, 0, 0},
                                          {(uint32_t)lw, (uint32_t)lh, 1}};
        nl++;
        src += sz;
    }
    free(file);
    if (!nl) return 0;
    VkFormat vf = !has_bc ? VK_FORMAT_R8G8B8A8_UNORM : kind == TF_DXT1 ? VK_FORMAT_BC1_RGBA_UNORM_BLOCK :
                  kind == TF_DXT3 ? VK_FORMAT_BC2_UNORM_BLOCK : VK_FORMAT_BC3_UNORM_BLOCK;
    Img im = {0};
    if (!img_create(&im, w, h, nl, vf, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                    VK_IMAGE_ASPECT_COLOR_BIT)) return 0;
    barrier(fr->up, &im, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    vkCmdCopyBufferToImage(fr->up, fr->stage.buf, im.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, nl, regions);
    barrier(fr->up, &im, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    fr->up_used = 1;
    hc->im = im;
    hc->bytes = (uint32_t)total;
    hc->last = frame_no;
    e->hd = 1 + idx;
    hd_vram += total;
    hd_budget_bytes += (uint32_t)total;
    static int n_hd;
    if (++n_hd % 100 == 1) LOG("[hdtex] %d HD textures loaded (%.0f MB)\n", n_hd, hd_vram / 1048576.0);
    return 1;
}
/* F10 toggles the HD pack on and off; UG_SHOTAB=1 renders the frame after each UG_SHOT frame without it */
volatile int g_hd_hidden;
static int hd_shot_ab = -1, shot_every = -1;
static VkImageView tex_view(TexEntry *e) {
    if (hd_shot_ab < 0) { const char *a = getenv("UG_SHOTAB"); hd_shot_ab = a && *a == '1'; }
    /* frame_no counts flips: while frame N+1 is drawn, frame_no is still N */
    int off = g_hd_hidden || (hd_shot_ab && shot_every > 0 && frame_no % (uint32_t)shot_every == 0);
    if (e->hd > 0 && !off) {
        HdImg *hc = hd_img(e->hd - 1, 0);
        if (hc && hc->im.img) { hc->last = frame_no; return hc->im.view; }
        e->hd = -e->hd;   /* freed while unused: loaded again on a later frame */
    }
    return e->im.view;
}

/* stage i -> image view (or 0); *rect = linear texture (unnormalized coordinates) */
static ZSurface *find_zs(uint32_t addr) {
    for (int i = 0; i < n_zss; i++) if (zss[i].addr == addr && zss[i].depth.img) return &zss[i];
    return NULL;
}

/* stage i -> image view (or 0); *rect = linear texture (unnormalized coordinates); *zmax > 0: depth texture */
static VkImageView get_texture(int i, int *rect, int *tw, int *th, float *zmax) {
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
    if (f.swizzled || (f.kind >= TF_DXT1 && f.kind <= TF_DXT5)) {
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
    if (f.kind == TF_DEPTH) {   /* shadow map: sample the host depth surface at this address */
        ZSurface *z = find_zs(addr);
        {
            static uint32_t seen[8]; static int ns;
            int k = 0;
            while (k < ns && seen[k] != addr) k++;
            if (k == ns && ns < 8) { seen[ns++] = addr; LOG("[gpu] depth texture %08X -> %s\n", addr, z ? "host depth surface" : "none"); }
        }
        if (!z || z == cur_zs) {
            static int warned;
            if (!warned++) LOG("[gpu] depth texture %08X has no host depth surface (or is bound)\n", addr);
            return 0;
        }
        end_rendering();
        barrier(CMD(), &z->depth, VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        *zmax = f.bpp == 2 ? 65535.0f : 16777215.0f;
        return z->dview;
    }
    /* render-to-texture: sample the host surface (not while it is the bound target) */
    Surface *s = find_rt(addr);
    if (s) {
        if (s == cur_rt) return 0;
        if (*rect) { *tw = s->aw; *th = s->ah; }   /* texel coordinates over the whole (possibly larger) allocation */
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
            if (e->hd == INT_MIN) {   /* still being matched */
                int r = hd_match(e, f, hsh);
                e->hd = r >= 0 ? -(1 + r) : r == HD_WAIT ? INT_MIN : 0;
            }
            if (e->hd < 0 && e->hd != INT_MIN) create_hd(e, -e->hd - 1);   /* file read / upload budget */
            return tex_view(e);
        }
    }
    /* same texture slot rewritten (movie frames, render-to-memory): re-upload into the previous image, unless that
     * image was already sampled this frame (the upload command buffer runs before this frame's draws) */
    uint32_t aslot = ((addr * 0x9E3779B1u) ^ (fmtw * 0x85EBCA77u)) >> 20 & (ADDR_SLOTS - 1);
    TexEntry *old_e = addr_slot[aslot];
    if (old_e && old_e->im.img && old_e->addr == addr && old_e->fmt == fmtw && old_e->pitch == pitch && old_e->w == w &&
        old_e->h == h && old_e->pal_addr == pal_addr && old_e->pal_len == pal_len && old_e->last_frame != frame_no &&
        old_e->levels == levels && !old_e->hd) {   /* an HD image has another size: take the new-entry path */
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
    v->hd = 0;
    {   /* UG_TEXDUMP=<format word, hex>: write each new texture of that format to out/tex_<addr>_<fmt>.bin (raw guest
         * bytes, all levels; debug) */
        static int td = -1;
        static uint32_t td_fmt;
        if (td < 0) { const char *e = getenv("UG_TEXDUMP"); td = e != NULL; if (e) td_fmt = (uint32_t)strtoul(e, NULL, 16); }
        if (td && (td_fmt <= 0xFFFFu ? (fmtw & 0xFFFFu) == td_fmt : fmtw == td_fmt)) {   /* <= 4 digits: low bits */
            char fn[64];
            snprintf(fn, sizeof fn, "out/tex_%08X_%08X.bin", addr, fmtw);
            FILE *fo = fopen(fn, "wb");
            if (fo) { fwrite(g_mem + addr, 1, nbytes, fo); fclose(fo); }
        }
    }
    if (pal_len)
        for (uint32_t k = 0; k < 256; k++) {
            uint32_t c = k < pal_len ? RD32(pal_addr + k * 4) : 0;   /* A8R8G8B8 */
            palette[k] = ((c >> 16) & 0xFF) | (c & 0xFF00) | ((c & 0xFF) << 16) | (c & 0xFF000000u);
        }
    v->addr = addr; v->fmt = fmtw; v->pitch = pitch; v->w = w; v->h = h; v->pal_addr = pal_addr; v->pal_len = pal_len;
    v->hash = hsh; v->last_frame = frame_no; v->levels = levels;
    v->hd = 0;
    if (!create_texture(v, f, levels, palette)) return 0;
    addr_slot[aslot] = v;
    int hdi = hd_match(v, f, hsh);   /* HD pack (UG_HDTEX): replaces the image now, or on a later frame */
    if (hdi == HD_WAIT) v->hd = INT_MIN;
    else if (hdi >= 0 && !create_hd(v, hdi)) v->hd = -(1 + hdi);
    return tex_view(v);
}

/* ---- samplers ---- */
typedef struct { uint32_t address, filter, border; VkSampler s; } SamplerEntry;
static SamplerEntry samplers[512]; static int n_samplers;
static VkSampler blit_sampler, depth_sampler;

static VkSamplerAddressMode addr_mode(uint32_t m) {
    switch (m) {
    case 1: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
    case 2: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
    case 4: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    default: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    }
}

static VkSampler get_sampler(int i, int rect) {
    uint32_t a = KREG(K_TEX_ADDRESS(i)), f = (KREG(K_TEX_FILTER(i)) & 0x0FFF0000u) | (uint32_t)g_aniso << 8 | (uint32_t)rect, b = KREG(K_TEX_BORDER(i));
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
    if (g_aniso > 0 && mips && min_lin && !rect) {
        ci.anisotropyEnable = VK_TRUE;
        ci.maxAnisotropy = g_aniso < pprops.limits.maxSamplerAnisotropy ? g_aniso : pprops.limits.maxSamplerAnisotropy;
    }
    ci.borderColor = (b >> 24) < 0x80 ? VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK :
                     (b & 0xFFFFFF) > 0x808080 ? VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE : VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
    VkSampler s = 0;
    vkCreateSampler(dev, &ci, 0, &s);
    if (n_samplers < 512) { samplers[n_samplers].address = a; samplers[n_samplers].filter = f; samplers[n_samplers].border = b; samplers[n_samplers++].s = s; }
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
    rs.polygonMode = VK_POLYGON_MODE_FILL; rs.lineWidth = 1.0f; rs.depthClampEnable = has_depth_clamp;
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
                            VK_DYNAMIC_STATE_STENCIL_REFERENCE, VK_DYNAMIC_STATE_BLEND_CONSTANTS,
                            VK_DYNAMIC_STATE_DEPTH_BIAS, VK_DYNAMIC_STATE_DEPTH_BIAS_ENABLE};
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
    float mv[4][4], imv[4][4];   /* fixed function: model-view 0 and its inverse (texgen) */
    uint32_t tgen[4];            /* texgen per stage, 8 bits per s/t/r/q: 0 off, 4 normal map, 5 reflection map */
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
#ifdef _WIN32
    if (volkInitialize() != VK_SUCCESS) { LOG("[gpu] no Vulkan loader (vulkan-1.dll): install a GPU driver with Vulkan\n"); return 0; }
    const char *exts[] = {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_WIN32_SURFACE_EXTENSION_NAME};
    uint32_t n_exts = 2;
#else
    if (volkInitialize() != VK_SUCCESS) { LOG("[gpu] no Vulkan loader (libvulkan.so.1): install your GPU's Vulkan driver\n"); return 0; }
    Uint32 n_exts = 0;
    const char *const *exts = SDL_Vulkan_GetInstanceExtensions(&n_exts);
    if (!exts) { LOG("[gpu] SDL_Vulkan_GetInstanceExtensions: %s\n", SDL_GetError()); return 0; }
#endif
    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO, 0, "Project Underground", 1, "underground", 1, VK_API_VERSION_1_3};
    VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, 0, 0, &app, 0, 0, n_exts, exts};
    const char *layer = "VK_LAYER_KHRONOS_validation";
    if (getenv("UG_VKDEBUG")) { ici.enabledLayerCount = 1; ici.ppEnabledLayerNames = &layer; }
    VKC(vkCreateInstance(&ici, 0, &inst));
    volkLoadInstance(inst);
#ifdef _WIN32
    VkWin32SurfaceCreateInfoKHR sci = {VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR, 0, 0, GetModuleHandleW(NULL), g_hwnd};
    VKC(vkCreateWin32SurfaceKHR(inst, &sci, 0, &surface));
#else
    if (!SDL_Vulkan_CreateSurface((SDL_Window *)g_hwnd, inst, NULL, &surface)) {
        LOG("[gpu] SDL_Vulkan_CreateSurface: %s\n", SDL_GetError());
        return 0;
    }
#endif
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
    {   /* NV2A does not clip against the depth range (xemu enables depth clamp too) */
        VkPhysicalDeviceFeatures have;
        vkGetPhysicalDeviceFeatures(pdev, &have);
        f2.features.depthClamp = have.depthClamp;
        has_depth_clamp = have.depthClamp;
        /* anisotropic filtering (UG_ANISO=1..16, default 16; 0/1 = off): ground and walls seen at an angle keep their
         * detail instead of falling to small mips (the NV2A had none; matters most with UG_SCALE > 1 and HD packs) */
        const char *an = getenv("UG_ANISO");
        g_aniso = an ? (float)atof(an) : 16.0f;
        has_aniso = have.samplerAnisotropy;
        if (!has_aniso || g_aniso < 1.5f) g_aniso = 0;
        f2.features.samplerAnisotropy = have.samplerAnisotropy;
        const char *nobc = getenv("UG_NOBC");
        has_bc = have.textureCompressionBC && !(nobc && *nobc == '1');
        f2.features.textureCompressionBC = (VkBool32)has_bc;
        if (!has_bc) LOG("[gpu] no BC texture compression: DXT textures are decoded on the CPU\n");
    }
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, 0, 0, qfam, 1, &prio};
    const char *dexts[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &f2, 0, 1, &qci, 0, 0, 1, dexts, 0};
    VKC(vkCreateDevice(pdev, &dci, 0, &dev));
    volkLoadDevice(dev);
    vkGetDeviceQueue(dev, qfam, 0, &queue);
    /* depth: D24S8 is not on every GPU (AMD, many Mali); D32S8 is on most. Shadow maps also sample it. */
    {
        const VkFormatFeatureFlags need = VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
        const VkFormat cand[2] = {VK_FORMAT_D24_UNORM_S8_UINT, VK_FORMAT_D32_SFLOAT_S8_UINT};
        depth_fmt = VK_FORMAT_UNDEFINED;
        for (int i = 0; i < 2 && depth_fmt == VK_FORMAT_UNDEFINED; i++) {
            VkFormatProperties fp;
            vkGetPhysicalDeviceFormatProperties(pdev, cand[i], &fp);
            if ((fp.optimalTilingFeatures & need) == need) depth_fmt = cand[i];
        }
        if (depth_fmt == VK_FORMAT_UNDEFINED) {
            LOG("[gpu] no sampleable depth-stencil format (D24S8 or D32S8): shadows may be wrong\n");
            depth_fmt = VK_FORMAT_D32_SFLOAT_S8_UINT;
        }
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
    VKC(vkCreateSampler(dev, &sci2, 0, &depth_sampler));   /* nearest: the shadow compare runs in the shader */
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

volatile int g_fxaa, g_ao;      /* UG_FXAA, UG_AO: also the in-game menu (menu.c) */
uint32_t gpu_frame(void) { return frame_no; }
int gpu_has_aniso(void) { return has_aniso; }

void gpu_init(void) {
    if (gpu_ok) return;
    gpu_ok = -1;
    if (getenv("UG_HEADLESS") && *getenv("UG_HEADLESS") == '1') { LOG("[gpu] headless: no window, nothing drawn\n"); return; }
    g_fxaa = getenv("UG_FXAA") && *getenv("UG_FXAA") == '1';
    g_ao = getenv("UG_AO") && *getenv("UG_AO") == '1';
    if (getenv("UG_SCALE")) { g_scale = atoi(getenv("UG_SCALE")); if (g_scale < 1) g_scale = 1; if (g_scale > 8) g_scale = 8; }
#ifdef _WIN32
    HANDLE ev = CreateEventW(NULL, TRUE, FALSE, NULL);
    CreateThread(NULL, 0, window_thread, ev, 0, NULL);
    WaitForSingleObject(ev, INFINITE);
    CloseHandle(ev);
#else
    g_hwnd = linux_create_window();
    if (!g_hwnd) return;
#endif
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

/* ================================================================ world post-processing (UG_FXAA, UG_AO)
 * One pass over the 3D scene, before the HUD is drawn so that text stays sharp. The HUD is what the game draws from
 * pre-transformed vertices (vertex program, z = w = 0, no depth write). A frame without a HUD (some cutscenes) gets
 * its pass at the flip. The depth range and focal length come from the composite matrix of the frame's own
 * perspective draws, so any camera works. UG_FXDEBUG=1 shows the occlusion alone, 2 logs the projections. */
typedef struct { float proj[4], zp[4], rt[4], opt[4]; } FxUBO;
static Surface *fx_rt;          /* world target of this frame, NULL until a 3D draw */
static ZSurface *fx_zs;
static float fx_proj[6];        /* fx, fy, cx, cy (guest pixels), depth = [4] + [5] / w */
static int fx_proj_ok, fx_done, fx_hud, fx_failed;
static VkDescriptorSetLayout fx_dsl;
static VkPipelineLayout fx_pl;
static VkPipeline fx_pipe_ao, fx_pipe_post;
static VkSampler fx_linear;
static Img fx_color, fx_ao;
static int fx_w, fx_h, fx_aw, fx_ah;

static int fx_debug(void) {
    static int d = -1;
    if (d < 0) { const char *e = getenv("UG_FXDEBUG"); d = e ? atoi(e) : 0; }
    return d;
}

static VkPipeline fx_pipeline(VkFormat fmt, const uint32_t *code, size_t size) {
    VkShaderModule vs = 0, fs = 0;
    VkPipeline p = 0;
    VkShaderModuleCreateInfo smi = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, 0, 0, sizeof spv_touch_vert, spv_touch_vert};
    vkCreateShaderModule(dev, &smi, 0, &vs);
    smi.codeSize = size; smi.pCode = code;
    vkCreateShaderModule(dev, &smi, 0, &fs);
    VkPipelineShaderStageCreateInfo st[2] = {
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, 0, 0, VK_SHADER_STAGE_VERTEX_BIT, vs, "main"},
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, 0, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fs, "main"}};
    VkPipelineVertexInputStateCreateInfo vi = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, 0, 0, 1, 0, 1, 0};
    VkPipelineRasterizationStateCreateInfo rs = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL; rs.lineWidth = 1.0f; rs.cullMode = VK_CULL_MODE_NONE;
    VkPipelineMultisampleStateCreateInfo ms = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo ds = {VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    VkPipelineColorBlendAttachmentState ba = {0};
    ba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo cb = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = 1; cb.pAttachments = &ba;
    VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dy = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, 0, 0, 2, dyn};
    VkPipelineRenderingCreateInfo pr = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    pr.colorAttachmentCount = 1; pr.pColorAttachmentFormats = &fmt;
    VkGraphicsPipelineCreateInfo ci = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, &pr};
    ci.stageCount = 2; ci.pStages = st; ci.pVertexInputState = &vi; ci.pInputAssemblyState = &ia; ci.pViewportState = &vp;
    ci.pRasterizationState = &rs; ci.pMultisampleState = &ms; ci.pDepthStencilState = &ds; ci.pColorBlendState = &cb;
    ci.pDynamicState = &dy; ci.layout = fx_pl;
    if (!vs || !fs || vkCreateGraphicsPipelines(dev, 0, 1, &ci, 0, &p) != VK_SUCCESS) p = 0;
    if (vs) vkDestroyShaderModule(dev, vs, 0);
    if (fs) vkDestroyShaderModule(dev, fs, 0);
    return p;
}

static int fx_init(void) {
    VkDescriptorSetLayoutBinding b[4] = {
        {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1, VK_SHADER_STAGE_FRAGMENT_BIT, 0},
        {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, 0},
        {2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, 0},
        {3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, 0}};
    VkDescriptorSetLayoutCreateInfo dli = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, 0, 0, 4, b};
    VKC(vkCreateDescriptorSetLayout(dev, &dli, 0, &fx_dsl));
    VkPipelineLayoutCreateInfo pli = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, 0, 0, 1, &fx_dsl};
    VKC(vkCreatePipelineLayout(dev, &pli, 0, &fx_pl));
    VkSamplerCreateInfo sci = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sci.magFilter = sci.minFilter = VK_FILTER_LINEAR;
    sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    VKC(vkCreateSampler(dev, &sci, 0, &fx_linear));
    fx_pipe_ao = fx_pipeline(VK_FORMAT_R8_UNORM, spv_ao_frag, sizeof spv_ao_frag);
    fx_pipe_post = fx_pipeline(VK_FORMAT_R8G8B8A8_UNORM, spv_post_frag, sizeof spv_post_frag);
    return fx_pipe_ao && fx_pipe_post;
}

/* full-screen triangle into view (w x h); t = depth, color, ao */
static int fx_pass(VkPipeline p, VkImageView view, int w, int h, VkDeviceSize uoff, const VkDescriptorImageInfo t[3]) {
    Frame *f = &frames[fi];
    VkDescriptorSet set;
    VkDescriptorSetAllocateInfo dai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, 0, f->dpool, 1, &fx_dsl};
    if (vkAllocateDescriptorSets(dev, &dai, &set) != VK_SUCCESS) return 0;
    VkDescriptorBufferInfo ub = {f->ubo.buf, 0, sizeof(FxUBO)};
    VkWriteDescriptorSet wr[4];
    memset(wr, 0, sizeof wr);
    for (int i = 0; i < 4; i++) {
        wr[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; wr[i].dstSet = set; wr[i].dstBinding = (uint32_t)i; wr[i].descriptorCount = 1;
        wr[i].descriptorType = i ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
        if (i) wr[i].pImageInfo = &t[i - 1]; else wr[i].pBufferInfo = &ub;
    }
    vkUpdateDescriptorSets(dev, 4, wr, 0, 0);
    VkCommandBuffer cb = CMD();
    VkRenderingAttachmentInfo ca = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    ca.imageView = view; ca.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    ca.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD; ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo ri = {VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea = (VkRect2D){{0, 0}, {(uint32_t)w, (uint32_t)h}};
    ri.layerCount = 1; ri.colorAttachmentCount = 1; ri.pColorAttachments = &ca;
    vkCmdBeginRendering(cb, &ri);
    VkViewport vpt = {0, 0, (float)w, (float)h, 0, 1};
    vkCmdSetViewport(cb, 0, 1, &vpt);
    vkCmdSetScissor(cb, 0, 1, &ri.renderArea);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
    uint32_t dof = (uint32_t)uoff;
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, fx_pl, 0, 1, &set, 1, &dof);
    vkCmdDraw(cb, 3, 1, 0, 0);
    vkCmdEndRendering(cb);
    return 1;
}

static void fx_apply(void) {
    Surface *s = fx_rt;
    ZSurface *z = fx_zs;
    fx_done = 1;
    /* UG_FXAB=1: off in odd frames, so shots N and N + 1 compare the two (debug) */
    static int ab = -1;
    if (ab < 0) ab = getenv("UG_FXAB") != NULL;
    if (fx_failed || (ab && (frame_no & 1))) return;
    int fxaa = g_fxaa != 0, ao = g_ao != 0 && fx_proj_ok && z && z->dview;
    if (!fxaa && !ao) return;
    if (!fx_pl && !fx_init()) { fx_failed = 1; LOG("[gpu] post-processing pipelines failed: off\n"); return; }
    Frame *f = &frames[fi];
    int d = g_scale > 2 ? g_scale / 2 : 1;   /* occlusion at about 960 lines whatever the render scale */
    int w = s->aw * g_scale, h = s->ah * g_scale, aw = (w + d - 1) / d, ah = (h + d - 1) / d;
    end_rendering();
    if (!fx_color.img || fx_w != w || fx_h != h) {
        img_destroy_later(&fx_color);
        if (!img_create(&fx_color, (uint32_t)w, (uint32_t)h, 1, VK_FORMAT_R8G8B8A8_UNORM,
                        VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_ASPECT_COLOR_BIT)) { fx_failed = 1; return; }
        fx_w = w; fx_h = h;
    }
    if (ao && (!fx_ao.img || fx_aw != aw || fx_ah != ah)) {
        img_destroy_later(&fx_ao);
        if (!img_create(&fx_ao, (uint32_t)aw, (uint32_t)ah, 1, VK_FORMAT_R8_UNORM,
                        VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, VK_IMAGE_ASPECT_COLOR_BIT)) { fx_failed = 1; return; }
        fx_aw = aw; fx_ah = ah;
    }
    VkDeviceSize uoff;
    FxUBO *u = ring_alloc(&f->ubo, sizeof(FxUBO), pprops.limits.minUniformBufferOffsetAlignment, &uoff);
    if (!u) return;
    static float radius = -1, strength = -1;   /* tuning (debug): UG_AORADIUS world units, UG_AOSTRENGTH */
    if (radius < 0) {
        const char *r = getenv("UG_AORADIUS"), *st = getenv("UG_AOSTRENGTH");
        radius = r ? (float)atof(r) : 30.0f;
        strength = st ? (float)atof(st) : 3.0f;
    }
    for (int i = 0; i < 4; i++) u->proj[i] = fx_proj[i] * (float)g_scale;
    u->zp[0] = fx_proj[4]; u->zp[1] = fx_proj[5]; u->zp[2] = radius; u->zp[3] = strength;
    u->rt[0] = (float)s->hw; u->rt[1] = (float)s->hh; u->rt[2] = 1.0f / (float)w; u->rt[3] = 1.0f / (float)h;
    u->opt[0] = (float)d; u->opt[1] = (float)fxaa; u->opt[2] = (float)ao; u->opt[3] = fx_debug() == 1;

    VkCommandBuffer cb = CMD();
    barrier(cb, &s->color, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    barrier(cb, &fx_color, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkImageCopy cp = {{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0},
                      {(uint32_t)s->hw, (uint32_t)s->hh, 1}};
    vkCmdCopyImage(cb, s->color.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, fx_color.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &cp);
    barrier(cb, &fx_color, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    barrier(cb, &s->color, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    VkDescriptorImageInfo white = {blit_sampler, white_tex.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkDescriptorImageInfo t[3] = {white, white, white};
    if (ao) {
        barrier(cb, &z->depth, VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        barrier(cb, &fx_ao, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        t[0] = (VkDescriptorImageInfo){depth_sampler, z->dview, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        if (!fx_pass(fx_pipe_ao, fx_ao.view, (s->hw + d - 1) / d, (s->hh + d - 1) / d, uoff, t)) return;
        barrier(cb, &fx_ao, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        t[2] = (VkDescriptorImageInfo){blit_sampler, fx_ao.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    }
    t[1] = (VkDescriptorImageInfo){fx_linear, fx_color.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    fx_pass(fx_pipe_post, s->color.view, s->hw, s->hh, uoff, t);
}

/* called for each draw with its targets selected */
static void fx_watch(const KVertex *v) {
    if ((!g_fxaa && !g_ao) || fx_done || !cur_zs || cur_rt->w < 320) return;   /* not the shadow maps (256x256) */
    int vprog = (KREG(K_TRANSFORM_MODE) & 3) == 2, zwrite = KREG(K_DEPTH_MASK) & 1;
    if (vprog && !zwrite && v[0].a[0][2] == 0 && v[0].a[0][3] == 0) {   /* HUD */
        if (fx_rt == cur_rt) fx_apply(); else fx_hud = 1;
        return;
    }
    if (!zwrite) return;
    if (vprog) {
        if (v[0].a[0][3] != 0) { fx_rt = cur_rt; fx_zs = cur_zs; }
        return;
    }
    /* composite rows: x, y = screen (guest pixels) times w, z = depth (Z-buffer units) times w, w = view distance */
    double r[4][4], rr = 0, dz = 0, dx = 0, dy = 0, fx = 0, fy = 0;
    for (int i = 0; i < 16; i++) r[i / 4][i % 4] = kf(K_COMPOSITE + i * 4);
    for (int j = 0; j < 3; j++) { rr += r[3][j] * r[3][j]; dz += r[2][j] * r[3][j]; dx += r[0][j] * r[3][j]; dy += r[1][j] * r[3][j]; }
    if (rr < 1e-12) return;   /* not a perspective matrix */
    fx_rt = cur_rt; fx_zs = cur_zs;
    for (int j = 0; j < 3; j++) {
        double a = r[0][j] - dx / rr * r[3][j], b = r[1][j] - dy / rr * r[3][j];
        fx += a * a; fy += b * b;
    }
    double zmax = ((KREG(K_SURF_FORMAT) >> 4) & 0xF) == 1 ? 65535.0 : 16777215.0;
    /* a scaled object scales all rows alike (w too): the ratios are those of the camera */
    double len = sqrt(rr), p[6] = {sqrt(fx) / len, sqrt(fy) / len, dx / rr + kf(K_VIEWPORT_OFFSET), dy / rr + kf(K_VIEWPORT_OFFSET + 4),
                                   dz / rr / zmax, (r[2][3] - dz / rr * r[3][3]) / zmax};
    if (fx_debug() == 2 && frame_no % 300 == 0)
        LOG("[fx] f%u rt %08X %dx%d focal %.1f %.1f centre %.1f %.1f depth %.7f %+.4f / w, far %.0f\n", frame_no, cur_rt->addr, cur_rt->w,
            cur_rt->h, p[0], p[1], p[2], p[3], p[4], p[5], p[4] != 1 ? p[5] / (1 - p[4]) : 0.0);
    if (!(p[5] < 0) || !(p[4] > 0.5) || !(p[0] > 1) || !(p[1] > 1)) return;
    for (int i = 0; i < 6; i++) fx_proj[i] = (float)p[i];
    fx_proj_ok = 1;
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
        if (skipm >= 0 && ((frame_no + 150) / 300 & 1) && (int)(KREG(K_TRANSFORM_MODE) & 3) == skipm) return;
        /* UG_SKIPTEXMAT=1: same windows, drop draws with any texture matrix enabled (projected textures) */
        static int skipt = -1;
        if (skipt < 0) skipt = getenv("UG_SKIPTEXMAT") != NULL;
        if (skipt && ((frame_no + 150) / 300 & 1) && (KREG(K_TEXMAT_EN) & 1 || KREG(K_TEXMAT_EN + 4) & 1 ||
                                                       KREG(K_TEXMAT_EN + 8) & 1 || KREG(K_TEXMAT_EN + 12) & 1)) return;
    }
    if (!select_targets()) return;
    fx_watch(v);
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
    {   /* UG_DBGCOLOR=10: every draw painted with its index in the frame (r = low byte, g = high byte; matches the
         * order of the UG_DRAWLOG lines, "#n") */
        static int dbg = -1;
        static uint32_t idx_frame = ~0u, idx;
        if (dbg < 0) { const char *e = getenv("UG_DBGCOLOR"); dbg = e ? atoi(e) : 0; }
        if (idx_frame != frame_no) { idx_frame = frame_no; idx = 0; }
        g_draw_idx = idx++;
        int d = g_cap_dbg ? 10 : dbg;
        u->mode[3] = (uint32_t)d | (d == 10 ? g_draw_idx << 8 : 0);
    }
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
    for (int i = 0; i < 16; i++) {
        u->mv[i / 4][i % 4] = kf(K_MODELVIEW + i * 4);
        u->imv[i / 4][i % 4] = kf(K_INV_MODELVIEW + i * 4);
    }
    for (int st = 0; st < 4; st++) {
        u->tgen[st] = 0;
        if (u->mode[0] == 2) continue;
        for (int c = 0; c < 4; c++) {
            uint32_t g = KREG(K_TEXGEN(st, c)), code = 0;
            if (g == 0x8512) code = 5;
            else if (g == 0x8511) code = 4;
            else if (g) { static uint32_t seen; if (!(seen & (1u << (g & 31)))) { seen |= 1u << (g & 31); LOG("[gpu] texgen mode %X not implemented\n", g); } }
            u->tgen[st] |= code << (c * 8);
        }
    }
    if (u->mode[0] != 2 && (KREG(K_SKIN_MODE) & 0xF)) { static int w; if (!w++) LOG("[gpu] fixed-function skinning (mode %X) not implemented\n", KREG(K_SKIN_MODE)); }
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
        /* mode 2 (3D projective) on a depth format = shadow map; volume textures are not supported yet */
        int depth_fmt_tex = tex_format((KREG(K_TEX_FORMAT(t)) >> 8) & 0xFF).kind == TF_DEPTH;
        if (mode != 1 && !(mode == 2 && depth_fmt_tex)) {
            static uint32_t warned;
            if (!(warned & (1u << mode))) { warned |= 1u << mode; LOG("[gpu] shader stage mode %u not implemented\n", mode); }
            continue;
        }
        int rect = 0, tw = 1, th = 1;
        float zmax = 0;
        VkImageView view = get_texture(t, &rect, &tw, &th, &zmax);
        if (!view) continue;
        ii[t].imageView = view;
        ii[t].sampler = zmax > 0 ? depth_sampler : get_sampler(t, rect);
        u->tmode[t] = zmax > 0 ? 4 : rect ? 2 : 1;
        u->tsz[t][0] = 1.0f / tw; u->tsz[t][1] = 1.0f / th;
        if (zmax > 0) {   /* shadow compare: tsz.z = 1/zmax (texcoord z is in Z-buffer units), tsz.w = compare func */
            u->tsz[t][2] = 1.0f / zmax; u->tsz[t][3] = (float)(KREG(K_SHADOW_FUNC) & 7);
            if (!rect) u->tsz[t][0] = u->tsz[t][1] = 1.0f;
            static int logged;
            if (logged++ < 4)
                LOG("[gpu] shadow map stage %d: fmt %08X %dx%d rect %d zmax %g func %08X ctl0 %08X\n", t,
                    KREG(K_TEX_FORMAT(t)), tw, th, rect, zmax, KREG(K_SHADOW_FUNC), KREG(K_TEX_CONTROL0(t)));
        }
    }

    {   /* UG_DRAWLOG=N: log every draw of frame N (state, first vertices, vertex program) */
        static int dl = -1;
        if (dl < 0) { const char *e = getenv("UG_DRAWLOG"); dl = e ? atoi(e) : 0; }
        if ((dl && (int)frame_no == dl) || (g_cap_log_frame && frame_no == g_cap_log_frame)) {
            LOG("[draw] #%u f%u topo %d nv %u ni %u mode %u start %u surf %dx%d clip %g..%g comb %X fcw %08X %08X tm %u/%u/%u/%u prog %05X\n",
                g_draw_idx, frame_no, topo, nv, ni, u->mode[0], u->mode[1], cur_rt->w, cur_rt->h, u->zr[0], u->zr[1], ctl,
                u->comb[0][1], u->comb[0][2], u->tmode[0], u->tmode[1], u->tmode[2], u->tmode[3], prog);
            LOG("[draw]   zfunc %X polyoff fill %X factor %g units %g cull %X/%X tex0 off %08X addr %08X\n", KREG(K_DEPTH_FUNC),
                KREG(K_POLY_OFFSET_FILL), kf(K_POLY_OFFSET_FACTOR), kf(K_POLY_OFFSET_UNITS), KREG(K_CULL_EN), KREG(K_CULL_FACE),
                KREG(K_TEX_OFFSET(0)), KREG(K_TEX_ADDRESS(0)));
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
                if (u->tgen[0] | u->tgen[1] | u->tgen[2] | u->tgen[3]) {
                    /* the driver writes the inverse-transpose 3x3 of MV into IMV (translation row/col zero), same
                     * row layout as MV: eye normal j = dot(imv[j], n) */
                    LOG("[draw]   texgen %08X %08X %08X %08X vfmt2 %08X n0 %g %g %g light ctl %08X\n", u->tgen[0], u->tgen[1],
                        u->tgen[2], u->tgen[3], KREG(K_VTX_FORMAT + 8), v[0].a[2][0], v[0].a[2][1], v[0].a[2][2],
                        g_kreg[0x294 >> 2]);
                    for (int i = 0; i < 4; i++)
                        LOG("[draw]   mv %g %g %g %g | imv %g %g %g %g | comp %g %g %g %g\n", u->mv[i][0], u->mv[i][1], u->mv[i][2],
                            u->mv[i][3], u->imv[i][0], u->imv[i][1], u->imv[i][2], u->imv[i][3], u->comp[i][0], u->comp[i][1],
                            u->comp[i][2], u->comp[i][3]);
                }
                if (u->mode[2])
                    LOG("[draw]   texmat draw: tex fmt %08X %08X %08X %08X\n", KREG(K_TEX_FORMAT(0)), KREG(K_TEX_FORMAT(1)),
                        KREG(K_TEX_FORMAT(2)), KREG(K_TEX_FORMAT(3)));
                for (int a = 0; a < 16; a++) {   /* vertex arrays: type/size/stride and offset */
                    uint32_t vf = KREG(K_VTX_FORMAT + a * 4);
                    if ((vf >> 4) & 0xF) LOG("[draw]   attr %d fmt %08X off %08X\n", a, vf, KREG(0x1720 + a * 4));
                }
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
    {   /* UG_SHADOWNOZ=1: shadow-map draws without depth test or culling (debug) */
        static int snz = -1;
        if (snz < 0) snz = getenv("UG_SHADOWNOZ") != NULL;
        int sh = u->tmode[0] == 4 || u->tmode[1] == 4 || u->tmode[2] == 4 || u->tmode[3] == 4;
        if (snz && sh) { vkCmdSetDepthTestEnable(cb, 0); vkCmdSetCullMode(cb, VK_CULL_MODE_NONE); }
        else vkCmdSetDepthTestEnable(cb, has_z && (KREG(K_DEPTH_TEST_EN) & 1));
    }
    vkCmdSetDepthWriteEnable(cb, has_z && (KREG(K_DEPTH_MASK) & 1));
    {   /* polygon offset (fill only; triangles are all the game draws with it): units are Z-buffer units, which match
         * Vulkan's minimum resolvable difference for D24; the slope term is per host pixel (UG_SCALE) */
        int en = has_z && (KREG(K_POLY_OFFSET_FILL) & 1);
        vkCmdSetDepthBiasEnable(cb, en);
        if (en) vkCmdSetDepthBias(cb, kf(K_POLY_OFFSET_UNITS), 0.0f, kf(K_POLY_OFFSET_FACTOR) / (float)g_scale);
    }
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
    {   /* UG_SHADOWSKIP=1: drop shadow-receiver draws (debug) */
        static int ss = -1;
        if (ss < 0) ss = getenv("UG_SHADOWSKIP") != NULL;
        if (ss && (u->tmode[0] == 4 || u->tmode[1] == 4 || u->tmode[2] == 4 || u->tmode[3] == 4)) return;
    }
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

/* ================================================================ touch controls overlay
 * Android: the on-screen controls of linux/touch.c, drawn over the presented frame (touch.vert / touch.frag).
 * Desktop test: UG_TOUCHDEMO=1 draws sample shapes. */
typedef struct { float shape[24][4], color[24][4], info[4]; } TouchUBO;
static VkDescriptorSetLayout touch_dsl;
static VkPipelineLayout touch_pl;
static VkPipeline touch_pipe;
static VkFormat touch_fmt;

static int touch_shapes_get(float (*sh)[8], int max) {
#ifdef __ANDROID__
    return touch_shapes(sh, max, (int)sc_ext.width, (int)sc_ext.height);
#else
    static int demo = -1;
    if (demo < 0) { const char *e = getenv("UG_TOUCHDEMO"); demo = e && *e == '1'; }
    if (!demo) return 0;
    float H = (float)sc_ext.height, W = (float)sc_ext.width;
    float d[4][8] = {{W - 0.26f * H, 0.835f * H, 0.075f * H, 0, 0.25f, 0.80f, 0.30f, 0.30f},
                     {W - 0.125f * H, 0.70f * H, 0.075f * H, 0, 0.90f, 0.25f, 0.25f, 0.60f},
                     {0.28f * H, 0.72f * H, 0.16f * H, 0.012f * H, 0.85f, 0.85f, 0.90f, 0.30f},
                     {0.28f * H, 0.72f * H, 0.07f * H, 0, 0.85f, 0.85f, 0.90f, 0.30f}};
    int n = max < 4 ? max : 4;
    memcpy(sh, d, sizeof(float) * 8 * (size_t)n);
    return n;
#endif
}

static int touch_pipeline(void) {
    if (touch_pipe && touch_fmt == sc_fmt) return 1;
    if (touch_pipe) { vkDestroyPipeline(dev, touch_pipe, 0); touch_pipe = 0; }
    if (!touch_pl) {
        VkDescriptorSetLayoutBinding b = {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1, VK_SHADER_STAGE_FRAGMENT_BIT, 0};
        VkDescriptorSetLayoutCreateInfo dli = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, 0, 0, 1, &b};
        if (vkCreateDescriptorSetLayout(dev, &dli, 0, &touch_dsl) != VK_SUCCESS) return 0;
        VkPipelineLayoutCreateInfo pli = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, 0, 0, 1, &touch_dsl};
        if (vkCreatePipelineLayout(dev, &pli, 0, &touch_pl) != VK_SUCCESS) return 0;
    }
    VkShaderModule vs = 0, fs = 0;
    VkShaderModuleCreateInfo smi = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, 0, 0, sizeof spv_touch_vert, spv_touch_vert};
    vkCreateShaderModule(dev, &smi, 0, &vs);
    smi.codeSize = sizeof spv_touch_frag; smi.pCode = spv_touch_frag;
    vkCreateShaderModule(dev, &smi, 0, &fs);
    VkPipelineShaderStageCreateInfo st[2] = {
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, 0, 0, VK_SHADER_STAGE_VERTEX_BIT, vs, "main"},
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, 0, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fs, "main"}};
    VkPipelineVertexInputStateCreateInfo vi = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, 0, 0, 1, 0, 1, 0};
    VkPipelineRasterizationStateCreateInfo rs = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL; rs.lineWidth = 1.0f; rs.cullMode = VK_CULL_MODE_NONE;
    VkPipelineMultisampleStateCreateInfo ms = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo ds = {VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    VkPipelineColorBlendAttachmentState ba = {0};
    ba.blendEnable = VK_TRUE;
    ba.srcColorBlendFactor = ba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    ba.dstColorBlendFactor = ba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    ba.colorBlendOp = ba.alphaBlendOp = VK_BLEND_OP_ADD;
    ba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo cb = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = 1; cb.pAttachments = &ba;
    VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dy = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, 0, 0, 2, dyn};
    VkPipelineRenderingCreateInfo pr = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    pr.colorAttachmentCount = 1; pr.pColorAttachmentFormats = &sc_fmt;
    VkGraphicsPipelineCreateInfo ci = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, &pr};
    ci.stageCount = 2; ci.pStages = st; ci.pVertexInputState = &vi; ci.pInputAssemblyState = &ia; ci.pViewportState = &vp;
    ci.pRasterizationState = &rs; ci.pMultisampleState = &ms; ci.pDepthStencilState = &ds; ci.pColorBlendState = &cb;
    ci.pDynamicState = &dy; ci.layout = touch_pl;
    VkResult r = vs && fs ? vkCreateGraphicsPipelines(dev, 0, 1, &ci, 0, &touch_pipe) : VK_ERROR_INITIALIZATION_FAILED;
    if (vs) vkDestroyShaderModule(dev, vs, 0);
    if (fs) vkDestroyShaderModule(dev, fs, 0);
    if (r != VK_SUCCESS) { touch_pipe = 0; return 0; }
    touch_fmt = sc_fmt;
    return 1;
}

/* swapchain image in TRANSFER_DST: draws the controls; returns 1 if it left the image in COLOR_ATTACHMENT */
static int touch_draw(VkCommandBuffer cb, uint32_t img) {
    static int failed, logged;
    if (failed || !sc_view[img]) return 0;
    float sh[24][8];
    int n = touch_shapes_get(sh, 24);
    if (!logged++) LOG("[touch] overlay: %d shapes on %ux%u\n", n, sc_ext.width, sc_ext.height);
    if (n <= 0) return 0;
    if (!touch_pipeline()) { failed = 1; LOG("[touch] overlay pipeline failed\n"); return 0; }
    Frame *f = &frames[fi];
    VkDeviceSize uoff;
    TouchUBO *u = ring_alloc(&f->ubo, sizeof(TouchUBO), pprops.limits.minUniformBufferOffsetAlignment, &uoff);
    if (!u) return 0;
    memset(u, 0, sizeof *u);
    for (int i = 0; i < n; i++) { memcpy(u->shape[i], sh[i], 16); memcpy(u->color[i], sh[i] + 4, 16); }
    u->info[0] = (float)n; u->info[2] = (float)sc_ext.width; u->info[3] = (float)sc_ext.height;
    VkDescriptorSet set;
    VkDescriptorSetAllocateInfo dai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, 0, f->dpool, 1, &touch_dsl};
    if (vkAllocateDescriptorSets(dev, &dai, &set) != VK_SUCCESS) return 0;
    VkDescriptorBufferInfo ub = {f->ubo.buf, 0, sizeof(TouchUBO)};
    VkWriteDescriptorSet w = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, 0, set, 0, 0, 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 0, &ub, 0};
    vkUpdateDescriptorSets(dev, 1, &w, 0, 0);
    raw_barrier(cb, sc_img[img], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    VkRenderingAttachmentInfo ca = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    ca.imageView = sc_view[img]; ca.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    ca.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD; ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo ri = {VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea = (VkRect2D){{0, 0}, sc_ext};
    ri.layerCount = 1; ri.colorAttachmentCount = 1; ri.pColorAttachments = &ca;
    vkCmdBeginRendering(cb, &ri);
    VkViewport vpt = {0, 0, (float)sc_ext.width, (float)sc_ext.height, 0, 1};
    vkCmdSetViewport(cb, 0, 1, &vpt);
    VkRect2D sci = {{0, 0}, sc_ext};
    vkCmdSetScissor(cb, 0, 1, &sci);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, touch_pipe);
    uint32_t dof = (uint32_t)uoff;
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, touch_pl, 0, 1, &set, 1, &dof);
    vkCmdDraw(cb, 3, 1, 0, 0);
    vkCmdEndRendering(cb);
    return 1;
}

/* SUBOPTIMAL: recreate only when the surface really changed (Android reports it every frame for a rotated
 * display, and a recreate per frame would stall) */
static int surface_changed(void) {
    VkSurfaceCapabilitiesKHR caps;
    if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(pdev, surface, &caps) != VK_SUCCESS) return 0;
    if (caps.currentTransform != sc_transform) return 1;
    return caps.currentExtent.width != UINT32_MAX &&
           (caps.currentExtent.width != sc_ext.width || caps.currentExtent.height != sc_ext.height);
}

#ifndef _WIN32
/* the window's surface went away (Android: app in the background, a new native window on resume) */
static void surface_recreate(void) {
    vkDeviceWaitIdle(dev);
    for (uint32_t i = 0; i < 8; i++) if (sc_view[i]) { vkDestroyImageView(dev, sc_view[i], 0); sc_view[i] = 0; }
    if (swapchain) { vkDestroySwapchainKHR(dev, swapchain, 0); swapchain = 0; }
    if (surface) vkDestroySurfaceKHR(inst, surface, 0);
    surface = 0;
    if (!SDL_Vulkan_CreateSurface((SDL_Window *)g_hwnd, inst, NULL, &surface)) {
        LOG("[gpu] SDL_Vulkan_CreateSurface (recreate): %s\n", SDL_GetError());
        surface = 0;
        return;
    }
    swapchain_create();
    LOG("[gpu] surface recreated\n");
}
#endif

static Surface *last_present;   /* the in-game menu re-presents it (gpu_menu_present) */
static const uint8_t *menu_px;  /* menu panel to copy over the frame, RGBA */
static int menu_w, menu_h;

/* Supersampling: one linear blit reads 2x2 texels per pixel, so above 2x it would skip some. Halves the frame (each
 * axis on its own) until it is at most twice the output; returns the image to blit (TRANSFER_SRC) and its size. */
static VkImage downsample(VkCommandBuffer cb, Surface *s, int dw, int dh, int *cw, int *ch) {
    static struct { Img im; int w, h; } ds[4];
    Img *src = &s->color;
    *cw = s->hw; *ch = s->hh;
    for (int i = 0; i < 4 && (*cw > 2 * dw || *ch > 2 * dh); i++) {
        int w = *cw > 2 * dw ? (*cw + 1) / 2 : *cw, h = *ch > 2 * dh ? (*ch + 1) / 2 : *ch;
        if (!ds[i].im.img || ds[i].w != w || ds[i].h != h) {
            img_destroy_later(&ds[i].im);
            if (!img_create(&ds[i].im, (uint32_t)w, (uint32_t)h, 1, VK_FORMAT_R8G8B8A8_UNORM,
                            VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                            VK_IMAGE_ASPECT_COLOR_BIT)) break;
            ds[i].w = w; ds[i].h = h;
            LOG("[gpu] downsample step %d: %dx%d to %dx%d (shown at %dx%d)\n", i, *cw, *ch, w, h, dw, dh);
        }
        barrier(cb, &ds[i].im, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkImageBlit bl = {{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {{0, 0, 0}, {*cw, *ch, 1}},
                          {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {{0, 0, 0}, {w, h, 1}}};
        vkCmdBlitImage(cb, src->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, ds[i].im.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                       1, &bl, VK_FILTER_LINEAR);
        barrier(cb, &ds[i].im, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        src = &ds[i].im;
        *cw = w; *ch = h;
    }
    return src->img;
}

static void submit_and_present(Surface *s) {
    Frame *f = &frames[fi];
    end_rendering();
    uint32_t img = UINT32_MAX;
#ifndef _WIN32
    if (linux_surface_lost) { linux_surface_lost = 0; linux_window_resized = 0; surface_recreate(); }
    if (linux_window_resized) {   /* Wayland may never report OUT_OF_DATE after a resize or fullscreen toggle */
        linux_window_resized = 0;
        vkDeviceWaitIdle(dev);
        swapchain_create();
    }
#endif
    VkResult ar = swapchain && surface ? vkAcquireNextImageKHR(dev, swapchain, UINT64_MAX, f->acquired, 0, &img) : VK_ERROR_OUT_OF_DATE_KHR;
#ifndef _WIN32
    if (ar == VK_ERROR_SURFACE_LOST_KHR) { linux_surface_lost = 1; img = UINT32_MAX; } else
#endif
    if (ar == VK_ERROR_OUT_OF_DATE_KHR) { if (surface) { vkDeviceWaitIdle(dev); swapchain_create(); } img = UINT32_MAX; }
    else if (ar != VK_SUCCESS && ar != VK_SUBOPTIMAL_KHR) img = UINT32_MAX;
    VkCommandBuffer cb = f->cmd;
    if (img != UINT32_MAX) {
        raw_barrier(cb, sc_img[img], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkClearColorValue black = {{0, 0, 0, 1}};
        VkImageSubresourceRange rg = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdClearColorImage(cb, sc_img[img], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &rg);
        if (s) {
            barrier(cb, &s->color, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
            /* letterbox the guest surface's aspect into the window; widescreen frames are 16:9 anamorphic (4/3 wider
             * than their pixels); UG_ASPECT=stretch fills the window */
            static int stretch = -1;
            if (stretch < 0) { const char *e = getenv("UG_ASPECT"); stretch = e && !strcmp(e, "stretch"); }
            float sa = (float)s->w / s->h * (ug_widescreen() ? 4.0f / 3.0f : 1.0f), wa = (float)sc_ext.width / sc_ext.height;
            int dw = (int)sc_ext.width, dh = (int)sc_ext.height;
            if (stretch) {}
            else if (wa > sa) dw = (int)(dh * sa + 0.5f); else dh = (int)(dw / sa + 0.5f);
            int dx = ((int)sc_ext.width - dw) / 2, dy = ((int)sc_ext.height - dh) / 2;
            int cw, ch;
            VkImage src = downsample(cb, s, dw, dh, &cw, &ch);
            VkImageBlit bl = {{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {{0, 0, 0}, {cw, ch, 1}},
                              {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {{dx, dy, 0}, {dx + dw, dy + dh, 1}}};
            vkCmdBlitImage(cb, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, sc_img[img],
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bl, VK_FILTER_LINEAR);
        }
        if (menu_px) {   /* in-game menu panel (menu.c), centered, opaque */
            int w = menu_w < (int)sc_ext.width ? menu_w : (int)sc_ext.width;
            int h = menu_h < (int)sc_ext.height ? menu_h : (int)sc_ext.height;
            VkDeviceSize off;
            uint8_t *p = ring_alloc(&f->stage, (VkDeviceSize)menu_w * menu_h * 4, 16, &off);
            if (p) {
                int bgr = sc_fmt == VK_FORMAT_B8G8R8A8_UNORM || sc_fmt == VK_FORMAT_B8G8R8A8_SRGB;
                for (int i = 0; i < menu_w * menu_h; i++) {
                    const uint8_t *q = menu_px + (size_t)i * 4;
                    uint8_t *d = p + (size_t)i * 4;
                    d[0] = bgr ? q[2] : q[0]; d[1] = q[1]; d[2] = bgr ? q[0] : q[2]; d[3] = 255;
                }
                VkBufferImageCopy rg = {off, (uint32_t)menu_w, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                                        {((int)sc_ext.width - w) / 2, ((int)sc_ext.height - h) / 2, 0}, {(uint32_t)w, (uint32_t)h, 1}};
                vkCmdCopyBufferToImage(cb, f->stage.buf, sc_img[img], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &rg);
            }
        }
    }
    /* UG_SHOT=N: every N frames, read the presented surface back and write out/shot_<frame>.ppm (debug) */
    static Ring shot;
    if (shot_every < 0) { const char *e = getenv("UG_SHOT"); shot_every = e ? atoi(e) : 0; }
    static uint32_t shot_from, shot_to = UINT32_MAX;   /* UG_SHOTRANGE=<first>,<last> frame (debug) */
    static int range_init;
    if (!range_init++) { const char *r = getenv("UG_SHOTRANGE"); if (r) sscanf(r, "%u,%u", &shot_from, &shot_to); }
    int shoot = (s && g_cap_shoot) || s && shot_every > 0 && frame_no >= shot_from && frame_no <= shot_to &&
                (frame_no % (uint32_t)shot_every == 0 || (hd_shot_ab > 0 && frame_no % (uint32_t)shot_every == 1));
    int out = shot_out && img != UINT32_MAX, shw = out ? (int)sc_ext.width : s ? s->hw : 0;
    int shh = out ? (int)sc_ext.height : s ? s->hh : 0;
    if (shoot) {
        VkDeviceSize need = (VkDeviceSize)shw * shh * 4;
        if (shot.size < need) {
            if (shot.buf) { vkDestroyBuffer(dev, shot.buf, 0); vkFreeMemory(dev, shot.mem, 0); memset(&shot, 0, sizeof shot); }
            if (!ring_create(&shot, need, VK_BUFFER_USAGE_TRANSFER_DST_BIT)) shoot = 0;
        }
        if (shoot) {
            VkBufferImageCopy rg = {0, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {(uint32_t)shw, (uint32_t)shh, 1}};
            if (out) {
                raw_barrier(cb, sc_img[img], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
                vkCmdCopyImageToBuffer(cb, sc_img[img], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, shot.buf, 1, &rg);
                raw_barrier(cb, sc_img[img], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            } else {
                barrier(cb, &s->color, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
                vkCmdCopyImageToBuffer(cb, s->color.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, shot.buf, 1, &rg);
            }
        }
    }
    if (img != UINT32_MAX) {
        int drawn = touch_draw(cb, img);
        raw_barrier(cb, sc_img[img], drawn ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
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
            int bgr = out && sc_fmt == VK_FORMAT_B8G8R8A8_UNORM;
            fprintf(fp, "P6 %d %d 255\n", shw, shh);
            for (int i = 0; i < shw * shh; i++) {
                const uint8_t *q = shot.map + i * 4;
                uint8_t rgb[3] = {q[bgr ? 2 : 0], q[1], q[bgr ? 0 : 2]};
                fwrite(rgb, 1, 3, fp);
            }
            fclose(fp);
            LOG("[gpu] screenshot %s (%u draws)\n", path, g_draw_idx);
        }
    }
    if (img != UINT32_MAX) {
        VkPresentInfoKHR pi = {VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, 0, 1, &sc_done[img], 1, &swapchain, &img, 0};
        VkResult pr = vkQueuePresentKHR(queue, &pi);
        if (pr == VK_ERROR_OUT_OF_DATE_KHR || (pr == VK_SUBOPTIMAL_KHR && surface_changed())) {
            vkDeviceWaitIdle(dev);
            swapchain_create();
        }
#ifndef _WIN32
        if (pr == VK_ERROR_SURFACE_LOST_KHR) linux_surface_lost = 1;
#endif
    }
    fi = (fi + 1) % FRAMES;
    frame_begin();
}

static void flip_impl(uint32_t addr) {
    if (!ready()) return;
    frame_no++;
    g_cap_shoot = 0;
    {   /* UG_CAPTURE=N: the F9 capture at frame N (scripted) */
        static int at = -1;
        if (at < 0) { const char *e = getenv("UG_CAPTURE"); at = e ? atoi(e) : 0; }
        if (at && (int)frame_no == at && !g_cap) g_cap = 1;
    }
    if (g_cap == 1) {   /* log the frame drawn next; the rest of the session's log goes to out/capture.log */
        g_cap_log_frame = frame_no;
        g_cap = 2;
        if (freopen("out/capture.log", "a", stderr)) LOG("[capture] frame %u\n", frame_no);
    }
    else if (g_cap == 2) { g_cap_shoot = 1; g_cap_log_frame = 0; g_cap_dbg = 1; g_cap = 3; }   /* save it; ids next */
    else if (g_cap == 3) { g_cap_shoot = 1; g_cap_dbg = 0; g_cap = 4; }               /* save the id frame */
    else if (g_cap == 4) { g_cap = 0; LOG("[capture] done: frame %u logged, shots %u and %u\n", frame_no - 2, frame_no - 2, frame_no - 1); }
    if (frame_no % 60 == 30)   /* HD images unused for 30 s */
        for (int i = 0; i < HD_CACHE; i++)
            if (hd_imgs[i].im.img && frame_no - hd_imgs[i].last > 1800) {
                img_destroy_later(&hd_imgs[i].im);
                hd_vram -= hd_imgs[i].bytes;
            }
    if (frame_no % 60 == 0)   /* evict textures idle for 10 s */
        for (int i = 0; i < TEX_CACHE; i++)
            if (tcache[i].im.img && frame_no - tcache[i].last_frame > 600) {
                img_destroy_later(&tcache[i].im);
                tcache[i].hd = 0;
            }
    /* XDK 5659 swaps without naming the front buffer (4242 left it in 1D8C): present the surface just rendered */
    Surface *s = find_rt(addr);
    if (!s) s = cur_rt;
    last_present = s;
    {   /* UG_FXAB=1: draws per frame, on (even) against off (odd) */
        static int ab = -1;
        static uint32_t sum[2], cnt[2];
        if (ab < 0) ab = getenv("UG_FXAB") != NULL;
        /* frame_no has just advanced: the frame drawn is frame_no - 1 */
        if (ab) { int o = (frame_no - 1) & 1; sum[o] += g_draw_idx; cnt[o]++; }
        if (ab && frame_no % 300 == 0 && cnt[0] && cnt[1]) {
            LOG("[ab] draws per frame: even (on) %.1f, odd (off) %.1f\n", sum[0] / (double)cnt[0], sum[1] / (double)cnt[1]);
            sum[0] = sum[1] = cnt[0] = cnt[1] = 0;
        }
    }
    if (fx_rt && fx_rt == s && !fx_done && !fx_hud) fx_apply();   /* no HUD this frame */
    fx_rt = NULL; fx_zs = NULL;
    fx_proj_ok = fx_done = fx_hud = 0;
    submit_and_present(s);
    {   /* worst frame time (flip to flip) and frames over 25 ms since the last log line: stutter */
        static uint64_t last;
        uint64_t now = sched_now_100ns();
        if (last) {
            uint32_t ms10 = (uint32_t)((now - last) / 1000);   /* 0.1 ms */
            if (ms10 > st_worst) st_worst = ms10;
            if (ms10 > 250) st_slow++;
            static int hl = -1;   /* UG_HITCHLOG=1: every frame over 25 ms, with what it did (debug) */
            if (hl < 0) hl = getenv("UG_HITCHLOG") != NULL;
            if (hl && ms10 > 250)
                LOG("[hitch] frame %u: %.1f ms, new textures so far %u, pipelines %d\n", frame_no, ms10 / 10.0, st_newtex,
                    n_pipes);
        }
        last = now;
    }
    if (frame_no % 300 == 0) {
        static DWORD t0;
        DWORD t = GetTickCount();
        LOG("[gpu] frame %u: %.1f fps, worst %.1f ms, %u slow, per frame: draws %u verts %u new textures %u, %d pipelines, "
            "%d targets\n", frame_no, t0 ? 300000.0 / (double)(t - t0) : 0.0, st_worst / 10.0, st_slow, st_draws / 300,
            st_verts / 300, st_newtex / 300, n_pipes, n_rts);
        st_draws = st_verts = st_newtex = 0;
        st_worst = st_slow = 0;
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

/* in-game menu (menu.c): present the frozen frame with the panel (RGBA) over it */
void gpu_menu_present(const uint8_t *rgba, int w, int h) {
    AcquireSRWLockExclusive(&gpu_lock);
    if (ready() && rgba && w > 0 && h > 0) {
        menu_px = rgba; menu_w = w; menu_h = h;
        submit_and_present(last_present);
        menu_px = NULL;
    }
    ReleaseSRWLockExclusive(&gpu_lock);
}
int gpu_window_height(void) { return sc_ext.height ? (int)sc_ext.height : 720; }
#ifdef _WIN32
int plat_fullscreen_get(void) { return g_hwnd && (GetWindowLongW(g_hwnd, GWL_STYLE) & WS_POPUP) != 0; }
/* the in-game menu runs on the guest thread: the window thread does the toggle, as for F11 */
void plat_fullscreen_toggle(void) { if (g_hwnd) PostMessageW(g_hwnd, WM_APP + 1, 0, 0); }
#endif
