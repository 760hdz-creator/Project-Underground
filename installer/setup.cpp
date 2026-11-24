// Project Underground installer. Ships no game code or data: it installs the build kit (our lifter, runtime source,
// embedded Python) and builds the game on this PC from the player's own Tony Hawk's Underground (Xbox, USA) disc image.
//   1. unpack the embedded payload.zip (System32\tar.exe) into the install folder
//   2. python\python.exe -I tools\ug_build.py --root <dir> --iso <image> (extract, check, fetch the compiler
//      llvm-mingw into <dir>\toolchain on the first install, lift, compile, link)
//   3. shortcuts, per-user record HKCU\Software\ProjectUnderground, uninstall entry
// Over an existing install it offers an update: new kit + rebuild from the installed game data (no disc needed).
// Command line: --update [--dir <folder>] (what the game's updater runs), --uninstall [--dir <folder>],
// --iso <image> [--dir <folder>] (full install, starts at once: testing).
// The install keeps a copy of this exe as <install>\launcher.exe; "--launcher" opens the game launcher (Play, update
// check, display settings) instead of the setup. Settings go to <install>\settings.ini [settings] as UG_* environment
// variable names, which the game reads at start (runtime/src/main.c load_settings).
// Built with llvm-mingw (installer\build.py): clang++ -municode -mwindows, static, no Visual Studio.
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#ifndef WINVER
#define WINVER 0x0A00
#endif
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <uxtheme.h>
#include <wincodec.h>
#include <winhttp.h>
#include <gdiplus.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <thread>
#include <vector>
#include "resource.h"

#ifndef UG_SETUP_VERSION
#define UG_SETUP_VERSION "0.0.0"
#endif
#define W2(x) L##x
#define W1(x) W2(x)
static const wchar_t kVersion[] = W1(UG_SETUP_VERSION);
static const wchar_t kAppName[] = L"Project Underground";
static const wchar_t kRegKey[] = L"Software\\ProjectUnderground";
static const wchar_t kUninstKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\ProjectUnderground";
static const wchar_t kShortcut[] = L"Project Underground.lnk";

enum { ID_ISO_BROWSE = 200, ID_DIR_BROWSE, ID_DESKTOP, ID_STARTMENU, ID_INSTALL, ID_PROGRESS,
       ID_STATUS, ID_PLAY, ID_UPDATE, ID_UNINSTALL, ID_FULLSCREEN, ID_SCALE, ID_PADS, ID_FPS60, ID_VSYNC, ID_LOGS,
       ID_MONITOR, ID_ANDROID, ID_SETTINGS, ID_QUIT, ID_CLOSE, ID_WINSIZE, ID_SCREEN, ID_FIT, ID_LEVEL, ID_HD, ID_CAMERA };
#define WM_APP_STATUS (WM_APP + 1)
#define WM_APP_PROGRESS (WM_APP + 2)
#define WM_APP_DONE (WM_APP + 3)
#define WM_APP_UPDATE (WM_APP + 4)   // launcher: update check finished (wp 1 = newer version in g_upd)
#define WM_APP_UPDATING (WM_APP + 5) // launcher: wp 1 = new setup started (close), 0 = download failed
#define WM_APP_ANDROID (WM_APP + 6)  // launcher: Android APK build finished (wp 1 = ok; g_error / g_apk)

static HWND g_wnd, g_status, g_progress, g_install, g_updBtn;
static bool g_updating, g_running, g_launcher;
static std::wstring g_instDir;   // launcher: the install folder (the exe's own folder)
// Buttons are owner-drawn on the art (DrawButton; the kinds are its K_* bits). GWLP_USERDATA | 4: a ticked toggle.
static bool IsOn(int id) { return (GetWindowLongPtrW(GetDlgItem(g_wnd, id), GWLP_USERDATA) & 4) != 0; }
static void SetOn(int id, bool on) {
    HWND c = GetDlgItem(g_wnd, id);
    LONG_PTR v = GetWindowLongPtrW(c, GWLP_USERDATA);
    SetWindowLongPtrW(c, GWLP_USERDATA, on ? v | 4 : v & ~4);
    InvalidateRect(c, nullptr, FALSE);
}
static std::wstring g_error, g_doneDir;
static std::thread g_worker;

// ---- small helpers ----
static void PostStatus(const std::wstring& s) { if (g_wnd) PostMessageW(g_wnd, WM_APP_STATUS, 0, (LPARAM) new std::wstring(s)); }
static void PostProgress(int pct) { if (g_wnd) PostMessageW(g_wnd, WM_APP_PROGRESS, pct, 0); }
static std::wstring Widen(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), w.data(), n);
    return w;
}
static std::wstring KnownDir(REFKNOWNFOLDERID id) {
    PWSTR p = nullptr;
    std::wstring r;
    if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &p))) r = p;
    CoTaskMemFree(p);
    return r;
}
static bool Exists(const std::wstring& p) { return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES; }
static void SetReg(HKEY root, const wchar_t* key, const wchar_t* name, const std::wstring& v) {
    HKEY k;
    if (RegCreateKeyExW(root, key, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &k, nullptr) == ERROR_SUCCESS) {
        RegSetValueExW(k, name, 0, REG_SZ, (const BYTE*)v.c_str(), DWORD((v.size() + 1) * 2));
        RegCloseKey(k);
    }
}
static std::wstring GetReg(const wchar_t* name) {
    wchar_t buf[1024];
    DWORD n = sizeof buf;
    if (RegGetValueW(HKEY_CURRENT_USER, kRegKey, name, RRF_RT_REG_SZ, nullptr, buf, &n) == ERROR_SUCCESS) return buf;
    return L"";
}
static bool WriteResource(int id, const std::wstring& path) {
    HRSRC r = FindResourceW(nullptr, MAKEINTRESOURCEW(id), RT_RCDATA);
    HGLOBAL g = r ? LoadResource(nullptr, r) : nullptr;
    if (!g) return false;
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD w = 0, n = SizeofResource(nullptr, r);
    bool ok = WriteFile(f, LockResource(g), n, &w, nullptr) && w == n;
    CloseHandle(f);
    return ok;
}
static void MakeShortcut(const std::wstring& lnk, const std::wstring& target, const std::wstring& workDir,
                         const wchar_t* args = L"") {
    IShellLinkW* sl = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&sl)))) return;
    sl->SetPath(target.c_str());
    sl->SetArguments(args);
    sl->SetWorkingDirectory(workDir.c_str());
    sl->SetIconLocation(target.c_str(), 0);
    IPersistFile* pf = nullptr;
    if (SUCCEEDED(sl->QueryInterface(IID_PPV_ARGS(&pf)))) { pf->Save(lnk.c_str(), TRUE); pf->Release(); }
    sl->Release();
}
// settings.ini next to the game: [settings] holds UG_* variables for the game.
// An empty value deletes the key (the game then uses its default, so a later default change reaches the player).
static std::wstring IniGet(const std::wstring& dir, const wchar_t* sec, const wchar_t* key) {
    wchar_t b[1024];
    GetPrivateProfileStringW(sec, key, L"", b, 1024, (dir + L"\\settings.ini").c_str());
    return b;
}
static void IniSet(const std::wstring& dir, const wchar_t* sec, const wchar_t* key, const std::wstring& v) {
    WritePrivateProfileStringW(sec, key, v.empty() ? nullptr : v.c_str(), (dir + L"\\settings.ini").c_str());
}
static std::wstring ExeDir() {
    wchar_t self[MAX_PATH];
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    std::wstring d = self;
    return d.substr(0, d.find_last_of(L'\\'));
}
// Copies this exe to <dir>\launcher.exe. Retries for 15 s: the launcher that started this update may still be closing.
static bool InstallLauncher(const std::wstring& dir) {
    wchar_t self[MAX_PATH];
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    std::wstring l = dir + L"\\launcher.exe";
    if (!_wcsicmp(self, l.c_str())) return true;
    for (int i = 0; i < 30; i++) {
        if (CopyFileW(self, l.c_str(), FALSE)) return true;
        Sleep(500);
    }
    return false;
}

// Runs cmd (hidden), feeding each output line to onLine. Returns the exit code (or -1).
template <class F> static int RunLines(std::wstring cmd, const std::wstring& cwd, F onLine) {
    SECURITY_ATTRIBUTES sa = {sizeof sa, nullptr, TRUE};
    HANDLE rd, wr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return -1;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si = {sizeof si};
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput = si.hStdError = wr;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION pi;
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr,
                        cwd.empty() ? nullptr : cwd.c_str(), &si, &pi)) {
        CloseHandle(rd); CloseHandle(wr);
        return -1;
    }
    CloseHandle(wr);
    std::string buf;
    char chunk[4096];
    DWORD n;
    while (ReadFile(rd, chunk, sizeof chunk, &n, nullptr) && n) {
        buf.append(chunk, n);
        size_t p;
        while ((p = buf.find('\n')) != std::string::npos) {
            std::string line = buf.substr(0, p);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            onLine(line);
            buf.erase(0, p + 1);
        }
    }
    if (!buf.empty()) onLine(buf);
    CloseHandle(rd);
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
    return (int)code;
}

// ---- install / update worker ----
static bool Unpack(const std::wstring& dir) {
    PostStatus(L"Copying files...");
    CreateDirectoryW(dir.c_str(), nullptr);
    SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);
    std::wstring zip = dir + L"\\payload.zip";
    if (!WriteResource(IDR_PAYLOAD, zip)) return false;
    wchar_t sys[MAX_PATH];
    GetSystemDirectoryW(sys, MAX_PATH);
    int code = RunLines(L"\"" + std::wstring(sys) + L"\\tar.exe\" -xf \"" + zip + L"\" -C \"" + dir + L"\"", L"",
                        [](const std::string&) {});
    DeleteFileW(zip.c_str());
    WriteResource(IDR_SETUP_ICON, dir + L"\\underground.ico");
    return code == 0;
}

