#include "matata/ftp.hpp"
#include "matata/tls.hpp"

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <memory>
#include <sstream>
#include <string>

namespace matata {

namespace {

std::string wideToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(),
                                nullptr, 0, nullptr, nullptr);
    std::string out(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(),
                        out.data(), n, nullptr, nullptr);
    return out;
}

std::wstring utf8ToWide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}

std::wstring lastSockErr(const wchar_t* op) {
    int e = WSAGetLastError();
    std::wostringstream ss;
    ss << op << L" failed (WSA=" << e << L")";
    return ss.str();
}

// One-time Winsock startup guarded by a ref counter so the module plays
// nice with the rest of the app (which also uses Winsock indirectly).
std::atomic<int> g_wsRefs{0};
void wsAcquire() {
    if (g_wsRefs.fetch_add(1) == 0) {
        WSADATA d{};
        WSAStartup(MAKEWORD(2, 2), &d);
    }
}
void wsRelease() {
    if (g_wsRefs.fetch_sub(1) == 1) WSACleanup();
}

// Open a blocking TCP connection to host:port. Returns INVALID_SOCKET on
// failure with err populated.
SOCKET tcpConnect(const std::string& host, uint16_t port, std::wstring& err) {
    addrinfo hints{}, *res = nullptr;
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    char portStr[8];
    _snprintf_s(portStr, _TRUNCATE, "%u", (unsigned)port);
    int r = getaddrinfo(host.c_str(), portStr, &hints, &res);
    if (r != 0 || !res) {
        std::wostringstream s; s << L"getaddrinfo failed (" << r << L")";
        err = s.str();
        return INVALID_SOCKET;
    }
    SOCKET s = INVALID_SOCKET;
    for (auto* p = res; p; p = p->ai_next) {
        s = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (s == INVALID_SOCKET) continue;
        if (connect(s, p->ai_addr, (int)p->ai_addrlen) == 0) break;
        closesocket(s);
        s = INVALID_SOCKET;
    }
    freeaddrinfo(res);
    if (s == INVALID_SOCKET) err = lastSockErr(L"connect");
    return s;
}

// TLS-aware recv: pulls ciphertext if a TlsStream is set, else raw socket.
int ioRecv(SOCKET s, TlsStream* tls, char* buf, int len, std::wstring& err) {
    if (tls) return tls->recvSome(buf, len, err);
    int got = recv(s, buf, len, 0);
    if (got == SOCKET_ERROR) { err = lastSockErr(L"recv"); return -1; }
    return got;
}

// TLS-aware send (blocks until all bytes sent or error).
bool ioSendAll(SOCKET s, TlsStream* tls, const char* buf, int len,
               std::wstring& err) {
    if (tls) return tls->sendAll(buf, len, err) == len;
    int left = len;
    const char* p = buf;
    while (left > 0) {
        int sent = send(s, p, left, 0);
        if (sent == SOCKET_ERROR) { err = lastSockErr(L"send"); return false; }
        if (sent == 0) { err = L"send returned 0"; return false; }
        p += sent; left -= sent;
    }
    return true;
}

bool readLine(SOCKET s, TlsStream* tls, std::string& line,
              std::string& spill,
              std::atomic<bool>* abortFlag, std::wstring& err) {
    line.clear();
    for (;;) {
        auto nl = spill.find('\n');
        if (nl != std::string::npos) {
            line = spill.substr(0, nl);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            spill.erase(0, nl + 1);
            return true;
        }
        if (abortFlag && abortFlag->load()) { err = L"aborted"; return false; }
        char buf[4096];
        int got = ioRecv(s, tls, buf, sizeof(buf), err);
        if (got == 0) { err = L"server closed control connection"; return false; }
        if (got < 0)  return false;
        spill.append(buf, got);
    }
}

bool sendAll(SOCKET s, const std::string& data, std::wstring& err) {
    return ioSendAll(s, nullptr, data.data(), (int)data.size(), err);
}

