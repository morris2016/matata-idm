// matata-gui.exe — WebView2-based GUI shell.
//
// The window hosts a single WebView2 control filling the client area; all
// rendering is HTML/CSS in ui/. Communication is JSON over
// chrome.webview.postMessage (JS → C++) and ExecuteScriptAsync (C++ → JS).
// The C++ side keeps the Downloader / VideoGrabber / FtpDownloader workers,
// owns the filesystem state, and persists settings to HKCU.

#define NOMINMAX
#define _WIN32_WINNT 0x0A00

#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <wincrypt.h>

#include <wrl/client.h>
#include <wrl/event.h>
#include <wrl/implements.h>

#include "WebView2.h"

#include "matata/categories.hpp"
#include "matata/downloader.hpp"
#include "matata/http.hpp"
#include "matata/ftp_downloader.hpp"
#include "matata/hls.hpp"
#include "matata/dash.hpp"
#include "matata/video.hpp"
#include "matata/url.hpp"
#include "matata/updater.hpp"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <ctime>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace matata;
using Microsoft::WRL::ComPtr;
using Microsoft::WRL::Callback;
using Microsoft::WRL::Make;

namespace {

// ---- constants -------------------------------------------------------

constexpr wchar_t kClassName[]    = L"MatataMainWnd"; // SAME as legacy GUI so the extension's WM_COPYDATA finds us.
constexpr wchar_t kAppTitle[]     = L"matata";
constexpr wchar_t kInstanceMutex[]= L"Local\\com.matata.gui.singleton";
constexpr wchar_t kVirtualHost[]  = L"matata.local";
constexpr ULONG_PTR kHandoffMagic = 0x4D415441; // 'MATA' — matches legacy.

constexpr UINT WM_APP_BRIDGE_EVENT  = WM_APP + 10; // post JSON to UI
constexpr UINT WM_APP_PROGRESS      = WM_APP + 11; // worker progress
constexpr UINT WM_APP_COMPLETED     = WM_APP + 12; // worker done
constexpr UINT WM_APP_UPDATE_AVAIL  = WM_APP + 13; // background update check found newer version
constexpr UINT WM_APP_TRAYICON      = WM_APP + 14; // Shell_NotifyIcon callback

// Update manifest published at the project's GitHub Pages site. Schema:
//   { "version": "0.9.4", "url": "https://.../matata-0.9.4-setup.exe",
//     "notes":   "What's new in 0.9.4..." }
// Bumping the file there triggers the in-app prompt on next launch.
constexpr const wchar_t* kUpdateManifestUrl =
    L"https://morris2016.github.io/matata-idm/latest.json";

// ---- globals ---------------------------------------------------------

HINSTANCE g_hInst   = nullptr;
HWND      g_hwnd    = nullptr;
HANDLE    g_instanceMutex = nullptr;

ComPtr<ICoreWebView2Controller> g_controller;
ComPtr<ICoreWebView2>           g_webview;
bool g_webviewReady = false;
std::vector<std::wstring> g_pendingScripts; // sent before webview was ready

// ---- diagnostic log (~/AppData/Local/Temp/matata-gui.log) -------------

void dbgLog(const wchar_t* fmt, ...) {
    wchar_t path[MAX_PATH];
    DWORD n = GetTempPathW(MAX_PATH, path);
    if (n == 0 || n >= MAX_PATH) return;
    wcscat_s(path, MAX_PATH, L"matata-gui.log");
    HANDLE h = CreateFileW(path, FILE_APPEND_DATA,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    SYSTEMTIME st; GetLocalTime(&st);
    wchar_t pre[64];
    _snwprintf_s(pre, _TRUNCATE, L"[%02d:%02d:%02d.%03d pid=%lu] ",
                 st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
                 GetCurrentProcessId());
    wchar_t body[4096];
    va_list ap; va_start(ap, fmt);
    int len = _vsnwprintf_s(body, _TRUNCATE, fmt, ap);
    va_end(ap);
    if (len < 0) len = 0;
    std::wstring line = pre;
    line += body;
    line += L"\r\n";
    int u8 = WideCharToMultiByte(CP_UTF8, 0, line.c_str(), (int)line.size(),
                                 nullptr, 0, nullptr, nullptr);
    std::string utf8(u8, '\0');
    WideCharToMultiByte(CP_UTF8, 0, line.c_str(), (int)line.size(),
                        utf8.data(), u8, nullptr, nullptr);
    SetFilePointer(h, 0, nullptr, FILE_END);
    DWORD w = 0;
    WriteFile(h, utf8.data(), (DWORD)utf8.size(), &w, nullptr);
    CloseHandle(h);
}

// ---- handoff (compatible with legacy gui_main.cpp + extension) -------

struct HandoffSpec {
    std::wstring url;
    std::wstring filename;
    std::wstring outDir;
    std::wstring referer;
    std::wstring cookie;
    std::wstring userAgent;
    std::wstring ytFormat;     // yt-dlp -f selector (e.g. "bv*[height=1080]+ba/b")
    std::wstring cookiesPath;  // path to a Netscape cookies.txt the host wrote
};

std::wstring serializeHandoff(const HandoffSpec& s) {
    std::wstring out;
    auto put = [&](const wchar_t* k, const std::wstring& v) {
        if (v.empty()) return;
        out.append(k); out.push_back(L'\0');
        out.append(v); out.push_back(L'\0');
    };
    put(L"url",       s.url);
    put(L"filename",  s.filename);
    put(L"outDir",    s.outDir);
    put(L"referer",   s.referer);
    put(L"cookie",    s.cookie);
    put(L"userAgent", s.userAgent);
    put(L"ytFormat",  s.ytFormat);
    put(L"cookiesPath", s.cookiesPath);
    return out;
}

HandoffSpec deserializeHandoff(const wchar_t* data, size_t count) {
    HandoffSpec s;
    size_t i = 0;
    auto readField = [&](std::wstring& out) -> bool {
        size_t start = i;
        while (i < count && data[i] != L'\0') ++i;
        if (i >= count) return false;
        out.assign(data + start, i - start);
        ++i;
        return true;
    };
    while (i < count) {
        std::wstring key, val;
        if (!readField(key)) break;
        if (!readField(val)) break;
        if      (key == L"url")       s.url = std::move(val);
        else if (key == L"filename")  s.filename = std::move(val);
        else if (key == L"outDir")    s.outDir = std::move(val);
        else if (key == L"referer")   s.referer = std::move(val);
        else if (key == L"cookie")    s.cookie = std::move(val);
        else if (key == L"userAgent") s.userAgent = std::move(val);
        else if (key == L"ytFormat")  s.ytFormat = std::move(val);
        else if (key == L"cookiesPath") s.cookiesPath = std::move(val);
    }
    return s;
}

bool parseHandoffArgs(PWSTR cmdLine, HandoffSpec& out) {
    if (!cmdLine || !*cmdLine) return false;
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(cmdLine, &argc);
    if (!argv) return false;
    auto next = [&](int& i, std::wstring& dst) {
        if (i + 1 < argc) dst = argv[++i];
    };
    for (int i = 0; i < argc; ++i) {
        std::wstring a = argv[i];
        if      (a == L"--add-url")    next(i, out.url);
        else if (a == L"--filename")   next(i, out.filename);
        else if (a == L"--out-dir")    next(i, out.outDir);
        else if (a == L"--referer")    next(i, out.referer);
        else if (a == L"--cookie")     next(i, out.cookie);
        else if (a == L"--user-agent") next(i, out.userAgent);
        else if (a == L"--yt-format")  next(i, out.ytFormat);
        else if (a == L"--cookies-file") next(i, out.cookiesPath);
        else if (out.url.empty()) {
            if (a.size() > 9 && a.compare(0, 9, L"matata://") == 0) out.url = a.substr(9);
            else if (a.compare(0, 7, L"http://")  == 0 ||
                     a.compare(0, 8, L"https://") == 0 ||
                     a.compare(0, 6, L"ftp://")   == 0 ||
                     a.compare(0, 7, L"ftps://")  == 0) out.url = a;
        }
    }
    LocalFree(argv);
    auto trim = [](std::wstring& s) {
        while (!s.empty() && (s.front() == L'"' || s.front() == L' ')) s.erase(s.begin());
        while (!s.empty() && (s.back()  == L'"' || s.back()  == L' ')) s.pop_back();
    };
    trim(out.url); trim(out.filename); trim(out.outDir);
    trim(out.referer); trim(out.cookie); trim(out.userAgent); trim(out.ytFormat); trim(out.cookiesPath);
    return !out.url.empty();
}

bool forwardHandoffToExistingInstance(const HandoffSpec& spec) {
    HWND target = FindWindowW(kClassName, nullptr);
    if (!target) return false;
    std::wstring blob = serializeHandoff(spec);
    COPYDATASTRUCT cds{};
    cds.dwData = kHandoffMagic;
    cds.cbData = (DWORD)(blob.size() * sizeof(wchar_t));
    cds.lpData = (PVOID)blob.data();
    if (IsIconic(target)) ShowWindow(target, SW_RESTORE);
    ShowWindow(target, SW_SHOW);
    SetForegroundWindow(target);
    DWORD_PTR result = 0;
    SendMessageTimeoutW(target, WM_COPYDATA, 0, (LPARAM)&cds,
                        SMTO_ABORTIFHUNG, 5000, &result);
    return true;
}

// ---- settings (HKCU) -------------------------------------------------

struct Settings {
    std::wstring outputDir;
    int          connections     = 8;
    int          maxJobs         = 3;
    int64_t      bandwidthBps    = 0;
    bool         clipboardWatch  = false;
    bool         verifyChecksum  = true;
    bool         categorize      = false;
    // Global "Stop queue" toggle. When true, addDownloadEx parks new items
    // in Queued and dispatchQueued doesn't promote anything -- already-running
    // downloads keep going. Mirrors IDM's Downloads -> Stop queue.
    bool         queuePaused     = false;
    // Scheduler. When schedEnabled, a worker tick toggles queuePaused on the
    // Start/Stop edges and (optionally) aborts running items at Stop. Times
    // are minutes-since-midnight. daysMask is bit 0=Sun..bit 6=Sat. If the
    // start/stop window wraps midnight (start > stop) it spans two days.
    // shutdownWhenDone: once a download lands successfully while enabled,
    // we arm a one-shot; when the queue subsequently fully drains we run
    // shutdown.exe with a 60s grace period.
    bool         schedEnabled        = false;
    int          schedStartMinutes   = 22 * 60;   // 22:00
    int          schedStopMinutes    = 7  * 60;   // 07:00
    int          schedDaysMask       = 0x7F;      // every day
    bool         schedShutdownDone   = false;
    // v0.9.7 General-tab additions.
    bool         launchOnStartup     = false;   // HKCU\...\Run entry
    bool         soundOnComplete     = true;    // MessageBeep on Done/Err
    // Per-category download folders. Blank = fall through to outputDir.
    // Used only when categorize=true and the item has no explicit outDir.
    std::wstring catDirVideo;
    std::wstring catDirMusic;
    std::wstring catDirArchive;
    std::wstring catDirProgram;
    std::wstring catDirDocument;
    // v0.9.8 Proxy.
    int          proxyMode      = 0;     // 0=system, 1=none, 2=manual
    std::wstring proxyServer;            // "host:port"
    std::wstring proxyBypass;            // ";"-separated list, or "<local>"
    std::wstring proxyUser;
    std::wstring proxyPass;              // plaintext for now; v0.9.9 to DPAPI
    // v0.9.9 Sites Logins. Plaintext list lives in memory and is pushed
    // into the HTTP layer (http.cpp::setSiteLogins). On disk the list is
    // a single REG_BINARY blob with each password DPAPI-encrypted.
    std::vector<SiteLogin> siteLogins;
    int          windowX = CW_USEDEFAULT, windowY = CW_USEDEFAULT;
    int          windowW = 1180, windowH = 760;
    bool         windowMaximized = false;
};

Settings g_settings;

// ---- Sites Logins persistence (DPAPI + REG_BINARY blob) -------------

std::vector<unsigned char> dpapiProtect(const std::wstring& plaintext) {
    DATA_BLOB in{}, out{};
    in.pbData = (BYTE*)plaintext.data();
    in.cbData = (DWORD)(plaintext.size() * sizeof(wchar_t));
    if (!CryptProtectData(&in, L"matata-site-login", nullptr, nullptr,
                          nullptr, 0, &out)) return {};
    std::vector<unsigned char> v(out.pbData, out.pbData + out.cbData);
    LocalFree(out.pbData);
    return v;
}

std::wstring dpapiUnprotect(const unsigned char* data, size_t len) {
    DATA_BLOB in{}, out{};
    in.pbData = (BYTE*)data;
    in.cbData = (DWORD)len;
    if (!CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr,
                            0, &out)) return L"";
    std::wstring s((const wchar_t*)out.pbData, out.cbData / sizeof(wchar_t));
    LocalFree(out.pbData);
    return s;
}

template <typename T>
void blobAppendU32(std::vector<T>& blob, uint32_t v) {
    static_assert(sizeof(T) == 1, "byte vector required");
    blob.push_back((T)( v        & 0xFF));
    blob.push_back((T)((v >>  8) & 0xFF));
    blob.push_back((T)((v >> 16) & 0xFF));
    blob.push_back((T)((v >> 24) & 0xFF));
}

bool blobReadU32(const unsigned char* p, size_t n, size_t& off, uint32_t& v) {
    if (off + 4 > n) return false;
    v = (uint32_t)p[off]
      | ((uint32_t)p[off+1] <<  8)
      | ((uint32_t)p[off+2] << 16)
      | ((uint32_t)p[off+3] << 24);
    off += 4;
    return true;
}

std::vector<unsigned char> serializeSiteLogins(const std::vector<SiteLogin>& list) {
    std::vector<unsigned char> blob;
    blobAppendU32(blob, (uint32_t)list.size());
    for (auto& s : list) {
        auto pushWide = [&](const std::wstring& w) {
            uint32_t cb = (uint32_t)(w.size() * sizeof(wchar_t));
            blobAppendU32(blob, cb);
            blob.insert(blob.end(),
                        (const unsigned char*)w.data(),
                        (const unsigned char*)w.data() + cb);
        };
        pushWide(s.host);
        pushWide(s.user);
        auto enc = dpapiProtect(s.pass);
        blobAppendU32(blob, (uint32_t)enc.size());
        blob.insert(blob.end(), enc.begin(), enc.end());
    }
    return blob;
}

std::vector<SiteLogin> deserializeSiteLogins(const unsigned char* data, size_t len) {
    std::vector<SiteLogin> out;
    size_t off = 0;
    uint32_t count = 0;
    if (!blobReadU32(data, len, off, count)) return out;
    if (count > 4096) return out;
    out.reserve(count);
    auto readWide = [&](std::wstring& w) -> bool {
        uint32_t cb = 0;
        if (!blobReadU32(data, len, off, cb)) return false;
        if (cb > len - off || (cb % sizeof(wchar_t)) != 0) return false;
        w.assign((const wchar_t*)(data + off), cb / sizeof(wchar_t));
        off += cb;
        return true;
    };
    for (uint32_t i = 0; i < count; ++i) {
        SiteLogin s;
        if (!readWide(s.host) || !readWide(s.user)) return out;
        uint32_t encLen = 0;
        if (!blobReadU32(data, len, off, encLen)) return out;
        if (encLen > len - off) return out;
        s.pass = dpapiUnprotect(data + off, encLen);
        off += encLen;
        out.push_back(std::move(s));
    }
    return out;
}

void loadSettings() {
    HKEY hk;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\matata", 0, KEY_READ, &hk) != ERROR_SUCCESS)
        return;
    auto rdString = [&](const wchar_t* name, std::wstring& out) {
        DWORD type = 0, cb = 0;
        if (RegQueryValueExW(hk, name, nullptr, &type, nullptr, &cb) != ERROR_SUCCESS) return;
        if (type != REG_SZ || cb == 0) return;
        out.resize(cb / sizeof(wchar_t));
        RegQueryValueExW(hk, name, nullptr, nullptr, (LPBYTE)out.data(), &cb);
        if (!out.empty() && out.back() == L'\0') out.pop_back();
    };
    auto rdDword = [&](const wchar_t* name, DWORD& out) {
        DWORD type = 0, cb = sizeof(out);
        RegQueryValueExW(hk, name, nullptr, &type, (LPBYTE)&out, &cb);
    };
    auto rdQword = [&](const wchar_t* name, int64_t& out) {
        DWORD type = 0, cb = sizeof(out);
        RegQueryValueExW(hk, name, nullptr, &type, (LPBYTE)&out, &cb);
    };
    rdString(L"outputDir", g_settings.outputDir);
    DWORD d;
    d = (DWORD)g_settings.connections;     rdDword(L"connections",     d); g_settings.connections = (int)d;
    d = (DWORD)g_settings.maxJobs;         rdDword(L"maxJobs",         d); g_settings.maxJobs     = (int)d;
    rdQword(L"bandwidthBps", g_settings.bandwidthBps);
    d = g_settings.clipboardWatch ? 1 : 0; rdDword(L"clipboardWatch",  d); g_settings.clipboardWatch  = (d != 0);
    d = g_settings.verifyChecksum ? 1 : 0; rdDword(L"verifyChecksum",  d); g_settings.verifyChecksum  = (d != 0);
    d = g_settings.categorize     ? 1 : 0; rdDword(L"categorize",      d); g_settings.categorize      = (d != 0);
    d = g_settings.queuePaused    ? 1 : 0; rdDword(L"queuePaused",     d); g_settings.queuePaused     = (d != 0);
    d = g_settings.schedEnabled   ? 1 : 0; rdDword(L"schedEnabled",    d); g_settings.schedEnabled    = (d != 0);
    d = (DWORD)g_settings.schedStartMinutes; rdDword(L"schedStartMinutes", d); g_settings.schedStartMinutes = (int)d;
    d = (DWORD)g_settings.schedStopMinutes;  rdDword(L"schedStopMinutes",  d); g_settings.schedStopMinutes  = (int)d;
    d = (DWORD)g_settings.schedDaysMask;     rdDword(L"schedDaysMask",     d); g_settings.schedDaysMask     = (int)d;
    d = g_settings.schedShutdownDone ? 1 : 0; rdDword(L"schedShutdownDone", d); g_settings.schedShutdownDone = (d != 0);
    d = g_settings.launchOnStartup ? 1 : 0; rdDword(L"launchOnStartup", d); g_settings.launchOnStartup = (d != 0);
    d = g_settings.soundOnComplete ? 1 : 0; rdDword(L"soundOnComplete", d); g_settings.soundOnComplete = (d != 0);
    rdString(L"catDirVideo",    g_settings.catDirVideo);
    rdString(L"catDirMusic",    g_settings.catDirMusic);
    rdString(L"catDirArchive",  g_settings.catDirArchive);
    rdString(L"catDirProgram",  g_settings.catDirProgram);
    rdString(L"catDirDocument", g_settings.catDirDocument);
    d = (DWORD)g_settings.proxyMode; rdDword(L"proxyMode", d); g_settings.proxyMode = (int)d;
    rdString(L"proxyServer", g_settings.proxyServer);
    rdString(L"proxyBypass", g_settings.proxyBypass);
    rdString(L"proxyUser",   g_settings.proxyUser);
    rdString(L"proxyPass",   g_settings.proxyPass);
    {
        DWORD type = 0, cb = 0;
        if (RegQueryValueExW(hk, L"siteLogins", nullptr, &type,
                             nullptr, &cb) == ERROR_SUCCESS &&
            type == REG_BINARY && cb > 0 && cb < 1024 * 1024) {
            std::vector<unsigned char> blob(cb);
            if (RegQueryValueExW(hk, L"siteLogins", nullptr, nullptr,
                                 blob.data(), &cb) == ERROR_SUCCESS) {
                g_settings.siteLogins = deserializeSiteLogins(blob.data(), blob.size());
            }
        }
    }
    d = (DWORD)g_settings.windowX; rdDword(L"windowX", d); g_settings.windowX = (int)d;
    d = (DWORD)g_settings.windowY; rdDword(L"windowY", d); g_settings.windowY = (int)d;
    d = (DWORD)g_settings.windowW; rdDword(L"windowW", d); g_settings.windowW = (int)d;
    d = (DWORD)g_settings.windowH; rdDword(L"windowH", d); g_settings.windowH = (int)d;
    d = g_settings.windowMaximized ? 1 : 0; rdDword(L"windowMaximized", d); g_settings.windowMaximized = (d != 0);
    RegCloseKey(hk);
}

void saveSettings() {
    HKEY hk;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\matata", 0, nullptr,
                        REG_OPTION_NON_VOLATILE, KEY_WRITE, nullptr, &hk, nullptr) != ERROR_SUCCESS)
        return;
    auto wrString = [&](const wchar_t* name, const std::wstring& v) {
        RegSetValueExW(hk, name, 0, REG_SZ, (const BYTE*)v.c_str(),
                       (DWORD)((v.size() + 1) * sizeof(wchar_t)));
    };
    auto wrDword = [&](const wchar_t* name, DWORD v) {
        RegSetValueExW(hk, name, 0, REG_DWORD, (const BYTE*)&v, sizeof(v));
    };
    auto wrQword = [&](const wchar_t* name, int64_t v) {
        RegSetValueExW(hk, name, 0, REG_QWORD, (const BYTE*)&v, sizeof(v));
    };
    wrString(L"outputDir", g_settings.outputDir);
    wrDword(L"connections",     (DWORD)g_settings.connections);
    wrDword(L"maxJobs",         (DWORD)g_settings.maxJobs);
    wrQword(L"bandwidthBps",    g_settings.bandwidthBps);
    wrDword(L"clipboardWatch",  g_settings.clipboardWatch ? 1 : 0);
    wrDword(L"verifyChecksum",  g_settings.verifyChecksum ? 1 : 0);
    wrDword(L"categorize",      g_settings.categorize     ? 1 : 0);
    wrDword(L"queuePaused",     g_settings.queuePaused    ? 1 : 0);
    wrDword(L"schedEnabled",    g_settings.schedEnabled   ? 1 : 0);
    wrDword(L"schedStartMinutes", (DWORD)g_settings.schedStartMinutes);
    wrDword(L"schedStopMinutes",  (DWORD)g_settings.schedStopMinutes);
    wrDword(L"schedDaysMask",     (DWORD)g_settings.schedDaysMask);
    wrDword(L"schedShutdownDone", g_settings.schedShutdownDone ? 1 : 0);
    wrDword(L"launchOnStartup", g_settings.launchOnStartup ? 1 : 0);
    wrDword(L"soundOnComplete", g_settings.soundOnComplete ? 1 : 0);
    wrString(L"catDirVideo",    g_settings.catDirVideo);
    wrString(L"catDirMusic",    g_settings.catDirMusic);
    wrString(L"catDirArchive",  g_settings.catDirArchive);
    wrString(L"catDirProgram",  g_settings.catDirProgram);
    wrString(L"catDirDocument", g_settings.catDirDocument);
    wrDword(L"proxyMode", (DWORD)g_settings.proxyMode);
    wrString(L"proxyServer", g_settings.proxyServer);
    wrString(L"proxyBypass", g_settings.proxyBypass);
    wrString(L"proxyUser",   g_settings.proxyUser);
    wrString(L"proxyPass",   g_settings.proxyPass);
    {
        auto blob = serializeSiteLogins(g_settings.siteLogins);
        if (blob.empty()) RegDeleteValueW(hk, L"siteLogins");
        else RegSetValueExW(hk, L"siteLogins", 0, REG_BINARY,
                            blob.data(), (DWORD)blob.size());
    }
    wrDword(L"windowX", (DWORD)g_settings.windowX);
    wrDword(L"windowY", (DWORD)g_settings.windowY);
    wrDword(L"windowW", (DWORD)g_settings.windowW);
    wrDword(L"windowH", (DWORD)g_settings.windowH);
    wrDword(L"windowMaximized", g_settings.windowMaximized ? 1 : 0);
    RegCloseKey(hk);
}

