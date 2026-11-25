/* Android: on-screen controls for player 1 (drawn by gpu_d3d11.c touch_draw, merged into pad 1 by pad_sdl.c).
 * Left side: a floating stick (touch anywhere in the left 40% below the top band; the base sits where the finger
 * lands). Right side: A (bottom, green), B (right, red), X (left, blue), Y (top, yellow); White and Black to
 * their left; LT / RT in the top corners; BACK / START at the top centre. A finger can slide between buttons.
 * The controls hide while a real controller is in slot 1. Sizes are in units of the screen height. */
#ifdef __ANDROID__
#include <SDL3/SDL.h>
#include <math.h>
#include <string.h>
#include "rt.h"

#define MAX_FINGERS 10
typedef struct { uint64_t id; float x, y; int used, stick; } Finger;
static Finger fingers[MAX_FINGERS];
static float scr_w = 3120, scr_h = 1440;
static float stick_ox, stick_oy;   /* base of the stick while held */
static int pad_present;

enum { B_A, B_B, B_X, B_Y, B_WHITE, B_BLACK, B_LT, B_RT, B_BACK, B_START, B_MENU, NBTN };
typedef struct { float x, y, r; uint16_t bit; float cr, cg, cb; } Button;
static Button btn[NBTN];
static float stick_r, stick_dx, stick_dy;

static void layout(void) {
    float W = scr_w, H = scr_h, fx = W - 0.26f * H, fy = 0.70f * H, o = 0.135f * H, r = 0.075f * H;
    btn[B_A] = (Button){fx, fy + o, r, 0x1000, 0.25f, 0.80f, 0.30f};
    btn[B_B] = (Button){fx + o, fy, r, 0x2000, 0.90f, 0.25f, 0.25f};
    btn[B_X] = (Button){fx - o, fy, r, 0x4000, 0.25f, 0.45f, 0.95f};
    btn[B_Y] = (Button){fx, fy - o, r, 0x8000, 0.95f, 0.80f, 0.20f};
    btn[B_WHITE] = (Button){fx - 2.35f * o, fy - 0.55f * o, 0.06f * H, 0x0100, 0.95f, 0.95f, 0.95f};
    btn[B_BLACK] = (Button){fx - 2.35f * o, fy + 0.75f * o, 0.06f * H, 0x0200, 0.20f, 0.20f, 0.22f};
    btn[B_LT] = (Button){0.14f * H, 0.13f * H, 0.085f * H, 0, 0.70f, 0.70f, 0.72f};
    btn[B_RT] = (Button){W - 0.14f * H, 0.13f * H, 0.085f * H, 0, 0.70f, 0.70f, 0.72f};
    btn[B_BACK] = (Button){W / 2 - 0.14f * H, 0.08f * H, 0.05f * H, 0x0020, 0.60f, 0.60f, 0.62f};
    btn[B_START] = (Button){W / 2 + 0.14f * H, 0.08f * H, 0.05f * H, 0x0010, 0.60f, 0.60f, 0.62f};
    btn[B_MENU] = (Button){W / 2, 0.08f * H, 0.05f * H, 0, 0.94f, 0.47f, 0.16f};   /* our in-game menu */
    stick_r = 0.16f * H;
}

static int pressed(int b) {
    for (int i = 0; i < MAX_FINGERS; i++) {
        if (!fingers[i].used || fingers[i].stick) continue;
        float dx = fingers[i].x - btn[b].x, dy = fingers[i].y - btn[b].y, rr = btn[b].r * 1.3f;
        if (dx * dx + dy * dy <= rr * rr) return 1;
    }
    return 0;
}

static void update_stick(void) {
    stick_dx = stick_dy = 0;
    for (int i = 0; i < MAX_FINGERS; i++)
        if (fingers[i].used && fingers[i].stick) {
            float dx = fingers[i].x - stick_ox, dy = fingers[i].y - stick_oy, d = sqrtf(dx * dx + dy * dy);
            if (d > stick_r) { dx *= stick_r / d; dy *= stick_r / d; }
            stick_dx = dx / stick_r;
            stick_dy = dy / stick_r;
        }
}

