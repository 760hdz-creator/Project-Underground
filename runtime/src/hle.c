/* Host implementations of guest library functions (recomp/hle.txt; XDK 5659 addresses from tools/oovpa_scan.py).
 * XPP input: the game sees UG_PADS gamepads (default 1) on ports 0..N-1, fed by the host's XInput controllers and,
 * on port 0, the keyboard. THUG never calls XGetDevices/XGetDeviceChanges: it retries XInputOpen per port.
 */
#include <windows.h>
#include <stdlib.h>
#include <string.h>
#include "rt.h"

/* stdcall return: pop return address + nargs, set eax */
static void ret_std(X86Context *c, int nargs, uint32_t eax) {
    c->eax = eax;
    c->esp += 4 + 4 * nargs;
}

/* ---- host gamepads: SDL3 (every controller SDL knows: Xbox, PlayStation, Switch, Steam, 8BitDo, ... see
 * pad_sdl.c); on Windows without SDL3.dll next to the exe, XInput (loaded dynamically: the exe runs without it) ---- */
static DWORD (WINAPI *host_get_state)(DWORD, void *);
static DWORD (WINAPI *host_set_state)(DWORD, void *);

/* WINAPI-typed wrappers: no cast across calling conventions (DXVK Native's WINAPI may be ms_abi) */
static DWORD WINAPI sdl_get_state(DWORD port, void *s) { return pad_sdl_get_state(port, s); }
static DWORD WINAPI sdl_set_state(DWORD port, void *v) { return pad_sdl_set_state(port, v); }

static void host_xinput_init(void) {
    static int done;
    if (done++) return;
    if (pad_sdl_init()) {   /* SDL3 gamepads (pad_sdl.c), same XINPUT_STATE layout */
        host_get_state = sdl_get_state;
        host_set_state = sdl_set_state;
        return;
    }
    HMODULE m = LoadLibraryA("xinput1_4.dll");
    if (!m) m = LoadLibraryA("xinput9_1_0.dll");
    if (m) host_get_state = (void *)GetProcAddress(m, "XInputGetState");
    if (m) host_set_state = (void *)GetProcAddress(m, "XInputSetState");
}

#define PAD_HANDLE 0x7A000000u   /* port in low bits */
#define ERROR_DEVICE_NOT_CONNECTED_ 1167

static uint32_t packet;

static int our_focus(void) {
    DWORD pid = 0;
    HWND f = GetForegroundWindow();
    if (!f) return 0;
    GetWindowThreadProcessId(f, &pid);
    return pid == GetCurrentProcessId();
}
static int key(int vk) { return vk && (GetAsyncKeyState(vk) & 0x8000) != 0; }

/* Typing on the PC keyboard. THUG keeps the PS2 USB-keyboard path: the on-screen keyboard script
 * (create_onscreen_keyboard, scripts/engine/menu/keyboard.qb) calls StartKeyboardHandler, whose task (0xFA660) polls
 * GetKeyboardInput(buf) every frame and runs the script handle_keyboard_input for each char: 0x20..0x7E text, 0x19
 * enter (got_enter), 0x18 backspace (got_backspace). On Xbox the function is a stub returning 0; here the handler task
 * gets what the window received (WM_CHAR / SDL text input), one char per call. Other tasks poll it too (menu arrows
 * 0x14..0x17 at 0xF9A60, and more) and get nothing, so they cannot eat typed chars. While the handler task polls, the
 * letter keys stop driving the pad so typing does not also press buttons. */
static char kbd_q[256];
static volatile unsigned kbd_head, kbd_tail;   /* window thread writes head, guest thread reads tail */
static volatile uint32_t kbd_poll_ms;           /* last poll by the keyboard handler task (GetTickCount) */
static int kbd_typing(void) { return kbd_poll_ms && GetTickCount() - kbd_poll_ms < 1000; }
void kbd_text_push(int ch) {
    if (ch == 13 || ch == 10) ch = 0x19;   /* Enter */
    else if (ch == 8) ch = 0x18;           /* Backspace */
    else if (ch < 0x20 || ch > 0x7E) return;
    if (!kbd_typing() || kbd_head - kbd_tail >= sizeof kbd_q) return;
    kbd_q[kbd_head % sizeof kbd_q] = (char)ch;
    __atomic_thread_fence(__ATOMIC_RELEASE);
    kbd_head++;
}

