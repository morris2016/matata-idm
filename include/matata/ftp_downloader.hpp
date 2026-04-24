#pragma once
#include "matata/downloader.hpp"

#include <atomic>
#include <memory>
#include <string>

namespace matata {

class FtpClient;

// FtpDownloader mirrors `Downloader`'s interface so callers (CLI, GUI)
// can substitute it for ftp:// URLs. Single-stream transfer; resume via
// REST when a .mtpart file already exists.
class FtpDownloader {
public:
    FtpDownloader(std::wstring url, DownloadOptions opts);
    ~FtpDownloader();

    void setProgressCallback(ProgressCallback cb);
    DownloadResult run();
    void abort();

private:
    std::wstring      m_rawUrl;
    DownloadOptions   m_opts;
    ProgressCallback  m_progress;
    std::atomic<bool> m_abort{false};
    std::unique_ptr<FtpClient> m_client;  // held so abort() can cancel
};

}
