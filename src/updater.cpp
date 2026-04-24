#include "matata/updater.hpp"
#include "matata/http.hpp"
#include "matata/url.hpp"

#include <windows.h>

#include <cwctype>
#include <sstream>

namespace matata {

namespace {

std::wstring utf8ToWide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}

// Pull the string value of a top-level JSON field. Accepts escaped-quote
// `\"` sequences. Returns empty on failure. This is not a general JSON
// parser — we only need three flat fields from a trusted manifest.
std::string jsonStringField(const std::string& body, const char* key) {
    std::string needle = std::string("\"") + key + "\"";
    size_t k = body.find(needle);
    if (k == std::string::npos) return {};
    size_t colon = body.find(':', k + needle.size());
    if (colon == std::string::npos) return {};
    size_t i = colon + 1;
    while (i < body.size() && (body[i] == ' ' || body[i] == '\t')) ++i;
    if (i >= body.size() || body[i] != '"') return {};
    ++i;
    std::string out;
    while (i < body.size() && body[i] != '"') {
        if (body[i] == '\\' && i + 1 < body.size()) {
            char c = body[i+1];
            if      (c == 'n')  out.push_back('\n');
            else if (c == 't')  out.push_back('\t');
            else if (c == 'r')  out.push_back('\r');
            else if (c == '"')  out.push_back('"');
            else if (c == '\\') out.push_back('\\');
            else                out.push_back(c);
            i += 2;
        } else {
            out.push_back(body[i++]);
        }
    }
    return out;
}

}

// a.b.c version compare. Any trailing "-rc1" / "-beta" components are
// stripped; missing components default to 0.
int compareVersions(const std::wstring& a, const std::wstring& b) {
    auto nextPart = [](const std::wstring& s, size_t& i) -> int {
        int n = 0;
        while (i < s.size() && s[i] >= L'0' && s[i] <= L'9') {
            n = n * 10 + (s[i] - L'0');
            ++i;
        }
        if (i < s.size() && s[i] == L'.') ++i;
        return n;
    };
    size_t ia = 0, ib = 0;
    for (int k = 0; k < 4; ++k) {
        int x = (ia < a.size()) ? nextPart(a, ia) : 0;
        int y = (ib < b.size()) ? nextPart(b, ib) : 0;
        if (x != y) return x < y ? -1 : 1;
    }
    return 0;
}

bool checkForUpdate(const std::wstring& manifestUrl,
                    const std::wstring& currentVersion,
                    UpdateInfo& info, std::wstring& err) {
    Url u;
    if (!parseUrl(manifestUrl, u, err)) return false;
    if (u.scheme != L"https" && u.scheme != L"http") {
        err = L"updater manifest must be http(s)";
        return false;
    }

    std::string body;
    ExtraHeaders hdrs;
    int status = httpFetchBody(u, hdrs, body, err, 64 * 1024);
    if (status < 200 || status >= 300) {
        if (err.empty()) {
            std::wostringstream s; s << L"HTTP " << status;
            err = s.str();
        }
        return false;
    }

    info.latestVersion = utf8ToWide(jsonStringField(body, "version"));
    info.downloadUrl   = utf8ToWide(jsonStringField(body, "url"));
    info.notes         = utf8ToWide(jsonStringField(body, "notes"));

    if (info.latestVersion.empty() || info.downloadUrl.empty()) {
        err = L"manifest missing required fields";
        return false;
    }
    info.available = compareVersions(currentVersion, info.latestVersion) < 0;
    err.clear();
    return true;
}

}
