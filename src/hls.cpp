#include "matata/hls.hpp"
#include "matata/aes.hpp"

#include <windows.h>

#include <algorithm>
#include <cwctype>
#include <map>
#include <sstream>

namespace matata {

namespace {

std::wstring utf8ToWide(const std::string& s) {
    if (s.empty()) return {};
    // Strip UTF-8 BOM if present.
    const char* data = s.data();
    size_t      len  = s.size();
    if (len >= 3 && (unsigned char)data[0] == 0xEF
                 && (unsigned char)data[1] == 0xBB
                 && (unsigned char)data[2] == 0xBF) {
        data += 3; len -= 3;
    }
    int n = MultiByteToWideChar(CP_UTF8, 0, data, (int)len, nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, data, (int)len, w.data(), n);
    return w;
}

std::wstring trim(std::wstring s) {
    size_t a = 0, b = s.size();
    while (a < b && iswspace(s[a])) ++a;
    while (b > a && iswspace(s[b-1])) --b;
    return s.substr(a, b - a);
}

bool startsWith(const std::wstring& s, const wchar_t* p) {
    size_t n = 0; while (p[n]) ++n;
    return s.size() >= n && s.compare(0, n, p) == 0;
}

std::wstring lowerAscii(std::wstring s) {
    for (auto& c : s) if (c < 128) c = (wchar_t)towlower(c);
    return s;
}

// Parse comma-separated KEY=VALUE pairs. VALUE may be "quoted" (can contain
// commas) or unquoted (read until next comma). HLS does not define escape
// sequences inside quoted strings.
std::map<std::wstring, std::wstring> parseAttrs(const std::wstring& s) {
    std::map<std::wstring, std::wstring> out;
    size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && (s[i] == L' ' || s[i] == L'\t')) ++i;
        size_t keyStart = i;
        while (i < s.size() && s[i] != L'=' && s[i] != L',') ++i;
        std::wstring key = trim(s.substr(keyStart, i - keyStart));
        std::wstring val;
        if (i < s.size() && s[i] == L'=') {
            ++i;
            if (i < s.size() && s[i] == L'"') {
                ++i;
                while (i < s.size() && s[i] != L'"') val.push_back(s[i++]);
                if (i < s.size()) ++i; // consume closing "
            } else {
                while (i < s.size() && s[i] != L',') val.push_back(s[i++]);
            }
        }
        if (!key.empty()) out[key] = val;
        if (i < s.size() && s[i] == L',') ++i;
    }
    return out;
}

void parseResolution(const std::wstring& res, int& w, int& h) {
    auto x = res.find(L'x');
    if (x == std::wstring::npos) return;
    try {
        w = std::stoi(res.substr(0, x));
        h = std::stoi(res.substr(x + 1));
    } catch (...) {}
}

}

bool looksLikeHlsUrl(const std::wstring& url) {
    // Match ".m3u8" anywhere before a '?' or end.
    std::wstring u = url;
    auto q = u.find(L'?');
    if (q != std::wstring::npos) u.resize(q);
    if (u.size() < 5) return false;
    std::wstring tail = lowerAscii(u.substr(u.size() - 5));
    return tail == L".m3u8";
}