static void Work(std::wstring iso, std::wstring dir, bool desktop, bool startMenu, bool update) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    g_error.clear();
    auto done = [&](bool ok) { g_doneDir = dir; PostMessageW(g_wnd, WM_APP_DONE, ok, 0); CoUninitialize(); };
    PostProgress(2);
    if (!Unpack(dir)) { g_error = L"Could not write the files to " + dir + L"."; return done(false); }
    PostProgress(8);
    // ug_build.py fetches its own compiler (llvm-mingw, into <dir>\toolchain) on the first install: no admin, no VS
    std::wstring cmd = L"\"" + dir + L"\\python\\python.exe\" -I \"" + dir + L"\\tools\\ug_build.py\" --root \"" + dir +
                       L"\" --version " + kVersion;
    if (!update) cmd += L" --iso \"" + iso + L"\"";
    // weights: extract 10-28, check 28-30, compiler 30-40, lift 40-55, compile 55-95, link 95-99
    int base = 8;
    CreateDirectoryW((dir + L"\\build").c_str(), nullptr);
    HANDLE log = CreateFileW((dir + L"\\build\\setup.log").c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                             CREATE_ALWAYS, 0, nullptr);
    std::wstring err;
    int code = RunLines(cmd, dir, [&](const std::string& l) {
        DWORD w;
        if (log != INVALID_HANDLE_VALUE) { WriteFile(log, l.data(), (DWORD)l.size(), &w, nullptr); WriteFile(log, "\r\n", 2, &w, nullptr); }
        if (!l.compare(0, 3, "## ")) {
            std::wstring s = Widen(l.substr(3));
            PostStatus(s + L"...");
            if (s.rfind(L"Extracting", 0) == 0) base = 10;
            else if (s.rfind(L"Checking", 0) == 0) base = 28;
            else if (s.rfind(L"Getting", 0) == 0) base = 30;
            else if (s.rfind(L"Translating", 0) == 0) base = 40;
            else if (s.rfind(L"Compiling", 0) == 0) base = 55;
            else if (s.rfind(L"Linking", 0) == 0) base = 95;
            else if (s.rfind(L"Done", 0) == 0) base = 99;
            PostProgress(base);
        } else if (!l.compare(0, 7, "ERROR: ")) {
            err = Widen(l.substr(7));
        } else if (base == 55 && !l.compare(0, 9, "compiled ")) {   // the runtime's own "compiled" lines come after Linking
            int a = 0, b = 0;
            size_t p = l.rfind('(');
            if (p != std::string::npos && sscanf(l.c_str() + p, "(%d/%d)", &a, &b) == 2 && b)
                PostProgress(55 + 40 * a / b);
        }
    });
    if (log != INVALID_HANDLE_VALUE) CloseHandle(log);
    if (code != 0 || !Exists(dir + L"\\underground.exe")) {
        g_error = (err.empty() ? L"The build failed." : err) + L"\n\nLog: " + dir + L"\\build\\setup.log";
        return done(false);
    }
    // the launcher (a copy of this setup) is what the shortcuts and the uninstall entry run
    if (!InstallLauncher(dir)) {
        g_error = L"Could not write " + dir + L"\\launcher.exe (is the launcher still open?).";
        return done(false);
    }
    std::wstring un = dir + L"\\launcher.exe";
    // shortcuts: the ticked ones, and on an update the existing ones
    std::wstring desk = KnownDir(FOLDERID_Desktop) + L"\\" + kShortcut, sm = KnownDir(FOLDERID_Programs) + L"\\" + kShortcut;
    if (desktop || Exists(desk)) MakeShortcut(desk, un, dir, L"--launcher");
    if (startMenu || Exists(sm)) MakeShortcut(sm, un, dir, L"--launcher");
    SetReg(HKEY_CURRENT_USER, kRegKey, L"InstallLocation", dir);
    SetReg(HKEY_CURRENT_USER, kRegKey, L"Version", kVersion);
    SetReg(HKEY_CURRENT_USER, kUninstKey, L"DisplayName", kAppName);
    SetReg(HKEY_CURRENT_USER, kUninstKey, L"DisplayVersion", kVersion);
    SetReg(HKEY_CURRENT_USER, kUninstKey, L"Publisher", L"8gram Labs");
    SetReg(HKEY_CURRENT_USER, kUninstKey, L"DisplayIcon", dir + L"\\underground.ico");
    SetReg(HKEY_CURRENT_USER, kUninstKey, L"InstallLocation", dir);
    SetReg(HKEY_CURRENT_USER, kUninstKey, L"UninstallString", L"\"" + un + L"\" --uninstall --dir \"" + dir + L"\"");
    PostProgress(100);
    done(true);
}

// ---- Android: tools\ug_android.py builds an APK from this install (NDK downloaded once into <install>\android) ----
static std::wstring g_apk;
static HWND g_androidBtn;
static void BuildAndroid() {
    g_error.clear();
    g_apk.clear();
    std::wstring cmd = L"\"" + g_instDir + L"\\python\\python.exe\" -I \"" + g_instDir + L"\\tools\\ug_android.py\" --root \"" +
                       g_instDir + L"\" --accept-ndk-license";
    CreateDirectoryW((g_instDir + L"\\build").c_str(), nullptr);
    HANDLE log = CreateFileW((g_instDir + L"\\build\\android.log").c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                             CREATE_ALWAYS, 0, nullptr);
    std::wstring err;
    int code = RunLines(cmd, g_instDir, [&](const std::string& l) {
        DWORD w;
        if (log != INVALID_HANDLE_VALUE) { WriteFile(log, l.data(), (DWORD)l.size(), &w, nullptr); WriteFile(log, "\r\n", 2, &w, nullptr); }
        if (!l.compare(0, 3, "## ")) PostStatus(L"Android: " + Widen(l.substr(3)) + L"...");
        else if (!l.compare(0, 7, "ERROR: ")) err = Widen(l.substr(7));
        else if (!l.compare(0, 5, "APK: ")) g_apk = Widen(l.substr(5));
        else if (!l.compare(0, 9, "compiled ") || !l.compare(0, 11, "downloaded ") || !l.compare(0, 9, "unpacked ") ||
                 !l.compare(0, 7, "packed ")) {
            PostStatus(L"Android: " + Widen(l) + L"...");
        }
    });
    if (log != INVALID_HANDLE_VALUE) CloseHandle(log);
    bool ok = code == 0 && !g_apk.empty() && Exists(g_apk);
    if (!ok) g_error = (err.empty() ? L"The Android build failed." : err) + L"\n\nLog: " + g_instDir + L"\\build\\android.log";
    PostMessageW(g_wnd, WM_APP_ANDROID, ok, 0);
}

static int Uninstall(const std::wstring& dir) {
    if (dir.empty() || !Exists(dir + L"\\underground.ico")) {
        MessageBoxW(nullptr, L"Project Underground is not installed.", kAppName, MB_OK);
        return 1;
    }
    if (MessageBoxW(nullptr, (L"Remove Project Underground from " + dir + L"?\n\nYour saves (out\\hdd) are kept.").c_str(),
                    kAppName, MB_YESNO | MB_ICONQUESTION) != IDYES)
        return 1;
    // shortcuts and registry entries are shared: only remove them when they belong to this folder
    wchar_t a[MAX_PATH] = L"", b[MAX_PATH] = L"";
    std::wstring reg = GetReg(L"InstallLocation");
    bool registered = reg.empty() || (GetFullPathNameW(reg.c_str(), MAX_PATH, a, nullptr) &&
                                      GetFullPathNameW(dir.c_str(), MAX_PATH, b, nullptr) && !_wcsicmp(a, b));
    if (registered) {
        DeleteFileW((KnownDir(FOLDERID_Desktop) + L"\\" + kShortcut).c_str());
        DeleteFileW((KnownDir(FOLDERID_Programs) + L"\\" + kShortcut).c_str());
    }
    // never out\ (saves, log)
    const wchar_t* parts[] = {L"python", L"tools", L"recomp", L"runtime", L"third_party", L"licenses", L"build", L"game", L"sdl3",
                              L"toolchain", L"android\\ndk", L"android\\template.apk", L"android\\include",
                              L"android\\libugrt.a"};   // keeps android\signing-key.json (a new APK installs over the old one)
    for (auto p : parts) {
        std::wstring path = dir + L"\\" + p;
        DWORD at = GetFileAttributesW(path.c_str());
        if (at == INVALID_FILE_ATTRIBUTES) continue;
        if (at & FILE_ATTRIBUTE_REPARSE_POINT) {   // a junction / link: remove the link, never what it points to
            RemoveDirectoryW(path.c_str());
            continue;
        }
        std::wstring from = path + std::wstring(1, L'\0');
        SHFILEOPSTRUCTW op = {nullptr, FO_DELETE, from.c_str(), nullptr, FOF_NO_UI};
        SHFileOperationW(&op);
    }
    for (auto f : {L"underground.exe", L"underground.ico", L"VERSION", L"settings.ini", L"SDL3.dll",
                   L"gamecontrollerdb.txt"})   // controllers.txt (the player's own mappings) is kept
        DeleteFileW((dir + L"\\" + f).c_str());
    if (registered) {
        RegDeleteTreeW(HKEY_CURRENT_USER, kRegKey);
        RegDeleteTreeW(HKEY_CURRENT_USER, kUninstKey);
    }
    MessageBoxW(nullptr, L"Project Underground was removed. Your saves are kept in the install folder's out\\hdd.",
                kAppName, MB_OK);
    // the launcher is deleted once this process has exited
    std::wstring cmd = L"cmd /c ping -n 3 127.0.0.1 >nul & del \"" + dir + L"\\launcher.exe\" & rmdir \"" + dir + L"\"";
    STARTUPINFOW si = {sizeof si};
    PROCESS_INFORMATION pi;
    if (CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
    }
    return 0;
}

