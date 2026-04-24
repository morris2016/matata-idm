#pragma once
#include "matata/http.hpp"
#include "matata/segments.hpp"
#include "matata/url.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace matata {

class RateLimiter;

struct DownloadOptions {
    int          connections = 8;
    std::wstring outputDir;        // if empty, current directory
    std::wstring outputName;       // if empty, inferred from URL / headers
    int64_t      metaFlushEveryB = 1 << 20;    // persist .meta every ~1 MiB

    // v0.2 additions
    ExtraHeaders headers;               // extra request headers (cookie, ref, UA)
    int64_t      bandwidthBps  = 0;     // 0 = unlimited
    bool         verbose       = false; // print probe diagnostic to stderr
    int64_t      minStealBytes = 1024 * 1024; // min remaining for a steal
    bool         verifyChecksum = true; // verify advertised server digest on finish
};

struct DownloadProgress {
    int64_t total        = -1;
    int64_t downloaded   = 0;
    int64_t bytesPerSec  = 0;
    int     activeConns  = 0;
    int     segments     = 0; // current segment count (grows with stealing)
};

using ProgressCallback = std::function<void(const DownloadProgress&)>;

enum class DownloadStatus {
    Ok,
    NetworkError,
    HttpError,
    FileError,
    Aborted,
};

struct DownloadResult {
    DownloadStatus status = DownloadStatus::Ok;
    int            httpCode = 0;
    std::wstring   message;
    std::wstring   outputPath;
};

class Downloader {
public:
    Downloader(std::wstring url, DownloadOptions opts);
    ~Downloader();

    void setProgressCallback(ProgressCallback cb);

    DownloadResult run();
    void abort();

private:
    std::wstring     m_rawUrl;
    DownloadOptions  m_opts;
    ProgressCallback m_progress;
    std::atomic<bool> m_abort{false};
};

}
