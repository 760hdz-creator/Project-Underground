/* Host gamepads on SDL3 (Windows and Linux), handed to hle.c as XInput states.
 * SDL3 reads Xbox (XInput / GameInput / Bluetooth), PlayStation 3/4/5, Switch Pro / Joy-Con / NSO pads, Steam
 * Controller / Steam Deck, 8BitDo, GameCube adapters and most other USB / Bluetooth pads. On top of that:
 *   - gamecontrollerdb.txt (SDL_GameControllerDB, next to the exe) adds mappings for a few thousand more pads, and
 *     controllers.txt (same folder, the player's own, never touched by updates) adds or overrides lines;
 *   - a joystick that still has no mapping gets a generic one (buttons 0..9 = A B X Y LB RB BACK START LS RS,
 *     axes 0/1 left stick, 2/3 right stick, 4/5 triggers, hat 0 D-pad), so every pad does something; UG_PADMAP
 *     (the launcher's Controls page) can then rearrange the buttons.
 * Slots 0..3 go to pads in connection order; the slot number lights the player LED where the pad has one.
 * Windows: SDL3.dll is loaded at run time (shipped next to the exe); without it hle.c falls back to XInput.
 * UG_PADTEST=1: attach two virtual pads (a known-layout gamepad and an unmapped generic joystick), check the
 * mapping of every button, the sticks, triggers and rumble, and log "[padtest] N/N passed".
 * Every call runs on the thread that called pad_sdl_init (the main thread: guest threads are fibers on it). */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rt.h"

#ifdef _WIN32
#include <windows.h>
/* the few SDL3 declarations used here (SDL 3.x ABI, stable) */
#define SDLCALL __cdecl
typedef uint8_t Uint8;
typedef uint16_t Uint16;
typedef uint32_t Uint32;
typedef uint64_t Uint64;
typedef int16_t Sint16;
typedef Uint32 SDL_JoystickID;
typedef struct SDL_Gamepad SDL_Gamepad;
typedef struct SDL_Joystick SDL_Joystick;
typedef struct SDL_GUID { Uint8 data[16]; } SDL_GUID;
typedef int SDL_GamepadButton, SDL_GamepadAxis, SDL_JoystickType;
enum { SDL_GAMEPAD_BUTTON_SOUTH, SDL_GAMEPAD_BUTTON_EAST, SDL_GAMEPAD_BUTTON_WEST, SDL_GAMEPAD_BUTTON_NORTH,
       SDL_GAMEPAD_BUTTON_BACK, SDL_GAMEPAD_BUTTON_GUIDE, SDL_GAMEPAD_BUTTON_START, SDL_GAMEPAD_BUTTON_LEFT_STICK,
       SDL_GAMEPAD_BUTTON_RIGHT_STICK, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER,
       SDL_GAMEPAD_BUTTON_DPAD_UP, SDL_GAMEPAD_BUTTON_DPAD_DOWN, SDL_GAMEPAD_BUTTON_DPAD_LEFT,
       SDL_GAMEPAD_BUTTON_DPAD_RIGHT };
enum { SDL_GAMEPAD_AXIS_LEFTX, SDL_GAMEPAD_AXIS_LEFTY, SDL_GAMEPAD_AXIS_RIGHTX, SDL_GAMEPAD_AXIS_RIGHTY,
       SDL_GAMEPAD_AXIS_LEFT_TRIGGER, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER };
enum { SDL_JOYSTICK_TYPE_UNKNOWN, SDL_JOYSTICK_TYPE_GAMEPAD };
#define SDL_INIT_GAMEPAD 0x00002000u
#define SDL_HAT_UP 0x01u
typedef struct SDL_VirtualJoystickDesc {
    Uint32 version;
    Uint16 type, padding, vendor_id, product_id, naxes, nbuttons, nballs, nhats, ntouchpads, nsensors, padding2[2];
    Uint32 button_mask, axis_mask;
    const char *name;
    const void *touchpads, *sensors;
    void *userdata;
    void (SDLCALL *Update)(void *);
    void (SDLCALL *SetPlayerIndex)(void *, int);
    bool (SDLCALL *Rumble)(void *, Uint16, Uint16);
    void *RumbleTriggers, *SetLED, *SendEffect, *SetSensorsEnabled, *Cleanup;
} SDL_VirtualJoystickDesc;
#else
#include <SDL3/SDL.h>
#include <unistd.h>
#endif

