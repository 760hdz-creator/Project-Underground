/* In-game menu (F1, Esc, or L3+R3 on a pad): pauses the game and changes settings while playing.
 *
 * It runs as a modal loop at the NV2A flip (nv2a.c): guest threads are fibers on one host thread, so while the loop
 * runs no guest code does, the whole game is paused. Every guest clock comes from sched_now_100ns, which leaves the
 * paused time out (sched_pause_add); the mixer holds (dsound.c g_audio_hold), so sounds and streams resume where
 * they stopped. The renderer re-presents the frozen frame with the menu panel copied over it (gpu_menu_present).
 *
 * Input while open: keys queued from the window thread (menu_key: arrows, Enter/Space, Esc/F1/Backspace) and pad 0
 * read directly (menu_pad_buttons, hle.c). Before the game resumes, every key and button must be released, so the
 * press that closed the menu never reaches the game. Changes are written to settings.ini (the launcher's keys) when
 * that file exists, i.e. in an installed game.
 *
 * Text: font8x8_basic.h (public domain, Daniel Hepper / IBM VGA), scaled to the window.
 * Debug: UG_INPUT "<s>:MENU" opens it; UG_MENUKEYS=down,right,ok,... feeds keys (one every 300 ms; any other word
 * just waits) once open;
 * UG_MENUSHOT=1 writes each drawn panel to out/menu.ppm. */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rt.h"
#include "font8x8_basic.h"

enum { MK_UP = 1, MK_DOWN, MK_LEFT, MK_RIGHT, MK_OK, MK_BACK };

volatile int g_menu_req;                 /* set by F1 / Esc / L3+R3 / UG_INPUT MENU: open at the next flip */
static volatile int g_menu_open;
static volatile LONG mkq[32];
static volatile LONG mk_head, mk_tail;   /* window thread writes head, the menu loop reads tail */

int menu_is_open(void) { return g_menu_open; }
void menu_key(int k) {
    if (!g_menu_open || mk_head - mk_tail >= 32) return;
    mkq[mk_head % 32] = k;
    InterlockedIncrement(&mk_head);
}
static int menu_key_next(void) {
    if (mk_tail == mk_head) return 0;
    int k = (int)mkq[mk_tail % 32];
    InterlockedIncrement(&mk_tail);
    return k;
}

/* settings (hle.c, gpu_vulkan.c, nv2a.c, sdl_plat.c) */
extern volatile int g_invert_x, g_invert_y, g_hd_hidden, g_fps;
extern int hdtex_enabled(void);
extern volatile int g_fxaa, g_ao;
extern volatile float g_aniso;
int gpu_has_aniso(void);
extern volatile int g_scale_req;
int gpu_scale(void);
extern volatile int g_drawdist;
int drawdist_on(void);
int plat_fullscreen_get(void);
void plat_fullscreen_toggle(void);
uint32_t menu_pad_buttons(void);         /* pad 0, XInput layout, stick folded into the D-pad bits (hle.c) */
int menu_keys_down(void);                /* any menu key still held (hle.c) */
void gpu_menu_present(const uint8_t *rgba, int w, int h);
int gpu_window_height(void);
extern volatile int g_audio_hold;

/* settings.ini [settings] key=value, written only when the file exists (installed game; the launcher made it) */
static void ini_set(const char *key, const char *val) {
    FILE *f = fopen("settings.ini", "rb");
    if (!f) return;
    static char in[65536], out[65536 + 256];
    size_t n = fread(in, 1, sizeof in - 1, f);
    fclose(f);
    in[n] = 0;
    size_t o = 0, klen = strlen(key);
    int section = 0, done = 0;
    for (char *line = in; *line;) {
        char *end = strchr(line, '\n');
        size_t len = end ? (size_t)(end - line + 1) : strlen(line);
        int header = line[0] == '[';
        if (header && section && !done) {   /* leaving [settings]: add the key */
            if (*val) o += (size_t)snprintf(out + o, sizeof out - o, "%s=%s\r\n", key, val);
            done = 1;
        }
        if (header) section = !strncmp(line, "[settings]", 10);
        if (section && !header && !strncmp(line, key, klen) && line[klen] == '=') {
            if (*val && !done) o += (size_t)snprintf(out + o, sizeof out - o, "%s=%s\r\n", key, val);
            done = 1;
        } else if (o + len < sizeof out) {
            memcpy(out + o, line, len);
            o += len;
        }
        line += len;
    }
    if (!done && *val) {
        if (!section) o += (size_t)snprintf(out + o, sizeof out - o, "%s[settings]\r\n", o && out[o - 1] != '\n' ? "\r\n" : "");
        else if (o && out[o - 1] != '\n') o += (size_t)snprintf(out + o, sizeof out - o, "\r\n");
        o += (size_t)snprintf(out + o, sizeof out - o, "%s=%s\r\n", key, val);
    }
    f = fopen("settings.ini", "wb");
    if (!f) return;
    fwrite(out, 1, o, f);
    fclose(f);
}

