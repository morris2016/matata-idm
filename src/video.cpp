#include "matata/video.hpp"
#include "matata/aes.hpp"
#include "matata/dash.hpp"
#include "matata/hls.hpp"
#include "matata/url.hpp"

#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <cwctype>
#include <deque>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <numeric>
#include <sstream>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

namespace matata {

namespace {

constexpr int     kSegmentRetries = 3;
constexpr int64_t kCopyBufferSize = 1 << 20;   // 1 MiB

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

std::wstring inferStem(const Url& manifestUrl) {
    std::wstring p = manifestUrl.path;
    auto slash = p.find_last_of(L'/');
    std::wstring leaf = (slash == std::wstring::npos) ? p : p.substr(slash + 1);
    if (leaf.empty()) leaf = L"stream";
    // strip trailing .m3u8
    auto dot = leaf.find_last_of(L'.');
    if (dot != std::wstring::npos) leaf = leaf.substr(0, dot);
    // If it's just "master" or "index" or "playlist", prefer the parent dir.
    std::wstring lc;
    lc.reserve(leaf.size());
    for (wchar_t c : leaf) lc.push_back((wchar_t)towlower(c));
    if (lc == L"master" || lc == L"index" || lc == L"playlist") {
        if (slash != std::wstring::npos && slash > 0) {
            std::wstring parent = manifestUrl.path.substr(0, slash);
            auto pslash = parent.find_last_of(L'/');
            std::wstring pleaf = (pslash == std::wstring::npos) ? parent
                                                                : parent.substr(pslash + 1);
            if (!pleaf.empty()) leaf = pleaf;
        }
    }
    // sanitize for NTFS
    const std::wstring illegal = L"<>:\"/\\|?*";
    std::wstring out;
    for (wchar_t c : leaf) {
        if (illegal.find(c) != std::wstring::npos || c < 32) continue;
        out.push_back(c);
    }
    if (out.empty()) out = L"stream";
    return out;
}

bool writeAllToFile(const std::wstring& path, const std::string& bytes,
                    std::wstring& err) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        err = L"create seg file: " + winErr(GetLastError());
        return false;
    }
    const char* p = bytes.data();
    size_t      remaining = bytes.size();
    while (remaining > 0) {
        DWORD toWrite = remaining > (1u << 30) ? (1u << 30) : (DWORD)remaining;
        DWORD written = 0;
        if (!WriteFile(h, p, toWrite, &written, nullptr) || written == 0) {
            err = L"write seg file: " + winErr(GetLastError());
            CloseHandle(h);
            return false;
        }
        p         += written;
        remaining -= written;
    }
    CloseHandle(h);
    return true;
}