#define SDL_FUNCS(X)                                                                        \
    X(bool, SetHint, (const char *, const char *))                                          \
    X(bool, InitSubSystem, (Uint32))                                                        \
    X(const char *, GetError, (void))                                                       \
    X(void, free, (void *))                                                                 \
    X(Uint64, GetTicks, (void))                                                             \
    X(void, UpdateGamepads, (void))                                                         \
    X(SDL_JoystickID *, GetGamepads, (int *))                                               \
    X(SDL_JoystickID *, GetJoysticks, (int *))                                              \
    X(bool, IsGamepad, (SDL_JoystickID))                                                    \
    X(SDL_Gamepad *, OpenGamepad, (SDL_JoystickID))                                         \
    X(void, CloseGamepad, (SDL_Gamepad *))                                                  \
    X(bool, GamepadConnected, (SDL_Gamepad *))                                              \
    X(SDL_JoystickID, GetGamepadID, (SDL_Gamepad *))                                        \
    X(Uint16, GetGamepadVendor, (SDL_Gamepad *))                                            \
    X(Uint16, GetGamepadProduct, (SDL_Gamepad *))                                           \
    X(SDL_JoystickType, GetJoystickTypeForID, (SDL_JoystickID))                             \
    X(const char *, GetGamepadName, (SDL_Gamepad *))                                        \
    X(bool, GetGamepadButton, (SDL_Gamepad *, SDL_GamepadButton))                           \
    X(Sint16, GetGamepadAxis, (SDL_Gamepad *, SDL_GamepadAxis))                             \
    X(bool, RumbleGamepad, (SDL_Gamepad *, Uint16, Uint16, Uint32))                         \
    X(bool, SetGamepadPlayerIndex, (SDL_Gamepad *, int))                                    \
    X(int, AddGamepadMappingsFromFile, (const char *))                                      \
    X(int, AddGamepadMapping, (const char *))                                               \
    X(SDL_Joystick *, OpenJoystick, (SDL_JoystickID))                                       \
    X(void, CloseJoystick, (SDL_Joystick *))                                                \
    X(int, GetNumJoystickAxes, (SDL_Joystick *))                                            \
    X(int, GetNumJoystickButtons, (SDL_Joystick *))                                         \
    X(int, GetNumJoystickHats, (SDL_Joystick *))                                            \
    X(SDL_GUID, GetJoystickGUID, (SDL_Joystick *))                                          \
    X(const char *, GetJoystickName, (SDL_Joystick *))                                      \
    X(void, GUIDToString, (SDL_GUID, char *, int))                                          \
    X(void, SetJoystickEventsEnabled, (bool))                                               \
    X(void, SetGamepadEventsEnabled, (bool))                                                \
    X(SDL_JoystickID, AttachVirtualJoystick, (const SDL_VirtualJoystickDesc *))             \
    X(bool, DetachVirtualJoystick, (SDL_JoystickID))                                        \
    X(bool, SetJoystickVirtualButton, (SDL_Joystick *, int, bool))                          \
    X(bool, SetJoystickVirtualAxis, (SDL_Joystick *, int, Sint16))                          \
    X(bool, SetJoystickVirtualHat, (SDL_Joystick *, int, Uint8))

#define DECL(r, n, a) r (SDLCALL *n) a;
static struct { SDL_FUNCS(DECL) } S;

#define MAX_PADS 4
static SDL_Gamepad *pads[MAX_PADS];
static uint32_t packets[MAX_PADS];
static struct { Uint16 lo, hi; uint64_t at; } rumble[MAX_PADS];
static SDL_JoystickID generic_done[64];
static int n_generic_done;
static int state = -1;   /* -1 not tried, 0 unavailable, 1 ready */

static uint64_t now_ms(void) { return S.GetTicks(); }
static uint64_t next_scan;