// ---- update check (forge releases, as in runtime/src/updater.c) ----
// GET https url; body to *out, or to the file `to`.
static bool g_allowHttp;   // UG_UPDATE_API test override only; releases are always fetched over https
static bool HttpGet(const std::wstring& url, std::string* out, const wchar_t* to, DWORD* status = nullptr) {
    URL_COMPONENTS uc = {sizeof uc};
    wchar_t host[256], path[2048];
    uc.lpszHostName = host; uc.dwHostNameLength = 256;
    uc.lpszUrlPath = path; uc.dwUrlPathLength = 2048;
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &uc)) return false;
    bool secure = uc.nScheme == INTERNET_SCHEME_HTTPS;
    if (!secure && !g_allowHttp) return false;
    HINTERNET s = WinHttpOpen((L"ProjectUnderground/" + std::wstring(kVersion)).c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                              nullptr, nullptr, 0);
    if (!s) return false;
    WinHttpSetTimeouts(s, 10000, 10000, 15000, 60000);
    HINTERNET c = WinHttpConnect(s, host, uc.nPort, 0);
    HINTERNET r = c ? WinHttpOpenRequest(c, L"GET", path, nullptr, nullptr, nullptr, secure ? WINHTTP_FLAG_SECURE : 0) : nullptr;
    bool ok = r && WinHttpSendRequest(r, L"Accept: application/json\r\n", (DWORD)-1, nullptr, 0, 0, 0) &&
              WinHttpReceiveResponse(r, nullptr);
    DWORD code = 0, cl = sizeof code;
    if (ok) WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, nullptr, &code, &cl, nullptr);
    if (status) *status = code;
    ok = ok && code == 200;
    FILE* f = nullptr;
    if (ok && to) ok = (f = _wfopen(to, L"wb")) != nullptr;
    char chunk[65536];
    DWORD got;
    while (ok && WinHttpReadData(r, chunk, sizeof chunk, &got) && got) {
        if (f) ok = fwrite(chunk, 1, got, f) == got;
        else if (out) out->append(chunk, got);
    }
    if (f) fclose(f);
    if (r) WinHttpCloseHandle(r);
    if (c) WinHttpCloseHandle(c);
    WinHttpCloseHandle(s);
    return ok;
}
// The JSON string value after "key": from pos (advanced past it); unescapes \x to x and \n.
static bool JsonStr(const std::string& j, const char* key, size_t& pos, std::string& v) {
    size_t p = j.find(std::string("\"") + key + "\"", pos);
    if (p == std::string::npos) return false;
    p = j.find('"', p + strlen(key) + 2);
    if (p == std::string::npos) return false;
    v.clear();
    for (p++; p < j.size() && j[p] != '"'; p++) {
        char ch = j[p];
        if (ch == '\\' && p + 1 < j.size()) { p++; ch = j[p] == 'n' ? '\n' : j[p]; }
        v += ch;
    }
    pos = p;
    return true;
}
static unsigned long long VerNum(const char* v) {
    unsigned a = 0, b = 0, c = 0;
    if (*v == 'v' || *v == 'V') v++;
    sscanf(v, "%u.%u.%u", &a, &b, &c);
    return ((unsigned long long)a << 40) | ((unsigned long long)b << 20) | c;
}
struct UpdInfo { std::string tag, url, notes; };
static UpdInfo g_upd;
static void CheckUpdates() {
    const char *repo = getenv("UG_UPDATE_REPO"), *asset = getenv("UG_UPDATE_ASSET");   // testing overrides
    if (!asset) asset = "ProjectUnderground-Setup";
    std::wstring api = L"https://git.8gramlabs.com/api/v1/repos/" +
                       (repo ? Widen(repo) : std::wstring(L"8gramlabs/Project-Underground")) + L"/releases/latest";
    if (const char* t = getenv("UG_UPDATE_API")) { api = Widen(t); g_allowHttp = true; }   // test server (may be http)
    std::string body, name;
    DWORD status = 0;
    bool ok = HttpGet(api, &body, nullptr, &status), newer = false;
    if (ok) {
        size_t pos = 0;
        JsonStr(body, "tag_name", pos, g_upd.tag);
        if (!g_upd.tag.empty() && (g_upd.tag[0] == 'v' || g_upd.tag[0] == 'V')) g_upd.tag.erase(0, 1);   // "v0.2.0" -> "0.2.0"
        pos = 0;
        JsonStr(body, "body", pos, g_upd.notes);
        pos = 0;
        while (JsonStr(body, "name", pos, name))   // assets: "name" ... "browser_download_url"
            if (!name.compare(0, strlen(asset), asset) && name.find(".exe") != std::string::npos) {
                JsonStr(body, "browser_download_url", pos, g_upd.url);
                break;
            }
        newer = !g_upd.tag.empty() && !g_upd.url.empty() && VerNum(g_upd.tag.c_str()) > VerNum(UG_SETUP_VERSION);
    }
    PostMessageW(g_wnd, WM_APP_UPDATE, newer, ok || status == 404);   // 404: no release published yet
}
// Downloads the new setup to %TEMP% and starts it as "--update --dir <install>"; the launcher then closes.
static void DownloadUpdate() {
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    std::wstring setup = std::wstring(tmp) + L"ProjectUnderground-Setup-" + Widen(g_upd.tag) + L".exe";
    bool ok = HttpGet(Widen(g_upd.url), nullptr, setup.c_str());
    if (ok) {
        std::wstring cmd = L"\"" + setup + L"\" --update --dir \"" + g_instDir + L"\"";
        STARTUPINFOW si = {sizeof si};
        PROCESS_INFORMATION pi;
        ok = CreateProcessW(setup.c_str(), cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi);
        if (ok) { CloseHandle(pi.hThread); CloseHandle(pi.hProcess); }
    }
    PostMessageW(g_wnd, WM_APP_UPDATING, ok, 0);
}

// ---- launcher settings (settings.ini [settings], UG_* names the game reads) ----
struct Choice { const wchar_t* label; const wchar_t* value; };   // value nullptr: the game's default (key removed)
// UG_SCALE: host render scale over the Xbox's 480 lines (runtime default 2)
static const Choice kScale[] = {{L"2x  (960 lines, default)", nullptr}, {L"1x  (480 lines)", L"1"}, {L"3x  (1440 lines)", L"3"},
                                {L"4x  (1920 lines)", L"4"}, {L"5x  (2400 lines)", L"5"}, {L"6x  (2880 lines)", L"6"}};
static const Choice kPads[] = {{L"1 (default)", nullptr}, {L"2", L"2"}, {L"3", L"3"}, {L"4", L"4"}};
// UG_WINDOW: windowed client size (default: 2/3 of the monitor's height at the game's aspect)
// UG_WINDOW: windowed resolution (client size in pixels); larger than the monitor is scaled down to fit, same aspect.
// Fullscreen always uses the monitor's own resolution.
static const Choice kWinSize[] = {{L"Auto (2/3 of the monitor, default)", nullptr},
                                  {L"1280 x 720  (16:9)", L"1280x720"},
                                  {L"1366 x 768  (16:9)", L"1366x768"},
                                  {L"1600 x 900  (16:9)", L"1600x900"},
                                  {L"1920 x 1080  (16:9)", L"1920x1080"},
                                  {L"2560 x 1440  (16:9)", L"2560x1440"},
                                  {L"3200 x 1800  (16:9)", L"3200x1800"},
                                  {L"3840 x 2160  (16:9)", L"3840x2160"},
                                  {L"5120 x 2880  (16:9)", L"5120x2880"},
                                  {L"7680 x 4320  (16:9)", L"7680x4320"},
                                  {L"1280 x 800  (16:10)", L"1280x800"},
                                  {L"1440 x 900  (16:10)", L"1440x900"},
                                  {L"1680 x 1050  (16:10)", L"1680x1050"},
                                  {L"1920 x 1200  (16:10)", L"1920x1200"},
                                  {L"2560 x 1600  (16:10)", L"2560x1600"},
                                  {L"2880 x 1800  (16:10)", L"2880x1800"},
                                  {L"3840 x 2400  (16:10)", L"3840x2400"},
                                  {L"2560 x 1080  (21:9)", L"2560x1080"},
                                  {L"3440 x 1440  (21:9)", L"3440x1440"},
                                  {L"3840 x 1600  (21:9)", L"3840x1600"},
                                  {L"5120 x 2160  (21:9)", L"5120x2160"},
                                  {L"3840 x 1080  (32:9)", L"3840x1080"},
                                  {L"5120 x 1440  (32:9)", L"5120x1440"},
                                  {L"7680 x 2160  (32:9)", L"7680x2160"},
                                  {L"640 x 480  (4:3)", L"640x480"},
                                  {L"800 x 600  (4:3)", L"800x600"},
                                  {L"1024 x 768  (4:3)", L"1024x768"},
                                  {L"1280 x 960  (4:3)", L"1280x960"},
                                  {L"1400 x 1050  (4:3)", L"1400x1050"},
                                  {L"1600 x 1200  (4:3)", L"1600x1200"},
                                  {L"2048 x 1536  (4:3)", L"2048x1536"},
                                  {L"1280 x 1024  (5:4)", L"1280x1024"}};
#define N_WINSIZE 32
// UG_WIDESCREEN: the game's own 16:9 mode (Xbox widescreen flag), on by default; UG_ASPECT=stretch fills the window
static const Choice kScreen[] = {{L"16:9 widescreen (default)", nullptr}, {L"4:3 original", L"0"}};
// The game's right stick (walking camera). Both axes are flipped from the Xbox original by default (up is up, left is
// left: runtime/src/hle.c); UG_INVERTY=0 / UG_INVERTX=0 keep the original Xbox direction on that axis. One combo, two
// keys: index bit 0 = original up/down, bit 1 = original left/right (LoadSettings / SaveSettings).
static const Choice kCamera[] = {{L"Normal (default)", nullptr}, {L"Xbox up/down", nullptr},
                                 {L"Xbox left/right", nullptr}, {L"Xbox both ways (original)", nullptr}};
static const Choice kFit[] = {{L"Keep aspect, black bars (default)", nullptr}, {L"Stretch to the window", L"stretch"}};
// UG_LEVEL (test cheat, runtime/src/hle.c): story or free skate loads this level instead of the one it asks for
static const Choice kLevel[] = {{L"Off (normal game)", nullptr}, {L"NJ  New Jersey", L"nj"}, {L"NY  Manhattan", L"ny"},
                                {L"FL  Tampa", L"fl"}, {L"SD  San Diego", L"sd"}, {L"HI  Hawaii", L"hi"},
                                {L"VC  Vancouver", L"vc"}, {L"SJ  Slam City Jam", L"sj"}, {L"RU  Moscow", L"ru"},
                                {L"HN", L"hn"}, {L"PH", L"ph"}, {L"SE", L"se"}, {L"SC", L"sc"}, {L"SC2", L"sc2"},
                                {L"DJ", L"dj"}, {L"SP", L"sp"}, {L"VN", L"vn"}, {L"Test level", L"testlevel"}};
// Monitors in EnumDisplayMonitors order (the game's UG_MONITOR uses the same order); filled by InitMonitors.
static Choice kMonitor[10] = {{L"Main monitor (default)", nullptr}};
static std::wstring g_monLabels[9];
static const wchar_t* kMonValues[9] = {L"0", L"1", L"2", L"3", L"4", L"5", L"6", L"7", L"8"};
static int g_nMonitors = 1;
static BOOL CALLBACK MonEnum(HMONITOR m, HDC, LPRECT, LPARAM) {
    int i = g_nMonitors - 1;
    if (i >= 9) return FALSE;
    MONITORINFO mi = {sizeof mi};
    GetMonitorInfoW(m, &mi);
    g_monLabels[i] = L"Monitor " + std::to_wstring(i + 1) + L": " + std::to_wstring(mi.rcMonitor.right - mi.rcMonitor.left) +
                     L" x " + std::to_wstring(mi.rcMonitor.bottom - mi.rcMonitor.top) +
                     ((mi.dwFlags & MONITORINFOF_PRIMARY) ? L" (main)" : L"");
    kMonitor[g_nMonitors] = {g_monLabels[i].c_str(), kMonValues[i]};
    g_nMonitors++;
    return TRUE;
}
struct ComboSetting { int id; const wchar_t* key; const Choice* c; int n; };
static ComboSetting kCombos[] = {{ID_MONITOR, L"UG_MONITOR", kMonitor, 1}, {ID_WINSIZE, L"UG_WINDOW", kWinSize, N_WINSIZE},
                                 {ID_SCREEN, L"UG_WIDESCREEN", kScreen, 2}, {ID_FIT, L"UG_ASPECT", kFit, 2},
                                 {ID_SCALE, L"UG_SCALE", kScale, 6}, {ID_PADS, L"UG_PADS", kPads, 4},
                                 {ID_LEVEL, L"UG_LEVEL", kLevel, 18}};
static void InitMonitors() {
    EnumDisplayMonitors(nullptr, nullptr, MonEnum, 0);
    for (auto& cs : kCombos) if (cs.id == ID_MONITOR) cs.n = g_nMonitors;
}