/* keyboard on port 0: arrows = D-pad, Enter = START, Backspace = BACK, Z/X/C/V = A/B/X/Y, F/G = Black/White,
 * Q/E = LT/RT, WASD = left stick, IJKL = right stick */
static const int kb_dpad[6] = {VK_UP, VK_DOWN, VK_LEFT, VK_RIGHT, VK_RETURN, VK_BACK};
static const int kb_analog[8] = {'Z', 'X', 'C', 'V', 'F', 'G', 'Q', 'E'};

/* host XINPUT_STATE: dwPacketNumber, wButtons, bLeftTrigger, bRightTrigger, sThumbLX LY RX RY */
typedef struct { DWORD packet; WORD buttons; BYTE lt, rt; SHORT lx, ly, rx, ry; } HostXState;

/* guest XINPUT_STATE: dwPacketNumber, wButtons, bAnalogButtons[8] (A B X Y BLACK WHITE LT RT), sThumbLX LY RX RY */
volatile int g_invert_x = -1, g_invert_y = -1;   /* set by the F1 menu (-1: UG_INVERTX / UG_INVERTY) */
static void fill_state(uint32_t dst, int port) {
    HostXState h;
    memset(&h, 0, sizeof h);
    host_xinput_init();
    if (host_get_state) host_get_state((DWORD)port, &h);
    uint16_t b = h.buttons;
    /* host: up 1, down 2, left 4, right 8, start 0x10, back 0x20, LS 0x40, RS 0x80, LB 0x100, RB 0x200,
     * A 0x1000, B 0x2000, X 0x4000, Y 0x8000. Guest digital: same low byte. LB/RB -> White/Black. */
    uint16_t buttons = b & 0xFF;
    uint8_t analog[8] = {
        b & 0x1000 ? 255 : 0, b & 0x2000 ? 255 : 0, b & 0x4000 ? 255 : 0, b & 0x8000 ? 255 : 0,
        b & 0x0200 ? 255 : 0, b & 0x0100 ? 255 : 0, h.lt, h.rt};
    if (port == 0 && (b & 0xC0) == 0xC0) {   /* L3+R3: in-game menu (menu.c) */
        extern volatile int g_menu_req;
        g_menu_req = 1;
    }
    if (port == 0 && our_focus()) {
        int typing = kbd_typing();   /* text entry polling the keyboard: only the arrows still act as the D-pad */
        for (int i = 0; i < (typing ? 4 : 6); i++) if (key(kb_dpad[i])) buttons |= 1u << i;
        for (int i = 0; i < 8 && !typing; i++) if (key(kb_analog[i])) analog[i] = 255;
        if (typing) goto kb_done;
        if (key('W')) h.ly = 32767;
        if (key('S')) h.ly = -32767;
        if (key('A')) h.lx = -32767;
        if (key('D')) h.lx = 32767;
        if (key('I')) h.ry = 32767;
        if (key('K')) h.ry = -32767;
        if (key('J')) h.rx = -32767;
        if (key('L')) h.rx = 32767;
    kb_done:;
    }
    if (port == 0) {   /* UG_INPUT=<s>:<button>[:<dur s>],... scripted pad 0 input (guest seconds; default 0.15 s) */
        static struct { double t, d; int btn; } ev[256];
        static int nev = -1;
        if (nev < 0) {
            nev = 0;
            static const char *names[] = {"UP", "DOWN", "LEFT", "RIGHT", "START", "BACK", "LS", "RS",
                                          "A", "B", "X", "Y", "BLACK", "WHITE", "LT", "RT",
                                          "LUP", "LDOWN", "LLEFT", "LRIGHT", "MENU"};
            const char *e = getenv("UG_INPUT");
            char buf[4096];
            snprintf(buf, sizeof buf, "%s", e ? e : "");
            for (char *tok = strtok(buf, ","); tok && nev < 256; tok = strtok(NULL, ",")) {
                char name[16] = {0};
                double t = 0, d = 0.15;
                if (sscanf(tok, "%lf:%15[A-Z]:%lf", &t, name, &d) < 2) continue;
                for (int k = 0; k < 21; k++)
                    if (!strcmp(name, names[k])) { ev[nev].t = t; ev[nev].d = d; ev[nev].btn = k; nev++; break; }
            }
            if (nev) LOG("[input] %d scripted events\n", nev);
        }
        double now = sched_now_100ns() / 1e7;
        for (int i = 0; i < nev; i++) {
            if (now < ev[i].t || now >= ev[i].t + ev[i].d) continue;
            int k = ev[i].btn;
            if (k < 8) buttons |= 1u << k;
            else if (k < 16) analog[k - 8] = 255;
            else if (k == 16) h.ly = 32767;
            else if (k == 17) h.ly = -32767;
            else if (k == 18) h.lx = -32767;
            else if (k == 19) h.lx = 32767;
            else { extern volatile int g_menu_req; g_menu_req = 1; ev[i].t = -1e9; }   /* MENU: once */
        }
    }
    WR32(dst, ++packet);
    WR16(dst + 4, buttons);
    memcpy(g_mem + dst + 6, analog, 8);
    WR16(dst + 14, (uint16_t)h.lx);
    WR16(dst + 16, (uint16_t)h.ly);
    {   /* right stick (camera): both axes are flipped from the original Xbox game by default, so up is up and left
         * is left (user's choice 2026-11-23). UG_INVERTX=0 / UG_INVERTY=0 = the original Xbox direction on that axis
         * (launcher "Camera", F1 menu). */
        static int ix = -1, iy = -1;
        if (ix < 0) { const char *e = getenv("UG_INVERTX"); ix = !(e && *e == '0'); }
        if (iy < 0) { const char *e = getenv("UG_INVERTY"); iy = !(e && *e == '0'); }
        if (g_invert_x >= 0) ix = g_invert_x;
        if (g_invert_y >= 0) iy = g_invert_y;
        if (ix) h.rx = (SHORT)(h.rx == -32768 ? 32767 : -h.rx);
        if (iy) h.ry = (SHORT)(h.ry == -32768 ? 32767 : -h.ry);
    }
    WR16(dst + 18, (uint16_t)h.rx);
    WR16(dst + 20, (uint16_t)h.ry);
}