/* a joystick with no gamepad mapping: give it the common layout (see the top of this file) */
static void map_generic(SDL_JoystickID id) {
    for (int i = 0; i < n_generic_done; i++) if (generic_done[i] == id) return;
    if (n_generic_done < 64) generic_done[n_generic_done++] = id;
    SDL_Joystick *j = S.OpenJoystick(id);
    if (!j) return;
    int na = S.GetNumJoystickAxes(j), nb = S.GetNumJoystickButtons(j), nh = S.GetNumJoystickHats(j);
    char guid[40], name[64], m[512];
    S.GUIDToString(S.GetJoystickGUID(j), guid, sizeof guid);
    const char *nm = S.GetJoystickName(j);
    snprintf(name, sizeof name, "%s", nm && *nm ? nm : "Generic joystick");
    for (char *p = name; *p; p++) if (*p == ',') *p = ' ';
    int n = snprintf(m, sizeof m, "%s,%s,", guid, name);
    static const char *btn[] = {"a", "b", "x", "y", "leftshoulder", "rightshoulder", "back", "start", "leftstick",
                                "rightstick"};
    for (int b = 0; b < nb && b < 10; b++) n += snprintf(m + n, sizeof m - n, "%s:b%d,", btn[b], b);
    static const char *ax[] = {"leftx", "lefty", "rightx", "righty", "lefttrigger", "righttrigger"};
    for (int a = 0; a < na && a < 6; a++) n += snprintf(m + n, sizeof m - n, "%s:a%d,", ax[a], a);
    if (nh) n += snprintf(m + n, sizeof m - n, "dpup:h0.1,dpright:h0.2,dpdown:h0.4,dpleft:h0.8,");
    S.CloseJoystick(j);
    if (nb + na + nh == 0) return;
    LOG("[input] no mapping for \"%s\" (%d axes, %d buttons, %d hats): generic layout\n", name, na, nb, nh);
    S.AddGamepadMapping(m);
}

static void scan(void) {
    uint64_t t = now_ms();
    if (t < next_scan) return;
    next_scan = t + 500;
    for (int k = 0; k < MAX_PADS; k++)
        if (pads[k] && !S.GamepadConnected(pads[k])) {
            S.CloseGamepad(pads[k]);
            pads[k] = NULL;
            LOG("[input] pad %d removed\n", k + 1);
        }
    int count = 0;
    SDL_JoystickID *js = S.GetJoysticks(&count);
    for (int i = 0; js && i < count; i++)
        if (!S.IsGamepad(js[i])) map_generic(js[i]);
    S.free(js);
    SDL_JoystickID *ids = S.GetGamepads(&count);
    /* real gamepads take the free slots before wheels, sticks and generic-mapped devices */
    for (int pass = 0; pass < 2; pass++)
        for (int i = 0; ids && i < count; i++) {
            if ((S.GetJoystickTypeForID(ids[i]) == SDL_JOYSTICK_TYPE_GAMEPAD) != (pass == 0)) continue;
            int open = 0;
            for (int k = 0; k < MAX_PADS; k++) open |= pads[k] && S.GetGamepadID(pads[k]) == ids[i];
            for (int k = 0; k < MAX_PADS && !open; k++)
                if (!pads[k]) {
                    if ((pads[k] = S.OpenGamepad(ids[i]))) {
                        const char *nm = S.GetGamepadName(pads[k]);
                        S.SetGamepadPlayerIndex(pads[k], k);
                        packets[k] = 0;
                        memset(&rumble[k], 0, sizeof rumble[k]);
                        LOG("[input] pad %d: %s (%04X:%04X)\n", k + 1, nm ? nm : "?", S.GetGamepadVendor(pads[k]),
                            S.GetGamepadProduct(pads[k]));
                    }
                    break;
                }
        }
    S.free(ids);
}

static int16_t axis(SDL_Gamepad *g, SDL_GamepadAxis a, int flip) {
    int v = S.GetGamepadAxis(g, a);
    if (flip) v = -v - 1;   /* SDL: down positive; XInput: up positive */
    return (int16_t)(v < -32768 ? -32768 : v > 32767 ? 32767 : v);
}

