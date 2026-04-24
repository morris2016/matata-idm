#include "matata/segments.hpp"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <sstream>

namespace matata {

namespace {

std::string toUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(),
                                nullptr, 0, nullptr, nullptr);
    std::string out(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(),
                        out.data(), n, nullptr, nullptr);
    return out;
}

std::wstring fromUtf8(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring out(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), out.data(), n);
    return out;
}

}

std::vector<std::unique_ptr<Segment>> splitRange(int64_t totalSize, int n) {
    std::vector<std::unique_ptr<Segment>> segs;
    if (totalSize <= 0 || n <= 0) return segs;
    if (n > totalSize) n = (int)totalSize;
    int64_t chunk = totalSize / n;
    int64_t start = 0;
    for (int i = 0; i < n; ++i) {
        int64_t end = (i == n - 1) ? (totalSize - 1) : (start + chunk - 1);
        segs.push_back(std::make_unique<Segment>(start, end, 0));
        start = end + 1;
    }
    return segs;
}

size_t DownloadMeta::segmentCount() const {
    std::lock_guard<std::mutex> lk(segMu);
    return segments.size();
}

int64_t DownloadMeta::totalDownloaded() const {
    std::lock_guard<std::mutex> lk(segMu);
    int64_t sum = 0;
    for (auto& s : segments) sum += s->downloaded.load();
    return sum;
}

Segment* DownloadMeta::stealFromSlowest(int64_t minSteal, int64_t bufferAhead) {
    std::lock_guard<std::mutex> lk(segMu);

    Segment* target = nullptr;
    int64_t  bestRem = minSteal;
    for (auto& s : segments) {
        int64_t rem = s->remaining();
        if (rem > bestRem) { bestRem = rem; target = s.get(); }
    }
    if (!target) return nullptr;

    int64_t workerPos = target->start + target->downloaded.load();
    int64_t curEnd    = target->end.load();
    int64_t remain    = curEnd - workerPos + 1;
    if (remain < minSteal) return nullptr;

    // Split halfway through what's remaining, but leave at least
    // `bufferAhead` bytes ahead of the worker's current position so the
    // worker doesn't race past the new boundary before it observes it.
    int64_t splitPoint = workerPos + std::max<int64_t>(remain / 2, bufferAhead);
    if (splitPoint >= curEnd || splitPoint <= workerPos) return nullptr;

    // Shrink target; publish new end *before* adding the tail so a workers
    // mid-write sees the smaller end and clips.
    target->end.store(splitPoint - 1);

    segments.push_back(std::make_unique<Segment>(splitPoint, curEnd, 0));
    return segments.back().get();
}

bool DownloadMeta::save(const std::wstring& path) const {
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"wb") != 0 || !f) return false;

    std::lock_guard<std::mutex> lk(segMu);

    std::ostringstream os;
    os << "version=1\n";
    os << "url=" << toUtf8(url) << "\n";
    os << "final_url=" << toUtf8(finalUrl) << "\n";
    os << "total_size=" << totalSize << "\n";
    os << "etag=" << toUtf8(etag) << "\n";
    os << "last_modified=" << toUtf8(lastModified) << "\n";
    os << "segments=" << segments.size() << "\n";
    for (size_t i = 0; i < segments.size(); ++i) {
        const auto& s = segments[i];
        os << "[seg " << i << "]\n";
        os << "start=" << s->start << "\n";
        os << "end=" << s->end.load() << "\n";
        os << "downloaded=" << s->downloaded.load() << "\n";
    }
    std::string s = os.str();
    fwrite(s.data(), 1, s.size(), f);
    fclose(f);
    return true;
}

bool DownloadMeta::load(const std::wstring& path) {
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"rb") != 0 || !f) return false;

    std::string content;
    char buf[4096];
    size_t got;
    while ((got = fread(buf, 1, sizeof(buf), f)) > 0) content.append(buf, got);
    fclose(f);

    std::lock_guard<std::mutex> lk(segMu);
    segments.clear();

    bool    inSeg = false;
    int64_t curStart = 0, curEnd = 0, curDone = 0;

    auto flushSeg = [&](){
        if (inSeg) {
            segments.push_back(std::make_unique<Segment>(curStart, curEnd, curDone));
        }
        inSeg = false; curStart = curEnd = curDone = 0;
    };

    size_t i = 0;
    while (i < content.size()) {
        size_t eol = content.find('\n', i);
        if (eol == std::string::npos) eol = content.size();
        std::string line = content.substr(i, eol - i);
        i = eol + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;

        if (line.front() == '[') { flushSeg(); inSeg = true; continue; }
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = line.substr(0, eq);
        std::string v = line.substr(eq + 1);

        if (!inSeg) {
            if      (k == "url")           url           = fromUtf8(v);
            else if (k == "final_url")     finalUrl      = fromUtf8(v);
            else if (k == "total_size")    totalSize     = std::stoll(v);
            else if (k == "etag")          etag          = fromUtf8(v);
            else if (k == "last_modified") lastModified  = fromUtf8(v);
        } else {
            if      (k == "start")      curStart = std::stoll(v);
            else if (k == "end")        curEnd   = std::stoll(v);
            else if (k == "downloaded") curDone  = std::stoll(v);
        }
    }
    flushSeg();
    return true;
}

}