// ---- data model ------------------------------------------------------

enum class ItemState { Queued, Running, Done, Err, Aborted, Paused };

struct GuiItem {
    int                            id = 0;
    std::wstring                   url;
    std::wstring                   filename;
    std::wstring                   outDir;
    std::wstring                   resultPath;
    std::wstring                   message;
    ItemState                      state = ItemState::Queued;
    int64_t                        total = -1;
    int64_t                        downloaded = 0;
    int64_t                        bps = 0;
    int                            activeConns = 0;
    int                            segments = 0;
    bool                           isVideo = false;
    bool                           isFtp   = false;
    bool                           isYtDlp = false;
    bool                           isYtPlaylist = false; // expand into per-video children
    std::wstring                   ytFormat;        // e.g. "best", "bestaudio"
    std::wstring                   cookiesPath;     // Netscape cookies.txt to feed yt-dlp
    HANDLE                         ytdlpProc = nullptr;
    bool                           ytdlpAbort = false;
    int64_t                        startedEpoch = 0;

    std::shared_ptr<Downloader>    dl;
    std::shared_ptr<VideoGrabber>  vg;
    std::shared_ptr<FtpDownloader> ftp;
    std::vector<std::pair<std::wstring,std::wstring>> headers;
    std::unique_ptr<std::thread>   thread;
};

std::mutex                                  g_itemsMu;
std::vector<std::unique_ptr<GuiItem>>       g_items;
std::atomic<int>                            g_nextId{1};

GuiItem* findItem(int id) {
    for (auto& p : g_items) if (p->id == id) return p.get();
    return nullptr;
}

bool looksLikeVideoUrl(const std::wstring& url) {
    return looksLikeHlsUrl(url) || looksLikeDashUrl(url);
}
bool looksLikeFtpUrl(const std::wstring& url) {
    return (url.size() > 6 && _wcsnicmp(url.c_str(), L"ftp://",  6) == 0)
        || (url.size() > 7 && _wcsnicmp(url.c_str(), L"ftps://", 7) == 0);
}
// YouTube watch / shorts / playlist / youtu.be — yt-dlp handles these
// reliably across cipher / SABR changes that our in-page extractor can't.
bool looksLikeYouTubeUrl(const std::wstring& url) {
    return url.find(L"youtube.com/watch")    != std::wstring::npos
        || url.find(L"youtube.com/shorts/")  != std::wstring::npos
        || url.find(L"youtube.com/playlist") != std::wstring::npos
        || url.find(L"music.youtube.com/")   != std::wstring::npos
        || url.find(L"youtu.be/")            != std::wstring::npos;
}

// `youtube.com/playlist?list=...` — multi-video container that should be
// flattened into individual watch downloads. Plain `watch?v=...&list=...`
// stays single-video (the user picked one specific video).
bool looksLikeYouTubePlaylistUrl(const std::wstring& url) {
    return url.find(L"youtube.com/playlist") != std::wstring::npos
        || url.find(L"music.youtube.com/playlist") != std::wstring::npos;
}

const wchar_t* stateLabel(ItemState s) {
    switch (s) {
        case ItemState::Queued:  return L"queued";
        case ItemState::Running: return L"running";
        case ItemState::Done:    return L"done";
        case ItemState::Err:     return L"err";
        case ItemState::Aborted: return L"aborted";
        case ItemState::Paused:  return L"queued";
    }
    return L"queued";
}

// ---- JSON encoding ---------------------------------------------------

std::wstring jsonEscape(const std::wstring& s) {
    std::wstring out; out.reserve(s.size() + 8);
    for (wchar_t c : s) {
        switch (c) {
            case L'"':  out += L"\\\""; break;
            case L'\\': out += L"\\\\"; break;
            case L'\n': out += L"\\n";  break;
            case L'\r': out += L"\\r";  break;
            case L'\t': out += L"\\t";  break;
            case L'\b': out += L"\\b";  break;
            case L'\f': out += L"\\f";  break;
            default:
                if (c < 0x20) {
                    wchar_t buf[8];
                    _snwprintf_s(buf, _TRUNCATE, L"\\u%04x", (unsigned)c);
                    out += buf;
                } else out.push_back(c);
        }
    }
    return out;
}

std::wstring quoteJson(const std::wstring& s) {
    std::wstring out = L"\"";
    out += jsonEscape(s);
    out += L"\"";
    return out;
}

std::wstring serializeItem(const GuiItem& it) {
    wchar_t buf[256];
    std::wstring out = L"{";
    _snwprintf_s(buf, _TRUNCATE, L"\"id\":%d,", it.id);                  out += buf;
    out += L"\"url\":";        out += quoteJson(it.url);        out += L",";
    out += L"\"filename\":";   out += quoteJson(it.filename);   out += L",";
    out += L"\"outDir\":";     out += quoteJson(it.outDir);     out += L",";
    out += L"\"resultPath\":"; out += quoteJson(it.resultPath); out += L",";
    out += L"\"message\":";    out += quoteJson(it.message);    out += L",";
    out += L"\"state\":";      out += quoteJson(stateLabel(it.state)); out += L",";
    _snwprintf_s(buf, _TRUNCATE, L"\"total\":%lld,",      (long long)it.total);      out += buf;
    _snwprintf_s(buf, _TRUNCATE, L"\"downloaded\":%lld,", (long long)it.downloaded); out += buf;
    _snwprintf_s(buf, _TRUNCATE, L"\"bps\":%lld,",        (long long)it.bps);        out += buf;
    _snwprintf_s(buf, _TRUNCATE, L"\"activeConns\":%d,",  it.activeConns); out += buf;
    _snwprintf_s(buf, _TRUNCATE, L"\"segments\":%d,",     it.segments);    out += buf;
    _snwprintf_s(buf, _TRUNCATE, L"\"startedEpoch\":%lld,",(long long)it.startedEpoch); out += buf;
    // Surface request headers so the UI's Properties dialog can show
    // Referer / User-Agent without a separate round-trip.
    auto findHeader = [&](const wchar_t* name) -> std::wstring {
        for (auto& kv : it.headers) {
            if (_wcsicmp(kv.first.c_str(), name) == 0) return kv.second;
        }
        return L"";
    };
    out += L"\"referer\":";   out += quoteJson(findHeader(L"Referer"));   out += L",";
    out += L"\"userAgent\":"; out += quoteJson(findHeader(L"User-Agent"));
    out += L"}";
    return out;
}

std::wstring serializeSettings() {
    wchar_t buf[128];
    std::wstring out = L"{";
    out += L"\"outDir\":"; out += quoteJson(g_settings.outputDir); out += L",";
    _snwprintf_s(buf, _TRUNCATE, L"\"connections\":%d,",  g_settings.connections);  out += buf;
    _snwprintf_s(buf, _TRUNCATE, L"\"maxJobs\":%d,",      g_settings.maxJobs);      out += buf;
    _snwprintf_s(buf, _TRUNCATE, L"\"bandwidthBps\":%lld,", (long long)g_settings.bandwidthBps); out += buf;
    out += L"\"clipboardWatch\":"; out += g_settings.clipboardWatch ? L"true" : L"false"; out += L",";
    out += L"\"verifyChecksum\":"; out += g_settings.verifyChecksum ? L"true" : L"false"; out += L",";
    out += L"\"categorize\":";     out += g_settings.categorize     ? L"true" : L"false"; out += L",";
    out += L"\"queuePaused\":";    out += g_settings.queuePaused    ? L"true" : L"false"; out += L",";
    out += L"\"schedEnabled\":";   out += g_settings.schedEnabled   ? L"true" : L"false"; out += L",";
    _snwprintf_s(buf, _TRUNCATE, L"\"schedStartMinutes\":%d,", g_settings.schedStartMinutes); out += buf;
    _snwprintf_s(buf, _TRUNCATE, L"\"schedStopMinutes\":%d,",  g_settings.schedStopMinutes);  out += buf;
    _snwprintf_s(buf, _TRUNCATE, L"\"schedDaysMask\":%d,",     g_settings.schedDaysMask);     out += buf;
    out += L"\"schedShutdownDone\":"; out += g_settings.schedShutdownDone ? L"true" : L"false"; out += L",";
    out += L"\"launchOnStartup\":";   out += g_settings.launchOnStartup   ? L"true" : L"false"; out += L",";
    out += L"\"soundOnComplete\":";   out += g_settings.soundOnComplete   ? L"true" : L"false"; out += L",";
    out += L"\"catDirVideo\":";       out += quoteJson(g_settings.catDirVideo);    out += L",";
    out += L"\"catDirMusic\":";       out += quoteJson(g_settings.catDirMusic);    out += L",";
    out += L"\"catDirArchive\":";     out += quoteJson(g_settings.catDirArchive);  out += L",";
    out += L"\"catDirProgram\":";     out += quoteJson(g_settings.catDirProgram);  out += L",";
    out += L"\"catDirDocument\":";    out += quoteJson(g_settings.catDirDocument);  out += L",";
    _snwprintf_s(buf, _TRUNCATE, L"\"proxyMode\":%d,", g_settings.proxyMode); out += buf;
    out += L"\"proxyServer\":";       out += quoteJson(g_settings.proxyServer);     out += L",";
    out += L"\"proxyBypass\":";       out += quoteJson(g_settings.proxyBypass);     out += L",";
    out += L"\"proxyUser\":";         out += quoteJson(g_settings.proxyUser);       out += L",";
    out += L"\"proxyPass\":";         out += quoteJson(g_settings.proxyPass);    out += L",";
    out += L"\"siteLogins\":[";
    for (size_t i = 0; i < g_settings.siteLogins.size(); ++i) {
        if (i) out += L",";
        out += L"{\"host\":";  out += quoteJson(g_settings.siteLogins[i].host);
        out += L",\"user\":";  out += quoteJson(g_settings.siteLogins[i].user);
        // Surface passwords to the UI so the user can edit a row inline.
        // The bridge is local IPC; same trust boundary as the Settings modal.
        out += L",\"pass\":";  out += quoteJson(g_settings.siteLogins[i].pass);
        out += L"}";
    }
    out += L"]";
    out += L"}";
    return out;
}