static void read_pad(SDL_Gamepad *g, HostState *s) {
    static const struct { int b; uint16_t bit; } map[] = {
        {SDL_GAMEPAD_BUTTON_DPAD_UP, 0x0001}, {SDL_GAMEPAD_BUTTON_DPAD_DOWN, 0x0002},
        {SDL_GAMEPAD_BUTTON_DPAD_LEFT, 0x0004}, {SDL_GAMEPAD_BUTTON_DPAD_RIGHT, 0x0008},
        {SDL_GAMEPAD_BUTTON_START, 0x0010}, {SDL_GAMEPAD_BUTTON_BACK, 0x0020},
        {SDL_GAMEPAD_BUTTON_LEFT_STICK, 0x0040}, {SDL_GAMEPAD_BUTTON_RIGHT_STICK, 0x0080},
        {SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, 0x0100}, {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, 0x0200},
        {SDL_GAMEPAD_BUTTON_SOUTH, 0x1000}, {SDL_GAMEPAD_BUTTON_EAST, 0x2000},
        {SDL_GAMEPAD_BUTTON_WEST, 0x4000}, {SDL_GAMEPAD_BUTTON_NORTH, 0x8000},
    };
    memset(&s->Gamepad, 0, sizeof s->Gamepad);
    for (size_t i = 0; i < sizeof map / sizeof map[0]; i++)
        if (S.GetGamepadButton(g, map[i].b)) s->Gamepad.wButtons |= map[i].bit;
    int lt = S.GetGamepadAxis(g, SDL_GAMEPAD_AXIS_LEFT_TRIGGER), rt = S.GetGamepadAxis(g, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER);
    s->Gamepad.bLeftTrigger = (uint8_t)(lt <= 0 ? 0 : lt * 255 / 32767);
    s->Gamepad.bRightTrigger = (uint8_t)(rt <= 0 ? 0 : rt * 255 / 32767);
    s->Gamepad.sThumbLX = axis(g, SDL_GAMEPAD_AXIS_LEFTX, 0);
    s->Gamepad.sThumbLY = axis(g, SDL_GAMEPAD_AXIS_LEFTY, 1);
    s->Gamepad.sThumbRX = axis(g, SDL_GAMEPAD_AXIS_RIGHTX, 0);
    s->Gamepad.sThumbRY = axis(g, SDL_GAMEPAD_AXIS_RIGHTY, 1);
}

static uint32_t get_live(uint32_t port, HostState *s) {
    if (state != 1 || port >= MAX_PADS) return 1167;
    S.UpdateGamepads();
    scan();
    SDL_Gamepad *g = pads[port];
    if (!g) return 1167;   /* ERROR_DEVICE_NOT_CONNECTED */
    read_pad(g, s);
    s->dwPacketNumber = ++packets[port];
    /* XInput rumble lasts until changed; SDL's stops after at most 65 s, so renew it while it runs */
    if ((rumble[port].lo || rumble[port].hi) && now_ms() - rumble[port].at > 30000) {
        S.RumbleGamepad(g, rumble[port].lo, rumble[port].hi, 0xFFFF);
        rumble[port].at = now_ms();
    }
    return 0;
}

static uint32_t set_live(uint32_t port, uint16_t *vib) {
    if (state != 1 || port >= MAX_PADS || !pads[port]) return 1167;
    if (vib[0] != rumble[port].lo || vib[1] != rumble[port].hi) {
        S.RumbleGamepad(pads[port], vib[0], vib[1], 0xFFFF);
        rumble[port].lo = vib[0];
        rumble[port].hi = vib[1];
        rumble[port].at = now_ms();
    }
    return 0;
}

/* ---- UG_PADTEST ---- */
static Uint16 vrumble[2];
static bool SDLCALL vrumble_cb(void *u, Uint16 lo, Uint16 hi) { (void)u; vrumble[0] = lo; vrumble[1] = hi; return true; }

static int slot_of(SDL_JoystickID id) {
    for (int k = 0; k < MAX_PADS; k++) if (pads[k] && S.GetGamepadID(pads[k]) == id) return k;
    return -1;
}
static int test_pass, test_total;
static void check(const char *what, int ok) {
    test_total++;
    test_pass += ok != 0;
    if (!ok) LOG("[padtest] FAIL %s\n", what);
}
static void settle(int slot, HostState *s) {
    S.UpdateGamepads();
    memset(s, 0, sizeof *s);
    get_live((uint32_t)slot, s);
}

