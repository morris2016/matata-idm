// matata CLI entry point.

#include "matata/auth.hpp"
#include "matata/dash.hpp"
#include "matata/downloader.hpp"
#include "matata/ftp_downloader.hpp"
#include "matata/hls.hpp"
#include "matata/queue.hpp"
#include "matata/updater.hpp"
#include "matata/url.hpp"
#include "matata/video.hpp"

#include <windows.h>
#include <fcntl.h>
#include <io.h>

#include <atomic>
#include <cstdio>
#include <ctime>
#include <cwchar>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace matata;

namespace {

void printUsage() {
    std::wprintf(
        L"matata 0.3 - multi-connection downloader\n"
        L"\n"
        L"Usage:\n"
        L"  matata <url> [url ...] [options]\n"
        L"  matata --help\n"
        L"\n"
        L"Output:\n"
        L"  -o NAME             output filename (single-URL only)\n"
        L"  -d DIR              output directory (default: current)\n"
        L"  --categorize        route each file into a subfolder by extension\n"
        L"\n"
        L"Connections:\n"
        L"  -n CONNS            connections per download (default: 8)\n"
        L"  -j N                max concurrent downloads (default: 3)\n"
        L"  -J N                max total connections across queue (default: 16)\n"
        L"\n"
        L"Headers:\n"
        L"  --cookie STR        Cookie header\n"
        L"  --referer URL       Referer header\n"
        L"  --user-agent STR    User-Agent header\n"
        L"  -H 'K: V'           arbitrary header (repeatable)\n"
        L"\n"
        L"Auth:\n"
        L"  --user USER:PASS    HTTP Basic auth (colon-separated)\n"
        L"  --bearer TOKEN      HTTP Bearer auth\n"
        L"  --netrc             look up creds in default netrc (%%USERPROFILE%%\\_netrc)\n"
        L"  --netrc-file PATH   look up creds in a specific netrc file\n"
        L"\n"
        L"Throttling:\n"
        L"  --limit-rate BYTES  global bandwidth cap; suffixes k/m/g accepted\n"
        L"\n"
        L"Verification:\n"
        L"  --no-verify-checksum  skip digest check even if server advertises one\n"
        L"\n"
        L"Video (HLS / DASH):\n"
        L"  --hls               force video mode (else auto-detect .m3u8 / .mpd)\n"
        L"  --no-hls            disable auto-detect\n"
        L"  --quality Q         variant: best | worst | 720p | 1080p | <height>\n"
        L"  --ffmpeg PATH       HLS: remux .ts to .mp4; DASH: mux video + audio\n"
        L"  --video-parallel N  parallel segment downloads (default: 8)\n"
        L"\n"
        L"Queue:\n"
        L"  --queue FILE        persist/resume queue from FILE\n"
        L"  --start-at TIME     hold queue until TIME (HH:MM or YYYY-MM-DD HH:MM)\n"
        L"  --stop-at TIME      stop starting new items after TIME\n"
        L"  --shutdown-on-done  Windows shutdown after every item is done\n"
        L"\n"
        L"Diagnostics:\n"
        L"  -v, --verbose       verbose probe output\n"
        L"  --check-update [URL]\n"
        L"                      fetch a version manifest and report if newer\n"
        L"\n");
}

int runCheckUpdate(const std::wstring& manifestUrl) {
    std::wstring url = manifestUrl.empty()
        ? L"https://matata.example/update/manifest.json"
        : manifestUrl;
    UpdateInfo info;
    std::wstring err;
    std::wprintf(L"current version: %ls\n", kMatataVersion);
    std::wprintf(L"checking:        %ls\n", url.c_str());
    if (!checkForUpdate(url, kMatataVersion, info, err)) {
        std::wprintf(L"[err] %ls\n", err.c_str());
        return 1;
    }
    std::wprintf(L"latest:  %ls\n", info.latestVersion.c_str());
    if (info.available) {
        std::wprintf(L"update available at: %ls\n", info.downloadUrl.c_str());
        if (!info.notes.empty())
            std::wprintf(L"notes: %ls\n", info.notes.c_str());
    } else {
        std::wprintf(L"up to date.\n");
    }
    return info.available ? 0 : 0;
}

// Parse HH:MM (today, or tomorrow if past) or "YYYY-MM-DD HH:MM".
// Returns 0 on parse failure.
int64_t parseScheduleTime(const std::wstring& s) {
    if (s.empty()) return 0;

    int year = 0, mon = 0, day = 0, hh = 0, mm = 0;
    bool haveDate = false;

    // Split on whitespace: either "HH:MM" or "YYYY-MM-DD HH:MM".
    std::wstring left = s;
    std::wstring right;
    auto space = s.find(L' ');
    if (space != std::wstring::npos) {
        left = s.substr(0, space);
        right = s.substr(space + 1);
        while (!right.empty() && right.front() == L' ') right.erase(right.begin());
    }

    auto parseHM = [](const std::wstring& x, int& H, int& M) -> bool {
        auto colon = x.find(L':');
        if (colon == std::wstring::npos) return false;
        try {
            H = std::stoi(x.substr(0, colon));
            M = std::stoi(x.substr(colon + 1));
        } catch (...) { return false; }
        return H >= 0 && H < 24 && M >= 0 && M < 60;
    };

    if (!right.empty()) {
        // left = date, right = time
        int y = 0, mo = 0, d = 0;
        int pos = 0;
        try {
            size_t p1 = left.find(L'-');
            size_t p2 = left.find(L'-', p1 + 1);
            if (p1 == std::wstring::npos || p2 == std::wstring::npos) return 0;
            y = std::stoi(left.substr(0, p1));
            mo = std::stoi(left.substr(p1 + 1, p2 - p1 - 1));
            d = std::stoi(left.substr(p2 + 1));
        } catch (...) { return 0; }
        (void)pos;
        if (!parseHM(right, hh, mm)) return 0;
        year = y; mon = mo; day = d; haveDate = true;
    } else {
        if (!parseHM(left, hh, mm)) return 0;
    }

    std::time_t now = std::time(nullptr);
    std::tm     t{};
    if (localtime_s(&t, &now) != 0) return 0;

    if (haveDate) {
        t.tm_year = year - 1900;
        t.tm_mon  = mon - 1;
        t.tm_mday = day;
    }
    t.tm_hour = hh;
    t.tm_min  = mm;
    t.tm_sec  = 0;

    std::time_t target = std::mktime(&t);
    if (target == (std::time_t)-1) return 0;

    // HH:MM only: if target is in the past today, roll to tomorrow.
    if (!haveDate && target <= now) target += 24 * 3600;
    return (int64_t)target;
}

std::wstring hostOf(const std::wstring& url) {
    Url u;
    std::wstring err;
    if (parseUrl(url, u, err)) return u.host;
    return L"";
}

int64_t parseRate(const std::wstring& s) {
    if (s.empty()) return 0;
    wchar_t* endp = nullptr;
    double v = wcstod(s.c_str(), &endp);
    if (v <= 0) return 0;
    if (endp && *endp) {
        wchar_t u = (wchar_t)towlower(*endp);
        if      (u == L'k') v *= 1024.0;
        else if (u == L'm') v *= 1024.0 * 1024.0;
        else if (u == L'g') v *= 1024.0 * 1024.0 * 1024.0;
    }
    return (int64_t)v;
}

std::wstring formatBytes(int64_t b) {
    if (b < 0) return L"?";
    double v = (double)b;
    const wchar_t* u[] = { L"B", L"KiB", L"MiB", L"GiB", L"TiB" };
    int i = 0;
    while (v >= 1024.0 && i < 4) { v /= 1024.0; ++i; }
    wchar_t buf[32];
    if (i == 0) _snwprintf_s(buf, _TRUNCATE, L"%lld %s", (long long)b, u[i]);
    else        _snwprintf_s(buf, _TRUNCATE, L"%.2f %s", v, u[i]);
    return buf;
}

std::wstring formatRate(int64_t bps) {
    return formatBytes(bps) + L"/s";
}

// ---- JSON progress helpers (line-delimited, for the native host) ----

std::wstring jsonEscape(const std::wstring& s) {
    std::wstring out;
    out.reserve(s.size() + 2);
    for (wchar_t c : s) {
        switch (c) {
            case L'"':  out += L"\\\""; break;
            case L'\\': out += L"\\\\"; break;
            case L'\n': out += L"\\n";  break;
            case L'\r': out += L"\\r";  break;
            case L'\t': out += L"\\t";  break;
            default:
                if (c < 0x20) {
                    wchar_t b[8];
                    _snwprintf_s(b, _TRUNCATE, L"\\u%04x", (unsigned)c);
                    out += b;
                } else out.push_back(c);
        }
    }
    return out;
}

void emitJsonLine(const std::wstring& body, const std::wstring& jobId) {
    if (jobId.empty()) {
        std::wprintf(L"%ls\n", body.c_str());
    } else if (!body.empty() && body[0] == L'{') {
        std::wprintf(L"{\"jobId\":\"%ls\",%ls\n",
                     jsonEscape(jobId).c_str(),
                     body.substr(1).c_str());
    } else {
        std::wprintf(L"%ls\n", body.c_str());
    }
    fflush(stdout);
}

std::atomic<bool>* g_abortFlag = nullptr;
BOOL WINAPI ctrlHandler(DWORD type) {
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT) {
        if (g_abortFlag) g_abortFlag->store(true);
        return TRUE;
    }
    return FALSE;
}

