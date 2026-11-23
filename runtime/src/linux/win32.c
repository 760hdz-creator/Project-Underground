/* Linux: the Win32 subset declared in linux/include/windows.h, on POSIX. */
#include <windows.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <time.h>
#ifdef __ANDROID__
#include "rt.h"   /* g_android_relaunch, ug_sched_yield */
#endif
#include <unistd.h>
#undef fopen

/* ---------------- handles ---------------- */
enum { HK_FILE = 0x46494C45, HK_FIND, HK_EVENT, HK_SEM, HK_THREAD };
typedef struct { int kind; } HBase;
typedef struct { int kind; int fd; } HFile;
typedef struct { int kind; DIR *dir; char base[MAX_PATH]; char pat[MAX_PATH]; } HFind;
typedef struct { int kind; pthread_mutex_t m; pthread_cond_t cv; int manual, signaled; LONG count, max; } HSync;
typedef struct { int kind; pthread_t t; LPTHREAD_START_ROUTINE fn; void *arg; } HThread;

static __thread DWORD last_error;
DWORD GetLastError(void) { return last_error; }
static void set_errno_error(void) {
    last_error = errno == ENOENT ? ERROR_FILE_NOT_FOUND : errno == ENOTDIR ? ERROR_PATH_NOT_FOUND
               : errno == EEXIST ? ERROR_FILE_EXISTS : errno == EACCES ? ERROR_ACCESS_DENIED : ERROR_PATH_NOT_FOUND;
}

/* ---------------- paths ---------------- */
void win32_host_path(const char *in, char *out) {
    char p[MAX_PATH];
    size_t n = 0;
    for (const char *s = in; *s && n < MAX_PATH - 1; s++) p[n++] = *s == '\\' ? '/' : *s;
    p[n] = 0;
    if (!strcasecmp(p, "NUL")) { strcpy(out, "/dev/null"); return; }
    /* walk the components; an existing one keeps its spelling, else a case-insensitive directory match */
    char cur[MAX_PATH];
    size_t cl = 0;
    const char *s = p;
    if (*s == '/') { cur[cl++] = '/'; while (*s == '/') s++; }
    cur[cl] = 0;
    while (*s) {
        const char *e = strchr(s, '/');
        size_t len = e ? (size_t)(e - s) : strlen(s);
        char comp[MAX_PATH];
        memcpy(comp, s, len);
        comp[len] = 0;
        char cand[MAX_PATH];
        snprintf(cand, sizeof cand, "%s%s", cur, comp);
        struct stat st;
        if (strcmp(comp, ".") && strcmp(comp, "..") && stat(cand, &st) != 0) {
            DIR *d = opendir(cl ? cur : ".");
            if (d) {
                struct dirent *de;
                while ((de = readdir(d)))
                    if (!strcasecmp(de->d_name, comp)) { snprintf(cand, sizeof cand, "%s%s", cur, de->d_name); break; }
                closedir(d);
            }
        }
        cl = (size_t)snprintf(cur, sizeof cur, "%s", cand);
        s += len;
        if (*s == '/') {
            if (cl < MAX_PATH - 1) { cur[cl++] = '/'; cur[cl] = 0; }
            while (*s == '/') s++;
        }
    }
    while (cl > 1 && cur[cl - 1] == '/') cur[--cl] = 0;
    strcpy(out, cl ? cur : ".");
}

FILE *win32_fopen(const char *path, const char *mode) {
    char h[MAX_PATH];
    win32_host_path(path, h);
    return fopen(h, mode);
}

const char *StrStrIA(const char *hay, const char *needle) { return strcasestr(hay, needle); }

