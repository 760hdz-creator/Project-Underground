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
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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

enum { ID_ISO_EDIT = 200, ID_ISO_BROWSE, ID_DIR_EDIT, ID_DIR_BROWSE, ID_DESKTOP, ID_STARTMENU, ID_INSTALL, ID_PROGRESS,
       ID_STATUS, ID_PLAY, ID_UPDATE, ID_UNINSTALL, ID_FULLSCREEN, ID_SCALE, ID_PADS, ID_FPS60, ID_VSYNC, ID_LOGS,
       ID_MONITOR, ID_ANDROID };
#define WM_APP_STATUS (WM_APP + 1)
#define WM_APP_PROGRESS (WM_APP + 2)
#define WM_APP_DONE (WM_APP + 3)
#define WM_APP_UPDATE (WM_APP + 4)   // launcher: update check finished (wp 1 = newer version in g_upd)
#define WM_APP_UPDATING (WM_APP + 5) // launcher: wp 1 = new setup started (close), 0 = download failed
#define WM_APP_ANDROID (WM_APP + 6)  // launcher: Android APK build finished (wp 1 = ok; g_error / g_apk)

static HWND g_wnd, g_status, g_progress, g_iso, g_dir, g_install, g_updBtn;
static bool g_updating, g_running, g_launcher;
static std::wstring g_instDir;   // launcher: the install folder (the exe's own folder)
// Buttons are owner-drawn (dark, on the art). GWLP_USERDATA: 1 push button, 2 toggle (checkbox), | 4 when ticked.
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
static std::wstring GetText(HWND h) {
    int n = GetWindowTextLengthW(h);
    std::wstring s(n, 0);
    GetWindowTextW(h, s.data(), n + 1);
    return s;
}
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
    const wchar_t* parts[] = {L"python", L"tools", L"recomp", L"runtime", L"third_party", L"licenses", L"build", L"game",
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
    for (auto f : {L"underground.exe", L"underground.ico", L"VERSION", L"settings.ini"})
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
static ComboSetting kCombos[] = {{ID_MONITOR, L"UG_MONITOR", kMonitor, 1}, {ID_SCALE, L"UG_SCALE", kScale, 6},
                                 {ID_PADS, L"UG_PADS", kPads, 4}};
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
    SetOn(ID_FULLSCREEN, IniGet(g_instDir, L"settings", L"UG_FULLSCREEN") == L"1");
    SetOn(ID_FPS60, IniGet(g_instDir, L"settings", L"UG_FPS") != L"30");     // 60 fps is the default
    SetOn(ID_VSYNC, IniGet(g_instDir, L"settings", L"UG_VSYNC") != L"0");    // vsync (FIFO) is the default
}
static void SaveSettings() {
    for (auto& cs : kCombos) {
        int sel = (int)SendDlgItemMessageW(g_wnd, cs.id, CB_GETCURSEL, 0, 0);
        if (sel >= 0 && sel < cs.n) IniSet(g_instDir, L"settings", cs.key, cs.c[sel].value ? cs.c[sel].value : L"");
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
static const int kArtW = 1600, kArtH = 893;
static RECT kPanel = {60, 140, 760, 800};   // art pixels: the translucent control panel (left side)
static double g_scale = 1;
static HDC g_artDC;
static HBITMAP g_artBmp;
static HFONT g_font, g_titleFont, g_smallFont;
static HBRUSH g_fieldBrush, g_artBrush;   // g_artBrush: the panel art, behind labels
static const COLORREF kText = RGB(228, 232, 240), kDim = RGB(150, 160, 180), kAccent = RGB(255, 176, 60);
static const COLORREF kField = RGB(20, 24, 32), kEdge = RGB(150, 100, 40);
static int S(int v) { return (int)lround(v * g_scale); }

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

// The art with the darkened panel painted in (kept in g_artDC; WM_PAINT copies it).
static void BuildArt(int w, int h) {
    HDC screen = GetDC(nullptr);
    g_artDC = CreateCompatibleDC(screen);
    g_artBmp = LoadArt(w, h);
    if (!g_artBmp) g_artBmp = CreateCompatibleBitmap(screen, w, h);
    SelectObject(g_artDC, g_artBmp);
    ReleaseDC(nullptr, screen);
    RECT p = {S(kPanel.left), S(kPanel.top), S(kPanel.right), S(kPanel.bottom)};
    HDC tmp = CreateCompatibleDC(g_artDC);
    HBITMAP b = CreateCompatibleBitmap(g_artDC, 1, 1);
    HGDIOBJ old = SelectObject(tmp, b);
    SetPixel(tmp, 0, 0, RGB(6, 8, 12));
    BLENDFUNCTION bf = {AC_SRC_OVER, 0, 200, 0};
    GdiAlphaBlend(g_artDC, p.left, p.top, p.right - p.left, p.bottom - p.top, tmp, 0, 0, 1, 1, bf);
    SelectObject(tmp, old);
    DeleteObject(b);
    DeleteDC(tmp);
    HPEN pen = CreatePen(PS_SOLID, std::max(1, S(2)), kEdge);
    HGDIOBJ op = SelectObject(g_artDC, pen), ob = SelectObject(g_artDC, GetStockObject(NULL_BRUSH));
    Rectangle(g_artDC, p.left, p.top, p.right, p.bottom);
    SelectObject(g_artDC, op); SelectObject(g_artDC, ob);
    DeleteObject(pen);
    // static labels
    SetBkMode(g_artDC, TRANSPARENT);
    auto text = [&](int x, int y, HFONT f, COLORREF c, const std::wstring& s) {
        SelectObject(g_artDC, f);
        SetTextColor(g_artDC, c);
        TextOutW(g_artDC, S(x), S(y), s.c_str(), (int)s.size());
    };
    int x = kPanel.left + 30, t = kPanel.top;
    if (g_launcher) {
        text(x, t + 24, g_titleFont, kAccent, L"Project Underground");
        {   // long install paths end in an ellipsis inside the panel
            std::wstring v = L"Version " + std::wstring(kVersion) + L"   " + g_instDir;
            SelectObject(g_artDC, g_smallFont);
            SetTextColor(g_artDC, kDim);
            RECT r = {S(x), S(t + 84), S(kPanel.right - 20), S(t + 120)};
            DrawTextW(g_artDC, v.data(), (int)v.size(), &r, DT_SINGLELINE | DT_PATH_ELLIPSIS | DT_NOPREFIX);
        }
        text(x, t + 262, g_font, kAccent, L"Display");
        text(x, t + 371, g_font, kText, L"Monitor");
        text(x, t + 407, g_font, kText, L"Render scale");
        text(x, t + 443, g_font, kText, L"Players (gamepads)");
        text(x, t + 490, g_smallFont, kDim, L"Settings are saved at once to settings.ini in the install folder.");
        return;
    }
    text(x, t + 24, g_titleFont, kAccent, g_updating ? L"Update Project Underground" : L"Install Project Underground");
    text(x, t + 84, g_smallFont, kDim, L"Builds the game on this PC from your own disc image. Version " + std::wstring(kVersion) + L".");
    if (!g_updating) text(x, t + 130, g_font, kText, L"Tony Hawk's Underground disc image (Xbox, USA)");
    text(x, t + 230, g_font, kText, L"Install folder");
}

static HWND Make(const wchar_t* cls, const wchar_t* text, DWORD style, int id, int x, int y, int w, int h,
                 HFONT font = nullptr) {
    LONG_PTR kind = 0;
    if (!wcscmp(cls, L"BUTTON")) {
        kind = (style & BS_TYPEMASK) == BS_AUTOCHECKBOX ? 2 : 1;
        style = (style & ~BS_TYPEMASK) | BS_OWNERDRAW;
    }
    HWND c = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, S(x), S(y), S(w), S(h), g_wnd,
                             (HMENU)(INT_PTR)id, GetModuleHandleW(nullptr), nullptr);
    SendMessageW(c, WM_SETFONT, (WPARAM)(font ? font : g_font), TRUE);
    if (kind) SetWindowLongPtrW(c, GWLP_USERDATA, kind);
    return c;
}
// Lines the art brush up with control c, so its background is the art behind it.
static HBRUSH ArtBrushFor(HDC dc, HWND c) {
    RECT rc;
    GetWindowRect(c, &rc);
    MapWindowPoints(nullptr, g_wnd, (POINT*)&rc, 2);
    SetBrushOrgEx(dc, -rc.left, -rc.top, nullptr);
    return g_artBrush;
}
static void DrawButton(const DRAWITEMSTRUCT* di) {
    HDC dc = di->hDC;
    RECT r = di->rcItem;
    FillRect(dc, &r, ArtBrushFor(dc, di->hwndItem));
    LONG_PTR kind = GetWindowLongPtrW(di->hwndItem, GWLP_USERDATA);
    bool dis = di->itemState & ODS_DISABLED, focus = di->itemState & ODS_FOCUS, down = di->itemState & ODS_SELECTED;
    wchar_t text[128];
    GetWindowTextW(di->hwndItem, text, 128);
    SelectObject(dc, (HFONT)SendMessageW(di->hwndItem, WM_GETFONT, 0, 0));
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, dis ? kDim : kText);
    HPEN pen = CreatePen(PS_SOLID, std::max(1, S(2)), focus ? kAccent : kEdge);
    HGDIOBJ op = SelectObject(dc, pen);
    if (kind & 2) {   // toggle: box + label
        int b = S(24), y0 = (r.top + r.bottom - b) / 2;
        HBRUSH fill = CreateSolidBrush(kField);
        HGDIOBJ ob = SelectObject(dc, fill);
        Rectangle(dc, r.left + S(2), y0, r.left + S(2) + b, y0 + b);
        SelectObject(dc, ob);
        DeleteObject(fill);
        if (kind & 4) {
            RECT in = {r.left + S(2) + S(6), y0 + S(6), r.left + S(2) + b - S(6), y0 + b - S(6)};
            HBRUSH on = CreateSolidBrush(kAccent);
            FillRect(dc, &in, on);
            DeleteObject(on);
        }
        RECT tr = {r.left + b + S(14), r.top, r.right, r.bottom};
        DrawTextW(dc, text, -1, &tr, DT_SINGLELINE | DT_VCENTER | DT_LEFT);
    } else {          // push button
        HBRUSH fill = CreateSolidBrush(dis ? RGB(18, 20, 26) : down ? RGB(120, 80, 30) : RGB(40, 34, 30));
        HGDIOBJ ob = SelectObject(dc, fill);
        RoundRect(dc, r.left + 1, r.top + 1, r.right - 1, r.bottom - 1, S(10), S(10));
        SelectObject(dc, ob);
        DeleteObject(fill);
        DrawTextW(dc, text, -1, &r, DT_SINGLELINE | DT_VCENTER | DT_CENTER);
    }
    SelectObject(dc, op);
    DeleteObject(pen);
}
static HWND MakeCheck(const wchar_t* text, int id, int x, int y, int w) {
    return Make(L"BUTTON", text, BS_AUTOCHECKBOX | WS_TABSTOP, id, x, y, w, 34);
}