void requestShutdown() {
    HANDLE tok;
    if (!OpenProcessToken(GetCurrentProcess(),
                          TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &tok)) return;
    TOKEN_PRIVILEGES tp{};
    LookupPrivilegeValueW(nullptr, SE_SHUTDOWN_NAME, &tp.Privileges[0].Luid);
    tp.PrivilegeCount           = 1;
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    AdjustTokenPrivileges(tok, FALSE, &tp, 0, nullptr, nullptr);
    CloseHandle(tok);
    ExitWindowsEx(EWX_SHUTDOWN | EWX_FORCEIFHUNG,
                  SHTDN_REASON_MAJOR_APPLICATION | SHTDN_REASON_MINOR_OTHER |
                  SHTDN_REASON_FLAG_PLANNED);
}

struct Args {
    std::vector<std::wstring> urls;
    DownloadOptions           opts;
    int                       jobs       = 3;
    int                       totalConns = 16;
    bool                      shutdown   = false;
    bool                      categorize = false;
    std::wstring              queuePath;
    int64_t                   startAtEpoch = 0;
    int64_t                   stopAtEpoch  = 0;
    BasicCreds                userCreds;
    std::wstring              bearerToken;
    bool                      useNetrc   = false;
    std::wstring              netrcPath;
    // Video / HLS
    std::wstring              quality    = L"best";
    std::wstring              ffmpegPath;
    int                       videoParallel = 8;
    int                       hlsMode    = 0;   // 0 auto, 1 force, -1 disable
    // Output style
    bool                      jsonProgress = false;
    std::wstring              jobId;  // optional, forwarded in JSON progress
    bool                      showHelp   = false;
    bool                      checkUpdate = false;
    std::wstring              updateManifest;
    std::wstring              parseError;
};

