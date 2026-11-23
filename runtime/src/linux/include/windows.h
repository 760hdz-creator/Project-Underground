/* Linux: the Win32 subset the runtime uses, on top of DXVK Native's Windows ABI headers (types, COM, HRESULT).
 * Implemented in runtime/src/linux/win32.c (files, threads, sync, time) and linux/sdl_plat.c (window, input).
 * Paths: '\' becomes '/', and each component is matched case-insensitively (the game's paths and the
 * extracted disc's file names differ in case; Windows does not care, Linux does). */
#pragma once
#include_next <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <wchar.h>

#ifndef MAX_PATH
#define MAX_PATH 1024
#endif
#define CALLBACK
#define CP_UTF8 65001
#define INFINITE 0xFFFFFFFFu
#define WAIT_OBJECT_0 0u
#define WAIT_TIMEOUT 258u
#define INVALID_HANDLE_VALUE ((HANDLE)(intptr_t)-1)
#define INVALID_FILE_ATTRIBUTES ((DWORD)-1)
#define FILE_ATTRIBUTE_DIRECTORY 0x10
#define FILE_ATTRIBUTE_NORMAL 0x80
#define GENERIC_READ 0x80000000u
#define GENERIC_WRITE 0x40000000u
#define FILE_SHARE_READ 1
#define FILE_SHARE_WRITE 2
#define CREATE_NEW 1
#define CREATE_ALWAYS 2
#define OPEN_EXISTING 3
#define OPEN_ALWAYS 4
#define TRUNCATE_EXISTING 5
#define FILE_FLAG_BACKUP_SEMANTICS 0x02000000u
#define FILE_BEGIN 0
#define FILE_CURRENT 1
#define FILE_END 2
#define ERROR_FILE_NOT_FOUND 2
#define ERROR_PATH_NOT_FOUND 3
#define ERROR_ACCESS_DENIED 5
#define ERROR_FILE_EXISTS 80
#define ERROR_ALREADY_EXISTS 183
#define THREAD_PRIORITY_BELOW_NORMAL (-1)
#define MONITOR_DEFAULTTONEAREST 2
#define MONITOR_DEFAULTTOPRIMARY 1
#define MONITORINFOF_PRIMARY 1

typedef uint64_t DWORD_PTR;
typedef uintptr_t ULONG_PTR;
typedef intptr_t LONG_PTR;
typedef void *LPVOID;
typedef DWORD (WINAPI *LPTHREAD_START_ROUTINE)(LPVOID);

typedef struct FILETIME { DWORD dwLowDateTime, dwHighDateTime; } FILETIME;
typedef struct SYSTEMTIME { WORD wYear, wMonth, wDayOfWeek, wDay, wHour, wMinute, wSecond, wMilliseconds; } SYSTEMTIME;
typedef struct WIN32_FIND_DATAA {
    DWORD dwFileAttributes;
    FILETIME ftCreationTime, ftLastAccessTime, ftLastWriteTime;
    DWORD nFileSizeHigh, nFileSizeLow;
    char cFileName[MAX_PATH];
} WIN32_FIND_DATAA;
typedef struct BY_HANDLE_FILE_INFORMATION {
    DWORD dwFileAttributes;
    FILETIME ftCreationTime, ftLastAccessTime, ftLastWriteTime;
    DWORD nFileSizeHigh, nFileSizeLow;
} BY_HANDLE_FILE_INFORMATION;
typedef struct WIN32_FILE_ATTRIBUTE_DATA {
    DWORD dwFileAttributes;
    FILETIME ftCreationTime, ftLastAccessTime, ftLastWriteTime;
    DWORD nFileSizeHigh, nFileSizeLow;
} WIN32_FILE_ATTRIBUTE_DATA;
typedef enum { GetFileExInfoStandard } GET_FILEEX_INFO_LEVELS;
#define PAGE_READWRITE 0x04
typedef struct CRITICAL_SECTION { void *impl; } CRITICAL_SECTION;
#include <pthread.h>
typedef pthread_mutex_t SRWLOCK;   /* exclusive use only */
#define SRWLOCK_INIT PTHREAD_MUTEX_INITIALIZER
#define AcquireSRWLockExclusive(l) pthread_mutex_lock(l)
#define ReleaseSRWLockExclusive(l) pthread_mutex_unlock(l)
typedef struct SYSTEM_INFO { DWORD dwNumberOfProcessors; } SYSTEM_INFO;
typedef struct MONITORINFO { DWORD cbSize; RECT rcMonitor, rcWork; DWORD dwFlags; } MONITORINFO;

