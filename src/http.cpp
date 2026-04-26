#include "matata/http.hpp"
#include "matata/bandwidth.hpp"

#include <windows.h>
#include <winhttp.h>

#include <atomic>
#include <cctype>
#include <mutex>
#include <sstream>
#include <string>

namespace matata {

namespace {

std::mutex          g_sessionMu;
HINTERNET           g_session = nullptr;
std::atomic<int>    g_sessionRefs{0};

// Proxy state. Guarded by g_proxyMu; copied into the session at open
// and applied per-request below.
std::mutex          g_proxyMu;
ProxyConfig         g_proxyCfg;
bool                g_sessionStale = false;  // proxy changed -> rebuild

ProxyConfig snapshotProxy() {
    std::lock_guard<std::mutex> lk(g_proxyMu);
    return g_proxyCfg;
}

// Sites Logins. Plaintext list held in memory; the GUI layer is
// responsible for DPAPI-encrypting before persisting.
std::mutex                  g_siteLoginsMu;
std::vector<SiteLogin>      g_siteLogins;

std::wstring lowerWide(std::wstring s) {
    for (auto& c : s) if (c < 128) c = (wchar_t)std::tolower((int)c);
    return s;
}

// Tiny standards-conformant base64 encoder. Used only to encode the
// Authorization: Basic <base64(user:pass)> credentials. Input is the
// raw byte stream; output is ASCII.
std::wstring base64Encode(const std::string& in) {
    static const char* kAlphabet =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::wstring out;
    out.reserve(((in.size() + 2) / 3) * 4);
    size_t i = 0;
    while (i + 3 <= in.size()) {
        unsigned a = (unsigned char)in[i+0];
        unsigned b = (unsigned char)in[i+1];
        unsigned c = (unsigned char)in[i+2];
        out.push_back(kAlphabet[(a >> 2) & 0x3F]);
        out.push_back(kAlphabet[((a << 4) | (b >> 4)) & 0x3F]);
        out.push_back(kAlphabet[((b << 2) | (c >> 6)) & 0x3F]);
        out.push_back(kAlphabet[c & 0x3F]);
        i += 3;
    }
    size_t rem = in.size() - i;
    if (rem == 1) {
        unsigned a = (unsigned char)in[i+0];
        out.push_back(kAlphabet[(a >> 2) & 0x3F]);
        out.push_back(kAlphabet[(a << 4) & 0x3F]);
        out.push_back(L'=');
        out.push_back(L'=');
    } else if (rem == 2) {
        unsigned a = (unsigned char)in[i+0];
        unsigned b = (unsigned char)in[i+1];
        out.push_back(kAlphabet[(a >> 2) & 0x3F]);
        out.push_back(kAlphabet[((a << 4) | (b >> 4)) & 0x3F]);
        out.push_back(kAlphabet[(b << 2) & 0x3F]);
        out.push_back(L'=');
    }
    return out;
}

// UTF-16 -> UTF-8 (used for the user:pass payload before base64).
std::string wideToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(),
                                nullptr, 0, nullptr, nullptr);
    std::string out(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(),
                        out.data(), n, nullptr, nullptr);
    return out;
}

std::wstring lastErrWinHttp(const wchar_t* op) {
    DWORD e = GetLastError();
    std::wostringstream ss;
    ss << op << L" failed (WinHTTP err=" << e << L")";
    return ss.str();
}

std::wstring lowerAscii(std::wstring s) {
    for (auto& c : s) if (c < 128) c = (wchar_t)std::tolower((int)c);
    return s;
}

std::wstring trim(std::wstring s) {
    size_t a = 0, b = s.size();
    while (a < b && iswspace(s[a])) ++a;
    while (b > a && iswspace(s[b-1])) --b;
    return s.substr(a, b - a);
}

