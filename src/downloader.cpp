#include "matata/downloader.hpp"
#include "matata/bandwidth.hpp"
#include "matata/digest.hpp"

#include <windows.h>
#include <shlwapi.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <memory>
#include <mutex>
#include <sstream>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

namespace matata {

namespace {

constexpr int     kMaxRetries       = 5;
constexpr int     kBaseBackoffMs    = 500;
constexpr int64_t kSingleThreadHint = 1 << 20;  // < 1 MiB: don't bother splitting
constexpr int64_t kStealBufferAhead = 256 * 1024; // keep at least 256 KiB ahead

std::wstring winErr(DWORD e) {
    LPWSTR buf = nullptr;
    DWORD n = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER |
                             FORMAT_MESSAGE_FROM_SYSTEM |
                             FORMAT_MESSAGE_IGNORE_INSERTS,
                             nullptr, e, 0, (LPWSTR)&buf, 0, nullptr);
    std::wstring s = (n && buf) ? std::wstring(buf, n) : L"";
    if (buf) LocalFree(buf);
    while (!s.empty() && (s.back() == L'\r' || s.back() == L'\n' || s.back() == L' '))
        s.pop_back();
    std::wostringstream ss; ss << L"(" << e << L") " << s;
    return ss.str();
}

std::wstring joinPath(const std::wstring& dir, const std::wstring& name) {
    if (dir.empty()) return name;
    wchar_t last = dir.back();
    if (last == L'\\' || last == L'/') return dir + name;
    return dir + L"\\" + name;
}

std::wstring uniqueOutputPath(const std::wstring& dir, const std::wstring& name) {
    std::wstring candidate = joinPath(dir, name);
    if (!fs::exists(candidate)) return candidate;
    std::wstring stem = name, ext;
    auto dot = name.find_last_of(L'.');
    if (dot != std::wstring::npos && dot != 0) {
        stem = name.substr(0, dot);
        ext  = name.substr(dot);
    }
    for (int i = 1; i < 10000; ++i) {
        std::wostringstream ss;
        ss << stem << L" (" << i << L")" << ext;
        std::wstring try_ = joinPath(dir, ss.str());
        if (!fs::exists(try_)) return try_;
    }
    return candidate;
}

HANDLE createSparseFile(const std::wstring& path, int64_t size) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return h;
    if (size > 0) {
        LARGE_INTEGER li{};
        li.QuadPart = size;
        if (!SetFilePointerEx(h, li, nullptr, FILE_BEGIN) || !SetEndOfFile(h)) {
            CloseHandle(h);
            DeleteFileW(path.c_str());
            return INVALID_HANDLE_VALUE;
        }
    }
    CloseHandle(h);
    return CreateFileW(path.c_str(), GENERIC_WRITE,
                       FILE_SHARE_READ | FILE_SHARE_WRITE,
                       nullptr, OPEN_EXISTING,
                       FILE_ATTRIBUTE_NORMAL, nullptr);
}

bool writeAt(HANDLE h, int64_t offset, const uint8_t* data, size_t len) {
    while (len > 0) {
        OVERLAPPED ov{};
        ov.Offset     = (DWORD)(offset & 0xFFFFFFFFULL);
        ov.OffsetHigh = (DWORD)((uint64_t)offset >> 32);
        DWORD written = 0;
        DWORD chunk   = (len > (1u << 30)) ? (1u << 30) : (DWORD)len;
        if (!WriteFile(h, data, chunk, &written, &ov)) return false;
        if (written == 0) return false;
        data   += written;
        len    -= written;
        offset += written;
    }
    return true;
}

struct WorkerCtx {
    int                 index = 0;
    Url                 url;
    DownloadMeta*       meta = nullptr;
    Segment*            seg  = nullptr;    // current assignment; changes on steal
    bool                canSteal = false;
    int64_t             minStealBytes = 0;
    HANDLE              file = nullptr;
    const ExtraHeaders* headers = nullptr;
    RateLimiter*        limiter = nullptr;
    std::atomic<bool>*  abortFlag = nullptr;
    std::atomic<int>*   activeConns = nullptr;
    std::atomic<int>*   finishedCount = nullptr;
    std::wstring        error;
    bool                ok = false;
};