Args parseArgs(int argc, wchar_t** argv) {
    Args a;
    for (int i = 1; i < argc; ++i) {
        std::wstring s = argv[i];
        auto need = [&](const wchar_t* name) -> wchar_t* {
            if (i + 1 >= argc) { a.parseError = std::wstring(name) + L" needs a value"; return nullptr; }
            return argv[++i];
        };

        if (s == L"--help" || s == L"-h") { a.showHelp = true; return a; }
        if (s == L"--check-update") {
            a.checkUpdate = true;
            if (i + 1 < argc && argv[i+1][0] != L'-') a.updateManifest = argv[++i];
            return a;
        }
        else if (s == L"-o")               { if (auto v = need(L"-o")) a.opts.outputName = v; }
        else if (s == L"-d")               { if (auto v = need(L"-d")) a.opts.outputDir  = v; }
        else if (s == L"-n")               { if (auto v = need(L"-n")) a.opts.connections = _wtoi(v); }
        else if (s == L"-j")               { if (auto v = need(L"-j")) a.jobs = _wtoi(v); }
        else if (s == L"-J")               { if (auto v = need(L"-J")) a.totalConns = _wtoi(v); }
        else if (s == L"--cookie")         { if (auto v = need(L"--cookie"))
                                                 a.opts.headers.push_back({L"Cookie", v}); }
        else if (s == L"--referer")        { if (auto v = need(L"--referer"))
                                                 a.opts.headers.push_back({L"Referer", v}); }
        else if (s == L"--user-agent")     { if (auto v = need(L"--user-agent"))
                                                 a.opts.headers.push_back({L"User-Agent", v}); }
        else if (s == L"-H")               {
            if (auto v = need(L"-H")) {
                std::wstring raw = v;
                auto colon = raw.find(L':');
                if (colon != std::wstring::npos) {
                    std::wstring k = raw.substr(0, colon);
                    std::wstring val = raw.substr(colon + 1);
                    while (!val.empty() && val.front() == L' ') val.erase(val.begin());
                    a.opts.headers.push_back({std::move(k), std::move(val)});
                }
            }
        }
        else if (s == L"--limit-rate")     { if (auto v = need(L"--limit-rate")) a.opts.bandwidthBps = parseRate(v); }
        else if (s == L"--queue")          { if (auto v = need(L"--queue")) a.queuePath = v; }
        else if (s == L"--shutdown-on-done") { a.shutdown = true; }
        else if (s == L"--categorize")     { a.categorize = true; }
        else if (s == L"--no-verify-checksum") { a.opts.verifyChecksum = false; }
        else if (s == L"--start-at")       {
            if (auto v = need(L"--start-at")) {
                a.startAtEpoch = parseScheduleTime(v);
                if (a.startAtEpoch == 0) {
                    a.parseError = L"--start-at: could not parse time (use HH:MM or YYYY-MM-DD HH:MM)";
                    return a;
                }
            }
        }
        else if (s == L"--stop-at")        {
            if (auto v = need(L"--stop-at")) {
                a.stopAtEpoch = parseScheduleTime(v);
                if (a.stopAtEpoch == 0) {
                    a.parseError = L"--stop-at: could not parse time (use HH:MM or YYYY-MM-DD HH:MM)";
                    return a;
                }
            }
        }
        else if (s == L"--user")           {
            if (auto v = need(L"--user")) {
                std::wstring raw = v;
                auto colon = raw.find(L':');
                if (colon == std::wstring::npos) {
                    a.userCreds.user = raw;  // allow prompt? for now, no password
                } else {
                    a.userCreds.user     = raw.substr(0, colon);
                    a.userCreds.password = raw.substr(colon + 1);
                }
            }
        }
        else if (s == L"--bearer")         { if (auto v = need(L"--bearer"))     a.bearerToken = v; }
        else if (s == L"--netrc")          { a.useNetrc = true; }
        else if (s == L"--netrc-file")     { if (auto v = need(L"--netrc-file")) { a.netrcPath = v; a.useNetrc = true; } }
        else if (s == L"--quality")        { if (auto v = need(L"--quality"))       a.quality = v; }
        else if (s == L"--ffmpeg")         { if (auto v = need(L"--ffmpeg"))        a.ffmpegPath = v; }
        else if (s == L"--video-parallel") { if (auto v = need(L"--video-parallel")) a.videoParallel = _wtoi(v); }
        else if (s == L"--hls")            { a.hlsMode = 1; }
        else if (s == L"--no-hls")         { a.hlsMode = -1; }
        else if (s == L"--json-progress")  { a.jsonProgress = true; }
        else if (s == L"--job-id")         { if (auto v = need(L"--job-id")) a.jobId = v; }
        else if (s == L"-v" || s == L"--verbose") { a.opts.verbose = true; }
        else if (!s.empty() && s[0] == L'-') {
            a.parseError = L"unknown option: " + s;
            return a;
        }
        else {
            a.urls.push_back(s);
        }
    }
    if (a.opts.connections <= 0) a.opts.connections = 8;
    if (a.opts.connections > 32) a.opts.connections = 32;
    if (a.jobs <= 0) a.jobs = 1;
    if (a.totalConns <= 0) a.totalConns = 8;
    return a;
}