// ---- bridge: post events from any thread to the UI -------------------

void postEvent(std::wstring eventJson) {
    auto* heap = new std::wstring(std::move(eventJson));
    if (g_hwnd && PostMessageW(g_hwnd, WM_APP_BRIDGE_EVENT, 0, (LPARAM)heap))
        return;
    delete heap;
}

void executeOnUi(const std::wstring& eventJson) {
    std::wstring script = L"window.matata && window.matata.onEvent(" + eventJson + L")";
    if (g_webviewReady && g_webview) {
        g_webview->ExecuteScript(script.c_str(), nullptr);
    } else {
        g_pendingScripts.push_back(script);
    }
}

// Helpers for common event shapes
void emitItem(const GuiItem& it) {
    std::wstring ev = L"{\"type\":\"item\",\"item\":" + serializeItem(it) + L"}";
    postEvent(std::move(ev));
}

void emitItemRemoved(int id) {
    wchar_t buf[64];
    _snwprintf_s(buf, _TRUNCATE, L"{\"type\":\"remove\",\"id\":%d}", id);
    postEvent(buf);
}

void emitToast(const std::wstring& message, const wchar_t* kind = L"info") {
    std::wstring ev = L"{\"type\":\"toast\",\"kind\":\"";
    ev += kind;
    ev += L"\",\"message\":";
    ev += quoteJson(message);
    ev += L"}";
    postEvent(std::move(ev));
}

void emitItemsSnapshot() {
    std::wstring ev = L"{\"type\":\"items\",\"list\":[";
    bool first = true;
    {
        std::lock_guard<std::mutex> lk(g_itemsMu);
        for (auto& it : g_items) {
            if (!first) ev += L",";
            first = false;
            ev += serializeItem(*it);
        }
    }
    ev += L"]}";
    postEvent(std::move(ev));
}

void emitSettings() {
    std::wstring ev = L"{\"type\":\"settings\",\"settings\":" + serializeSettings() + L"}";
    postEvent(std::move(ev));
}

// ---- background update check ----------------------------------------

// Heap-allocated payload posted from the worker thread to the UI thread
// on WM_APP_UPDATE_AVAIL. The handler owns it and frees with delete.
struct UpdateAvailMsg {
    std::wstring latestVersion;
    std::wstring downloadUrl;
    std::wstring notes;
};

void updateCheckWorker() {
    UpdateInfo info;
    std::wstring err;
    if (!checkForUpdate(kUpdateManifestUrl, kMatataVersion, info, err)) {
        dbgLog(L"updateCheck: fetch failed (%ls)", err.c_str());
        return;
    }
    if (!info.available) {
        dbgLog(L"updateCheck: up to date (local=%ls remote=%ls)",
               kMatataVersion, info.latestVersion.c_str());
        return;
    }
    auto* msg = new UpdateAvailMsg{
        std::move(info.latestVersion),
        std::move(info.downloadUrl),
        std::move(info.notes),
    };
    if (!g_hwnd ||
        !PostMessageW(g_hwnd, WM_APP_UPDATE_AVAIL, 0, (LPARAM)msg)) {
        delete msg;
    }
}

void onUpdateAvailable(UpdateAvailMsg* m) {
    std::unique_ptr<UpdateAvailMsg> g(m);
    // Render an IDM-style "new version available" prompt. MessageBox is
    // intentionally minimal; the goal is parity with IDM's Update / Cancel
    // dialog without growing a custom-dialog footprint.
    std::wstring text = L"matata " + m->latestVersion + L" is available.\n\n";
    text += L"You're running matata " + std::wstring(kMatataVersion) + L".";
    if (!m->notes.empty()) {
        text += L"\n\nWhat's new:\n";
        text += m->notes;
    }
    text += L"\n\nUpdate now?";
    int r = MessageBoxW(g_hwnd, text.c_str(),
                        L"matata - Update available",
                        MB_YESNO | MB_ICONINFORMATION);
    if (r == IDYES && !m->downloadUrl.empty()) {
        ShellExecuteW(g_hwnd, L"open", m->downloadUrl.c_str(),
                      nullptr, nullptr, SW_SHOWNORMAL);
    }
}

// ---- scheduler ------------------------------------------------------

// Globals owned by the scheduler tick. Both flags are written by the worker
// thread and the message handler that toggles schedEnabled, so they're
// std::atomic to avoid undefined behaviour when both touch them.
std::atomic<bool> g_schedExitFlag{false};
std::atomic<bool> g_schedShutdownArmed{false};

// Forward decls: these helpers live in item-ops further down.
void dispatchQueued();
void abortItem(int id);
void restartItem(int id);
int  addDownloadEx(const HandoffSpec& spec);

// Returns minutes-since-midnight for the local clock now, plus the day of
// week in 0..6 with 0 = Sunday (matches schedDaysMask bit ordering).
void schedNow(int& minutes, int& dayOfWeek) {
    SYSTEMTIME st;
    GetLocalTime(&st);
    minutes   = st.wHour * 60 + st.wMinute;
    dayOfWeek = st.wDayOfWeek;     // already 0..6, Sun..Sat
}

// In-window check that handles wrap-around (e.g. 22:00 -> 07:00 spans
// midnight). For non-wrap windows, the half-open interval [start, stop)
// is in-window. For wrap windows, in-window is "now >= start OR now < stop".
// Equal start and stop is treated as never (zero-width window).
bool schedInWindow(int nowMin, int startMin, int stopMin) {
    if (startMin == stopMin) return false;
    if (startMin < stopMin)  return nowMin >= startMin && nowMin < stopMin;
    return nowMin >= startMin || nowMin < stopMin;
}

// Trigger Windows shutdown with a 60-second grace window so the user can
// abort with `shutdown /a`. We never run this if the user disabled the
// shutdown-when-done option mid-flight.
void schedTriggerShutdown() {
    dbgLog(L"scheduler: triggering OS shutdown (60s grace)");
    STARTUPINFOW si{}; si.cb = sizeof(si); si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};
    wchar_t cmd[] = L"shutdown.exe /s /t 60 /c \"matata: scheduled downloads complete\"";
    if (CreateProcessW(nullptr, cmd, nullptr, nullptr, FALSE,
                       CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    } else {
        dbgLog(L"scheduler: shutdown.exe launch failed err=%lu", GetLastError());
    }
}

// One scheduler tick. Detects the IN/OUT edges and applies them.
//   start edge: clear queuePaused, kick the queue
//   stop  edge: set   queuePaused, abort everything currently running
// Also handles the shutdown-when-done one-shot.
void schedTick(bool& wasIn) {
    if (!g_settings.schedEnabled) {
        wasIn = false;
        return;
    }
    int nowMin, dow;
    schedNow(nowMin, dow);
    bool dayOk = (g_settings.schedDaysMask >> dow) & 1;
    bool inNow = dayOk && schedInWindow(nowMin,
                                        g_settings.schedStartMinutes,
                                        g_settings.schedStopMinutes);
    if (inNow != wasIn) {
        if (inNow) {
            dbgLog(L"scheduler: START edge (now=%02d:%02d window=%d-%d dow=%d)",
                   nowMin/60, nowMin%60,
                   g_settings.schedStartMinutes, g_settings.schedStopMinutes, dow);
            // Un-pause the queue if it was paused. Don't clobber a manual
            // pause the user set after the last start: only flip if we
            // were the ones that set it on the previous Stop edge.
            if (g_settings.queuePaused) {
                g_settings.queuePaused = false;
                saveSettings();
                emitSettings();
            }
            dispatchQueued();
            emitToast(L"Scheduler: starting downloads", L"info");
            // Only arm shutdown-when-done if we actually started something --
            // an empty queue at the Start edge means the user has nothing
            // scheduled and we shouldn't shut their PC down for nothing.
            bool anyPending = false;
            {
                std::lock_guard<std::mutex> lk(g_itemsMu);
                for (auto& it : g_items) {
                    if (it->state == ItemState::Running ||
                        it->state == ItemState::Queued) {
                        anyPending = true; break;
                    }
                }
            }
            g_schedShutdownArmed.store(g_settings.schedShutdownDone && anyPending);
        } else {
            dbgLog(L"scheduler: STOP edge (now=%02d:%02d window=%d-%d dow=%d)",
                   nowMin/60, nowMin%60,
                   g_settings.schedStartMinutes, g_settings.schedStopMinutes, dow);
            if (!g_settings.queuePaused) {
                g_settings.queuePaused = true;
                saveSettings();
                emitSettings();
            }
            std::vector<int> ids;
            {
                std::lock_guard<std::mutex> lk(g_itemsMu);
                for (auto& it : g_items)
                    if (it->state == ItemState::Running) ids.push_back(it->id);
            }
            for (int id : ids) abortItem(id);
            emitToast(L"Scheduler: pausing downloads", L"info");
        }
        wasIn = inNow;
    }

    // Shutdown-when-done one-shot: only fires if armed (a download
    // completed during a scheduled window), the queue has fully drained,
    // and the user still has shutdownDone enabled.
    if (g_schedShutdownArmed.load() && g_settings.schedShutdownDone) {
        bool anyActive = false;
        {
            std::lock_guard<std::mutex> lk(g_itemsMu);
            for (auto& it : g_items) {
                if (it->state == ItemState::Running ||
                    it->state == ItemState::Queued) {
                    anyActive = true; break;
                }
            }
        }
        if (!anyActive) {
            g_schedShutdownArmed.store(false);
            schedTriggerShutdown();
        }
    }
}

void schedulerWorker() {
    bool wasIn = false;
    // First tick: align state with the current window so we don't fire a
    // spurious "Starting downloads" toast on a fresh launch that's already
    // inside the active window.
    {
        int nowMin, dow;
        schedNow(nowMin, dow);
        bool dayOk = g_settings.schedEnabled &&
                     ((g_settings.schedDaysMask >> dow) & 1);
        wasIn = dayOk && schedInWindow(nowMin,
                                       g_settings.schedStartMinutes,
                                       g_settings.schedStopMinutes);
    }
    while (!g_schedExitFlag.load()) {
        schedTick(wasIn);
        // Sleep in 1s slices so app shutdown isn't held up by the tick rate.
        for (int i = 0; i < 30 && !g_schedExitFlag.load(); ++i)
            std::this_thread::sleep_for(std::chrono::seconds(1));
    }
}

// ---- v0.9.7 General-tab helpers ------------------------------------

// HKCU\Software\Microsoft\Windows\CurrentVersion\Run\matata-gui:
//   on  -> set REG_SZ to "<exe path>"
//   off -> delete the value
void applyLaunchOnStartup(bool enable) {
    const wchar_t* subkey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
    HKEY hk;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, subkey, 0,
                      KEY_SET_VALUE | KEY_QUERY_VALUE, &hk) != ERROR_SUCCESS) {
        dbgLog(L"applyLaunchOnStartup: open Run key failed err=%lu", GetLastError());
        return;
    }
    if (enable) {
        wchar_t path[MAX_PATH];
        DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
        if (n > 0 && n < MAX_PATH) {
            std::wstring quoted = L"\"";
            quoted += path;
            quoted += L"\"";
            RegSetValueExW(hk, L"matata-gui", 0, REG_SZ,
                           (const BYTE*)quoted.c_str(),
                           (DWORD)((quoted.size() + 1) * sizeof(wchar_t)));
        }
    } else {
        RegDeleteValueW(hk, L"matata-gui");
    }
    RegCloseKey(hk);
}

// Map matata's Category::name -> the per-category folder setting. Empty
// string means "fall through to outputDir" -- the caller handles that.
const std::wstring& categoryFolder(const std::wstring& categoryName) {
    static const std::wstring empty;
    if      (categoryName == L"video")    return g_settings.catDirVideo;
    else if (categoryName == L"music")    return g_settings.catDirMusic;
    else if (categoryName == L"archive")  return g_settings.catDirArchive;
    else if (categoryName == L"program")  return g_settings.catDirProgram;
    else if (categoryName == L"document") return g_settings.catDirDocument;
    return empty;
}

// Heuristic: should clipboard text trigger an auto-add?
//   - http/https/ftp/ftps URL
//   - either ends in a known download extension, or is a YouTube/Vimeo URL
//     (the rest of the pipeline handles the video extraction)
bool clipboardLooksLikeDownload(const std::wstring& s) {
    if (s.size() < 8 || s.size() > 4096) return false;
    std::wstring low;
    low.reserve(s.size());
    for (wchar_t c : s) low.push_back((wchar_t)towlower(c));
    if (low.compare(0, 7, L"http://")  != 0 &&
        low.compare(0, 8, L"https://") != 0 &&
        low.compare(0, 6, L"ftp://")   != 0 &&
        low.compare(0, 7, L"ftps://")  != 0) {
        return false;
    }
    if (low.find(L'\n') != std::wstring::npos ||
        low.find(L'\r') != std::wstring::npos ||
        low.find(L' ')  != std::wstring::npos) return false;
    if (looksLikeYouTubeUrl(s) || looksLikeVideoUrl(s)) return true;
    auto qm = low.find(L'?');
    std::wstring path = (qm == std::wstring::npos) ? low : low.substr(0, qm);
    auto dot = path.find_last_of(L'.');
    if (dot == std::wstring::npos) return false;
    std::wstring ext = path.substr(dot + 1);
    return categorize(L"x." + ext)->name != L"other";
}

std::wstring g_lastClipboardUrl;
bool         g_clipboardSubscribed = false;

void readClipboardAndMaybeAdd() {
    if (!OpenClipboard(g_hwnd)) return;
    HANDLE h = GetClipboardData(CF_UNICODETEXT);
    std::wstring text;
    if (h) {
        const wchar_t* p = (const wchar_t*)GlobalLock(h);
        if (p) {
            text = p;
            GlobalUnlock(h);
        }
    }
    CloseClipboard();
    while (!text.empty() && (text.back() == L'\r' || text.back() == L'\n' ||
                              text.back() == L' ' || text.back() == L'\t'))
        text.pop_back();
    while (!text.empty() && (text.front() == L'\r' || text.front() == L'\n' ||
                              text.front() == L' ' || text.front() == L'\t'))
        text.erase(text.begin());
    if (text.empty() || text == g_lastClipboardUrl) return;
    if (!clipboardLooksLikeDownload(text)) return;
    g_lastClipboardUrl = text;
    HandoffSpec spec;
    spec.url = text;
    addDownloadEx(spec);
    emitToast(L"Auto-added from clipboard", L"info");
}

// Build the yt-dlp `--proxy <url>` argument from current settings, or "" if
// no manual proxy is configured. Mode 2 = http, mode 3 = socks5. WinHTTP
// only honours mode 2; mode 3 still works here because yt-dlp speaks SOCKS5
// natively. Caller must already have a leading space if needed.
std::wstring ytDlpProxyArg() {
    if (g_settings.proxyMode != 2 && g_settings.proxyMode != 3) return L"";
    if (g_settings.proxyServer.empty()) return L"";
    const wchar_t* scheme = (g_settings.proxyMode == 3) ? L"socks5://" : L"http://";
    std::wstring auth;
    if (!g_settings.proxyUser.empty() || !g_settings.proxyPass.empty()) {
        auth = g_settings.proxyUser + L":" + g_settings.proxyPass + L"@";
    }
    std::wstring url = scheme + auth + g_settings.proxyServer;
    return L" --proxy \"" + url + L"\"";
}