bool parseHls(const std::wstring& body, const Url& baseUrl,
              HlsManifest& out, std::wstring& err) {
    out = HlsManifest{};
    out.manifestUrl = baseUrl;

    // Split into lines (handling CRLF and LF).
    std::vector<std::wstring> lines;
    lines.reserve(256);
    size_t i = 0;
    while (i < body.size()) {
        size_t eol = body.find(L'\n', i);
        std::wstring line = (eol == std::wstring::npos)
            ? body.substr(i)
            : body.substr(i, eol - i);
        if (!line.empty() && line.back() == L'\r') line.pop_back();
        lines.push_back(std::move(line));
        if (eol == std::wstring::npos) break;
        i = eol + 1;
    }

    if (lines.empty() || lines[0].find(L"#EXTM3U") != 0) {
        err = L"not an HLS manifest (missing #EXTM3U header)";
        return false;
    }

    HlsMedia            media;
    std::vector<HlsVariant> variants;
    bool                sawStreamInf = false;
    bool                sawExtInf    = false;
    HlsVariant          pendingVariant;
    double              pendingDuration = 0.0;

    // #EXT-X-KEY is sticky: the most recently declared key applies to every
    // subsequent segment until the next #EXT-X-KEY tag changes it.
    HlsKey  activeKey;
    int64_t nextSeq = 0;  // populated once we see MEDIA-SEQUENCE

    for (size_t li = 1; li < lines.size(); ++li) {
        std::wstring line = trim(lines[li]);
        if (line.empty()) continue;

        if (line[0] == L'#') {
            if (startsWith(line, L"#EXT-X-STREAM-INF:")) {
                auto attrs = parseAttrs(line.substr(18));
                pendingVariant = HlsVariant{};
                if (auto it = attrs.find(L"BANDWIDTH"); it != attrs.end())
                    try { pendingVariant.bandwidth = std::stoll(it->second); } catch (...) {}
                if (auto it = attrs.find(L"RESOLUTION"); it != attrs.end()) {
                    pendingVariant.resolution = it->second;
                    parseResolution(it->second, pendingVariant.width, pendingVariant.height);
                }
                if (auto it = attrs.find(L"CODECS"); it != attrs.end())
                    pendingVariant.codecs = it->second;
                sawStreamInf = true;
            } else if (startsWith(line, L"#EXT-X-TARGETDURATION:")) {
                try { media.targetDuration = std::stoi(line.substr(22)); } catch (...) {}
            } else if (startsWith(line, L"#EXT-X-MEDIA-SEQUENCE:")) {
                try {
                    media.mediaSequence = std::stoi(line.substr(22));
                    nextSeq = media.mediaSequence;
                } catch (...) {}
            } else if (startsWith(line, L"#EXT-X-ENDLIST")) {
                media.endList = true;
            } else if (startsWith(line, L"#EXT-X-MAP:")) {
                media.isFmp4 = true;
                auto attrs = parseAttrs(line.substr(11));
                if (auto it = attrs.find(L"URI"); it != attrs.end())
                    media.initUri = resolveUrl(baseUrl, it->second);
            } else if (startsWith(line, L"#EXT-X-KEY:")) {
                auto attrs = parseAttrs(line.substr(11));
                HlsKey k;
                auto methIt = attrs.find(L"METHOD");
                std::wstring meth = (methIt == attrs.end()) ? L"NONE" : methIt->second;
                if      (meth == L"NONE")       k.method = HlsCryptMethod::None;
                else if (meth == L"AES-128")    k.method = HlsCryptMethod::Aes128;
                else if (meth == L"SAMPLE-AES") k.method = HlsCryptMethod::SampleAes;
                else                            k.method = HlsCryptMethod::Unknown;

                if (k.method == HlsCryptMethod::Aes128) {
                    auto uriIt = attrs.find(L"URI");
                    if (uriIt != attrs.end())
                        k.keyUri = resolveUrl(baseUrl, uriIt->second);
                    auto ivIt = attrs.find(L"IV");
                    if (ivIt != attrs.end() && parseHexIv(ivIt->second, k.iv))
                        k.hasExplicitIv = true;
                }
                if (k.method != HlsCryptMethod::None) media.encrypted = true;
                if (k.method == HlsCryptMethod::SampleAes ||
                    k.method == HlsCryptMethod::Unknown) {
                    media.unsupportedCrypto = true;
                }
                activeKey = k;
            } else if (startsWith(line, L"#EXTINF:")) {
                // #EXTINF:<duration>,<title>
                std::wstring rest = line.substr(8);
                auto comma = rest.find(L',');
                std::wstring durStr = (comma == std::wstring::npos) ? rest : rest.substr(0, comma);
                try { pendingDuration = std::stod(durStr); } catch (...) { pendingDuration = 0.0; }
                sawExtInf = true;
            }
            // silently ignore other tags
        } else {
            std::wstring absolute = resolveUrl(baseUrl, line);
            if (sawStreamInf) {
                pendingVariant.url = absolute;
                variants.push_back(pendingVariant);
                sawStreamInf = false;
            } else if (sawExtInf) {
                HlsSegment s;
                s.url      = absolute;
                s.duration = pendingDuration;
                s.key      = activeKey;
                s.sequence = nextSeq++;
                media.segments.push_back(std::move(s));
                sawExtInf = false;
                pendingDuration = 0.0;
            }
            // orphan URL lines are silently skipped
        }
    }

    if (!variants.empty()) {
        out.isMaster = true;
        out.variants = std::move(variants);
    } else if (!media.segments.empty()) {
        out.isMaster = false;
        out.media    = std::move(media);
    } else {
        err = L"manifest contained no variants or segments";
        return false;
    }
    err.clear();
    return true;
}

bool fetchAndParseHls(const Url& url, const ExtraHeaders& headers,
                      HlsManifest& out, std::wstring& err) {
    std::string body;
    int status = httpFetchBody(url, headers, body, err);
    if (status < 200 || status >= 300) {
        if (err.empty()) {
            std::wostringstream s; s << L"HTTP " << status;
            err = s.str();
        }
        return false;
    }
    std::wstring text = utf8ToWide(body);
    return parseHls(text, url, out, err);
}

const HlsVariant* pickVariant(const std::vector<HlsVariant>& variants,
                              const std::wstring& quality) {
    if (variants.empty()) return nullptr;

    std::wstring q = lowerAscii(trim(quality));
    if (q.empty() || q == L"best") {
        const HlsVariant* best = &variants[0];
        for (auto& v : variants) if (v.bandwidth > best->bandwidth) best = &v;
        return best;
    }
    if (q == L"worst") {
        const HlsVariant* worst = &variants[0];
        for (auto& v : variants) if (v.bandwidth < worst->bandwidth) worst = &v;
        return worst;
    }
    // "720p" / "720" / "1080p" — match by height, nearest <= given.
    std::wstring digits;
    for (wchar_t c : q) if (c >= L'0' && c <= L'9') digits.push_back(c);
    if (!digits.empty()) {
        int target = 0;
        try { target = std::stoi(digits); } catch (...) { target = 0; }
        if (target > 0) {
            const HlsVariant* pick = nullptr;
            for (auto& v : variants) {
                if (v.height == target) return &v;      // exact match wins
                if (v.height > 0 && v.height <= target) {
                    if (!pick || v.height > pick->height) pick = &v;
                }
            }
            if (pick) return pick;
            // No variant at or below target: return the smallest we have.
            const HlsVariant* smallest = &variants[0];
            for (auto& v : variants)
                if (v.height > 0 && (smallest->height == 0 || v.height < smallest->height))
                    smallest = &v;
            return smallest;
        }
    }
    // fallback: best
    const HlsVariant* best = &variants[0];
    for (auto& v : variants) if (v.bandwidth > best->bandwidth) best = &v;
    return best;
}

}