void printSingleProgress(const DownloadProgress& p, int& lastLineLen) {
    wchar_t buf[256];
    std::wstring line;
    if (p.total > 0) {
        double pct = (double)p.downloaded * 100.0 / (double)p.total;
        _snwprintf_s(buf, _TRUNCATE,
            L"  %5.1f%%  %ls / %ls  @ %ls  (%d conn, %d seg)",
            pct, formatBytes(p.downloaded).c_str(),
            formatBytes(p.total).c_str(),
            formatRate(p.bytesPerSec).c_str(),
            p.activeConns, p.segments);
    } else {
        _snwprintf_s(buf, _TRUNCATE,
            L"  %ls  @ %ls  (%d conn, %d seg)",
            formatBytes(p.downloaded).c_str(),
            formatRate(p.bytesPerSec).c_str(),
            p.activeConns, p.segments);
    }
    line = buf;
    int pad = lastLineLen - (int)line.size();
    if (pad < 0) pad = 0;
    std::wprintf(L"\r%ls%*s", line.c_str(), pad, L"");
    fflush(stdout);
    lastLineLen = (int)line.size();
}

DownloadOptions optsForUrl(const Args& a, const std::wstring& url) {
    DownloadOptions o = a.opts;
    // Auth: explicit --user / --bearer > netrc.
    if (!a.userCreds.empty()) {
        appendBasicAuth(o.headers, a.userCreds);
    } else if (!a.bearerToken.empty()) {
        appendBearerAuth(o.headers, a.bearerToken);
    } else if (a.useNetrc) {
        std::wstring path = a.netrcPath.empty() ? defaultNetrcPath() : a.netrcPath;
        if (!path.empty()) {
            std::wstring host = hostOf(url);
            if (!host.empty()) {
                BasicCreds c = lookupNetrc(path, host);
                if (!c.empty()) appendBasicAuth(o.headers, c);
            }
        }
    }
    return o;
}