void applyClipboardWatch(bool enable) {
    if (enable && !g_clipboardSubscribed) {
        if (g_hwnd && AddClipboardFormatListener(g_hwnd)) {
            g_clipboardSubscribed = true;
            // Seed last-seen so we don't immediately re-add an existing URL.
            if (OpenClipboard(g_hwnd)) {
                HANDLE h = GetClipboardData(CF_UNICODETEXT);
                if (h) {
                    const wchar_t* p = (const wchar_t*)GlobalLock(h);
                    if (p) g_lastClipboardUrl = p;
                    if (h)  GlobalUnlock(h);
                }
                CloseClipboard();
            }
        }
    } else if (!enable && g_clipboardSubscribed) {
        if (g_hwnd) RemoveClipboardFormatListener(g_hwnd);
        g_clipboardSubscribed = false;
        g_lastClipboardUrl.clear();
    }
}

// ---- v0.9.9 system tray --------------------------------------------

constexpr UINT kTrayIconId = 1;
NOTIFYICONDATAW g_trayIcon{};
bool            g_trayInstalled = false;

void installTrayIcon() {
    if (g_trayInstalled || !g_hwnd) return;
    g_trayIcon.cbSize = sizeof(g_trayIcon);
    g_trayIcon.hWnd   = g_hwnd;
    g_trayIcon.uID    = kTrayIconId;
    g_trayIcon.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_trayIcon.uCallbackMessage = WM_APP_TRAYICON;
    g_trayIcon.hIcon  = (HICON)LoadImageW(nullptr, IDI_APPLICATION,
                                          IMAGE_ICON, 16, 16, LR_SHARED);
    wcscpy_s(g_trayIcon.szTip, _countof(g_trayIcon.szTip), L"matata");
    if (Shell_NotifyIconW(NIM_ADD, &g_trayIcon)) g_trayInstalled = true;
}

void removeTrayIcon() {
    if (!g_trayInstalled) return;
    Shell_NotifyIconW(NIM_DELETE, &g_trayIcon);
    g_trayInstalled = false;
}

void showFromTray() {
    if (!g_hwnd) return;
    ShowWindow(g_hwnd, SW_SHOW);
    if (IsIconic(g_hwnd)) ShowWindow(g_hwnd, SW_RESTORE);
    SetForegroundWindow(g_hwnd);
}

