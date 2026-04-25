// matata-gui.exe — Win32 GUI for the matata download manager.
//
// Pure Win32 + common controls; no MFC or ATL. Everything in one file to
// keep the v0.6 surface area obvious. Layered on top of the same
// Downloader / VideoGrabber library the CLI uses, so HLS / DASH / AES /
// live / bandwidth cap / auth etc. all work out of the box.

#define NOMINMAX
#define _WIN32_WINNT 0x0A00

#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <ole2.h>
#include <oleidl.h>

#include "matata/downloader.hpp"
#include "matata/ftp_downloader.hpp"
#include "matata/hls.hpp"
#include "matata/dash.hpp"
#include "matata/video.hpp"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <cwctype>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace matata;

// ---- constants --------------------------------------------------------

namespace {

constexpr wchar_t kClassName[]    = L"MatataMainWnd";
constexpr wchar_t kAddDlgClass[]  = L"MatataAddDlg";
constexpr wchar_t kAppTitle[]     = L"matata";

constexpr UINT    kTrayIconId     = 1;
constexpr UINT    WM_APP_TRAY       = WM_APP + 1;
constexpr UINT    WM_APP_PROGRESS   = WM_APP + 2;
constexpr UINT    WM_APP_COMPLETED  = WM_APP + 3;
constexpr UINT    WM_APP_CLIPURL    = WM_APP + 4;

enum : UINT {
    ID_MENU_ADD = 100, ID_MENU_PAUSE, ID_MENU_RESUME, ID_MENU_REMOVE,
    ID_MENU_STOP_ALL, ID_MENU_DELETE_COMPLETE,
    ID_MENU_OPEN_FILE, ID_MENU_OPEN_FOLDER, ID_MENU_COPY_URL,
    ID_MENU_OPTIONS, ID_MENU_SCHEDULER,
    ID_MENU_EXIT, ID_MENU_ABOUT,
    ID_MENU_CLIPWATCH,
    ID_TRAY_SHOW, ID_TRAY_EXIT,
    ID_LISTVIEW   = 200,
    ID_TOOLBAR    = 201,
    ID_STATUSBAR  = 202,
    ID_TREEVIEW   = 203,
    ID_ADD_URL    = 300, ID_ADD_NAME, ID_ADD_OK, ID_ADD_CANCEL
};

// Listview column indices (must match creation order).
enum : int {
    COL_FILE = 0, COL_Q, COL_SIZE, COL_STATUS, COL_TIMELEFT,
    COL_RATE, COL_LAST, COL_DESC,
    COL_COUNT
};

// Category kinds for the tree sidebar.
enum class Cat {
    All,           // "All Downloads"
    Compressed, Documents, Music, Programs, Video, Other,
    Unfinished,    // in-progress + failed/aborted
    Finished,      // done
    Queues,
};

// ---- data model ------------------------------------------------------

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

enum class ItemState { Queued, Running, Done, Err, Aborted };

struct GuiItem {
    int                          id = 0;
    std::wstring                 url;
    std::wstring                 filename;    // display / "Save as" name
    std::wstring                 outDir;
    std::wstring                 description;
    ItemState                    state = ItemState::Queued;
    int64_t                      total = -1;
    int64_t                      downloaded = 0;
    int64_t                      bps = 0;
    int                          activeConns = 0;
    int                          segments = 0;
    std::wstring                 message;
    std::wstring                 resultPath;
    bool                         isVideo = false;
    bool                         isFtp   = false;
    Cat                          category = Cat::Other;
    SYSTEMTIME                   startedAt{};     // local time
    SYSTEMTIME                   lastTryDate{};   // last progress / completion
    int                          imageIndex = 0;  // into g_imgList
    std::shared_ptr<Downloader>    dl;
    std::shared_ptr<VideoGrabber>  vg;
    std::shared_ptr<FtpDownloader> ftp;
    std::unique_ptr<std::thread>   thread;
};

HINSTANCE g_hInst;
HWND      g_hwnd;
HWND      g_hwndListView;
HWND      g_hwndStatus;
HWND      g_hwndToolbar;
HWND      g_hwndTree;
HMENU     g_hMenu;
HIMAGELIST g_imgList = nullptr;
Cat        g_filter = Cat::All;

// Extension -> image-list index, populated lazily via SHGetFileInfo on a
// synthesised filename with that extension.
std::map<std::wstring, int> g_iconCache;

// ---- persistent settings --------------------------------------------
//
// Stored under HKCU\Software\matata\Gui. Everything is optional and
// falls back to sensible defaults if the key is missing. We re-save
// the whole structure from saveSettings() on options changes and on
// window teardown so partial state is never written.

struct Settings {
    // General
    std::wstring outputDir;           // empty -> "<user's Downloads>" at run time
    int          connections   = 8;   // per HTTP download
    int64_t      bandwidthBps  = 0;   // 0 = unlimited
    bool         categorize    = false;
    bool         verifyChecksum = true;
    bool         clipboardWatch = true;

    // Scheduler (HH:MM; empty = disabled).
    std::wstring startAt;
    std::wstring stopAt;