bool concatFiles(const std::vector<std::wstring>& inputs,
                 const std::wstring& output,
                 std::atomic<bool>* abortFlag,
                 std::atomic<int64_t>& bytesCopied,
                 std::wstring& err) {
    HANDLE out = CreateFileW(output.c_str(), GENERIC_WRITE, 0, nullptr,
                             CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (out == INVALID_HANDLE_VALUE) {
        err = L"create output: " + winErr(GetLastError());
        return false;
    }
    std::vector<uint8_t> buf((size_t)kCopyBufferSize);
    for (auto& in : inputs) {
        if (abortFlag && abortFlag->load()) { err = L"aborted"; CloseHandle(out); return false; }
        HANDLE h = CreateFileW(in.c_str(), GENERIC_READ, FILE_SHARE_READ,
                               nullptr, OPEN_EXISTING,
                               FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        if (h == INVALID_HANDLE_VALUE) {
            err = L"open " + in + L": " + winErr(GetLastError());
            CloseHandle(out); return false;
        }
        for (;;) {
            DWORD got = 0;
            if (!ReadFile(h, buf.data(), (DWORD)buf.size(), &got, nullptr)) {
                err = L"read " + in + L": " + winErr(GetLastError());
                CloseHandle(h); CloseHandle(out); return false;
            }
            if (got == 0) break;
            DWORD wrote = 0;
            if (!WriteFile(out, buf.data(), got, &wrote, nullptr) || wrote != got) {
                err = L"write output: " + winErr(GetLastError());
                CloseHandle(h); CloseHandle(out); return false;
            }
            bytesCopied.fetch_add((int64_t)wrote);
        }
        CloseHandle(h);
    }
    CloseHandle(out);
    return true;
}

bool runFfmpegRemux(const std::wstring& ffmpegPath,
                    const std::wstring& inputTs,
                    const std::wstring& outputMp4,
                    std::wstring& err) {
    std::wstring cmd = L"\"" + ffmpegPath + L"\" -y -loglevel error"
                       L" -i \"" + inputTs + L"\" -c copy -bsf:a aac_adtstoasc \""
                     + outputMp4 + L"\"";

    STARTUPINFOW         si{}; si.cb = sizeof(si);
    PROCESS_INFORMATION  pi{};

    std::vector<wchar_t> cmdBuf(cmd.begin(), cmd.end());
    cmdBuf.push_back(L'\0');

    if (!CreateProcessW(nullptr, cmdBuf.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        err = L"ffmpeg launch failed: " + winErr(GetLastError());
        return false;
    }
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD rc = 1;
    GetExitCodeProcess(pi.hProcess, &rc);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    if (rc != 0) {
        std::wostringstream s; s << L"ffmpeg exit " << rc;
        err = s.str();
        return false;
    }
    return true;
}

struct SegJob {
    int          index = 0;
    std::wstring url;
    std::wstring outPath;
    bool         done = false;
    std::wstring error;
    int64_t      bytes = 0;
    // AES-128 (optional). `encrypted` controls whether `key`/`iv` are used.
    bool         encrypted = false;
    uint8_t      key[16]{};
    uint8_t      iv[16]{};
};

struct SegPool {
    std::mutex            mu;
    std::deque<SegJob>    jobs;              // pointer-stable on push_back
    size_t                nextIdx = 0;       // guarded by `mu`
    std::atomic<bool>     addingDone{false}; // main is done appending
    std::atomic<int>      completed{0};
    std::atomic<int64_t>  totalBytes{0};
    std::atomic<bool>*    abortFlag = nullptr;
    const ExtraHeaders*   headers = nullptr;

    size_t size() {
        std::lock_guard<std::mutex> lk(mu);
        return jobs.size();
    }
    void append(SegJob&& j) {
        std::lock_guard<std::mutex> lk(mu);
        jobs.push_back(std::move(j));
    }
    // Claim the next unworked job slot. Returns nullptr when the queue is
    // currently drained; `shouldRetry` says whether to sleep + try again.
    SegJob* claim(bool& shouldRetry) {
        std::lock_guard<std::mutex> lk(mu);
        if (nextIdx < jobs.size()) {
            SegJob* p = &jobs[nextIdx];
            ++nextIdx;
            shouldRetry = true;
            return p;
        }
        shouldRetry = !addingDone.load();
        return nullptr;
    }
};

void segmentWorker(SegPool* pool) {
    while (true) {
        if (pool->abortFlag && pool->abortFlag->load()) return;

        bool   retry = true;
        SegJob* slot = pool->claim(retry);
        if (!slot) {
            if (!retry) return;
            Sleep(100);       // queue temporarily empty; poll
            continue;
        }
        SegJob& job = *slot;

        Url u;
        std::wstring err;
        if (!parseUrl(job.url, u, err)) {
            job.error = L"parse url: " + err;
            pool->completed.fetch_add(1);
            continue;
        }

        std::string body;
        int status = 0;
        // Total retry budget — HLS CDNs (e.g. neonhorizonworkshops, Vimeo)
        // start serving 429 after a couple hundred segments at 8x parallel.
        // We need to ride the rate-limit out, not give up after 3 quick
        // retries. 8 attempts with exponential backoff lets a single
        // segment recover from up to ~30s of cooldown.
        constexpr int kMaxAttempts = 8;
        for (int attempt = 0; attempt < kMaxAttempts; ++attempt) {
            if (pool->abortFlag && pool->abortFlag->load()) break;
            body.clear();
            err.clear();
            status = httpFetchBody(u, *pool->headers, body, err, 512ull * 1024ull * 1024ull);
            if (status >= 200 && status < 300 && !body.empty()) break;
            // 429 = rate limited. Sleep dramatically longer than for other
            // errors so the entire pool naturally backs off (every worker
            // hitting 429 will be sleeping at the same time, giving the CDN
            // a full break instead of a steady stream of fresh requests).
            DWORD ms;
            if (status == 429) {
                ms = (DWORD)(1500 * (1 << std::min(attempt, 5))); // 1.5s..48s
            } else {
                ms = (DWORD)(300 * (1 << std::min(attempt, 4)));  // 0.3s..4.8s
            }
            Sleep(ms);
        }
        if (status < 200 || status >= 300 || body.empty()) {
            std::wostringstream s;
            s << L"seg " << job.index << L" HTTP " << status;
            if (!err.empty()) s << L" (" << err << L")";
            job.error = s.str();
            pool->completed.fetch_add(1);
            continue;
        }

        // AES-128 decrypt if this segment is keyed.
        if (job.encrypted) {
            std::vector<uint8_t> plain;
            std::wstring decErr;
            if (!aes128CbcDecrypt(job.key, job.iv,
                                  (const uint8_t*)body.data(), body.size(),
                                  plain, decErr)) {
                std::wostringstream s;
                s << L"seg " << job.index << L" decrypt: " << decErr;
                job.error = s.str();
                pool->completed.fetch_add(1);
                continue;
            }
            body.assign((const char*)plain.data(), plain.size());
        }

        if (!writeAllToFile(job.outPath, body, err)) {
            job.error = err;
            pool->completed.fetch_add(1);
            continue;
        }
        job.bytes = (int64_t)body.size();
        pool->totalBytes.fetch_add(job.bytes);
        job.done = true;
        pool->completed.fetch_add(1);
    }
}

// Fetch HLS AES-128 key blob (16 raw bytes). Caches by URI.
bool fetchHlsKey(const std::wstring& keyUri,
                 const ExtraHeaders& headers,
                 std::map<std::wstring, std::array<uint8_t, 16>>& cache,
                 uint8_t outKey[16],
                 std::wstring& err) {
    auto it = cache.find(keyUri);
    if (it != cache.end()) {
        std::memcpy(outKey, it->second.data(), 16);
        return true;
    }
    Url ku;
    if (!parseUrl(keyUri, ku, err)) return false;
    std::string body;
    int status = httpFetchBody(ku, headers, body, err, 4096);
    if (status < 200 || status >= 300) {
        if (err.empty()) {
            std::wostringstream s; s << L"key HTTP " << status;
            err = s.str();
        }
        return false;
    }
    if (body.size() != 16) {
        std::wostringstream s;
        s << L"key blob wrong size (" << body.size() << L" != 16)";
        err = s.str();
        return false;
    }
    std::array<uint8_t, 16> arr{};
    std::memcpy(arr.data(), body.data(), 16);
    cache[keyUri] = arr;
    std::memcpy(outKey, arr.data(), 16);
    return true;
}

} // anon

VideoGrabber::VideoGrabber(std::wstring url, VideoOptions opts)
    : m_rawUrl(std::move(url)), m_opts(std::move(opts)) {}

VideoGrabber::~VideoGrabber() = default;

void VideoGrabber::setProgressCallback(VideoProgressCallback cb) {
    m_progress = std::move(cb);
}

void VideoGrabber::abort() { m_abort = true; }

DownloadResult VideoGrabber::run() {
    if (looksLikeDashUrl(m_rawUrl)) return runDash();
    return runHls();
}

DownloadResult VideoGrabber::runHls() {
    DownloadResult res;

    Url manifestUrl;
    std::wstring err;
    if (!parseUrl(m_rawUrl, manifestUrl, err)) {
        res.status  = DownloadStatus::NetworkError;
        res.message = L"URL parse: " + err;
        return res;
    }

    HlsManifest manifest;
    if (!fetchAndParseHls(manifestUrl, m_opts.headers, manifest, err)) {
        res.status  = DownloadStatus::NetworkError;
        res.message = L"HLS fetch/parse: " + err;
        return res;
    }

    // If master, pick a variant and re-fetch its media playlist.
    if (manifest.isMaster) {
        const HlsVariant* v = pickVariant(manifest.variants, m_opts.quality);
        if (!v) {
            res.status  = DownloadStatus::NetworkError;
            res.message = L"no variants in master manifest";
            return res;
        }
        if (m_opts.verbose) {
            fwprintf(stderr,
                L"[matata] HLS master: %zu variants; picked %dx%d @ %lld kbps\n",
                manifest.variants.size(), v->width, v->height,
                (long long)(v->bandwidth / 1000));
            fflush(stderr);
        }
        Url childUrl;
        if (!parseUrl(v->url, childUrl, err)) {
            res.status  = DownloadStatus::NetworkError;
            res.message = L"variant URL parse: " + err;
            return res;
        }
        HlsManifest child;
        if (!fetchAndParseHls(childUrl, m_opts.headers, child, err)) {
            res.status  = DownloadStatus::NetworkError;
            res.message = L"variant fetch/parse: " + err;
            return res;
        }
        if (child.isMaster) {
            res.status  = DownloadStatus::NetworkError;
            res.message = L"nested master playlists not supported";
            return res;
        }
        manifest = std::move(child);
    }

    HlsMedia& media = manifest.media;
    if (media.segments.empty() && media.endList) {
        res.status  = DownloadStatus::NetworkError;
        res.message = L"no segments in media playlist";
        return res;
    }
    if (media.unsupportedCrypto) {
        res.status  = DownloadStatus::NetworkError;
        res.message = L"unsupported HLS crypto (only AES-128 is supported)";
        return res;
    }
    if (media.isFmp4 && m_opts.ffmpegPath.empty()) {
        res.status  = DownloadStatus::NetworkError;
        res.message = L"fMP4 segments require --ffmpeg for muxing";
        return res;
    }

    // Output paths.
    std::wstring outDir = m_opts.outputDir;
    if (outDir.empty()) {
        wchar_t cwd[MAX_PATH];
        GetCurrentDirectoryW(MAX_PATH, cwd);
        outDir = cwd;
    }
    std::error_code ec;
    fs::create_directories(outDir, ec);

    std::wstring stem = m_opts.outputName.empty()
        ? inferStem(manifest.manifestUrl)
        : m_opts.outputName;
    // If user-supplied name has an extension, strip it for our internal ".ts" stage.
    {
        auto dot = stem.find_last_of(L'.');
        if (dot != std::wstring::npos) stem = stem.substr(0, dot);
    }

    std::wstring partDir  = joinPath(outDir, stem + L".parts");
    fs::create_directories(partDir, ec);

    std::wstring tsPath  = joinPath(outDir, stem + L".ts");
    std::wstring mp4Path = joinPath(outDir, stem + L".mp4");

    if (m_opts.verbose) {
        const wchar_t* mode = media.endList ? L"VOD" : L"LIVE";
        fwprintf(stderr,
            L"[matata] media playlist (%ls): %zu segments, ~%.0fs of media\n",
            mode, media.segments.size(),
            std::accumulate(media.segments.begin(), media.segments.end(),
                            0.0, [](double a, const HlsSegment& s){ return a + s.duration; }));
        fflush(stderr);
    }

    // Pool owns the (growable) job queue. Workers start now; main keeps
    // appending over time if the playlist is live.
    SegPool pool;
    pool.abortFlag = &m_abort;
    pool.headers   = &m_opts.headers;

    std::map<std::wstring, std::array<uint8_t, 16>> keyCache;
    int nextJobIdx = 0;

    auto addJob = [&](const std::wstring& url, const HlsKey* key,
                      int64_t seq) -> bool {
        SegJob j;
        j.index = nextJobIdx;
        j.url   = url;
        wchar_t buf[64];
        _snwprintf_s(buf, _TRUNCATE, L"seg%06d.bin", nextJobIdx);
        j.outPath = joinPath(partDir, buf);
        if (key && key->method == HlsCryptMethod::Aes128 && !key->keyUri.empty()) {
            std::wstring kerr;
            if (!fetchHlsKey(key->keyUri, m_opts.headers, keyCache, j.key, kerr)) {
                err = kerr;
                return false;
            }
            if (key->hasExplicitIv) std::memcpy(j.iv, key->iv, 16);
            else                    deriveIvFromSequence(seq, j.iv);
            j.encrypted = true;
        }
        pool.append(std::move(j));
        ++nextJobIdx;
        return true;
    };

    // fMP4 init segment first (no key; #EXT-X-MAP is not keyed in typical HLS).
    if (media.isFmp4 && !media.initUri.empty()) {
        if (!addJob(media.initUri, nullptr, 0)) {
            res.status  = DownloadStatus::NetworkError;
            res.message = L"fetch key: " + err;
            return res;
        }
    }

    // Track the highest media-sequence we've queued. On live refetch we only
    // enqueue strictly newer segments.
    int64_t highestQueuedSeq = -1;
    auto queueSegment = [&](const HlsSegment& s) -> bool {
        if (s.sequence <= highestQueuedSeq) return true;
        if (!addJob(s.url, &s.key, s.sequence)) return false;
        highestQueuedSeq = s.sequence;
        return true;
    };
    for (auto& s : media.segments) {
        if (!queueSegment(s)) {
            res.status  = DownloadStatus::NetworkError;
            res.message = L"fetch key: " + err;
            return res;
        }
    }

    std::vector<std::thread> workers;
    int parallel = std::max(1, std::min(m_opts.parallel, (int)pool.size()));
    if (parallel < 1) parallel = 1;
    for (int i = 0; i < parallel; ++i) workers.emplace_back(segmentWorker, &pool);

    auto t0 = std::chrono::steady_clock::now();
    int64_t lastBytes = 0;
    auto    lastTick  = t0;

    auto tickProgress = [&]() {
        int64_t now  = pool.totalBytes.load();
        auto    tnow = std::chrono::steady_clock::now();
        auto    elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(tnow - lastTick).count();
        if (m_progress && elapsed > 0) {
            VideoProgress p;
            p.segmentsDone  = pool.completed.load();
            p.segmentsTotal = (int)pool.size();
            p.bytes         = now;
            p.bytesPerSec   = (int64_t)((now - lastBytes) * 1000 / elapsed);
            m_progress(p);
        }
        lastBytes = now;
        lastTick  = tnow;
    };

    // VOD: exit when all queued segments finish.
    // LIVE: refetch the playlist every ~targetDuration/2 and enqueue anything
    // new, exit when we see #EXT-X-ENDLIST or the user aborts.
    bool live        = !media.endList;
    int  pollSeconds = std::max(1, media.targetDuration ? media.targetDuration / 2 : 3);
    auto lastPoll    = std::chrono::steady_clock::now();

    while (!m_abort) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        tickProgress();

        if (live) {
            auto tnow  = std::chrono::steady_clock::now();
            auto since = std::chrono::duration_cast<std::chrono::seconds>(tnow - lastPoll).count();
            if (since >= pollSeconds) {
                lastPoll = tnow;
                HlsManifest refetch;
                std::wstring ferr;
                if (fetchAndParseHls(manifest.manifestUrl, m_opts.headers,
                                     refetch, ferr) && !refetch.isMaster) {
                    for (auto& s : refetch.media.segments) {
                        if (!queueSegment(s)) {
                            fwprintf(stderr, L"[matata] live queue error: %ls\n", err.c_str());
                            fflush(stderr);
                            m_abort = true;
                            break;
                        }
                    }
                    if (refetch.media.endList) live = false;
                } else if (m_opts.verbose) {
                    fwprintf(stderr, L"[matata] live refetch failed: %ls\n", ferr.c_str());
                    fflush(stderr);
                }
            }
            // Exit once endList appears AND we've processed everything queued.
            if (!live && pool.completed.load() >= (int)pool.size()) break;
            continue;
        }

        if (pool.completed.load() >= (int)pool.size()) break;
    }

    // No more jobs will arrive; let workers drain then exit.
    pool.addingDone.store(true);
    for (auto& t : workers) if (t.joinable()) t.join();

    if (m_abort) {
        res.status = DownloadStatus::Aborted;
        res.message = L"aborted";
        return res;
    }

    // Collect output paths in order; check for per-segment errors.
    std::vector<std::wstring> inputs;
    {
        std::lock_guard<std::mutex> lk(pool.mu);
        inputs.reserve(pool.jobs.size());
        for (auto& j : pool.jobs) {
            if (!j.done) {
                res.status  = DownloadStatus::NetworkError;
                res.message = j.error.empty() ? L"unknown segment failure" : j.error;
                return res;
            }
            inputs.push_back(j.outPath);
        }
    }

    std::atomic<int64_t> copied{0};
    if (!concatFiles(inputs, tsPath, &m_abort, copied, err)) {
        res.status  = DownloadStatus::FileError;
        res.message = L"concat: " + err;
        res.outputPath = tsPath;
        return res;
    }

    // Optional ffmpeg remux.
    std::wstring finalPath = tsPath;
    if (!m_opts.ffmpegPath.empty()) {
        if (m_opts.verbose) {
            fwprintf(stderr, L"[matata] remuxing to mp4 via ffmpeg...\n");
            fflush(stderr);
        }
        if (!runFfmpegRemux(m_opts.ffmpegPath, tsPath, mp4Path, err)) {
            res.status  = DownloadStatus::FileError;
            res.message = L"ffmpeg remux failed: " + err;
            res.outputPath = tsPath;   // .ts is still on disk for the user
            return res;
        }
        DeleteFileW(tsPath.c_str());
        finalPath = mp4Path;
    }

    // Cleanup parts directory.
    for (auto& p : inputs) DeleteFileW(p.c_str());
    RemoveDirectoryW(partDir.c_str());

    res.status     = DownloadStatus::Ok;
    res.outputPath = finalPath;
    return res;
}

DownloadResult VideoGrabber::runDash() {
    DownloadResult res;

    Url manifestUrl;
    std::wstring err;
    if (!parseUrl(m_rawUrl, manifestUrl, err)) {
        res.status  = DownloadStatus::NetworkError;
        res.message = L"URL parse: " + err;
        return res;
    }

    DashManifest manifest;
    if (!fetchAndParseDash(manifestUrl, m_opts.headers, manifest, err)) {
        res.status  = DownloadStatus::NetworkError;
        res.message = L"DASH fetch/parse: " + err;
        return res;
    }
    if (manifest.unsupported) {
        res.status  = DownloadStatus::NetworkError;
        res.message = manifest.warning;
        return res;
    }

    const DashRepresentation* vid = pickDashVariant(manifest.videoReps, m_opts.quality);
    if (!vid && !manifest.audioReps.empty()) {
        // Audio-only MPD (rare): fall back to picking an audio rep.
        vid = pickDashVariant(manifest.audioReps, m_opts.quality);
    }
    if (!vid) {
        res.status  = DownloadStatus::NetworkError;
        res.message = L"no video representations in MPD";
        return res;
    }

    if (m_opts.verbose) {
        fwprintf(stderr,
            L"[matata] DASH: %zu video reps, %zu audio reps; "
            L"picked id=%ls %dx%d @ %lld kbps (%zu segments)\n",
            manifest.videoReps.size(), manifest.audioReps.size(),
            vid->id.c_str(), vid->width, vid->height,
            (long long)(vid->bandwidth / 1000),
            vid->segmentUrls.size());
        if (manifest.audioReps.empty() == false && m_opts.ffmpegPath.empty()) {
            fwprintf(stderr,
                L"[matata] note: audio reps present but will be SKIPPED "
                L"(pass --ffmpeg PATH to mux video + audio)\n");
        }
        fflush(stderr);
    }

    // Paths.
    std::wstring outDir = m_opts.outputDir;
    if (outDir.empty()) {
        wchar_t cwd[MAX_PATH];
        GetCurrentDirectoryW(MAX_PATH, cwd);
        outDir = cwd;
    }
    std::error_code ec;
    fs::create_directories(outDir, ec);

    std::wstring stem = m_opts.outputName.empty()
        ? inferStem(manifest.manifestUrl)
        : m_opts.outputName;
    {
        auto dot = stem.find_last_of(L'.');
        if (dot != std::wstring::npos) stem = stem.substr(0, dot);
    }

    std::wstring partDir = joinPath(outDir, stem + L".parts");
    fs::create_directories(partDir, ec);

    // DASH segments are fMP4 fragments. The concat we produce (init + media)
    // is a valid fragmented MP4 container.
    std::wstring videoPath = joinPath(outDir, stem + L".video.mp4");
    std::wstring audioPath = joinPath(outDir, stem + L".audio.mp4");
    std::wstring muxedPath = joinPath(outDir, stem + L".mp4");

    auto downloadRep = [&](const DashRepresentation& rep,
                           const std::wstring& outPath,
                           const std::wstring& tag) -> std::wstring {
        SegPool pool;
        pool.abortFlag = &m_abort;
        pool.headers   = &m_opts.headers;

        int idx = 0;
        auto push = [&](const std::wstring& url) {
            SegJob j;
            j.index = idx;
            j.url   = url;
            wchar_t buf[64];
            _snwprintf_s(buf, _TRUNCATE, L"%ls-%06d.bin", tag.c_str(), idx);
            j.outPath = joinPath(partDir, buf);
            pool.append(std::move(j));
            ++idx;
        };
        if (!rep.initUrl.empty()) push(rep.initUrl);
        for (auto& u : rep.segmentUrls) push(u);

        std::vector<std::thread> workers;
        int parallel = (std::max)(1, (std::min)(m_opts.parallel, (int)pool.size()));
        for (int i = 0; i < parallel; ++i)
            workers.emplace_back(segmentWorker, &pool);

        auto lastTick = std::chrono::steady_clock::now();
        int64_t lastBytes = 0;
        while (pool.completed.load() < (int)pool.size() && !m_abort) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            int64_t now  = pool.totalBytes.load();
            auto    tnow = std::chrono::steady_clock::now();
            auto    elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(tnow - lastTick).count();
            if (m_progress && elapsed > 0) {
                VideoProgress p;
                p.segmentsDone  = pool.completed.load();
                p.segmentsTotal = (int)pool.size();
                p.bytes         = now;
                p.bytesPerSec   = (int64_t)((now - lastBytes) * 1000 / elapsed);
                m_progress(p);
            }
            lastBytes = now;
            lastTick  = tnow;
        }
        pool.addingDone.store(true);
        for (auto& t : workers) if (t.joinable()) t.join();

        std::vector<std::wstring> inputs;
        {
            std::lock_guard<std::mutex> lk(pool.mu);
            for (auto& j : pool.jobs) {
                if (!j.done) return j.error.empty() ? L"segment failure" : j.error;
                inputs.push_back(j.outPath);
            }
        }
        std::wstring concatErr;
        std::atomic<int64_t> copied{0};
        if (!concatFiles(inputs, outPath, &m_abort, copied, concatErr))
            return L"concat: " + concatErr;
        for (auto& p : inputs) DeleteFileW(p.c_str());
        return L""; // success
    };

    std::wstring vErr = downloadRep(*vid, videoPath, L"v");
    if (m_abort) {
        res.status = DownloadStatus::Aborted;
        res.message = L"aborted";
        return res;
    }
    if (!vErr.empty()) {
        res.status  = DownloadStatus::NetworkError;
        res.message = L"video: " + vErr;
        res.outputPath = videoPath;
        return res;
    }

    std::wstring finalPath = videoPath;

    // If we have audio reps AND ffmpeg, download the best audio rep and
    // mux it with the video.
    if (!manifest.audioReps.empty() && !m_opts.ffmpegPath.empty()) {
        const DashRepresentation* aud = pickDashVariant(manifest.audioReps, L"best");
        if (aud) {
            std::wstring aErr = downloadRep(*aud, audioPath, L"a");
            if (!aErr.empty()) {
                res.status  = DownloadStatus::NetworkError;
                res.message = L"audio: " + aErr;
                res.outputPath = videoPath;
                return res;
            }
            // ffmpeg mux: -i video -i audio -c copy output
            std::wstring cmd = L"\"" + m_opts.ffmpegPath + L"\""
                L" -y -loglevel error -i \"" + videoPath + L"\""
                L" -i \"" + audioPath + L"\" -c copy \"" + muxedPath + L"\"";
            STARTUPINFOW         si{}; si.cb = sizeof(si);
            PROCESS_INFORMATION  pi{};
            std::vector<wchar_t> cmdBuf(cmd.begin(), cmd.end());
            cmdBuf.push_back(L'\0');
            if (CreateProcessW(nullptr, cmdBuf.data(), nullptr, nullptr,
                               FALSE, CREATE_NO_WINDOW, nullptr, nullptr,
                               &si, &pi)) {
                WaitForSingleObject(pi.hProcess, INFINITE);
                DWORD rc = 1;
                GetExitCodeProcess(pi.hProcess, &rc);
                CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
                if (rc == 0) {
                    DeleteFileW(videoPath.c_str());
                    DeleteFileW(audioPath.c_str());
                    finalPath = muxedPath;
                } else {
                    fwprintf(stderr,
                        L"[matata] ffmpeg mux exit %lu; keeping split files\n", rc);
                    fflush(stderr);
                }
            }
        }
    } else if (!manifest.audioReps.empty() && m_opts.ffmpegPath.empty()) {
        // Rename video to .mp4 (it was already .video.mp4 — keep the explicit
        // naming so the user knows audio wasn't included).
    } else if (manifest.audioReps.empty() && !m_opts.ffmpegPath.empty()) {
        // Validate-remux single video (normalizes any fragment weirdness).
        if (runFfmpegRemux(m_opts.ffmpegPath, videoPath, muxedPath, err)) {
            DeleteFileW(videoPath.c_str());
            finalPath = muxedPath;
        }
    }

    RemoveDirectoryW(partDir.c_str());

    res.status     = DownloadStatus::Ok;
    res.outputPath = finalPath;
    return res;
}

}