bool queryAllHeaders(HINTERNET hReq, std::wstring& out) {
    DWORD size = 0;
    WinHttpQueryHeaders(hReq, WINHTTP_QUERY_RAW_HEADERS_CRLF,
                        WINHTTP_HEADER_NAME_BY_INDEX, nullptr, &size,
                        WINHTTP_NO_HEADER_INDEX);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER) return false;
    std::wstring buf(size / sizeof(wchar_t), L'\0');
    if (!WinHttpQueryHeaders(hReq, WINHTTP_QUERY_RAW_HEADERS_CRLF,
                             WINHTTP_HEADER_NAME_BY_INDEX, buf.data(), &size,
                             WINHTTP_NO_HEADER_INDEX)) return false;
    buf.resize(wcslen(buf.c_str()));
    out = std::move(buf);
    return true;
}

std::wstring getHeader(const std::wstring& blob, const std::wstring& name) {
    std::wstring wantLc = lowerAscii(name);
    size_t pos = 0;
    while (pos < blob.size()) {
        size_t eol = blob.find(L"\r\n", pos);
        if (eol == std::wstring::npos) eol = blob.size();
        std::wstring line = blob.substr(pos, eol - pos);
        pos = (eol == blob.size()) ? eol : eol + 2;
        auto colon = line.find(L':');
        if (colon == std::wstring::npos) continue;
        std::wstring key = lowerAscii(trim(line.substr(0, colon)));
        if (key == wantLc) return trim(line.substr(colon + 1));
    }
    return L"";
}

std::wstring parseContentDispositionName(const std::wstring& hdr) {
    if (hdr.empty()) return L"";
    std::wstring lc = lowerAscii(hdr);
    auto keyPos = lc.find(L"filename");
    if (keyPos == std::wstring::npos) return L"";
    auto eq = lc.find(L'=', keyPos);
    if (eq == std::wstring::npos) return L"";
    size_t i = eq + 1;
    while (i < hdr.size() && iswspace(hdr[i])) ++i;
    if (i >= hdr.size()) return L"";
    std::wstring val;
    if (hdr[i] == L'"') {
        ++i;
        while (i < hdr.size() && hdr[i] != L'"') { val += hdr[i++]; }
    } else {
        while (i < hdr.size() && hdr[i] != L';') { val += hdr[i++]; }
    }
    return trim(val);
}

HINTERNET openConnection(HINTERNET session, const Url& url) {
    return WinHttpConnect(session, url.host.c_str(), url.port, 0);
}

HINTERNET openRequest(HINTERNET conn, const Url& url, const wchar_t* verb) {
    DWORD flags = url.secure ? WINHTTP_FLAG_SECURE : 0;
    return WinHttpOpenRequest(conn, verb, url.pathAndQuery().c_str(),
                              nullptr, WINHTTP_NO_REFERER,
                              WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
}

Url urlFromHandle(HINTERNET hReq, const Url& fallback) {
    DWORD size = 0;
    WinHttpQueryOption(hReq, WINHTTP_OPTION_URL, nullptr, &size);
    if (size == 0) return fallback;
    std::wstring buf(size / sizeof(wchar_t), L'\0');
    if (!WinHttpQueryOption(hReq, WINHTTP_OPTION_URL, buf.data(), &size)) return fallback;
    buf.resize(wcslen(buf.c_str()));
    Url u; std::wstring err;
    if (parseUrl(buf, u, err)) return u;
    return fallback;
}

// Flatten ExtraHeaders into "K: V\r\n..." form.
std::wstring flattenHeaders(const ExtraHeaders& hdrs) {
    std::wostringstream ss;
    for (auto& kv : hdrs) {
        if (kv.first.empty() || kv.second.empty()) continue;
        ss << kv.first << L": " << kv.second << L"\r\n";
    }
    return ss.str();
}

// Build final header block: site-login auth (Basic) + custom headers +
// optional Range. The auth header is suppressed if the caller already
// supplied an Authorization in extras (caller wins).
std::wstring buildHeaders(const ExtraHeaders& extras, const std::wstring& rangeHdr,
                          const std::wstring& host) {
    std::wstring out;
    bool callerHasAuth = false;
    for (auto& kv : extras) {
        if (lowerAscii(kv.first) == L"authorization" && !kv.second.empty()) {
            callerHasAuth = true; break;
        }
    }
    if (!callerHasAuth) {
        std::wstring h = siteAuthHeaderFor(host);
        if (!h.empty()) { out += h; out += L"\r\n"; }
    }
    out += flattenHeaders(extras);
    out += rangeHdr;
    return out;
}

// Best-effort: enable HTTP/2 on the session. Ignored on Windows < 10 1709.
void enableHttp2(HINTERNET session) {
    DWORD protoFlags = 0;
#ifdef WINHTTP_PROTOCOL_FLAG_HTTP2
    protoFlags = WINHTTP_PROTOCOL_FLAG_HTTP2;
#endif
    if (protoFlags) {
        WinHttpSetOption(session, 133 /* WINHTTP_OPTION_ENABLE_HTTP_PROTOCOL */,
                         &protoFlags, sizeof(protoFlags));
    }
}

} // anon