/* ------------------------------------------------------------------ drawing */
static uint8_t *px;
static int pw, ph, sc;   /* panel size, font scale */

static void fill(int x, int y, int w, int h, uint32_t rgb) {
    for (int j = y < 0 ? 0 : y; j < y + h && j < ph; j++)
        for (int i = x < 0 ? 0 : x; i < x + w && i < pw; i++) {
            uint8_t *p = px + ((size_t)j * pw + i) * 4;
            p[0] = (uint8_t)(rgb >> 16); p[1] = (uint8_t)(rgb >> 8); p[2] = (uint8_t)rgb; p[3] = 255;
        }
}
static void text(int x, int y, const char *s, uint32_t rgb) {
    for (; *s; s++, x += 8 * sc) {
        const unsigned char *g = (const unsigned char *)font8x8_basic[(unsigned char)*s & 127];
        for (int r = 0; r < 8; r++)
            for (int b = 0; b < 8; b++)
                if (g[r] >> b & 1) {
                    fill(x + b * sc + sc / 2, y + r * sc + sc / 2, sc, sc, 0x000000);   /* drop shadow */
                    fill(x + b * sc, y + r * sc, sc, sc, rgb);
                }
    }
}

/* ------------------------------------------------------------------ items */
enum { IT_RESUME, IT_FULLSCREEN, IT_FPS, IT_INVX, IT_INVY, IT_SCALE, IT_FXAA, IT_AO, IT_ANISO, IT_DRAWDIST, IT_HD, IT_UNLOCK, IT_SPECIAL, IT_RAIL, IT_MANUAL, IT_MOON,
       IT_SKITCH, IT_QUIT, IT_COUNT };
static int sel, quit_armed;

/* Cheats. The game's own cheat flags are career global flags (SetGlobalFlag 0x1298B0 -> 0x113820): a bitset at
 * career + 0x584, career = [[0x2FCD68] + 0x20]. Numbers from scripts/game/misc/cheats.qb + global_flags.qb
 * (CHEAT_ALWAYS_SPECIAL = CHEAT_ON_2 = 186 ...). The game is paused while the menu runs, so they are set directly.
 * "Unlock everything" runs the game's own script unlock_all_cheats (levels, skaters, movies, cheat codes,
 * chapters, sponsors, cutscenes); a script needs a guest thread, so hle.c runs it at the next XInputGetState. */
volatile int g_unlock_req;
static int unlock_done;
static uint32_t career(void) {
    uint32_t sk = RD32(0x2FCD68u);   /* heap pointers are 0x80xxxxxx (RAM mirror) */
    return sk ? RD32(sk + 0x20) : 0;
}
static int cheat_flag(int i) {
    switch (i) {
    /* CHEAT_ON_n = 184 + n for n = 1..15 (global_flags.qb) */
    case IT_SPECIAL: return 186;   /* CHEAT_ALWAYS_SPECIAL = CHEAT_ON_2 */
    case IT_RAIL: return 187;      /* CHEAT_PERFECT_RAIL = CHEAT_ON_3 */
    case IT_MANUAL: return 191;    /* CHEAT_PERFECT_MANUAL = CHEAT_ON_7 */
    case IT_MOON: return 193;      /* CHEAT_MOON = CHEAT_ON_9 */
    case IT_SKITCH: return 196;    /* CHEAT_PERFECT_SKITCH = CHEAT_ON_12 */
    }
    return -1;
}
static int flag_get(int f) { uint32_t ca = career(); return ca && (RD32(ca + 0x584 + (uint32_t)(f >> 5) * 4) >> (f & 31) & 1); }
static void flag_set(int f, int on) {
    uint32_t ca = career(), a = ca + 0x584 + (uint32_t)(f >> 5) * 4;
    if (!ca) return;
    WR32(a, on ? RD32(a) | 1u << (f & 31) : RD32(a) & ~(1u << (f & 31)));
    LOG("[menu] cheat flag %d %s\n", f, on ? "on" : "off");
}

