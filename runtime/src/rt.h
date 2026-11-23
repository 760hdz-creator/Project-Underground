/* Project Underground runtime: shared declarations. */
#pragma once
#include <stdint.h>
#include <stdio.h>
#include "x86rt.h"

/* ---- guest memory map ---- */
#define RAM_SIZE        0x04000000u   /* 64 MB, also mirrored at 0x80000000 (contiguous/physical alias) */
#define RAM_ALIAS       0x80000000u
#define RAM_ALIAS_WC    0xF0000000u   /* write-combined view D3D hands out for LockRect on surfaces */
#define HEAP_BASE       0x00400000u   /* above the XBE image (THUG ends 0x382480; loader.c checks) */
#define HEAP_END        0x03F00000u
#define KAREA_BASE      0x8F000000u   /* kernel data exports, KPCRs, kernel objects */
#define KAREA_SIZE      0x00400000u
#define KSENTINEL_BASE  0x8FF00000u   /* kernel function "addresses": base + ordinal*4 */
#define HSENTINEL_BASE  0x8FF80000u   /* HLE object vtable entries (dsound.c): base + slot*4 */
#define THREAD_EXIT_VA  0x8FFFF000u   /* return address of a guest thread's start routine */
#define HW_BASE         0xFD000000u   /* NV2A / APU / other MMIO: dummy RAM for now */
#define HW_SIZE         0x03000000u

#define ARG(i) RD32(c->esp + 4 + 4 * (i))

/* logging */
extern int g_log_kernel;
#define LOG(...) do { fprintf(stderr, __VA_ARGS__); fflush(stderr); } while (0)
_Noreturn void rt_fatal(const char *fmt, ...);

/* host gamepads (pad_sdl.c): XINPUT_STATE layout; 0 = ok, 1167 = not connected */
typedef struct {
    uint16_t wButtons; uint8_t bLeftTrigger, bRightTrigger; int16_t sThumbLX, sThumbLY, sThumbRX, sThumbRY;
} HostPad;
typedef struct { uint32_t dwPacketNumber; HostPad Gamepad; } HostState;
int pad_sdl_init(void);   /* 1: SDL3 gamepads in use (Windows: SDL3.dll next to the exe) */
uint32_t pad_sdl_get_state(uint32_t port, HostState *s);
uint32_t pad_sdl_set_state(uint32_t port, uint16_t *vib);   /* left (low frequency), right (high frequency) motor */

/* memory */
void mem_init(void);
uint32_t mem_alloc(uint32_t size, uint32_t align);   /* guest RAM, zeroed; 0 on failure */
void mem_free(uint32_t va);
uint32_t mem_size(uint32_t va);
uint32_t karea_alloc(uint32_t size);                 /* kernel area, zeroed, never freed */

/* loader */
uint32_t xbe_load(const char *path);                 /* returns entry point */
extern uint32_t g_kernel_thunk;
extern uint32_t g_xbe_stack;                         /* stack commit from the XBE header */

/* threads (cooperative fibers) */
typedef struct GThread GThread;
GThread *sched_create_thread(uint32_t start, uint32_t arg1, uint32_t arg2, uint32_t routine, uint32_t stack_size,
                             uint32_t tls_size, int suspended);
GThread *sched_current(void);
uint32_t sched_thread_object(GThread *t);            /* guest KTHREAD address */
GThread *sched_create_host_thread(void (*fn)(X86Context *c));
/* Call a guest stdcall/cdecl function from host code running on a guest thread. */
void guest_call(X86Context *c, uint32_t va, int nargs, const uint32_t *args);
void ug_sched_yield(void);
void sched_maybe_yield(void);                        /* yield if the current slice exceeded 1 ms */                              /* let other threads run */
_Noreturn void sched_exit_thread(uint32_t status);
void sched_run(void);                                 /* main loop; returns when all threads exit */
uint64_t sched_now_100ns(void);                       /* guest time (100 ns units since boot) */
void script_file_event(const char *path);           /* scripted-input file anchors (hle.c) */
void sched_sleep_until(uint64_t t100ns);

/* kernel */
void kernel_init(void);
void kernel_poll(void);                              /* timers -> DPCs; called by the scheduler */
int kernel_call(X86Context *c, uint32_t va);          /* 1 if va was a kernel sentinel */
void kernel_raise_irq(int level);                    /* device interrupt; ISR runs on the DPC thread */
uint32_t kernel_wait(uint32_t *objects, int count, int wait_any, int64_t timeout_100ns, int has_timeout);

/* handles (kernel.c) */
typedef struct { int used; int kind; uint32_t obj; void *host; } Handle;
enum { H_OBJECT = 1, H_FILE };
uint32_t handle_new(int kind, uint32_t obj, void *host);
Handle *handle_get(uint32_t h);
uint32_t handle_object(uint32_t h);
typedef uint32_t (*KHandler)(X86Context *c);

/* DSOUND HLE (dsound.c) */
int dsound_hle(X86Context *c, uint32_t va);          /* 1 if va is a DSOUND C-API entry */
int dsound_vcall(X86Context *c, uint32_t va);        /* 1 if va is a DSOUND object vtable sentinel */

/* NV2A (nv2a.c) */
void nv2a_vblank(void);                              /* called at 60 Hz by kernel_poll */

/* file system (fs.c) */
void fs_register(KHandler *handlers);
void kernel2_register(KHandler *handlers);   /* kernel2.c: exports beyond the Gauntlet set */
void fs_close(Handle *h);

#ifndef _WIN32
/* Linux (linux/win32.c): argv kept for the HalReturnToFirmware relaunch (execv of /proc/self/exe) */
void linux_set_argv(char **argv);
_Noreturn void linux_relaunch(void);
#endif
#ifdef __ANDROID__
/* Android (linux/sdl_plat.c): working directory = the app's external files folder, stderr -> out/gauntlet.log,
 * the window created on the main stack (SDL calls into Java there; Java checks the stack pointer) */
void android_start(void);
void android_relaunch(void);   /* restart the activity (Java), then the process ends */
void android_pad_poll(void);   /* pad_sdl.c: SDL gamepad calls on the main stack (sched_run) */
extern volatile int g_android_relaunch;   /* set by a guest thread; sched_run restarts from the main stack */
/* on-screen controls (linux/touch.c) */
void touch_event(uint32_t type, uint64_t id, float nx, float ny);
void touch_set_pad_present(int present);
void touch_merge(HostState *s);
int touch_shapes(float (*out)[8], int max, int w, int h);
#endif
