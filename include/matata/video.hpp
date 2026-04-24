#pragma once
#include "matata/downloader.hpp"
#include "matata/http.hpp"

#include <atomic>
#include <functional>
#include <string>

namespace matata {

struct VideoOptions {
    std::wstring outputDir;
    std::wstring outputName;        // empty = inferred from manifest URL
    std::wstring quality = L"best"; // best|worst|720p|1080p|<height>
    std::wstring ffmpegPath;        // if set, remux concatenated .ts to .mp4
    int          parallel = 8;
    ExtraHeaders headers;
    bool         verbose = false;
};

struct VideoProgress {
    int     segmentsDone  = 0;
    int     segmentsTotal = 0;
    int64_t bytes         = 0;
    int64_t bytesPerSec   = 0;
};

using VideoProgressCallback = std::function<void(const VideoProgress&)>;

class VideoGrabber {
public:
    VideoGrabber(std::wstring url, VideoOptions opts);
    ~VideoGrabber();

    void setProgressCallback(VideoProgressCallback cb);

    DownloadResult run();
    void abort();

private:
    DownloadResult runHls();
    DownloadResult runDash();

    std::wstring          m_rawUrl;
    VideoOptions          m_opts;
    VideoProgressCallback m_progress;
    std::atomic<bool>     m_abort{false};
};

}