void touch_event(uint32_t type, uint64_t id, float nx, float ny) {
    if (!btn[B_A].r) layout();
    float x = nx * scr_w, y = ny * scr_h;
    extern int menu_is_open(void);
    extern void menu_tap(int x, int y, int w, int h);
    extern volatile int g_menu_req;
    if (menu_is_open()) {   /* taps go to the menu; nothing stays held */
        memset(fingers, 0, sizeof fingers);
        update_stick();
        if (type == SDL_EVENT_FINGER_DOWN) menu_tap((int)x, (int)y, (int)scr_w, (int)scr_h);
        return;
    }
    if (type == SDL_EVENT_FINGER_DOWN && !pad_present) {
        float dx = x - btn[B_MENU].x, dy = y - btn[B_MENU].y, rr = btn[B_MENU].r * 1.3f;
        if (dx * dx + dy * dy <= rr * rr) { g_menu_req = 1; return; }
    }
    int k = -1;
    for (int i = 0; i < MAX_FINGERS; i++) if (fingers[i].used && fingers[i].id == id) k = i;
    if (type == SDL_EVENT_FINGER_DOWN) {
        if (k < 0) for (int i = 0; i < MAX_FINGERS && k < 0; i++) if (!fingers[i].used) k = i;
        if (k < 0) return;
        int stick_taken = 0;
        for (int i = 0; i < MAX_FINGERS; i++) stick_taken |= fingers[i].used && fingers[i].stick;
        int on_button = 0;
        for (int b = 0; b < NBTN; b++) {
            float dx = x - btn[b].x, dy = y - btn[b].y, rr = btn[b].r * 1.3f;
            on_button |= dx * dx + dy * dy <= rr * rr;
        }
        fingers[k] = (Finger){id, x, y, 1, !stick_taken && !on_button && x < 0.4f * scr_w && y > 0.25f * scr_h};
        if (fingers[k].stick) {   /* the base where the finger lands, kept fully on screen */
            stick_ox = fminf(fmaxf(x, stick_r), 0.4f * scr_w);
            stick_oy = fminf(fmaxf(y, stick_r), scr_h - stick_r);
        }
    } else if (k >= 0) {
        fingers[k].x = x;
        fingers[k].y = y;
        if (type != SDL_EVENT_FINGER_MOTION) fingers[k].used = 0;
    }
    update_stick();
}

void touch_set_pad_present(int present) { pad_present = present; }

/* player 1's state: the touch controls on top of the real pad (if any) */
void touch_merge(HostState *s) {
    if (!btn[B_A].r) layout();
    for (int b = 0; b < NBTN; b++) {
        if (!pressed(b)) continue;
        s->Gamepad.wButtons |= btn[b].bit;
        if (b == B_LT) s->Gamepad.bLeftTrigger = 255;
        if (b == B_RT) s->Gamepad.bRightTrigger = 255;
    }
    if (stick_dx || stick_dy) {
        s->Gamepad.sThumbLX = (int16_t)(stick_dx * 32767);
        s->Gamepad.sThumbLY = (int16_t)(-stick_dy * 32767);   /* XInput: up is positive */
    }
}

/* shapes for the overlay: x, y, radius, ring width (0 = disc), r, g, b, a (pixels of a w x h back buffer) */
int touch_shapes(float (*out)[8], int max, int w, int h) {
    if (w != (int)scr_w || h != (int)scr_h) { scr_w = (float)w; scr_h = (float)h; layout(); }
    if (!btn[B_A].r) layout();
    extern int menu_is_open(void);
    if (pad_present || menu_is_open()) return 0;
    int n = 0;
    for (int b = 0; b < NBTN && n < max; b++) {
        float a = pressed(b) ? 0.60f : 0.30f;
        out[n][0] = btn[b].x; out[n][1] = btn[b].y; out[n][2] = btn[b].r; out[n][3] = 0;
        out[n][4] = btn[b].cr; out[n][5] = btn[b].cg; out[n][6] = btn[b].cb; out[n][7] = a;
        n++;
    }
    for (int d = -1; d <= 1 && n < max; d++) {   /* three dots on the menu button */
        float dot[8] = {btn[B_MENU].x + d * 0.4f * btn[B_MENU].r, btn[B_MENU].y, 0.11f * btn[B_MENU].r, 0, 1, 1, 1, 0.85f};
        memcpy(out[n++], dot, sizeof dot);
    }
    int held = 0;
    for (int i = 0; i < MAX_FINGERS; i++) held |= fingers[i].used && fingers[i].stick;
    float bx = held ? stick_ox : 0.28f * scr_h, by = held ? stick_oy : scr_h - 0.28f * scr_h;
    if (n + 2 <= max) {
        float ring[8] = {bx, by, stick_r, 0.012f * scr_h, 0.85f, 0.85f, 0.90f, held ? 0.55f : 0.30f};
        memcpy(out[n++], ring, sizeof ring);
        float knob[8] = {bx + stick_dx * stick_r, by + stick_dy * stick_r, 0.07f * scr_h, 0, 0.85f, 0.85f, 0.90f,
                         held ? 0.60f : 0.30f};
        memcpy(out[n++], knob, sizeof knob);
    }
    return n;
}
#endif