void* HttpSession::acquire() {
    std::lock_guard<std::mutex> lk(g_sessionMu);
    // If proxy settings changed and no requests are in flight, drop the
    // current session so we can rebuild it with the new proxy config.
    if (g_session && g_sessionStale && g_sessionRefs.load() == 0) {
        WinHttpCloseHandle(g_session);
        g_session = nullptr;
        g_sessionStale = false;
    }
    if (!g_session) {
        ProxyConfig cfg = snapshotProxy();
        DWORD accessType = WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY;
        const wchar_t* proxyName = WINHTTP_NO_PROXY_NAME;
        const wchar_t* proxyBypass = WINHTTP_NO_PROXY_BYPASS;
        if (cfg.mode == 1) {
            accessType = WINHTTP_ACCESS_TYPE_NO_PROXY;
        } else if (cfg.mode == 2 && !cfg.server.empty()) {
            accessType = WINHTTP_ACCESS_TYPE_NAMED_PROXY;
            proxyName = cfg.server.c_str();
            if (!cfg.bypass.empty()) proxyBypass = cfg.bypass.c_str();
        }
        g_session = WinHttpOpen(L"matata/0.2",
                                accessType, proxyName, proxyBypass, 0);
        if (g_session) enableHttp2(g_session);
    }
    if (g_session) g_sessionRefs++;
    return g_session;
}

void HttpSession::release() {
    std::lock_guard<std::mutex> lk(g_sessionMu);
    if (g_sessionRefs > 0) g_sessionRefs--;
    if (g_sessionRefs == 0 && g_session) {
        WinHttpCloseHandle(g_session);
        g_session = nullptr;
        g_sessionStale = false;
    }
}

void HttpSession::resetForReconfig() {
    std::lock_guard<std::mutex> lk(g_sessionMu);
    g_sessionStale = true;
    if (g_session && g_sessionRefs.load() == 0) {
        WinHttpCloseHandle(g_session);
        g_session = nullptr;
        g_sessionStale = false;
    }
}

void setProxyConfig(const ProxyConfig& cfg) {
    {
        std::lock_guard<std::mutex> lk(g_proxyMu);
        g_proxyCfg = cfg;
    }
    HttpSession::resetForReconfig();
}

ProxyConfig currentProxyConfig() { return snapshotProxy(); }

void setSiteLogins(std::vector<SiteLogin> logins) {
    for (auto& s : logins) s.host = lowerWide(s.host);
    std::lock_guard<std::mutex> lk(g_siteLoginsMu);
    g_siteLogins = std::move(logins);
}

std::vector<SiteLogin> currentSiteLogins() {
    std::lock_guard<std::mutex> lk(g_siteLoginsMu);
    return g_siteLogins;
}

std::wstring siteAuthHeaderFor(const std::wstring& host) {
    std::wstring lcHost = lowerWide(host);
    SiteLogin match;
    bool have = false;
    {
        std::lock_guard<std::mutex> lk(g_siteLoginsMu);
        for (auto& s : g_siteLogins) {
            if (s.host.empty()) continue;
            if (s.host == lcHost) { match = s; have = true; break; }
            // Suffix match for subdomains: ".example.com" matches a host
            // that ends in ".example.com" or equals "example.com".
            if (s.host.size() > 1 && s.host.front() == L'.') {
                std::wstring suffix = s.host.substr(1);
                if (lcHost == suffix ||
                    (lcHost.size() > suffix.size() &&
                     lcHost.compare(lcHost.size() - suffix.size() - 1,
                                    suffix.size() + 1,
                                    L"." + suffix) == 0)) {
                    match = s; have = true; break;
                }
            }
        }
    }
    if (!have) return L"";
    std::string raw = wideToUtf8(match.user) + ":" + wideToUtf8(match.pass);
    return L"Authorization: Basic " + base64Encode(raw);
}