// Read an FTP response: one or more CRLF-terminated lines. Multi-line
// responses start with "nnn-" and end with a line starting with "nnn ".
// Returns the numeric code and the joined text.
bool readResponse(SOCKET s, TlsStream* tls, std::string& spill,
                  std::atomic<bool>* abortFlag,
                  int& code, std::string& text, std::wstring& err) {
    text.clear();
    std::string line;
    if (!readLine(s, tls, line, spill, abortFlag, err)) return false;
    if (line.size() < 3) { err = L"short FTP response"; return false; }

    code = std::atoi(line.substr(0, 3).c_str());
    text = line;

    if (line.size() >= 4 && line[3] == '-') {
        std::string closer = line.substr(0, 3) + " ";
        for (;;) {
            if (!readLine(s, tls, line, spill, abortFlag, err)) return false;
            text += "\n"; text += line;
            if (line.size() >= 4 && line.compare(0, 4, closer) == 0) break;
        }
    }
    return true;
}

} // anon

struct FtpClient::Impl {
    SOCKET                        ctrl = INVALID_SOCKET;
    std::string                   spill;          // leftover bytes from recv
    std::atomic<bool>             abortFlag{false};
    std::string                   host;           // used for PASV fallback
    std::wstring                  hostW;          // same, for TLS SNI
    bool                          useTls = false;
    std::unique_ptr<TlsStream>    ctrlTls;        // populated when useTls
};

FtpClient::FtpClient() : m_p(new Impl()) { wsAcquire(); }

FtpClient::~FtpClient() {
    close();
    delete m_p;
    wsRelease();
}

void FtpClient::abort() { m_p->abortFlag = true; }

void FtpClient::close() {
    if (m_p->ctrl != INVALID_SOCKET) {
        std::string spill;
        std::wstring err;
        std::string ignore;
        int code = 0;
        std::string command = "QUIT\r\n";
        ioSendAll(m_p->ctrl, m_p->ctrlTls.get(),
                  command.data(), (int)command.size(), err);
        readResponse(m_p->ctrl, m_p->ctrlTls.get(), spill, nullptr,
                     code, ignore, err);
        if (m_p->ctrlTls) m_p->ctrlTls->shutdown();
        m_p->ctrlTls.reset();
        closesocket(m_p->ctrl);
        m_p->ctrl = INVALID_SOCKET;
    }
}

// Send one command and read the response. `command` is appended with CRLF.
// Returns false on socket error; otherwise `code` + `text` are populated
// regardless of whether the code is success.
static bool cmd(FtpClient::Impl& p, const std::string& command,
                int& code, std::string& text, std::wstring& err) {
    std::string wire = command + "\r\n";
    if (!ioSendAll(p.ctrl, p.ctrlTls.get(),
                   wire.data(), (int)wire.size(), err)) return false;
    return readResponse(p.ctrl, p.ctrlTls.get(), p.spill, &p.abortFlag,
                        code, text, err);
}

