/* Update check (forge releases), like Project Nexus' launcher updater.
 * At start a background thread GETs https://git.8gramlabs.com/api/v1/repos/8gramlabs/Project-Underground/releases/latest.
 * When its tag_name ("v0.1.2") is newer than UG_VERSION (the build stamp ug_build.py passes), the player is asked;
 * yes downloads the release asset ProjectUnderground-Setup-*.exe to %TEMP% and runs it with
 * "--update --dir <install dir>" (it rebuilds the game from the installed data and starts it again), then exits.
 * Off for dev builds (no UG_VERSION), scripted test runs (UG_INPUT) and UG_NOUPDATE=1.
 * Testing: UG_UPDATE_REPO=<owner/repo> and UG_UPDATE_ASSET=<asset name prefix> check another forge repo;
 * Linux also takes UG_UPDATE_API=<releases/latest URL> (http allowed while set). */
#include <windows.h>
#ifdef _WIN32
#include <winhttp.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rt.h"

#ifndef UG_VERSION
#define UG_VERSION ""
#endif
#define UG_WIDEN2(x) L##x
#define UG_WIDEN(x) UG_WIDEN2(x)
#define UG_VERSION_W UG_WIDEN(UG_VERSION)

#define UPD_HOST L"git.8gramlabs.com"
#define UPD_PATH L"/api/v1/repos/8gramlabs/Project-Underground/releases/latest"
#define UPD_ASSET "ProjectUnderground-Setup"

#ifdef _WIN32
/* GET https://host/path (host NULL: url is a full https URL). Body to *out (malloc'd) or to file `to`. */
static int http_get(const wchar_t *url, char **out, size_t *len, const wchar_t *to) {
    URL_COMPONENTS uc = {sizeof uc};
    wchar_t host[256], path[2048];
    uc.lpszHostName = host; uc.dwHostNameLength = 256;
    uc.lpszUrlPath = path; uc.dwUrlPathLength = 2048;
    if (!WinHttpCrackUrl(url, 0, 0, &uc)) return 0;
    HINTERNET s = WinHttpOpen(L"ProjectUnderground/" UG_VERSION_W, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, NULL, NULL, 0);
    if (!s) return 0;
    WinHttpSetTimeouts(s, 10000, 10000, 15000, 60000);
    HINTERNET c = WinHttpConnect(s, host, uc.nPort, 0);
    HINTERNET r = c ? WinHttpOpenRequest(c, L"GET", path, NULL, NULL, NULL, WINHTTP_FLAG_SECURE) : NULL;
    int ok = r && WinHttpSendRequest(r, L"Accept: application/json\r\n", (DWORD)-1, NULL, 0, 0, 0) &&
             WinHttpReceiveResponse(r, NULL);
    DWORD code = 0, cl = sizeof code;
    if (ok) WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, NULL, &code, &cl, NULL);
    ok = ok && code == 200;
    FILE *f = NULL;
    if (ok && to) ok = (f = _wfopen(to, L"wb")) != NULL;
    size_t n = 0, cap = 0;
    char *buf = NULL, chunk[65536];
    DWORD got;
    while (ok && WinHttpReadData(r, chunk, sizeof chunk, &got) && got) {
        if (f) { if (fwrite(chunk, 1, got, f) != got) ok = 0; continue; }
        if (n + got + 1 > cap) { cap = (n + got + 1) * 2; buf = realloc(buf, cap); }
        memcpy(buf + n, chunk, got);
        n += got;
    }
    if (f) fclose(f);
    if (buf) buf[n] = 0;
    if (r) WinHttpCloseHandle(r);
    if (c) WinHttpCloseHandle(c);
    WinHttpCloseHandle(s);
    if (out) *out = buf; else free(buf);
    if (len) *len = n;
    return ok;
}

#endif

/* The JSON string value after "key": starting at *pos (advanced past it); unescapes \" \\ \/ \n. */
static int json_str(const char *j, const char *key, size_t *pos, char *dst, size_t cap) {
    char pat[64];
    snprintf(pat, sizeof pat, "\"%s\"", key);
    const char *p = strstr(j + *pos, pat);
    if (!p) return 0;
    p = strchr(p + strlen(pat), '"');
    if (!p) return 0;
    size_t k = 0;
    for (p++; *p && *p != '"'; p++) {
        char ch = *p;
        if (ch == '\\' && p[1]) { p++; ch = *p == 'n' ? '\n' : *p; }
        if (k + 1 < cap) dst[k++] = ch;
    }
    dst[k] = 0;
    *pos = (size_t)(p - j);
    return 1;
}

/* "v1.2.3" / "1.2.3" -> comparable number */
static unsigned long long ver_num(const char *v) {
    unsigned a = 0, b = 0, c = 0;
    if (*v == 'v' || *v == 'V') v++;
    sscanf(v, "%u.%u.%u", &a, &b, &c);
    return ((unsigned long long)a << 40) | ((unsigned long long)b << 20) | c;
}