/* strings */
#define _stricmp strcasecmp
#define _strnicmp strncasecmp
#define _strtoui64 strtoull
#define _strdup strdup
const char *StrStrIA(const char *hay, const char *needle);

/* host paths: '\' -> '/', case-insensitive match of each existing component (result in out, MAX_PATH) */
void win32_host_path(const char *in, char *out);
FILE *win32_fopen(const char *path, const char *mode);
#define fopen win32_fopen

/* files */
HANDLE CreateFileA(const char *path, DWORD access, DWORD share, void *sa, DWORD disposition, DWORD flags, HANDLE tmpl);
BOOL ReadFile(HANDLE h, void *buf, DWORD n, DWORD *got, void *ov);
BOOL WriteFile(HANDLE h, const void *buf, DWORD n, DWORD *put, void *ov);
BOOL SetFilePointerEx(HANDLE h, LARGE_INTEGER dist, LARGE_INTEGER *pos, DWORD method);
BOOL SetEndOfFile(HANDLE h);
BOOL FlushFileBuffers(HANDLE h);
BOOL GetFileInformationByHandle(HANDLE h, BY_HANDLE_FILE_INFORMATION *bi);
DWORD GetFileAttributesA(const char *path);
BOOL GetFileAttributesExA(const char *path, GET_FILEEX_INFO_LEVELS level, void *out);
BOOL CreateDirectoryA(const char *path, void *sa);
BOOL RemoveDirectoryA(const char *path);
BOOL DeleteFileA(const char *path);
HANDLE FindFirstFileA(const char *pattern, WIN32_FIND_DATAA *fd);
BOOL FindNextFileA(HANDLE h, WIN32_FIND_DATAA *fd);
BOOL FindClose(HANDLE h);
BOOL CloseHandle(HANDLE h);
DWORD GetLastError(void);

/* time */
DWORD GetTickCount(void);
void GetSystemTimeAsFileTime(FILETIME *ft);
BOOL FileTimeToSystemTime(const FILETIME *ft, SYSTEMTIME *st);
BOOL SystemTimeToFileTime(const SYSTEMTIME *st, FILETIME *ft);
BOOL QueryPerformanceCounter(LARGE_INTEGER *c);
BOOL QueryPerformanceFrequency(LARGE_INTEGER *f);
void Sleep(DWORD ms);
static inline UINT timeBeginPeriod(UINT p) { (void)p; return 0; }