static void BrowseIso() {
    wchar_t file[MAX_PATH] = L"";
    OPENFILENAMEW of = {sizeof of};
    of.hwndOwner = g_wnd;
    of.lpstrFilter = L"Xbox disc image (*.iso;*.xiso)\0*.iso;*.xiso\0All files\0*.*\0";
    of.lpstrFile = file;
    of.nMaxFile = MAX_PATH;
    of.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    if (GetOpenFileNameW(&of)) SetWindowTextW(g_iso, file);
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
    SetWindowTextW(g_dir, s.c_str());
}

static void Start() {
    std::wstring iso = g_iso ? GetText(g_iso) : L"", dir = GetText(g_dir);
    if (!g_updating && !Exists(iso)) { MessageBoxW(g_wnd, L"Choose your Tony Hawk's Underground disc image.", kAppName, MB_OK); return; }
    if (dir.empty()) return;
    g_running = true;
    EnableWindow(g_install, FALSE);
    bool desk = IsOn(ID_DESKTOP), sm = IsOn(ID_STARTMENU);
    g_worker = std::thread(Work, iso, dir, desk, sm, g_updating);
}

// After an install: open the launcher.
static void Launch(const std::wstring& dir) {
    ShellExecuteW(nullptr, L"open", (dir + L"\\launcher.exe").c_str(), L"--launcher", dir.c_str(), SW_SHOWNORMAL);
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
    case ID_UPDATE:
        if (g_worker.joinable()) return true;
        EnableWindow(g_updBtn, FALSE);
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
        EnableWindow(g_androidBtn, FALSE);
        SetWindowTextW(g_status, L"Android: starting...");
        g_worker = std::thread(BuildAndroid);
        return true;
    case ID_FULLSCREEN: case ID_FPS60: case ID_VSYNC:
        if (code == BN_CLICKED) SaveSettings();
        return true;
    case ID_SCALE: case ID_PADS: case ID_MONITOR:
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
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX:
        SetTextColor((HDC)wp, kText);
        SetBkColor((HDC)wp, kField);
        return (LRESULT)g_fieldBrush;
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN:
        SetTextColor((HDC)wp, (HWND)lp == g_status ? kDim : kText);
        SetBkMode((HDC)wp, TRANSPARENT);
        return (LRESULT)ArtBrushFor((HDC)wp, (HWND)lp);
    case WM_DRAWITEM: DrawButton((const DRAWITEMSTRUCT*)lp); return TRUE;
    case WM_COMMAND:
        if ((HIWORD(wp) == BN_CLICKED || HIWORD(wp) == BN_DOUBLECLICKED) && lp &&
            (GetWindowLongPtrW((HWND)lp, GWLP_USERDATA) & 2)) {
            SetOn(LOWORD(wp), !IsOn(LOWORD(wp)));
            wp = MAKEWPARAM(LOWORD(wp), BN_CLICKED);
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
            ShowWindow(g_updBtn, SW_SHOW);
            EnableWindow(g_updBtn, TRUE);
        } else {
            SetWindowTextW(g_status, lp ? (std::wstring(L"Up to date (version ") + kVersion + L").").c_str()
                                        : L"Could not check for updates (offline?).");
        }
        return 0;
    case WM_APP_ANDROID:
        if (g_worker.joinable()) g_worker.join();
        EnableWindow(g_androidBtn, TRUE);
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
        EnableWindow(g_updBtn, TRUE);
        return 0;
    case WM_APP_DONE:
        if (g_worker.joinable()) g_worker.join();
        g_running = false;
        if (wp) {
            SetWindowTextW(g_status, (std::wstring(g_updating ? L"Updated to version " : L"Installed version ") + kVersion +
                                      L" in " + g_doneDir + L". Play opens the launcher (display settings).").c_str());
            SetWindowTextW(g_install, L"Play");
            EnableWindow(g_install, TRUE);
        } else {
            g_doneDir.clear();
            SetWindowTextW(g_status, L"Failed. See the message.");
            MessageBoxW(h, g_error.c_str(), kAppName, MB_OK | MB_ICONERROR);
            EnableWindow(g_install, TRUE);
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
    HWND cb = Make(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, id, x, y, w, 300, font);
    SetWindowTheme(cb, L"", L"");
    for (int i = 0; i < n; i++) SendMessageW(cb, CB_ADDSTRING, 0, (LPARAM)c[i].label);
}

static void CreateLauncherControls() {
    InitMonitors();
    int x = kPanel.left + 30, t = kPanel.top, fw = kPanel.right - kPanel.left - 60, cx = x + 260, cw = fw - 260;
    HWND play = Make(L"BUTTON", L"Play", BS_DEFPUSHBUTTON | WS_TABSTOP, ID_PLAY, x, t + 130, 240, 64, g_titleFont);
    g_updBtn = Make(L"BUTTON", L"Update", BS_PUSHBUTTON | WS_TABSTOP, ID_UPDATE, x + 260, t + 130, 200, 64);
    ShowWindow(g_updBtn, SW_HIDE);
    g_status = Make(L"STATIC", L"Checking for updates...", SS_LEFT, ID_STATUS, x, t + 205, fw, 50, g_smallFont);
    MakeCheck(L"Fullscreen", ID_FULLSCREEN, x, t + 296, 220);
    MakeCheck(L"60 fps", ID_FPS60, x + 240, t + 296, 160);
    MakeCheck(L"VSync", ID_VSYNC, x + 420, t + 296, 160);
    MakeCombo(ID_MONITOR, kMonitor, g_nMonitors, cx, t + 366, cw);
    MakeCombo(ID_SCALE, kScale, 6, cx, t + 402, cw);
    MakeCombo(ID_PADS, kPads, 4, cx, t + 438, cw);
    Make(L"BUTTON", L"Logs", BS_PUSHBUTTON | WS_TABSTOP, ID_LOGS, x + fw - 350, t + 560, 170, 36, g_smallFont);
    Make(L"BUTTON", L"Uninstall", BS_PUSHBUTTON | WS_TABSTOP, ID_UNINSTALL, x + fw - 170, t + 560, 170, 36, g_smallFont);
    g_androidBtn = Make(L"BUTTON", L"Android...", BS_PUSHBUTTON | WS_TABSTOP, ID_ANDROID, x, t + 560, 200, 36, g_smallFont);
    LoadSettings();
    SetFocus(play);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
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
        kPanel = {60, 50, 800, 690};
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

    // window sized to the art, scaled to ~70% of the work area
    RECT wa;
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    g_scale = std::min((wa.right - wa.left) * 0.7 / kArtW, (wa.bottom - wa.top) * 0.8 / kArtH);
    int w = S(kArtW), h = S(kArtH);
    g_font = CreateFontW(-S(22), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    g_smallFont = CreateFontW(-S(18), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    g_titleFont = CreateFontW(-S(44), 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    g_fieldBrush = CreateSolidBrush(kField);
    BuildArt(w, h);
    g_artBrush = CreatePatternBrush(g_artBmp);

    WNDCLASSW wc = {};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(IDI_APP));
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"ProjectUndergroundSetup";
    RegisterClassW(&wc);
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    RECT r = {0, 0, w, h};
    AdjustWindowRect(&r, style, FALSE);
    g_wnd = CreateWindowW(wc.lpszClassName, launcher ? L"Project Underground" : L"Project Underground Setup", style,
                          CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top, nullptr, nullptr, inst, nullptr);
    if (launcher) {
        CreateLauncherControls();
        ShowWindow(g_wnd, SW_SHOW);
        g_worker = std::thread(CheckUpdates);
    } else {
        int x = kPanel.left + 30, t = kPanel.top, fw = kPanel.right - kPanel.left - 60;
        if (!update) {
            g_iso = Make(L"EDIT", L"", ES_AUTOHSCROLL | WS_BORDER, ID_ISO_EDIT, x, t + 165, fw - 130, 38);
            Make(L"BUTTON", L"Browse...", BS_PUSHBUTTON, ID_ISO_BROWSE, x + fw - 120, t + 165, 120, 38);
        }
        g_dir = Make(L"EDIT", dir.c_str(), ES_AUTOHSCROLL | WS_BORDER | (update ? ES_READONLY : 0), ID_DIR_EDIT, x,
                     t + 265, fw - 130, 38);
        if (!update) Make(L"BUTTON", L"Browse...", BS_PUSHBUTTON, ID_DIR_BROWSE, x + fw - 120, t + 265, 120, 38);
        MakeCheck(L"Desktop shortcut", ID_DESKTOP, x, t + 335, 260);
        MakeCheck(L"Start menu shortcut", ID_STARTMENU, x + 280, t + 335, 280);
        SetOn(ID_DESKTOP, !update);
        SetOn(ID_STARTMENU, !update);
        g_install = Make(L"BUTTON", update ? L"Update" : L"Install", BS_DEFPUSHBUTTON, ID_INSTALL, x, t + 390, 200, 52);
        g_progress = Make(PROGRESS_CLASSW, L"", PBS_SMOOTH, ID_PROGRESS, x, t + 465, fw, 20);
        SendMessageW(g_progress, PBM_SETRANGE, 0, MAKELPARAM(0, 100));
        SendMessageW(g_progress, PBM_SETBARCOLOR, 0, kAccent);
        SendMessageW(g_progress, PBM_SETBKCOLOR, 0, kField);
        g_status = Make(L"STATIC", update ? L"Ready to update. Your game data, settings and saves are kept."
                                           : L"The game is built on this PC from your disc image (a few minutes; the first "
                                             L"install also downloads the compiler, about 190 MB).",
                        SS_LEFT, ID_STATUS, x, t + 500, fw, 80);
        ShowWindow(g_wnd, SW_SHOW);
        if (g_iso && !isoArg.empty()) SetWindowTextW(g_iso, isoArg.c_str());
        // started by the game's updater (--update --dir) or with --iso: go straight away
        if ((update && !dirArg.empty()) || (!update && !isoArg.empty())) Start();
    }
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        if (!IsDialogMessageW(g_wnd, &msg)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    }
    // launcher: an update check still waiting on the network must not keep the process alive
    if (g_worker.joinable()) { if (g_launcher) g_worker.detach(); else g_worker.join(); }
    return 0;
}
