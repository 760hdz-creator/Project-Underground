/* File system HLE: Xbox object paths -> host files.
 *   \Device\CdRom0\...              -> game/
 *   \Device\Harddisk0\PartitionN\...  -> out/hdd/pN/
 * Symbolic links (\??\D: etc.) come from IoCreateSymbolicLink plus defaults. */
#include <windows.h>
#include <stdlib.h>
#include "rt.h"

#define STATUS_SUCCESS 0x00000000u
#define ST_NO_MORE_FILES 0x80000006u
#define ST_END_OF_FILE 0xC0000011u
#define ST_NOT_FOUND 0xC0000034u
#define ST_PATH_NOT_FOUND 0xC000003Au
#define ST_COLLISION 0xC0000035u
#define ST_INVALID_HANDLE 0xC0000008u
#define ST_NOT_IMPL 0xC0000002u
#define ST_INVALID_PARAM 0xC000000Du

typedef struct {
    HANDLE h;
    int is_dir;
    char guest_path[MAX_PATH];
    char host_path[MAX_PATH];
    HANDLE find;
    int find_done;
    int async; /* opened without FILE_SYNCHRONOUS_IO_*: reads complete as STATUS_PENDING */
} FsFile;

/* ---- symbolic links ---- */
#define MAX_LINKS 32
static struct { char name[64]; char target[MAX_PATH]; } links[MAX_LINKS];
static int nlinks;

static void link_add(const char *name, const char *target) {
    for (int i = 0; i < nlinks; i++) {
        if (!_stricmp(links[i].name, name)) { strcpy(links[i].target, target); return; }
    }
    if (nlinks < MAX_LINKS) {
        strcpy(links[nlinks].name, name);
        strcpy(links[nlinks].target, target);
        nlinks++;
    }
    LOG("[fs] link %s -> %s\n", name, target);
}

static void guest_string(uint32_t str, char *out, size_t cap) {
    uint16_t len = RD16(str);
    uint32_t buf = RD32(str + 4);
    if (len >= cap) len = (uint16_t)(cap - 1);
    memcpy(out, g_mem + buf, len);
    out[len] = 0;
}

/* Expand links until the path starts with \Device\. */
static void normalize(const char *in, char *out) {
    char tmp[MAX_PATH];
    strcpy(tmp, in);
    for (int depth = 0; depth < 8; depth++) {
        int changed = 0;
        for (int i = 0; i < nlinks; i++) {
            size_t n = strlen(links[i].name);
            if (!_strnicmp(tmp, links[i].name, n) && (tmp[n] == 0 || tmp[n] == '\\')) {
                char rest[MAX_PATH];
                strcpy(rest, tmp + n);
                snprintf(tmp, sizeof tmp, "%s%s", links[i].target, rest);
                changed = 1;
                break;
            }
        }
        if (!changed) break;
    }
    strcpy(out, tmp);
}

/* Guest object path -> host path; 0 if not mappable. */
static int to_host(const char *gpath, char *host) {
    char p[MAX_PATH];
    normalize(gpath, p);
    const char *rest = NULL;
    char prefix[MAX_PATH];
    if (!_strnicmp(p, "\\Device\\CdRom0", 14)) {
        strcpy(prefix, "game");
        rest = p + 14;
    } else if (!_strnicmp(p, "\\Device\\Harddisk0\\Partition", 27)) {
        int n = atoi(p + 27);
        snprintf(prefix, sizeof prefix, "out\\hdd\\p%d", n);
        rest = p + 27;
        while (*rest >= '0' && *rest <= '9') rest++;
        CreateDirectoryA("out\\hdd", NULL);
        CreateDirectoryA(prefix, NULL);
    } else {
        return 0;
    }
    if (!strcmp(prefix, "out\\hdd\\p0")) {
        /* Partition0 = raw disk start (config sectors, cache table): a zero-filled image file. */
        strcpy(host, "out\\hdd\\p0.bin");
        if (GetFileAttributesA(host) == INVALID_FILE_ATTRIBUTES) {
            HANDLE h = CreateFileA(host, GENERIC_WRITE, 0, NULL, CREATE_NEW, 0, NULL);
            LARGE_INTEGER sz = {.QuadPart = 0x100000};
            SetFilePointerEx(h, sz, NULL, FILE_BEGIN);
            SetEndOfFile(h);
            CloseHandle(h);
        }
        return 1;
    }
    snprintf(host, MAX_PATH, "%s%s", prefix, rest);
    size_t l = strlen(host);
    while (l && host[l - 1] == '\\') host[--l] = 0;
    return 1;
}