/* ---------------- files ---------------- */
HANDLE CreateFileA(const char *path, DWORD access, DWORD share, void *sa, DWORD disp, DWORD flags, HANDLE tmpl) {
    (void)share; (void)sa; (void)tmpl;
    char h[MAX_PATH];
    win32_host_path(path, h);
    struct stat st;
    int isdir = stat(h, &st) == 0 && S_ISDIR(st.st_mode);
    int o = isdir ? O_RDONLY | O_DIRECTORY
          : (access & GENERIC_WRITE) ? ((access & GENERIC_READ) ? O_RDWR : O_WRONLY) : O_RDONLY;
    if (isdir && !(flags & FILE_FLAG_BACKUP_SEMANTICS)) { last_error = ERROR_ACCESS_DENIED; return INVALID_HANDLE_VALUE; }
    if (!isdir) switch (disp) {
    case CREATE_NEW: o |= O_CREAT | O_EXCL; break;
    case CREATE_ALWAYS: o |= O_CREAT | O_TRUNC; break;
    case OPEN_ALWAYS: o |= O_CREAT; break;
    case TRUNCATE_EXISTING: o |= O_TRUNC; break;
    default: break;
    }
    int fd = open(h, o | O_CLOEXEC, 0644);
    if (fd < 0) { set_errno_error(); return INVALID_HANDLE_VALUE; }
    HFile *f = calloc(1, sizeof *f);
    f->kind = HK_FILE;
    f->fd = fd;
    last_error = 0;
    return f;
}

static int fd_of(HANDLE h) {
    HFile *f = h;
    return (h && h != INVALID_HANDLE_VALUE && f->kind == HK_FILE) ? f->fd : -1;
}

BOOL ReadFile(HANDLE h, void *buf, DWORD n, DWORD *got, void *ov) {
    (void)ov;
    size_t done = 0;
    while (done < n) {
        ssize_t r = read(fd_of(h), (char *)buf + done, n - done);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) break;
        done += (size_t)r;
    }
    if (got) *got = (DWORD)done;
    return TRUE;
}
BOOL WriteFile(HANDLE h, const void *buf, DWORD n, DWORD *put, void *ov) {
    (void)ov;
    size_t done = 0;
    while (done < n) {
        ssize_t r = write(fd_of(h), (const char *)buf + done, n - done);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) break;
        done += (size_t)r;
    }
    if (put) *put = (DWORD)done;
    return done == n;
}
BOOL SetFilePointerEx(HANDLE h, LARGE_INTEGER dist, LARGE_INTEGER *pos, DWORD method) {
    off_t r = lseek(fd_of(h), (off_t)dist.QuadPart, method == FILE_BEGIN ? SEEK_SET : method == FILE_CURRENT ? SEEK_CUR : SEEK_END);
    if (r < 0) return FALSE;
    if (pos) pos->QuadPart = r;
    return TRUE;
}
BOOL SetEndOfFile(HANDLE h) {
    int fd = fd_of(h);
    off_t cur = lseek(fd, 0, SEEK_CUR);
    return cur >= 0 && ftruncate(fd, cur) == 0;
}
BOOL FlushFileBuffers(HANDLE h) { return fsync(fd_of(h)) == 0; }

static FILETIME ft_of(struct timespec ts) {
    uint64_t t = (uint64_t)ts.tv_sec * 10000000ull + (uint64_t)ts.tv_nsec / 100 + 116444736000000000ull;
    return (FILETIME){(DWORD)t, (DWORD)(t >> 32)};
}
static void fill_stat(const struct stat *st, DWORD *attr, FILETIME *c, FILETIME *a, FILETIME *w, DWORD *hi, DWORD *lo) {
    *attr = S_ISDIR(st->st_mode) ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
    *c = ft_of(st->st_mtim);
    *a = ft_of(st->st_atim);
    *w = ft_of(st->st_mtim);
    uint64_t size = S_ISDIR(st->st_mode) ? 0 : (uint64_t)st->st_size;
    *hi = (DWORD)(size >> 32);
    *lo = (DWORD)size;
}
BOOL GetFileInformationByHandle(HANDLE h, BY_HANDLE_FILE_INFORMATION *bi) {
    struct stat st;
    memset(bi, 0, sizeof *bi);
    if (fstat(fd_of(h), &st)) return FALSE;
    fill_stat(&st, &bi->dwFileAttributes, &bi->ftCreationTime, &bi->ftLastAccessTime, &bi->ftLastWriteTime,
              &bi->nFileSizeHigh, &bi->nFileSizeLow);
    return TRUE;
}
DWORD GetFileAttributesA(const char *path) {
    char h[MAX_PATH];
    struct stat st;
    win32_host_path(path, h);
    if (stat(h, &st)) { set_errno_error(); return INVALID_FILE_ATTRIBUTES; }
    return S_ISDIR(st.st_mode) ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
}
BOOL GetFileAttributesExA(const char *path, GET_FILEEX_INFO_LEVELS level, void *out) {
    char h[MAX_PATH];
    struct stat st;
    WIN32_FILE_ATTRIBUTE_DATA *d = out;
    (void)level;
    win32_host_path(path, h);
    if (stat(h, &st)) { set_errno_error(); return FALSE; }
    fill_stat(&st, &d->dwFileAttributes, &d->ftCreationTime, &d->ftLastAccessTime, &d->ftLastWriteTime,
              &d->nFileSizeHigh, &d->nFileSizeLow);
    return TRUE;
}
BOOL CreateDirectoryA(const char *path, void *sa) {
    (void)sa;
    char h[MAX_PATH];
    win32_host_path(path, h);
    if (mkdir(h, 0755)) { last_error = errno == EEXIST ? ERROR_ALREADY_EXISTS : ERROR_PATH_NOT_FOUND; return FALSE; }
    return TRUE;
}
BOOL RemoveDirectoryA(const char *path) { char h[MAX_PATH]; win32_host_path(path, h); return rmdir(h) == 0; }
BOOL DeleteFileA(const char *path) { char h[MAX_PATH]; win32_host_path(path, h); return unlink(h) == 0; }

