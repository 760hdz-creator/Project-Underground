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

/* ---- host gamepads: XInput, loaded dynamically so the exe runs without it ---- */
static DWORD (WINAPI *host_get_state)(DWORD, void *);
static DWORD (WINAPI *host_set_state)(DWORD, void *);

static void host_xinput_init(void) {
    static int done;
    if (done++) return;
#ifndef _WIN32
    if (pad_sdl_init()) {   /* Linux: SDL3 gamepads (pad_sdl.c), same XINPUT_STATE layout */
        host_get_state = (void *)pad_sdl_get_state;
        host_set_state = (void *)pad_sdl_set_state;
        return;
    }
#endif
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

/* keyboard on port 0: arrows = D-pad, Enter = START, Backspace = BACK, Z/X/C/V = A/B/X/Y, F/G = Black/White,
 * Q/E = LT/RT, WASD = left stick, IJKL = right stick */
static const int kb_dpad[6] = {VK_UP, VK_DOWN, VK_LEFT, VK_RIGHT, VK_RETURN, VK_BACK};
static const int kb_analog[8] = {'Z', 'X', 'C', 'V', 'F', 'G', 'Q', 'E'};

/* host XINPUT_STATE: dwPacketNumber, wButtons, bLeftTrigger, bRightTrigger, sThumbLX LY RX RY */
typedef struct { DWORD packet; WORD buttons; BYTE lt, rt; SHORT lx, ly, rx, ry; } HostXState;

/* guest XINPUT_STATE: dwPacketNumber, wButtons, bAnalogButtons[8] (A B X Y BLACK WHITE LT RT), sThumbLX LY RX RY */
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
    if (port == 0 && our_focus()) {
        for (int i = 0; i < 6; i++) if (key(kb_dpad[i])) buttons |= 1u << i;
        for (int i = 0; i < 8; i++) if (key(kb_analog[i])) analog[i] = 255;
        if (key('W')) h.ly = 32767;
        if (key('S')) h.ly = -32767;
        if (key('A')) h.lx = -32767;
        if (key('D')) h.lx = 32767;
        if (key('I')) h.ry = 32767;
        if (key('K')) h.ry = -32767;
        if (key('J')) h.rx = -32767;
        if (key('L')) h.rx = 32767;
    }
    if (port == 0) {   /* UG_INPUT=<s>:<button>[:<dur s>],... scripted pad 0 input (guest seconds; default 0.15 s) */
        static struct { double t, d; int btn; } ev[256];
        static int nev = -1;
        if (nev < 0) {
            nev = 0;
            static const char *names[] = {"UP", "DOWN", "LEFT", "RIGHT", "START", "BACK", "LS", "RS",
                                          "A", "B", "X", "Y", "BLACK", "WHITE", "LT", "RT",
                                          "LUP", "LDOWN", "LLEFT", "LRIGHT"};
            const char *e = getenv("UG_INPUT");
            char buf[4096];
            snprintf(buf, sizeof buf, "%s", e ? e : "");
            for (char *tok = strtok(buf, ","); tok && nev < 256; tok = strtok(NULL, ",")) {
                char name[16] = {0};
                double t = 0, d = 0.15;
                if (sscanf(tok, "%lf:%15[A-Z]:%lf", &t, name, &d) < 2) continue;
                for (int k = 0; k < 20; k++)
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
            else h.lx = 32767;
        }
    }
    WR32(dst, ++packet);
    WR16(dst + 4, buttons);
    memcpy(g_mem + dst + 6, analog, 8);
    WR16(dst + 14, (uint16_t)h.lx);
    WR16(dst + 16, (uint16_t)h.ly);
    WR16(dst + 18, (uint16_t)h.rx);
    WR16(dst + 20, (uint16_t)h.ry);
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

/* recomp/hooks.txt entry hooks: debugging only (UG_HOOKLOG=1 logs registers and the first stack slots) */
void x86_hook(X86Context *c, uint32_t va) {
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
    default:
        if (dsound_hle(c, va)) return;
        rt_fatal("no HLE implementation for %08X (ret %08X)", va, RD32(c->esp));
    }
}