bool isVideoRequest(const Args& a, const std::wstring& url) {
    if (a.hlsMode == 1)  return true;
    if (a.hlsMode == -1) return false;
    return looksLikeHlsUrl(url) || looksLikeDashUrl(url);
}

VideoOptions videoOptsForUrl(const Args& a, const std::wstring& url) {
    VideoOptions v;
    v.outputDir   = a.opts.outputDir;
    v.outputName  = a.opts.outputName;
    v.quality     = a.quality;
    v.ffmpegPath  = a.ffmpegPath;
    v.parallel    = (a.videoParallel > 0 ? a.videoParallel : 8);
    v.verbose     = a.opts.verbose;
    v.headers     = optsForUrl(a, url).headers;
    return v;
}

int runVideo(const Args& a) {
    VideoOptions opts = videoOptsForUrl(a, a.urls[0]);
    auto vg = std::make_unique<VideoGrabber>(a.urls[0], opts);

    std::atomic<bool> abortFlag{false};
    g_abortFlag = &abortFlag;
    SetConsoleCtrlHandler(ctrlHandler, TRUE);
    std::thread relay([&]{ while (!abortFlag.load()) Sleep(100); vg->abort(); });

    int lineLen = 0;
    if (a.jsonProgress) {
        vg->setProgressCallback([&](const VideoProgress& p){
            wchar_t buf[256];
            double pct = p.segmentsTotal > 0
                ? (double)p.segmentsDone * 100.0 / (double)p.segmentsTotal
                : 0.0;
            _snwprintf_s(buf, _TRUNCATE,
                L"{\"type\":\"progress\",\"kind\":\"video\","
                L"\"percent\":%.2f,\"done\":%d,\"total\":%d,"
                L"\"bytes\":%lld,\"rate\":%lld}",
                pct, p.segmentsDone, p.segmentsTotal,
                (long long)p.bytes, (long long)p.bytesPerSec);
            emitJsonLine(buf, a.jobId);
        });
    } else {
        vg->setProgressCallback([&](const VideoProgress& p){
            wchar_t buf[256];
            double pct = p.segmentsTotal > 0
                ? (double)p.segmentsDone * 100.0 / (double)p.segmentsTotal
                : 0.0;
            _snwprintf_s(buf, _TRUNCATE,
                L"  %5.1f%%  %d / %d segs  %ls  @ %ls",
                pct, p.segmentsDone, p.segmentsTotal,
                formatBytes(p.bytes).c_str(),
                formatRate(p.bytesPerSec).c_str());
            std::wstring line = buf;
            int pad = lineLen - (int)line.size();
            if (pad < 0) pad = 0;
            std::wprintf(L"\r%ls%*s", line.c_str(), pad, L"");
            fflush(stdout);
            lineLen = (int)line.size();
        });
    }

    if (a.jsonProgress) {
        wchar_t start[512];
        _snwprintf_s(start, _TRUNCATE,
            L"{\"type\":\"start\",\"kind\":\"video\",\"url\":\"%ls\"}",
            jsonEscape(a.urls[0]).c_str());
        emitJsonLine(start, a.jobId);
    } else {
        std::wprintf(L"-> %ls (hls)\n", a.urls[0].c_str());
    }
    DownloadResult r = vg->run();
    if (!a.jsonProgress) std::wprintf(L"\n");

    abortFlag = true;
    if (relay.joinable()) relay.join();

    if (a.jsonProgress) {
        wchar_t end[2048];
        const wchar_t* tag = r.status == DownloadStatus::Ok ? L"done" :
                             r.status == DownloadStatus::Aborted ? L"abort" : L"err";
        _snwprintf_s(end, _TRUNCATE,
            L"{\"type\":\"%ls\",\"path\":\"%ls\",\"message\":\"%ls\"}",
            tag,
            jsonEscape(r.outputPath).c_str(),
            jsonEscape(r.message).c_str());
        emitJsonLine(end, a.jobId);
        return r.status == DownloadStatus::Ok ? 0 :
               r.status == DownloadStatus::Aborted ? 130 : 1;
    }

    switch (r.status) {
    case DownloadStatus::Ok:
        std::wprintf(L"[ok] saved: %ls\n", r.outputPath.c_str());
        return 0;
    case DownloadStatus::Aborted:
        std::wprintf(L"[abort] partial parts may remain; re-run to retry\n");
        return 130;
    default:
        std::wprintf(L"[err] %ls\n", r.message.c_str());
        if (!r.outputPath.empty())
            std::wprintf(L"      partial: %ls\n", r.outputPath.c_str());
        return 1;
    }
}

