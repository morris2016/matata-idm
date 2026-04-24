#pragma once
#include "matata/downloader.hpp"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace matata {

enum class ItemStatus {
    Queued,
    Running,
    Done,
    Failed,
    Aborted,
};

struct QueueItem {
    std::wstring    url;
    DownloadOptions opts;
    ItemStatus      status    = ItemStatus::Queued;
    std::wstring    outputPath;    // set on completion
    std::wstring    message;       // set on failure
    int64_t         createdEpoch = 0;
};

// Multi-download queue with a global cap on concurrent downloads AND on
// total concurrent HTTP connections (sum of per-download connections).
class DownloadQueue {
public:
    struct Config {
        int  maxConcurrentDownloads = 3;
        int  maxTotalConnections    = 16;
        bool categorize             = false;    // route by extension subdir
        std::wstring statePath;                 // plain-text persistence file

        // 0 = unset; otherwise unix epoch seconds.
        int64_t startAtEpoch = 0;   // dispatcher sleeps until this time
        int64_t stopAtEpoch  = 0;   // dispatcher stops starting new items
    };

    explicit DownloadQueue(Config cfg);
    ~DownloadQueue();

    // Append to the end of the queue; returns the item's index.
    size_t enqueue(QueueItem item);

    // Block until every item reaches a terminal state (Done/Failed/Aborted).
    void runAll();

    // Signal all running downloads to stop and the dispatcher to exit.
    void abort();

    // Thread-safe snapshot.
    std::vector<QueueItem> snapshot() const;

    // Persistence. Format is one line per item:
    //   status|url|name|dir|created_epoch
    bool saveState() const;
    bool loadState();

private:
    void dispatcherLoop();
    void runItem(size_t idx);

    Config                     m_cfg;
    mutable std::mutex         m_mu;
    std::condition_variable    m_cv;
    std::vector<QueueItem>     m_items;
    std::atomic<bool>          m_abort{false};

    // Tracking of in-flight downloads.
    int                                           m_running = 0;
    int                                           m_connsInUse = 0;
    std::vector<std::shared_ptr<Downloader>>      m_active;
};

}
