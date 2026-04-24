#pragma once
#include <string>
#include <cstdint>

namespace matata {

struct Url {
    std::wstring scheme;
    std::wstring host;
    uint16_t     port = 0;
    std::wstring path;
    std::wstring query;
    std::wstring username;      // only populated for ftp:// (URLs with creds)
    std::wstring password;
    bool         secure = false;

    std::wstring authority() const;
    std::wstring pathAndQuery() const;
    std::wstring toString() const;
    std::wstring inferredFilename() const;
};

bool parseUrl(const std::wstring& raw, Url& out, std::wstring& err);

// Resolve `rel` against `base` the same way browsers do for simple cases:
//   - absolute (has "://")         -> rel as-is
//   - protocol-relative ("//...")  -> base.scheme + rel
//   - absolute path ("/...")       -> base.scheme + authority + rel
//   - everything else              -> base dir + rel (base's query stripped)
std::wstring resolveUrl(const Url& base, const std::wstring& rel);

}