    // Layout
    int          windowX = CW_USEDEFAULT, windowY = CW_USEDEFAULT;
    int          windowW = 980,           windowH = 600;
    bool         windowMaximized = false;
    int          treePaneWidth = 200;
    int          colWidth[COL_COUNT] = { 260, 24, 88, 100, 110, 110, 160, 200 };
};

Settings g_settings;

constexpr const wchar_t* kSettingsKey = L"Software\\matata\\Gui";

bool regReadStr(HKEY root, const wchar_t* subkey, const wchar_t* name,
                std::wstring& out) {
    HKEY h;
    if (RegOpenKeyExW(root, subkey, 0, KEY_READ, &h) != ERROR_SUCCESS) return false;
    DWORD type = 0, cb = 0;
    LONG r = RegQueryValueExW(h, name, nullptr, &type, nullptr, &cb);
    if (r != ERROR_SUCCESS || type != REG_SZ) { RegCloseKey(h); return false; }
    std::wstring v(cb / sizeof(wchar_t), L'\0');
    r = RegQueryValueExW(h, name, nullptr, nullptr, (BYTE*)v.data(), &cb);
    RegCloseKey(h);
    if (r != ERROR_SUCCESS) return false;
    while (!v.empty() && v.back() == L'\0') v.pop_back();
    out = std::move(v);
    return true;
}

bool regReadDword(HKEY root, const wchar_t* subkey, const wchar_t* name,
                  DWORD& out) {
    HKEY h;
    if (RegOpenKeyExW(root, subkey, 0, KEY_READ, &h) != ERROR_SUCCESS) return false;
    DWORD type = 0, v = 0, cb = sizeof(v);
    LONG r = RegQueryValueExW(h, name, nullptr, &type, (BYTE*)&v, &cb);
    RegCloseKey(h);
    if (r != ERROR_SUCCESS || type != REG_DWORD) return false;
    out = v;
    return true;
}

bool regReadQword(HKEY root, const wchar_t* subkey, const wchar_t* name,
                  ULONGLONG& out) {
    HKEY h;
    if (RegOpenKeyExW(root, subkey, 0, KEY_READ, &h) != ERROR_SUCCESS) return false;
    DWORD type = 0, cb = sizeof(ULONGLONG);
    ULONGLONG v = 0;
    LONG r = RegQueryValueExW(h, name, nullptr, &type, (BYTE*)&v, &cb);
    RegCloseKey(h);
    if (r != ERROR_SUCCESS || type != REG_QWORD) return false;
    out = v;
    return true;
}

void regWriteStr(HKEY root, const wchar_t* subkey, const wchar_t* name,
                 const std::wstring& val) {
    HKEY h;
    if (RegCreateKeyExW(root, subkey, 0, nullptr, 0, KEY_WRITE, nullptr, &h, nullptr)
        != ERROR_SUCCESS) return;
    RegSetValueExW(h, name, 0, REG_SZ,
                   (const BYTE*)val.c_str(),
                   (DWORD)((val.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(h);
}

void regWriteDword(HKEY root, const wchar_t* subkey, const wchar_t* name, DWORD val) {
    HKEY h;
    if (RegCreateKeyExW(root, subkey, 0, nullptr, 0, KEY_WRITE, nullptr, &h, nullptr)
        != ERROR_SUCCESS) return;
    RegSetValueExW(h, name, 0, REG_DWORD, (const BYTE*)&val, sizeof(val));
    RegCloseKey(h);
}

void regWriteQword(HKEY root, const wchar_t* subkey, const wchar_t* name, ULONGLONG val) {
    HKEY h;
    if (RegCreateKeyExW(root, subkey, 0, nullptr, 0, KEY_WRITE, nullptr, &h, nullptr)
        != ERROR_SUCCESS) return;
    RegSetValueExW(h, name, 0, REG_QWORD, (const BYTE*)&val, sizeof(val));
    RegCloseKey(h);
}

void loadSettings() {
    regReadStr(HKEY_CURRENT_USER, kSettingsKey, L"OutputDir",  g_settings.outputDir);
    regReadStr(HKEY_CURRENT_USER, kSettingsKey, L"StartAt",    g_settings.startAt);
    regReadStr(HKEY_CURRENT_USER, kSettingsKey, L"StopAt",     g_settings.stopAt);

    DWORD d; ULONGLONG q;
    if (regReadDword(HKEY_CURRENT_USER, kSettingsKey, L"Connections", d)
        && d > 0 && d <= 64) g_settings.connections = (int)d;
    if (regReadQword(HKEY_CURRENT_USER, kSettingsKey, L"BandwidthBps", q))
        g_settings.bandwidthBps = (int64_t)q;
    if (regReadDword(HKEY_CURRENT_USER, kSettingsKey, L"Categorize", d))
        g_settings.categorize = (d != 0);
    if (regReadDword(HKEY_CURRENT_USER, kSettingsKey, L"VerifyChecksum", d))
        g_settings.verifyChecksum = (d != 0);
    if (regReadDword(HKEY_CURRENT_USER, kSettingsKey, L"ClipboardWatch", d))
        g_settings.clipboardWatch = (d != 0);

    if (regReadDword(HKEY_CURRENT_USER, kSettingsKey, L"WindowX", d)) g_settings.windowX = (int)d;
    if (regReadDword(HKEY_CURRENT_USER, kSettingsKey, L"WindowY", d)) g_settings.windowY = (int)d;
    if (regReadDword(HKEY_CURRENT_USER, kSettingsKey, L"WindowW", d) && d > 300) g_settings.windowW = (int)d;
    if (regReadDword(HKEY_CURRENT_USER, kSettingsKey, L"WindowH", d) && d > 200) g_settings.windowH = (int)d;
    if (regReadDword(HKEY_CURRENT_USER, kSettingsKey, L"WindowMax", d)) g_settings.windowMaximized = (d != 0);
    if (regReadDword(HKEY_CURRENT_USER, kSettingsKey, L"TreePaneW", d) && d >= 100 && d <= 500)
        g_settings.treePaneWidth = (int)d;

    for (int i = 0; i < COL_COUNT; ++i) {
        wchar_t n[16]; _snwprintf_s(n, _TRUNCATE, L"Col%d", i);
        if (regReadDword(HKEY_CURRENT_USER, kSettingsKey, n, d) && d > 10 && d < 2000)
            g_settings.colWidth[i] = (int)d;
    }
}

void saveSettings() {
    regWriteStr  (HKEY_CURRENT_USER, kSettingsKey, L"OutputDir",      g_settings.outputDir);
    regWriteStr  (HKEY_CURRENT_USER, kSettingsKey, L"StartAt",        g_settings.startAt);
    regWriteStr  (HKEY_CURRENT_USER, kSettingsKey, L"StopAt",         g_settings.stopAt);
    regWriteDword(HKEY_CURRENT_USER, kSettingsKey, L"Connections",    (DWORD)g_settings.connections);
    regWriteQword(HKEY_CURRENT_USER, kSettingsKey, L"BandwidthBps",   (ULONGLONG)g_settings.bandwidthBps);
    regWriteDword(HKEY_CURRENT_USER, kSettingsKey, L"Categorize",     g_settings.categorize ? 1u : 0u);
    regWriteDword(HKEY_CURRENT_USER, kSettingsKey, L"VerifyChecksum", g_settings.verifyChecksum ? 1u : 0u);
    regWriteDword(HKEY_CURRENT_USER, kSettingsKey, L"ClipboardWatch", g_settings.clipboardWatch ? 1u : 0u);
    regWriteDword(HKEY_CURRENT_USER, kSettingsKey, L"WindowX",        (DWORD)g_settings.windowX);
    regWriteDword(HKEY_CURRENT_USER, kSettingsKey, L"WindowY",        (DWORD)g_settings.windowY);
    regWriteDword(HKEY_CURRENT_USER, kSettingsKey, L"WindowW",        (DWORD)g_settings.windowW);
    regWriteDword(HKEY_CURRENT_USER, kSettingsKey, L"WindowH",        (DWORD)g_settings.windowH);
    regWriteDword(HKEY_CURRENT_USER, kSettingsKey, L"WindowMax",      g_settings.windowMaximized ? 1u : 0u);
    regWriteDword(HKEY_CURRENT_USER, kSettingsKey, L"TreePaneW",      (DWORD)g_settings.treePaneWidth);
    for (int i = 0; i < COL_COUNT; ++i) {
        wchar_t n[16]; _snwprintf_s(n, _TRUNCATE, L"Col%d", i);
        regWriteDword(HKEY_CURRENT_USER, kSettingsKey, n, (DWORD)g_settings.colWidth[i]);
    }
}

NOTIFYICONDATAW g_nid{};
bool            g_trayInstalled = false;
bool            g_clipWatchEnabled = true;
std::wstring    g_lastClipboardUrl;

std::mutex                               g_itemsMu;
std::vector<std::unique_ptr<GuiItem>>    g_items;   // stable-addressed
std::atomic<int>                         g_nextId{1};

// ---- small helpers ---------------------------------------------------

// IDM-style size: integer byte counts under 1 KB, otherwise two decimals
// with a double space between the number and the unit ("32.23  GB").
std::wstring fmtBytesIdm(int64_t b) {
    if (b < 0) return L"";
    if (b < 1024) { wchar_t buf[32]; _snwprintf_s(buf, _TRUNCATE, L"%lld  B", (long long)b); return buf; }
    double v = (double)b;
    const wchar_t* u[] = { L"KB", L"MB", L"GB", L"TB" };
    int i = 0;
    v /= 1024.0;  // past the B threshold above
    while (v >= 1024.0 && i < 3) { v /= 1024.0; ++i; }
    wchar_t buf[32];
    _snwprintf_s(buf, _TRUNCATE, L"%.2f  %s", v, u[i]);
    return buf;
}

// Short bytes-per-second ("X.XX MB/sec"), empty when unknown / stopped.
std::wstring fmtRateIdm(int64_t bps) {
    if (bps <= 0) return L"";
    std::wstring s = fmtBytesIdm(bps);
    return s + L"/sec";
}

// "3 hour(s) 34 min" / "42 min 4 sec" / "5 sec" / empty.
std::wstring fmtTimeLeft(int64_t total, int64_t done, int64_t bps) {
    if (bps <= 0 || total <= 0 || done >= total) return L"";
    int64_t remaining = (total - done) / bps;
    if (remaining <= 0) return L"";
    int h = (int)(remaining / 3600);
    int m = (int)((remaining % 3600) / 60);
    int s = (int)(remaining % 60);
    wchar_t buf[64];
    if (h > 0)
        _snwprintf_s(buf, _TRUNCATE, L"%d hour(s) %d min", h, m);
    else if (m > 0)
        _snwprintf_s(buf, _TRUNCATE, L"%d min %d sec", m, s);
    else
        _snwprintf_s(buf, _TRUNCATE, L"%d sec", s);
    return buf;
}

// "Jan 23 18:27:04 2026" (locale-independent — we always want English
// month abbreviations, matching IDM).
std::wstring fmtIdmDate(const SYSTEMTIME& st) {
    if (st.wYear == 0) return L"";
    static const wchar_t* months[] = {
        L"Jan", L"Feb", L"Mar", L"Apr", L"May", L"Jun",
        L"Jul", L"Aug", L"Sep", L"Oct", L"Nov", L"Dec"
    };
    wchar_t buf[64];
    int m = st.wMonth >= 1 && st.wMonth <= 12 ? st.wMonth - 1 : 0;
    _snwprintf_s(buf, _TRUNCATE, L"%s %02d %02d:%02d:%02d %04d",
                 months[m], st.wDay,
                 st.wHour, st.wMinute, st.wSecond, st.wYear);
    return buf;
}

std::wstring fmtStatusIdm(const GuiItem& it) {
    switch (it.state) {
    case ItemState::Queued:  return L"Queued";
    case ItemState::Running:
        if (it.total > 0) {
            wchar_t b[16];
            _snwprintf_s(b, _TRUNCATE, L"%.2f%%",
                         (double)it.downloaded * 100.0 / (double)it.total);
            return b;
        }
        return L"Downloading";
    case ItemState::Done:    return L"Complete";
    case ItemState::Err:     return L"Error";
    case ItemState::Aborted: return L"Paused";
    }
    return L"";
}

// Infer the IDM-style category from a filename / URL tail.
Cat categoryFromExt(const std::wstring& nameOrUrl) {
    // Peel off any query string.
    std::wstring s = nameOrUrl;
    auto q = s.find(L'?');
    if (q != std::wstring::npos) s.resize(q);
    // Take the tail after the last slash.
    auto sl = s.find_last_of(L"/\\");
    if (sl != std::wstring::npos) s = s.substr(sl + 1);
    auto dot = s.find_last_of(L'.');
    if (dot == std::wstring::npos) return Cat::Other;
    std::wstring ext = s.substr(dot + 1);
    for (auto& c : ext) c = (wchar_t)towlower(c);

    const std::wstring video[]    = { L"mp4", L"mkv", L"avi", L"mov", L"webm",
                                      L"flv", L"wmv", L"mpg", L"mpeg", L"m4v",
                                      L"ts", L"m3u8", L"mpd" };
    const std::wstring music[]    = { L"mp3", L"flac", L"wav", L"m4a", L"aac",
                                      L"ogg", L"opus", L"wma" };
    const std::wstring docs[]     = { L"pdf", L"doc", L"docx", L"xls", L"xlsx",
                                      L"ppt", L"pptx", L"txt", L"rtf", L"epub",
                                      L"mobi" };
    const std::wstring archive[]  = { L"zip", L"rar", L"7z", L"tar", L"gz",
                                      L"bz2", L"xz", L"iso", L"cab" };
    const std::wstring programs[] = { L"exe", L"msi", L"apk", L"dmg", L"pkg",
                                      L"deb", L"rpm", L"appimage" };
    auto contains = [&](const std::wstring* arr, size_t n) {
        for (size_t i = 0; i < n; ++i) if (arr[i] == ext) return true;
        return false;
    };
    if (contains(video,    sizeof(video)/sizeof(video[0])))       return Cat::Video;
    if (contains(music,    sizeof(music)/sizeof(music[0])))       return Cat::Music;
    if (contains(docs,     sizeof(docs)/sizeof(docs[0])))         return Cat::Documents;
    if (contains(archive,  sizeof(archive)/sizeof(archive[0])))   return Cat::Compressed;
    if (contains(programs, sizeof(programs)/sizeof(programs[0]))) return Cat::Programs;
    return Cat::Other;
}

// Lookup-or-populate an icon for this extension. Uses the shell's
// associated-icon lookup with SHGFI_USEFILEATTRIBUTES so we don't need the
// file to actually exist.
int iconForName(const std::wstring& nameOrUrl) {
    std::wstring s = nameOrUrl;
    auto q = s.find(L'?'); if (q != std::wstring::npos) s.resize(q);
    auto sl = s.find_last_of(L"/\\"); if (sl != std::wstring::npos) s = s.substr(sl + 1);
    auto dot = s.find_last_of(L'.');
    std::wstring ext = (dot == std::wstring::npos) ? L"" : s.substr(dot);
    for (auto& c : ext) c = (wchar_t)towlower(c);
    if (ext.empty()) ext = L".bin";

    auto it = g_iconCache.find(ext);
    if (it != g_iconCache.end()) return it->second;

    std::wstring probe = L"x" + ext;
    SHFILEINFOW info{};
    DWORD_PTR h = SHGetFileInfoW(probe.c_str(), FILE_ATTRIBUTE_NORMAL,
                                 &info, sizeof(info),
                                 SHGFI_ICON | SHGFI_SMALLICON |
                                 SHGFI_USEFILEATTRIBUTES);
    if (!h || !info.hIcon) {
        g_iconCache[ext] = 0;
        return 0;
    }
    int idx = ImageList_AddIcon(g_imgList, info.hIcon);
    DestroyIcon(info.hIcon);
    if (idx < 0) idx = 0;
    g_iconCache[ext] = idx;
    return idx;
}

GuiItem* findItem(int id) {  // caller holds g_itemsMu
    for (auto& p : g_items) if (p->id == id) return p.get();
    return nullptr;
}

int listRowForId(int id) {
    LVFINDINFOW fi{};
    fi.flags = LVFI_PARAM;
    fi.lParam = (LPARAM)id;
    return ListView_FindItem(g_hwndListView, -1, &fi);
}

void setRowText(int row, int col, const std::wstring& s) {
    ListView_SetItemText(g_hwndListView, row, col, (LPWSTR)s.c_str());
}

// Returns true if `it` should be visible given the current sidebar filter.
bool matchesFilter(const GuiItem& it) {
    switch (g_filter) {
    case Cat::All:         return true;
    case Cat::Unfinished:  return it.state == ItemState::Running
                               || it.state == ItemState::Queued
                               || it.state == ItemState::Err
                               || it.state == ItemState::Aborted;
    case Cat::Finished:    return it.state == ItemState::Done;
    case Cat::Queues:      return it.state == ItemState::Queued;
    default:               return it.category == g_filter;
    }
}

std::wstring displayName(const GuiItem& it) {
    if (!it.filename.empty()) return it.filename;
    // Fall back to the tail of the URL.
    std::wstring u = it.url;
    auto q = u.find(L'?'); if (q != std::wstring::npos) u.resize(q);
    auto sl = u.find_last_of(L"/\\");
    if (sl != std::wstring::npos) u = u.substr(sl + 1);
    return u.empty() ? it.url : u;
}

void renderRow(int row, const GuiItem& it) {
    // Update the item's icon.
    LVITEMW lv{};
    lv.mask     = LVIF_IMAGE | LVIF_TEXT;
    lv.iItem    = row;
    lv.iSubItem = 0;
    std::wstring name = displayName(it);
    lv.pszText = (LPWSTR)name.c_str();
    lv.iImage  = it.imageIndex;
    ListView_SetItem(g_hwndListView, &lv);

    setRowText(row, COL_Q,        L"");
    setRowText(row, COL_SIZE,     fmtBytesIdm(it.total));
    setRowText(row, COL_STATUS,   fmtStatusIdm(it));
    setRowText(row, COL_TIMELEFT, it.state == ItemState::Running
                                   ? fmtTimeLeft(it.total, it.downloaded, it.bps)
                                   : std::wstring());
    setRowText(row, COL_RATE,     it.state == ItemState::Running
                                   ? fmtRateIdm(it.bps)
                                   : std::wstring());
    setRowText(row, COL_LAST,     fmtIdmDate(it.lastTryDate));
    setRowText(row, COL_DESC,     it.description);
}

void insertRowForItem(GuiItem& it) {
    if (!matchesFilter(it)) return;
    std::wstring name = displayName(it);
    it.imageIndex = iconForName(name);

    LVITEMW lv{};
    lv.mask  = LVIF_TEXT | LVIF_PARAM | LVIF_IMAGE;
    lv.iItem = ListView_GetItemCount(g_hwndListView);
    lv.iSubItem = 0;
    lv.pszText = (LPWSTR)name.c_str();
    lv.lParam  = (LPARAM)it.id;
    lv.iImage  = it.imageIndex;
    int row = ListView_InsertItem(g_hwndListView, &lv);
    if (row >= 0) renderRow(row, it);
}

// Rebuild the entire listview (used when the filter changes).
void rebuildListView() {
    ListView_DeleteAllItems(g_hwndListView);
    std::lock_guard<std::mutex> lk(g_itemsMu);
    for (auto& p : g_items) {
        if (!matchesFilter(*p)) continue;
        std::wstring name = displayName(*p);
        if (p->imageIndex == 0) p->imageIndex = iconForName(name);
        LVITEMW lv{};
        lv.mask  = LVIF_TEXT | LVIF_PARAM | LVIF_IMAGE;
        lv.iItem = ListView_GetItemCount(g_hwndListView);
        lv.pszText = (LPWSTR)name.c_str();
        lv.lParam  = (LPARAM)p->id;
        lv.iImage  = p->imageIndex;
        int row = ListView_InsertItem(g_hwndListView, &lv);
        if (row >= 0) renderRow(row, *p);
    }
}

void updateStatusBar() {
    std::lock_guard<std::mutex> lk(g_itemsMu);
    int running = 0, done = 0, total = (int)g_items.size();
    int64_t totalBps = 0;
    for (auto& p : g_items) {
        if (p->state == ItemState::Running) { ++running; totalBps += p->bps; }
        else if (p->state == ItemState::Done) ++done;
    }
    wchar_t b[256];
    _snwprintf_s(b, _TRUNCATE,
        L"  %d item%ls  |  %d running  |  %d done  |  %ls",
        total, total == 1 ? L"" : L"s",
        running, done,
        totalBps > 0 ? fmtRateIdm(totalBps).c_str() : L"0 B/sec");
    SendMessageW(g_hwndStatus, SB_SETTEXT, 0, (LPARAM)b);
}

// ---- download orchestration -----------------------------------------

bool looksLikeVideoUrl(const std::wstring& url) {
    return looksLikeHlsUrl(url) || looksLikeDashUrl(url);
}

bool looksLikeFtpUrl(const std::wstring& url) {
    return (url.size() > 6 && _wcsnicmp(url.c_str(), L"ftp://",  6) == 0)
        || (url.size() > 7 && _wcsnicmp(url.c_str(), L"ftps://", 7) == 0);
}

void workerRun(int id) {
    std::shared_ptr<Downloader>    dl;
    std::shared_ptr<VideoGrabber>  vg;
    std::shared_ptr<FtpDownloader> ftp;
    std::wstring                 urlSnap;
    {
        std::lock_guard<std::mutex> lk(g_itemsMu);
        GuiItem* it = findItem(id);
        if (!it) return;
        dl  = it->dl;
        vg  = it->vg;
        ftp = it->ftp;
        urlSnap = it->url;
    }

    auto httpProgress = [id](const DownloadProgress& p) {
        auto* msg = new ProgressMsg{ id, p.total, p.downloaded,
                                     p.bytesPerSec, p.activeConns, p.segments };
        if (!PostMessageW(g_hwnd, WM_APP_PROGRESS, 0, (LPARAM)msg))
            delete msg;
    };
    auto videoProgress = [id](const VideoProgress& p) {
        auto* msg = new ProgressMsg{ id, (int64_t)p.segmentsTotal,
                                     (int64_t)p.segmentsDone,
                                     p.bytesPerSec, 0, p.segmentsTotal };
        // For video rows, "total" / "downloaded" are segments counts; the
        // ListView render handles either. We also stash bytes into `bps`
        // so the rate column looks right.
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

// Forward declarations used across sections below.
bool splitHHMM(const std::wstring& s, int& hh, int& mm);

// If the category-routing option is on AND the user didn't provide an
// explicit outDir, append an IDM-style subfolder ("Video", "Music", etc)
// to the configured default. Returns the final outDir to pass to the
// downloader.
std::wstring resolveOutDir(const std::wstring& perItemDir,
                           const std::wstring& nameOrUrl) {
    std::wstring base = perItemDir.empty() ? g_settings.outputDir : perItemDir;
    if (!g_settings.categorize) return base;
    if (base.empty()) return base;  // no base to hang a subfolder off
    const wchar_t* sub = L"";
    switch (categoryFromExt(nameOrUrl)) {
        case Cat::Compressed: sub = L"Archives";   break;
        case Cat::Documents:  sub = L"Documents";  break;
        case Cat::Music:      sub = L"Music";      break;
        case Cat::Programs:   sub = L"Programs";   break;
        case Cat::Video:      sub = L"Video";      break;
        default:              sub = L"Other";      break;
    }
    std::wstring out = base;
    if (!out.empty() && out.back() != L'\\' && out.back() != L'/')
        out.push_back(L'\\');
    out += sub;
    return out;
}

// Turn "HH:MM" (local today) into a unix epoch; returns 0 if unset /
// malformed. If the target is already in the past today, it rolls to
// tomorrow (same semantics as the CLI's --start-at / --stop-at).
int64_t scheduleToEpoch(const std::wstring& hhmm) {
    int h = 0, m = 0;
    if (!splitHHMM(hhmm, h, m)) return 0;
    std::time_t now = std::time(nullptr);
    std::tm t{};
    if (localtime_s(&t, &now) != 0) return 0;
    t.tm_hour = h; t.tm_min = m; t.tm_sec = 0;
    std::time_t target = std::mktime(&t);
    if (target == (std::time_t)-1) return 0;
    if (target <= now) target += 24 * 3600;
    return (int64_t)target;
}

// ---- handoff from extension / second instance ------------------------

struct HandoffSpec {
    std::wstring url;
    std::wstring filename;
    std::wstring outDir;
    std::wstring referer;
    std::wstring cookie;
    std::wstring userAgent;
};

// WM_COPYDATA magic so we don't accidentally accept payloads from unrelated
// apps that happen to find our window class.
constexpr ULONG_PTR kHandoffMagic    = 0x4D415441; // 'MATA'
constexpr wchar_t   kInstanceMutex[] = L"Local\\com.matata.gui.singleton";

std::wstring serializeHandoff(const HandoffSpec& s) {
    std::wstring out;
    auto put = [&](const wchar_t* key, const std::wstring& val) {
        if (val.empty()) return;
        out.append(key);
        out.push_back(L'\0');
        out.append(val);
        out.push_back(L'\0');
    };
    put(L"url",       s.url);
    put(L"filename",  s.filename);
    put(L"outDir",    s.outDir);
    put(L"referer",   s.referer);
    put(L"cookie",    s.cookie);
    put(L"userAgent", s.userAgent);
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
        ++i; // skip NUL
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
        else if (out.url.empty()) {
            // Bare URL / matata:// URL (backward compat).
            if (a.size() > 9 && a.compare(0, 9, L"matata://") == 0)
                out.url = a.substr(9);
            else if (a.compare(0, 7, L"http://")  == 0 ||
                     a.compare(0, 8, L"https://") == 0 ||
                     a.compare(0, 6, L"ftp://")   == 0 ||
                     a.compare(0, 7, L"ftps://")  == 0)
                out.url = a;
        }
    }
    LocalFree(argv);
    // Trim stray quotes left over from odd callers.
    auto trimQuotes = [](std::wstring& s) {
        while (!s.empty() && (s.front() == L'"' || s.front() == L' ')) s.erase(s.begin());
        while (!s.empty() && (s.back()  == L'"' || s.back()  == L' ')) s.pop_back();
    };
    trimQuotes(out.url);
    trimQuotes(out.filename);
    trimQuotes(out.outDir);
    trimQuotes(out.referer);
    trimQuotes(out.cookie);
    trimQuotes(out.userAgent);
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

    // Nudge the existing window forward so the user sees it come up.
    if (IsIconic(target)) ShowWindow(target, SW_RESTORE);
    ShowWindow(target, SW_SHOW);
    SetForegroundWindow(target);

    DWORD_PTR result = 0;
    SendMessageTimeoutW(target, WM_COPYDATA, 0, (LPARAM)&cds,
                        SMTO_ABORTIFHUNG, 5000, &result);
    return true;
}

int addDownload(const std::wstring& url, const std::wstring& filename,
                const std::wstring& outDir,
                const std::vector<std::pair<std::wstring,std::wstring>>& headers = {}) {
    if (url.empty()) return 0;
    auto it = std::make_unique<GuiItem>();
    it->id       = g_nextId.fetch_add(1);
    it->url      = url;
    it->filename = filename;
    it->outDir   = resolveOutDir(outDir, filename.empty() ? url : filename);
    it->isVideo  = looksLikeVideoUrl(url);
    it->isFtp    = looksLikeFtpUrl(url);
    it->category = categoryFromExt(filename.empty() ? url : filename);
    GetLocalTime(&it->startedAt);
    it->lastTryDate = it->startedAt;

    // Honor the global Scheduler: if start-at is set in the future, mark
    // the item Queued and delay spawning the worker.
    int64_t startAt = scheduleToEpoch(g_settings.startAt);
    int64_t now     = (int64_t)std::time(nullptr);
    bool delayed = (startAt > now && startAt - now < 24 * 3600);
    it->state    = delayed ? ItemState::Queued : ItemState::Running;

    if (it->isVideo) {
        VideoOptions vo;
        vo.outputDir  = it->outDir;
        vo.outputName = filename;
        vo.quality    = L"best";
        vo.headers    = headers;
        it->vg = std::make_shared<VideoGrabber>(url, vo);
    } else if (it->isFtp) {
        DownloadOptions dlo;
        dlo.outputDir    = it->outDir;
        dlo.outputName   = filename;
        dlo.bandwidthBps = g_settings.bandwidthBps;
        dlo.headers      = headers;
        it->ftp = std::make_shared<FtpDownloader>(url, dlo);
    } else {
        DownloadOptions dlo;
        dlo.outputDir      = it->outDir;
        dlo.outputName     = filename;
        dlo.connections    = g_settings.connections;
        dlo.bandwidthBps   = g_settings.bandwidthBps;
        dlo.verifyChecksum = g_settings.verifyChecksum;
        dlo.headers        = headers;
        it->dl = std::make_shared<Downloader>(url, dlo);
    }

    int id = it->id;
    int64_t waitSec = delayed ? (startAt - now) : 0;
    {
        std::lock_guard<std::mutex> lk(g_itemsMu);
        g_items.push_back(std::move(it));
        GuiItem* p = g_items.back().get();
        insertRowForItem(*p);
        if (delayed) {
            // Spawn a tiny timer thread that sleeps until the schedule
            // window opens, then promotes the item to Running and kicks
            // off the worker.
            p->thread.reset(new std::thread([id, waitSec]() {
                for (int64_t slept = 0; slept < waitSec; slept += 1) {
                    Sleep(1000);
                    // Early-exit if the item was removed while waiting.
                    std::lock_guard<std::mutex> lk2(g_itemsMu);
                    if (!findItem(id)) return;
                }
                {
                    std::lock_guard<std::mutex> lk2(g_itemsMu);
                    GuiItem* it2 = findItem(id);
                    if (!it2) return;
                    it2->state = ItemState::Running;
                    int row = listRowForId(id);
                    if (row >= 0) renderRow(row, *it2);
                }
                // Run the download inline on this same thread.
                workerRun(id);
            }));
        } else {
            p->thread.reset(new std::thread(workerRun, id));
        }
    }
    updateStatusBar();
    return id;
}

int addDownloadFromHandoff(const HandoffSpec& s) {
    std::vector<std::pair<std::wstring,std::wstring>> headers;
    if (!s.referer.empty())   headers.push_back({L"Referer",    s.referer});
    if (!s.cookie.empty())    headers.push_back({L"Cookie",     s.cookie});
    if (!s.userAgent.empty()) headers.push_back({L"User-Agent", s.userAgent});
    return addDownload(s.url, s.filename, s.outDir, headers);
}

void abortItem(int id) {
    std::lock_guard<std::mutex> lk(g_itemsMu);
    GuiItem* it = findItem(id);
    if (!it) return;
    if (it->dl)  it->dl->abort();
    if (it->vg)  it->vg->abort();
    if (it->ftp) it->ftp->abort();
}

void restartItem(int id) {
    std::wstring url, name, outDir;
    {
        std::lock_guard<std::mutex> lk(g_itemsMu);
        GuiItem* it = findItem(id);
        if (!it) return;
        if (it->state == ItemState::Running) return;
        if (it->thread && it->thread->joinable()) it->thread->join();
        url = it->url; name = it->filename; outDir = it->outDir;
    }
    int row = listRowForId(id);
    if (row >= 0) ListView_DeleteItem(g_hwndListView, row);
    {
        std::lock_guard<std::mutex> lk(g_itemsMu);
        auto pos = std::remove_if(g_items.begin(), g_items.end(),
            [id](const std::unique_ptr<GuiItem>& p){ return p->id == id; });
        g_items.erase(pos, g_items.end());
    }
    addDownload(url, name, outDir);
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
        th = std::move(it->thread);
    }
    if (th && th->joinable()) th->join();

    int row = listRowForId(id);
    if (row >= 0) ListView_DeleteItem(g_hwndListView, row);
    {
        std::lock_guard<std::mutex> lk(g_itemsMu);
        auto pos = std::remove_if(g_items.begin(), g_items.end(),
            [id](const std::unique_ptr<GuiItem>& p){ return p->id == id; });
        g_items.erase(pos, g_items.end());
    }
    updateStatusBar();
}

// ---- Add URL dialog (custom modal window, no .rc needed) -------------

struct AddDlgResult {
    bool         ok = false;
    std::wstring url;
    std::wstring filename;
    std::wstring outDir;
};

LRESULT CALLBACK AddDlgWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* out = (AddDlgResult*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    switch (msg) {
    case WM_CREATE: {
        auto cs = (CREATESTRUCTW*)lp;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)cs->lpCreateParams);
        HFONT font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);

        auto mkLabel = [&](const wchar_t* t, int x, int y, int w, int h) {
            HWND ctl = CreateWindowExW(0, L"STATIC", t, WS_CHILD | WS_VISIBLE,
                x, y, w, h, hwnd, nullptr, g_hInst, nullptr);
            SendMessageW(ctl, WM_SETFONT, (WPARAM)font, TRUE);
            return ctl;
        };
        auto mkEdit = [&](int id, int x, int y, int w, int h) {
            HWND e = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                x, y, w, h, hwnd, (HMENU)(UINT_PTR)id, g_hInst, nullptr);
            SendMessageW(e, WM_SETFONT, (WPARAM)font, TRUE);
            return e;
        };
        auto mkBtn = [&](const wchar_t* t, int id, int x, int y, int w, int h, bool def) {
            DWORD s = WS_CHILD | WS_VISIBLE | WS_TABSTOP
                    | (def ? BS_DEFPUSHBUTTON : BS_PUSHBUTTON);
            HWND b = CreateWindowExW(0, L"BUTTON", t, s,
                x, y, w, h, hwnd, (HMENU)(UINT_PTR)id, g_hInst, nullptr);
            SendMessageW(b, WM_SETFONT, (WPARAM)font, TRUE);
            return b;
        };

        mkLabel(L"URL:", 12, 14, 60, 18);
        HWND urlEdit = mkEdit(ID_ADD_URL, 78, 12, 460, 22);
        mkLabel(L"Save as (optional):", 12, 48, 150, 18);
        mkEdit(ID_ADD_NAME, 170, 46, 368, 22);
        mkBtn(L"Start", ID_ADD_OK, 380, 84, 76, 26, true);
        mkBtn(L"Cancel", ID_ADD_CANCEL, 462, 84, 76, 26, false);

        // Prefill URL from clipboard if it looks like one.
        if (OpenClipboard(hwnd)) {
            HANDLE h = GetClipboardData(CF_UNICODETEXT);
            if (h) {
                auto* p = (const wchar_t*)GlobalLock(h);
                if (p && (wcsncmp(p, L"http://", 7) == 0 ||
                          wcsncmp(p, L"https://", 8) == 0)) {
                    SetWindowTextW(urlEdit, p);
                }
                GlobalUnlock(h);
            }
            CloseClipboard();
        }
        SetFocus(urlEdit);
        return 0;
    }
    case WM_COMMAND: {
        UINT id = LOWORD(wp);
        if (id == ID_ADD_OK) {
            wchar_t u[2048], n[512];
            GetDlgItemTextW(hwnd, ID_ADD_URL, u, 2048);
            GetDlgItemTextW(hwnd, ID_ADD_NAME, n, 512);
            if (out) {
                out->ok = (u[0] != 0);
                out->url = u;
                out->filename = n;
            }
            DestroyWindow(hwnd);
            return 0;
        }
        if (id == ID_ADD_CANCEL) { DestroyWindow(hwnd); return 0; }
        break;
    }
    case WM_CLOSE:   DestroyWindow(hwnd); return 0;
    // No PostQuitMessage in modal-dialog WM_DESTROY: the dialog's own
    // nested GetMessage loop exits via the IsWindow(dlg) check after
    // DestroyWindow. PostQuitMessage leaks WM_QUIT into the outer app
    // loop on some Windows versions and short-circuits orderly
    // shutdown, which killed in-flight worker threads.
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

AddDlgResult showAddUrlDialog(HWND owner) {
    static bool classRegistered = false;
    if (!classRegistered) {
        WNDCLASSEXW wc = { sizeof(wc) };
        wc.lpfnWndProc   = AddDlgWndProc;
        wc.hInstance     = g_hInst;
        wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = kAddDlgClass;
        RegisterClassExW(&wc);
        classRegistered = true;
    }
    AddDlgResult out;
    HWND dlg = CreateWindowExW(
        WS_EX_DLGMODALFRAME | WS_EX_TOPMOST,
        kAddDlgClass, L"Add download",
        WS_CAPTION | WS_SYSMENU | WS_VISIBLE,
        CW_USEDEFAULT, CW_USEDEFAULT, 570, 160,
        owner, nullptr, g_hInst, &out);
    if (!dlg) return out;
    EnableWindow(owner, FALSE);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        if (IsDialogMessageW(dlg, &msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
        if (!IsWindow(dlg)) break;
    }
    EnableWindow(owner, TRUE);
    SetForegroundWindow(owner);
    return out;
}

// ---- OLE drop target ------------------------------------------------
//
// Accepts:
//   - CF_UNICODETEXT / CF_TEXT containing one or more http(s):// URLs
//     (one per line, or whitespace-separated)
//   - CF_HDROP file lists (queues each dropped file's full path as a
//     local URL — useful for .m3u8 / .mpd playlists opened from disk)

class MainDropTarget : public IDropTarget {
    LONG m_ref = 1;
public:
    // IUnknown
    ULONG STDMETHODCALLTYPE AddRef()  override { return InterlockedIncrement(&m_ref); }
    ULONG STDMETHODCALLTYPE Release() override {
        LONG n = InterlockedDecrement(&m_ref);
        if (n == 0) delete this;
        return (ULONG)n;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (iid == IID_IUnknown || iid == IID_IDropTarget) {
            *ppv = this; AddRef(); return S_OK;
        }
        *ppv = nullptr; return E_NOINTERFACE;
    }

    HRESULT STDMETHODCALLTYPE DragEnter(IDataObject* obj, DWORD, POINTL, DWORD* eff) override {
        *eff = accepts(obj) ? DROPEFFECT_COPY : DROPEFFECT_NONE;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DragOver(DWORD, POINTL, DWORD* eff) override {
        *eff = (*eff & DROPEFFECT_COPY) ? DROPEFFECT_COPY : DROPEFFECT_NONE;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DragLeave() override { return S_OK; }
    HRESULT STDMETHODCALLTYPE Drop(IDataObject* obj, DWORD, POINTL, DWORD* eff) override {
        std::vector<std::wstring> urls = extractUrls(obj);
        if (!urls.empty()) {
            ShowWindow(g_hwnd, SW_SHOW);
            SetForegroundWindow(g_hwnd);
            for (auto& u : urls) addDownload(u, L"", L"");
            *eff = DROPEFFECT_COPY;
        } else {
            *eff = DROPEFFECT_NONE;
        }
        return S_OK;
    }

private:
    static bool accepts(IDataObject* obj) {
        FORMATETC fmts[] = {
            { CF_UNICODETEXT, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL },
            { CF_TEXT,        nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL },
            { CF_HDROP,       nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL },
        };
        for (auto& f : fmts)
            if (obj->QueryGetData(&f) == S_OK) return true;
        return false;
    }

    static std::vector<std::wstring> extractUrls(IDataObject* obj) {
        std::vector<std::wstring> out;

        auto pushIfUrl = [&](std::wstring s) {
            size_t a = 0, b = s.size();
            while (a < b && iswspace(s[a])) ++a;
            while (b > a && iswspace(s[b-1])) --b;
            s = s.substr(a, b - a);
            if (s.empty()) return;
            if ((s.size() > 7 && s.compare(0, 7,  L"http://")  == 0) ||
                (s.size() > 8 && s.compare(0, 8,  L"https://") == 0) ||
                (s.size() > 9 && s.compare(0, 9,  L"matata://") == 0)) {
                if (s.size() > 9 && s.compare(0, 9, L"matata://") == 0)
                    s = s.substr(9);
                out.push_back(std::move(s));
            }
        };

        FORMATETC fUni = { CF_UNICODETEXT, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
        STGMEDIUM med{};
        if (obj->GetData(&fUni, &med) == S_OK) {
            auto* p = (const wchar_t*)GlobalLock(med.hGlobal);
            if (p) {
                std::wstring s(p);
                // Split on newlines (multi-URL paste).
                size_t i = 0;
                while (i < s.size()) {
                    size_t nl = s.find_first_of(L"\r\n", i);
                    if (nl == std::wstring::npos) nl = s.size();
                    pushIfUrl(s.substr(i, nl - i));
                    i = nl;
                    while (i < s.size() && (s[i] == L'\r' || s[i] == L'\n')) ++i;
                }
                GlobalUnlock(med.hGlobal);
            }
            ReleaseStgMedium(&med);
            if (!out.empty()) return out;
        }

        FORMATETC fHdrop = { CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
        if (obj->GetData(&fHdrop, &med) == S_OK) {
            HDROP hDrop = (HDROP)med.hGlobal;
            UINT n = DragQueryFileW(hDrop, 0xFFFFFFFF, nullptr, 0);
            for (UINT k = 0; k < n; ++k) {
                wchar_t path[MAX_PATH * 2];
                if (DragQueryFileW(hDrop, k, path, MAX_PATH * 2)) {
                    // Hand the file path straight in — Downloader + HLS /
                    // DASH paths all accept file:// URLs via WinHTTP's
                    // URL cracker. For the general case, treat the local
                    // path as the URL.
                    std::wstring fileUrl = L"file:///";
                    for (wchar_t* q = path; *q; ++q)
                        fileUrl.push_back(*q == L'\\' ? L'/' : *q);
                    out.push_back(std::move(fileUrl));
                }
            }
            ReleaseStgMedium(&med);
        }
        return out;
    }
};

MainDropTarget* g_dropTarget = nullptr;

// ---- Options dialog --------------------------------------------------

constexpr wchar_t kOptionsClass[] = L"MatataOptionsDlg";
enum {
    ID_OPT_OUTDIR = 400, ID_OPT_BROWSE, ID_OPT_CONN, ID_OPT_BW,
    ID_OPT_CATEG, ID_OPT_VERIFY, ID_OPT_CLIP,
    ID_OPT_OK, ID_OPT_CANCEL
};

LRESULT CALLBACK OptionsWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        HFONT font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
        auto mk = [&](const wchar_t* cls, const wchar_t* text, DWORD style,
                      int x, int y, int w, int h, int id, DWORD exStyle = 0) {
            HWND c = CreateWindowExW(exStyle, cls, text,
                WS_CHILD | WS_VISIBLE | style, x, y, w, h, hwnd,
                (HMENU)(UINT_PTR)id, g_hInst, nullptr);
            SendMessageW(c, WM_SETFONT, (WPARAM)font, TRUE);
            return c;
        };

        mk(L"STATIC", L"Default download folder:", 0,                          12, 14, 200, 18, 0);
        mk(L"EDIT",   g_settings.outputDir.c_str(), WS_TABSTOP | ES_AUTOHSCROLL,
                                                                               12, 34, 370, 22, ID_OPT_OUTDIR, WS_EX_CLIENTEDGE);
        mk(L"BUTTON", L"Browse...", BS_PUSHBUTTON | WS_TABSTOP,                390, 34, 96, 22, ID_OPT_BROWSE);

        mk(L"STATIC", L"Connections per download (1-32):", 0,                  12, 70, 220, 18, 0);
        mk(L"EDIT",   std::to_wstring(g_settings.connections).c_str(),
            WS_TABSTOP | ES_NUMBER,                                            240, 68, 60, 22, ID_OPT_CONN, WS_EX_CLIENTEDGE);

        mk(L"STATIC", L"Bandwidth cap (KB/sec, 0 = unlimited):", 0,            12, 102, 260, 18, 0);
        mk(L"EDIT",   std::to_wstring(g_settings.bandwidthBps / 1024).c_str(),
            WS_TABSTOP | ES_NUMBER,                                            280, 100, 100, 22, ID_OPT_BW, WS_EX_CLIENTEDGE);

        mk(L"BUTTON", L"Route downloads into per-type subfolders",
           BS_AUTOCHECKBOX | WS_TABSTOP,                                       12, 134, 320, 22, ID_OPT_CATEG);
        mk(L"BUTTON", L"Verify server-advertised checksum on finish",
           BS_AUTOCHECKBOX | WS_TABSTOP,                                       12, 160, 340, 22, ID_OPT_VERIFY);
        mk(L"BUTTON", L"Monitor clipboard for URLs",
           BS_AUTOCHECKBOX | WS_TABSTOP,                                       12, 186, 240, 22, ID_OPT_CLIP);

        CheckDlgButton(hwnd, ID_OPT_CATEG,  g_settings.categorize     ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(hwnd, ID_OPT_VERIFY, g_settings.verifyChecksum ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(hwnd, ID_OPT_CLIP,   g_settings.clipboardWatch ? BST_CHECKED : BST_UNCHECKED);

        mk(L"BUTTON", L"OK",     BS_DEFPUSHBUTTON | WS_TABSTOP, 320, 222, 80, 26, ID_OPT_OK);
        mk(L"BUTTON", L"Cancel", BS_PUSHBUTTON    | WS_TABSTOP, 406, 222, 80, 26, ID_OPT_CANCEL);
        return 0;
    }
    case WM_COMMAND: {
        UINT id = LOWORD(wp);
        if (id == ID_OPT_BROWSE) {
            BROWSEINFOW bi{};
            bi.hwndOwner = hwnd;
            bi.lpszTitle = L"Choose default download folder";
            bi.ulFlags   = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
            LPITEMIDLIST pidl = SHBrowseForFolderW(&bi);
            if (pidl) {
                wchar_t path[MAX_PATH];
                if (SHGetPathFromIDListW(pidl, path))
                    SetDlgItemTextW(hwnd, ID_OPT_OUTDIR, path);
                CoTaskMemFree(pidl);
            }
            return 0;
        }
        if (id == ID_OPT_OK) {
            wchar_t buf[MAX_PATH];
            GetDlgItemTextW(hwnd, ID_OPT_OUTDIR, buf, MAX_PATH);
            g_settings.outputDir = buf;

            GetDlgItemTextW(hwnd, ID_OPT_CONN, buf, 16);
            int c = _wtoi(buf);
            if (c < 1) c = 1; if (c > 32) c = 32;
            g_settings.connections = c;

            GetDlgItemTextW(hwnd, ID_OPT_BW, buf, 32);
            int64_t kb = _wtoi64(buf);
            g_settings.bandwidthBps = kb > 0 ? kb * 1024 : 0;

            g_settings.categorize     = IsDlgButtonChecked(hwnd, ID_OPT_CATEG)  == BST_CHECKED;
            g_settings.verifyChecksum = IsDlgButtonChecked(hwnd, ID_OPT_VERIFY) == BST_CHECKED;
            g_settings.clipboardWatch = IsDlgButtonChecked(hwnd, ID_OPT_CLIP)   == BST_CHECKED;

            saveSettings();
            CheckMenuItem(g_hMenu, ID_MENU_CLIPWATCH,
                MF_BYCOMMAND | (g_settings.clipboardWatch ? MF_CHECKED : MF_UNCHECKED));
            DestroyWindow(hwnd);
            return 0;
        }
        if (id == ID_OPT_CANCEL) { DestroyWindow(hwnd); return 0; }
        break;
    }
    case WM_CLOSE:   DestroyWindow(hwnd); return 0;
    // Dialog WM_DESTROY deliberately does NOT PostQuitMessage -- see
    // AddDlgWndProc for the rationale.
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void showOptionsDialog(HWND owner) {
    static bool reg = false;
    if (!reg) {
        WNDCLASSEXW wc = { sizeof(wc) };
        wc.lpfnWndProc   = OptionsWndProc;
        wc.hInstance     = g_hInst;
        wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = kOptionsClass;
        RegisterClassExW(&wc);
        reg = true;
    }
    HWND dlg = CreateWindowExW(
        WS_EX_DLGMODALFRAME | WS_EX_TOPMOST,
        kOptionsClass, L"matata - Options",
        WS_CAPTION | WS_SYSMENU | WS_VISIBLE,
        CW_USEDEFAULT, CW_USEDEFAULT, 520, 300,
        owner, nullptr, g_hInst, nullptr);
    if (!dlg) return;
    EnableWindow(owner, FALSE);
    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0)) {
        if (IsDialogMessageW(dlg, &m)) continue;
        TranslateMessage(&m);
        DispatchMessageW(&m);
        if (!IsWindow(dlg)) break;
    }
    EnableWindow(owner, TRUE);
    SetForegroundWindow(owner);
}

// ---- Scheduler dialog ------------------------------------------------

constexpr wchar_t kSchedulerClass[] = L"MatataSchedulerDlg";
enum {
    ID_SCH_START_CHK = 420, ID_SCH_START_HH, ID_SCH_START_MM,
    ID_SCH_STOP_CHK,        ID_SCH_STOP_HH,  ID_SCH_STOP_MM,
    ID_SCH_OK, ID_SCH_CANCEL
};

// Parse "HH:MM" into hh and mm; returns false if malformed / empty.
bool splitHHMM(const std::wstring& s, int& hh, int& mm) {
    auto c = s.find(L':');
    if (c == std::wstring::npos) return false;
    try {
        hh = std::stoi(s.substr(0, c));
        mm = std::stoi(s.substr(c + 1));
    } catch (...) { return false; }
    return hh >= 0 && hh < 24 && mm >= 0 && mm < 60;
}

LRESULT CALLBACK SchedulerWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        HFONT font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
        auto mk = [&](const wchar_t* cls, const wchar_t* text, DWORD style,
                      int x, int y, int w, int h, int id, DWORD ex = 0) {
            HWND c = CreateWindowExW(ex, cls, text, WS_CHILD | WS_VISIBLE | style,
                x, y, w, h, hwnd, (HMENU)(UINT_PTR)id, g_hInst, nullptr);
            SendMessageW(c, WM_SETFONT, (WPARAM)font, TRUE);
            return c;
        };

        mk(L"STATIC",
           L"Hold new downloads until / stop starting new downloads after\n"
           L"(the engine's --start-at and --stop-at, applied to the GUI's queue).",
           0, 12, 12, 440, 36, 0);

        mk(L"BUTTON", L"Start queue at:", BS_AUTOCHECKBOX | WS_TABSTOP,
                                          12, 60, 130, 22, ID_SCH_START_CHK);
        mk(L"EDIT",   L"", WS_TABSTOP | ES_NUMBER, 150, 60, 40, 22, ID_SCH_START_HH, WS_EX_CLIENTEDGE);
        mk(L"STATIC", L":", 0, 192, 62, 10, 18, 0);
        mk(L"EDIT",   L"", WS_TABSTOP | ES_NUMBER, 204, 60, 40, 22, ID_SCH_START_MM, WS_EX_CLIENTEDGE);
        mk(L"STATIC", L"(24-hour, e.g. 22:30)", 0, 254, 64, 140, 18, 0);

        mk(L"BUTTON", L"Stop queue at:",  BS_AUTOCHECKBOX | WS_TABSTOP,
                                          12, 96, 130, 22, ID_SCH_STOP_CHK);
        mk(L"EDIT",   L"", WS_TABSTOP | ES_NUMBER, 150, 96, 40, 22, ID_SCH_STOP_HH, WS_EX_CLIENTEDGE);
        mk(L"STATIC", L":", 0, 192, 98, 10, 18, 0);
        mk(L"EDIT",   L"", WS_TABSTOP | ES_NUMBER, 204, 96, 40, 22, ID_SCH_STOP_MM, WS_EX_CLIENTEDGE);

        int hh = 0, mm = 0;
        if (splitHHMM(g_settings.startAt, hh, mm)) {
            CheckDlgButton(hwnd, ID_SCH_START_CHK, BST_CHECKED);
            SetDlgItemInt(hwnd, ID_SCH_START_HH, hh, FALSE);
            SetDlgItemInt(hwnd, ID_SCH_START_MM, mm, FALSE);
        }
        if (splitHHMM(g_settings.stopAt, hh, mm)) {
            CheckDlgButton(hwnd, ID_SCH_STOP_CHK, BST_CHECKED);
            SetDlgItemInt(hwnd, ID_SCH_STOP_HH, hh, FALSE);
            SetDlgItemInt(hwnd, ID_SCH_STOP_MM, mm, FALSE);
        }

        mk(L"BUTTON", L"OK",     BS_DEFPUSHBUTTON | WS_TABSTOP, 304, 148, 80, 26, ID_SCH_OK);
        mk(L"BUTTON", L"Cancel", BS_PUSHBUTTON    | WS_TABSTOP, 390, 148, 80, 26, ID_SCH_CANCEL);
        return 0;
    }
    case WM_COMMAND: {
        UINT id = LOWORD(wp);
        if (id == ID_SCH_OK) {
            auto read = [&](UINT hhId, UINT mmId, UINT chkId) -> std::wstring {
                if (IsDlgButtonChecked(hwnd, chkId) != BST_CHECKED) return L"";
                int h = (int)GetDlgItemInt(hwnd, hhId, nullptr, FALSE);
                int m = (int)GetDlgItemInt(hwnd, mmId, nullptr, FALSE);
                if (h < 0 || h > 23 || m < 0 || m > 59) return L"";
                wchar_t b[16]; _snwprintf_s(b, _TRUNCATE, L"%02d:%02d", h, m);
                return b;
            };
            g_settings.startAt = read(ID_SCH_START_HH, ID_SCH_START_MM, ID_SCH_START_CHK);
            g_settings.stopAt  = read(ID_SCH_STOP_HH,  ID_SCH_STOP_MM,  ID_SCH_STOP_CHK);
            saveSettings();
            DestroyWindow(hwnd);
            return 0;
        }
        if (id == ID_SCH_CANCEL) { DestroyWindow(hwnd); return 0; }
        break;
    }
    case WM_CLOSE:   DestroyWindow(hwnd); return 0;
    // Dialog WM_DESTROY deliberately does NOT PostQuitMessage -- see
    // AddDlgWndProc for the rationale.
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void showSchedulerDialog(HWND owner) {
    static bool reg = false;
    if (!reg) {
        WNDCLASSEXW wc = { sizeof(wc) };
        wc.lpfnWndProc   = SchedulerWndProc;
        wc.hInstance     = g_hInst;
        wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = kSchedulerClass;
        RegisterClassExW(&wc);
        reg = true;
    }
    HWND dlg = CreateWindowExW(
        WS_EX_DLGMODALFRAME | WS_EX_TOPMOST,
        kSchedulerClass, L"matata - Scheduler",
        WS_CAPTION | WS_SYSMENU | WS_VISIBLE,
        CW_USEDEFAULT, CW_USEDEFAULT, 500, 230,
        owner, nullptr, g_hInst, nullptr);
    if (!dlg) return;
    EnableWindow(owner, FALSE);
    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0)) {
        if (IsDialogMessageW(dlg, &m)) continue;
        TranslateMessage(&m);
        DispatchMessageW(&m);
        if (!IsWindow(dlg)) break;
    }
    EnableWindow(owner, TRUE);
    SetForegroundWindow(owner);
}

// ---- tray icon -------------------------------------------------------

void installTrayIcon(HWND hwnd) {
    if (g_trayInstalled) return;
    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd   = hwnd;
    g_nid.uID    = kTrayIconId;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_APP_TRAY;
    g_nid.hIcon  = LoadIconW(nullptr, IDI_APPLICATION);
    wcscpy_s(g_nid.szTip, L"matata");
    g_trayInstalled = Shell_NotifyIconW(NIM_ADD, &g_nid) == TRUE;
}

void removeTrayIcon() {
    if (g_trayInstalled) {
        Shell_NotifyIconW(NIM_DELETE, &g_nid);
        g_trayInstalled = false;
    }
}

void notifyTray(const wchar_t* title, const wchar_t* body) {
    if (!g_trayInstalled) return;
    NOTIFYICONDATAW n = g_nid;
    n.uFlags = NIF_INFO;
    n.dwInfoFlags = NIIF_INFO;
    wcscpy_s(n.szInfoTitle, title);
    wcscpy_s(n.szInfo, body);
    Shell_NotifyIconW(NIM_MODIFY, &n);
}

// ---- clipboard URL watcher ------------------------------------------

std::wstring readClipboardUrl(HWND owner) {
    std::wstring out;
    if (!OpenClipboard(owner)) return out;
    HANDLE h = GetClipboardData(CF_UNICODETEXT);
    if (h) {
        const wchar_t* p = (const wchar_t*)GlobalLock(h);
        if (p) {
            std::wstring s(p);
            // Trim leading/trailing whitespace.
            size_t a = 0, b = s.size();
            while (a < b && iswspace(s[a])) ++a;
            while (b > a && iswspace(s[b-1])) --b;
            s = s.substr(a, b - a);
            if ((s.size() > 7 && s.compare(0, 7, L"http://") == 0) ||
                (s.size() > 8 && s.compare(0, 8, L"https://") == 0)) {
                // Reject if there's whitespace (probably a paragraph, not a URL).
                if (s.find_first_of(L" \t\r\n") == std::wstring::npos)
                    out = s;
            }
        }
        GlobalUnlock(h);
    }
    CloseClipboard();
    return out;
}

// ---- window procedure ------------------------------------------------

void layoutChildren(HWND hwnd) {
    RECT rc; GetClientRect(hwnd, &rc);
    SendMessageW(g_hwndToolbar, TB_AUTOSIZE, 0, 0);
    SendMessageW(g_hwndStatus, WM_SIZE, 0, 0);

    RECT tb; GetClientRect(g_hwndToolbar, &tb);
    RECT sb; GetClientRect(g_hwndStatus, &sb);
    int top  = tb.bottom - tb.top;
    int bot  = sb.bottom - sb.top;
    int w    = rc.right  - rc.left;
    int h    = rc.bottom - rc.top - top - bot;
    int tree = 200;  // fixed sidebar width (IDM style)
    if (tree > w / 2) tree = w / 2;
    MoveWindow(g_hwndTree,     0,    top, tree,     h, TRUE);
    MoveWindow(g_hwndListView, tree, top, w - tree, h, TRUE);
}

void createToolbar(HWND parent) {
    g_hwndToolbar = CreateWindowExW(0, TOOLBARCLASSNAMEW, nullptr,
        WS_CHILD | WS_VISIBLE | TBSTYLE_FLAT | TBSTYLE_LIST,
        0, 0, 0, 0, parent, (HMENU)(UINT_PTR)ID_TOOLBAR, g_hInst, nullptr);
    SendMessageW(g_hwndToolbar, TB_BUTTONSTRUCTSIZE, sizeof(TBBUTTON), 0);
    SendMessageW(g_hwndToolbar, TB_SETEXTENDEDSTYLE, 0,
                 TBSTYLE_EX_MIXEDBUTTONS | TBSTYLE_EX_DRAWDDARROWS);

    TBBUTTON btns[] = {
        { I_IMAGENONE, ID_MENU_ADD,             TBSTATE_ENABLED, BTNS_SHOWTEXT | BTNS_AUTOSIZE, {0}, 0, (INT_PTR)L"Add URL" },
        { I_IMAGENONE, 0,                       TBSTATE_ENABLED, BTNS_SEP,                        {0}, 0, 0 },
        { I_IMAGENONE, ID_MENU_RESUME,          TBSTATE_ENABLED, BTNS_SHOWTEXT | BTNS_AUTOSIZE, {0}, 0, (INT_PTR)L"Resume" },
        { I_IMAGENONE, ID_MENU_PAUSE,           TBSTATE_ENABLED, BTNS_SHOWTEXT | BTNS_AUTOSIZE, {0}, 0, (INT_PTR)L"Stop" },
        { I_IMAGENONE, ID_MENU_STOP_ALL,        TBSTATE_ENABLED, BTNS_SHOWTEXT | BTNS_AUTOSIZE, {0}, 0, (INT_PTR)L"Stop All" },
        { I_IMAGENONE, ID_MENU_REMOVE,          TBSTATE_ENABLED, BTNS_SHOWTEXT | BTNS_AUTOSIZE, {0}, 0, (INT_PTR)L"Delete" },
        { I_IMAGENONE, ID_MENU_DELETE_COMPLETE, TBSTATE_ENABLED, BTNS_SHOWTEXT | BTNS_AUTOSIZE, {0}, 0, (INT_PTR)L"Delete Completed" },
        { I_IMAGENONE, 0,                       TBSTATE_ENABLED, BTNS_SEP,                        {0}, 0, 0 },
        { I_IMAGENONE, ID_MENU_OPTIONS,         TBSTATE_ENABLED, BTNS_SHOWTEXT | BTNS_AUTOSIZE, {0}, 0, (INT_PTR)L"Options" },
        { I_IMAGENONE, ID_MENU_SCHEDULER,       TBSTATE_ENABLED, BTNS_SHOWTEXT | BTNS_AUTOSIZE, {0}, 0, (INT_PTR)L"Scheduler" },
        { I_IMAGENONE, 0,                       TBSTATE_ENABLED, BTNS_SEP,                        {0}, 0, 0 },
        { I_IMAGENONE, ID_MENU_OPEN_FILE,       TBSTATE_ENABLED, BTNS_SHOWTEXT | BTNS_AUTOSIZE, {0}, 0, (INT_PTR)L"Open file" },
        { I_IMAGENONE, ID_MENU_OPEN_FOLDER,     TBSTATE_ENABLED, BTNS_SHOWTEXT | BTNS_AUTOSIZE, {0}, 0, (INT_PTR)L"Open folder" },
    };
    SendMessageW(g_hwndToolbar, TB_ADDBUTTONSW,
                 (WPARAM)(sizeof(btns)/sizeof(btns[0])), (LPARAM)btns);
}

void createTreeView(HWND parent) {
    g_hwndTree = CreateWindowExW(WS_EX_CLIENTEDGE, WC_TREEVIEWW, L"",
        WS_CHILD | WS_VISIBLE | TVS_HASBUTTONS | TVS_HASLINES |
        TVS_LINESATROOT | TVS_SHOWSELALWAYS,
        0, 0, 0, 0, parent, (HMENU)(UINT_PTR)ID_TREEVIEW, g_hInst, nullptr);

    auto addItem = [&](HTREEITEM parentItem, const wchar_t* text, Cat cat,
                       bool expand) -> HTREEITEM {
        TVINSERTSTRUCTW tvis{};
        tvis.hParent = parentItem;
        tvis.hInsertAfter = TVI_LAST;
        tvis.item.mask    = TVIF_TEXT | TVIF_PARAM;
        tvis.item.pszText = (LPWSTR)text;
        tvis.item.lParam  = (LPARAM)cat;
        HTREEITEM h = (HTREEITEM)SendMessageW(g_hwndTree, TVM_INSERTITEMW,
                                              0, (LPARAM)&tvis);
        if (expand && h) SendMessageW(g_hwndTree, TVM_EXPAND,
                                      TVE_EXPAND, (LPARAM)h);
        return h;
    };

    HTREEITEM all = addItem(nullptr, L"All Downloads", Cat::All, true);
    addItem(all, L"Compressed", Cat::Compressed, false);
    addItem(all, L"Documents",  Cat::Documents,  false);
    addItem(all, L"Music",      Cat::Music,      false);
    addItem(all, L"Programs",   Cat::Programs,   false);
    addItem(all, L"Video",      Cat::Video,      false);
    addItem(nullptr, L"Unfinished", Cat::Unfinished, false);
    addItem(nullptr, L"Finished",   Cat::Finished,   false);
    addItem(nullptr, L"Queues",     Cat::Queues,     false);

    // Select "All Downloads" by default.
    SendMessageW(g_hwndTree, TVM_SELECTITEM, TVGN_CARET, (LPARAM)all);
}

void createListView(HWND parent) {
    g_hwndListView = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
        WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SHOWSELALWAYS,
        0, 0, 0, 0, parent, (HMENU)(UINT_PTR)ID_LISTVIEW, g_hInst, nullptr);
    ListView_SetExtendedListViewStyle(g_hwndListView,
        LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_DOUBLEBUFFER);

    // Shared small-icon image list.
    g_imgList = ImageList_Create(GetSystemMetrics(SM_CXSMICON),
                                 GetSystemMetrics(SM_CYSMICON),
                                 ILC_COLOR32 | ILC_MASK, 8, 16);
    ListView_SetImageList(g_hwndListView, g_imgList, LVSIL_SMALL);

    struct Col { const wchar_t* name; int width; int align; };
    Col cols[COL_COUNT] = {
        { L"File Name",     260, LVCFMT_LEFT  },
        { L"Q",              24, LVCFMT_LEFT  },
        { L"Size",           88, LVCFMT_RIGHT },
        { L"Status",        100, LVCFMT_LEFT  },
        { L"Time left",     110, LVCFMT_LEFT  },
        { L"Transfer rate", 110, LVCFMT_LEFT  },
        { L"Last Try Date", 160, LVCFMT_LEFT  },
        { L"Description",   200, LVCFMT_LEFT  },
    };
    for (int i = 0; i < COL_COUNT; ++i) {
        LVCOLUMNW c{};
        c.mask    = LVCF_TEXT | LVCF_WIDTH | LVCF_FMT;
        c.pszText = (LPWSTR)cols[i].name;
        c.cx      = cols[i].width;
        c.fmt     = cols[i].align;
        ListView_InsertColumn(g_hwndListView, i, &c);
    }
}

void createStatusBar(HWND parent) {
    g_hwndStatus = CreateWindowExW(0, STATUSCLASSNAMEW, L"",
        WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP,
        0, 0, 0, 0, parent, (HMENU)(UINT_PTR)ID_STATUSBAR, g_hInst, nullptr);
}

void createMenu(HWND hwnd) {
    g_hMenu = CreateMenu();
    HMENU file = CreatePopupMenu();
    AppendMenuW(file, MF_STRING, ID_MENU_ADD,   L"&Add URL...\tCtrl+N");
    AppendMenuW(file, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(file, MF_STRING, ID_MENU_EXIT,  L"E&xit");
    AppendMenuW(g_hMenu, MF_POPUP, (UINT_PTR)file, L"&File");

    HMENU item = CreatePopupMenu();
    AppendMenuW(item, MF_STRING, ID_MENU_PAUSE,  L"&Pause\tDel");
    AppendMenuW(item, MF_STRING, ID_MENU_RESUME, L"&Resume");
    AppendMenuW(item, MF_STRING, ID_MENU_REMOVE, L"Re&move");
    AppendMenuW(item, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(item, MF_STRING, ID_MENU_OPEN_FILE,   L"&Open file\tEnter");
    AppendMenuW(item, MF_STRING, ID_MENU_OPEN_FOLDER, L"Open &folder");
    AppendMenuW(item, MF_STRING, ID_MENU_COPY_URL,    L"&Copy URL");
    AppendMenuW(g_hMenu, MF_POPUP, (UINT_PTR)item, L"&Item");

    HMENU opt = CreatePopupMenu();
    AppendMenuW(opt, MF_STRING | MF_CHECKED, ID_MENU_CLIPWATCH,
                L"Watch cl&ipboard for URLs");
    AppendMenuW(g_hMenu, MF_POPUP, (UINT_PTR)opt, L"&Options");

    HMENU help = CreatePopupMenu();
    AppendMenuW(help, MF_STRING, ID_MENU_ABOUT, L"&About");
    AppendMenuW(g_hMenu, MF_POPUP, (UINT_PTR)help, L"&Help");

    SetMenu(hwnd, g_hMenu);
}

std::vector<int> selectedIds() {
    std::vector<int> ids;
    int i = -1;
    while ((i = ListView_GetNextItem(g_hwndListView, i, LVNI_SELECTED)) != -1) {
        LVITEMW lv{}; lv.iItem = i; lv.mask = LVIF_PARAM;
        ListView_GetItem(g_hwndListView, &lv);
        ids.push_back((int)lv.lParam);
    }
    return ids;
}

void openShell(const std::wstring& path, bool folder) {
    if (path.empty()) return;
    if (folder) {
        // Open Explorer with the file selected.
        std::wstring cmd = L"/select,\"" + path + L"\"";
        ShellExecuteW(g_hwnd, L"open", L"explorer.exe",
                      cmd.c_str(), nullptr, SW_SHOWNORMAL);
    } else {
        ShellExecuteW(g_hwnd, L"open", path.c_str(),
                      nullptr, nullptr, SW_SHOWNORMAL);
    }
}

void copyToClipboard(const std::wstring& s) {
    if (!OpenClipboard(g_hwnd)) return;
    EmptyClipboard();
    size_t bytes = (s.size() + 1) * sizeof(wchar_t);
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (h) {
        auto* p = (wchar_t*)GlobalLock(h);
        std::memcpy(p, s.c_str(), bytes);
        GlobalUnlock(h);
        SetClipboardData(CF_UNICODETEXT, h);
    }
    CloseClipboard();
}

void showContextMenu(HWND hwnd, int x, int y) {
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, ID_MENU_PAUSE,       L"Pause");
    AppendMenuW(m, MF_STRING, ID_MENU_RESUME,      L"Resume");
    AppendMenuW(m, MF_STRING, ID_MENU_REMOVE,      L"Remove");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, ID_MENU_OPEN_FILE,   L"Open file");
    AppendMenuW(m, MF_STRING, ID_MENU_OPEN_FOLDER, L"Open folder");
    AppendMenuW(m, MF_STRING, ID_MENU_COPY_URL,    L"Copy URL");
    TrackPopupMenu(m, TPM_RIGHTBUTTON, x, y, 0, hwnd, nullptr);
    DestroyMenu(m);
}

void handleProgress(ProgressMsg* m) {
    std::unique_ptr<ProgressMsg> owned(m);
    std::lock_guard<std::mutex> lk(g_itemsMu);
    GuiItem* it = findItem(m->id);
    if (!it) return;
    it->state       = ItemState::Running;
    it->total       = m->total;
    it->downloaded  = m->downloaded;
    it->bps         = m->bps;
    it->activeConns = m->activeConns;
    it->segments    = m->segments;
    GetLocalTime(&it->lastTryDate);

    // Scheduler stop-at: abort the download if the configured stop time
    // has passed. Using the item's own downloader abort() makes this a
    // graceful end (state will flip to Aborted via handleCompleted).
    int64_t stopAt = scheduleToEpoch(g_settings.stopAt);
    if (stopAt > 0 && (int64_t)std::time(nullptr) >= stopAt) {
        if (it->dl)  it->dl->abort();
        if (it->vg)  it->vg->abort();
        if (it->ftp) it->ftp->abort();
    }

    int row = listRowForId(m->id);
    if (row >= 0) renderRow(row, *it);
}

void handleCompleted(CompleteMsg* m) {
    std::unique_ptr<CompleteMsg> owned(m);
    std::lock_guard<std::mutex> lk(g_itemsMu);
    GuiItem* it = findItem(m->id);
    if (!it) return;
    switch (m->status) {
    case DownloadStatus::Ok:        it->state = ItemState::Done;    break;
    case DownloadStatus::Aborted:   it->state = ItemState::Aborted; break;
    default:                        it->state = ItemState::Err;     break;
    }
    it->message    = m->message;
    it->resultPath = m->path;
    GetLocalTime(&it->lastTryDate);
    if (it->thread) it->thread->detach();
    it->thread.reset();
    // If the item no longer matches the current filter (e.g., it's Done
    // but we're viewing "Unfinished"), drop it from the ListView.
    int row = listRowForId(m->id);
    if (!matchesFilter(*it)) {
        if (row >= 0) ListView_DeleteItem(g_hwndListView, row);
    } else if (row >= 0) {
        renderRow(row, *it);
    }

    if (it->state == ItemState::Done) {
        notifyTray(L"matata", (L"Download complete: " + it->resultPath).c_str());
    } else if (it->state == ItemState::Err) {
        notifyTray(L"matata", (L"Download failed: " + it->message).c_str());
    }
}

LRESULT CALLBACK MainWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE:
        createMenu(hwnd);
        createToolbar(hwnd);
        createTreeView(hwnd);
        createListView(hwnd);
        createStatusBar(hwnd);
        installTrayIcon(hwnd);
        AddClipboardFormatListener(hwnd);
        g_dropTarget = new MainDropTarget();
        RegisterDragDrop(hwnd, g_dropTarget);
        updateStatusBar();
        return 0;

    case WM_SIZE:
        if (wp == SIZE_MINIMIZED) {
            ShowWindow(hwnd, SW_HIDE);
            return 0;
        }
        layoutChildren(hwnd);
        return 0;

    case WM_COMMAND: {
        UINT id = LOWORD(wp);
        switch (id) {
        case ID_MENU_ADD: {
            auto r = showAddUrlDialog(hwnd);
            if (r.ok) addDownload(r.url, r.filename, r.outDir);
            return 0;
        }
        case ID_MENU_PAUSE:  for (int id2 : selectedIds()) abortItem(id2); return 0;
        case ID_MENU_RESUME: for (int id2 : selectedIds()) restartItem(id2); return 0;
        case ID_MENU_REMOVE: for (int id2 : selectedIds()) removeItem(id2); return 0;
        case ID_MENU_STOP_ALL: {
            std::vector<int> ids;
            {
                std::lock_guard<std::mutex> lk(g_itemsMu);
                for (auto& p : g_items)
                    if (p->state == ItemState::Running) ids.push_back(p->id);
            }
            for (int id2 : ids) abortItem(id2);
            return 0;
        }
        case ID_MENU_DELETE_COMPLETE: {
            std::vector<int> ids;
            {
                std::lock_guard<std::mutex> lk(g_itemsMu);
                for (auto& p : g_items)
                    if (p->state == ItemState::Done) ids.push_back(p->id);
            }
            for (int id2 : ids) removeItem(id2);
            return 0;
        }
        case ID_MENU_OPTIONS:   showOptionsDialog(hwnd);   return 0;
        case ID_MENU_SCHEDULER: showSchedulerDialog(hwnd); return 0;
        case ID_MENU_OPEN_FILE:
        case ID_MENU_OPEN_FOLDER:
        case ID_MENU_COPY_URL: {
            auto ids = selectedIds();
            if (ids.empty()) return 0;
            std::wstring path, url;
            {
                std::lock_guard<std::mutex> lk(g_itemsMu);
                if (auto* it = findItem(ids[0])) {
                    path = it->resultPath;
                    url  = it->url;
                }
            }
            if (id == ID_MENU_OPEN_FILE)   openShell(path, false);
            if (id == ID_MENU_OPEN_FOLDER) openShell(path, true);
            if (id == ID_MENU_COPY_URL)    copyToClipboard(url);
            return 0;
        }
        case ID_MENU_CLIPWATCH:
            g_clipWatchEnabled = !g_clipWatchEnabled;
            CheckMenuItem(GetMenu(hwnd), ID_MENU_CLIPWATCH,
                MF_BYCOMMAND | (g_clipWatchEnabled ? MF_CHECKED : MF_UNCHECKED));
            return 0;
        case ID_MENU_ABOUT:
            MessageBoxW(hwnd,
                L"matata — a Windows download manager.\n"
                L"Pure Win32 + C++17. v0.6.",
                L"About matata", MB_OK | MB_ICONINFORMATION);
            return 0;
        case ID_MENU_EXIT:
            DestroyWindow(hwnd);
            return 0;
        case ID_TRAY_SHOW:
            ShowWindow(hwnd, SW_SHOW);
            SetForegroundWindow(hwnd);
            return 0;
        case ID_TRAY_EXIT:
            DestroyWindow(hwnd);
            return 0;
        }
        break;
    }

    case WM_NOTIFY: {
        auto* nm = (NMHDR*)lp;
        if (nm->idFrom == ID_TREEVIEW && nm->code == TVN_SELCHANGEDW) {
            auto* sc = (NMTREEVIEWW*)lp;
            g_filter = (Cat)sc->itemNew.lParam;
            rebuildListView();
            updateStatusBar();
            return 0;
        }
        if (nm->idFrom == ID_LISTVIEW) {
            if (nm->code == NM_DBLCLK) {
                auto ids = selectedIds();
                if (!ids.empty()) {
                    std::wstring path;
                    {
                        std::lock_guard<std::mutex> lk(g_itemsMu);
                        if (auto* it = findItem(ids[0])) path = it->resultPath;
                    }
                    openShell(path, false);
                }
                return 0;
            }
            if (nm->code == NM_RCLICK) {
                DWORD p = GetMessagePos();
                showContextMenu(hwnd, GET_X_LPARAM(p), GET_Y_LPARAM(p));
                return 0;
            }
        }
        break;
    }

    case WM_APP_PROGRESS:  handleProgress((ProgressMsg*)lp);  updateStatusBar(); return 0;
    case WM_APP_COMPLETED: handleCompleted((CompleteMsg*)lp); updateStatusBar(); return 0;

    case WM_APP_TRAY:
        if (LOWORD(lp) == WM_LBUTTONDBLCLK || LOWORD(lp) == WM_LBUTTONUP) {
            ShowWindow(hwnd, SW_SHOW);
            SetForegroundWindow(hwnd);
            return 0;
        }
        if (LOWORD(lp) == WM_RBUTTONUP) {
            POINT pt; GetCursorPos(&pt);
            HMENU m = CreatePopupMenu();
            AppendMenuW(m, MF_STRING, ID_TRAY_SHOW, L"Open");
            AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(m, MF_STRING, ID_TRAY_EXIT, L"Exit");
            SetForegroundWindow(hwnd);
            TrackPopupMenu(m, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, nullptr);
            DestroyMenu(m);
            return 0;
        }
        return 0;

    case WM_COPYDATA: {
        auto cds = reinterpret_cast<COPYDATASTRUCT*>(lp);
        if (!cds || cds->dwData != kHandoffMagic) return 0;
        if (cds->cbData == 0 || cds->cbData % sizeof(wchar_t) != 0) return 0;
        HandoffSpec s = deserializeHandoff(
            reinterpret_cast<const wchar_t*>(cds->lpData),
            cds->cbData / sizeof(wchar_t));
        if (s.url.empty()) return 0;
        if (IsIconic(hwnd))       ShowWindow(hwnd, SW_RESTORE);
        else if (!IsWindowVisible(hwnd)) ShowWindow(hwnd, SW_SHOW);
        SetForegroundWindow(hwnd);
        addDownloadFromHandoff(s);
        return TRUE;
    }
    case WM_CLIPBOARDUPDATE: {
        if (!g_clipWatchEnabled) return 0;
        std::wstring u = readClipboardUrl(hwnd);
        if (u.empty() || u == g_lastClipboardUrl) return 0;
        g_lastClipboardUrl = u;
        // Only prompt for URLs that LOOK interesting (extension / stream).
        // Skip very short / bare-host URLs to avoid nagging on normal browsing.
        if (u.find_last_of(L'/') == u.size() - 1) return 0;
        PostMessageW(hwnd, WM_APP_CLIPURL, 0, (LPARAM)new std::wstring(u));
        return 0;
    }
    case WM_APP_CLIPURL: {
        std::unique_ptr<std::wstring> uw((std::wstring*)lp);
        std::wstring& u = *uw;
        // Show a Yes/No prompt next to the tray icon.
        std::wstring body = L"Download this URL?\n\n" + u;
        int r = MessageBoxW(hwnd, body.c_str(), L"matata",
                            MB_YESNO | MB_ICONQUESTION | MB_SETFOREGROUND);
        if (r == IDYES) {
            ShowWindow(hwnd, SW_SHOW);
            SetForegroundWindow(hwnd);
            addDownload(u, L"", L"");
        }
        return 0;
    }

    case WM_DESTROY: {
        // Snapshot layout into g_settings before tearing down.
        WINDOWPLACEMENT wp_{};
        wp_.length = sizeof(wp_);
        if (GetWindowPlacement(hwnd, &wp_)) {
            g_settings.windowMaximized = (wp_.showCmd == SW_SHOWMAXIMIZED);
            RECT& r = wp_.rcNormalPosition;
            g_settings.windowX = r.left;
            g_settings.windowY = r.top;
            g_settings.windowW = r.right  - r.left;
            g_settings.windowH = r.bottom - r.top;
        }
        for (int i = 0; i < COL_COUNT; ++i) {
            int w = ListView_GetColumnWidth(g_hwndListView, i);
            if (w > 10 && w < 2000) g_settings.colWidth[i] = w;
        }
        g_settings.clipboardWatch = g_clipWatchEnabled;
        saveSettings();

        RevokeDragDrop(hwnd);
        if (g_dropTarget) { g_dropTarget->Release(); g_dropTarget = nullptr; }
        RemoveClipboardFormatListener(hwnd);
        removeTrayIcon();
        PostQuitMessage(0);
        return 0;
    }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

} // anon

// ---- entry point -----------------------------------------------------

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR cmdLine, int nCmdShow) {
    g_hInst = hInst;

    // Parse the command line into a handoff spec up front. Same format is
    // used by CommandLineToArgvW invocation and by WM_COPYDATA payloads.
    HandoffSpec cliSpec;
    bool haveCliUrl = parseHandoffArgs(cmdLine, cliSpec);

    // Single-instance gate. If another matata-gui is running, forward the
    // spec to it (so the user sees the existing window, not a new one).
    HANDLE hInstanceMutex = CreateMutexW(nullptr, TRUE, kInstanceMutex);
    bool alreadyRunning = (GetLastError() == ERROR_ALREADY_EXISTS);
    if (alreadyRunning) {
        if (haveCliUrl) {
            if (!forwardHandoffToExistingInstance(cliSpec)) {
                // Existing window not found — shouldn't happen given the
                // mutex, but fall through and run normally in that case.
                alreadyRunning = false;
            }
        } else {
            // No handoff — just bring the running window forward and exit.
            HWND target = FindWindowW(kClassName, nullptr);
            if (target) {
                if (IsIconic(target)) ShowWindow(target, SW_RESTORE);
                else                  ShowWindow(target, SW_SHOW);
                SetForegroundWindow(target);
            }
        }
        if (alreadyRunning) {
            if (hInstanceMutex) CloseHandle(hInstanceMutex);
            return 0;
        }
    }

    // Load persisted settings BEFORE the main window is created so we
    // can honor the saved size/position.
    loadSettings();

    // OleInitialize (not just CoInitialize) so RegisterDragDrop works.
    OleInitialize(nullptr);

    INITCOMMONCONTROLSEX icc{};
    icc.dwSize = sizeof(icc);
    icc.dwICC  = ICC_LISTVIEW_CLASSES | ICC_BAR_CLASSES | ICC_STANDARD_CLASSES
              | ICC_TREEVIEW_CLASSES;
    InitCommonControlsEx(&icc);

    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = MainWndProc;
    wc.hInstance     = hInst;
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = kClassName;
    wc.hIcon         = LoadIconW(nullptr, IDI_APPLICATION);
    wc.hIconSm       = LoadIconW(nullptr, IDI_APPLICATION);
    RegisterClassExW(&wc);

    g_hwnd = CreateWindowExW(0, kClassName, kAppTitle,
        WS_OVERLAPPEDWINDOW,
        g_settings.windowX, g_settings.windowY,
        g_settings.windowW, g_settings.windowH,
        nullptr, nullptr, hInst, nullptr);
    if (!g_hwnd) return 1;

    // Apply saved clipboard-watch preference to the menu checkmark.
    g_clipWatchEnabled = g_settings.clipboardWatch;
    CheckMenuItem(g_hMenu, ID_MENU_CLIPWATCH,
        MF_BYCOMMAND | (g_clipWatchEnabled ? MF_CHECKED : MF_UNCHECKED));

    // Apply persisted column widths to the listview.
    for (int i = 0; i < COL_COUNT; ++i) {
        ListView_SetColumnWidth(g_hwndListView, i, g_settings.colWidth[i]);
    }

    ShowWindow(g_hwnd, g_settings.windowMaximized ? SW_SHOWMAXIMIZED : nCmdShow);
    UpdateWindow(g_hwnd);

    // If a URL / handoff spec was passed on the command line, queue it.
    if (haveCliUrl) {
        addDownloadFromHandoff(cliSpec);
    }

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    // On exit: tell all live downloaders to abort, then detach their threads.
    {
        std::lock_guard<std::mutex> lk(g_itemsMu);
        for (auto& it : g_items) {
            if (it->dl)  it->dl->abort();
            if (it->vg)  it->vg->abort();
            if (it->ftp) it->ftp->abort();
        }
    }
    OleUninitialize();
    if (hInstanceMutex) { ReleaseMutex(hInstanceMutex); CloseHandle(hInstanceMutex); }
    return (int)msg.wParam;
}
