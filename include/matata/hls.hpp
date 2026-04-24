#pragma once
#include "matata/http.hpp"
#include "matata/url.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace matata {

struct HlsVariant {
    std::wstring url;          // absolute URL to the media playlist
    int64_t      bandwidth = 0;
    int          width      = 0;
    int          height     = 0;
    std::wstring codecs;
    std::wstring resolution;   // "WxH" (cached string form)
};

enum class HlsCryptMethod {
    None,
    Aes128,       // AES-128-CBC with PKCS7 padding (supported)
    SampleAes,    // sample-AES (not supported)
    Unknown,
};

struct HlsKey {
    HlsCryptMethod method = HlsCryptMethod::None;
    std::wstring   keyUri;        // absolute URL to the 16-byte key blob
    bool           hasExplicitIv = false;
    uint8_t        iv[16]{};      // only valid when hasExplicitIv is true
};

struct HlsSegment {
    std::wstring url;
    double       duration = 0.0;
    // Snapshot of the key in effect when this segment was declared.
    HlsKey       key;
    // Sequence number of this segment (for IV derivation when no explicit IV).
    int64_t      sequence = 0;
};

struct HlsMedia {
    int                     targetDuration = 0;
    int                     mediaSequence  = 0;
    bool                    endList        = false;
    bool                    isFmp4         = false;   // #EXT-X-MAP present
    std::wstring            initUri;                  // resolved absolute URL
    std::vector<HlsSegment> segments;
    bool                    encrypted      = false;   // any key method != None
    bool                    unsupportedCrypto = false;  // SampleAES / Unknown
};

struct HlsManifest {
    bool                    isMaster   = false;
    std::vector<HlsVariant> variants;      // master only
    HlsMedia                media;         // media only
    Url                     manifestUrl;   // base URL for resolving segments
};

// Fetch manifest via HTTP + parse.
bool fetchAndParseHls(const Url& url, const ExtraHeaders& headers,
                      HlsManifest& out, std::wstring& err);

// Parse a manifest body you already have.
bool parseHls(const std::wstring& body, const Url& baseUrl,
              HlsManifest& out, std::wstring& err);

// Pick a variant by quality string ("best" | "worst" | "<height>p" | "<height>").
// Returns nullptr if variants is empty.
const HlsVariant* pickVariant(const std::vector<HlsVariant>& variants,
                              const std::wstring& quality);

// Does the URL's path look like an HLS manifest (ends in .m3u8)?
bool looksLikeHlsUrl(const std::wstring& url);

}