bool FtpClient::login(const std::wstring& host, uint16_t port,
                      const std::wstring& user, const std::wstring& pass,
                      std::wstring& err, bool useTls) {
    std::string h = wideToUtf8(host);
    m_p->host  = h;
    m_p->hostW = host;
    m_p->useTls = useTls;
    m_p->ctrl = tcpConnect(h, port ? port : 21, err);
    if (m_p->ctrl == INVALID_SOCKET) return false;

    int  code = 0;
    std::string text;
    // Server greeting (plaintext, before TLS).
    if (!readResponse(m_p->ctrl, nullptr, m_p->spill, &m_p->abortFlag,
                      code, text, err)) return false;
    if (code < 200 || code >= 300) {
        err = L"FTP greeting refused: " + utf8ToWide(text); return false;
    }

    if (useTls) {
        // AUTH TLS -> 234 Ready to negotiate.
        std::string wire = "AUTH TLS\r\n";
        if (!ioSendAll(m_p->ctrl, nullptr, wire.data(), (int)wire.size(), err))
            return false;
        if (!readResponse(m_p->ctrl, nullptr, m_p->spill, &m_p->abortFlag,
                          code, text, err)) return false;
        if (code != 234 && code != 334) {
            err = L"AUTH TLS refused: " + utf8ToWide(text);
            return false;
        }
        m_p->ctrlTls.reset(new TlsStream());
        if (!m_p->ctrlTls->handshakeClient(
                (MATATA_SOCKET)m_p->ctrl, host, err)) {
            m_p->ctrlTls.reset();
            return false;
        }
        m_p->spill.clear();  // anything before TLS is now moot
    }

    std::string u = user.empty() ? std::string("anonymous") : wideToUtf8(user);
    std::string p = pass.empty() ? std::string("guest@") : wideToUtf8(pass);

    if (!cmd(*m_p, "USER " + u, code, text, err)) return false;
    if (code != 230 && code != 331) {
        err = L"USER refused: " + utf8ToWide(text); return false;
    }
    if (code == 331) {
        if (!cmd(*m_p, "PASS " + p, code, text, err)) return false;
        if (code != 230 && code != 202) {
            err = L"PASS refused: " + utf8ToWide(text); return false;
        }
    }

    if (useTls) {
        if (!cmd(*m_p, "PBSZ 0", code, text, err)) return false;
        if (code != 200) { err = L"PBSZ refused: " + utf8ToWide(text); return false; }
        if (!cmd(*m_p, "PROT P", code, text, err)) return false;
        if (code != 200) { err = L"PROT P refused: " + utf8ToWide(text); return false; }
    }
    return true;
}

bool FtpClient::setTypeBinary(std::wstring& err) {
    int code = 0; std::string text;
    if (!cmd(*m_p, "TYPE I", code, text, err)) return false;
    if (code != 200) { err = L"TYPE I refused: " + utf8ToWide(text); return false; }
    return true;
}

int64_t FtpClient::size(const std::wstring& path, std::wstring& err) {
    int code = 0; std::string text;
    if (!cmd(*m_p, "SIZE " + wideToUtf8(path), code, text, err)) return -1;
    if (code != 213) return -1;
    // "213 12345"
    size_t sp = text.find(' ');
    if (sp == std::string::npos) return -1;
    try { return std::stoll(text.substr(sp + 1)); } catch (...) { return -1; }
}

// Parse PASV response "227 Entering Passive Mode (h1,h2,h3,h4,p1,p2)".
static bool parsePasv(const std::string& text,
                      std::string& host, uint16_t& port) {
    auto lp = text.find('(');
    auto rp = text.find(')', lp == std::string::npos ? 0 : lp);
    if (lp == std::string::npos || rp == std::string::npos) return false;
    std::string inside = text.substr(lp + 1, rp - lp - 1);
    int n[6]{};
    int parsed = sscanf_s(inside.c_str(), "%d,%d,%d,%d,%d,%d",
                          &n[0], &n[1], &n[2], &n[3], &n[4], &n[5]);
    if (parsed != 6) return false;
    for (int x : n) if (x < 0 || x > 255) return false;
    char ip[32];
    _snprintf_s(ip, _TRUNCATE, "%d.%d.%d.%d", n[0], n[1], n[2], n[3]);
    host = ip;
    port = (uint16_t)((n[4] << 8) | n[5]);
    return true;
}

// Parse EPSV "229 Extended Passive Mode (|||port|)".
static bool parseEpsv(const std::string& text, uint16_t& port) {
    auto lp = text.find('(');
    auto rp = text.find(')', lp == std::string::npos ? 0 : lp);
    if (lp == std::string::npos || rp == std::string::npos) return false;
    std::string inside = text.substr(lp + 1, rp - lp - 1);
    // The delimiter character is repeated three times before the port.
    if (inside.size() < 5) return false;
    char d = inside[0];
    if (inside[1] != d || inside[2] != d) return false;
    std::string portStr;
    for (size_t i = 3; i < inside.size() && inside[i] != d; ++i)
        portStr.push_back(inside[i]);
    try { port = (uint16_t)std::stoi(portStr); } catch (...) { return false; }
    return true;
}