static int on_x(void) {
    if (g_invert_x >= 0) return g_invert_x;
    const char *e = getenv("UG_INVERTX");
    return !(e && *e == '0');   /* flipped by default; UG_INVERTX=0 = original direction */
}
static int on_y(void) {
    if (g_invert_y >= 0) return g_invert_y;
    const char *e = getenv("UG_INVERTY");
    return !(e && *e == '0');   /* flipped by default; UG_INVERTY=0 = original direction */
}
static int item_shown(int i) {
    if (i == IT_HD) return hdtex_enabled();
    if (i == IT_ANISO) return gpu_has_aniso();
    if (i >= IT_UNLOCK && i <= IT_SKITCH) return career() != 0;
    return 1;
}

static const char *item_label(int i, char *buf, size_t n) {
    switch (i) {
    case IT_RESUME: return "Resume";
    case IT_FULLSCREEN: snprintf(buf, n, "Fullscreen         < %s >", plat_fullscreen_get() ? "On " : "Off"); return buf;
    case IT_FPS: snprintf(buf, n, "Frame rate         < %d >", g_fps == 30 ? 30 : 60); return buf;
    case IT_INVX: snprintf(buf, n, "Camera left/right  < %s >", on_x() ? "PC  " : "Xbox"); return buf;
    case IT_INVY: snprintf(buf, n, "Camera up/down     < %s >", on_y() ? "PC  " : "Xbox"); return buf;
    case IT_SCALE: snprintf(buf, n, "Supersampling      < %dx >", gpu_scale()); return buf;
    case IT_FXAA: snprintf(buf, n, "Anti-aliasing      < %s >", g_fxaa ? "On " : "Off"); return buf;
    case IT_AO: snprintf(buf, n, "Ambient occlusion  < %s >", g_ao ? "On " : "Off"); return buf;
    case IT_ANISO:
        if (g_aniso > 0) snprintf(buf, n, "Texture filtering  < %dx >", (int)g_aniso);
        else snprintf(buf, n, "Texture filtering  < Off >");
        return buf;
    case IT_DRAWDIST: snprintf(buf, n, "Draw distance      < %s >", drawdist_on() ? "Max" : "Xbox"); return buf;
    case IT_HD: snprintf(buf, n, "HD textures        < %s >", g_hd_hidden ? "Off" : "On "); return buf;
    case IT_UNLOCK: return unlock_done ? "Unlock everything: done" : "Unlock everything";
    case IT_SPECIAL: case IT_RAIL: case IT_MANUAL: case IT_MOON: case IT_SKITCH: {
        static const char *nm[] = {"Always special", "Perfect rails", "Perfect manuals", "Moon gravity", "Perfect skitch"};
        snprintf(buf, n, "%-19s< %s >", nm[i - IT_SPECIAL], flag_get(cheat_flag(i)) ? "On " : "Off");
        return buf;
    }
    case IT_QUIT: return quit_armed ? "Quit game: press again" : "Quit game";
    }
    return "";
}

