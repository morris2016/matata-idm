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
};

}