/* in-game menu input (menu.c): pad 0 in XInput layout, left stick folded into the D-pad bits */
uint32_t menu_pad_buttons(void) {
    HostXState h;
    memset(&h, 0, sizeof h);
    host_xinput_init();
    if (host_get_state) host_get_state(0, &h);
    uint32_t b = h.buttons;
    if (h.ly > 16000) b |= 1;
    if (h.ly < -16000) b |= 2;
    if (h.lx < -16000) b |= 4;
    if (h.lx > 16000) b |= 8;
    return b;
}
int menu_keys_down(void) {
    static const int k[] = {VK_RETURN, VK_SPACE, VK_ESCAPE, VK_F1, VK_BACK, VK_UP, VK_DOWN, VK_LEFT, VK_RIGHT, 'Z', 'X'};
    for (unsigned i = 0; i < sizeof k / sizeof *k; i++) if (key(k[i])) return 1;
    return 0;
}

/* UG_PADS=N: gamepads on ports 0..N-1 (1..4, default 1) */
static uint32_t pad_mask(void) {
    static int n = -1;
    if (n < 0) {
        const char *e = getenv("UG_PADS");
        n = e ? atoi(e) : 1;
        if (n < 1) n = 1;
        if (n > 4) n = 4;
    }
    return (1u << n) - 1;
}
static int pad_ok(uint32_t h) { return (h & ~3u) == PAD_HANDLE && (pad_mask() >> (h & 3) & 1); }

/* Script checksums: the game's CRC32 (lower case, '/' as '\', init ~0, no final xor; table at 0x225EC0, GenerateCRC
 * 0x118C0). */