static void pad_selftest(void) {
    /* 1: a known-layout gamepad (a virtual "DualSense": SDL maps it from the button / axis masks) */
    SDL_VirtualJoystickDesc d;
    memset(&d, 0, sizeof d);
    d.version = sizeof d;
    d.type = SDL_JOYSTICK_TYPE_GAMEPAD;
    d.vendor_id = 0x054C;
    d.product_id = 0x0CE6;
    d.naxes = 6;
    d.nbuttons = 15;
    d.button_mask = 0x7FFF;
    d.axis_mask = 0x3F;
    d.name = "Virtual DualSense";
    d.Rumble = vrumble_cb;
    SDL_JoystickID vid = S.AttachVirtualJoystick(&d);
    /* 2: a generic joystick SDL has no mapping for (12 buttons, 4 axes, 1 hat) */
    SDL_VirtualJoystickDesc e;
    memset(&e, 0, sizeof e);
    e.version = sizeof e;
    e.type = SDL_JOYSTICK_TYPE_UNKNOWN;
    e.vendor_id = 0x1234;
    e.product_id = 0x5678;
    e.naxes = 4;
    e.nbuttons = 12;
    e.nhats = 1;
    e.name = "Virtual Generic, USB Pad";
    SDL_JoystickID gid = S.AttachVirtualJoystick(&e);
    check("attach", vid && gid);
    if (!vid || !gid) { LOG("[padtest] attach failed: %s\n", S.GetError()); return; }
    S.UpdateGamepads();
    next_scan = 0;
    scan();
    int sv = slot_of(vid), sg = slot_of(gid);
    check("virtual gamepad opened", sv >= 0);
    check("generic joystick mapped and opened", sg >= 0);
    SDL_Joystick *jv = sv >= 0 ? S.OpenJoystick(vid) : NULL, *jg = sg >= 0 ? S.OpenJoystick(gid) : NULL;
    HostState s;
    char what[96];
    if (jv) {
        static const struct { int b; uint16_t bit; const char *n; } exp[] = {
            {SDL_GAMEPAD_BUTTON_SOUTH, 0x1000, "A"}, {SDL_GAMEPAD_BUTTON_EAST, 0x2000, "B"},
            {SDL_GAMEPAD_BUTTON_WEST, 0x4000, "X"}, {SDL_GAMEPAD_BUTTON_NORTH, 0x8000, "Y"},
            {SDL_GAMEPAD_BUTTON_BACK, 0x0020, "BACK"}, {SDL_GAMEPAD_BUTTON_START, 0x0010, "START"},
            {SDL_GAMEPAD_BUTTON_LEFT_STICK, 0x0040, "LS"}, {SDL_GAMEPAD_BUTTON_RIGHT_STICK, 0x0080, "RS"},
            {SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, 0x0100, "LB"}, {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, 0x0200, "RB"},
            {SDL_GAMEPAD_BUTTON_DPAD_UP, 0x0001, "up"}, {SDL_GAMEPAD_BUTTON_DPAD_DOWN, 0x0002, "down"},
            {SDL_GAMEPAD_BUTTON_DPAD_LEFT, 0x0004, "left"}, {SDL_GAMEPAD_BUTTON_DPAD_RIGHT, 0x0008, "right"},
        };
        for (size_t i = 0; i < sizeof exp / sizeof exp[0]; i++) {
            S.SetJoystickVirtualButton(jv, exp[i].b, true);
            settle(sv, &s);
            snprintf(what, sizeof what, "gamepad %s -> %04X (got %04X)", exp[i].n, exp[i].bit, s.Gamepad.wButtons);
            check(what, s.Gamepad.wButtons == exp[i].bit);
            S.SetJoystickVirtualButton(jv, exp[i].b, false);
        }
        S.SetJoystickVirtualAxis(jv, SDL_GAMEPAD_AXIS_LEFTY, -32768);   /* stick up */
        S.SetJoystickVirtualAxis(jv, SDL_GAMEPAD_AXIS_RIGHTX, 32767);
        S.SetJoystickVirtualAxis(jv, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, 32767);
        settle(sv, &s);
        snprintf(what, sizeof what, "gamepad sticks/trigger (LY %d RX %d LT %d)", s.Gamepad.sThumbLY,
                 s.Gamepad.sThumbRX, s.Gamepad.bLeftTrigger);
        check(what, s.Gamepad.sThumbLY == 32767 && s.Gamepad.sThumbRX == 32767 && s.Gamepad.bLeftTrigger == 255 &&
                    s.Gamepad.bRightTrigger == 0);
        S.SetJoystickVirtualAxis(jv, SDL_GAMEPAD_AXIS_LEFTY, 0);
        S.SetJoystickVirtualAxis(jv, SDL_GAMEPAD_AXIS_RIGHTX, 0);
        S.SetJoystickVirtualAxis(jv, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, -32768);
        uint16_t v[2] = {0x4000, 0xC000};
        set_live((uint32_t)sv, v);
        snprintf(what, sizeof what, "rumble (got %04X %04X)", vrumble[0], vrumble[1]);
        check(what, vrumble[0] == 0x4000 && vrumble[1] == 0xC000);
        v[0] = v[1] = 0;
        set_live((uint32_t)sv, v);
        check("rumble off", !vrumble[0] && !vrumble[1]);
    }
    if (jg) {
        static const uint16_t exp[10] = {0x1000, 0x2000, 0x4000, 0x8000, 0x0100, 0x0200, 0x0020, 0x0010, 0x0040, 0x0080};
        for (int b = 0; b < 10; b++) {
            S.SetJoystickVirtualButton(jg, b, true);
            settle(sg, &s);
            snprintf(what, sizeof what, "generic button %d -> %04X (got %04X)", b, exp[b], s.Gamepad.wButtons);
            check(what, s.Gamepad.wButtons == exp[b]);
            S.SetJoystickVirtualButton(jg, b, false);
        }
        static const struct { Uint8 hat; uint16_t bit; } hats[] = {{1, 0x0001}, {2, 0x0008}, {4, 0x0002}, {8, 0x0004}};
        for (int i = 0; i < 4; i++) {
            S.SetJoystickVirtualHat(jg, 0, hats[i].hat);
            settle(sg, &s);
            snprintf(what, sizeof what, "generic hat %d -> %04X (got %04X)", hats[i].hat, hats[i].bit, s.Gamepad.wButtons);
            check(what, s.Gamepad.wButtons == hats[i].bit);
        }
        S.SetJoystickVirtualHat(jg, 0, 0);
        S.SetJoystickVirtualAxis(jg, 0, -32768);
        S.SetJoystickVirtualAxis(jg, 3, 32767);   /* right stick down */
        settle(sg, &s);
        snprintf(what, sizeof what, "generic sticks (LX %d RY %d)", s.Gamepad.sThumbLX, s.Gamepad.sThumbRY);
        check(what, s.Gamepad.sThumbLX == -32768 && s.Gamepad.sThumbRY == -32768);
    }
    if (jv) S.CloseJoystick(jv);
    if (jg) S.CloseJoystick(jg);
    S.DetachVirtualJoystick(vid);
    S.DetachVirtualJoystick(gid);
    LOG("[padtest] %d/%d passed\n", test_pass, test_total);
}

