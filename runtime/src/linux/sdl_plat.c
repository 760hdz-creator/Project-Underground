/* Linux: the game window, keyboard and monitors on SDL3 (gamepads: pad_sdl.c) (see linux/include/windows.h).
 * The window is an SDL_Window * used as the HWND (DXVK Native's SDL3 WSI takes it as the swap chain's window).
 * Every call here runs on the main thread: all guest threads are fibers on it, and sched_run pumps events. */
#include <windows.h>
#include <SDL3/SDL.h>
#include <unistd.h>
#ifdef __ANDROID__
void touch_event(uint32_t type, uint64_t id, float nx, float ny);   /* touch.c */
#endif
int ug_widescreen(void);   /* main.c (rt.h is not included here) */

#define LOG(...) do { fprintf(stderr, __VA_ARGS__); fflush(stderr); } while (0)

static SDL_Window *g_win;
volatile int linux_window_resized;
volatile int linux_surface_lost;
static int g_video;

/* ---------------- window ---------------- */
static void toggle_fullscreen(void) {
    int fs = (SDL_GetWindowFlags(g_win) & SDL_WINDOW_FULLSCREEN) != 0;
    SDL_SetWindowFullscreen(g_win, !fs);   /* borderless desktop fullscreen (no mode change) */
}

/* UG_MONITOR=N picks display N (SDL order). Default as on Windows: the installed game (underground.sh sets
 * UG_INSTALLED=1) opens on the primary display and takes focus; dev/test runs open on the first other display. */