static uint32_t qb_crc(const char *s) {
    uint32_t r = 0xFFFFFFFFu;
    for (; *s; s++) {
        uint8_t ch = (uint8_t)*s;
        if (ch >= 'A' && ch <= 'Z') ch += 32;
        if (ch == '/') ch = '\\';
        r = (r >> 8) ^ RD32(0x225EC0u + 4 * ((ch ^ r) & 0xFF));
    }
    return r;
}

/* Level test cheat. ChangeLevel (script C function 0xF1D60, params = arg 0): its CStruct is a list of components
 * (+0 type, 0x0D = name/checksum; +4 name; +8 value; +0xC next); "level" holds the level script (load_nj ...).
 * Every change is logged as "[level] load_xx". UG_LEVEL=<xx or load_xx> sends every change into a skating level to
 * that level instead (menus, Create-a-Skater bedroom, skate shops and park editor are left alone): start story or
 * free skate and land in Moscow with UG_LEVEL=ru. Levels on the disc: nj ny fl sd hi vc sj ru hn ph se sc sc2 dj sp
 * vn testlevel. */
static const char *const k_levels[] = {"nj", "ny", "fl", "sd", "hi", "vc", "sj", "ru", "hn", "ph", "se", "sc", "sc2",
                                       "dj", "sp", "vn", "testlevel",   /* skating levels (K_SKATE_LEVELS) */
                                       "mainmenu", "cas_bedroom", "skateshop", "nj_skateshop", "sk5ed", "sk5ed_shell",
                                       "sk5ed2", "sk5ed3", "sk5ed4", "sk5ed5"};
#define K_SKATE_LEVELS 17
static int level_index(uint32_t crc) {
    char b[48];
    for (int i = 0; i < (int)(sizeof k_levels / sizeof *k_levels); i++) {
        snprintf(b, sizeof b, "load_%s", k_levels[i]);
        if (qb_crc(b) == crc || qb_crc(k_levels[i]) == crc) return i;
    }
    return -1;
}
static void change_level_hook(X86Context *c) {
    uint32_t params = RD32(c->esp + 4), want = qb_crc("level");
    for (uint32_t comp = params ? RD32(params) : 0, n = 0; comp && n < 64; comp = RD32(comp + 0xC), n++) {
        if (RD32(comp + 4) != want || RD8(comp) != 0x0D) continue;
        uint32_t v = RD32(comp + 8);
        int i = level_index(v);
        const char *e = getenv("UG_LEVEL");
        if (e && *e && i >= 0 && i < K_SKATE_LEVELS) {
            char to[48];
            snprintf(to, sizeof to, "%s%s", strncmp(e, "load_", 5) ? "load_" : "", e);
            uint32_t t = qb_crc(to);
            if (t != v) { WR32(comp + 8, t); LOG("[level] load_%s -> %s (UG_LEVEL)\n", k_levels[i], to); return; }
        }
        if (i >= 0) LOG("[level] load_%s\n", k_levels[i]);
        else LOG("[level] %08X\n", v);
        return;
    }
    LOG("[level] ChangeLevel without a level name\n");
}

