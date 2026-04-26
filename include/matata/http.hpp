#pragma once
#include "matata/digest.hpp"
#include "matata/url.hpp"

#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace matata {

class RateLimiter;

struct ResourceInfo {
    int64_t           totalSize     = -1;   // -1 = unknown (no Content-Length)
    bool              acceptsRanges = false;
    std::wstring      etag;
    std::wstring      lastModified;
    std::wstring      contentType;
    std::wstring      filename;          // from Content-Disposition, if present
    Url               finalUrl;          // after redirects
    int               statusCode    = 0;
    DigestExpectation digest;            // from Digest / Content-MD5 headers
};

// Name/value pairs appended to every request. Content-Length and Range are
// managed internally — do not add them here.
using ExtraHeaders = std::vector<std::pair<std::wstring, std::wstring>>;

// Probe a URL with a ranged GET (bytes=0-0): reliably detects range support,
// reads only headers, then closes the request.
bool probeResource(const Url& url, const ExtraHeaders& headers,
                   ResourceInfo& info, std::wstring& err);

using ChunkSink = std::function<bool(const uint8_t* data, size_t len)>;

struct RangeRequest {
    int64_t start = 0;     // inclusive
    int64_t end   = -1;    // inclusive; -1 = to end
};

// GET url with Range header; invokes sink with body chunks.
// If `limiter` is non-null, blocks before each write until tokens are available.
int httpGetRange(const Url& url, const RangeRequest& range,
                 const ExtraHeaders& headers, RateLimiter* limiter,
                 ChunkSink sink, std::wstring& err);

// Simple plain GET that accumulates the whole body into `body`.
// Returns HTTP status code, or 0 on network error (err populated).
int httpFetchBody(const Url& url, const ExtraHeaders& headers,
                  std::string& body, std::wstring& err,
                  size_t maxBytes = 8 * 1024 * 1024);

class HttpSession {
public:
    static void*  acquire();
    static void   release();
    // Drop the cached session so the next acquire() picks up changed
    // ProxyConfig. Safe to call from any thread; the session is only
    // closed when no requests are in flight (refcount == 0).
    static void   resetForReconfig();
};

// Global proxy configuration. Read on every session-open and applied per
// request. Mode 0 = system (WinHTTP automatic), 1 = none, 2 = manual.
struct ProxyConfig {
    int          mode = 0;
    std::wstring server;     // "host:port"  (no scheme)  for HTTP proxy,
                             // "socks=host:port"          for SOCKS5
    std::wstring bypass;     // "*.local;<local>;..."
    std::wstring user;
    std::wstring pass;
};

void setProxyConfig(const ProxyConfig& cfg);
ProxyConfig currentProxyConfig();

// Sites Logins. The HTTP layer keeps the list in memory and matches by
// case-insensitive host on every outbound request, prepending an
// Authorization: Basic <base64(user:pass)> header if a match exists.
// Persistence (HKCU + DPAPI-encrypted password) is the caller's job.
struct SiteLogin {
    std::wstring host;       // case-insensitive match against Url.host
    std::wstring user;
    std::wstring pass;       // plaintext in process memory; encrypted at rest
};

void setSiteLogins(std::vector<SiteLogin> logins);
std::vector<SiteLogin> currentSiteLogins();

// Returns "Authorization: Basic <base64>" (no trailing CRLF) for the given
// host if a matching login is registered, else empty. Exposed so the
// downloader can inject it for paths that bypass the central httpFetchBody
// helper (e.g. segmented range workers).
std::wstring siteAuthHeaderFor(const std::wstring& host);

}