bool isFtpUrl(const std::wstring& url) {
    return (url.size() > 6 && _wcsnicmp(url.c_str(), L"ftp://",  6) == 0)
        || (url.size() > 7 && _wcsnicmp(url.c_str(), L"ftps://", 7) == 0);
}

int runFtp(const Args& a) {
    DownloadOptions opts = optsForUrl(a, a.urls[0]);
    auto dl = std::make_unique<FtpDownloader>(a.urls[0], opts);

    std::atomic<bool> abortFlag{false};
    g_abortFlag = &abortFlag;
    SetConsoleCtrlHandler(ctrlHandler, TRUE);
    std::thread relay([&]{ while (!abortFlag.load()) Sleep(100); dl->abort(); });

    int lineLen = 0;
    dl->setProgressCallback([&](const DownloadProgress& p){
        printSingleProgress(p, lineLen);
    });

    std::wprintf(L"-> %ls (ftp)\n", a.urls[0].c_str());
    DownloadResult r = dl->run();
    std::wprintf(L"\n");

    abortFlag = true;
    if (relay.joinable()) relay.join();

    switch (r.status) {
    case DownloadStatus::Ok:
        std::wprintf(L"[ok] saved: %ls\n", r.outputPath.c_str());
        return 0;
    case DownloadStatus::Aborted:
        std::wprintf(L"[abort] partial file kept: %ls\n", r.outputPath.c_str());
        return 130;
    default:
        std::wprintf(L"[err] %ls\n", r.message.c_str());
        if (!r.outputPath.empty())
            std::wprintf(L"      partial file: %ls\n", r.outputPath.c_str());
        return 1;
    }
}