static void LoadSettings() {
    for (auto& cs : kCombos) {
        std::wstring v = IniGet(g_instDir, L"settings", cs.key);
        int sel = 0;
        for (int i = 0; i < cs.n; i++)
            if (cs.c[i].value ? v == cs.c[i].value : v.empty()) sel = i;
        SendDlgItemMessageW(g_wnd, cs.id, CB_SETCURSEL, sel, 0);
    }
    int cam = (IniGet(g_instDir, L"settings", L"UG_INVERTY") == L"0" ? 1 : 0) |
              (IniGet(g_instDir, L"settings", L"UG_INVERTX") == L"0" ? 2 : 0);
    SendDlgItemMessageW(g_wnd, ID_CAMERA, CB_SETCURSEL, cam, 0);
    SetOn(ID_FULLSCREEN, IniGet(g_instDir, L"settings", L"UG_FULLSCREEN") == L"1");
    SetOn(ID_FPS60, IniGet(g_instDir, L"settings", L"UG_FPS") != L"30");     // 60 fps is the default
    SetOn(ID_VSYNC, IniGet(g_instDir, L"settings", L"UG_VSYNC") != L"0");    // vsync (FIFO) is the default
}
static void SaveSettings() {
    for (auto& cs : kCombos) {
        int sel = (int)SendDlgItemMessageW(g_wnd, cs.id, CB_GETCURSEL, 0, 0);
        if (sel >= 0 && sel < cs.n) IniSet(g_instDir, L"settings", cs.key, cs.c[sel].value ? cs.c[sel].value : L"");
    }
    {
        int cam = (int)SendDlgItemMessageW(g_wnd, ID_CAMERA, CB_GETCURSEL, 0, 0);
        if (cam >= 0 && cam < 4) {
            IniSet(g_instDir, L"settings", L"UG_INVERTY", (cam & 1) ? L"0" : L"");
            IniSet(g_instDir, L"settings", L"UG_INVERTX", (cam & 2) ? L"0" : L"");
        }
    }
    IniSet(g_instDir, L"settings", L"UG_FULLSCREEN", IsOn(ID_FULLSCREEN) ? L"1" : L"");
    IniSet(g_instDir, L"settings", L"UG_FPS", IsOn(ID_FPS60) ? L"" : L"30");
    // runtime/src/gpu_vulkan.c: UG_VSYNC=0 picks an immediate/mailbox present mode; unset keeps FIFO (vsync on)
    IniSet(g_instDir, L"settings", L"UG_VSYNC", IsOn(ID_VSYNC) ? L"" : L"0");
}
static bool PlayGame() {
    SetEnvironmentVariableW(L"UG_NOUPDATE", L"1");   // the launcher already checked
    std::wstring exe = g_instDir + L"\\underground.exe", cmd = L"\"" + exe + L"\"";
    STARTUPINFOW si = {sizeof si};
    PROCESS_INFORMATION pi;
    if (!CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, g_instDir.c_str(), &si, &pi))
        return false;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

// ---- window ----
// Laid out in pixels of the background art (art/src/background.jpg, 2752x1536), scaled by g_scale. The art has its own
// frame and close button, so the window is borderless (dragged anywhere on the art); its six skateboards are the
// buttons, the bottom metal bar holds the status line and progress.
static const int kArtW = 2752, kArtH = 1536;
static const RECT kBoards[6] = {{1590, 225, 2522, 421}, {1590, 433, 2522, 621}, {1590, 633, 2522, 823},
                                {1590, 832, 2522, 1021}, {1590, 1030, 2522, 1219}, {1590, 1228, 2522, 1420}};
static const RECT kClose = {2618, 12, 2710, 100};     // the X plate, top right
static const RECT kOverlay = {200, 650, 1550, 1415};  // launcher settings panel (over the skater)
static const RECT kStatus = {200, 1438, 2250, 1482}, kStatusSetup = {200, 1432, 2250, 1468}, kBar = {200, 1464, 2250, 1534}, kInfo = {2280, 1446, 2600, 1494};
static double g_scale = 1;
static HDC g_artDC;
static HBITMAP g_artBmp, g_artPanelBmp;                 // the art, and the art with the settings panel painted in
static HBRUSH g_artBrush, g_plainBrush, g_panelBrush;   // g_artBrush: the current one (control backgrounds)
static bool g_panel;
static HFONT g_font, g_titleFont, g_smallFont, g_boardFont, g_subFont, g_statusFont;
static HBRUSH g_fieldBrush;
static const COLORREF kText = RGB(236, 232, 224), kDim = RGB(170, 162, 150), kAccent = RGB(247, 148, 29);
static const COLORREF kOff = RGB(125, 120, 112), kField = RGB(24, 22, 20), kEdge = RGB(150, 100, 40);
static std::map<int, std::wstring> g_sub;   // second line on a skateboard (a path, a hint)
static std::wstring g_isoPath, g_dirPath;
static int S(int v) { return (int)lround(v * g_scale); }
static RECT SR(const RECT& r) { return {S(r.left), S(r.top), S(r.right), S(r.bottom)}; }

static HBITMAP LoadArt(int w, int h) {
    HRSRC r = FindResourceW(nullptr, MAKEINTRESOURCEW(IDR_INSTALLER_BG), RT_RCDATA);
    if (!r) return nullptr;
    IWICImagingFactory* f = nullptr;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f)))) return nullptr;
    IWICStream* s = nullptr;
    IWICBitmapDecoder* d = nullptr;
    IWICBitmapFrameDecode* fr = nullptr;
    IWICBitmapScaler* sc = nullptr;
    IWICFormatConverter* cv = nullptr;
    HBITMAP bmp = nullptr;
    f->CreateStream(&s);
    if (s && SUCCEEDED(s->InitializeFromMemory((BYTE*)LockResource(LoadResource(nullptr, r)), SizeofResource(nullptr, r))) &&
        SUCCEEDED(f->CreateDecoderFromStream(s, nullptr, WICDecodeMetadataCacheOnLoad, &d)) &&
        SUCCEEDED(d->GetFrame(0, &fr)) && SUCCEEDED(f->CreateBitmapScaler(&sc)) &&
        SUCCEEDED(sc->Initialize(fr, w, h, WICBitmapInterpolationModeFant)) && SUCCEEDED(f->CreateFormatConverter(&cv)) &&
        SUCCEEDED(cv->Initialize(sc, GUID_WICPixelFormat32bppBGR, WICBitmapDitherTypeNone, nullptr, 0,
                                 WICBitmapPaletteTypeCustom))) {
        BITMAPINFO bi = {{sizeof(BITMAPINFOHEADER), w, -h, 1, 32, BI_RGB}};
        void* bits = nullptr;
        bmp = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (bmp) cv->CopyPixels(nullptr, w * 4, w * h * 4, (BYTE*)bits);
    }
    if (cv) cv->Release();
    if (sc) sc->Release();
    if (fr) fr->Release();
    if (d) d->Release();
    if (s) s->Release();
    f->Release();
    return bmp;
}

static void ArtText(HDC dc, const RECT& r, HFONT f, COLORREF c, const std::wstring& s, UINT fmt) {
    RECT sr = SR(r);
    SelectObject(dc, f);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, c);
    DrawTextW(dc, s.c_str(), (int)s.size(), &sr, fmt | DT_NOPREFIX);
}
// g_artBmp: the art + the version on the bottom bar. g_artPanelBmp (launcher): the same with the settings panel.
static void BuildArt(int w, int h) {
    HDC screen = GetDC(nullptr);
    g_artDC = CreateCompatibleDC(screen);
    g_artBmp = LoadArt(w, h);
    if (!g_artBmp) g_artBmp = CreateCompatibleBitmap(screen, w, h);
    g_artPanelBmp = CreateCompatibleBitmap(screen, w, h);
    ReleaseDC(nullptr, screen);
    SelectObject(g_artDC, g_artBmp);
    ArtText(g_artDC, kInfo, g_smallFont, kDim, L"Version " + std::wstring(kVersion), DT_SINGLELINE | DT_VCENTER | DT_RIGHT);
    HDC pd = CreateCompatibleDC(g_artDC);
    SelectObject(pd, g_artPanelBmp);
    BitBlt(pd, 0, 0, w, h, g_artDC, 0, 0, SRCCOPY);
    if (g_launcher) {
        RECT p = SR(kOverlay);
        HDC tmp = CreateCompatibleDC(pd);
        HBITMAP b = CreateCompatibleBitmap(pd, 1, 1);
        HGDIOBJ old = SelectObject(tmp, b);
        SetPixel(tmp, 0, 0, RGB(8, 7, 6));
        BLENDFUNCTION bf = {AC_SRC_OVER, 0, 222, 0};
        GdiAlphaBlend(pd, p.left, p.top, p.right - p.left, p.bottom - p.top, tmp, 0, 0, 1, 1, bf);
        SelectObject(tmp, old);
        DeleteObject(b);
        DeleteDC(tmp);
        HPEN pen = CreatePen(PS_SOLID, std::max(1, S(4)), kAccent);
        HGDIOBJ op = SelectObject(pd, pen), ob = SelectObject(pd, GetStockObject(NULL_BRUSH));
        Rectangle(pd, p.left, p.top, p.right, p.bottom);
        SelectObject(pd, op); SelectObject(pd, ob);
        DeleteObject(pen);
        ArtText(pd, {260, 665, 1500, 755}, g_titleFont, kAccent, L"SETTINGS", DT_SINGLELINE | DT_VCENTER);
        static const wchar_t* rows[] = {L"Monitor", L"Resolution", L"Screen", L"Picture", L"Render scale",
                                        L"Players (gamepads)", L"Camera (right stick)", L"Level (test cheat)",
                                        L"HD texture pack"};
        for (int i = 0; i < 9; i++)
            ArtText(pd, {260, 836 + 61 * i, 700, 896 + 61 * i}, g_font, kText, rows[i], DT_SINGLELINE | DT_VCENTER);
        ArtText(pd, {700, 665, 1500, 755}, g_smallFont, kDim, L"Saved at once to settings.ini",
                DT_SINGLELINE | DT_VCENTER | DT_RIGHT);
    }
    DeleteDC(pd);
}