/* recomp/hooks.txt entry hooks (UG_HOOKLOG=1 logs registers and the first stack slots) */
void x86_hook(X86Context *c, uint32_t va) {
    if (va == 0xF1D60) { change_level_hook(c); return; }
    if (va == 0x34F80 && getenv("UG_STREAMLOG")) {   /* PlayStreamById(checksum): table at [29E83C], count [29E840] */
        uint32_t id = RD32(c->esp + 4), n = RD32(0x29E840), t = RD32(0x29E83C), found = 0xFFFFFFFF;
        for (uint32_t i = 0; i < n && i < 100000; i++) if (RD32(t + i * 12) == id) { found = i; break; }
        LOG("[stream] play %08X: table %u entries, %s (entry %08X %08X) slots %08X %08X\n", id, n,
            found != 0xFFFFFFFF ? "found" : "NOT FOUND", found != 0xFFFFFFFF ? RD32(t + found * 12 + 4) : 0,
            found != 0xFFFFFFFF ? RD32(t + found * 12 + 8) : 0, RD32(0x24E7DC), RD32(0x24E7E0));
    }
    if (va == 0x34C70 && getenv("UG_STREAMLOG")) {   /* stop both stream slots (this = ecx) */
        static int n;
        if (n++ < 40) LOG("[stream] t=%.2f stop-all this %08X [this] %08X %08X from %08X\n", sched_now_100ns() / 1e7,
                          c->ecx, RD32(c->ecx), RD32(c->ecx + 4), RD32(c->esp));
    }
    if (va == 0x34CC0 && getenv("UG_STREAMLOG")) {   /* stream manager update: slot changes */
        static uint32_t a0, b0;
        uint32_t a = RD32(0x24E7DC), b = RD32(0x24E7E0);
        if (a != a0 || b != b0) {
            LOG("[stream] t=%.2f slots music %08X speech %08X\n", sched_now_100ns() / 1e7, a, b);
            if (b) LOG("[stream]   speech: ready %u busy %u state %u buffered %X\n", RD8(b + 0xC), RD8(b + 0xD), RD32(b + 0xA4), RD32(b + 0x9C));
            a0 = a; b0 = b;
        }
    }
    if ((va == 0x106C10 || va == 0x31C60) && getenv("UG_AVSYNCLOG")) {   /* cutscene frame / start preloaded stream */
        static double last;
        static int frames;
        double now = host_ms();
        if (va == 0x31C60) LOG("[avsync] %.1f ms start preloaded stream (cutscene frame %d)\n", now, frames);
        else {
            if (now - last > 1000) { frames = 0; LOG("[avsync] %.1f ms cutscene frame 0\n", now); }
            if (++frames % 300 == 0) LOG("[avsync] %.1f ms cutscene frame %d\n", now, frames);
            last = now;
        }
        return;
    }
    if (va == 0x34490) {   /* music / speech stream update (this = ecx): UG_STREAMLOG=1 logs its state (debug) */
        static int on = -1;
        static uint32_t last;
        if (on < 0) on = getenv("UG_STREAMLOG") != NULL;
        uint32_t t = GetTickCount();
        if (on && t - last > 500) {
            last = t;
            uint32_t s = c->ecx;
            LOG("[stream] %08X ready %u busy %u state %u buffered %X want %X pos %X a0 %X 94 %X 98 %X\n", s, RD8(s + 0xC),
                RD8(s + 0xD), RD32(s + 0xA4), RD32(s + 0x9C), RD32(s + 0xA0), RD32(s + 0x4C), RD32(s + 0xA0),
                RD32(s + 0x94), RD32(s + 0x98));
        }
        return;
    }
    static int on = -1;
    if (on < 0) on = getenv("UG_HOOKLOG") != NULL;
    if (on) LOG("[hook] %06X eax %08X ecx %08X edx %08X ebx %08X | %08X %08X %08X %08X %08X\n", va, c->eax, c->ecx,
                c->edx, c->ebx, RD32(c->esp), RD32(c->esp + 4), RD32(c->esp + 8), RD32(c->esp + 12), RD32(c->esp + 16));
    if (on && c->ecx < RAM_SIZE) LOG("[hook]   [ecx] %08X [ecx+4] %08X\n", RD32(c->ecx), RD32(c->ecx + 4));
}
void script_file_event(const char *path) { (void)path; }