static int init_real(void) {
    if (state >= 0) return state;
    state = 0;
    const char *err = NULL;
#ifdef _WIN32
    char path[MAX_PATH];
    DWORD n = GetModuleFileNameA(NULL, path, MAX_PATH);
    char *slash = n && n < MAX_PATH ? strrchr(path, '\\') : NULL;
    if (!slash) return 0;
    strcpy(slash + 1, "SDL3.dll");
    HMODULE m = LoadLibraryExA(path, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!m) { LOG("[input] SDL3.dll not found: XInput controllers only\n"); return 0; }
#define LOADF(r, nm, a) if (!(*(void **)&S.nm = (void *)GetProcAddress(m, "SDL_" #nm))) err = "SDL_" #nm;
#else
#define LOADF(r, nm, a) S.nm = SDL_##nm;
#endif
    SDL_FUNCS(LOADF)
    if (err) { LOG("[input] SDL3.dll has no %s: XInput controllers only\n", err); return 0; }
    S.SetHint("SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS", "1");   /* like XInput: pads work while unfocused */
    S.SetHint("SDL_JOYSTICK_THREAD", "1");                     /* Windows: device notifications on SDL's thread */
    S.SetHint("SDL_APP_NAME", "Project Underground");
    /* vJoy (virtual joystick driver, often installed and idle) would take player 1; the environment variable
     * SDL_JOYSTICK_BLACKLIST_DEVICES overrides this list */
    S.SetHint("SDL_JOYSTICK_BLACKLIST_DEVICES", "0x1234/0xbead");
    if (!S.InitSubSystem(SDL_INIT_GAMEPAD)) {
        LOG("[input] SDL gamepads unavailable: %s\n", S.GetError());
        return 0;
    }
#ifdef _WIN32
    S.SetJoystickEventsEnabled(false);   /* nothing reads SDL's event queue on Windows; states are polled */
    S.SetGamepadEventsEnabled(false);
#endif
    /* both files sit next to the exe (dev runs start in the repo root, not the exe's folder) */
    char file[4096];
#ifdef _WIN32
    *slash = 0;
    const char *dir = path;
#else
    char dir[4000];
#ifdef __ANDROID__
    dir[0] = 0;   /* /proc/self/exe is app_process; the working directory is the app's files folder */
#else
    ssize_t len = readlink("/proc/self/exe", dir, sizeof dir - 1);
    while (len > 0 && dir[len - 1] != '/') len--;
    dir[len > 1 ? len - 1 : 0] = 0;
#endif
    if (!*dir) strcpy(dir, ".");
#endif
#ifdef __ANDROID__
    int db = S.AddGamepadMappingsFromFile("gamecontrollerdb.txt");   /* relative = the APK's assets */
    if (!getcwd(dir, sizeof dir)) strcpy(dir, ".");
#else
    snprintf(file, sizeof file, "%s/gamecontrollerdb.txt", dir);
    int db = S.AddGamepadMappingsFromFile(file);
#endif
    snprintf(file, sizeof file, "%s/controllers.txt", dir);
    int own = S.AddGamepadMappingsFromFile(file);
    LOG("[input] SDL gamepads ready (%d mappings from gamecontrollerdb.txt%s)\n", db < 0 ? 0 : db,
        own > 0 ? ", plus controllers.txt" : "");
    state = 1;
    const char *t = getenv("UG_PADTEST");
    if (t && *t == '1') pad_selftest();
    return 1;
}