/* returns 1 to close the menu */
static int item_change(int i, int dir) {
    switch (i) {
    case IT_RESUME: return dir == 0;
    case IT_FULLSCREEN:
    {
        int was = plat_fullscreen_get();
        plat_fullscreen_toggle();
        ini_set("UG_FULLSCREEN", was ? "" : "1");
        break;
    }
    case IT_FPS:
        g_fps = g_fps == 30 ? 60 : 30;
        ini_set("UG_FPS", g_fps == 30 ? "30" : "");
        break;
    case IT_INVX: g_invert_x = !on_x(); ini_set("UG_INVERTX", g_invert_x ? "" : "0"); break;
    case IT_INVY: g_invert_y = !on_y(); ini_set("UG_INVERTY", g_invert_y ? "" : "0"); break;
    case IT_SCALE: {   /* 1..6, 8 (default 2); shown once the menu closes */
        int v = gpu_scale(), steps[] = {1, 2, 3, 4, 5, 6, 8}, k = 0;
        while (k < 6 && steps[k] < v) k++;
        v = steps[(k + (dir < 0 ? 6 : 1)) % 7];
        g_scale_req = v;
        char val[8];
        snprintf(val, sizeof val, "%d", v);
        ini_set("UG_SCALE", v == 2 ? "" : val);
        break;
    }
    case IT_FXAA: g_fxaa = !g_fxaa; ini_set("UG_FXAA", g_fxaa ? "1" : ""); break;
    case IT_AO: g_ao = !g_ao; ini_set("UG_AO", g_ao ? "1" : ""); break;
    case IT_ANISO: {   /* off, 2, 4, 8, 16 (default 16) */
        int a = (int)g_aniso, steps[] = {0, 2, 4, 8, 16}, k = 0;
        while (k < 4 && steps[k] < a) k++;
        a = steps[(k + (dir < 0 ? 4 : 1)) % 5];
        g_aniso = (float)a;
        char val[8];
        snprintf(val, sizeof val, "%d", a);
        ini_set("UG_ANISO", a == 16 ? "" : val);
        break;
    }
    case IT_DRAWDIST: g_drawdist = !drawdist_on(); ini_set("UG_DRAWDIST", g_drawdist ? "1" : ""); break;
    case IT_HD: g_hd_hidden = !g_hd_hidden; break;   /* session only: the launcher's UG_HDTEX holds the pack folder */
    case IT_UNLOCK:
        if (dir == 0 && !unlock_done) { g_unlock_req = 1; unlock_done = 1; }
        break;
    case IT_SPECIAL: case IT_RAIL: case IT_MANUAL: case IT_MOON: case IT_SKITCH:
        flag_set(cheat_flag(i), !flag_get(cheat_flag(i)));
        break;
    case IT_QUIT:
        if (dir) break;
        if (quit_armed) { LOG("[menu] quit\n"); ExitProcess(0); }
        quit_armed = 1;
        return 0;
    }
    quit_armed = 0;
    return 0;
}

static void draw(void) {
    int vis[IT_COUNT], nv = 0;
    for (int i = 0; i < IT_COUNT; i++) if (item_shown(i)) vis[nv++] = i;
    int H = gpu_window_height();
    sc = H >= 1400 ? 4 : H >= 900 ? 3 : 2;
    int cols = 30, line = 14 * sc, pad = 12 * sc;
    pw = cols * 8 * sc + 2 * pad;
    ph = (nv + 3 + (career() ? 1 : 0)) * line + 2 * pad;
    static size_t cap;
    if ((size_t)pw * ph * 4 > cap) { free(px); cap = (size_t)pw * ph * 4; px = malloc(cap); }
    if (!px) return;
    fill(0, 0, pw, ph, 0xE0601C);                       /* border */
    fill(sc, sc, pw - 2 * sc, ph - 2 * sc, 0x16161A);    /* panel */
    text(pad, pad, "PAUSED", 0xF07828);
    char buf[64];
    for (int k = 0, gap = 0; k < nv; k++) {
        int i = vis[k];
        if (i == IT_UNLOCK) {   /* cheats section */
            gap = 1;
            text(pad, pad + (k + 2) * line, "CHEATS", 0xF07828);
        }
        if (i == IT_QUIT && gap == 1) gap = 2;
        int y = pad + (k + 2 + (gap ? 1 : 0)) * line;
        if (i == sel) fill(pad - 4 * sc, y - 3 * sc, pw - 2 * pad + 8 * sc, 14 * sc, 0x3A2416);
        text(pad, y, item_label(i, buf, sizeof buf), i == sel ? 0xFFFFFF : 0xB4B4BE);
    }
    text(pad, ph - pad - 8 * sc, "A/Enter ok  B/Esc back", 0x8C8C96);
    static int dump = -1;
    if (dump < 0) dump = getenv("UG_MENUSHOT") != NULL;
    if (dump) {
        FILE *f = fopen("out/menu.ppm", "wb");
        if (f) {
            fprintf(f, "P6 %d %d 255\n", pw, ph);
            for (int i = 0; i < pw * ph; i++) fwrite(px + i * 4, 1, 3, f);
            fclose(f);
        }
    }
}