HWND linux_create_window(void) {
#ifdef __ANDROID__
    /* made by android_start on the main stack; always fullscreen landscape */
    if (g_win) return (HWND)g_win;
    SDL_SetHint(SDL_HINT_ORIENTATIONS, "LandscapeLeft LandscapeRight");
    SDL_SetHint(SDL_HINT_APP_NAME, "Project Underground");
    if (!SDL_InitSubSystem(SDL_INIT_VIDEO)) { LOG("[gpu] SDL video unavailable: %s\n", SDL_GetError()); return NULL; }
    g_video = 1;
    g_win = SDL_CreateWindow("Project Underground", 0, 0,
                             SDL_WINDOW_FULLSCREEN | SDL_WINDOW_VULKAN | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (!g_win) { LOG("[gpu] SDL_CreateWindow failed: %s\n", SDL_GetError()); return NULL; }
    int w = 0, h = 0;
    SDL_GetWindowSizeInPixels(g_win, &w, &h);
    LOG("[gpu] window %dx%d\n", w, h);
    return (HWND)g_win;
#endif
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    SDL_SetHint(SDL_HINT_APP_NAME, "Project Underground");
    if (!SDL_InitSubSystem(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
        LOG("[gpu] SDL video unavailable: %s\n", SDL_GetError());
        return NULL;
    }
    g_video = 1;
    int n = 0;
    SDL_DisplayID *ids = SDL_GetDisplays(&n);
    SDL_DisplayID prim = SDL_GetPrimaryDisplay(), pick = prim;
    int installed = getenv("UG_INSTALLED") && *getenv("UG_INSTALLED") == '1';
    LOG("[gpu] %d display(s)\n", n);
    const char *me = getenv("UG_MONITOR");
    if (me && *me && ids && atoi(me) >= 0 && atoi(me) < n) pick = ids[atoi(me)];
    else if (!installed && ids)
        for (int i = 0; i < n; i++) if (ids[i] != prim) { pick = ids[i]; break; }
    SDL_free(ids);
    SDL_Rect r = {0, 0, 1920, 1080};
    SDL_GetDisplayBounds(pick, &r);
    /* UG_WINDOW=WxH, else 2/3 of the display's height at the game's aspect (16:9 widescreen or 4:3) */
    int wh = r.h * 2 / 3;
    int ww = ug_widescreen() ? wh * 16 / 9 : wh * 4 / 3;
    const char *wsz = getenv("UG_WINDOW");
    int qw = 0, qh = 0;
    if (wsz && sscanf(wsz, "%dx%d", &qw, &qh) == 2 && qw >= 320 && qh >= 240) { ww = qw; wh = qh; } {   /* larger than the monitor: scaled down to fit, same aspect */
        int mw = r.w - 16, mh = r.h - 64;
        if (ww > mw) { wh = wh * mw / ww; ww = mw; }
        if (wh > mh) { ww = ww * mh / wh; wh = mh; }
    }
    SDL_PropertiesID p = SDL_CreateProperties();
    SDL_SetStringProperty(p, SDL_PROP_WINDOW_CREATE_TITLE_STRING, "Tony Hawk's Underground (recomp)");
    SDL_SetNumberProperty(p, SDL_PROP_WINDOW_CREATE_X_NUMBER, SDL_WINDOWPOS_CENTERED_DISPLAY(pick));
    SDL_SetNumberProperty(p, SDL_PROP_WINDOW_CREATE_Y_NUMBER, SDL_WINDOWPOS_CENTERED_DISPLAY(pick));
    SDL_SetNumberProperty(p, SDL_PROP_WINDOW_CREATE_WIDTH_NUMBER, ww);
    SDL_SetNumberProperty(p, SDL_PROP_WINDOW_CREATE_HEIGHT_NUMBER, wh);
    SDL_SetBooleanProperty(p, SDL_PROP_WINDOW_CREATE_RESIZABLE_BOOLEAN, true);
    SDL_SetBooleanProperty(p, SDL_PROP_WINDOW_CREATE_VULKAN_BOOLEAN, true);
    SDL_SetBooleanProperty(p, SDL_PROP_WINDOW_CREATE_HIGH_PIXEL_DENSITY_BOOLEAN, true);
    SDL_SetBooleanProperty(p, SDL_PROP_WINDOW_CREATE_FOCUSABLE_BOOLEAN, true);
    g_win = SDL_CreateWindowWithProperties(p);
    SDL_DestroyProperties(p);
    if (!g_win) { LOG("[gpu] SDL_CreateWindow failed: %s\n", SDL_GetError()); return NULL; }
    if (getenv("UG_FULLSCREEN") && *getenv("UG_FULLSCREEN") == '1') toggle_fullscreen();
    if (installed) SDL_RaiseWindow(g_win);
    SDL_ShowWindow(g_win);
    SDL_SyncWindow(g_win);
    return (HWND)g_win;
}

void linux_pump_events(void) {
    static uint64_t last;
    if (!SDL_WasInit(0)) return;   /* also headless: SDL turns SIGTERM / SIGINT into SDL_EVENT_QUIT */
    uint64_t now = SDL_GetTicksNS();
    if (now - last < 2000000ull) return;   /* every 2 ms at most */
    last = now;
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        switch (e.type) {
        case SDL_EVENT_QUIT:
        case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
            LOG("[gpu] window closed\n");
            ExitProcess(0);
        case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
            linux_window_resized = 1;
            break;
#ifdef __ANDROID__
        case SDL_EVENT_DID_ENTER_FOREGROUND:
            linux_surface_lost = 1;   /* the native window was replaced while in the background */
            break;
#endif
        case SDL_EVENT_KEY_DOWN:
            if (e.key.repeat) break;
            if (e.key.key == SDLK_ESCAPE) { LOG("[gpu] escape\n"); ExitProcess(0); }
            if (e.key.key == SDLK_F11 || (e.key.key == SDLK_RETURN && (e.key.mod & SDL_KMOD_ALT))) toggle_fullscreen();
            if (e.key.key == SDLK_F10 && !e.key.repeat) {   /* HD texture pack on/off (gpu_vulkan.c) */
                extern volatile int g_hd_hidden;
                g_hd_hidden = !g_hd_hidden;
            }
            break;
#ifdef __ANDROID__
        case SDL_EVENT_FINGER_DOWN:
        case SDL_EVENT_FINGER_MOTION:
        case SDL_EVENT_FINGER_UP:
        case SDL_EVENT_FINGER_CANCELED:
            touch_event(e.type, (uint64_t)e.tfinger.fingerID, e.tfinger.x, e.tfinger.y);
            break;
#endif
        default: break;
        }
    }
}

BOOL GetClientRect(HWND w, RECT *r) {
    int pw = 0, ph = 0;
    if (w) SDL_GetWindowSizeInPixels((SDL_Window *)w, &pw, &ph);
    *r = (RECT){0, 0, pw, ph};
    return w != NULL;
}
HMONITOR MonitorFromWindow(HWND w, DWORD flags) {
    (void)flags;
    SDL_DisplayID d = w ? SDL_GetDisplayForWindow((SDL_Window *)w) : 0;
    return (HMONITOR)(uintptr_t)(d ? d : SDL_GetPrimaryDisplay());
}
BOOL GetMonitorInfoW(HMONITOR m, MONITORINFO *mi) {
    SDL_DisplayID d = (SDL_DisplayID)(uintptr_t)m;
    SDL_Rect b = {0, 0, 1920, 1080}, u;
    SDL_GetDisplayBounds(d, &b);
    /* physical pixels: bounds are in points on scaled Wayland outputs */
    const SDL_DisplayMode *dm = SDL_GetDesktopDisplayMode(d);
    if (dm && dm->w > b.w) { b.w = dm->w; b.h = dm->h; }
    u = b;
    mi->rcMonitor = (RECT){b.x, b.y, b.x + b.w, b.y + b.h};
    mi->rcWork = (RECT){u.x, u.y, u.x + u.w, u.y + u.h};
    mi->dwFlags = d == SDL_GetPrimaryDisplay() ? MONITORINFOF_PRIMARY : 0;
    return TRUE;
}
HWND GetForegroundWindow(void) { return g_win && SDL_GetKeyboardFocus() == g_win ? (HWND)g_win : NULL; }
DWORD GetWindowThreadProcessId(HWND w, DWORD *pid) { (void)w; if (pid) *pid = (DWORD)getpid(); return 0; }

/* ---------------- keyboard: virtual keys -> SDL scancodes ---------------- */
static SDL_Scancode scancode_of(int vk) {
    if (vk >= 'A' && vk <= 'Z') return (SDL_Scancode)(SDL_SCANCODE_A + (vk - 'A'));
    if (vk >= '1' && vk <= '9') return (SDL_Scancode)(SDL_SCANCODE_1 + (vk - '1'));
    if (vk == '0') return SDL_SCANCODE_0;
    if (vk >= VK_F1 && vk <= VK_F12) return (SDL_Scancode)(SDL_SCANCODE_F1 + (vk - VK_F1));
    if (vk >= VK_NUMPAD1 && vk <= VK_NUMPAD9) return (SDL_Scancode)(SDL_SCANCODE_KP_1 + (vk - VK_NUMPAD1));
    switch (vk) {
    case VK_NUMPAD0: return SDL_SCANCODE_KP_0;
    case VK_BACK: return SDL_SCANCODE_BACKSPACE;
    case VK_TAB: return SDL_SCANCODE_TAB;
    case VK_RETURN: return SDL_SCANCODE_RETURN;
    case VK_PAUSE: return SDL_SCANCODE_PAUSE;
    case VK_CAPITAL: return SDL_SCANCODE_CAPSLOCK;
    case VK_ESCAPE: return SDL_SCANCODE_ESCAPE;
    case VK_SPACE: return SDL_SCANCODE_SPACE;
    case VK_PRIOR: return SDL_SCANCODE_PAGEUP;
    case VK_NEXT: return SDL_SCANCODE_PAGEDOWN;
    case VK_END: return SDL_SCANCODE_END;
    case VK_HOME: return SDL_SCANCODE_HOME;
    case VK_LEFT: return SDL_SCANCODE_LEFT;
    case VK_UP: return SDL_SCANCODE_UP;
    case VK_RIGHT: return SDL_SCANCODE_RIGHT;
    case VK_DOWN: return SDL_SCANCODE_DOWN;
    case VK_INSERT: return SDL_SCANCODE_INSERT;
    case VK_DELETE: return SDL_SCANCODE_DELETE;
    case VK_MULTIPLY: return SDL_SCANCODE_KP_MULTIPLY;
    case VK_ADD: return SDL_SCANCODE_KP_PLUS;
    case VK_SUBTRACT: return SDL_SCANCODE_KP_MINUS;
    case VK_DECIMAL: return SDL_SCANCODE_KP_PERIOD;
    case VK_DIVIDE: return SDL_SCANCODE_KP_DIVIDE;
    case VK_LSHIFT: return SDL_SCANCODE_LSHIFT;
    case VK_RSHIFT: return SDL_SCANCODE_RSHIFT;
    case VK_LCONTROL: return SDL_SCANCODE_LCTRL;
    case VK_RCONTROL: return SDL_SCANCODE_RCTRL;
    case VK_LMENU: return SDL_SCANCODE_LALT;
    case VK_RMENU: return SDL_SCANCODE_RALT;
    case VK_OEM_1: return SDL_SCANCODE_SEMICOLON;
    case VK_OEM_PLUS: return SDL_SCANCODE_EQUALS;
    case VK_OEM_COMMA: return SDL_SCANCODE_COMMA;
    case VK_OEM_MINUS: return SDL_SCANCODE_MINUS;
    case VK_OEM_PERIOD: return SDL_SCANCODE_PERIOD;
    case VK_OEM_2: return SDL_SCANCODE_SLASH;
    case VK_OEM_3: return SDL_SCANCODE_GRAVE;
    case VK_OEM_4: return SDL_SCANCODE_LEFTBRACKET;
    case VK_OEM_5: return SDL_SCANCODE_BACKSLASH;
    case VK_OEM_6: return SDL_SCANCODE_RIGHTBRACKET;
    case VK_OEM_7: return SDL_SCANCODE_APOSTROPHE;
    default: return SDL_SCANCODE_UNKNOWN;
    }
}
SHORT GetAsyncKeyState(int vk) {
    if (!g_video) return 0;
    int n = 0;
    const bool *k = SDL_GetKeyboardState(&n);
    int down;
    switch (vk) {
    case VK_SHIFT: down = k[SDL_SCANCODE_LSHIFT] || k[SDL_SCANCODE_RSHIFT]; break;
    case VK_CONTROL: down = k[SDL_SCANCODE_LCTRL] || k[SDL_SCANCODE_RCTRL]; break;
    case VK_MENU: down = k[SDL_SCANCODE_LALT] || k[SDL_SCANCODE_RALT]; break;
    default: { SDL_Scancode s = scancode_of(vk); down = s != SDL_SCANCODE_UNKNOWN && s < n && k[s]; }
    }
    return down ? (SHORT)0x8000 : 0;
}

/* gamepads: pad_sdl.c (shared with Windows). Nothing else loads libraries on Linux. */
HMODULE LoadLibraryA(const char *name) { (void)name; return NULL; }
void *GetProcAddress(HMODULE m, const char *name) { (void)m; (void)name; return NULL; }

#ifdef __ANDROID__
/* ---------------- Android start / restart ---------------- */
#include <jni.h>
#include <sys/stat.h>
static void mkdirs(char *path) {   /* the folders of a file path */
    for (char *p = path + 1; *p; p++)
        if (*p == '/') { *p = 0; mkdir(path, 0755); *p = '/'; }
}
/* The APK built on the player's PC (tools/ug_android.py) carries their disc's files as assets: game.lst lists them
 * ("# <build id>", then "<size> <path>" per file) and they are copied to files/game on the first start of each build.
 * A dev APK has no game.lst (the files are copied in by hand). */
static void unpack_game(void) {
    size_t n = 0;
    char *lst = SDL_LoadFile("game.lst", &n);
    if (!lst) return;
    char id[160] = "", have[160] = "";
    sscanf(lst, "# %159s", id);
    FILE *f = fopen("game/.build-id", "r");
    if (f) { if (fscanf(f, "%159s", have) != 1) have[0] = 0; fclose(f); }
    if (id[0] && !strcmp(id, have) && !access("game/default.xbe", R_OK)) { SDL_free(lst); return; }
    SDL_ShowAndroidToast("Project Underground: preparing the game files (first start, about a minute)", 1, -1, 0, 0);
    LOG("[android] unpacking the game files (build %s)\n", id);
    remove("game/.build-id");
    static char buf[1 << 20];
    int files = 0;
    for (char *line = strtok(lst, "\n"); line; line = strtok(NULL, "\n")) {
        long long size;
        char rel[1024], src[1100], dst[1100];
        if (line[0] == '#' || sscanf(line, "%lld %1023[^\r\n]", &size, rel) != 2) continue;
        snprintf(src, sizeof src, "game/%s", rel);
        snprintf(dst, sizeof dst, "game/%s", rel);
        mkdirs(dst);
        SDL_IOStream *in = SDL_IOFromFile(src, "rb");
        FILE *out = in ? fopen(dst, "wb") : NULL;
        if (!in || !out) {
            LOG("[android] cannot copy %s\n", rel);
            if (in) SDL_CloseIO(in);
            continue;
        }
        size_t r;
        while ((r = SDL_ReadIO(in, buf, sizeof buf)) > 0) fwrite(buf, 1, r, out);
        SDL_CloseIO(in);
        fclose(out);
        files++;
    }
    SDL_free(lst);
    f = fopen("game/.build-id", "w");
    if (f) { fprintf(f, "%s\n", id); fclose(f); }
    LOG("[android] %d game files ready\n", files);
}

void android_start(void) {
    const char *dir = SDL_GetAndroidExternalStoragePath();   /* Android/data/<package>/files */
    if (!dir || chdir(dir)) {
        dir = SDL_GetAndroidInternalStoragePath();
        if (!dir || chdir(dir)) dir = "?";
    }
    mkdir("out", 0755);
    rename("out/underground.log", "out/underground-previous.log");
    if (!freopen("out/underground.log", "w", stderr)) {}
    LOG("[android] files folder %s\n", dir);
    mkdir("game", 0755);
    unpack_game();
    if (access("game/default.xbe", R_OK)) {
        char msg[1024];
        snprintf(msg, sizeof msg, "The game files are missing: copy the disc's files to\n%s/game", dir);
        LOG("[android] %s\n", msg);
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Project Underground", msg, NULL);
        _exit(1);
    }
    linux_create_window();
}

/* HalReturnToFirmware: UndergroundActivity.relaunch() starts a helper process that opens the game again and ends
 * this one (an Android app cannot exec itself) */
void android_relaunch(void) {
    LOG("[android] relaunch\n");
    fflush(NULL);
    JNIEnv *env = (JNIEnv *)SDL_GetAndroidJNIEnv();
    jobject act = (jobject)SDL_GetAndroidActivity();
    if (env && act) {
        jclass cls = (*env)->GetObjectClass(env, act);
        jmethodID m = cls ? (*env)->GetMethodID(env, cls, "relaunch", "()V") : NULL;
        if (m) (*env)->CallVoidMethod(env, act, m);
        if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
    }
    for (;;) SDL_Delay(1000);   /* the helper ends this process */
}
#endif