// Owner-drawn buttons. GWLP_USERDATA bits:
enum { K_PUSH = 1, K_TOGGLE = 2, K_ON = 4, K_HOT = 8, K_BOARD = 16, K_CLOSE = 32 };
// Hover: the button under the mouse gets K_HOT (owner-drawn buttons have no hover state of their own); hand cursor.
static LRESULT CALLBACK HoverProc(HWND h, UINT m, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR) {
    LONG_PTR k = GetWindowLongPtrW(h, GWLP_USERDATA);
    if (m == WM_MOUSEMOVE && !(k & K_HOT)) {
        SetWindowLongPtrW(h, GWLP_USERDATA, k | K_HOT);
        TRACKMOUSEEVENT t = {sizeof t, TME_LEAVE, h, 0};
        TrackMouseEvent(&t);
        InvalidateRect(h, nullptr, FALSE);
    } else if (m == WM_MOUSELEAVE) {
        SetWindowLongPtrW(h, GWLP_USERDATA, k & ~K_HOT);
        InvalidateRect(h, nullptr, FALSE);
    } else if (m == WM_SETCURSOR) {
        SetCursor(LoadCursorW(nullptr, IDC_HAND));
        return TRUE;
    }
    return DefSubclassProc(h, m, wp, lp);
}
static HWND Make(const wchar_t* cls, const wchar_t* text, DWORD style, int id, int x, int y, int w, int h,
                 HFONT font = nullptr) {
    LONG_PTR kind = 0;
    if (!wcscmp(cls, L"BUTTON")) {
        kind = (style & BS_TYPEMASK) == BS_AUTOCHECKBOX ? K_TOGGLE : K_PUSH;
        style = (style & ~BS_TYPEMASK) | BS_OWNERDRAW;
    }
    HWND c = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, S(x), S(y), S(w), S(h), g_wnd,
                             (HMENU)(INT_PTR)id, GetModuleHandleW(nullptr), nullptr);
    SendMessageW(c, WM_SETFONT, (WPARAM)(font ? font : g_font), TRUE);
    if (kind) {
        SetWindowLongPtrW(c, GWLP_USERDATA, kind);
        SetWindowSubclass(c, HoverProc, 1, 0);
    }
    return c;
}
static HWND MakeAt(const wchar_t* cls, const wchar_t* text, DWORD style, int id, const RECT& r, HFONT font = nullptr) {
    return Make(cls, text, style, id, r.left, r.top, r.right - r.left, r.bottom - r.top, font);
}
// Skateboard slot 0-5 (top to bottom) as a button; toggle: the label gets ": ON" / ": OFF".
static HWND MakeBoard(int slot, const wchar_t* text, int id, bool toggle = false) {
    HWND c = MakeAt(L"BUTTON", text, BS_PUSHBUTTON | WS_TABSTOP, id, kBoards[slot], g_boardFont);
    SetWindowLongPtrW(c, GWLP_USERDATA, K_BOARD | (toggle ? K_TOGGLE : 0));
    return c;
}
static void SetSub(int id, const std::wstring& s) {
    g_sub[id] = s;
    InvalidateRect(GetDlgItem(g_wnd, id), nullptr, FALSE);
}
static void SetBoard(HWND c, const std::wstring& text, const std::wstring& sub, bool enabled) {
    SetWindowTextW(c, text.c_str());
    EnableWindow(c, enabled);
    SetSub(GetDlgCtrlID(c), sub);
}
// Lines the art brush up with control c, so its background is the art behind it.
static HBRUSH ArtBrushFor(HDC dc, HWND c) {
    RECT rc;
    GetWindowRect(c, &rc);
    MapWindowPoints(nullptr, g_wnd, (POINT*)&rc, 2);
    SetBrushOrgEx(dc, -rc.left, -rc.top, nullptr);
    return g_artBrush;
}
// Text with a black outline (readable on the worn decks and stickers).
static void OutlinedText(HDC dc, const std::wstring& s, RECT r, UINT fmt, COLORREF c) {
    int o = std::max(1, S(4));
    SetTextColor(dc, RGB(0, 0, 0));
    for (int dy = -o; dy <= o; dy += o)
        for (int dx = -o; dx <= o; dx += o)
            if (dx || dy) {
                RECT t = r;
                OffsetRect(&t, dx, dy);
                DrawTextW(dc, s.c_str(), (int)s.size(), &t, fmt | DT_NOPREFIX);
            }
    SetTextColor(dc, c);
    DrawTextW(dc, s.c_str(), (int)s.size(), &r, fmt | DT_NOPREFIX);
}
static void DrawButton(const DRAWITEMSTRUCT* di) {
    RECT r = di->rcItem;
    int w = r.right - r.left, h = r.bottom - r.top;
    HDC dc = CreateCompatibleDC(di->hDC);   // drawn off screen, then copied: no flicker on hover
    HBITMAP bmp = CreateCompatibleBitmap(di->hDC, w, h);
    HGDIOBJ oldBmp = SelectObject(dc, bmp);
    FillRect(dc, &r, ArtBrushFor(dc, di->hwndItem));
    LONG_PTR kind = GetWindowLongPtrW(di->hwndItem, GWLP_USERDATA);
    bool dis = di->itemState & ODS_DISABLED, down = di->itemState & ODS_SELECTED, hot = (kind & K_HOT) && !dis;
    bool focus = (di->itemState & ODS_FOCUS) && !(di->itemState & ODS_NOFOCUSRECT);   // keyboard focus only
    wchar_t text[256];
    GetWindowTextW(di->hwndItem, text, 256);
    SetBkMode(dc, TRANSPARENT);
    if (kind & K_BOARD) {
        std::wstring label = text;
        if (kind & K_TOGGLE) label += (kind & K_ON) ? L":  ON" : L":  OFF";
        COLORREF c = dis ? kOff : (hot || focus || down) ? kAccent : kText;
        RECT tr = {S(50), 0, w - S(140), h};   // centred left of the stickers on the decks' right ends
        if (down) OffsetRect(&tr, S(4), S(4));
        SelectObject(dc, g_boardFont);
        auto sub = g_sub.find(GetDlgCtrlID(di->hwndItem));
        if (sub != g_sub.end() && !sub->second.empty()) {
            RECT t1 = tr, t2 = tr;
            t1.bottom = tr.top + h * 64 / 100;
            t2.top = t1.bottom + S(4);
            OutlinedText(dc, label, t1, DT_SINGLELINE | DT_BOTTOM | DT_CENTER, c);
            SelectObject(dc, g_subFont);
            UINT ell = sub->second.find(L'\\') != std::wstring::npos ? DT_PATH_ELLIPSIS : DT_END_ELLIPSIS;
            OutlinedText(dc, sub->second, t2, DT_SINGLELINE | DT_TOP | DT_CENTER | ell, dis ? kOff : kText);
        } else {
            OutlinedText(dc, label, tr, DT_SINGLELINE | DT_VCENTER | DT_CENTER, c);
        }
    } else if (kind & K_CLOSE) {   // the X is in the art: only a hover ring
        if (hot || down) {
            HPEN pen = CreatePen(PS_SOLID, std::max(2, S(5)), kAccent);
            HGDIOBJ op = SelectObject(dc, pen), ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
            RoundRect(dc, S(3), S(3), w - S(3), h - S(3), S(16), S(16));
            SelectObject(dc, op); SelectObject(dc, ob);
            DeleteObject(pen);
        }
    } else {
        SelectObject(dc, (HFONT)SendMessageW(di->hwndItem, WM_GETFONT, 0, 0));
        SetTextColor(dc, dis ? kDim : hot ? kAccent : kText);
        HPEN pen = CreatePen(PS_SOLID, std::max(1, S(3)), focus || hot ? kAccent : kEdge);
        HGDIOBJ op = SelectObject(dc, pen);
        if (kind & K_TOGGLE) {   // box + label
            int b = S(40), y0 = (h - b) / 2;
            HBRUSH fill = CreateSolidBrush(kField);
            HGDIOBJ ob = SelectObject(dc, fill);
            Rectangle(dc, S(3), y0, S(3) + b, y0 + b);
            SelectObject(dc, ob);
            DeleteObject(fill);
            if (kind & K_ON) {
                RECT in = {S(3) + S(10), y0 + S(10), S(3) + b - S(10), y0 + b - S(10)};
                HBRUSH on = CreateSolidBrush(kAccent);
                FillRect(dc, &in, on);
                DeleteObject(on);
            }
            RECT tr = {b + S(24), 0, w, h};
            DrawTextW(dc, text, -1, &tr, DT_SINGLELINE | DT_VCENTER | DT_LEFT);
        } else {                 // plain push button
            HBRUSH fill = CreateSolidBrush(dis ? RGB(18, 20, 26) : down ? RGB(120, 80, 30) : RGB(40, 34, 30));
            HGDIOBJ ob = SelectObject(dc, fill);
            RoundRect(dc, 1, 1, w - 1, h - 1, S(16), S(16));
            SelectObject(dc, ob);
            DeleteObject(fill);
            DrawTextW(dc, text, -1, &r, DT_SINGLELINE | DT_VCENTER | DT_CENTER);
        }
        SelectObject(dc, op);
        DeleteObject(pen);
    }
    BitBlt(di->hDC, r.left, r.top, w, h, dc, 0, 0, SRCCOPY);
    SelectObject(dc, oldBmp);
    DeleteObject(bmp);
    DeleteDC(dc);
}
static HWND MakeCheck(const wchar_t* text, int id, int x, int y, int w) {
    return Make(L"BUTTON", text, BS_AUTOCHECKBOX | WS_TABSTOP, id, x, y, w, 60);
}

static void BrowseIso() {
    wchar_t file[MAX_PATH] = L"";
    OPENFILENAMEW of = {sizeof of};
    of.hwndOwner = g_wnd;
    of.lpstrFilter = L"Xbox disc image (*.iso;*.xiso)\0*.iso;*.xiso\0All files\0*.*\0";
    of.lpstrFile = file;
    of.nMaxFile = MAX_PATH;
    of.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    if (GetOpenFileNameW(&of)) { g_isoPath = file; SetSub(ID_ISO_BROWSE, g_isoPath); }
}
static std::wstring BrowseFolder() {
    std::wstring r;
    IFileOpenDialog* d = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&d)))) return r;
    DWORD o;
    d->GetOptions(&o);
    d->SetOptions(o | FOS_PICKFOLDERS);
    IShellItem* it = nullptr;
    if (SUCCEEDED(d->Show(g_wnd)) && SUCCEEDED(d->GetResult(&it))) {
        PWSTR p = nullptr;
        if (SUCCEEDED(it->GetDisplayName(SIGDN_FILESYSPATH, &p))) r = p;
        CoTaskMemFree(p);
        it->Release();
    }
    d->Release();
    return r;
}
static void BrowseDir() {
    std::wstring s = BrowseFolder();
    if (s.empty()) return;
    static const wchar_t kSub[] = L"Project Underground";
    const size_t n = wcslen(kSub);
    if (s.size() < n || _wcsicmp(s.c_str() + s.size() - n, kSub)) s += std::wstring(L"\\") + kSub;
    g_dirPath = s;
    SetSub(ID_DIR_BROWSE, g_dirPath);
}

// installer boards that only make sense before the build starts
static void EnableInputs(bool on) {
    for (int id : {ID_DESKTOP, ID_STARTMENU}) EnableWindow(GetDlgItem(g_wnd, id), on);
    if (!g_updating)
        for (int id : {ID_ISO_BROWSE, ID_DIR_BROWSE}) EnableWindow(GetDlgItem(g_wnd, id), on);
}
static void Start() {
    if (!g_updating && !Exists(g_isoPath)) {
        MessageBoxW(g_wnd, L"Choose your Tony Hawk's Underground disc image (the DISC IMAGE board).", kAppName, MB_OK);
        return;
    }
    if (g_dirPath.empty()) return;
    g_running = true;
    EnableInputs(false);
    SetBoard(g_install, L"BUILDING...", L"", false);
    bool desk = IsOn(ID_DESKTOP), sm = IsOn(ID_STARTMENU);
    g_worker = std::thread(Work, g_isoPath, g_dirPath, desk, sm, g_updating);
}

