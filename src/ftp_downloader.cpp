#include "matata/ftp_downloader.hpp"
#include "matata/ftp.hpp"
#include "matata/url.hpp"

#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <sstream>
#include <thread>

namespace fs = std::filesystem;

namespace matata {

namespace {

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

std::wstring uniquePath(const std::wstring& dir, const std::wstring& name) {
    std::wstring candidate = joinPath(dir, name);
    if (!fs::exists(candidate)) return candidate;
    std::wstring stem = name, ext;
    auto dot = name.find_last_of(L'.');
    if (dot != std::wstring::npos && dot != 0) {
        stem = name.substr(0, dot); ext = name.substr(dot);
    }
    for (int i = 1; i < 10000; ++i) {
        std::wostringstream ss; ss << stem << L" (" << i << L")" << ext;
        std::wstring try_ = joinPath(dir, ss.str());
        if (!fs::exists(try_)) return try_;
    }
    return candidate;
}

int64_t fileSizeW(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA fad{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad))
        return 0;
    return ((int64_t)fad.nFileSizeHigh << 32) | (int64_t)fad.nFileSizeLow;
}

}

FtpDownloader::FtpDownloader(std::wstring url, DownloadOptions opts)
    : m_rawUrl(std::move(url)), m_opts(std::move(opts)) {}

FtpDownloader::~FtpDownloader() = default;

void FtpDownloader::setProgressCallback(ProgressCallback cb) {
    m_progress = std::move(cb);
}

void FtpDownloader::abort() {
    m_abort = true;
    if (m_client) m_client->abort();
}

DownloadResult FtpDownloader::run() {
    DownloadResult res;

    Url url;
    std::wstring err;
    if (!parseUrl(m_rawUrl, url, err)) {
        res.status = DownloadStatus::NetworkError;
        res.message = L"URL parse: " + err;
        return res;
    }
    bool useTls = (url.scheme == L"ftps");
    if (url.scheme != L"ftp" && !useTls) {
        res.status = DownloadStatus::NetworkError;
        res.message = L"FtpDownloader only handles ftp:// / ftps://";
        return res;
    }

    uint16_t port = url.port ? url.port : (uint16_t)21;

    // Output paths.
    std::wstring name = m_opts.outputName;
    if (name.empty()) name = url.inferredFilename();
    std::wstring outDir = m_opts.outputDir;
    if (outDir.empty()) {
        wchar_t cwd[MAX_PATH];
        GetCurrentDirectoryW(MAX_PATH, cwd);
        outDir = cwd;
    }
    std::error_code ec;
    fs::create_directories(outDir, ec);

    std::wstring finalPath = uniquePath(outDir, name);
    std::wstring partPath  = finalPath + L".mtpart";

    // Login.
    m_client.reset(new FtpClient());
    if (!m_client->login(url.host, port, url.username, url.password,
                         err, useTls)) {
        res.status = DownloadStatus::NetworkError;
        res.message = (useTls ? L"FTPS login: " : L"FTP login: ") + err;
        return res;
    }
    if (!m_client->setTypeBinary(err)) {
        res.status = DownloadStatus::NetworkError;
        res.message = L"FTP TYPE I: " + err;
        return res;
    }

    // Probe size (optional).
    int64_t total = m_client->size(url.path, err);
    err.clear();

    // Resume from an existing .mtpart if present and sanity-checks pass.
    int64_t offset = 0;
    if (fs::exists(partPath)) {
        int64_t existing = fileSizeW(partPath);
        if (existing > 0 && (total <= 0 || existing < total)) {
            offset = existing;
        } else {
            DeleteFileW(partPath.c_str());
        }
    }

    HANDLE fh = CreateFileW(partPath.c_str(), GENERIC_WRITE,
                            FILE_SHARE_READ, nullptr,
                            OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (fh == INVALID_HANDLE_VALUE) {
        res.status  = DownloadStatus::FileError;
        res.message = L"create part: " + winErr(GetLastError());
        return res;
    }
    // Seek to end (we either resume there or it's empty).
    LARGE_INTEGER li{}; li.QuadPart = offset;
    SetFilePointerEx(fh, li, nullptr, FILE_BEGIN);

    auto   t0      = std::chrono::steady_clock::now();
    int64_t lastBytes = offset;
    auto   lastTick = t0;
    int64_t downloaded = offset;

    auto sink = [&](const uint8_t* data, size_t len) -> bool {
        if (m_abort.load()) return false;
        const uint8_t* p = data;
        size_t left = len;
        while (left > 0) {
            DWORD wrote = 0;
            DWORD chunk = (left > (1u << 30)) ? (1u << 30) : (DWORD)left;
            if (!WriteFile(fh, p, chunk, &wrote, nullptr) || wrote == 0)
                return false;
            p += wrote; left -= wrote;
        }
        downloaded += (int64_t)len;

        auto tnow = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(tnow - lastTick).count();
        if (elapsed >= 200 && m_progress) {
            DownloadProgress prog;
            prog.total       = total;
            prog.downloaded  = downloaded;
            prog.bytesPerSec = elapsed > 0
                ? (int64_t)((downloaded - lastBytes) * 1000 / elapsed) : 0;
            prog.activeConns = 1;
            prog.segments    = 1;
            m_progress(prog);
            lastBytes = downloaded;
            lastTick  = tnow;
        }
        return true;
    };

    bool ok = m_client->retrieve(url.path, offset, sink, err);
    CloseHandle(fh);

    // Final progress tick.
    if (m_progress) {
        DownloadProgress p;
        p.total       = total;
        p.downloaded  = downloaded;
        p.bytesPerSec = 0;
        p.activeConns = 0;
        p.segments    = 1;
        m_progress(p);
    }

    if (m_abort.load()) {
        res.status = DownloadStatus::Aborted;
        res.message = L"aborted by caller";
        res.outputPath = partPath;
        return res;
    }
    if (!ok) {
        res.status  = DownloadStatus::NetworkError;
        res.message = L"FTP retrieve: " + err;
        res.outputPath = partPath;
        return res;
    }

    // If we know the total, verify we got everything.
    if (total > 0 && fileSizeW(partPath) < total) {
        res.status  = DownloadStatus::NetworkError;
        res.message = L"FTP transfer incomplete";
        res.outputPath = partPath;
        return res;
    }

    DeleteFileW(finalPath.c_str());
    if (!MoveFileW(partPath.c_str(), finalPath.c_str())) {
        res.status  = DownloadStatus::FileError;
        res.message = L"rename failed: " + winErr(GetLastError());
        res.outputPath = partPath;
        return res;
    }

    res.status     = DownloadStatus::Ok;
    res.outputPath = finalPath;
    return res;
}

}