/* threads and synchronisation */
HANDLE CreateThread(void *sa, SIZE_T stack, LPTHREAD_START_ROUTINE fn, void *arg, DWORD flags, DWORD *tid);
DWORD GetCurrentThreadId(void);
HANDLE GetCurrentThread(void);
DWORD GetCurrentProcessId(void);
BOOL SetThreadPriority(HANDLE t, int prio);
void InitializeCriticalSection(CRITICAL_SECTION *cs);
void EnterCriticalSection(CRITICAL_SECTION *cs);
void LeaveCriticalSection(CRITICAL_SECTION *cs);
void DeleteCriticalSection(CRITICAL_SECTION *cs);
HANDLE CreateEventA(void *sa, BOOL manual, BOOL initial, const char *name);
HANDLE CreateEventW(void *sa, BOOL manual, BOOL initial, const wchar_t *name);
BOOL SetEvent(HANDLE h);
BOOL ResetEvent(HANDLE h);
HANDLE CreateSemaphoreA(void *sa, LONG initial, LONG max, const char *name);
BOOL ReleaseSemaphore(HANDLE h, LONG n, LONG *prev);
DWORD WaitForSingleObject(HANDLE h, DWORD ms);
void GetSystemInfo(SYSTEM_INFO *si);
_Noreturn void ExitProcess(UINT code);
#define InterlockedExchange(p, v) __atomic_exchange_n((p), (v), __ATOMIC_SEQ_CST)
#define InterlockedIncrement(p) __atomic_add_fetch((p), 1, __ATOMIC_SEQ_CST)
#define InterlockedDecrement(p) __atomic_sub_fetch((p), 1, __ATOMIC_SEQ_CST)
#define InterlockedCompareExchange(p, v, cmp) __sync_val_compare_and_swap((p), (cmp), (v))
#define __declspec(x) __declspec_##x
#define __declspec_thread _Thread_local

/* hle.c falls back to LoadLibraryA("xinput*") only on Windows; here it returns NULL (gamepads: pad_sdl.c) */
HMODULE LoadLibraryA(const char *name);
void *GetProcAddress(HMODULE m, const char *name);

/* window, keyboard, monitors (SDL3; an HWND is an SDL_Window *) */
HWND GetForegroundWindow(void);
DWORD GetWindowThreadProcessId(HWND w, DWORD *pid);
SHORT GetAsyncKeyState(int vk);
HMONITOR MonitorFromWindow(HWND w, DWORD flags);
BOOL GetMonitorInfoW(HMONITOR m, MONITORINFO *mi);
BOOL GetClientRect(HWND w, RECT *r);
HWND linux_create_window(void);   /* sdl_plat.c: the game window (UG_MONITOR, UG_FULLSCREEN, size like Windows) */
void linux_pump_events(void);      /* sdl_plat.c: SDL events (close, Esc, F11, Alt+Enter); main thread only */

/* virtual keys used by the runtime (and the launcher's UG_KEYS values) */
#define VK_BACK 0x08
#define VK_TAB 0x09
#define VK_RETURN 0x0D
#define VK_SHIFT 0x10
#define VK_CONTROL 0x11
#define VK_MENU 0x12
#define VK_PAUSE 0x13
#define VK_CAPITAL 0x14
#define VK_ESCAPE 0x1B
#define VK_SPACE 0x20
#define VK_PRIOR 0x21
#define VK_NEXT 0x22
#define VK_END 0x23
#define VK_HOME 0x24
#define VK_LEFT 0x25
#define VK_UP 0x26
#define VK_RIGHT 0x27
#define VK_DOWN 0x28
#define VK_INSERT 0x2D
#define VK_DELETE 0x2E
#define VK_NUMPAD0 0x60
#define VK_NUMPAD1 0x61
#define VK_NUMPAD9 0x69
#define VK_MULTIPLY 0x6A
#define VK_ADD 0x6B
#define VK_SUBTRACT 0x6D
#define VK_DECIMAL 0x6E
#define VK_DIVIDE 0x6F
#define VK_F1 0x70
#define VK_F8 0x77
#define VK_F11 0x7A
#define VK_F12 0x7B
#define VK_LSHIFT 0xA0
#define VK_RSHIFT 0xA1
#define VK_LCONTROL 0xA2
#define VK_RCONTROL 0xA3
#define VK_LMENU 0xA4
#define VK_RMENU 0xA5
#define VK_OEM_1 0xBA
#define VK_OEM_PLUS 0xBB
#define VK_OEM_COMMA 0xBC
#define VK_OEM_MINUS 0xBD
#define VK_OEM_PERIOD 0xBE
#define VK_OEM_2 0xBF
#define VK_OEM_3 0xC0
#define VK_OEM_4 0xDB
#define VK_OEM_5 0xDC
#define VK_OEM_6 0xDD
#define VK_OEM_7 0xDE