// After an install: open the launcher.
static void Launch(const std::wstring& dir) {
    ShellExecuteW(nullptr, L"open", (dir + L"\\launcher.exe").c_str(), L"--launcher", dir.c_str(), SW_SHOWNORMAL);
}

// Launcher settings controls (shown on the panel over the art when the SETTINGS board is on).
static HWND g_settingsBtn;
static const int kSettingIds[] = {ID_FULLSCREEN, ID_FPS60, ID_VSYNC, ID_MONITOR, ID_WINSIZE, ID_SCREEN, ID_FIT, ID_SCALE,
                                  ID_PADS, ID_CAMERA, ID_LEVEL, ID_HD};
// HD texture pack (UG_HDTEX=<folder>, runtime/src/hdtex.c): a PCSX2 replacement pack made for the PS2 version, used
// where it is. Combo: Off / the folder / Choose folder... The folder is remembered in [launcher] HdFolder while off.
static int CountDds(const std::wstring& dir, int depth = 0) {
    if (depth > 6) return 0;
    int n = 0;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        if (fd.cFileName[0] == L'.') continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) n += CountDds(dir + L"\\" + fd.cFileName, depth + 1);
        else {
            size_t l = wcslen(fd.cFileName);
            if (l > 4 && !_wcsicmp(fd.cFileName + l - 4, L".dds")) n++;
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return n;
}
static void FillHdCombo() {
    HWND cb = GetDlgItem(g_wnd, ID_HD);
    SendMessageW(cb, CB_RESETCONTENT, 0, 0);
    std::wstring on = IniGet(g_instDir, L"settings", L"UG_HDTEX"), folder = on.empty() ? IniGet(g_instDir, L"launcher", L"HdFolder") : on;
    SendMessageW(cb, CB_ADDSTRING, 0, (LPARAM)L"Off (original textures, default)");
    if (!folder.empty()) SendMessageW(cb, CB_ADDSTRING, 0, (LPARAM)(L"On: " + folder).c_str());
    SendMessageW(cb, CB_ADDSTRING, 0, (LPARAM)L"Choose pack folder...");
    SendMessageW(cb, CB_SETCURSEL, on.empty() ? 0 : 1, 0);
}
static void HdComboChanged() {
    HWND cb = GetDlgItem(g_wnd, ID_HD);
    int sel = (int)SendMessageW(cb, CB_GETCURSEL, 0, 0), n = (int)SendMessageW(cb, CB_GETCOUNT, 0, 0);
    std::wstring folder = IniGet(g_instDir, L"launcher", L"HdFolder");
    if (sel == n - 1) {   // Choose pack folder...
        std::wstring pick = BrowseFolder();
        if (!pick.empty()) {
            int dds = CountDds(pick);
            if (!dds) {
                MessageBoxW(g_wnd, L"No .dds textures in that folder. Pick the pack's \"replacements\" folder (or a folder "
                                   L"above it), e.g. ...\\SLUS-20731\\replacements.", kAppName, MB_OK | MB_ICONWARNING);
            } else {
                folder = pick;
                IniSet(g_instDir, L"launcher", L"HdFolder", folder);
                IniSet(g_instDir, L"settings", L"UG_HDTEX", folder);
                SetWindowTextW(g_status, (L"HD texture pack: " + std::to_wstring(dds) + L" textures. F10 in the game "
                                          L"switches them on and off.").c_str());
            }
        }
    } else {
        IniSet(g_instDir, L"settings", L"UG_HDTEX", sel == 1 && !folder.empty() ? folder : L"");
    }
    FillHdCombo();
}