static BOOL find_next(HFind *f, WIN32_FIND_DATAA *fd) {
    struct dirent *de;
    while ((de = readdir(f->dir))) {
        if (fnmatch(f->pat, de->d_name, FNM_CASEFOLD | FNM_PERIOD) && strcmp(f->pat, "*") && strcmp(f->pat, "*.*"))
            continue;
        char full[MAX_PATH * 2];
        struct stat st;
        snprintf(full, sizeof full, "%s/%s", f->base, de->d_name);
        memset(fd, 0, sizeof *fd);
        if (stat(full, &st) == 0)
            fill_stat(&st, &fd->dwFileAttributes, &fd->ftCreationTime, &fd->ftLastAccessTime, &fd->ftLastWriteTime,
                      &fd->nFileSizeHigh, &fd->nFileSizeLow);
        snprintf(fd->cFileName, sizeof fd->cFileName, "%s", de->d_name);
        return TRUE;
    }
    return FALSE;
}
HANDLE FindFirstFileA(const char *pattern, WIN32_FIND_DATAA *fd) {
    char p[MAX_PATH], dir[MAX_PATH];
    snprintf(p, sizeof p, "%s", pattern);
    for (char *s = p; *s; s++) if (*s == '\\') *s = '/';
    char *slash = strrchr(p, '/');
    const char *pat = slash ? slash + 1 : p;
    if (slash) { *slash = 0; win32_host_path(p[0] ? p : "/", dir); }
    else strcpy(dir, ".");
    HFind *f = calloc(1, sizeof *f);
    f->kind = HK_FIND;
    snprintf(f->pat, sizeof f->pat, "%s", *pat ? pat : "*");
    snprintf(f->base, sizeof f->base, "%s", dir);
    f->dir = opendir(dir);
    if (!f->dir || !find_next(f, fd)) {
        if (f->dir) closedir(f->dir);
        free(f);
        last_error = ERROR_FILE_NOT_FOUND;
        return INVALID_HANDLE_VALUE;
    }
    return f;
}
BOOL FindNextFileA(HANDLE h, WIN32_FIND_DATAA *fd) { return find_next(h, fd); }
BOOL FindClose(HANDLE h) {
    HFind *f = h;
    if (!h || h == INVALID_HANDLE_VALUE) return FALSE;
    closedir(f->dir);
    free(f);
    return TRUE;
}