#ifndef __ANDROID__
int pad_sdl_init(void) { return init_real(); }
uint32_t pad_sdl_get_state(uint32_t port, HostState *s) { return get_live(port, s); }
uint32_t pad_sdl_set_state(uint32_t port, uint16_t *vib) { return set_live(port, vib); }
#else
/* Android: SDL may call into Java (rumble, device lists), and Java checks the stack pointer against the thread's
 * own stack, so guest fibers only read a copy that sched_run refreshes from the main stack (android_pad_poll). */
static HostState cache[MAX_PADS];
static int cache_ok[MAX_PADS];
static uint16_t want[MAX_PADS][2];
static int want_new[MAX_PADS];
int pad_sdl_init(void) { return 1; }   /* the real init runs in the first android_pad_poll */
uint32_t pad_sdl_get_state(uint32_t port, HostState *s) {
    if (port == 0) {   /* player 1 is always there: the on-screen controls (linux/touch.c) */
        static uint32_t packet;
        if (cache_ok[0]) *s = cache[0];
        else memset(s, 0, sizeof *s);
        touch_merge(s);
        s->dwPacketNumber = ++packet;
        return 0;
    }
    if (port >= MAX_PADS || !cache_ok[port]) return 1167;
    *s = cache[port];
    return 0;
}
uint32_t pad_sdl_set_state(uint32_t port, uint16_t *vib) {
    if (port >= MAX_PADS || !cache_ok[port]) return 1167;
    want[port][0] = vib[0];
    want[port][1] = vib[1];
    want_new[port] = 1;
    return 0;
}
void android_pad_poll(void) {
    static uint64_t last;
    if (state < 0) init_real();
    if (state != 1) return;
    uint64_t t = now_ms();
    if (t - last < 4) return;
    last = t;
    for (uint32_t k = 0; k < MAX_PADS; k++) {
        HostState h;
        memset(&h, 0, sizeof h);
        cache_ok[k] = get_live(k, &h) == 0;
        cache[k] = h;
        if (want_new[k]) { want_new[k] = 0; set_live(k, want[k]); }
    }
    touch_set_pad_present(cache_ok[0]);
}
#endif