static int step(int dir) {   /* next visible item */
    int i = sel;
    do i = (i + dir + IT_COUNT) % IT_COUNT; while (!item_shown(i));
    return i;
}

void menu_run(void) {
    g_menu_req = 0;
    double t0 = host_ms();
    g_audio_hold = 1;
    g_menu_open = 1;
    mk_tail = mk_head;
    sel = IT_RESUME;
    quit_armed = 0;
    LOG("[menu] open\n");
    uint32_t prev = menu_pad_buttons();   /* buttons held now (L3+R3 that opened it) count only once released */
    const char *script = getenv("UG_MENUKEYS");
    double next_script = host_ms() + 600;
    int dirty = 1, closing = 0;
    double last_draw = 0;
    while (!closing) {
#ifndef _WIN32
        extern void linux_pump_events(void);
        linux_pump_events();
#endif
        int k = menu_key_next();
        if (!k && script && *script && host_ms() >= next_script) {   /* UG_MENUKEYS (debug) */
            static const char *names[] = {"", "up", "down", "left", "right", "ok", "back"};
            for (int i = 1; i <= 6; i++)
                if (!strncmp(script, names[i], strlen(names[i]))) { k = i; break; }
            script = strchr(script, ',');
            script = script ? script + 1 : NULL;
            next_script = host_ms() + 300;
        }
        uint32_t b = menu_pad_buttons();
        uint32_t press = b & ~prev;
        prev = b;
        if (!k) {
            if (press & 0x0001) k = MK_UP;
            else if (press & 0x0002) k = MK_DOWN;
            else if (press & 0x0004) k = MK_LEFT;
            else if (press & 0x0008) k = MK_RIGHT;
            else if (press & 0x1000) k = MK_OK;                 /* A */
            else if (press & (0x2000 | 0x0010 | 0x0020)) k = MK_BACK;   /* B, START, BACK */
        }
        switch (k) {
        case MK_UP: sel = step(-1); quit_armed = 0; dirty = 1; break;
        case MK_DOWN: sel = step(1); quit_armed = 0; dirty = 1; break;
        case MK_LEFT: case MK_RIGHT: if (sel != IT_RESUME && sel != IT_QUIT) item_change(sel, k == MK_LEFT ? -1 : 1); dirty = 1; break;
        case MK_OK: closing = item_change(sel, 0); dirty = 1; break;
        case MK_BACK: closing = 1; break;
        }
        if (closing) break;
        if (dirty || host_ms() - last_draw > 250) {   /* redraw on change; re-present for window resizes / toggles */
            draw();   /* also picks up a fullscreen state that changed asynchronously */
            gpu_menu_present(px, pw, ph);
            last_draw = host_ms();
            dirty = 0;
        }
        Sleep(10);
    }
    g_menu_open = 0;
    /* nothing that closed the menu may reach the game: wait for every key and button to be released (max 2 s) */
    for (double t = host_ms(); host_ms() - t < 2000 && (menu_pad_buttons() || menu_keys_down());) {
#ifndef _WIN32
        extern void linux_pump_events(void);
        linux_pump_events();
#endif
        Sleep(10);
    }
    double paused = host_ms() - t0;
    sched_pause_add((uint64_t)(paused * 10000.0));
    g_audio_hold = 0;
    LOG("[menu] closed after %.1f s\n", paused / 1000.0);
}