void showTrayMenu() {
    POINT pt; GetCursorPos(&pt);
    HMENU hm = CreatePopupMenu();
    AppendMenuW(hm, MF_STRING, 1, L"Show matata");
    AppendMenuW(hm, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(hm, MF_STRING, 2, L"Pause all");
    AppendMenuW(hm, MF_STRING, 3, L"Resume all");
    AppendMenuW(hm, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(hm, MF_STRING, 4, L"Exit");
    // Required so the menu auto-dismisses when the user clicks elsewhere.
    SetForegroundWindow(g_hwnd);
    int cmd = TrackPopupMenu(hm, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY,
                             pt.x, pt.y, 0, g_hwnd, nullptr);
    DestroyMenu(hm);
    switch (cmd) {
        case 1: showFromTray(); break;
        case 2: {
            std::vector<int> ids;
            {
                std::lock_guard<std::mutex> lk(g_itemsMu);
                for (auto& it : g_items)
                    if (it->state == ItemState::Running) ids.push_back(it->id);
            }
            for (int id : ids) abortItem(id);
            break;
        }
        case 3: {
            std::vector<int> ids;
            {
                std::lock_guard<std::mutex> lk(g_itemsMu);
                for (auto& it : g_items)
                    if (it->state == ItemState::Aborted ||
                        it->state == ItemState::Err) ids.push_back(it->id);
            }
            for (int id : ids) restartItem(id);
            break;
        }
        case 4: DestroyWindow(g_hwnd); break;
    }
}

// ---- worker message types (used by both regular + yt-dlp workers) ---

struct ProgressMsg {
    int     id;
    int64_t total;
    int64_t downloaded;
    int64_t bps;
    int     activeConns;
    int     segments;
};
struct CompleteMsg {
    int             id;
    DownloadStatus  status;
    std::wstring    message;
    std::wstring    path;
};

// ---- yt-dlp worker --------------------------------------------------

std::wstring quoteArg(const std::wstring& s) {
    // Quote on whitespace/quotes (for CreateProcess argv parsing) AND on
    // cmd.exe metacharacters (& | ^ < > ( ) %), because we now wrap yt-dlp
    // via `cmd.exe /S /C "..."` so those would otherwise be interpreted.
    if (s.find_first_of(L" \t\"&|^<>()%") == std::wstring::npos) return s;
    std::wstring out = L"\"";
    for (size_t i = 0; i < s.size(); ++i) {
        int slashes = 0;
        while (i < s.size() && s[i] == L'\\') { ++slashes; ++i; }
        if (i == s.size()) { out.append(slashes * 2, L'\\'); break; }
        if (s[i] == L'"') { out.append(slashes * 2 + 1, L'\\'); out.push_back(L'"'); }
        else              { out.append(slashes, L'\\'); out.push_back(s[i]); }
    }
    out.push_back(L'"');
    return out;
}

std::wstring exeDirEarly() {
    wchar_t buf[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (n == 0) return L"";
    std::wstring p(buf, n);
    auto slash = p.find_last_of(L"\\/");
    return (slash == std::wstring::npos) ? L"" : p.substr(0, slash);
}

std::wstring defaultDownloadsDir() {
    PWSTR path = nullptr;
    if (SHGetKnownFolderPath(FOLDERID_Downloads, 0, nullptr, &path) != S_OK || !path) return L"";
    std::wstring s(path);
    CoTaskMemFree(path);
    return s;
}

int64_t parseSizeWithUnit(const std::wstring& num, const std::wstring& unit) {
    double v = _wtof(num.c_str());
    if (unit.find(L"GiB") != std::wstring::npos || unit.find(L"GB") != std::wstring::npos) v *= 1024.0 * 1024.0 * 1024.0;
    else if (unit.find(L"MiB") != std::wstring::npos || unit.find(L"MB") != std::wstring::npos) v *= 1024.0 * 1024.0;
    else if (unit.find(L"KiB") != std::wstring::npos || unit.find(L"KB") != std::wstring::npos) v *= 1024.0;
    return (int64_t)v;
}

// yt-dlp emits lines like:
//   [download]   3.4% of   12.34MiB at  1.23MiB/s ETA 00:30
//   [download] Destination: C:\Users\...\foo.mp4
//   [Merger]   Merging formats into "foo.mkv"
void parseYtDlpProgress(int id, const std::wstring& line, std::wstring& outputPath) {
    if (line.compare(0, 10, L"[download]") == 0) {
        size_t pct = line.find(L'%');
        if (pct != std::wstring::npos) {
            size_t s = pct;
            while (s > 0 && (iswdigit(line[s-1]) || line[s-1] == L'.')) --s;
            double percent = _wtof(line.substr(s, pct - s).c_str());

            int64_t total = 0, bps = 0;
            size_t ofPos = line.find(L"of", pct);
            if (ofPos != std::wstring::npos) {
                size_t i = ofPos + 2;
                while (i < line.size() && (line[i] == L' ' || line[i] == L'~')) ++i;
                size_t numStart = i;
                while (i < line.size() && (iswdigit(line[i]) || line[i] == L'.')) ++i;
                std::wstring num = line.substr(numStart, i - numStart);
                size_t unitEnd = i;
                while (unitEnd < line.size() && line[unitEnd] != L' ') ++unitEnd;
                std::wstring unit = line.substr(i, unitEnd - i);
                total = parseSizeWithUnit(num, unit);
            }
            size_t atPos = line.find(L"at ", pct);
            if (atPos != std::wstring::npos) {
                size_t i = atPos + 3;
                while (i < line.size() && line[i] == L' ') ++i;
                size_t numStart = i;
                while (i < line.size() && (iswdigit(line[i]) || line[i] == L'.')) ++i;
                std::wstring num = line.substr(numStart, i - numStart);
                size_t unitEnd = i;
                while (unitEnd < line.size() && line[unitEnd] != L'/') ++unitEnd;
                std::wstring unit = line.substr(i, unitEnd - i);
                bps = parseSizeWithUnit(num, unit);
            }
            int64_t downloaded = (total > 0) ? (int64_t)(total * percent / 100.0) : 0;
            auto* msg = new ProgressMsg{ id, total, downloaded, bps, 1, 0 };
            if (!PostMessageW(g_hwnd, WM_APP_PROGRESS, 0, (LPARAM)msg)) delete msg;
        }
        size_t dest = line.find(L"Destination: ");
        if (dest != std::wstring::npos) outputPath = line.substr(dest + 13);
    }
    size_t merge = line.find(L"Merging formats into \"");
    if (merge != std::wstring::npos) {
        size_t a = merge + 22;
        size_t b = line.find(L'"', a);
        if (b != std::wstring::npos) outputPath = line.substr(a, b - a);
    }
}

// Forward decl — addDownloadEx is defined further down but used here.
int addDownloadEx(const HandoffSpec& spec);

// Flatten a YouTube playlist URL into individual watch URLs and queue
// each as its own download. The current item (the playlist row) is
// marked Done with a summary message once enumeration finishes.
void workerYtDlpPlaylist(int id) {
    dbgLog(L"workerYtDlpPlaylist ENTER id=%d", id);
    HandoffSpec parent;
    {
        std::lock_guard<std::mutex> lk(g_itemsMu);
        GuiItem* it = findItem(id);
        if (!it) return;
        parent.url      = it->url;
        parent.outDir   = it->outDir;
        parent.ytFormat = it->ytFormat;
        for (auto& kv : it->headers) {
            if      (_wcsicmp(kv.first.c_str(), L"Referer") == 0)    parent.referer   = kv.second;
            else if (_wcsicmp(kv.first.c_str(), L"Cookie") == 0)     parent.cookie    = kv.second;
            else if (_wcsicmp(kv.first.c_str(), L"User-Agent") == 0) parent.userAgent = kv.second;
        }
    }

    // Locate yt-dlp / Python the same way workerYtDlp does.
    auto findSystemPython = []() -> std::wstring {
        const wchar_t* candidates[] = {
            L"C:\\Python314\\python.exe", L"C:\\Python313\\python.exe",
            L"C:\\Python312\\python.exe", L"C:\\Python311\\python.exe",
            L"C:\\Python310\\python.exe",
        };
        for (const wchar_t* p : candidates) {
            if (GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES) return p;
        }
        return L"";
    };
    std::wstring sysPython = findSystemPython();
    std::wstring exe;
    bool useSystemPython = !sysPython.empty();
    if (useSystemPython) {
        exe = sysPython;
    } else {
        exe = exeDirEarly() + L"\\yt-dlp.exe";
        if (GetFileAttributesW(exe.c_str()) == INVALID_FILE_ATTRIBUTES) {
            auto* done = new CompleteMsg{ id, DownloadStatus::NetworkError,
                L"yt-dlp.exe not found", L"" };
            if (!PostMessageW(g_hwnd, WM_APP_COMPLETED, 0, (LPARAM)done)) delete done;
            return;
        }
    }

    std::wstring cmd = quoteArg(exe);
    if (useSystemPython) cmd += L" -m yt_dlp";
    // yt-dlp's --print template engine does NOT interpret backslash escapes
    // (\t / \n) — they pass through literally. Embed a real TAB character as
    // the field separator so each emitted line has the form
    //   "<playlist_title>\t<id>\t<title>".
    cmd += L" --flat-playlist --print \"%(playlist_title|)s\t%(id)s\t%(title)s\" ";
    cmd += ytDlpProxyArg();
    cmd += L" ";
    cmd += quoteArg(parent.url);

    SECURITY_ATTRIBUTES sa{}; sa.nLength = sizeof(sa); sa.bInheritHandle = TRUE;
    HANDLE hRead = nullptr, hWrite = nullptr;
    if (!CreatePipe(&hRead, &hWrite, &sa, 0)) {
        auto* done = new CompleteMsg{ id, DownloadStatus::NetworkError, L"CreatePipe failed", L"" };
        if (!PostMessageW(g_hwnd, WM_APP_COMPLETED, 0, (LPARAM)done)) delete done;
        return;
    }
    SetHandleInformation(hRead, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si{}; si.cb = sizeof(si);
    si.dwFlags     = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput  = hWrite;
    si.hStdError   = hWrite;
    si.hStdInput   = INVALID_HANDLE_VALUE;
    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> cmdBuf(cmd.begin(), cmd.end());
    cmdBuf.push_back(L'\0');
    BOOL ok = CreateProcessW(nullptr, cmdBuf.data(), nullptr, nullptr, TRUE,
                             CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(hWrite);
    if (!ok) {
        CloseHandle(hRead);
        auto* done = new CompleteMsg{ id, DownloadStatus::NetworkError,
            L"CreateProcess failed", L"" };
        if (!PostMessageW(g_hwnd, WM_APP_COMPLETED, 0, (LPARAM)done)) delete done;
        return;
    }

    // Read stdout — each line is "<playlist_title>\t<id>\t<title>". Queue
    // each as a watch URL.
    auto sanitizeForFilename = [](std::wstring& s) {
        for (wchar_t& c : s) {
            if (c == L'\\' || c == L'/' || c == L':' || c == L'*' ||
                c == L'?'  || c == L'"' || c == L'<' || c == L'>' || c == L'|')
                c = L'_';
        }
        while (!s.empty() && (s.back() == L' ' || s.back() == L'.')) s.pop_back();
        if (s.size() > 120) s.resize(120);
    };
    std::string raw;
    char chunk[4096];
    DWORD got = 0;
    int queued = 0;
    std::wstring playlistTitle;
    while (ReadFile(hRead, chunk, sizeof(chunk), &got, nullptr) && got > 0) {
        raw.append(chunk, got);
        for (;;) {
            auto nl = raw.find_first_of("\r\n");
            if (nl == std::string::npos) break;
            std::string line = raw.substr(0, nl);
            raw.erase(0, nl + 1);
            if (line.empty()) continue;
            int n = MultiByteToWideChar(CP_UTF8, 0, line.data(), (int)line.size(), nullptr, 0);
            std::wstring wline(n, L'\0');
            MultiByteToWideChar(CP_UTF8, 0, line.data(), (int)line.size(), wline.data(), n);
            // Split on the two tab separators.
            auto tab1 = wline.find(L'\t');
            if (tab1 == std::wstring::npos) continue;
            auto tab2 = wline.find(L'\t', tab1 + 1);
            std::wstring plTitle = wline.substr(0, tab1);
            std::wstring vid     = (tab2 == std::wstring::npos)
                                   ? wline.substr(tab1 + 1)
                                   : wline.substr(tab1 + 1, tab2 - tab1 - 1);
            std::wstring title   = (tab2 == std::wstring::npos)
                                   ? L""
                                   : wline.substr(tab2 + 1);
            if (vid.empty() || vid == L"NA") continue;
            if (playlistTitle.empty() && !plTitle.empty()) playlistTitle = plTitle;
            // Sanitize title for use as a filename — strip Windows-illegal
            // chars and clamp length so disk write succeeds.
            sanitizeForFilename(title);
            HandoffSpec child = parent;
            child.url      = L"https://www.youtube.com/watch?v=" + vid;
            child.filename = title.empty() ? L"" : title + L".mp4";
            addDownloadEx(child);
            ++queued;
        }
    }
    // Stamp the parent (playlist) row with the playlist title so the UI shows
    // something meaningful instead of the URL leaf "playlist".
    if (!playlistTitle.empty()) {
        sanitizeForFilename(playlistTitle);
        GuiItem snap{};
        bool have = false;
        {
            std::lock_guard<std::mutex> lk(g_itemsMu);
            GuiItem* it = findItem(id);
            if (it) {
                it->filename = playlistTitle;
                snap.id           = it->id;
                snap.url          = it->url;
                snap.filename     = it->filename;
                snap.outDir       = it->outDir;
                snap.resultPath   = it->resultPath;
                snap.message      = it->message;
                snap.state        = it->state;
                snap.total        = it->total;
                snap.downloaded   = it->downloaded;
                snap.bps          = it->bps;
                snap.activeConns  = it->activeConns;
                snap.segments     = it->segments;
                snap.startedEpoch = it->startedEpoch;
                snap.headers      = it->headers;
                have = true;
            }
        }
        if (have) emitItem(snap);
    }
    CloseHandle(hRead);
    DWORD rc = 1;
    WaitForSingleObject(pi.hProcess, INFINITE);
    GetExitCodeProcess(pi.hProcess, &rc);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    dbgLog(L"workerYtDlpPlaylist EXIT id=%d rc=%lu queued=%d", id, rc, queued);

    DownloadStatus status = (rc == 0 && queued > 0)
        ? DownloadStatus::Ok : DownloadStatus::NetworkError;
    std::wstring msg = (queued > 0)
        ? (L"Queued " + std::to_wstring(queued) + L" videos from playlist")
        : L"Could not enumerate playlist";
    auto* done = new CompleteMsg{ id, status, msg, L"" };
    if (!PostMessageW(g_hwnd, WM_APP_COMPLETED, 0, (LPARAM)done)) delete done;
}

void workerYtDlp(int id) {
    dbgLog(L"workerYtDlp ENTER id=%d", id);
    std::wstring url, outDir, fmt, cookiesPath;
    {
        std::lock_guard<std::mutex> lk(g_itemsMu);
        GuiItem* it = findItem(id);
        if (!it) return;
        url         = it->url;
        outDir      = it->outDir.empty() ? defaultDownloadsDir() : it->outDir;
        fmt         = it->ytFormat.empty() ? L"bv*+ba/best" : it->ytFormat;
        cookiesPath = it->cookiesPath;
    }

    // Prefer pip-installed yt-dlp via the system Python over the bundled
    // PyInstaller win-exe. The win-exe's frozen Python doesn't see PATH
    // changes our cmd.exe wrapper makes, so it always reports
    // `JS runtimes: none` and can't solve YouTube's signature/n-challenge.
    // System Python doesn't have that blind spot.
    auto findSystemPython = []() -> std::wstring {
        const wchar_t* candidates[] = {
            L"C:\\Python314\\python.exe",
            L"C:\\Python313\\python.exe",
            L"C:\\Python312\\python.exe",
            L"C:\\Python311\\python.exe",
            L"C:\\Python310\\python.exe",
        };
        for (const wchar_t* p : candidates) {
            if (GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES) return p;
        }
        // Fall back to whatever `py.exe` (the launcher) resolves.
        wchar_t windir[MAX_PATH] = {};
        GetWindowsDirectoryW(windir, MAX_PATH);
        std::wstring py = std::wstring(windir) + L"\\py.exe";
        if (GetFileAttributesW(py.c_str()) != INVALID_FILE_ATTRIBUTES) return py;
        return L"";
    };
    std::wstring sysPython = findSystemPython();
    bool useSystemPython = !sysPython.empty();
    std::wstring ytdlp;
    if (useSystemPython) {
        // Verify pip-installed yt_dlp is importable. If not, fall back to
        // the bundled exe.
        std::wstring probeCmd = quoteArg(sysPython) +
            L" -c \"import yt_dlp\"";
        STARTUPINFOW psi{}; psi.cb = sizeof(psi);
        psi.dwFlags = STARTF_USESHOWWINDOW; psi.wShowWindow = SW_HIDE;
        PROCESS_INFORMATION ppi{};
        std::vector<wchar_t> probeBuf(probeCmd.begin(), probeCmd.end());
        probeBuf.push_back(L'\0');
        if (CreateProcessW(nullptr, probeBuf.data(), nullptr, nullptr, FALSE,
                           CREATE_NO_WINDOW, nullptr, nullptr, &psi, &ppi)) {
            WaitForSingleObject(ppi.hProcess, 5000);
            DWORD ec = 0; GetExitCodeProcess(ppi.hProcess, &ec);
            CloseHandle(ppi.hProcess); CloseHandle(ppi.hThread);
            if (ec != 0) useSystemPython = false;
        } else {
            useSystemPython = false;
        }
    }
    if (useSystemPython) {
        ytdlp = sysPython;
        dbgLog(L"workerYtDlp using system Python: %ls", sysPython.c_str());
    } else {
        ytdlp = exeDirEarly() + L"\\yt-dlp.exe";
        if (GetFileAttributesW(ytdlp.c_str()) == INVALID_FILE_ATTRIBUTES) {
            auto* done = new CompleteMsg{ id, DownloadStatus::NetworkError,
                L"yt-dlp.exe not found next to matata-gui.exe", L"" };
            if (!PostMessageW(g_hwnd, WM_APP_COMPLETED, 0, (LPARAM)done)) delete done;
            return;
        }
        dbgLog(L"workerYtDlp using bundled yt-dlp.exe (system Python unavailable)");
    }

    std::wstring tpl = outDir;
    if (!tpl.empty() && tpl.back() != L'\\' && tpl.back() != L'/') tpl.push_back(L'\\');
    tpl += L"%(title)s.%(ext)s";

    std::wstring cmd = quoteArg(ytdlp);
    if (useSystemPython) cmd += L" -m yt_dlp";
    // --verbose + leaving warnings on so we can see why yt-dlp filters out
    // every format (PO token, format-id mismatch, etc.). Drop both once the
    // YouTube path is reliable again.
    cmd += L" --newline --verbose --no-playlist --no-mtime";
    cmd += L" -f " + quoteArg(fmt);
    cmd += L" --merge-output-format mp4";
    // Prefer page-fresh cookies the extension shipped via a temp Netscape
    // cookies.txt file (no 32KB CreateProcess limit, and includes the live
    // SAPISIDHASH / VISITOR_INFO_LIVE that the disk-stored Firefox profile
    // sometimes lacks — yt-dlp otherwise gets bumped to a "tv downgraded"
    // player_client that has no high-res formats). Fall back to extracting
    // from Firefox if no file was supplied.
    // Intentionally NOT passing --cookies / --cookies-from-browser to yt-dlp
    // for YouTube. Verified: when cookies are supplied yt-dlp logs
    // `Skipping client "ios" since it does not support cookies` and falls
    // back to web/mweb clients that require JS-solved signature + n-challenge
    // (which fails with `JS runtimes: none` on this build). Without cookies
    // yt-dlp uses the ios client, which returns plain DASH formats up to 720p
    // AV1 + m4a audio that download cleanly. We still write the temp cookies
    // file (used by future age-gated / region-locked paths) and leave the
    // cleanup at the end of this worker.
    bool haveCookieFile = !cookiesPath.empty() &&
        GetFileAttributesW(cookiesPath.c_str()) != INVALID_FILE_ATTRIBUTES;
    // Coax yt-dlp into returning the full adaptive format set:
    //   - `formats=missing_pot` keeps formats that look like they need a PO
    //     token instead of silently filtering them out (with our cookies they
    //     usually work, and the ones that don't 403 are skipped at retrieval).
    //   - the `default` client is the same `web` yt-dlp uses by default, but
    //     listing it explicitly stops yt-dlp from downgrading to the limited
    //     `tv` client just because no PO token was supplied.
    //   - `web_embedded`, `mweb`, `ios`, `android_creator` are added as
    //     fallbacks; yt-dlp merges formats from every client that responds.
    cmd += L" --extractor-args \"youtube:player_client=default,web_embedded,mweb,ios,android_creator;formats=missing_pot\"";
    cmd += ytDlpProxyArg();
    // Prefer DASH over HLS. yt-dlp's default format ranker would otherwise
    // pick an HLS premium format (e.g. itag 616) that's served behind
    // SABR/signature gates; without a working JS solver every fragment 403s
    // and the row gets stuck in RUNNING with no progress. The `ios` /
    // `android` clients return plain DASH (https) for the same heights, so
    // ranking by protocol pushes those to the top.
    cmd += L" --format-sort proto";
    // Tell yt-dlp where ffmpeg is so it can mux video+audio into a clean
    // .mp4 (without ffmpeg yt-dlp falls back to .webm for opus audio).
    std::wstring ffmpeg = exeDirEarly() + L"\\ffmpeg.exe";
    if (GetFileAttributesW(ffmpeg.c_str()) != INVALID_FILE_ATTRIBUTES) {
        cmd += L" --ffmpeg-location " + quoteArg(ffmpeg);
    }
    cmd += L" -o " + quoteArg(tpl);
    cmd += L" " + quoteArg(url);

    // Wrap yt-dlp inside `cmd.exe /S /C "set PATH=...&& yt-dlp ..."`. The
    // PyInstaller bundle that ships yt-dlp.exe ignores PATH changes made via
    // SetEnvironmentVariableW in the parent (we still log that we patched it
    // — verbose yt-dlp output keeps reporting `JS runtimes: none`). Setting
    // PATH inside cmd.exe's own env block is the one place we know yt-dlp's
    // signature/n-challenge solver actually picks up node.exe.
    {
        wchar_t sysroot[MAX_PATH] = {};
        GetSystemDirectoryW(sysroot, MAX_PATH);
        std::wstring cmdExe = std::wstring(sysroot) + L"\\cmd.exe";
        std::wstring extraPath;
        const wchar_t* pathCands[] = {
            L"C:\\Program Files\\nodejs",
            L"C:\\Program Files (x86)\\nodejs",
            L"C:\\Program Files\\Deno",
            L"C:\\Program Files\\Bun",
        };
        for (const wchar_t* p : pathCands) {
            if (GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES) {
                if (!extraPath.empty()) extraPath += L";";
                extraPath += p;
            }
        }
        // cmd.exe's `set` doesn't tolerate `^` `&` `|` etc. without quoting
        // — wrap the value in quotes. Inner quotes inside our yt-dlp args
        // (e.g. --extractor-args "youtube:...") need to be escaped from
        // cmd.exe — but we already quote-wrap the whole cmd string with
        // /S /C "..." which lets cmd treat everything between the outer
        // quotes verbatim. So no further escaping is needed.
        // PYTHONIOENCODING + PYTHONUTF8 force yt-dlp's stdout to UTF-8 so we
        // can correctly parse paths that contain non-cp1252 characters
        // (YouTube replaces `|` in titles with full-width U+FF5C, which
        // otherwise comes through our pipe as spaces and breaks the final
        // file-size lookup).
        // The `where node` line helps diagnose PATH problems: its output
        // lands in our captured stdout pipe and is logged as a yt-dlp[N]
        // line, so we can see whether cmd actually picks up node.exe.
        std::wstring inner =
            L"set \"PATH=" + extraPath + L";%PATH%\" && "
            L"set \"PYTHONIOENCODING=utf-8\" && "
            L"set \"PYTHONUTF8=1\" && "
            L"where node 2>&1 && "
            + cmd;
        cmd = quoteArg(cmdExe) + L" /S /C \"" + inner + L"\"";
    }

    dbgLog(L"workerYtDlp cmd=%ls", cmd.c_str());

    // yt-dlp needs a JavaScript runtime (node / deno / bun / quickjs) to
    // solve YouTube's signature + n-challenge. When matata-gui is launched
    // from a browser → matata-host chain, the inherited PATH usually doesn't
    // include `C:\Program Files\nodejs`, so yt-dlp logs `JS runtimes: none`
    // and ends up with "Only images are available for download".
    // Build a child env that prepends candidate runtime directories to PATH.
    auto findRuntimeDir = []() -> std::wstring {
        // Try common install paths and HKLM registry hints. First match wins.
        const wchar_t* candidates[] = {
            L"C:\\Program Files\\nodejs\\node.exe",
            L"C:\\Program Files (x86)\\nodejs\\node.exe",
            L"C:\\Program Files\\Deno\\deno.exe",
            L"C:\\Program Files\\Bun\\bun.exe",
        };
        for (const wchar_t* p : candidates) {
            if (GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES) {
                std::wstring s = p;
                auto slash = s.find_last_of(L"\\/");
                return (slash == std::wstring::npos) ? L"" : s.substr(0, slash);
            }
        }
        // HKLM\SOFTWARE\Node.js\InstallPath (set by the Node MSI).
        HKEY hk;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Node.js", 0, KEY_READ, &hk) == ERROR_SUCCESS) {
            wchar_t buf[MAX_PATH] = {};
            DWORD cb = sizeof(buf);
            DWORD type = 0;
            if (RegQueryValueExW(hk, L"InstallPath", nullptr, &type, (LPBYTE)buf, &cb) == ERROR_SUCCESS &&
                (type == REG_SZ || type == REG_EXPAND_SZ)) {
                RegCloseKey(hk);
                return std::wstring(buf);
            }
            RegCloseKey(hk);
        }
        return L"";
    };
    std::wstring runtimeDir = findRuntimeDir();

    // Belt-and-suspenders: also drop a copy of node.exe (or whichever runtime
    // was found) next to yt-dlp.exe. yt-dlp's PyInstaller bundle searches its
    // own dir before PATH on Windows, so this is the most reliable way to
    // make sure the JS challenge solver finds a runtime regardless of how
    // PATH was inherited. Idempotent — the first run copies, future runs no-op.
    if (!runtimeDir.empty()) {
        std::wstring exeDir = exeDirEarly();
        struct { const wchar_t* exe; } names[] = {
            { L"node.exe" }, { L"deno.exe" }, { L"bun.exe" }, { L"qjs.exe" }
        };
        for (auto& nm : names) {
            std::wstring src = runtimeDir + L"\\" + nm.exe;
            std::wstring dst = exeDir + L"\\" + nm.exe;
            if (GetFileAttributesW(src.c_str()) == INVALID_FILE_ATTRIBUTES) continue;
            if (GetFileAttributesW(dst.c_str()) != INVALID_FILE_ATTRIBUTES) {
                dbgLog(L"workerYtDlp JS runtime already at %ls", dst.c_str());
                break;
            }
            BOOL copied = CopyFileW(src.c_str(), dst.c_str(), TRUE);
            dbgLog(L"workerYtDlp CopyFileW %ls -> %ls : %ls",
                   src.c_str(), dst.c_str(),
                   copied ? L"OK" : L"FAILED");
            if (copied) break;
        }
    }

    std::wstring savedPath;
    static std::mutex s_envMu;
    std::unique_lock<std::mutex> envLk(s_envMu, std::defer_lock);
    bool pathPatched = false;
    if (!runtimeDir.empty()) {
        // Mutate the parent's PATH briefly so the child inherits it via the
        // standard CreateProcess env-inheritance path. Mutex serializes
        // concurrent yt-dlp launches so they don't trample each other's PATH
        // restores. Held until just after CreateProcess returns.
        envLk.lock();
        wchar_t cur[32768] = {};
        DWORD n = GetEnvironmentVariableW(L"PATH", cur, 32768);
        if (n > 0) savedPath.assign(cur, n);
        std::wstring newPath = runtimeDir;
        if (!savedPath.empty()) { newPath += L";"; newPath += savedPath; }
        SetEnvironmentVariableW(L"PATH", newPath.c_str());
        pathPatched = true;
        dbgLog(L"workerYtDlp PATH prefix=%ls (savedPath len=%lu)",
               runtimeDir.c_str(), (unsigned long)savedPath.size());
    } else {
        dbgLog(L"workerYtDlp no JS runtime found — yt-dlp may fail signature solving");
    }

    SECURITY_ATTRIBUTES sa{}; sa.nLength = sizeof(sa); sa.bInheritHandle = TRUE;
    HANDLE hRead = nullptr, hWrite = nullptr;
    if (!CreatePipe(&hRead, &hWrite, &sa, 0)) {
        auto* done = new CompleteMsg{ id, DownloadStatus::NetworkError, L"CreatePipe failed", L"" };
        if (!PostMessageW(g_hwnd, WM_APP_COMPLETED, 0, (LPARAM)done)) delete done;
        return;
    }
    SetHandleInformation(hRead, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si{}; si.cb = sizeof(si);
    si.dwFlags     = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput  = hWrite;
    si.hStdError   = hWrite;
    si.hStdInput   = INVALID_HANDLE_VALUE;

    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> cmdBuf(cmd.begin(), cmd.end());
    cmdBuf.push_back(L'\0');
    BOOL ok = CreateProcessW(nullptr, cmdBuf.data(), nullptr, nullptr, TRUE,
                             CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    if (pathPatched) {
        SetEnvironmentVariableW(L"PATH", savedPath.empty() ? nullptr : savedPath.c_str());
        envLk.unlock();
    }
    CloseHandle(hWrite);
    if (!ok) {
        CloseHandle(hRead);
        DWORD ec = GetLastError();
        auto* done = new CompleteMsg{ id, DownloadStatus::NetworkError,
            L"CreateProcess failed: " + std::to_wstring(ec), L"" };
        if (!PostMessageW(g_hwnd, WM_APP_COMPLETED, 0, (LPARAM)done)) delete done;
        return;
    }

    {
        std::lock_guard<std::mutex> lk(g_itemsMu);
        GuiItem* it = findItem(id);
        if (it) it->ytdlpProc = pi.hProcess;
    }

    std::string raw;
    char chunk[4096];
    DWORD got = 0;
    std::wstring outputPath;
    std::wstring lastErr;
    while (ReadFile(hRead, chunk, sizeof(chunk), &got, nullptr) && got > 0) {
        raw.append(chunk, got);
        for (;;) {
            auto nl = raw.find_first_of("\r\n");
            if (nl == std::string::npos) break;
            std::string line = raw.substr(0, nl);
            raw.erase(0, nl + 1);
            if (line.empty()) continue;
            int n = MultiByteToWideChar(CP_UTF8, 0, line.data(), (int)line.size(), nullptr, 0);
            std::wstring wline(n, L'\0');
            MultiByteToWideChar(CP_UTF8, 0, line.data(), (int)line.size(), wline.data(), n);
            // Log everything that isn't a noisy [download] X.X% line so we
            // can see what yt-dlp / ffmpeg are actually doing.
            const bool isProgress = (wline.compare(0, 11, L"[download] ") == 0) &&
                                    (wline.find(L'%') != std::wstring::npos);
            if (!isProgress) dbgLog(L"yt-dlp[%d]: %ls", id, wline.c_str());
            if (wline.find(L"ERROR") != std::wstring::npos ||
                wline.find(L"WARNING") != std::wstring::npos) lastErr = wline;
            parseYtDlpProgress(id, wline, outputPath);
        }
    }
    CloseHandle(hRead);
    DWORD rc = 1;
    WaitForSingleObject(pi.hProcess, INFINITE);
    GetExitCodeProcess(pi.hProcess, &rc);
    dbgLog(L"workerYtDlp EXIT id=%d rc=%lu outputPath=[%ls] lastErr=[%ls]",
           id, rc, outputPath.c_str(), lastErr.c_str());

    {
        std::lock_guard<std::mutex> lk(g_itemsMu);
        GuiItem* it = findItem(id);
        if (it) it->ytdlpProc = nullptr;
    }
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    DownloadStatus status;
    std::wstring   message;
    bool aborted = false;
    {
        std::lock_guard<std::mutex> lk(g_itemsMu);
        GuiItem* it = findItem(id);
        aborted = it && it->ytdlpAbort;
    }
    if (aborted)        { status = DownloadStatus::Aborted; }
    else if (rc == 0)   { status = DownloadStatus::Ok; }
    else                { status = DownloadStatus::NetworkError;
                          message = L"yt-dlp exited with code " + std::to_wstring((int)rc); }

    // On success, fix the GUI's total/downloaded by reading the merged
    // file's actual size — otherwise the row still shows the audio chunk's
    // size from the last [download] Destination line before the merge.
    if (status == DownloadStatus::Ok && !outputPath.empty()) {
        WIN32_FILE_ATTRIBUTE_DATA fad{};
        if (GetFileAttributesExW(outputPath.c_str(), GetFileExInfoStandard, &fad)) {
            int64_t sz = ((int64_t)fad.nFileSizeHigh << 32) | fad.nFileSizeLow;
            auto* fix = new ProgressMsg{ id, sz, sz, 0, 0, 0 };
            if (!PostMessageW(g_hwnd, WM_APP_PROGRESS, 0, (LPARAM)fix)) delete fix;
        }
    }

    auto* done = new CompleteMsg{ id, status, message, outputPath };
    if (!PostMessageW(g_hwnd, WM_APP_COMPLETED, 0, (LPARAM)done)) delete done;

    // Best-effort: erase the temp cookies file we asked yt-dlp to read.
    // Holds the user's session — leaving it in %TEMP% is a privacy risk.
    if (haveCookieFile) DeleteFileW(cookiesPath.c_str());
}

// ---- worker ---------------------------------------------------------

void workerRun(int id) {
    std::shared_ptr<Downloader>    dl;
    std::shared_ptr<VideoGrabber>  vg;
    std::shared_ptr<FtpDownloader> ftp;
    {
        std::lock_guard<std::mutex> lk(g_itemsMu);
        GuiItem* it = findItem(id);
        if (!it) return;
        dl  = it->dl;
        vg  = it->vg;
        ftp = it->ftp;
    }

    auto httpProgress = [id](const DownloadProgress& p) {
        auto* msg = new ProgressMsg{ id, p.total, p.downloaded,
                                     p.bytesPerSec, p.activeConns, p.segments };
        if (!PostMessageW(g_hwnd, WM_APP_PROGRESS, 0, (LPARAM)msg))
            delete msg;
    };
    auto videoProgress = [id](const VideoProgress& p) {
        // HLS doesn't know the final size up front, so we extrapolate total
        // bytes from per-segment average. Until the first segment lands,
        // total stays -1 (the UI shows just the downloaded byte count and
        // hides the percentage).
        int64_t total = -1;
        if (p.segmentsDone > 0 && p.segmentsTotal > 0 && p.bytes > 0) {
            total = (p.bytes / p.segmentsDone) * p.segmentsTotal;
            if (total < p.bytes) total = p.bytes; // never report < downloaded
        }
        auto* msg = new ProgressMsg{ id, total, p.bytes,
                                     p.bytesPerSec, 0, p.segmentsTotal };
        if (!PostMessageW(g_hwnd, WM_APP_PROGRESS, 0, (LPARAM)msg))
            delete msg;
    };
    if (dl)  dl->setProgressCallback(httpProgress);
    if (vg)  vg->setProgressCallback(videoProgress);
    if (ftp) ftp->setProgressCallback(httpProgress);

    DownloadResult r;
    if (vg)       r = vg->run();
    else if (ftp) r = ftp->run();
    else if (dl)  r = dl->run();
    else return;

    auto* done = new CompleteMsg{ id, r.status, r.message, r.outputPath };
    if (!PostMessageW(g_hwnd, WM_APP_COMPLETED, 0, (LPARAM)done))
        delete done;
}

// ---- item ops --------------------------------------------------------

std::wstring resolveOutDir(const std::wstring& perItem) {
    return perItem.empty() ? g_settings.outputDir : perItem;
}

// Like resolveOutDir but, when categorize is on and the user hasn't given
// a per-item folder, route the file into its category's folder. Filename
// is needed because category routing is by file extension. Caller still
// passes the raw user input through perItem to win over categorization.
std::wstring resolveOutDirForFile(const std::wstring& perItem,
                                  const std::wstring& filename,
                                  const std::wstring& url) {
    if (!perItem.empty()) return perItem;
    if (!g_settings.categorize) return g_settings.outputDir;
    std::wstring forCat = filename;
    if (forCat.empty()) {
        Url u; std::wstring err;
        if (parseUrl(url, u, err)) forCat = u.inferredFilename();
    }
    if (forCat.empty()) return g_settings.outputDir;
    const Category* cat = categorize(forCat);
    if (cat) {
        const std::wstring& folder = categoryFolder(cat->name);
        if (!folder.empty()) return folder;
    }
    return g_settings.outputDir;
}

int addDownloadEx(const HandoffSpec& spec) {
    if (spec.url.empty()) return 0;
    // Deduplicate: if the same URL is already running (or queued), bring
    // the window forward and bail. Stops rapid double-clicks from spawning
    // parallel yt-dlp/ffmpeg pipelines that fight over the same temp files.
    {
        std::lock_guard<std::mutex> lk(g_itemsMu);
        for (auto& p : g_items) {
            if (p->url == spec.url &&
                (p->state == ItemState::Running || p->state == ItemState::Queued)) {
                dbgLog(L"addDownloadEx SKIP duplicate active url=%ls existingId=%d",
                       spec.url.c_str(), p->id);
                emitToast(L"Already downloading this URL", L"info");
                return p->id;
            }
        }
    }
    bool ytMatch = looksLikeYouTubeUrl(spec.url);
    dbgLog(L"addDownloadEx url=%ls", spec.url.c_str());
    dbgLog(L"  isYtDlp=%d isVideo=%d isFtp=%d filename=%ls outDir=%ls",
           (int)ytMatch,
           (int)looksLikeVideoUrl(spec.url),
           (int)looksLikeFtpUrl(spec.url),
           spec.filename.c_str(), spec.outDir.c_str());
    auto it = std::make_unique<GuiItem>();
    it->id       = g_nextId.fetch_add(1);
    it->url      = spec.url;
    it->filename = spec.filename;
    it->outDir   = resolveOutDirForFile(spec.outDir, spec.filename, spec.url);
    it->isYtDlp  = ytMatch;
    it->isYtPlaylist = it->isYtDlp && looksLikeYouTubePlaylistUrl(spec.url);
    it->isVideo  = !it->isYtDlp && looksLikeVideoUrl(spec.url);
    it->isFtp    = !it->isYtDlp && looksLikeFtpUrl(spec.url);
    // Concurrency cap: if there are already maxJobs items running, hold this
    // one as Queued. The dispatcher in onCompleted will start it later.
    // Also gate on the global queuePaused toggle (Downloads -> Stop queue).
    // Playlist enumeration (isYtPlaylist) is exempt -- it's a fast metadata
    // call, not a real download -- so it always runs immediately.
    int runningNow = 0;
    {
        std::lock_guard<std::mutex> lk(g_itemsMu);
        for (auto& p : g_items) if (p->state == ItemState::Running) ++runningNow;
    }
    bool capped = !it->isYtPlaylist && (
                      (g_settings.maxJobs > 0 && runningNow >= g_settings.maxJobs) ||
                      g_settings.queuePaused);
    it->state    = capped ? ItemState::Queued : ItemState::Running;
    it->startedEpoch = (int64_t)std::time(nullptr);

    if (!spec.referer.empty())   it->headers.push_back({L"Referer",    spec.referer});
    if (!spec.cookie.empty())    it->headers.push_back({L"Cookie",     spec.cookie});
    if (!spec.userAgent.empty()) it->headers.push_back({L"User-Agent", spec.userAgent});
    it->ytFormat    = spec.ytFormat;
    it->cookiesPath = spec.cookiesPath;

    if (it->isYtDlp) {
        // No backing Downloader/VideoGrabber/FtpDownloader — workerYtDlp
        // wraps the yt-dlp.exe child process directly.
    } else if (it->isVideo) {
        VideoOptions vo;
        vo.outputDir  = it->outDir;
        vo.outputName = spec.filename;
        vo.quality    = L"best";
        vo.headers    = it->headers;
        it->vg = std::make_shared<VideoGrabber>(it->url, vo);
    } else if (it->isFtp) {
        DownloadOptions dlo;
        dlo.outputDir    = it->outDir;
        dlo.outputName   = spec.filename;
        dlo.bandwidthBps = g_settings.bandwidthBps;
        dlo.headers      = it->headers;
        it->ftp = std::make_shared<FtpDownloader>(it->url, dlo);
    } else {
        DownloadOptions dlo;
        dlo.outputDir      = it->outDir;
        dlo.outputName     = spec.filename;
        dlo.connections    = g_settings.connections;
        dlo.bandwidthBps   = g_settings.bandwidthBps;
        dlo.verifyChecksum = g_settings.verifyChecksum;
        dlo.headers        = it->headers;
        it->dl = std::make_shared<Downloader>(it->url, dlo);
    }

    int id = it->id;
    bool ytdlp    = it->isYtDlp;
    bool playlist = it->isYtPlaylist;
    bool startNow = !capped;
    {
        std::lock_guard<std::mutex> lk(g_itemsMu);
        g_items.push_back(std::move(it));
        GuiItem* p = g_items.back().get();
        if (startNow) {
            p->thread.reset(new std::thread(
                playlist ? workerYtDlpPlaylist : (ytdlp ? workerYtDlp : workerRun),
                id));
        }
        emitItem(*p);
    }
    return id;
}

// Promote queued items to Running until maxJobs are in flight. Called after
// each completion so the next pending download starts automatically.
void dispatchQueued() {
    // Honor the global "Stop queue" toggle: don't promote anything new.
    // Already-running items keep going; this only blocks new starts.
    if (g_settings.queuePaused) return;
    std::vector<int> toStart;
    {
        std::lock_guard<std::mutex> lk(g_itemsMu);
        int running = 0;
        for (auto& p : g_items) if (p->state == ItemState::Running) ++running;
        int budget = g_settings.maxJobs > 0 ? (g_settings.maxJobs - running) : 999;
        if (budget <= 0) return;
        for (auto& p : g_items) {
            if (p->state != ItemState::Queued) continue;
            if (p->thread) continue; // already had a worker once (shouldn't happen)
            toStart.push_back(p->id);
            if ((int)toStart.size() >= budget) break;
        }
    }
    for (int id : toStart) {
        std::lock_guard<std::mutex> lk(g_itemsMu);
        GuiItem* it = findItem(id);
        if (!it || it->state != ItemState::Queued) continue;
        it->state = ItemState::Running;
        it->startedEpoch = (int64_t)std::time(nullptr);
        bool ytdlp    = it->isYtDlp;
        bool playlist = it->isYtPlaylist;
        int  cid      = it->id;
        it->thread.reset(new std::thread(
            playlist ? workerYtDlpPlaylist : (ytdlp ? workerYtDlp : workerRun),
            cid));
        emitItem(*it);
    }
}

int addDownload(const std::wstring& url, const std::wstring& filename,
                const std::wstring& outDir) {
    HandoffSpec s;
    s.url = url; s.filename = filename; s.outDir = outDir;
    return addDownloadEx(s);
}

void abortItem(int id) {
    std::lock_guard<std::mutex> lk(g_itemsMu);
    GuiItem* it = findItem(id);
    if (!it) return;
    if (it->dl)  it->dl->abort();
    if (it->vg)  it->vg->abort();
    if (it->ftp) it->ftp->abort();
    if (it->isYtDlp) {
        it->ytdlpAbort = true;
        if (it->ytdlpProc) TerminateProcess(it->ytdlpProc, 1);
    }
}

void removeItem(int id) {
    std::unique_ptr<std::thread> th;
    {
        std::lock_guard<std::mutex> lk(g_itemsMu);
        GuiItem* it = findItem(id);
        if (!it) return;
        if (it->dl)  it->dl->abort();
        if (it->vg)  it->vg->abort();
        if (it->ftp) it->ftp->abort();
        if (it->isYtDlp) {
            it->ytdlpAbort = true;
            if (it->ytdlpProc) TerminateProcess(it->ytdlpProc, 1);
        }
        if (it->thread) th = std::move(it->thread);
        auto pos = std::remove_if(g_items.begin(), g_items.end(),
            [id](const std::unique_ptr<GuiItem>& p){ return p->id == id; });
        g_items.erase(pos, g_items.end());
    }
    if (th && th->joinable()) th->detach();
    emitItemRemoved(id);
}

void restartItem(int id) {
    HandoffSpec spec;
    {
        std::lock_guard<std::mutex> lk(g_itemsMu);
        GuiItem* it = findItem(id);
        if (!it) return;
        if (it->state == ItemState::Running) return;
        spec.url      = it->url;
        spec.filename = it->filename;
        spec.outDir   = it->outDir;
        for (auto& h : it->headers) {
            if      (h.first == L"Referer")    spec.referer   = h.second;
            else if (h.first == L"Cookie")     spec.cookie    = h.second;
            else if (h.first == L"User-Agent") spec.userAgent = h.second;
        }
    }
    removeItem(id);
    addDownloadEx(spec);
}

void openInShell(const std::wstring& path, bool selectInFolder) {
    if (path.empty()) return;
    if (selectInFolder) {
        // Open Explorer with the file selected.
        std::wstring args = L"/select,\"" + path + L"\"";
        ShellExecuteW(nullptr, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
    } else {
        ShellExecuteW(nullptr, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
}

void openItemFile(int id) {
    std::wstring p;
    {
        std::lock_guard<std::mutex> lk(g_itemsMu);
        GuiItem* it = findItem(id);
        if (it) p = it->resultPath;
    }
    openInShell(p, false);
}

void openItemFolder(int id) {
    std::wstring p, dir;
    {
        std::lock_guard<std::mutex> lk(g_itemsMu);
        GuiItem* it = findItem(id);
        if (!it) return;
        p   = it->resultPath;
        dir = it->outDir;
    }
    if (!p.empty()) openInShell(p, true);
    else if (!dir.empty()) {
        ShellExecuteW(nullptr, L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
}

// ---- progress / completion handling on UI thread ---------------------

void onProgress(ProgressMsg* m) {
    std::unique_ptr<ProgressMsg> g(m);
    bool changed = false;
    GuiItem snap{};
    {
        std::lock_guard<std::mutex> lk(g_itemsMu);
        GuiItem* it = findItem(m->id);
        if (!it) return;
        it->total       = m->total;
        it->downloaded  = m->downloaded;
        it->bps         = m->bps;
        it->activeConns = m->activeConns;
        it->segments    = m->segments;
        if (it->state != ItemState::Running) {
            it->state = ItemState::Running;
            changed = true;
        }
        snap = GuiItem{};
        snap.id           = it->id;
        snap.url          = it->url;
        snap.filename     = it->filename;
        snap.outDir       = it->outDir;
        snap.resultPath   = it->resultPath;
        snap.message      = it->message;
        snap.state        = it->state;
        snap.total        = it->total;
        snap.downloaded   = it->downloaded;
        snap.bps          = it->bps;
        snap.activeConns  = it->activeConns;
        snap.segments     = it->segments;
        snap.startedEpoch = it->startedEpoch;
    }
    (void)changed;
    emitItem(snap);
}

void onCompleted(CompleteMsg* m) {
    std::unique_ptr<CompleteMsg> g(m);
    GuiItem snap{};
    {
        std::lock_guard<std::mutex> lk(g_itemsMu);
        GuiItem* it = findItem(m->id);
        if (!it) return;
        switch (m->status) {
            case DownloadStatus::Ok:      it->state = ItemState::Done;    break;
            case DownloadStatus::Aborted: it->state = ItemState::Aborted; break;
            default:                      it->state = ItemState::Err;     break;
        }
        it->message    = m->message;
        it->resultPath = m->path;
        // On success, prefer the actual file size on disk over our running
        // counter. HLS+ffmpeg remux produces a different byte count than the
        // sum of raw .ts segments we downloaded, and our extrapolated total
        // for HLS is also approximate. Stat the output and use that.
        if (it->state == ItemState::Done && !it->resultPath.empty()) {
            WIN32_FILE_ATTRIBUTE_DATA fad{};
            if (GetFileAttributesExW(it->resultPath.c_str(), GetFileExInfoStandard, &fad)) {
                int64_t sz = ((int64_t)fad.nFileSizeHigh << 32) | fad.nFileSizeLow;
                if (sz > 0) {
                    it->total      = sz;
                    it->downloaded = sz;
                }
            }
        }
        if (it->state == ItemState::Done && it->total < 0)
            it->total = it->downloaded;
        snap.id           = it->id;
        snap.url          = it->url;
        snap.filename     = it->filename;
        snap.outDir       = it->outDir;
        snap.resultPath   = it->resultPath;
        snap.message      = it->message;
        snap.state        = it->state;
        snap.total        = it->total;
        snap.downloaded   = it->downloaded;
        snap.bps          = 0;
        snap.activeConns  = 0;
        snap.startedEpoch = it->startedEpoch;
    }
    emitItem(snap);
    if (m->status == DownloadStatus::Ok) {
        emitToast(L"Download complete: " + (snap.filename.empty() ? snap.url : snap.filename), L"ok");
        if (g_settings.soundOnComplete) MessageBeep(MB_ICONINFORMATION);
    } else if (m->status != DownloadStatus::Aborted) {
        emitToast(L"Download failed: " + m->message, L"err");
        if (g_settings.soundOnComplete) MessageBeep(MB_ICONERROR);
    }
    // Promote the next queued item to Running, respecting maxJobs.
    dispatchQueued();
}

// ---- bridge: handle JS → C++ messages -------------------------------

// Tiny JSON reader that handles flat object {"type":"...","key":val}.
// Sufficient for our message surface; uses a permissive shallow parser.
struct JsonView {
    const wchar_t* s; size_t n;
};

void skipWs(JsonView& v, size_t& i) {
    while (i < v.n && (v.s[i] == L' ' || v.s[i] == L'\t' || v.s[i] == L'\n' || v.s[i] == L'\r')) ++i;
}
bool readString(JsonView& v, size_t& i, std::wstring& out) {
    skipWs(v, i);
    if (i >= v.n || v.s[i] != L'"') return false;
    ++i;
    out.clear();
    while (i < v.n && v.s[i] != L'"') {
        if (v.s[i] == L'\\' && i + 1 < v.n) {
            wchar_t c = v.s[i+1];
            switch (c) {
                case L'"':  out.push_back(L'"');  break;
                case L'\\': out.push_back(L'\\'); break;
                case L'/':  out.push_back(L'/');  break;
                case L'n':  out.push_back(L'\n'); break;
                case L't':  out.push_back(L'\t'); break;
                case L'r':  out.push_back(L'\r'); break;
                case L'u': {
                    if (i + 5 >= v.n) return false;
                    unsigned cp = 0;
                    for (int k = 0; k < 4; ++k) {
                        wchar_t h = v.s[i+2+k]; cp <<= 4;
                        if (h >= L'0' && h <= L'9') cp |= h - L'0';
                        else if (h >= L'a' && h <= L'f') cp |= 10 + (h - L'a');
                        else if (h >= L'A' && h <= L'F') cp |= 10 + (h - L'A');
                        else return false;
                    }
                    out.push_back((wchar_t)cp);
                    i += 6;
                    continue;
                }
                default: return false;
            }
            i += 2;
        } else {
            out.push_back(v.s[i++]);
        }
    }
    if (i >= v.n) return false;
    ++i;
    return true;
}
bool readNumber(JsonView& v, size_t& i, double& out) {
    skipWs(v, i);
    size_t start = i;
    if (i < v.n && (v.s[i] == L'-' || v.s[i] == L'+')) ++i;
    while (i < v.n && ((v.s[i] >= L'0' && v.s[i] <= L'9') || v.s[i] == L'.' || v.s[i] == L'e' || v.s[i] == L'E' || v.s[i] == L'+' || v.s[i] == L'-')) ++i;
    if (i == start) return false;
    std::wstring s(v.s + start, i - start);
    out = _wtof(s.c_str());
    return true;
}
bool readBool(JsonView& v, size_t& i, bool& out) {
    skipWs(v, i);
    if (i + 4 <= v.n && wcsncmp(v.s + i, L"true", 4) == 0) { out = true; i += 4; return true; }
    if (i + 5 <= v.n && wcsncmp(v.s + i, L"false", 5) == 0) { out = false; i += 5; return true; }
    return false;
}
bool skipValue(JsonView& v, size_t& i) {
    skipWs(v, i);
    if (i >= v.n) return false;
    wchar_t c = v.s[i];
    if (c == L'"') { std::wstring d; return readString(v, i, d); }
    if (c == L'{' || c == L'[') {
        wchar_t open = c, close = (c == L'{') ? L'}' : L']';
        int depth = 0;
        while (i < v.n) {
            wchar_t x = v.s[i];
            if (x == L'"') { std::wstring d; if (!readString(v, i, d)) return false; continue; }
            if (x == open)  { ++depth; ++i; continue; }
            if (x == close) { --depth; ++i; if (depth == 0) return true; continue; }
            ++i;
        }
        return false;
    }
    while (i < v.n && v.s[i] != L',' && v.s[i] != L'}' && v.s[i] != L']' &&
           v.s[i] != L' ' && v.s[i] != L'\t' && v.s[i] != L'\n' && v.s[i] != L'\r') ++i;
    return true;
}

struct ParsedMsg {
    std::wstring type;
    std::wstring url, filename, outDir;
    int          id = 0;
    int64_t      value = 0;     // generic numeric payload (setBandwidthBps, setQueuePaused)
    bool         hasSettings = false;
    Settings     settings;
};

bool parseMessage(const std::wstring& json, ParsedMsg& m) {
    JsonView v{ json.c_str(), json.size() };
    size_t i = 0;
    skipWs(v, i);
    if (i >= v.n || v.s[i] != L'{') return false;
    ++i;
    while (true) {
        skipWs(v, i);
        if (i >= v.n) break;
        if (v.s[i] == L'}') { ++i; break; }
        std::wstring key;
        if (!readString(v, i, key)) break;
        skipWs(v, i);
        if (i >= v.n || v.s[i] != L':') break;
        ++i;
        skipWs(v, i);
        if (key == L"type")     { if (!readString(v, i, m.type))     break; }
        else if (key == L"url")     { if (!readString(v, i, m.url))     break; }
        else if (key == L"filename"){ if (!readString(v, i, m.filename))break; }
        else if (key == L"outDir")  { if (!readString(v, i, m.outDir))  break; }
        else if (key == L"id")      { double d=0; if (!readNumber(v, i, d)) break; m.id = (int)d; }
        else if (key == L"value")   { double d=0; if (!readNumber(v, i, d)) break; m.value = (int64_t)d; }
        else if (key == L"settings") {
            // Parse nested settings object.
            skipWs(v, i);
            if (i >= v.n || v.s[i] != L'{') { skipValue(v, i); }
            else {
                ++i;
                while (true) {
                    skipWs(v, i);
                    if (i >= v.n) break;
                    if (v.s[i] == L'}') { ++i; break; }
                    std::wstring sk;
                    if (!readString(v, i, sk)) break;
                    skipWs(v, i);
                    if (i >= v.n || v.s[i] != L':') break;
                    ++i;
                    skipWs(v, i);
                    if (sk == L"outDir")          { readString(v, i, m.settings.outputDir); }
                    else if (sk == L"connections")    { double d=0; readNumber(v, i, d); m.settings.connections = (int)d; }
                    else if (sk == L"maxJobs")        { double d=0; readNumber(v, i, d); m.settings.maxJobs = (int)d; }
                    else if (sk == L"bandwidthBps")   { double d=0; readNumber(v, i, d); m.settings.bandwidthBps = (int64_t)d; }
                    else if (sk == L"clipboardWatch") { bool b=false; readBool(v, i, b); m.settings.clipboardWatch = b; }
                    else if (sk == L"verifyChecksum") { bool b=true;  readBool(v, i, b); m.settings.verifyChecksum = b; }
                    else if (sk == L"categorize")     { bool b=false; readBool(v, i, b); m.settings.categorize = b; }
                    else if (sk == L"queuePaused")    { bool b=false; readBool(v, i, b); m.settings.queuePaused = b; }
                    else if (sk == L"schedEnabled")   { bool b=false; readBool(v, i, b); m.settings.schedEnabled = b; }
                    else if (sk == L"schedStartMinutes") { double d=0; readNumber(v, i, d); m.settings.schedStartMinutes = (int)d; }
                    else if (sk == L"schedStopMinutes")  { double d=0; readNumber(v, i, d); m.settings.schedStopMinutes  = (int)d; }
                    else if (sk == L"schedDaysMask")     { double d=0; readNumber(v, i, d); m.settings.schedDaysMask     = (int)d; }
                    else if (sk == L"schedShutdownDone") { bool b=false; readBool(v, i, b); m.settings.schedShutdownDone = b; }
                    else if (sk == L"launchOnStartup") { bool b=false; readBool(v, i, b); m.settings.launchOnStartup = b; }
                    else if (sk == L"soundOnComplete") { bool b=true;  readBool(v, i, b); m.settings.soundOnComplete = b; }
                    else if (sk == L"catDirVideo")     { readString(v, i, m.settings.catDirVideo); }
                    else if (sk == L"catDirMusic")     { readString(v, i, m.settings.catDirMusic); }
                    else if (sk == L"catDirArchive")   { readString(v, i, m.settings.catDirArchive); }
                    else if (sk == L"catDirProgram")   { readString(v, i, m.settings.catDirProgram); }
                    else if (sk == L"catDirDocument")  { readString(v, i, m.settings.catDirDocument); }
                    else if (sk == L"proxyMode")    { double d=0; readNumber(v, i, d); m.settings.proxyMode = (int)d; }
                    else if (sk == L"proxyServer")  { readString(v, i, m.settings.proxyServer); }
                    else if (sk == L"proxyBypass")  { readString(v, i, m.settings.proxyBypass); }
                    else if (sk == L"proxyUser")    { readString(v, i, m.settings.proxyUser); }
                    else if (sk == L"proxyPass")    { readString(v, i, m.settings.proxyPass); }
                    else if (sk == L"siteLogins") {
                        skipWs(v, i);
                        if (i < v.n && v.s[i] == L'[') {
                            ++i;
                            while (true) {
                                skipWs(v, i);
                                if (i >= v.n) break;
                                if (v.s[i] == L']') { ++i; break; }
                                if (v.s[i] == L'{') {
                                    ++i;
                                    SiteLogin sl;
                                    while (true) {
                                        skipWs(v, i);
                                        if (i >= v.n) break;
                                        if (v.s[i] == L'}') { ++i; break; }
                                        std::wstring lk;
                                        if (!readString(v, i, lk)) break;
                                        skipWs(v, i);
                                        if (i >= v.n || v.s[i] != L':') break;
                                        ++i;
                                        skipWs(v, i);
                                        if      (lk == L"host") readString(v, i, sl.host);
                                        else if (lk == L"user") readString(v, i, sl.user);
                                        else if (lk == L"pass") readString(v, i, sl.pass);
                                        else                    skipValue(v, i);
                                        skipWs(v, i);
                                        if (i < v.n && v.s[i] == L',') { ++i; continue; }
                                    }
                                    if (!sl.host.empty()) m.settings.siteLogins.push_back(std::move(sl));
                                }
                                skipWs(v, i);
                                if (i < v.n && v.s[i] == L',') { ++i; continue; }
                            }
                        }
                    }
                    else                              { skipValue(v, i); }
                    skipWs(v, i);
                    if (i < v.n && v.s[i] == L',') { ++i; continue; }
                }
                m.hasSettings = true;
            }
        }
        else                        { skipValue(v, i); }
        skipWs(v, i);
        if (i < v.n && v.s[i] == L',') { ++i; continue; }
    }
    return true;
}

void handleMessage(const std::wstring& json) {
    ParsedMsg m;
    if (!parseMessage(json, m)) return;

    if      (m.type == L"ready") {
        emitItemsSnapshot();
        emitSettings();
    }
    else if (m.type == L"addDownload") {
        if (!m.url.empty()) addDownload(m.url, m.filename, m.outDir);
    }
    else if (m.type == L"pause" || m.type == L"abort") {
        abortItem(m.id);
    }
    else if (m.type == L"resume" || m.type == L"restart") {
        restartItem(m.id);
    }
    else if (m.type == L"remove") {
        removeItem(m.id);
    }
    else if (m.type == L"openFile") {
        openItemFile(m.id);
    }
    else if (m.type == L"openFolder") {
        openItemFolder(m.id);
    }
    else if (m.type == L"redownload") {
        // IDM-parity: re-fetch the same URL from scratch. Capture the spec
        // off the existing item, drop the row, then re-add as a new item.
        HandoffSpec spec;
        bool found = false;
        {
            std::lock_guard<std::mutex> lk(g_itemsMu);
            GuiItem* it = findItem(m.id);
            if (it) {
                spec.url      = it->url;
                spec.filename = it->filename;
                spec.outDir   = it->outDir;
                for (auto& kv : it->headers) {
                    if      (_wcsicmp(kv.first.c_str(), L"Referer") == 0)    spec.referer   = kv.second;
                    else if (_wcsicmp(kv.first.c_str(), L"Cookie") == 0)     spec.cookie    = kv.second;
                    else if (_wcsicmp(kv.first.c_str(), L"User-Agent") == 0) spec.userAgent = kv.second;
                }
                spec.ytFormat = it->ytFormat;
                found = true;
            }
        }
        if (found) {
            removeItem(m.id);
            addDownloadEx(spec);
        }
    }
    else if (m.type == L"setSettings") {
        if (m.hasSettings) {
            g_settings.outputDir       = m.settings.outputDir;
            g_settings.connections     = m.settings.connections > 0 ? m.settings.connections : 8;
            g_settings.maxJobs         = m.settings.maxJobs > 0 ? m.settings.maxJobs : 3;
            g_settings.bandwidthBps    = m.settings.bandwidthBps;
            g_settings.clipboardWatch  = m.settings.clipboardWatch;
            g_settings.verifyChecksum  = m.settings.verifyChecksum;
            g_settings.categorize      = m.settings.categorize;
            g_settings.schedEnabled       = m.settings.schedEnabled;
            g_settings.schedStartMinutes  = m.settings.schedStartMinutes;
            g_settings.schedStopMinutes   = m.settings.schedStopMinutes;
            g_settings.schedDaysMask      = m.settings.schedDaysMask & 0x7F;
            g_settings.schedShutdownDone  = m.settings.schedShutdownDone;
            // v0.9.7 General-tab fields. Side effects on transition are
            // wired below: launch-on-startup writes/clears HKCU\Run, and
            // the clipboard watcher subscribes/unsubscribes.
            bool wasLaunchOnStartup = g_settings.launchOnStartup;
            bool wasClipboardWatch  = g_settings.clipboardWatch;
            g_settings.launchOnStartup    = m.settings.launchOnStartup;
            g_settings.soundOnComplete    = m.settings.soundOnComplete;
            g_settings.catDirVideo        = m.settings.catDirVideo;
            g_settings.catDirMusic        = m.settings.catDirMusic;
            g_settings.catDirArchive      = m.settings.catDirArchive;
            g_settings.catDirProgram      = m.settings.catDirProgram;
            g_settings.catDirDocument     = m.settings.catDirDocument;
            // v0.9.8 Proxy.
            g_settings.proxyMode    = m.settings.proxyMode;
            g_settings.proxyServer  = m.settings.proxyServer;
            g_settings.proxyBypass  = m.settings.proxyBypass;
            g_settings.proxyUser    = m.settings.proxyUser;
            g_settings.proxyPass    = m.settings.proxyPass;
            g_settings.siteLogins   = m.settings.siteLogins;
            setSiteLogins(g_settings.siteLogins);
            if (wasLaunchOnStartup != g_settings.launchOnStartup)
                applyLaunchOnStartup(g_settings.launchOnStartup);
            if (wasClipboardWatch != g_settings.clipboardWatch)
                applyClipboardWatch(g_settings.clipboardWatch);
            // Push proxy state into the HTTP layer; HttpSession rebuilds
            // its WinHTTP session on the next acquire.
            {
                ProxyConfig pc;
                pc.mode   = g_settings.proxyMode;
                pc.server = g_settings.proxyServer;
                pc.bypass = g_settings.proxyBypass;
                pc.user   = g_settings.proxyUser;
                pc.pass   = g_settings.proxyPass;
                setProxyConfig(pc);
            }
            saveSettings();
            emitSettings();
            emitToast(L"Settings saved", L"ok");
            // If the user raised maxJobs, kick the queue.
            dispatchQueued();
        }
    }
    else if (m.type == L"pauseAll") {
        std::vector<int> ids;
        {
            std::lock_guard<std::mutex> lk(g_itemsMu);
            for (auto& it : g_items)
                if (it->state == ItemState::Running) ids.push_back(it->id);
        }
        for (int id : ids) abortItem(id);
    }
    else if (m.type == L"resumeAll") {
        std::vector<int> ids;
        {
            std::lock_guard<std::mutex> lk(g_itemsMu);
            for (auto& it : g_items)
                if (it->state == ItemState::Aborted || it->state == ItemState::Err) ids.push_back(it->id);
        }
        for (int id : ids) restartItem(id);
    }
    else if (m.type == L"deleteCompleted") {
        // Remove every Done item. Snapshot the ids under the lock, then
        // call removeItem (which takes the lock itself) outside.
        std::vector<int> ids;
        {
            std::lock_guard<std::mutex> lk(g_itemsMu);
            for (auto& it : g_items)
                if (it->state == ItemState::Done) ids.push_back(it->id);
        }
        for (int id : ids) removeItem(id);
        if (!ids.empty()) {
            std::wstring msg = L"Removed " + std::to_wstring(ids.size()) +
                               L" finished download" + (ids.size() == 1 ? L"" : L"s");
            emitToast(msg, L"ok");
        }
    }
    else if (m.type == L"setBandwidthBps") {
        // Quick speed-limiter pick from the Downloads menu. Per-item
        // RateLimiters don't expose a setter, so this only affects new
        // downloads -- matches IDM's behaviour closely enough.
        g_settings.bandwidthBps = m.value < 0 ? 0 : m.value;
        saveSettings();
        emitSettings();
    }
    else if (m.type == L"setQueuePaused") {
        bool paused = (m.value != 0);
        if (g_settings.queuePaused != paused) {
            g_settings.queuePaused = paused;
            saveSettings();
            emitSettings();
            // When un-pausing, promote whatever was sitting in Queued.
            if (!paused) dispatchQueued();
            emitToast(paused ? L"Queue stopped" : L"Queue started", L"info");
        }
    }
    else if (m.type == L"win.minimize") {
        // Hide instead of minimize so the window leaves the taskbar and
        // lives in the system tray. Double-click the tray icon to restore.
        ShowWindow(g_hwnd, SW_HIDE);
    }
    else if (m.type == L"win.toggleMaximize") {
        if (IsZoomed(g_hwnd)) ShowWindow(g_hwnd, SW_RESTORE);
        else                  ShowWindow(g_hwnd, SW_MAXIMIZE);
    }
    else if (m.type == L"win.beginDrag") {
        // The OS title bar is gone (WS_POPUP), so the user drags the window
        // by mousedown'ing the .titlebar-drag div in the WebView. Hand off
        // to the OS via the standard "fake an HTCAPTION click" pattern.
        ReleaseCapture();
        SendMessageW(g_hwnd, WM_NCLBUTTONDOWN, HTCAPTION, 0);
    }
    else if (m.type == L"win.close") {
        DestroyWindow(g_hwnd);
    }
}

// ---- WebView2 setup --------------------------------------------------

std::wstring exeDir() {
    wchar_t buf[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (n == 0) return L"";
    std::wstring p(buf, n);
    auto slash = p.find_last_of(L"\\/");
    return (slash == std::wstring::npos) ? L"" : p.substr(0, slash);
}

std::wstring uiDir() {
    // ui/ next to the exe (installed) or one level up under matata\ui\ in the
    // dev tree (build\ → ..\ui\). Try both.
    std::wstring base = exeDir();
    std::wstring tryA = base + L"\\ui";
    if (GetFileAttributesW(tryA.c_str()) != INVALID_FILE_ATTRIBUTES) return tryA;
    std::wstring tryB = base + L"\\..\\ui";
    if (GetFileAttributesW(tryB.c_str()) != INVALID_FILE_ATTRIBUTES) return tryB;
    return tryA; // fall through; navigation will fail noisily
}

std::wstring userDataDir() {
    wchar_t buf[MAX_PATH];
    if (SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, buf) == S_OK) {
        std::wstring p = buf;
        p += L"\\matata\\webview2";
        SHCreateDirectoryExW(nullptr, p.c_str(), nullptr);
        return p;
    }
    return L"";
}

void initWebView2() {
    HRESULT hr = CreateCoreWebView2EnvironmentWithOptions(
        nullptr, userDataDir().c_str(), nullptr,
        Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [](HRESULT result, ICoreWebView2Environment* env) -> HRESULT {
                if (!env) {
                    MessageBoxW(g_hwnd,
                        L"Failed to create WebView2 environment.\n\n"
                        L"matata needs the Microsoft Edge WebView2 Runtime.\n"
                        L"It ships with Windows 11 and recent Windows 10 builds.\n"
                        L"Get it from: https://go.microsoft.com/fwlink/p/?LinkId=2124703",
                        L"matata", MB_OK | MB_ICONERROR);
                    DestroyWindow(g_hwnd);
                    return S_OK;
                }
                env->CreateCoreWebView2Controller(g_hwnd,
                    Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                        [](HRESULT result, ICoreWebView2Controller* controller) -> HRESULT {
                            if (!controller) {
                                DestroyWindow(g_hwnd);
                                return S_OK;
                            }
                            g_controller = controller;
                            g_controller->get_CoreWebView2(&g_webview);

                            RECT rc; GetClientRect(g_hwnd, &rc);
                            g_controller->put_Bounds(rc);

                            // Map the virtual host to ui/ folder.
                            ComPtr<ICoreWebView2_3> wv3;
                            if (SUCCEEDED(g_webview.As(&wv3))) {
                                wv3->SetVirtualHostNameToFolderMapping(
                                    kVirtualHost,
                                    uiDir().c_str(),
                                    COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_DENY_CORS);
                            }

                            // Trim chrome: hide context menu / dev tools in release.
                            ComPtr<ICoreWebView2Settings> settings;
                            g_webview->get_Settings(&settings);
                            if (settings) {
                                settings->put_AreDefaultContextMenusEnabled(FALSE);
                                settings->put_IsStatusBarEnabled(FALSE);
                                settings->put_AreDevToolsEnabled(FALSE);
                                settings->put_IsZoomControlEnabled(FALSE);
                            }

                            // Wire JS → C++ message bridge.
                            EventRegistrationToken token;
                            g_webview->add_WebMessageReceived(
                                Callback<ICoreWebView2WebMessageReceivedEventHandler>(
                                    [](ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
                                        LPWSTR raw = nullptr;
                                        if (SUCCEEDED(args->TryGetWebMessageAsString(&raw)) && raw) {
                                            handleMessage(std::wstring(raw));
                                            CoTaskMemFree(raw);
                                        }
                                        return S_OK;
                                    }).Get(),
                                &token);

                            g_webview->Navigate((std::wstring(L"https://") + kVirtualHost + L"/index.html").c_str());

                            g_webviewReady = true;
                            // Flush anything queued before WebView was ready.
                            for (auto& s : g_pendingScripts) g_webview->ExecuteScript(s.c_str(), nullptr);
                            g_pendingScripts.clear();
                            return S_OK;
                        }).Get());
                return S_OK;
            }).Get());
    if (FAILED(hr)) {
        MessageBoxW(g_hwnd,
            L"WebView2 runtime not found.\n\n"
            L"Install it from: https://go.microsoft.com/fwlink/p/?LinkId=2124703",
            L"matata", MB_OK | MB_ICONERROR);
        DestroyWindow(g_hwnd);
    }
}

// ---- main window proc ------------------------------------------------

LRESULT CALLBACK MainWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_SIZE: {
        if (g_controller) {
            RECT rc; GetClientRect(hwnd, &rc);
            g_controller->put_Bounds(rc);
        }
        return 0;
    }
    case WM_MOVE:
    case WM_EXITSIZEMOVE: {
        WINDOWPLACEMENT wp_{}; wp_.length = sizeof(wp_);
        if (GetWindowPlacement(hwnd, &wp_)) {
            g_settings.windowMaximized = (wp_.showCmd == SW_SHOWMAXIMIZED);
            RECT& r = wp_.rcNormalPosition;
            g_settings.windowX = r.left;
            g_settings.windowY = r.top;
            g_settings.windowW = r.right  - r.left;
            g_settings.windowH = r.bottom - r.top;
        }
        return 0;
    }
    case WM_APP_BRIDGE_EVENT: {
        std::unique_ptr<std::wstring> p((std::wstring*)lp);
        executeOnUi(*p);
        return 0;
    }
    case WM_APP_PROGRESS: {
        onProgress((ProgressMsg*)lp);
        return 0;
    }
    case WM_APP_COMPLETED: {
        onCompleted((CompleteMsg*)lp);
        return 0;
    }
    case WM_APP_UPDATE_AVAIL: {
        onUpdateAvailable((UpdateAvailMsg*)lp);
        return 0;
    }
    case WM_APP_TRAYICON: {
        // lp's low word is the underlying mouse event from the tray.
        switch (LOWORD(lp)) {
            case WM_LBUTTONDBLCLK: showFromTray(); break;
            case WM_RBUTTONUP:     showTrayMenu(); break;
        }
        return 0;
    }
    case WM_CLIPBOARDUPDATE: {
        if (g_settings.clipboardWatch) readClipboardAndMaybeAdd();
        return 0;
    }
    case WM_COPYDATA: {
        auto cds = reinterpret_cast<COPYDATASTRUCT*>(lp);
        if (!cds || cds->dwData != kHandoffMagic) return 0;
        if (cds->cbData == 0 || cds->cbData % sizeof(wchar_t) != 0) return 0;
        HandoffSpec s = deserializeHandoff(
            reinterpret_cast<const wchar_t*>(cds->lpData),
            cds->cbData / sizeof(wchar_t));
        dbgLog(L"WM_COPYDATA url=[%ls]", s.url.c_str());
        if (s.url.empty()) return 0;
        if (IsIconic(hwnd)) ShowWindow(hwnd, SW_RESTORE);
        ShowWindow(hwnd, SW_SHOW);
        SetForegroundWindow(hwnd);
        addDownloadEx(s);
        return TRUE;
    }
    case WM_DESTROY: {
        saveSettings();
        // Stop the scheduler tick before the rest of teardown.
        g_schedExitFlag.store(true);
        if (g_clipboardSubscribed) {
            RemoveClipboardFormatListener(hwnd);
            g_clipboardSubscribed = false;
        }
        removeTrayIcon();
        // Tell every worker to stop; detach so we don't block teardown.
        std::lock_guard<std::mutex> lk(g_itemsMu);
        for (auto& it : g_items) {
            if (it->dl)  it->dl->abort();
            if (it->vg)  it->vg->abort();
            if (it->ftp) it->ftp->abort();
            if (it->isYtDlp) {
                it->ytdlpAbort = true;
                if (it->ytdlpProc) TerminateProcess(it->ytdlpProc, 1);
            }
            if (it->thread && it->thread->joinable()) it->thread->detach();
        }
        PostQuitMessage(0);
        return 0;
    }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

} // anon

// ---- entry point -----------------------------------------------------

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR cmdLine, int /*nCmdShow*/) {
    g_hInst = hInst;
    dbgLog(L"wWinMain ENTER cmdLine=[%ls]", cmdLine ? cmdLine : L"(null)");

    HandoffSpec cliSpec;
    bool haveCli = parseHandoffArgs(cmdLine, cliSpec);
    dbgLog(L"parsed haveCli=%d url=[%ls] filename=[%ls]",
           (int)haveCli, cliSpec.url.c_str(), cliSpec.filename.c_str());

    g_instanceMutex = CreateMutexW(nullptr, TRUE, kInstanceMutex);
    bool already = (GetLastError() == ERROR_ALREADY_EXISTS);
    if (already) {
        if (haveCli) {
            forwardHandoffToExistingInstance(cliSpec);
        } else {
            HWND target = FindWindowW(kClassName, nullptr);
            if (target) {
                if (IsIconic(target)) ShowWindow(target, SW_RESTORE);
                else                  ShowWindow(target, SW_SHOW);
                SetForegroundWindow(target);
            }
        }
        if (g_instanceMutex) CloseHandle(g_instanceMutex);
        return 0;
    }

    OleInitialize(nullptr);
    loadSettings();

    // Push the persisted proxy config + sites logins into the HTTP layer
    // before any request gets fired (update check, downloads).
    {
        ProxyConfig pc;
        pc.mode   = g_settings.proxyMode;
        pc.server = g_settings.proxyServer;
        pc.bypass = g_settings.proxyBypass;
        pc.user   = g_settings.proxyUser;
        pc.pass   = g_settings.proxyPass;
        setProxyConfig(pc);
    }
    setSiteLogins(g_settings.siteLogins);

    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = MainWndProc;
    wc.hInstance     = hInst;
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = kClassName;
    wc.hIcon         = LoadIconW(nullptr, IDI_APPLICATION);
    wc.hIconSm       = LoadIconW(nullptr, IDI_APPLICATION);
    RegisterClassExW(&wc);

    // Borderless top-level window: drop the OS title bar so only our custom
    // titlebar (rendered in the WebView) is visible. WS_THICKFRAME keeps OS
    // resize edges; WS_MINIMIZE/MAXIMIZE/SYSMENU keep taskbar interactions
    // (Win+Down minimize, snap, etc.). Window dragging is wired through the
    // JS bridge -> WM_NCLBUTTONDOWN(HTCAPTION).
    g_hwnd = CreateWindowExW(0, kClassName, kAppTitle,
        WS_POPUP | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_SYSMENU,
        g_settings.windowX, g_settings.windowY,
        g_settings.windowW, g_settings.windowH,
        nullptr, nullptr, hInst, nullptr);
    if (!g_hwnd) return 1;

    ShowWindow(g_hwnd, g_settings.windowMaximized ? SW_SHOWMAXIMIZED : SW_SHOWNORMAL);
    UpdateWindow(g_hwnd);

    initWebView2();

    // Kick off a one-shot background update check. Runs once per launch;
    // posts WM_APP_UPDATE_AVAIL back to the UI thread if a newer version
    // is published. Detached so we don't have to track the thread handle
    // through teardown -- worst case the HTTP fetch is a few KB and
    // returns within seconds.
    std::thread(updateCheckWorker).detach();

    // Long-lived scheduler tick. Handles Start/Stop edges of the configured
    // window, optional shutdown-when-done. Stops via g_schedExitFlag from
    // WM_DESTROY; detached so a hung shutdown.exe call can't block teardown.
    std::thread(schedulerWorker).detach();

    // v0.9.7: subscribe to the OS clipboard if enabled. WM_CLIPBOARDUPDATE
    // arrives whenever the clipboard contents change; the handler decides
    // whether the new text looks like a download URL.
    if (g_settings.clipboardWatch) applyClipboardWatch(true);

    // v0.9.9: install the tray icon. The minimize button hides the window
    // instead of going to the taskbar; double-click on tray restores.
    installTrayIcon();

    if (haveCli) {
        // Add now; if WebView is still initializing, the item event will be
        // queued and flushed once ready.
        addDownloadEx(cliSpec);
    }

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    g_webview.Reset();
    g_controller.Reset();
    OleUninitialize();
    if (g_instanceMutex) { ReleaseMutex(g_instanceMutex); CloseHandle(g_instanceMutex); }
    return (int)msg.wParam;
}