static void obj_attr_path(uint32_t oa, char *out) {
    char name[MAX_PATH];
    guest_string(RD32(oa + 4), name, sizeof name);
    uint32_t root = RD32(oa);
    if (root) {
        Handle *h = handle_get(root);
        if (h && h->kind == H_FILE) {
            FsFile *f = h->host;
            snprintf(out, MAX_PATH, "%s\\%s", f->guest_path, name);
            return;
        }
    }
    /* Drive-relative names ("D:\x", root = ObDosDevicesDirectory) live under \??\. */
    if (name[0] && name[1] == ':') snprintf(out, MAX_PATH, "\\??\\%s", name);
    else strcpy(out, name);
}

static void iosb(uint32_t p, uint32_t status, uint32_t info) {
    if (!p) return;
    WR32(p, status);
    WR32(p + 4, info);
}

/* A save folder that was never filled: empty, or only a 0-byte SaveMeta.xbx. A failed or interrupted XCreateSaveGame
 * (0x1768F4) leaves one, and then refuses its name for good (meta check 0x176818 -> ERROR_CANNOT_MAKE), because only the
 * Xbox dashboard could delete it. */
static int unfilled_save(const char *host) {
    if (_strnicmp(host, "out\\hdd\\p1\\UDATA\\", 17)) return 0;
    char pat[MAX_PATH];
    WIN32_FIND_DATAA fd;
    snprintf(pat, sizeof pat, "%s\\*", host);
    HANDLE f = FindFirstFileA(pat, &fd);
    if (f == INVALID_HANDLE_VALUE) return 0;
    int unfilled = 1;
    do {
        if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) continue;
        if (_stricmp(fd.cFileName, "SaveMeta.xbx") || fd.nFileSizeLow || fd.nFileSizeHigh) unfilled = 0;
    } while (unfilled && FindNextFileA(f, &fd));
    FindClose(f);
    return unfilled;
}