bool FtpClient::retrieve(const std::wstring& path, int64_t offset,
                         ChunkSink sink, std::wstring& err) {
    int  code = 0;
    std::string text;

    // Prefer EPSV (works over IPv6 and many NATed servers). Fall back to PASV.
    std::string dataHost;
    uint16_t    dataPort = 0;

    if (cmd(*m_p, "EPSV", code, text, err) && code == 229
        && parseEpsv(text, dataPort)) {
        dataHost = m_p->host;  // EPSV re-uses the control host
    } else {
        if (!cmd(*m_p, "PASV", code, text, err)) return false;
        if (code != 227) { err = L"PASV refused: " + utf8ToWide(text); return false; }
        if (!parsePasv(text, dataHost, dataPort)) {
            err = L"PASV response not parseable: " + utf8ToWide(text);
            return false;
        }
    }

    if (offset > 0) {
        char restBuf[32];
        _snprintf_s(restBuf, _TRUNCATE, "REST %lld", (long long)offset);
        if (!cmd(*m_p, restBuf, code, text, err)) return false;
        if (code != 350 && code != 200) {
            err = L"REST refused: " + utf8ToWide(text);
            return false;
        }
    }

    SOCKET data = tcpConnect(dataHost, dataPort, err);
    if (data == INVALID_SOCKET) return false;

    // With FTPS (PROT P) the data channel is also TLS-wrapped. The
    // handshake must complete BEFORE we issue RETR because some servers
    // start pushing data as soon as the data socket connects.
    std::unique_ptr<TlsStream> dataTls;
    if (m_p->useTls) {
        dataTls.reset(new TlsStream());
        if (!dataTls->handshakeDataClient((MATATA_SOCKET)data,
                                          m_p->hostW,
                                          *m_p->ctrlTls, err)) {
            closesocket(data);
            return false;
        }
    }

    // RETR
    {
        std::string wire = "RETR " + wideToUtf8(path) + "\r\n";
        if (!ioSendAll(m_p->ctrl, m_p->ctrlTls.get(),
                       wire.data(), (int)wire.size(), err)) {
            closesocket(data); return false;
        }
    }

    // Preliminary reply (150 or 125). If the server rejects, bail.
    if (!readResponse(m_p->ctrl, m_p->ctrlTls.get(), m_p->spill,
                      &m_p->abortFlag, code, text, err)) {
        closesocket(data); return false;
    }
    if (code != 150 && code != 125) {
        closesocket(data);
        err = L"RETR refused: " + utf8ToWide(text);
        return false;
    }

    // Stream the data channel until EOF.
    bool sinkOk = true;
    for (;;) {
        if (m_p->abortFlag.load()) {
            closesocket(data);
            err = L"aborted";
            return false;
        }
        char buf[64 * 1024];
        int got = ioRecv(data, dataTls.get(), buf, (int)sizeof(buf), err);
        if (got == 0) break;          // EOF
        if (got < 0)  { closesocket(data); return false; }
        if (!sink((const uint8_t*)buf, (size_t)got)) { sinkOk = false; break; }
    }
    if (dataTls) dataTls->shutdown();
    closesocket(data);

    // Transfer-complete response.
    if (!readResponse(m_p->ctrl, m_p->ctrlTls.get(), m_p->spill,
                      &m_p->abortFlag, code, text, err)) return false;
    if (!sinkOk) { err = L"aborted by sink"; return false; }
    if (code != 226 && code != 250) {
        err = L"RETR did not complete: " + utf8ToWide(text);
        return false;
    }
    return true;
}

}