int runSingle(const Args& a) {
    DownloadOptions opts = optsForUrl(a, a.urls[0]);
    auto dl = std::make_unique<Downloader>(a.urls[0], opts);

    std::atomic<bool> abortFlag{false};
    g_abortFlag = &abortFlag;
    SetConsoleCtrlHandler(ctrlHandler, TRUE);
    std::thread relay([&]{ while (!abortFlag.load()) Sleep(100); dl->abort(); });

    int lineLen = 0;
    if (a.jsonProgress) {
        dl->setProgressCallback([&](const DownloadProgress& p){
            double pct = (p.total > 0)
                ? (double)p.downloaded * 100.0 / (double)p.total : 0.0;
            wchar_t buf[256];
            _snwprintf_s(buf, _TRUNCATE,
                L"{\"type\":\"progress\",\"kind\":\"http\","
                L"\"percent\":%.2f,\"downloaded\":%lld,\"total\":%lld,"
                L"\"rate\":%lld,\"conns\":%d,\"segs\":%d}",
                pct, (long long)p.downloaded, (long long)p.total,
                (long long)p.bytesPerSec, p.activeConns, p.segments);
            emitJsonLine(buf, a.jobId);
        });
        wchar_t start[1024];
        _snwprintf_s(start, _TRUNCATE,
            L"{\"type\":\"start\",\"kind\":\"http\",\"url\":\"%ls\"}",
            jsonEscape(a.urls[0]).c_str());
        emitJsonLine(start, a.jobId);
    } else {
        dl->setProgressCallback([&](const DownloadProgress& p){
            printSingleProgress(p, lineLen);
        });
        std::wprintf(L"-> %ls\n", a.urls[0].c_str());
    }

    DownloadResult r = dl->run();
    if (!a.jsonProgress) std::wprintf(L"\n");

    abortFlag = true;
    if (relay.joinable()) relay.join();

    if (a.jsonProgress) {
        wchar_t end[2048];
        const wchar_t* tag = r.status == DownloadStatus::Ok ? L"done" :
                             r.status == DownloadStatus::Aborted ? L"abort" : L"err";
        _snwprintf_s(end, _TRUNCATE,
            L"{\"type\":\"%ls\",\"path\":\"%ls\",\"message\":\"%ls\"}",
            tag,
            jsonEscape(r.outputPath).c_str(),
            jsonEscape(r.message).c_str());
        emitJsonLine(end, a.jobId);
        return r.status == DownloadStatus::Ok ? 0 :
               r.status == DownloadStatus::Aborted ? 130 : 1;
    }

    switch (r.status) {
    case DownloadStatus::Ok:
        std::wprintf(L"[ok] saved: %ls\n", r.outputPath.c_str());
        return 0;
    case DownloadStatus::Aborted:
        std::wprintf(L"[abort] partial file kept: %ls\n", r.outputPath.c_str());
        return 130;
    default:
        std::wprintf(L"[err] %ls\n", r.message.c_str());
        if (!r.outputPath.empty())
            std::wprintf(L"      partial file: %ls\n", r.outputPath.c_str());
        return 1;
    }
}