static uint32_t open_common(uint32_t handle_out, uint32_t oa, uint32_t io, uint32_t disposition, uint32_t options) {
    char gpath[MAX_PATH], host[MAX_PATH];
    obj_attr_path(oa, gpath);
    if (getenv("UG_FSLOG")) LOG("[fs] create %s disp %u opts %X\n", gpath, disposition, options);
    if (!to_host(gpath, host)) {
        LOG("[fs] open %s: unmapped path\n", gpath);
        iosb(io, ST_PATH_NOT_FOUND, 0);
        return ST_PATH_NOT_FOUND;
    }
    DWORD attr = GetFileAttributesA(host);
    int exists = attr != INVALID_FILE_ATTRIBUTES;
    int want_dir = (options & 1) != 0; /* FILE_DIRECTORY_FILE */
    int created = 0;
    if (!exists && (disposition == 2 || disposition == 3 || disposition == 5 || disposition == 0) && want_dir) {
        created = CreateDirectoryA(host, NULL) != 0;
        attr = GetFileAttributesA(host);
        exists = attr != INVALID_FILE_ATTRIBUTES;
    }
    int is_dir = exists && (attr & FILE_ATTRIBUTE_DIRECTORY);
    if (is_dir && disposition == 2 && !created && unfilled_save(host)) {
        LOG("[fs] %s: unfilled save folder, reused\n", gpath);
        created = 1;
    }
    HANDLE h = INVALID_HANDLE_VALUE;
    if (is_dir) {
        /* FILE_CREATE on a directory that already existed: collision. One just made here is a success (it used to
         * report a collision too, so XCreateSaveGame took its new folder for an old save and failed: no saves). */
        if (disposition == 2 && !created) { iosb(io, ST_COLLISION, 0); return ST_COLLISION; }
        h = CreateFileA(host, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
                        FILE_FLAG_BACKUP_SEMANTICS, NULL);
    } else {
        static const DWORD disp_map[] = {CREATE_ALWAYS, OPEN_EXISTING, CREATE_NEW, OPEN_ALWAYS, TRUNCATE_EXISTING,
                                         CREATE_ALWAYS};
        DWORD d = disposition < 6 ? disp_map[disposition] : OPEN_EXISTING;
        int readonly = !_strnicmp(host, "game", 4);
        h = CreateFileA(host, readonly ? GENERIC_READ : GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                        NULL, readonly ? OPEN_EXISTING : d, FILE_ATTRIBUTE_NORMAL, NULL);
    }
    if (h == INVALID_HANDLE_VALUE) {
        DWORD e = GetLastError();
        uint32_t st = e == ERROR_FILE_NOT_FOUND ? ST_NOT_FOUND : e == ERROR_FILE_EXISTS ? ST_COLLISION : ST_PATH_NOT_FOUND;
        LOG("[fs] open %s (%s) failed: %08X\n", gpath, host, st);
        iosb(io, st, 0);
        return st;
    }
    FsFile *f = calloc(1, sizeof *f);
    f->h = h;
    f->is_dir = is_dir;
    f->find = INVALID_HANDLE_VALUE;
    f->async = !is_dir && !(options & 0x30); /* FILE_SYNCHRONOUS_IO_ALERT | _NONALERT */
    strcpy(f->guest_path, gpath);
    strcpy(f->host_path, host);
    WR32(handle_out, handle_new(H_FILE, 0, f));
    iosb(io, STATUS_SUCCESS, exists && !created ? 1 : 2); /* FILE_OPENED / FILE_CREATED */
    LOG("[fs] open %s -> %s%s\n", gpath, host, is_dir ? " (dir)" : "");
    script_file_event(gpath);
    return STATUS_SUCCESS;
}

void fs_close(Handle *h) {
    FsFile *f = h->host;
    if (!f) return;
    if (f->find != INVALID_HANDLE_VALUE) FindClose(f->find);
    CloseHandle(f->h);
    free(f);
    h->host = NULL;
}

static FsFile *file_of(uint32_t handle) {
    Handle *h = handle_get(handle);
    return (h && h->kind == H_FILE) ? h->host : NULL;
}

static void signal_event(uint32_t ev_handle) {
    if (!ev_handle) return;
    uint32_t ev = handle_object(ev_handle);
    if (ev) WR32(ev + 4, 1);
}

static uint64_t filetime(FILETIME t) { return ((uint64_t)t.dwHighDateTime << 32) | t.dwLowDateTime; }

/* ---- handlers ---- */
#define K(name) static uint32_t k_##name(X86Context *c)

