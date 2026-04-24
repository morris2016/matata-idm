#pragma once
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace matata {

// A contiguous byte range [start, end] of a file being downloaded.
// - `start` is immutable after construction.
// - `end` can SHRINK via dynamic stealing (never grow past its original value).
// - `downloaded` is advanced only by the owning worker thread; readable
//   from any thread via relaxed load.
struct Segment {
    int64_t              start = 0;
    std::atomic<int64_t> end{0};
    std::atomic<int64_t> downloaded{0};

    Segment() = default;
    Segment(int64_t s, int64_t e, int64_t d = 0)
        : start(s), end(e), downloaded(d) {}
    Segment(const Segment&)            = delete;
    Segment& operator=(const Segment&) = delete;

    int64_t length()    const { return end.load() - start + 1; }
    int64_t remaining() const { return length() - downloaded.load(); }
    bool    done()      const { return downloaded.load() >= length(); }
};

struct DownloadMeta {
    std::wstring  url;
    std::wstring  finalUrl;
    int64_t       totalSize = -1;
    std::wstring  etag;
    std::wstring  lastModified;

    mutable std::mutex                    segMu;
    std::vector<std::unique_ptr<Segment>> segments;

    // Thread-safe.
    size_t   segmentCount() const;
    int64_t  totalDownloaded() const;

    // Pick the segment with the largest remaining work, split it at a
    // safe point (at least `bufferAhead` bytes past its current downloaded
    // position), and return a pointer to the newly-created tail segment
    // that a new worker should claim.
    // Returns nullptr if no segment has at least `minSteal` remaining.
    Segment* stealFromSlowest(int64_t minSteal, int64_t bufferAhead);

    bool save(const std::wstring& path) const;
    bool load(const std::wstring& path);
};

// Split [0, totalSize-1] into n roughly-equal segments.
std::vector<std::unique_ptr<Segment>> splitRange(int64_t totalSize, int n);

}