#ifndef _WIN32
/* Linux: curl does the HTTPS (no TLS library in the game), zenity or kdialog asks; the asset is the Linux
 * package ProjectUnderground-Linux-<ver>.tar.gz, whose setup.sh --update --dir <install> rebuilds and restarts. */
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char **environ;

static int run_argv(char *const argv[], char **out) {
    int pipefd[2] = {-1, -1};
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    if (out) {
        if (pipe(pipefd)) return -1;
        posix_spawn_file_actions_adddup2(&fa, pipefd[1], 1);
        posix_spawn_file_actions_addclose(&fa, pipefd[0]);
    }
    pid_t pid;
    int r = posix_spawnp(&pid, argv[0], &fa, NULL, argv, environ);
    posix_spawn_file_actions_destroy(&fa);
    if (out) close(pipefd[1]);
    if (r) { if (out) close(pipefd[0]); return -1; }
    if (out) {
        size_t n = 0, cap = 65536;
        char *buf = malloc(cap);
        ssize_t got;
        while ((got = read(pipefd[0], buf + n, cap - n - 1)) > 0) {
            n += (size_t)got;
            if (cap - n < 4096) { cap *= 2; buf = realloc(buf, cap); }
        }
        close(pipefd[0]);
        buf[n] = 0;
        *out = buf;
    }
    int st = 0;
    waitpid(pid, &st, 0);
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

/* yes/no dialog: 1 yes, 0 no, -1 no dialog tool */
static int ask(const char *title, const char *msg) {
    char *z[] = {"zenity", "--question", "--title", (char *)title, "--text", (char *)msg, "--width=480", NULL};
    int r = run_argv(z, NULL);
    if (r == 0 || r == 1) return r == 0;
    char *k[] = {"kdialog", "--title", (char *)title, "--yesno", (char *)msg, NULL};
    r = run_argv(k, NULL);
    if (r == 0 || r == 1) return r == 0;
    return -1;
}

static DWORD WINAPI update_thread(void *arg) {
    (void)arg;
    char *body = NULL;
    const char *repo = getenv("UG_UPDATE_REPO"), *asset = getenv("UG_UPDATE_ASSET");
    if (!asset) asset = "ProjectUnderground-Linux";
    char api[512];
    snprintf(api, sizeof api, "https://git.8gramlabs.com/api/v1/repos/%s/releases/latest",
             repo ? repo : "8gramlabs/Project-Underground");
    /* UG_UPDATE_API=<releases/latest URL>: test override; plain http is allowed only while it is set */
    const char *test_api = getenv("UG_UPDATE_API");
    if (test_api && *test_api) snprintf(api, sizeof api, "%s", test_api);
    char *proto = test_api && *test_api ? "=http,https" : "=https";
    char *get[] = {"curl", "-sfL", "--proto", proto, "--proto-redir", proto, "--max-time", "15", "-H",
                   "Accept: application/json", api, NULL};
    if (run_argv(get, &body) != 0 || !body || !*body) {
        LOG("[update] no release found (or curl missing)\n");
        free(body);
        return 0;
    }
    char tag[64] = "", notes[1024] = "", url[1024] = "", name[256];
    size_t pos = 0;
    json_str(body, "tag_name", &pos, tag, sizeof tag);
    pos = 0;
    json_str(body, "body", &pos, notes, sizeof notes);
    pos = 0;
    while (json_str(body, "name", &pos, name, sizeof name)) {
        if (!strncmp(name, asset, strlen(asset)) && strstr(name, ".tar.gz")) {
            json_str(body, "browser_download_url", &pos, url, sizeof url);
            break;
        }
    }
    free(body);
    if (!tag[0] || !url[0] || ver_num(tag) <= ver_num(UG_VERSION)) {
        LOG("[update] up to date (installed %s, latest %s)\n", UG_VERSION, tag[0] ? tag : "none");
        return 0;
    }
    LOG("[update] %s available (installed %s): %s\n", tag, UG_VERSION, url);
    char msg[1600];
    snprintf(msg, sizeof msg, "Project Underground %s is available (you have %s).\n\n%.900s\n\n"
             "Update now? The game closes, the update rebuilds it (a few minutes) and starts it again.",
             tag, UG_VERSION, notes);
    if (ask("Project Underground update", msg) != 1) return 0;
    char tmp[] = "/tmp/underground-update-XXXXXX";
    if (!mkdtemp(tmp)) return 0;
    char tgz[MAX_PATH], dir[MAX_PATH], setup[MAX_PATH];
    snprintf(tgz, sizeof tgz, "%s/update.tar.gz", tmp);
    char *dl[] = {"curl", "-sfL", "--proto", proto, "--proto-redir", proto, "--max-time", "600", "-o", tgz, url, NULL};
    char *untar[] = {"tar", "-xzf", tgz, "-C", tmp, "--strip-components=1", NULL};
    if (run_argv(dl, NULL) != 0 || run_argv(untar, NULL) != 0) {
        LOG("[update] download failed\n");
        return 0;
    }
    if (!getcwd(dir, sizeof dir)) return 0;   /* the game runs from its install folder */
    snprintf(setup, sizeof setup, "%s/setup.sh", tmp);
    char *up[] = {"/bin/sh", setup, "--update", "--dir", dir, "--play", NULL};
    pid_t pid;
    posix_spawnattr_t at;
    posix_spawnattr_init(&at);
    posix_spawnattr_setflags(&at, POSIX_SPAWN_SETSID);
    if (posix_spawn(&pid, up[0], NULL, &at, up, environ) == 0) ExitProcess(0);
    return 0;
}
#else
static DWORD WINAPI update_thread(void *arg) {
    (void)arg;
    char *body = NULL;
    const char *repo = getenv("UG_UPDATE_REPO"), *asset = getenv("UG_UPDATE_ASSET");
    if (!asset) asset = UPD_ASSET;
    wchar_t api[512];
    if (repo) swprintf(api, 512, L"https://" UPD_HOST L"/api/v1/repos/%hs/releases/latest", repo);
    else wcscpy(api, L"https://" UPD_HOST UPD_PATH);
    if (!http_get(api, &body, NULL, NULL) || !body) {
        LOG("[update] no release found\n");
        free(body);
        return 0;
    }
    char tag[64] = "", notes[1024] = "", url[1024] = "", name[256];
    size_t pos = 0;
    json_str(body, "tag_name", &pos, tag, sizeof tag);
    pos = 0;
    json_str(body, "body", &pos, notes, sizeof notes);
    pos = 0;
    while (json_str(body, "name", &pos, name, sizeof name)) {   /* assets: "name" ... "browser_download_url" */
        if (!strncmp(name, asset, strlen(asset)) && strstr(name, ".exe")) {
            json_str(body, "browser_download_url", &pos, url, sizeof url);
            break;
        }
    }
    free(body);
    if (!tag[0] || !url[0] || ver_num(tag) <= ver_num(UG_VERSION)) {
        LOG("[update] up to date (installed %s, latest %s)\n", UG_VERSION, tag[0] ? tag : "none");
        return 0;
    }
    LOG("[update] %s available (installed %s): %s\n", tag, UG_VERSION, url);
    char msg[1600];
    snprintf(msg, sizeof msg, "Project Underground %s is available (you have %s).\n\n%.900s\n\n"
             "Update now? The game closes, the update rebuilds it (a few minutes) and starts it again.",
             tag, UG_VERSION, notes);
    if (MessageBoxA(NULL, msg, "Project Underground update", MB_YESNO | MB_ICONINFORMATION | MB_TOPMOST | MB_SETFOREGROUND)
        != IDYES)
        return 0;
    wchar_t wurl[1024], tmp[MAX_PATH], setup[MAX_PATH], dir[MAX_PATH], cmd[2 * MAX_PATH + 64];
    MultiByteToWideChar(CP_UTF8, 0, url, -1, wurl, 1024);
    GetTempPathW(MAX_PATH, tmp);
    swprintf(setup, MAX_PATH, L"%sProjectUnderground-Setup-%hs.exe", tmp, tag);
    if (!http_get(wurl, NULL, NULL, setup)) {
        MessageBoxA(NULL, "The update could not be downloaded. Try again later, or download it from\n"
                    "https://git.8gramlabs.com/8gramlabs/Project-Underground/releases", "Project Underground update",
                    MB_OK | MB_ICONWARNING | MB_TOPMOST);
        return 0;
    }
    GetModuleFileNameW(NULL, dir, MAX_PATH);
    wchar_t *slash = wcsrchr(dir, L'\\');
    if (slash) *slash = 0;
    swprintf(cmd, sizeof cmd / sizeof cmd[0], L"\"%s\" --update --dir \"%s\"", setup, dir);
    STARTUPINFOW si = {sizeof si};
    PROCESS_INFORMATION pi;
    if (CreateProcessW(setup, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
        CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
        ExitProcess(0);
    }
    return 0;
}

#endif

void updater_start(void) {
#ifdef __ANDROID__
    return;   /* a new version is a new APK, built on the PC */
#endif
    const char *no = getenv("UG_NOUPDATE");
    if (!UG_VERSION[0] || getenv("UG_INPUT") || (no && *no == '1')) return;
    HANDLE t = CreateThread(NULL, 0, update_thread, NULL, 0, NULL);
    if (t) CloseHandle(t);
}