void workerMain(WorkerCtx* ctx) {
    ++*ctx->activeConns;
    int attempt = 0;

    while (!*ctx->abortFlag && ctx->seg) {
        Segment* seg = ctx->seg;

        if (seg->done()) {
            if (!ctx->canSteal) { ctx->ok = true; break; }
            Segment* next = ctx->meta->stealFromSlowest(ctx->minStealBytes,
                                                        kStealBufferAhead);
            if (!next) { ctx->ok = true; break; }
            ctx->seg = next;
            attempt = 0;
            continue;
        }

        int64_t segStart = seg->start;
        int64_t segEnd   = seg->end.load();
        int64_t curDone  = seg->downloaded.load();

        RangeRequest rr;
        rr.start = segStart + curDone;
        rr.end   = (segEnd == INT64_MAX) ? -1 : segEnd;

        std::wstring err;
        int64_t before = curDone;
        int status = httpGetRange(ctx->url, rr, *ctx->headers, ctx->limiter,
            [&](const uint8_t* data, size_t len) -> bool {
                if (*ctx->abortFlag) return false;
                int64_t dnow = seg->downloaded.load();
                int64_t writeAtOff = seg->start + dnow;
                int64_t curEnd = seg->end.load();  // may have shrunk
                if (curEnd != INT64_MAX) {
                    int64_t maxBytes = curEnd - writeAtOff + 1;
                    if (maxBytes <= 0) return false;
                    if ((int64_t)len > maxBytes) len = (size_t)maxBytes;
                }
                if (len == 0) return false;
                if (!writeAt(ctx->file, writeAtOff, data, len)) return false;
                seg->downloaded.fetch_add((int64_t)len);
                // Stop when we reach the (possibly shrunk) end.
                if (seg->end.load() == INT64_MAX) return true;
                int64_t newLen = seg->end.load() - seg->start + 1;
                return seg->downloaded.load() < newLen;
            }, err);
        int64_t progressed = seg->downloaded.load() - before;

        if (*ctx->abortFlag) { ctx->error = L"aborted"; break; }

        if (seg->done()) { attempt = 0; continue; }  // either finished or stolen

        // Unknown-size stream (seg->end == INT64_MAX): a clean 2xx end == done.
        if (segEnd == INT64_MAX && status >= 200 && status < 300 && err.empty()) {
            ctx->ok = true; break;
        }

        if (progressed > 0) attempt = 0;

        if (++attempt >= kMaxRetries) {
            std::wostringstream ss;
            ss << L"seg " << ctx->index << L" gave up after "
               << kMaxRetries << L" retries (last: "
               << (err.empty() ? (L"HTTP " + std::to_wstring(status)) : err) << L")";
            ctx->error = ss.str();
            fwprintf(stderr, L"[matata] %ls\n", ctx->error.c_str());
            fflush(stderr);
            break;
        }
        int backoff = kBaseBackoffMs * (1 << (attempt - 1));
        for (int slept = 0; slept < backoff && !*ctx->abortFlag; slept += 50)
            Sleep(50);
    }

    --*ctx->activeConns;
    if (ctx->finishedCount) ctx->finishedCount->fetch_add(1);
}

} // anon

Downloader::Downloader(std::wstring url, DownloadOptions opts)
    : m_rawUrl(std::move(url)), m_opts(std::move(opts)) {}

Downloader::~Downloader() = default;

void Downloader::setProgressCallback(ProgressCallback cb) {
    m_progress = std::move(cb);
}

void Downloader::abort() { m_abort = true; }