static void ShowSettings(bool on) {
    g_panel = on;
    SelectObject(g_artDC, on ? g_artPanelBmp : g_artBmp);
    g_artBrush = on ? g_panelBrush : g_plainBrush;
    for (int id : kSettingIds) ShowWindow(GetDlgItem(g_wnd, id), on ? SW_SHOW : SW_HIDE);
    SetWindowTextW(g_settingsBtn, on ? L"HIDE SETTINGS" : L"SETTINGS");
    RedrawWindow(g_wnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
}

// ---- gamepad navigation (XInput, polled every 33 ms while the window is in front): D-pad / left stick up-down
// moves between the skateboards (or the settings rows), left-right changes a setting, A presses, B closes the
// settings panel, START plays (launcher) ----
typedef struct { DWORD packet; WORD buttons; BYTE lt, rt; SHORT lx, ly, rx, ry; } PadState;
static DWORD (WINAPI* g_xget)(DWORD, PadState*);
static std::vector<HWND> NavItems() {
    std::vector<HWND> v;
    if (g_panel)
        for (int id : kSettingIds) { HWND c = GetDlgItem(g_wnd, id); if (c && IsWindowVisible(c) && IsWindowEnabled(c)) v.push_back(c); }
    for (HWND c = GetWindow(g_wnd, GW_CHILD); c; c = GetWindow(c, GW_HWNDNEXT))
        if ((GetWindowLongPtrW(c, GWLP_USERDATA) & K_BOARD) && IsWindowVisible(c) && IsWindowEnabled(c)) {
            if (g_panel && c != g_settingsBtn) continue;   // settings open: its rows, then the HIDE SETTINGS board
            v.push_back(c);
        }
    return v;
}
static void PadPoll() {
    static bool init;
    if (!init) {
        init = true;
        HMODULE m = LoadLibraryW(L"xinput1_4.dll");
        if (!m) m = LoadLibraryW(L"xinput9_1_0.dll");
        if (m) g_xget = (DWORD(WINAPI*)(DWORD, PadState*))(void*)GetProcAddress(m, "XInputGetState");
    }
    if (!g_xget || GetForegroundWindow() != g_wnd) return;
    WORD b = 0;
    for (DWORD i = 0; i < 4; i++) {
        PadState st = {};
        if (g_xget(i, &st) != 0) continue;
        b |= st.buttons;
        if (st.ly > 16000) b |= 1; if (st.ly < -16000) b |= 2; if (st.lx < -16000) b |= 4; if (st.lx > 16000) b |= 8;
    }
    static WORD prev;
    static DWORD held_since, last_rep;
    DWORD now = GetTickCount();
    WORD dirs = b & 15, press = b & ~prev;
    if (dirs && dirs == (prev & 15)) {   // held: repeat after 400 ms, every 120 ms
        if (now - held_since > 400 && now - last_rep > 120) { press |= dirs; last_rep = now; }
    } else held_since = now;
    prev = b;
    if (!press) return;
    SendMessageW(g_wnd, WM_CHANGEUISTATE, MAKEWPARAM(UIS_CLEAR, UISF_HIDEFOCUS), 0);   // show the focus highlight
    std::vector<HWND> items = NavItems();
    if (items.empty()) return;
    HWND f = GetFocus();
    int cur = -1;
    for (size_t i = 0; i < items.size(); i++) if (items[i] == f) cur = (int)i;
    if (press & 3) {   // up / down
        int n = (int)items.size();
        cur = cur < 0 ? 0 : (cur + ((press & 2) ? 1 : n - 1)) % n;
        SetFocus(items[cur]);
        return;
    }
    if (cur < 0) { SetFocus(items[0]); return; }
    HWND c = items[cur];
    wchar_t cls[16];
    GetClassNameW(c, cls, 16);
    bool combo = !_wcsicmp(cls, L"COMBOBOX");
    if ((press & 12) && combo) {   // left / right: previous / next choice
        int n = (int)SendMessageW(c, CB_GETCOUNT, 0, 0), sel = (int)SendMessageW(c, CB_GETCURSEL, 0, 0);
        int to = sel + ((press & 8) ? 1 : -1);
        if (to >= 0 && to < n) {
            SendMessageW(c, CB_SETCURSEL, to, 0);
            SendMessageW(g_wnd, WM_COMMAND, MAKEWPARAM(GetDlgCtrlID(c), CBN_SELCHANGE), (LPARAM)c);
        }
        return;
    }
    if ((press & 0x1000) || ((press & 12) && (GetWindowLongPtrW(c, GWLP_USERDATA) & K_TOGGLE))) {   // A (or left/right on a toggle)
        if (combo) SendMessageW(c, CB_SHOWDROPDOWN, TRUE, 0);
        else SendMessageW(c, BM_CLICK, 0, 0);
        return;
    }
    if ((press & 0x2000) && g_launcher && g_panel) { ShowSettings(false); SetFocus(g_settingsBtn); return; }   // B
    if ((press & 0x10) && g_launcher) SendMessageW(GetDlgItem(g_wnd, ID_PLAY), BM_CLICK, 0, 0);               // START
}

static bool LauncherCommand(HWND h, int id, int code) {
    switch (id) {
    case ID_LOGS: {   // the game log (out\underground.log), selected in Explorer; the folder if there is no log yet
        std::wstring log = g_instDir + L"\\out\\underground.log";
        if (Exists(log)) ShellExecuteW(h, nullptr, L"explorer.exe", (L"/select,\"" + log + L"\"").c_str(), nullptr, SW_SHOWNORMAL);
        else ShellExecuteW(h, L"open", g_instDir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        return true;
    }
    case ID_PLAY:
        SaveSettings();
        if (PlayGame()) DestroyWindow(h);
        else MessageBoxW(h, (L"Could not start " + g_instDir + L"\\underground.exe.").c_str(), kAppName, MB_OK | MB_ICONERROR);
        return true;
    case ID_SETTINGS:
        ShowSettings(!g_panel);
        return true;
    case ID_UPDATE:
        if (g_worker.joinable()) return true;
        SetBoard(g_updBtn, L"UPDATING...", L"Downloading version " + Widen(g_upd.tag), false);
        SetWindowTextW(g_status, (L"Downloading version " + Widen(g_upd.tag) + L"...").c_str());
        g_worker = std::thread(DownloadUpdate);
        return true;
    case ID_UNINSTALL:
        if (Uninstall(g_instDir) == 0) DestroyWindow(h);
        return true;
    case ID_ANDROID:
        if (g_worker.joinable()) return true;
        if (MessageBoxW(h, L"Build Project Underground for Android phones (arm64, Android 13 or newer, Vulkan 1.3)?\n\n"
                           L"Your installed game is compiled for Android on this PC and packed with your game files into "
                           L"one APK (about 3.5 GB; the phone needs about 7 GB free). The first build downloads Google's "
                           L"Android NDK (about 700 MB; Android SDK license: developer.android.com/studio/terms) and takes "
                           L"5-15 minutes.\n\nThen copy the APK to your phone and open it there to install it.",
                        kAppName, MB_OKCANCEL | MB_ICONQUESTION) != IDOK)
            return true;
        SetBoard(g_androidBtn, L"ANDROID", L"Building, see below...", false);
        SetWindowTextW(g_status, L"Android: starting...");
        g_worker = std::thread(BuildAndroid);
        return true;
    case ID_FULLSCREEN: case ID_FPS60: case ID_VSYNC:
        if (code == BN_CLICKED) SaveSettings();
        return true;
    case ID_HD:
        if (code == CBN_SELCHANGE) HdComboChanged();
        return true;
    case ID_SCALE: case ID_PADS: case ID_MONITOR: case ID_WINSIZE: case ID_SCREEN: case ID_FIT: case ID_LEVEL: case ID_CAMERA:
        if (code == CBN_SELCHANGE) SaveSettings();
        return true;
    }
    return false;
}

static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT r;
        GetClientRect(h, &r);
        BitBlt(dc, 0, 0, r.right, r.bottom, g_artDC, 0, 0, SRCCOPY);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_ERASEBKGND: return 1;
    case WM_NCHITTEST: {   // borderless: drag the window by the art
        LRESULT r = DefWindowProcW(h, m, wp, lp);
        return r == HTCLIENT ? HTCAPTION : r;
    }
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX:
        SetTextColor((HDC)wp, kText);
        SetBkColor((HDC)wp, kField);
        return (LRESULT)g_fieldBrush;
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN:
        SetTextColor((HDC)wp, kText);
        SetBkMode((HDC)wp, TRANSPARENT);
        return (LRESULT)ArtBrushFor((HDC)wp, (HWND)lp);
    case WM_DRAWITEM: DrawButton((const DRAWITEMSTRUCT*)lp); return TRUE;
    case WM_TIMER: if (wp == 77) PadPoll(); return 0;
    case WM_COMMAND:
        if ((HIWORD(wp) == BN_CLICKED || HIWORD(wp) == BN_DOUBLECLICKED) && lp &&
            (GetWindowLongPtrW((HWND)lp, GWLP_USERDATA) & K_TOGGLE)) {
            SetOn(LOWORD(wp), !IsOn(LOWORD(wp)));
            wp = MAKEWPARAM(LOWORD(wp), BN_CLICKED);
        }
        if (LOWORD(wp) == ID_CLOSE || LOWORD(wp) == ID_QUIT || LOWORD(wp) == IDCANCEL) {   // the art's X, QUIT, Esc
            PostMessageW(h, WM_CLOSE, 0, 0);
            return 0;
        }
        if (g_launcher && LauncherCommand(h, LOWORD(wp), HIWORD(wp))) return 0;
        switch (LOWORD(wp)) {
        case ID_ISO_BROWSE: BrowseIso(); break;
        case ID_DIR_BROWSE: BrowseDir(); break;
        case ID_INSTALL:
            if (!g_running && !g_doneDir.empty()) { Launch(g_doneDir); DestroyWindow(h); }
            else if (!g_running) Start();
            break;
        }
        return 0;
    case WM_APP_STATUS: {
        auto* s = (std::wstring*)lp;
        SetWindowTextW(g_status, s->c_str());
        delete s;
        return 0;
    }
    case WM_APP_PROGRESS: SendMessageW(g_progress, PBM_SETPOS, wp, 0); return 0;
    case WM_APP_UPDATE:
        if (g_worker.joinable()) g_worker.join();
        if (wp) {
            SetWindowTextW(g_status, (L"Version " + Widen(g_upd.tag) + L" is available (you have " + kVersion +
                                      L"). Update rebuilds the game in a few minutes; saves are kept.").c_str());
            SetBoard(g_updBtn, L"UPDATE TO " + Widen(g_upd.tag), L"Saves and settings are kept", true);
        } else {
            SetWindowTextW(g_status, lp ? (std::wstring(L"Up to date (version ") + kVersion + L").").c_str()
                                        : L"Could not check for updates (offline?).");
            SetBoard(g_updBtn, lp ? L"UP TO DATE" : L"NO UPDATE CHECK", lp ? L"" : L"Offline?", false);
        }
        return 0;
    case WM_APP_ANDROID:
        if (g_worker.joinable()) g_worker.join();
        SetBoard(g_androidBtn, L"ANDROID", L"Build an APK for your phone", true);
        if (wp) {
            SetWindowTextW(g_status, (L"Android APK ready: " + g_apk).c_str());
            ShellExecuteW(h, nullptr, L"explorer.exe", (L"/select,\"" + g_apk + L"\"").c_str(), nullptr, SW_SHOWNORMAL);
        } else {
            SetWindowTextW(g_status, L"The Android build failed.");
            MessageBoxW(h, g_error.c_str(), kAppName, MB_OK | MB_ICONERROR);
        }
        return 0;
    case WM_APP_UPDATING:
        if (g_worker.joinable()) g_worker.join();
        if (wp) { DestroyWindow(h); return 0; }
        SetWindowTextW(g_status, L"The update could not be downloaded. Try again later.");
        SetBoard(g_updBtn, L"UPDATE TO " + Widen(g_upd.tag), L"Download failed: try again", true);
        return 0;
    case WM_APP_DONE:
        if (g_worker.joinable()) g_worker.join();
        g_running = false;
        if (wp) {
            SetWindowTextW(g_status, (std::wstring(g_updating ? L"Updated to version " : L"Installed version ") + kVersion +
                                      L" in " + g_doneDir + L".").c_str());
            SetBoard(g_install, L"PLAY", L"Opens the launcher", true);
            SetFocus(g_install);
        } else {
            g_doneDir.clear();
            SetWindowTextW(g_status, L"Failed. See the message.");
            MessageBoxW(h, g_error.c_str(), kAppName, MB_OK | MB_ICONERROR);
            EnableInputs(true);
            SetBoard(g_install, g_updating ? L"UPDATE" : L"INSTALL", L"Try again", true);
        }
        return 0;
    case WM_CLOSE:
        if (g_running && MessageBoxW(h, L"The build is still running. Quit anyway?", kAppName, MB_YESNO) != IDYES) return 0;
        DestroyWindow(h);
        return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(h, m, wp, lp);
}

static std::wstring DefaultDir() {
    std::wstring rec = GetReg(L"InstallLocation");
    if (!rec.empty()) return rec;
    return KnownDir(FOLDERID_LocalAppData) + L"\\Project Underground";
}

static void MakeCombo(int id, const Choice* c, int n, int x, int y, int w, HFONT font = nullptr) {
    HWND cb = Make(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, id, x, y, w, 520, font);
    SetWindowTheme(cb, L"", L"");
    for (int i = 0; i < n; i++) SendMessageW(cb, CB_ADDSTRING, 0, (LPARAM)c[i].label);
}

// Progress: a skateboard rolling along a rail (grind bar) on the bottom metal bar; the rail behind it lights up.
// Takes PBM_SETPOS (0..100) like the common progress bar; drawn with GDI+ (antialiased) over the art.
static LRESULT CALLBACK SkateBarProc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
    case PBM_SETPOS: {
        LONG_PTR old = GetWindowLongPtrW(h, GWLP_USERDATA);
        SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)std::min<WPARAM>(wp, 100));
        if ((WPARAM)old != wp) InvalidateRect(h, nullptr, FALSE);
        return old;
    }
    case PBM_SETRANGE: case PBM_SETBARCOLOR: case PBM_SETBKCOLOR: return 0;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC wdc = BeginPaint(h, &ps);
        RECT r;
        GetClientRect(h, &r);
        int W = r.right, H = r.bottom;
        HDC dc = CreateCompatibleDC(wdc);
        HBITMAP bmp = CreateCompatibleBitmap(wdc, W, H);
        HGDIOBJ ob = SelectObject(dc, bmp);
        FillRect(dc, &r, ArtBrushFor(dc, h));
        {
            using namespace Gdiplus;
            Graphics g(dc);
            g.SetSmoothingMode(SmoothingModeAntiAlias);
            float pct = (float)GetWindowLongPtrW(h, GWLP_USERDATA) / 100.f;
            float wr = H * 0.16f;                    // wheel radius
            float L = H * 3.3f;                      // deck length
            float railY = H - H * 0.12f, railH = H * 0.11f;
            float cx = L / 2 + 4 + pct * (W - L - 8);   // board centre
            // rail: dark steel, lit orange up to the board
            SolidBrush steel(Color(255, 40, 38, 36)), lit(Color(255, 255, 160, 40)), edge(Color(255, 90, 86, 80));
            g.FillRectangle(&steel, 0.f, railY - railH / 2, (REAL)W, railH);
            g.FillRectangle(&lit, 0.f, railY - railH / 2, pct >= 1.f ? (REAL)W : cx, railH);
            Pen railPen(Color(255, 0, 0, 0), 1.5f);
            g.DrawRectangle(&railPen, 0.f, railY - railH / 2, (REAL)W - 1, railH);
            // wheels sit on the rail; trucks; deck = a thick stroke with kicked-up tails (black outline under it)
            float wy = railY - railH / 2 - wr, deckT = H * 0.12f, deckY = wy - wr - H * 0.07f;
            float spin = pct * 40.f;
            float x0 = cx - L / 2, x1 = cx + L / 2, tail = L * 0.16f, lift = H * 0.17f;
            GraphicsPath deck;
            deck.AddBezier(x0, deckY - lift, x0 + tail * 0.25f, deckY, x0 + tail * 0.6f, deckY, x0 + tail, deckY);
            deck.AddLine(x0 + tail, deckY, x1 - tail, deckY);
            deck.AddBezier(x1 - tail, deckY, x1 - tail * 0.6f, deckY, x1 - tail * 0.25f, deckY, x1, deckY - lift);
            Pen dout(Color(255, 0, 0, 0), deckT + 3.f), dwood(Color(255, 247, 148, 29), deckT), dgrip(Color(255, 25, 22, 20), deckT * 0.35f);
            for (Pen* pn : {&dout, &dwood, &dgrip}) { pn->SetStartCap(LineCapRound); pn->SetEndCap(LineCapRound); pn->SetLineJoin(LineJoinRound); }
            SolidBrush truck(Color(255, 175, 175, 182)), wheel(Color(255, 245, 240, 225)), hub(Color(255, 60, 60, 60));
            Pen black(Color(255, 0, 0, 0), 1.6f), spoke(Color(255, 110, 110, 110), 1.4f);
            for (int k = -1; k <= 1; k += 2) {
                float wx = cx + k * L * 0.27f;
                PointF tp[4] = {PointF(wx - wr * 0.9f, deckY + deckT * 0.4f), PointF(wx + wr * 0.9f, deckY + deckT * 0.4f),
                                PointF(wx + wr * 0.45f, wy), PointF(wx - wr * 0.45f, wy)};
                g.FillPolygon(&truck, tp, 4);
                g.DrawPolygon(&black, tp, 4);
                g.FillEllipse(&wheel, wx - wr, wy - wr, wr * 2, wr * 2);
                g.DrawEllipse(&black, wx - wr, wy - wr, wr * 2, wr * 2);
                g.FillEllipse(&hub, wx - wr * 0.36f, wy - wr * 0.36f, wr * 0.72f, wr * 0.72f);
                float an = spin + k;
                g.DrawLine(&spoke, wx + wr * 0.48f * cosf(an), wy + wr * 0.48f * sinf(an), wx + wr * 0.85f * cosf(an),
                           wy + wr * 0.85f * sinf(an));
            }
            g.DrawPath(&dout, &deck);
            g.DrawPath(&dwood, &deck);
            g.TranslateTransform(0.f, -deckT * 0.32f);
            g.DrawPath(&dgrip, &deck);   // grip tape along the top
            g.ResetTransform();
            {   // the percentage riding on top of the board
                wchar_t pc[8];
                swprintf(pc, 8, L"%d%%", (int)GetWindowLongPtrW(h, GWLP_USERDATA));
                FontFamily ff(L"Impact");
                Font pf(&ff, H * 0.34f, FontStyleRegular, UnitPixel);
                StringFormat sf;
                sf.SetAlignment(StringAlignmentCenter);
                sf.SetLineAlignment(StringAlignmentFar);
                RectF box(cx - L / 2, -H * 0.2f, L, deckY + deckT * 0.75f + H * 0.2f);   // digits have no descent: sit on the deck
                GraphicsPath tp;
                tp.AddString(pc, -1, &ff, FontStyleRegular, H * 0.34f, box, &sf);
                Pen tout(Color(255, 0, 0, 0), 3.f);
                tout.SetLineJoin(LineJoinRound);
                SolidBrush tfill(Color(255, 255, 255, 255));
                g.DrawPath(&tout, &tp);
                g.FillPath(&tfill, &tp);
            }
        }
        BitBlt(wdc, 0, 0, W, H, dc, 0, 0, SRCCOPY);
        SelectObject(dc, ob);
        DeleteObject(bmp);
        DeleteDC(dc);
        EndPaint(h, &ps);
        return 0;
    }
    }
    return DefWindowProcW(h, m, wp, lp);
}

