#pragma once
#include "matata/http.hpp"
#include "matata/url.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace matata {

// A fully-resolved DASH media stream: init segment + media segment URLs.
struct DashRepresentation {
    std::wstring id;
    int64_t      bandwidth = 0;
    int          width     = 0;
    int          height    = 0;
    std::wstring mimeType;        // e.g. "video/mp4", "audio/mp4"
    std::wstring codecs;
    std::wstring initUrl;         // absolute URL to the init segment (fMP4)
    std::vector<std::wstring> segmentUrls;  // absolute URLs, in order
};

struct DashManifest {
    Url                             manifestUrl;
    std::vector<DashRepresentation> videoReps;
    std::vector<DashRepresentation> audioReps;
    // If true, the manifest was parsed but contained a feature matata
    // can't resolve yet (e.g. live dynamic MPD). Message is in `warning`.
    bool         unsupported = false;
    std::wstring warning;
};

bool fetchAndParseDash(const Url& url, const ExtraHeaders& headers,
                       DashManifest& out, std::wstring& err);

bool parseDash(const std::wstring& xmlBody, const Url& baseUrl,
               DashManifest& out, std::wstring& err);

// Pick the highest-bandwidth video representation matching the quality
// string (same rules as HLS: "best"/"worst"/"<height>[p]"). Returns nullptr
// if there are no video representations.
const DashRepresentation* pickDashVariant(const std::vector<DashRepresentation>& reps,
                                          const std::wstring& quality);

bool looksLikeDashUrl(const std::wstring& url);

}