DownloadResult Downloader::run() {
    DownloadResult res;

    Url url;
    std::wstring err;
    if (!parseUrl(m_rawUrl, url, err)) {
        res.status = DownloadStatus::NetworkError;
        res.message = L"URL parse: " + err;
        return res;
    }

    ResourceInfo info;
    if (!probeResource(url, m_opts.headers, info, err)) {
        res.status   = DownloadStatus::HttpError;
        res.httpCode = info.statusCode;
        res.message  = L"probe failed: " + err;
        return res;
    }

    if (m_opts.verbose) {
        fwprintf(stderr,
            L"[matata] probe: status=%d size=%lld ranges=%ls type=%ls file=%ls digest=%ls\n",
            info.statusCode, (long long)info.totalSize,
            info.acceptsRanges ? L"yes" : L"no",
            info.contentType.c_str(),
            info.filename.c_str(),
            digestName(info.digest.kind));
        fflush(stderr);
    }

    std::wstring name = m_opts.outputName;
    if (name.empty()) name = info.filename;
    if (name.empty()) name = info.finalUrl.inferredFilename();
    if (name.empty()) name = url.inferredFilename();

    std::wstring outDir = m_opts.outputDir;
    if (outDir.empty()) {
        wchar_t cwd[MAX_PATH];
        GetCurrentDirectoryW(MAX_PATH, cwd);
        outDir = cwd;
    }
    std::error_code ec;
    fs::create_directories(outDir, ec);

    std::wstring finalPath = uniqueOutputPath(outDir, name);
    std::wstring partPath  = finalPath + L".mtpart";
    std::wstring metaPath  = finalPath + L".mtmeta";

    DownloadMeta meta;
    bool resuming = false;
    if (fs::exists(metaPath) && fs::exists(partPath)) {
        DownloadMeta old;
        if (old.load(metaPath)) {
            bool sizeOk = (old.totalSize == info.totalSize);
            bool idOk   = true;
            if (!info.etag.empty() && !old.etag.empty())
                idOk = (info.etag == old.etag);
            else if (!info.lastModified.empty() && !old.lastModified.empty())
                idOk = (info.lastModified == old.lastModified);
            if (sizeOk && idOk) {
                meta.url          = std::move(old.url);
                meta.finalUrl     = std::move(old.finalUrl);
                meta.totalSize    = old.totalSize;
                meta.etag         = std::move(old.etag);
                meta.lastModified = std::move(old.lastModified);
                std::lock_guard<std::mutex> lk(old.segMu);
                meta.segments = std::move(old.segments);
                resuming = true;
            }
        }
    }

    const int64_t total = info.totalSize;
    const bool    knownSize   = (total > 0);
    const bool    canParallel = (knownSize && info.acceptsRanges &&
                                 total >= kSingleThreadHint &&
                                 m_opts.connections > 1);

    if (!resuming) {
        meta.url          = url.toString();
        meta.finalUrl     = info.finalUrl.toString();
        meta.totalSize    = total;
        meta.etag         = info.etag;
        meta.lastModified = info.lastModified;

        std::lock_guard<std::mutex> lk(meta.segMu);
        if (canParallel) {
            meta.segments = splitRange(total, m_opts.connections);
        } else {
            meta.segments.clear();
            int64_t end = knownSize ? (total - 1) : INT64_MAX;
            meta.segments.push_back(std::make_unique<Segment>(0, end, 0));
        }
    }

    HANDLE fh = INVALID_HANDLE_VALUE;
    if (knownSize) {
        fh = createSparseFile(partPath, total);
    } else {
        fh = CreateFileW(partPath.c_str(), GENERIC_WRITE,
                         FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                         OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    }
    if (fh == INVALID_HANDLE_VALUE) {
        res.status  = DownloadStatus::FileError;
        res.message = L"could not create " + partPath + L": " + winErr(GetLastError());
        return res;
    }

    std::atomic<int> activeConns{0};
    std::atomic<int> finishedCount{0};

    RateLimiter limiter(m_opts.bandwidthBps);

    std::vector<std::thread>               workers;
    std::vector<std::unique_ptr<WorkerCtx>> ctxs;

    // Only workers paired with a splittable-segment layout may steal.
    const bool stealEnabled = (knownSize && info.acceptsRanges);

    {
        std::lock_guard<std::mutex> lk(meta.segMu);
        for (size_t i = 0; i < meta.segments.size(); ++i) {
            auto c = std::make_unique<WorkerCtx>();
            c->index         = (int)i;
            c->url           = info.finalUrl;
            c->meta          = &meta;
            c->seg           = meta.segments[i].get();
            c->canSteal      = stealEnabled;
            c->minStealBytes = m_opts.minStealBytes;
            c->file          = fh;
            c->headers       = &m_opts.headers;
            c->limiter       = (m_opts.bandwidthBps > 0) ? &limiter : nullptr;
            c->abortFlag     = &m_abort;
            c->activeConns   = &activeConns;
            c->finishedCount = &finishedCount;
            ctxs.push_back(std::move(c));
        }
    }
    const int totalWorkers = (int)ctxs.size();
    for (auto& c : ctxs)
        workers.emplace_back(workerMain, c.get());

    auto t0 = std::chrono::steady_clock::now();
    int64_t lastBytes  = meta.totalDownloaded();
    int64_t sinceFlush = 0;
    auto lastTick = t0;

    while (true) {
        if (finishedCount.load() >= totalWorkers) break;
        if (m_abort) break;

        std::this_thread::sleep_for(std::chrono::milliseconds(200));

        int64_t now   = meta.totalDownloaded();
        auto    tnow  = std::chrono::steady_clock::now();
        auto    elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(tnow - lastTick).count();

        if (m_progress && elapsed > 0) {
            DownloadProgress p;
            p.total       = total;
            p.downloaded  = now;
            p.bytesPerSec = (int64_t)((now - lastBytes) * 1000 / elapsed);
            p.activeConns = activeConns.load();
            p.segments    = (int)meta.segmentCount();
            m_progress(p);
        }

        sinceFlush += (now - lastBytes);
        lastBytes   = now;
        lastTick    = tnow;

        if (sinceFlush >= m_opts.metaFlushEveryB) {
            meta.save(metaPath);
            sinceFlush = 0;
        }
    }

    for (auto& t : workers) if (t.joinable()) t.join();

    int64_t finalBytes = meta.totalDownloaded();
    if (m_progress) {
        DownloadProgress p;
        p.total       = total;
        p.downloaded  = finalBytes;
        p.bytesPerSec = 0;
        p.activeConns = 0;
        p.segments    = (int)meta.segmentCount();
        m_progress(p);
    }

    meta.save(metaPath);
    CloseHandle(fh);

    if (m_abort) {
        res.status     = DownloadStatus::Aborted;
        res.message    = L"aborted by caller";
        res.outputPath = partPath;
        return res;
    }
    for (auto& c : ctxs) {
        if (!c->error.empty()) {
            res.status     = DownloadStatus::NetworkError;
            res.message    = c->error;
            res.outputPath = partPath;
            return res;
        }
    }
    if (knownSize && finalBytes < total) {
        res.status     = DownloadStatus::NetworkError;
        res.message    = L"incomplete download";
        res.outputPath = partPath;
        return res;
    }

    DeleteFileW(finalPath.c_str());
    if (!MoveFileW(partPath.c_str(), finalPath.c_str())) {
        res.status     = DownloadStatus::FileError;
        res.message    = L"rename failed: " + winErr(GetLastError());
        res.outputPath = partPath;
        return res;
    }
    DeleteFileW(metaPath.c_str());

    if (m_opts.verifyChecksum && info.digest.kind != DigestKind::None) {
        if (m_opts.verbose) {
            fwprintf(stderr, L"[matata] verifying %ls checksum...\n",
                     digestName(info.digest.kind));
            fflush(stderr);
        }
        std::vector<uint8_t> got;
        std::wstring hashErr;
        if (!hashFile(finalPath, info.digest.kind, got, hashErr)) {
            res.status     = DownloadStatus::FileError;
            res.message    = L"checksum verify failed: " + hashErr;
            res.outputPath = finalPath;
            return res;
        }
        if (got != info.digest.raw) {
            DeleteFileW(finalPath.c_str());
            res.status  = DownloadStatus::FileError;
            res.message = std::wstring(L"checksum mismatch (") +
                          digestName(info.digest.kind) + L")";
            return res;
        }
    }

    res.status     = DownloadStatus::Ok;
    res.outputPath = finalPath;
    (void)resuming;
    return res;
}

}
