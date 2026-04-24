#include "matata/url.hpp"

#include <windows.h>
#include <winhttp.h>
#include <shlwapi.h>

#include <algorithm>
#include <sstream>

namespace matata {

std::wstring Url::authority() const {
    std::wostringstream ss;
    ss << host;
    const bool defaultPort = (secure && port == 443) || (!secure && port == 80);
    if (port != 0 && !defaultPort) ss << L':' << port;
    return ss.str();
}

std::wstring Url::pathAndQuery() const {
    std::wstring p = path.empty() ? L"/" : path;
    if (!query.empty()) p += L"?" + query;
    return p;
}

std::wstring Url::toString() const {
    std::wostringstream ss;
    ss << scheme << L"://" << authority() << pathAndQuery();
    return ss.str();
}

std::wstring Url::inferredFilename() const {
    std::wstring p = path;
    if (p.empty() || p == L"/") return L"index";
    auto slash = p.find_last_of(L'/');
    std::wstring name = (slash == std::wstring::npos) ? p : p.substr(slash + 1);
    if (name.empty()) name = L"index";

    // percent-decode
    std::wstring out;
    out.reserve(name.size());
    for (size_t i = 0; i < name.size(); ++i) {
        if (name[i] == L'%' && i + 2 < name.size()) {
            auto hex = [](wchar_t c) -> int {
                if (c >= L'0' && c <= L'9') return c - L'0';
                if (c >= L'a' && c <= L'f') return 10 + (c - L'a');
                if (c >= L'A' && c <= L'F') return 10 + (c - L'A');
                return -1;
            };
            int hi = hex(name[i+1]), lo = hex(name[i+2]);
            if (hi >= 0 && lo >= 0) {
                out.push_back(static_cast<wchar_t>((hi << 4) | lo));
                i += 2;
                continue;
            }
        }
        out.push_back(name[i]);
    }

    // strip characters illegal in NTFS filenames
    const std::wstring illegal = L"<>:\"/\\|?*";
    out.erase(std::remove_if(out.begin(), out.end(),
        [&](wchar_t c){ return illegal.find(c) != std::wstring::npos || c < 32; }),
        out.end());
    if (out.empty()) out = L"download";
    return out;
}

std::wstring resolveUrl(const Url& base, const std::wstring& rel) {
    if (rel.empty()) return base.toString();
    // Absolute?
    if (rel.find(L"://") != std::wstring::npos) return rel;
    // Protocol-relative ("//host/path")
    if (rel.size() >= 2 && rel[0] == L'/' && rel[1] == L'/')
        return base.scheme + L":" + rel;
    // Absolute path
    if (rel[0] == L'/')
        return base.scheme + L"://" + base.authority() + rel;
    // Relative path: take dir of base.path + rel.
    std::wstring dir = base.path;
    auto slash = dir.find_last_of(L'/');
    if (slash == std::wstring::npos) dir = L"/";
    else                              dir = dir.substr(0, slash + 1);
    return base.scheme + L"://" + base.authority() + dir + rel;
}

bool parseUrl(const std::wstring& raw, Url& out, std::wstring& err) {
    // WinHTTP's URL cracker only understands http(s) and ftp. For
    // ftps://host/... we massage the string to ftp://host/... before
    // the call and re-stamp the original scheme on the way out.
    bool wasFtps = false;
    std::wstring work = raw;
    if (work.size() > 7 &&
        (work[0] == L'F' || work[0] == L'f') &&
        (work[1] == L'T' || work[1] == L't') &&
        (work[2] == L'P' || work[2] == L'p') &&
        (work[3] == L'S' || work[3] == L's') &&
        work.compare(4, 3, L"://") == 0) {
        wasFtps = true;
        work = L"ftp://" + work.substr(7);
    }

    URL_COMPONENTS uc{};
    uc.dwStructSize = sizeof(uc);
    uc.dwSchemeLength    = (DWORD)-1;
    uc.dwHostNameLength  = (DWORD)-1;
    uc.dwUserNameLength  = (DWORD)-1;
    uc.dwPasswordLength  = (DWORD)-1;
    uc.dwUrlPathLength   = (DWORD)-1;
    uc.dwExtraInfoLength = (DWORD)-1;

    if (!WinHttpCrackUrl(work.c_str(), (DWORD)work.size(), 0, &uc)) {
        DWORD e = GetLastError();
        std::wostringstream s;
        s << L"WinHttpCrackUrl failed (" << e << L")";
        err = s.str();
        return false;
    }

    out.scheme.assign(uc.lpszScheme,    uc.dwSchemeLength);
    out.host  .assign(uc.lpszHostName,  uc.dwHostNameLength);
    out.path  .assign(uc.lpszUrlPath,   uc.dwUrlPathLength);
    if (uc.dwUserNameLength > 0 && uc.lpszUserName)
        out.username.assign(uc.lpszUserName, uc.dwUserNameLength);
    if (uc.dwPasswordLength > 0 && uc.lpszPassword)
        out.password.assign(uc.lpszPassword, uc.dwPasswordLength);
    if (uc.dwExtraInfoLength > 0 && uc.lpszExtraInfo && *uc.lpszExtraInfo == L'?') {
        out.query.assign(uc.lpszExtraInfo + 1, uc.dwExtraInfoLength - 1);
    }
    out.port   = uc.nPort;
    out.secure = (uc.nScheme == INTERNET_SCHEME_HTTPS);

    if (wasFtps) {
        out.scheme = L"ftps";
        out.secure = true;
    }

    if (out.scheme != L"http"  && out.scheme != L"https"
     && out.scheme != L"ftp"   && out.scheme != L"ftps") {
        err = L"unsupported scheme: " + out.scheme;
        return false;
    }
    if (out.host.empty()) { err = L"empty host"; return false; }
    return true;
}

}