int runQueue(const Args& a) {
    DownloadQueue::Config qc;
    qc.maxConcurrentDownloads = a.jobs;
    qc.maxTotalConnections    = a.totalConns;
    qc.categorize             = a.categorize;
    qc.statePath              = a.queuePath;
    qc.startAtEpoch           = a.startAtEpoch;
    qc.stopAtEpoch            = a.stopAtEpoch;

    auto queue = std::make_unique<DownloadQueue>(qc);

    if (!qc.statePath.empty()) queue->loadState();

    for (auto& u : a.urls) {
        QueueItem it;
        it.url  = u;
        it.opts = optsForUrl(a, u);
        it.opts.outputName.clear();  // per-URL naming is inferred
        queue->enqueue(std::move(it));
    }

    if (a.startAtEpoch > 0) {
        int64_t wait = a.startAtEpoch - (int64_t)std::time(nullptr);
        if (wait > 0) {
            std::wprintf(L"[matata] holding queue for %llds (--start-at)\n",
                         (long long)wait);
            fflush(stdout);
        }
    }

    std::atomic<bool> abortFlag{false};
    g_abortFlag = &abortFlag;
    SetConsoleCtrlHandler(ctrlHandler, TRUE);
    std::thread relay([&]{ while (!abortFlag.load()) Sleep(100); queue->abort(); });

    std::thread reporter([&]{
        while (!abortFlag.load()) {
            auto items = queue->snapshot();
            int done = 0, running = 0, queued = 0, failed = 0;
            for (auto& it : items) {
                switch (it.status) {
                    case ItemStatus::Done:    ++done; break;
                    case ItemStatus::Running: ++running; break;
                    case ItemStatus::Queued:  ++queued; break;
                    case ItemStatus::Failed:
                    case ItemStatus::Aborted: ++failed; break;
                }
            }
            std::wprintf(L"\r[queue] total=%zu done=%d running=%d queued=%d failed=%d   ",
                         items.size(), done, running, queued, failed);
            fflush(stdout);
            if (running == 0 && queued == 0) break;
            Sleep(500);
        }
    });

    queue->runAll();
    abortFlag = true;
    if (reporter.joinable()) reporter.join();
    if (relay.joinable())    relay.join();
    std::wprintf(L"\n");

    auto items = queue->snapshot();
    int failed = 0;
    for (auto& it : items) {
        const wchar_t* tag = nullptr;
        switch (it.status) {
            case ItemStatus::Done:    tag = L"[ok]";    break;
            case ItemStatus::Failed:  tag = L"[err]";   ++failed; break;
            case ItemStatus::Aborted: tag = L"[abort]"; ++failed; break;
            default:                  tag = L"[?]";     break;
        }
        std::wprintf(L"%ls %ls", tag, it.url.c_str());
        if (!it.outputPath.empty()) std::wprintf(L"  -> %ls", it.outputPath.c_str());
        if (!it.message.empty())    std::wprintf(L"  (%ls)", it.message.c_str());
        std::wprintf(L"\n");
    }

    return failed == 0 ? 0 : 1;
}

}

int wmain(int argc, wchar_t** argv) {
    _setmode(_fileno(stdout), _O_U8TEXT);
    _setmode(_fileno(stderr), _O_U8TEXT);

    if (argc < 2) { printUsage(); return 1; }

    Args a = parseArgs(argc, argv);
    if (a.showHelp)               { printUsage(); return 0; }
    if (a.checkUpdate)            { return runCheckUpdate(a.updateManifest); }
    if (!a.parseError.empty())    { std::wprintf(L"%ls\n", a.parseError.c_str()); return 2; }
    if (a.urls.empty())           { printUsage(); return 1; }

    int rc = 0;
    if (a.urls.size() == 1 && a.queuePath.empty()) {
        if      (isVideoRequest(a, a.urls[0])) rc = runVideo(a);
        else if (isFtpUrl(a.urls[0]))          rc = runFtp(a);
        else                                    rc = runSingle(a);
    } else {
        rc = runQueue(a);
    }

    if (a.shutdown && rc == 0) {
        std::wprintf(L"[matata] all downloads finished; initiating shutdown...\n");
        requestShutdown();
    }
    return rc;
}