namespace {

// Apply per-request proxy settings: when manual mode and creds are set,
// stamp them onto the request handle so WinHTTP sends them on the first
// proxy challenge. No-op for system/none modes.
void applyProxyAuth(HINTERNET hReq) {
    ProxyConfig cfg = snapshotProxy();
    if (cfg.mode != 2) return;
    if (cfg.user.empty() && cfg.pass.empty()) return;
    if (!cfg.user.empty()) {
        WinHttpSetOption(hReq, WINHTTP_OPTION_PROXY_USERNAME,
                         (LPVOID)cfg.user.c_str(),
                         (DWORD)(cfg.user.size() * sizeof(wchar_t)));
    }
    if (!cfg.pass.empty()) {
        WinHttpSetOption(hReq, WINHTTP_OPTION_PROXY_PASSWORD,
                         (LPVOID)cfg.pass.c_str(),
                         (DWORD)(cfg.pass.size() * sizeof(wchar_t)));
    }
}

} // anon

bool probeResource(const Url& url, const ExtraHeaders& headers,
                   ResourceInfo& info, std::wstring& err) {
    HINTERNET session = (HINTERNET)HttpSession::acquire();
    if (!session) { err = lastErrWinHttp(L"WinHttpOpen"); return false; }

    HINTERNET conn = openConnection(session, url);
    if (!conn) { err = lastErrWinHttp(L"WinHttpConnect"); HttpSession::release(); return false; }

    HINTERNET req = openRequest(conn, url, L"GET");
    if (!req) {
        err = lastErrWinHttp(L"WinHttpOpenRequest");
        WinHttpCloseHandle(conn); HttpSession::release(); return false;
    }

    std::wstring hdrBlock = buildHeaders(headers, L"Range: bytes=0-0\r\n", url.host);
    applyProxyAuth(req);

    BOOL ok = WinHttpSendRequest(req, hdrBlock.c_str(), (DWORD)-1L,
                                 WINHTTP_NO_REQUEST_DATA, 0, 0, 0);
    if (ok) ok = WinHttpReceiveResponse(req, nullptr);
    if (!ok) {
        err = lastErrWinHttp(L"WinHttpSendRequest/Receive");
        WinHttpCloseHandle(req); WinHttpCloseHandle(conn); HttpSession::release();
        return false;
    }

    DWORD status = 0, sz = sizeof(status);
    WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &status, &sz,
                        WINHTTP_NO_HEADER_INDEX);
    info.statusCode = (int)status;

    std::wstring respHdrs;
    if (!queryAllHeaders(req, respHdrs)) {
        err = L"could not read response headers";
        WinHttpCloseHandle(req); WinHttpCloseHandle(conn); HttpSession::release();
        return false;
    }

    info.finalUrl     = urlFromHandle(req, url);
    info.contentType  = getHeader(respHdrs, L"Content-Type");
    info.etag         = getHeader(respHdrs, L"ETag");
    info.lastModified = getHeader(respHdrs, L"Last-Modified");
    info.filename     = parseContentDispositionName(getHeader(respHdrs, L"Content-Disposition"));

    {
        std::wstring dg = getHeader(respHdrs, L"Digest");
        if (!dg.empty()) info.digest = parseDigestHeader(dg);
        if (info.digest.kind == DigestKind::None) {
            std::wstring md5 = getHeader(respHdrs, L"Content-MD5");
            if (!md5.empty()) info.digest = parseContentMd5Header(md5);
        }
    }

    if (status == 206) {
        info.acceptsRanges = true;
        std::wstring cr = getHeader(respHdrs, L"Content-Range");
        auto slash = cr.find(L'/');
        if (slash != std::wstring::npos) {
            std::wstring total = cr.substr(slash + 1);
            if (total != L"*") {
                try { info.totalSize = std::stoll(total); } catch (...) {}
            }
        }
    } else if (status >= 200 && status < 300) {
        info.acceptsRanges = false;
        std::wstring cl = getHeader(respHdrs, L"Content-Length");
        if (!cl.empty()) {
            try { info.totalSize = std::stoll(cl); } catch (...) {}
        }
    }

    WinHttpCloseHandle(req);
    WinHttpCloseHandle(conn);
    HttpSession::release();

    if (info.statusCode < 200 || info.statusCode >= 400) {
        if (err.empty()) {
            std::wostringstream s; s << L"HTTP " << info.statusCode;
            err = s.str();
        }
        return false;
    }
    err.clear();
    return true;
}