static void MakeStatusBar(const wchar_t* text, bool progress) {
    if (progress) {
        static bool reg;
        if (!reg) {
            WNDCLASSW wc = {};
            wc.lpfnWndProc = SkateBarProc;
            wc.hInstance = GetModuleHandleW(nullptr);
            wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
            wc.lpszClassName = L"UgSkateBar";
            RegisterClassW(&wc);
            reg = true;
        }
        g_progress = MakeAt(L"UgSkateBar", L"", 0, ID_PROGRESS, kBar);
    }
    g_status = MakeAt(L"STATIC", text, SS_LEFT | SS_CENTERIMAGE | SS_ENDELLIPSIS | SS_NOPREFIX, ID_STATUS,
                      progress ? kStatusSetup : kStatus, progress ? g_statusFont : g_smallFont);
}

static void CreateLauncherControls() {
    InitMonitors();
    HWND play = MakeBoard(0, L"PLAY", ID_PLAY);
    g_settingsBtn = MakeBoard(1, L"SETTINGS", ID_SETTINGS);
    g_updBtn = MakeBoard(2, L"UPDATE", ID_UPDATE);
    SetBoard(g_updBtn, L"UPDATE", L"Checking...", false);
    g_androidBtn = MakeBoard(3, L"ANDROID", ID_ANDROID);
    SetSub(ID_ANDROID, L"Build an APK for your phone");
    MakeBoard(4, L"LOGS", ID_LOGS);
    MakeBoard(5, L"UNINSTALL", ID_UNINSTALL);
    SetSub(ID_UNINSTALL, g_instDir);
    MakeStatusBar(L"Checking for updates...", false);
    MakeCheck(L"Fullscreen", ID_FULLSCREEN, 260, 772, 380);
    MakeCheck(L"60 fps", ID_FPS60, 700, 772, 300);
    MakeCheck(L"VSync", ID_VSYNC, 1060, 772, 300);
    MakeCombo(ID_MONITOR, kMonitor, g_nMonitors, 720, 841, 780);
    MakeCombo(ID_WINSIZE, kWinSize, N_WINSIZE, 720, 902, 780);
    MakeCombo(ID_SCREEN, kScreen, 2, 720, 963, 780);
    MakeCombo(ID_FIT, kFit, 2, 720, 1024, 780);
    MakeCombo(ID_SCALE, kScale, 6, 720, 1085, 780);
    MakeCombo(ID_PADS, kPads, 4, 720, 1146, 780);
    MakeCombo(ID_CAMERA, kCamera, 4, 720, 1207, 780);
    MakeCombo(ID_LEVEL, kLevel, 18, 720, 1268, 780);
    MakeCombo(ID_HD, nullptr, 0, 720, 1329, 780);
    FillHdCombo();
    for (int id : kSettingIds) ShowWindow(GetDlgItem(g_wnd, id), SW_HIDE);
    LoadSettings();
    SetFocus(play);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    ULONG_PTR gdipToken = 0;   // GDI+ for the skateboard progress bar
    { Gdiplus::GdiplusStartupInput gsi; Gdiplus::GdiplusStartup(&gdipToken, &gsi, nullptr); }
    INITCOMMONCONTROLSEX icc = {sizeof icc, ICC_PROGRESS_CLASS | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::wstring dirArg, isoArg;
    bool update = false, uninstall = false, launcher = false;
    for (int i = 1; i < argc; i++) {
        if (!wcscmp(argv[i], L"--update")) update = true;
        else if (!wcscmp(argv[i], L"--uninstall")) uninstall = true;
        else if (!wcscmp(argv[i], L"--launcher")) launcher = true;
        else if (!wcscmp(argv[i], L"--dir") && i + 1 < argc) dirArg = argv[++i];
        else if (!wcscmp(argv[i], L"--iso") && i + 1 < argc) isoArg = argv[++i];
    }
    if (launcher) {   // the installed copy: its own folder is the install
        g_instDir = dirArg.empty() ? ExeDir() : dirArg;
        if (!Exists(g_instDir + L"\\underground.exe")) {
            MessageBoxW(nullptr, (L"The game is not built in " + g_instDir + L". Run the Project Underground setup.").c_str(),
                        kAppName, MB_OK | MB_ICONERROR);
            return 1;
        }
        g_launcher = true;
    }
    std::wstring dir = dirArg.empty() ? DefaultDir() : dirArg;
    if (uninstall) return Uninstall(dir);
    bool installed = Exists(dir + L"\\game\\default.xbe") && Exists(dir + L"\\underground.exe");
    if (!launcher && !update && installed) {
        std::wstring q = std::wstring(kAppName) + L" is installed in " + dir + L" (version " + GetReg(L"Version") +
                         L"; this installer " + kVersion + L").\n\nYes: update it (keeps game data, settings and saves; "
                         L"no disc image needed).\nNo: full reinstall from a disc image.";
        int r = MessageBoxW(nullptr, q.c_str(), kAppName, MB_YESNOCANCEL | MB_ICONQUESTION);
        if (r == IDCANCEL) return 0;
        update = r == IDYES;
    }
    if (!launcher && update && !installed) {
        MessageBoxW(nullptr, (L"No installed game data in " + dir + L". Run a full install.").c_str(), kAppName, MB_OK);
        update = false;
    }
    g_updating = update;

    // borderless window the shape of the art, ~70% of the work area, centred
    RECT wa;
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    g_scale = std::min((wa.right - wa.left) * 0.7 / kArtW, (wa.bottom - wa.top) * 0.8 / kArtH);
    int w = S(kArtW), h = S(kArtH);
    auto font = [](int px, int weight, const wchar_t* face) {
        return CreateFontW(-S(px), 0, 0, 0, weight, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, face);
    };
    g_font = font(38, FW_NORMAL, L"Segoe UI");
    g_smallFont = font(32, FW_NORMAL, L"Segoe UI");
    g_subFont = font(32, FW_SEMIBOLD, L"Segoe UI");
    g_statusFont = font(24, FW_NORMAL, L"Segoe UI");   // setup status line, above the skateboard bar
    g_titleFont = font(76, FW_NORMAL, L"Impact");
    g_boardFont = font(84, FW_NORMAL, L"Impact");
    g_fieldBrush = CreateSolidBrush(kField);
    BuildArt(w, h);
    g_artBrush = g_plainBrush = CreatePatternBrush(g_artBmp);
    g_panelBrush = CreatePatternBrush(g_artPanelBmp);

    WNDCLASSW wc = {};
    wc.style = CS_DROPSHADOW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(IDI_APP));
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"ProjectUndergroundSetup";
    RegisterClassW(&wc);
    g_wnd = CreateWindowExW(WS_EX_APPWINDOW, wc.lpszClassName,
                            launcher ? L"Project Underground" : L"Project Underground Setup",
                            WS_POPUP | WS_SYSMENU | WS_MINIMIZEBOX, wa.left + (wa.right - wa.left - w) / 2,
                            wa.top + (wa.bottom - wa.top - h) / 2, w, h, nullptr, nullptr, inst, nullptr);
    SetWindowLongPtrW(MakeAt(L"BUTTON", L"", BS_PUSHBUTTON, ID_CLOSE, kClose), GWLP_USERDATA, K_CLOSE);
    if (launcher) {
        CreateLauncherControls();
        ShowWindow(g_wnd, SW_SHOW);
        g_worker = std::thread(CheckUpdates);
    } else {
        g_isoPath = isoArg;
        g_dirPath = dir;
        MakeBoard(0, L"DISC IMAGE", ID_ISO_BROWSE);
        SetSub(ID_ISO_BROWSE, update ? L"Not needed for an update"
                              : isoArg.empty() ? L"Click to choose your Xbox (USA) disc image" : isoArg);
        MakeBoard(1, L"INSTALL FOLDER", ID_DIR_BROWSE);
        SetSub(ID_DIR_BROWSE, dir);
        MakeBoard(2, L"DESKTOP ICON", ID_DESKTOP, true);
        MakeBoard(3, L"START MENU", ID_STARTMENU, true);
        SetOn(ID_DESKTOP, !update);
        SetOn(ID_STARTMENU, !update);
        g_install = MakeBoard(4, update ? L"UPDATE" : L"INSTALL", ID_INSTALL);
        SetSub(ID_INSTALL, update ? L"Keeps game data, settings and saves" : L"Builds the game on this PC");
        MakeBoard(5, L"QUIT", ID_QUIT);
        if (update)   // an update rebuilds the installed game in place: no disc image, same folder
            for (int id : {ID_ISO_BROWSE, ID_DIR_BROWSE}) EnableWindow(GetDlgItem(g_wnd, id), FALSE);
        MakeStatusBar(update ? L"Ready to update. Your game data, settings and saves are kept."
                             : L"The game is built on this PC from your disc image: a few minutes (the first install "
                               L"also downloads the compiler, about 190 MB).",
                      true);
        ShowWindow(g_wnd, SW_SHOW);
        SetFocus(g_install);
        // started by the game's updater (--update --dir) or with --iso: go straight away
        if ((update && !dirArg.empty()) || (!update && !isoArg.empty())) Start();
    }
    SetTimer(g_wnd, 77, 33, nullptr);   // gamepad navigation (PadPoll)
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        if (!IsDialogMessageW(g_wnd, &msg)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    }
    // launcher: an update check still waiting on the network must not keep the process alive
    if (g_worker.joinable()) { if (g_launcher) g_worker.detach(); else g_worker.join(); }
    return 0;
}
