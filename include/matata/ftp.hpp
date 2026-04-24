#pragma once
#include <cstdint>
#include <functional>
#include <string>

namespace matata {

// A thin, synchronous FTP client over Winsock. Implements enough of
// RFC 959 to download a single file:
//   login (USER/PASS, anonymous if empty)
//   TYPE I  (binary)
//   SIZE    (optional total-size probe)
//   PASV    (passive-mode data channel)
//   REST    (resume offset)
//   RETR    (streamed retrieve)
//   QUIT
//
// TLS (explicit FTPS via AUTH TLS) is deferred to v0.7.1.
class FtpClient {
public:
    FtpClient();
    ~FtpClient();
    FtpClient(const FtpClient&)            = delete;
    FtpClient& operator=(const FtpClient&) = delete;

    // Resolve, connect, USER + PASS. Empty user => "anonymous".
    // When `useTls` is true the client issues AUTH TLS, upgrades the
    // control channel, then sends PBSZ 0 + PROT P so the data channel
    // is also TLS-encrypted (explicit FTPS, RFC 4217).
    bool login(const std::wstring& host, uint16_t port,
               const std::wstring& user, const std::wstring& pass,
               std::wstring& err, bool useTls = false);

    bool setTypeBinary(std::wstring& err);

    // Ask the server for a file's size via SIZE; -1 if server refuses.
    int64_t size(const std::wstring& path, std::wstring& err);

    // Stream bytes. Calls `sink(data, len)` per chunk; returning false
    // aborts the transfer. `offset > 0` issues REST first for resume.
    using ChunkSink = std::function<bool(const uint8_t*, size_t)>;
    bool retrieve(const std::wstring& path, int64_t offset,
                  ChunkSink sink, std::wstring& err);

    // Cancel any in-flight retrieve. Safe to call from another thread;
    // the retrieve() call returns with `err` = "aborted".
    void abort();

    void close();

public:
    struct Impl;
private:
    Impl* m_p;
};

}
