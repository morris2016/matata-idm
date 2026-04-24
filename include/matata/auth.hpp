#pragma once
#include "matata/http.hpp"

#include <string>

namespace matata {

struct BasicCreds {
    std::wstring user;
    std::wstring password;
    bool empty() const { return user.empty() && password.empty(); }
};

// Append "Authorization: Basic base64(user:pass)" to headers.
void appendBasicAuth(ExtraHeaders& hdrs, const BasicCreds& c);

// Append "Authorization: Bearer <token>" to headers.
void appendBearerAuth(ExtraHeaders& hdrs, const std::wstring& token);

// Default netrc path: %USERPROFILE%\_netrc (Windows convention).
// Returns empty if USERPROFILE isn't set.
std::wstring defaultNetrcPath();

// Look up `host` in the netrc file. Returns empty creds if not found.
// "default" entry is used as a fallback.
BasicCreds lookupNetrc(const std::wstring& netrcPath,
                       const std::wstring& host);

}