K(NtOpenFile) {
    /* (*FileHandle, DesiredAccess, ObjectAttributes, IoStatusBlock, ShareAccess, OpenOptions) */
    return open_common(ARG(0), ARG(2), ARG(3), 1, ARG(5));
}
K(NtCreateFile) {
    /* (*FileHandle, DesiredAccess, ObjectAttributes, IoStatusBlock, AllocationSize, FileAttributes, ShareAccess,
     *  CreateDisposition, CreateOptions) */
    return open_common(ARG(0), ARG(2), ARG(3), ARG(7), ARG(8));
}
K(NtReadFile) {
    /* (FileHandle, Event, ApcRoutine, ApcContext, IoStatusBlock, Buffer, Length, *ByteOffset) */
    FsFile *f = file_of(ARG(0));
    if (!f) return ST_INVALID_HANDLE;
    if (ARG(2)) LOG("[fs] NtReadFile with APC routine %08X ignored\n", ARG(2));
    if (ARG(7)) {
        LARGE_INTEGER off;
        off.QuadPart = (LONGLONG)RD64(ARG(7));
        if (off.QuadPart >= 0) SetFilePointerEx(f->h, off, NULL, FILE_BEGIN);
    }
    DWORD got = 0;
    ReadFile(f->h, g_mem + ARG(5), ARG(6), &got, NULL);
    uint32_t st = (got == 0 && ARG(6)) ? ST_END_OF_FILE : STATUS_SUCCESS;
    iosb(ARG(4), st, got);
    signal_event(ARG(1));
    /* Async handles: the data is already in place, but report STATUS_PENDING like the real
     * device does. THUG's cutscene streamer (35700/357F7) takes a synchronous success of an
     * overlapped ReadFile as end of stream and closes the file. EOF stays synchronous. */
    /* Only for OVERLAPPED reads (event or ApcContext given). XAPI's plain ReadFile passes neither; on
     * STATUS_PENDING it waits on the file handle, which kernel.c treats as signaled. */
    int overlapped = ARG(1) || ARG(3);
    uint32_t ret = (f->async && overlapped && st == STATUS_SUCCESS) ? 0x103 /* STATUS_PENDING */ : st;
    static int fslog = -1;
    if (fslog < 0) fslog = getenv("UG_FSLOG") != NULL;
    if (fslog)
        LOG("[fs] read %s len %X off %llX ev %X apc %X/%X -> got %X ret %08X\n", f->guest_path, ARG(6),
            ARG(7) ? (unsigned long long)RD64(ARG(7)) : ~0ull, ARG(1), ARG(2), ARG(3), (unsigned)got, ret);
    return ret;
}
K(NtWriteFile) {
    FsFile *f = file_of(ARG(0));
    if (!f) return ST_INVALID_HANDLE;
    if (ARG(7)) {
        LARGE_INTEGER off;
        off.QuadPart = (LONGLONG)RD64(ARG(7));
        if (off.QuadPart >= 0) SetFilePointerEx(f->h, off, NULL, FILE_BEGIN);
    }
    DWORD put = 0;
    BOOL ok = WriteFile(f->h, g_mem + ARG(5), ARG(6), &put, NULL);
    iosb(ARG(4), STATUS_SUCCESS, put);
    if (getenv("UG_FSLOG")) LOG("[fs] write %s len %X -> %u%s\n", f->guest_path, ARG(6), (unsigned)put, ok ? "" : " (failed)");
    signal_event(ARG(1));
    return STATUS_SUCCESS;
}
K(NtFlushBuffersFile) {
    FsFile *f = file_of(ARG(0));
    if (f) FlushFileBuffers(f->h);
    iosb(ARG(1), STATUS_SUCCESS, 0);
    return STATUS_SUCCESS;
}
K(NtQueryInformationFile) {
    /* (FileHandle, IoStatusBlock, FileInformation, Length, FileInformationClass) */
    FsFile *f = file_of(ARG(0));
    if (!f) return ST_INVALID_HANDLE;
    uint32_t out = ARG(2), cls = ARG(4);
    BY_HANDLE_FILE_INFORMATION bi;
    GetFileInformationByHandle(f->h, &bi);
    uint64_t size = ((uint64_t)bi.nFileSizeHigh << 32) | bi.nFileSizeLow;
    uint64_t alloc = (size + 0xFFF) & ~0xFFFull;
    uint32_t attrs = f->is_dir ? 0x10 : 0x80;
    switch (cls) {
    case 4: /* FileBasicInformation */
        WR64(out, filetime(bi.ftCreationTime)); WR64(out + 8, filetime(bi.ftLastAccessTime));
        WR64(out + 16, filetime(bi.ftLastWriteTime)); WR64(out + 24, filetime(bi.ftLastWriteTime));
        WR32(out + 32, attrs);
        iosb(ARG(1), 0, 40);
        return 0;
    case 5: /* FileStandardInformation */
        WR64(out, alloc); WR64(out + 8, size); WR32(out + 16, 1); WR8(out + 20, 0); WR8(out + 21, (uint8_t)f->is_dir);
        iosb(ARG(1), 0, 24);
        return 0;
    case 14: { /* FilePositionInformation */
        LARGE_INTEGER z = {0}, pos;
        SetFilePointerEx(f->h, z, &pos, FILE_CURRENT);
        WR64(out, (uint64_t)pos.QuadPart);
        iosb(ARG(1), 0, 8);
        return 0;
    }
    case 34: /* FileNetworkOpenInformation */
        WR64(out, filetime(bi.ftCreationTime)); WR64(out + 8, filetime(bi.ftLastAccessTime));
        WR64(out + 16, filetime(bi.ftLastWriteTime)); WR64(out + 24, filetime(bi.ftLastWriteTime));
        WR64(out + 32, alloc); WR64(out + 40, size); WR32(out + 48, attrs);
        iosb(ARG(1), 0, 56);
        return 0;
    default:
        LOG("[fs] NtQueryInformationFile class %u not implemented\n", cls);
        return ST_INVALID_PARAM;
    }
}
K(NtSetInformationFile) {
    /* (FileHandle, IoStatusBlock, FileInformation, Length, FileInformationClass) */
    FsFile *f = file_of(ARG(0));
    if (!f) return ST_INVALID_HANDLE;
    uint32_t in = ARG(2), cls = ARG(4);
    switch (cls) {
    case 14: { /* FilePositionInformation */
        LARGE_INTEGER pos;
        pos.QuadPart = (LONGLONG)RD64(in);
        SetFilePointerEx(f->h, pos, NULL, FILE_BEGIN);
        break;
    }
    case 20: { /* FileEndOfFileInformation */
        LARGE_INTEGER pos, cur, z = {0};
        SetFilePointerEx(f->h, z, &cur, FILE_CURRENT);
        pos.QuadPart = (LONGLONG)RD64(in);
        SetFilePointerEx(f->h, pos, NULL, FILE_BEGIN);
        SetEndOfFile(f->h);
        SetFilePointerEx(f->h, cur, NULL, FILE_BEGIN);
        break;
    }
    case 13: /* FileDispositionInformation */
        if (RD8(in)) {
            CloseHandle(f->h);
            if (f->is_dir) RemoveDirectoryA(f->host_path);
            else DeleteFileA(f->host_path);
            f->h = CreateFileA("NUL", GENERIC_READ, 0, NULL, OPEN_EXISTING, 0, NULL);
        }
        break;
    case 4: /* FileBasicInformation: ignore timestamps/attributes */
        break;
    default:
        LOG("[fs] NtSetInformationFile class %u not implemented\n", cls);
        return ST_INVALID_PARAM;
    }
    iosb(ARG(1), 0, 0);
    return STATUS_SUCCESS;
}
K(NtQueryVolumeInformationFile) {
    /* (FileHandle, IoStatusBlock, FsInformation, Length, FsInformationClass) */
    uint32_t out = ARG(2), cls = ARG(4);
    if (cls == 3) { /* FileFsSizeInformation */
        WR64(out, 0x100000); WR64(out + 8, 0x80000); WR32(out + 16, 32); WR32(out + 20, 512);
        iosb(ARG(1), 0, 24);
        return 0;
    }
    if (cls == 1) { /* FileFsVolumeInformation */
        memset(g_mem + out, 0, ARG(3));
        iosb(ARG(1), 0, 18);
        return 0;
    }
    LOG("[fs] NtQueryVolumeInformationFile class %u not implemented\n", cls);
    return ST_INVALID_PARAM;
}
K(NtQueryDirectoryFile) {
    /* (FileHandle, Event, ApcRoutine, ApcContext, IoStatusBlock, FileInformation, Length, FileInformationClass,
     *  FileMask (PSTRING), RestartScan): one entry per call (FileDirectoryInformation, ANSI name) */
    FsFile *f = file_of(ARG(0));
    if (!f) return ST_INVALID_HANDLE;
    WIN32_FIND_DATAA fd;
    if (f->find == INVALID_HANDLE_VALUE || ARG(9)) {
        char mask[MAX_PATH] = "*", pat[MAX_PATH];
        if (ARG(8)) guest_string(ARG(8), mask, sizeof mask);
        if (f->find != INVALID_HANDLE_VALUE) FindClose(f->find);
        snprintf(pat, sizeof pat, "%s\\%s", f->host_path, mask);
        f->find = FindFirstFileA(pat, &fd);
        f->find_done = f->find == INVALID_HANDLE_VALUE;
    } else if (!FindNextFileA(f->find, &fd)) {
        f->find_done = 1;
    }
    while (!f->find_done && (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, ".."))) {
        if (!FindNextFileA(f->find, &fd)) f->find_done = 1;
    }
    if (f->find_done) {
        iosb(ARG(4), ST_NO_MORE_FILES, 0);
        return ST_NO_MORE_FILES;
    }
    uint32_t out = ARG(5);
    uint32_t nlen = (uint32_t)strlen(fd.cFileName);
    uint64_t size = ((uint64_t)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
    memset(g_mem + out, 0, 64);
    WR64(out + 8, filetime(fd.ftCreationTime)); WR64(out + 16, filetime(fd.ftLastAccessTime));
    WR64(out + 24, filetime(fd.ftLastWriteTime)); WR64(out + 32, filetime(fd.ftLastWriteTime));
    WR64(out + 40, size); WR64(out + 48, (size + 0xFFF) & ~0xFFFull);
    WR32(out + 56, (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? 0x10 : 0x80);
    WR32(out + 60, nlen);
    memcpy(g_mem + out + 64, fd.cFileName, nlen);
    iosb(ARG(4), STATUS_SUCCESS, 64 + nlen);
    signal_event(ARG(1));
    return STATUS_SUCCESS;
}
K(IoCreateSymbolicLink) {
    /* (SymbolicLinkName, DeviceName) */
    char name[MAX_PATH], target[MAX_PATH];
    guest_string(ARG(0), name, sizeof name);
    guest_string(ARG(1), target, sizeof target);
    link_add(name, target);
    return STATUS_SUCCESS;
}
K(NtOpenSymbolicLinkObject) {
    /* (*LinkHandle, ObjectAttributes) */
    char name[MAX_PATH];
    obj_attr_path(ARG(1), name);
    for (int i = 0; i < nlinks; i++) {
        if (!_stricmp(links[i].name, name)) {
            WR32(ARG(0), handle_new(H_OBJECT, 0x80000000u | (uint32_t)i, NULL));
            return STATUS_SUCCESS;
        }
    }
    return ST_NOT_FOUND;
}
K(NtQuerySymbolicLinkObject) {
    /* (LinkHandle, STRING* LinkTarget, *ReturnedLength) */
    Handle *h = handle_get(ARG(0));
    if (!h || !(h->obj & 0x80000000u)) return ST_INVALID_HANDLE;
    const char *t = links[h->obj & 0xFF].target;
    uint32_t s = ARG(1), n = (uint32_t)strlen(t);
    if (n > RD16(s + 2)) n = RD16(s + 2);
    memcpy(g_mem + RD32(s + 4), t, n);
    WR16(s, (uint16_t)n);
    if (ARG(2)) WR32(ARG(2), n);
    return STATUS_SUCCESS;
}
K(NtDeviceIoControlFile) {
    /* (FileHandle, Event, Apc, ApcCtx, IoStatusBlock, IoControlCode, InBuf, InLen, OutBuf, OutLen) */
    if (ARG(5) == 0x4D014 && ARG(6)) { /* IOCTL_SCSI_PASS_THROUGH_DIRECT */
        uint32_t sp = ARG(6);
        LOG("[fs] SCSI cdb %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X len %u buf %08X datain %u (caller %08X)\n",
            RD8(sp + 28), RD8(sp + 29), RD8(sp + 30), RD8(sp + 31), RD8(sp + 32), RD8(sp + 33), RD8(sp + 34),
            RD8(sp + 35), RD8(sp + 36), RD8(sp + 37), RD32(sp + 12), RD32(sp + 20), RD8(sp + 11), RD32(c->esp));
        uint32_t buf = RD32(sp + 20), len = RD32(sp + 12);
        if (RD8(sp + 28) == 0x5A && RD8(sp + 30) == 0x3E && buf && len >= 0x1C) {
            /* MODE SENSE(10) page 0x3E (Xbox DVD security): 8-byte header, then page code, length,
             * partition (game area), authenticated, book type. XAPI checks [10], [11] == 1, [12]. */
            memset(g_mem + buf, 0, len);
            WR8(buf + 1, 0x1A);
            WR8(buf + 8, 0x3E);
            WR8(buf + 9, 0x12);
            WR8(buf + 10, 1);
            WR8(buf + 11, 1);
            WR8(buf + 12, 0xD1);
            iosb(ARG(4), STATUS_SUCCESS, len);
            return STATUS_SUCCESS;
        }
    }
    LOG("[fs] NtDeviceIoControlFile code %08X ignored\n", ARG(5));
    iosb(ARG(4), STATUS_SUCCESS, 0);
    return STATUS_SUCCESS;
}
K(NtFsControlFile) {
    LOG("[fs] NtFsControlFile code %08X ignored\n", ARG(5));
    iosb(ARG(4), STATUS_SUCCESS, 0);
    return STATUS_SUCCESS;
}

K(NtQueryFullAttributesFile) {
    /* (ObjectAttributes, FILE_NETWORK_OPEN_INFORMATION*): Creation/LastAccess/LastWrite/Change times,
     * AllocationSize, EndOfFile (8 bytes each), FileAttributes */
    char gpath[MAX_PATH], host[MAX_PATH];
    obj_attr_path(ARG(0), gpath);
    WIN32_FILE_ATTRIBUTE_DATA d;
    if (!to_host(gpath, host) || !GetFileAttributesExA(host, GetFileExInfoStandard, &d)) return ST_NOT_FOUND;
    uint32_t o = ARG(1);
    uint64_t size = ((uint64_t)d.nFileSizeHigh << 32) | d.nFileSizeLow;
    WR64(o + 0, filetime(d.ftCreationTime));
    WR64(o + 8, filetime(d.ftLastAccessTime));
    WR64(o + 16, filetime(d.ftLastWriteTime));
    WR64(o + 24, filetime(d.ftLastWriteTime));
    WR64(o + 32, (size + 4095) & ~4095ull);
    WR64(o + 40, size);
    WR32(o + 48, (d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL);
    return STATUS_SUCCESS;
}

void fs_register(KHandler *h) {
    h[210] = k_NtQueryFullAttributesFile;
    h[202] = k_NtOpenFile;
    h[190] = k_NtCreateFile;
    h[219] = k_NtReadFile;
    h[236] = k_NtWriteFile;
    h[198] = k_NtFlushBuffersFile;
    h[211] = k_NtQueryInformationFile;
    h[226] = k_NtSetInformationFile;
    h[218] = k_NtQueryVolumeInformationFile;
    h[207] = k_NtQueryDirectoryFile;
    h[67] = k_IoCreateSymbolicLink;
    h[203] = k_NtOpenSymbolicLinkObject;
    h[215] = k_NtQuerySymbolicLinkObject;
    h[196] = k_NtDeviceIoControlFile;
    h[200] = k_NtFsControlFile;
    link_add("\\??\\D:", "\\Device\\CdRom0");
}