int httpFetchBody(const Url& url, const ExtraHeaders& headers,
                  std::string& body, std::wstring& err, size_t maxBytes) {
    body.clear();
    RangeRequest rr;  // start=0, end=-1 => no Range header, full body
    return httpGetRange(url, rr, headers, nullptr,
        [&](const uint8_t* data, size_t len) -> bool {
            if (body.size() + len > maxBytes) return false;
            body.append((const char*)data, len);
            return true;
        }, err);
}

int httpGetRange(const Url& url, const RangeRequest& range,
                 const ExtraHeaders& headers, RateLimiter* limiter,
                 ChunkSink sink, std::wstring& err) {
    HINTERNET session = (HINTERNET)HttpSession::acquire();
    if (!session) { err = lastErrWinHttp(L"WinHttpOpen"); return 0; }

    HINTERNET conn = openConnection(session, url);
    if (!conn) { err = lastErrWinHttp(L"WinHttpConnect"); HttpSession::release(); return 0; }

    HINTERNET req = openRequest(conn, url, L"GET");
    if (!req) {
        err = lastErrWinHttp(L"WinHttpOpenRequest");
        WinHttpCloseHandle(conn); HttpSession::release(); return 0;
    }

    std::wstring rangeHdr;
    if (range.start > 0 || range.end >= 0) {
        std::wostringstream ss;
        ss << L"Range: bytes=" << range.start << L"-";
        if (range.end >= 0) ss << range.end;
        ss << L"\r\n";
        rangeHdr = ss.str();
    }
    std::wstring hdrBlock = buildHeaders(headers, rangeHdr, url.host);
    applyProxyAuth(req);

    BOOL ok = WinHttpSendRequest(req,
                                 hdrBlock.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS
                                                  : hdrBlock.c_str(),
                                 hdrBlock.empty() ? 0 : (DWORD)-1L,
                                 WINHTTP_NO_REQUEST_DATA, 0, 0, 0);
    if (ok) ok = WinHttpReceiveResponse(req, nullptr);
    if (!ok) {
        err = lastErrWinHttp(L"WinHttpSendRequest/Receive");
        WinHttpCloseHandle(req); WinHttpCloseHandle(conn); HttpSession::release();
        return 0;
    }

    DWORD status = 0, sz = sizeof(status);
    WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &status, &sz,
                        WINHTTP_NO_HEADER_INDEX);

    if (status >= 200 && status < 300) {
        std::vector<uint8_t> buf(64 * 1024);
        for (;;) {
            DWORD avail = 0;
            if (!WinHttpQueryDataAvailable(req, &avail)) {
                err = lastErrWinHttp(L"QueryDataAvailable");
                status = 0;
                break;
            }
            if (avail == 0) break;
            while (avail > 0) {
                DWORD toRead = (avail > buf.size()) ? (DWORD)buf.size() : avail;
                DWORD got = 0;
                if (!WinHttpReadData(req, buf.data(), toRead, &got)) {
                    err = lastErrWinHttp(L"ReadData");
                    status = 0;
                    break;
                }
                if (got == 0) { avail = 0; break; }
                if (limiter) limiter->acquire((int64_t)got);
                if (!sink(buf.data(), got)) { status = 0; avail = 0; break; }
                avail = (avail > got) ? (avail - got) : 0;
            }
            if (status == 0) break;
        }
    } else if (err.empty()) {
        std::wostringstream s; s << L"HTTP " << status;
        err = s.str();
    }

    WinHttpCloseHandle(req);
    WinHttpCloseHandle(conn);
    HttpSession::release();
    return (int)status;
}

}
