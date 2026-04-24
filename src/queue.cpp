#include "matata/queue.hpp"
#include "matata/categories.hpp"

#include <windows.h>

#include <cstdio>
#include <filesystem>
#include <sstream>
#include <thread>
#include <ctime>

namespace fs = std::filesystem;

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

const wchar_t* statusLabel(ItemStatus s) {
    switch (s) {
        case ItemStatus::Queued:  return L"queued";
        case ItemStatus::Running: return L"running";
        case ItemStatus::Done:    return L"done";
        case ItemStatus::Failed:  return L"failed";
        case ItemStatus::Aborted: return L"aborted";
    }
    return L"?";
}

ItemStatus parseStatus(const std::string& s) {
    if (s == "done")    return ItemStatus::Done;
    if (s == "failed")  return ItemStatus::Failed;
    if (s == "aborted") return ItemStatus::Aborted;
    if (s == "running") return ItemStatus::Queued; // restart in-flight items
    return ItemStatus::Queued;
}

} // anon

DownloadQueue::DownloadQueue(Config cfg) : m_cfg(std::move(cfg)) {}
DownloadQueue::~DownloadQueue() = default;

size_t DownloadQueue::enqueue(QueueItem item) {
    if (item.createdEpoch == 0) item.createdEpoch = (int64_t)std::time(nullptr);
    std::lock_guard<std::mutex> lk(m_mu);
    m_items.push_back(std::move(item));
    m_cv.notify_all();
    return m_items.size() - 1;
}

void DownloadQueue::abort() {
    std::unique_lock<std::mutex> lk(m_mu);
    m_abort = true;
    for (auto& d : m_active) if (d) d->abort();
    m_cv.notify_all();
}

std::vector<QueueItem> DownloadQueue::snapshot() const {
    std::lock_guard<std::mutex> lk(m_mu);
    return m_items;
}

void DownloadQueue::runAll() {
    std::vector<std::thread> workers;

    auto terminal = [](ItemStatus s) {
        return s == ItemStatus::Done
            || s == ItemStatus::Failed
            || s == ItemStatus::Aborted;
    };

    // Honor --start-at: hold the whole dispatcher until the chosen time.
    if (m_cfg.startAtEpoch > 0) {
        while (!m_abort) {
            int64_t now = (int64_t)std::time(nullptr);
            if (now >= m_cfg.startAtEpoch) break;
            int64_t waitSec = m_cfg.startAtEpoch - now;
            DWORD step = (waitSec > 1) ? 1000 : 200;
            Sleep(step);
        }
    }

    while (true) {
        size_t pickIdx = (size_t)-1;

        // Past stop-at? Don't dispatch new items; wait for in-flight to drain.
        bool afterStop = (m_cfg.stopAtEpoch > 0
                       && (int64_t)std::time(nullptr) >= m_cfg.stopAtEpoch);

        {
            std::unique_lock<std::mutex> lk(m_mu);
            m_cv.wait(lk, [&]{
                if (m_abort) return true;
                bool anyPending = false;
                bool anyNonTerminal = false;
                for (auto& it : m_items) {
                    if (!terminal(it.status)) anyNonTerminal = true;
                    if (it.status == ItemStatus::Queued) anyPending = true;
                }
                if (!anyNonTerminal) return true;
                if (afterStop) return m_running == 0; // exit once drained
                if (!anyPending) return false;
                if (m_running >= m_cfg.maxConcurrentDownloads) return false;
                return true;
            });

            if (m_abort) break;

            bool anyNonTerminal = false;
            for (auto& it : m_items)
                if (!terminal(it.status)) { anyNonTerminal = true; break; }
            if (!anyNonTerminal) break;
            if (afterStop && m_running == 0) break;
            if (afterStop) continue;

            for (size_t i = 0; i < m_items.size(); ++i) {
                if (m_items[i].status != ItemStatus::Queued) continue;
                int need = m_items[i].opts.connections;
                if (need < 1) need = 1;
                int budget = m_cfg.maxTotalConnections - m_connsInUse;
                int use    = (need <= budget) ? need : (budget > 0 ? budget : 0);
                if (use == 0) continue;
                if (use < need) m_items[i].opts.connections = use;
                m_items[i].status = ItemStatus::Running;
                m_connsInUse += use;
                ++m_running;
                pickIdx = i;
                break;
            }
        }

        if (pickIdx != (size_t)-1) {
            workers.emplace_back([this, pickIdx]{ runItem(pickIdx); });
        }

        // If we have stop-at set but haven't hit it yet, wake periodically
        // so we re-evaluate the window even if no download state changes.
        if (m_cfg.stopAtEpoch > 0 && !afterStop && pickIdx == (size_t)-1) {
            Sleep(500);
        }
    }

    for (auto& t : workers) if (t.joinable()) t.join();

    if (!m_cfg.statePath.empty()) saveState();
}