/* ---------------- time ---------------- */
static uint64_t mono_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}
DWORD GetTickCount(void) { return (DWORD)(mono_ns() / 1000000ull); }
void GetSystemTimeAsFileTime(FILETIME *ft) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    *ft = ft_of(ts);
}
BOOL FileTimeToSystemTime(const FILETIME *ft, SYSTEMTIME *st) {
    uint64_t t = ((uint64_t)ft->dwHighDateTime << 32 | ft->dwLowDateTime);
    time_t secs = (time_t)((t - 116444736000000000ull) / 10000000ull);
    struct tm tm;
    gmtime_r(&secs, &tm);
    *st = (SYSTEMTIME){(WORD)(tm.tm_year + 1900), (WORD)(tm.tm_mon + 1), (WORD)tm.tm_wday, (WORD)tm.tm_mday,
                       (WORD)tm.tm_hour, (WORD)tm.tm_min, (WORD)tm.tm_sec, (WORD)(t / 10000 % 1000)};
    return TRUE;
}
BOOL SystemTimeToFileTime(const SYSTEMTIME *st, FILETIME *ft) {
    if (st->wMonth < 1 || st->wMonth > 12 || st->wDay < 1 || st->wDay > 31 || st->wHour > 23 || st->wMinute > 59 ||
        st->wSecond > 59 || st->wMilliseconds > 999)
        return FALSE;
    struct tm tm = {0};
    tm.tm_year = st->wYear - 1900;
    tm.tm_mon = st->wMonth - 1;
    tm.tm_mday = st->wDay;
    tm.tm_hour = st->wHour;
    tm.tm_min = st->wMinute;
    tm.tm_sec = st->wSecond;
    int64_t secs = (int64_t)timegm(&tm);
    uint64_t t = (uint64_t)(secs * 10000000ll) + 116444736000000000ull + (uint64_t)st->wMilliseconds * 10000ull;
    ft->dwLowDateTime = (DWORD)t;
    ft->dwHighDateTime = (DWORD)(t >> 32);
    return TRUE;
}
BOOL QueryPerformanceCounter(LARGE_INTEGER *c) { c->QuadPart = (LONGLONG)mono_ns(); return TRUE; }
BOOL QueryPerformanceFrequency(LARGE_INTEGER *f) { f->QuadPart = 1000000000ll; return TRUE; }
void Sleep(DWORD ms) {
    if (!ms) { sched_yield(); return; }
    struct timespec ts = {ms / 1000, (long)(ms % 1000) * 1000000l};
    while (nanosleep(&ts, &ts) && errno == EINTR) {}
}

/* ---------------- threads ---------------- */
static void *thread_main(void *p) {
    HThread *t = p;
    t->fn(t->arg);
    return NULL;
}
HANDLE CreateThread(void *sa, SIZE_T stack, LPTHREAD_START_ROUTINE fn, void *arg, DWORD flags, DWORD *tid) {
    (void)sa; (void)flags;
    HThread *t = calloc(1, sizeof *t);
    t->kind = HK_THREAD;
    t->fn = fn;
    t->arg = arg;
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setstacksize(&at, stack > (1 << 20) ? stack : (8u << 20));
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&t->t, &at, thread_main, t)) { pthread_attr_destroy(&at); free(t); return NULL; }
    pthread_attr_destroy(&at);
    if (tid) *tid = 0;
    return t;
}
DWORD GetCurrentThreadId(void) { return (DWORD)syscall(SYS_gettid); }
HANDLE GetCurrentThread(void) { return (HANDLE)(intptr_t)-2; }
DWORD GetCurrentProcessId(void) { return (DWORD)getpid(); }
BOOL SetThreadPriority(HANDLE t, int prio) { (void)t; (void)prio; return TRUE; }
void GetSystemInfo(SYSTEM_INFO *si) { long n = sysconf(_SC_NPROCESSORS_ONLN); si->dwNumberOfProcessors = n > 0 ? (DWORD)n : 1; }
_Noreturn void ExitProcess(UINT code) { fflush(NULL); _exit((int)code); }

void InitializeCriticalSection(CRITICAL_SECTION *cs) {
    pthread_mutexattr_t a;
    pthread_mutexattr_init(&a);
    pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);   /* Win32 critical sections are recursive */
    cs->impl = malloc(sizeof(pthread_mutex_t));
    pthread_mutex_init(cs->impl, &a);
    pthread_mutexattr_destroy(&a);
}
void EnterCriticalSection(CRITICAL_SECTION *cs) { pthread_mutex_lock(cs->impl); }
void LeaveCriticalSection(CRITICAL_SECTION *cs) { pthread_mutex_unlock(cs->impl); }
void DeleteCriticalSection(CRITICAL_SECTION *cs) { pthread_mutex_destroy(cs->impl); free(cs->impl); cs->impl = NULL; }