void x86_hle(X86Context *c, uint32_t va) {
    switch (va) {
    case 0x200AF3: /* VOID XInitDevices(DWORD n, PXDEVICE_PREALLOC_TYPE types) */
        ret_std(c, 2, 0);
        return;
    case 0x201592: /* HANDLE XInputOpen(type, DWORD port, DWORD slot, PXINPUT_POLLING_PARAMETERS) */
        ret_std(c, 4, ARG(1) < 4 && (pad_mask() >> ARG(1) & 1) ? PAD_HANDLE | ARG(1) : 0);
        return;
    case 0x2015E8: /* VOID XInputClose(HANDLE) */
        ret_std(c, 1, 0);
        return;
    case 0x2015F4: /* DWORD XInputGetState(HANDLE, PXINPUT_STATE) */
        {   /* in-game menu "Unlock everything" (menu.c): the game's script unlock_all_cheats, run on this guest thread
             * with RunScript(checksum, params, object, 0) = 0x1F4B0 */
            extern volatile int g_unlock_req;
            if (g_unlock_req) {
                g_unlock_req = 0;
                uint32_t args[4] = {qb_crc("unlock_all_cheats"), 0, 0, 0};
                X86Context sub = *c;
                sub.esp = c->esp - 64;
                guest_call(&sub, 0x1F4B0u, 4, args);
                uint32_t sk = RD32(0x2FCD68u), ca = sk ? RD32(sk + 0x20) : 0;   /* CHEAT_UNLOCKED_1 = flag 120 */
                LOG("[menu] unlock_all_cheats done (flag 120 = %u)\n", ca ? RD32(ca + 0x584 + 12) >> 24 & 1 : 9);
            }
        }
        if (!pad_ok(ARG(0))) { ret_std(c, 2, ERROR_DEVICE_NOT_CONNECTED_); return; }
        fill_state(ARG(1), (int)(ARG(0) & 3));
        ret_std(c, 2, 0);
        return;
    case 0x201667: { /* DWORD XInputSetState(HANDLE, PXINPUT_FEEDBACK): XINPUT_FEEDBACK_HEADER = dwStatus, hEvent,
                      * Reserved[58]; then wLeftMotorSpeed (+0x42), wRightMotorSpeed (+0x44). Completes at once. */
        uint32_t h = ARG(0), fb = ARG(1);
        host_xinput_init();
        if (host_set_state && pad_ok(h)) {
            WORD v[2] = {RD16(fb + 0x42), RD16(fb + 0x44)};
            host_set_state((h - PAD_HANDLE) & 3, v);
        }
        WR32(fb, 0);
        ret_std(c, 2, 997);   /* ERROR_IO_PENDING; dwStatus already says done */
        return;
    }
    case 0x8BE20: { /* int GetKeyboardInput(char *buf) (cdecl): typed chars, for the keyboard handler task only */
        uint32_t buf = ARG(0), n = 0;
        if (RD32(c->esp) != 0xFA688u) { c->eax = 0; c->esp += 4; return; }
        if (!kbd_typing()) LOG("[input] text entry open: typing goes to the game\n");
        kbd_poll_ms = GetTickCount() | 1;
        {   /* UG_TYPE=<s>:<text>[,...] scripted typing (guest seconds; '|' = Enter, '<' = Backspace) */
            static struct { double t; char s[64]; } ev[16];
            static int nev = -1;
            if (nev < 0) {
                nev = 0;
                char b[1024];
                snprintf(b, sizeof b, "%s", getenv("UG_TYPE") ? getenv("UG_TYPE") : "");
                for (char *tok = strtok(b, ","); tok && nev < 16; tok = strtok(NULL, ","))
                    if (sscanf(tok, "%lf:%63[^,]", &ev[nev].t, ev[nev].s) == 2) nev++;
            }
            double now = sched_now_100ns() / 1e7;
            for (int i = 0; i < nev && kbd_typing(); i++) {
                if (!ev[i].s[0] || now < ev[i].t) continue;
                for (const char *p = ev[i].s; *p; p++) kbd_text_push(*p == '|' ? 13 : *p == '<' ? 8 : *p);
                ev[i].s[0] = 0;
            }
        }
        if (kbd_tail != kbd_head) {
            __atomic_thread_fence(__ATOMIC_ACQUIRE);
            WR8(buf, (uint8_t)kbd_q[kbd_tail % sizeof kbd_q]);
            kbd_tail++;
            n = 1;
        }
        c->eax = n;
        c->esp += 4;
        return;
    }
    default:
        if (dsound_hle(c, va)) return;
        rt_fatal("no HLE implementation for %08X (ret %08X)", va, RD32(c->esp));
    }
}
