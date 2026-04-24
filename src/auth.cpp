#include "matata/auth.hpp"
#include "matata/base64.hpp"

#include <windows.h>

#include <cstdio>
#include <cwctype>
#include <sstream>
#include <string>

namespace matata {

namespace {

std::string wideToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(),
                                nullptr, 0, nullptr, nullptr);
    std::string out(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(),
                        out.data(), n, nullptr, nullptr);
    return out;
}

std::wstring utf8ToWide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring out(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), out.data(), n);
    return out;
}

std::wstring lowerAscii(std::wstring s) {
    for (auto& c : s) if (c < 128) c = (wchar_t)towlower(c);
    return s;
}

}

void appendBasicAuth(ExtraHeaders& hdrs, const BasicCreds& c) {
    if (c.empty()) return;
    std::string joined = wideToUtf8(c.user) + ":" + wideToUtf8(c.password);
    std::string enc = base64Encode((const uint8_t*)joined.data(), joined.size());
    hdrs.push_back({ L"Authorization", L"Basic " + utf8ToWide(enc) });
}

void appendBearerAuth(ExtraHeaders& hdrs, const std::wstring& token) {
    if (token.empty()) return;
    hdrs.push_back({ L"Authorization", L"Bearer " + token });
}

std::wstring defaultNetrcPath() {
    wchar_t buf[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"USERPROFILE", buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return L"";
    std::wstring p(buf, n);
    p += L"\\_netrc";
    return p;
}

// Minimal netrc tokenizer: whitespace-separated tokens, '#' starts a comment
// to end-of-line. macdef blocks are skipped (body terminated by blank line).
namespace {

struct Tokenizer {
    const std::string& src;
    size_t             pos = 0;

    explicit Tokenizer(const std::string& s) : src(s) {}

    void skipLineComment() {
        while (pos < src.size() && src[pos] != '\n') ++pos;
    }

    bool next(std::string& out) {
        while (pos < src.size()) {
            char c = src[pos];
            if (c == '#')                         { skipLineComment(); continue; }
            if (c == ' ' || c == '\t'
             || c == '\n' || c == '\r')           { ++pos; continue; }
            break;
        }
        if (pos >= src.size()) return false;
        out.clear();
        while (pos < src.size()) {
            char c = src[pos];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '#') break;
            out.push_back(c);
            ++pos;
        }
        return !out.empty();
    }

    // Skip the body of a macdef definition (empty line terminates).
    void skipMacdef() {
        while (pos < src.size()) {
            // consume to end of line
            size_t lineStart = pos;
            while (pos < src.size() && src[pos] != '\n') ++pos;
            if (pos < src.size()) ++pos;
            // check if the line we just consumed was empty (only whitespace)
            bool blank = true;
            for (size_t i = lineStart; i < pos; ++i) {
                char c = src[i];
                if (c != ' ' && c != '\t' && c != '\r' && c != '\n') { blank = false; break; }
            }
            if (blank) return;
        }
    }
};

}

BasicCreds lookupNetrc(const std::wstring& netrcPath,
                       const std::wstring& host) {
    BasicCreds matched;
    BasicCreds defaulted;
    bool       haveMatch = false;
    bool       haveDefault = false;

    FILE* f = nullptr;
    if (_wfopen_s(&f, netrcPath.c_str(), L"rb") != 0 || !f) return matched;

    std::string content;
    char cb[4096]; size_t got;
    while ((got = fread(cb, 1, sizeof(cb), f)) > 0) content.append(cb, got);
    fclose(f);

    std::wstring wantHost = lowerAscii(host);

    Tokenizer tok(content);

    enum class InBlock { None, Match, Other };
    InBlock inBlock = InBlock::None;
    BasicCreds cur;

    auto commitBlock = [&]() {
        if (inBlock == InBlock::Match && !haveMatch) {
            matched = cur; haveMatch = true;
        }
        cur = {};
    };

    std::string t;
    while (tok.next(t)) {
        if (t == "machine") {
            commitBlock();
            inBlock = InBlock::Other;
            if (tok.next(t)) {
                std::wstring hw = lowerAscii(utf8ToWide(t));
                if (hw == wantHost) inBlock = InBlock::Match;
            }
        } else if (t == "default") {
            commitBlock();
            inBlock = InBlock::Other;
            // treat the following login/password as a default, recorded
            // even if host didn't match any machine entry.
            while (tok.next(t)) {
                if (t == "login") {
                    if (tok.next(t)) defaulted.user = utf8ToWide(t);
                } else if (t == "password") {
                    if (tok.next(t)) defaulted.password = utf8ToWide(t);
                } else if (t == "account") {
                    tok.next(t); // ignore
                } else if (t == "macdef") {
                    tok.next(t); // skip name
                    tok.skipMacdef();
                } else if (t == "machine" || t == "default") {
                    // next block begins; rewind so outer loop handles it
                    // (simplest: just push back by seeking — but Tokenizer
                    // doesn't support it, so handle inline).
                    commitBlock();
                    if (t == "machine") {
                        inBlock = InBlock::Other;
                        if (tok.next(t)) {
                            std::wstring hw = lowerAscii(utf8ToWide(t));
                            if (hw == wantHost) inBlock = InBlock::Match;
                        }
                    } else {
                        inBlock = InBlock::Other;
                    }
                    break;
                }
            }
            if (!defaulted.empty()) haveDefault = true;
            inBlock = InBlock::None;
        } else if (inBlock != InBlock::None) {
            if (t == "login") {
                if (tok.next(t)) cur.user = utf8ToWide(t);
            } else if (t == "password") {
                if (tok.next(t)) cur.password = utf8ToWide(t);
            } else if (t == "account") {
                tok.next(t); // ignore
            } else if (t == "macdef") {
                tok.next(t); // skip macro name
                tok.skipMacdef();
            }
            // unknown keywords inside a block: ignore
        }
    }
    commitBlock();

    if (haveMatch) return matched;
    if (haveDefault) return defaulted;
    return {};
}

}