void DownloadQueue::runItem(size_t idx) {
    QueueItem snap;
    {
        std::lock_guard<std::mutex> lk(m_mu);
        snap = m_items[idx];
    }

    DownloadOptions opts = snap.opts;

    if (m_cfg.categorize && opts.outputDir.empty() == false) {
        std::wstring inferred = opts.outputName;
        if (inferred.empty()) {
            // Use URL tail as a rough hint for categorization.
            auto slash = snap.url.find_last_of(L'/');
            inferred = (slash == std::wstring::npos) ? snap.url : snap.url.substr(slash + 1);
            auto q = inferred.find(L'?');
            if (q != std::wstring::npos) inferred.resize(q);
        }
        const Category* cat = categorize(inferred);
        if (cat && !cat->subdir.empty()) {
            wchar_t sep = L'\\';
            if (!opts.outputDir.empty()) {
                wchar_t last = opts.outputDir.back();
                if (last == L'\\' || last == L'/') sep = L'\0';
            }
            if (sep == L'\0') opts.outputDir += cat->subdir;
            else              opts.outputDir += sep + cat->subdir;
        }
    }

    auto dl = std::make_shared<Downloader>(snap.url, opts);
    {
        std::lock_guard<std::mutex> lk(m_mu);
        m_active.push_back(dl);
    }

    dl->setProgressCallback([](const DownloadProgress&){});

    DownloadResult r = dl->run();

    {
        std::unique_lock<std::mutex> lk(m_mu);
        for (auto it = m_active.begin(); it != m_active.end(); ++it) {
            if (it->get() == dl.get()) { m_active.erase(it); break; }
        }

        QueueItem& item = m_items[idx];
        switch (r.status) {
        case DownloadStatus::Ok:      item.status = ItemStatus::Done;    break;
        case DownloadStatus::Aborted: item.status = ItemStatus::Aborted; break;
        default:                      item.status = ItemStatus::Failed;  break;
        }
        item.outputPath = r.outputPath;
        item.message    = r.message;

        m_connsInUse -= opts.connections;
        if (m_connsInUse < 0) m_connsInUse = 0;
        --m_running;
    }
    m_cv.notify_all();

    if (!m_cfg.statePath.empty()) saveState();
}

bool DownloadQueue::saveState() const {
    if (m_cfg.statePath.empty()) return false;

    std::lock_guard<std::mutex> lk(m_mu);

    FILE* f = nullptr;
    if (_wfopen_s(&f, m_cfg.statePath.c_str(), L"wb") != 0 || !f) return false;

    auto escapeField = [](std::wstring s) {
        for (auto& c : s) if (c == L'|' || c == L'\n' || c == L'\r') c = L' ';
        return s;
    };

    std::string buf;
    buf.reserve(m_items.size() * 128);
    buf += "# matata queue state v1\n";
    buf += "# status|url|name|dir|created_epoch\n";

    for (auto& it : m_items) {
        std::wstringstream ss;
        ss << statusLabel(it.status) << L"|"
           << escapeField(it.url) << L"|"
           << escapeField(it.opts.outputName) << L"|"
           << escapeField(it.opts.outputDir) << L"|"
           << it.createdEpoch << L"\n";
        buf += toUtf8(ss.str());
    }
    fwrite(buf.data(), 1, buf.size(), f);
    fclose(f);
    return true;
}

bool DownloadQueue::loadState() {
    if (m_cfg.statePath.empty()) return false;
    if (!fs::exists(m_cfg.statePath)) return false;

    FILE* f = nullptr;
    if (_wfopen_s(&f, m_cfg.statePath.c_str(), L"rb") != 0 || !f) return false;

    std::string content;
    char cb[4096];
    size_t got;
    while ((got = fread(cb, 1, sizeof(cb), f)) > 0) content.append(cb, got);
    fclose(f);

    std::lock_guard<std::mutex> lk(m_mu);

    size_t i = 0;
    while (i < content.size()) {
        size_t eol = content.find('\n', i);
        if (eol == std::string::npos) eol = content.size();
        std::string line = content.substr(i, eol - i);
        i = eol + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '#') continue;

        std::vector<std::string> fields;
        size_t start = 0;
        while (start <= line.size()) {
            size_t bar = line.find('|', start);
            if (bar == std::string::npos) { fields.push_back(line.substr(start)); break; }
            fields.push_back(line.substr(start, bar - start));
            start = bar + 1;
        }
        if (fields.size() < 5) continue;

        QueueItem it;
        it.status            = parseStatus(fields[0]);
        it.url               = fromUtf8(fields[1]);
        it.opts.outputName   = fromUtf8(fields[2]);
        it.opts.outputDir    = fromUtf8(fields[3]);
        try { it.createdEpoch = std::stoll(fields[4]); } catch (...) {}

        m_items.push_back(std::move(it));
    }
    return true;
}

}