static HSync *sync_new(int kind) {
    HSync *s = calloc(1, sizeof *s);
    s->kind = kind;
    pthread_mutex_init(&s->m, NULL);
    pthread_condattr_t ca;
    pthread_condattr_init(&ca);
    pthread_condattr_setclock(&ca, CLOCK_MONOTONIC);
    pthread_cond_init(&s->cv, &ca);
    pthread_condattr_destroy(&ca);
    return s;
}
HANDLE CreateEventA(void *sa, BOOL manual, BOOL initial, const char *name) {
    (void)sa; (void)name;
    HSync *s = sync_new(HK_EVENT);
    s->manual = manual;
    s->signaled = initial;
    return s;
}
HANDLE CreateEventW(void *sa, BOOL manual, BOOL initial, const wchar_t *name) { (void)name; return CreateEventA(sa, manual, initial, NULL); }
BOOL SetEvent(HANDLE h) {
    HSync *s = h;
    pthread_mutex_lock(&s->m);
    s->signaled = 1;
    pthread_cond_broadcast(&s->cv);
    pthread_mutex_unlock(&s->m);
    return TRUE;
}
BOOL ResetEvent(HANDLE h) { HSync *s = h; pthread_mutex_lock(&s->m); s->signaled = 0; pthread_mutex_unlock(&s->m); return TRUE; }
HANDLE CreateSemaphoreA(void *sa, LONG initial, LONG max, const char *name) {
    (void)sa; (void)name;
    HSync *s = sync_new(HK_SEM);
    s->count = initial;
    s->max = max;
    return s;
}
BOOL ReleaseSemaphore(HANDLE h, LONG n, LONG *prev) {
    HSync *s = h;
    pthread_mutex_lock(&s->m);
    if (prev) *prev = s->count;
    s->count += n;
    if (s->count > s->max) s->count = s->max;
    pthread_cond_broadcast(&s->cv);
    pthread_mutex_unlock(&s->m);
    return TRUE;
}
DWORD WaitForSingleObject(HANDLE h, DWORD ms) {
    HBase *b = h;
    if (b->kind == HK_THREAD) { pthread_join(((HThread *)h)->t, NULL); return WAIT_OBJECT_0; }
    HSync *s = h;
    struct timespec until;
    clock_gettime(CLOCK_MONOTONIC, &until);
    until.tv_sec += ms / 1000;
    until.tv_nsec += (long)(ms % 1000) * 1000000l;
    if (until.tv_nsec >= 1000000000l) { until.tv_sec++; until.tv_nsec -= 1000000000l; }
    pthread_mutex_lock(&s->m);
    DWORD r = WAIT_OBJECT_0;
    for (;;) {
        if (s->kind == HK_EVENT ? s->signaled : s->count > 0) break;
        if (ms != INFINITE) {
            if (pthread_cond_timedwait(&s->cv, &s->m, &until) == ETIMEDOUT &&
                !(s->kind == HK_EVENT ? s->signaled : s->count > 0)) { r = WAIT_TIMEOUT; break; }
        } else {
            pthread_cond_wait(&s->cv, &s->m);
        }
    }
    if (r == WAIT_OBJECT_0) {
        if (s->kind == HK_EVENT) { if (!s->manual) s->signaled = 0; }
        else s->count--;
    }
    pthread_mutex_unlock(&s->m);
    return r;
}

BOOL CloseHandle(HANDLE h) {
    if (!h || h == INVALID_HANDLE_VALUE || h == (HANDLE)(intptr_t)-2) return FALSE;
    HBase *b = h;
    switch (b->kind) {
    case HK_FILE: close(((HFile *)h)->fd); free(h); return TRUE;
    case HK_EVENT: case HK_SEM: return TRUE;   /* may still be waited on by a detached thread: kept */
    case HK_THREAD: return TRUE;               /* detached; the record stays with the thread */
    default: return FALSE;
    }
}

/* ---------------- relaunch (HalReturnToFirmware) ---------------- */
static char **saved_argv;
void linux_set_argv(char **argv) { saved_argv = argv; }
_Noreturn void linux_relaunch(void) {
    fflush(NULL);
#ifdef __ANDROID__
    g_android_relaunch = 1;   /* runs on a guest fiber: the restart goes through Java from sched_run */
    for (;;) ug_sched_yield();
#endif
    if (saved_argv) execv("/proc/self/exe", saved_argv);
    _exit(0);
}
