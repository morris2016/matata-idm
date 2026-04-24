#include "matata/digest.hpp"
#include "matata/base64.hpp"

#include <windows.h>
#include <bcrypt.h>

#include <cctype>
#include <cwctype>
#include <sstream>

namespace matata {

namespace {

const wchar_t* bcryptAlgId(DigestKind k) {
    switch (k) {
        case DigestKind::MD5:    return BCRYPT_MD5_ALGORITHM;
        case DigestKind::Sha1:   return BCRYPT_SHA1_ALGORITHM;
        case DigestKind::Sha256: return BCRYPT_SHA256_ALGORITHM;
        case DigestKind::Sha384: return BCRYPT_SHA384_ALGORITHM;
        case DigestKind::Sha512: return BCRYPT_SHA512_ALGORITHM;
        default:                 return nullptr;
    }
}

int strengthRank(DigestKind k) {
    switch (k) {
        case DigestKind::Sha512: return 5;
        case DigestKind::Sha384: return 4;
        case DigestKind::Sha256: return 3;
        case DigestKind::Sha1:   return 2;
        case DigestKind::MD5:    return 1;
        default:                 return 0;
    }
}

std::string wideToAscii(const std::wstring& w) {
    std::string s;
    s.reserve(w.size());
    for (wchar_t c : w) s.push_back((c < 128) ? (char)c : '?');
    return s;
}

std::wstring trim(std::wstring s) {
    size_t a = 0, b = s.size();
    while (a < b && iswspace(s[a])) ++a;
    while (b > a && iswspace(s[b-1])) --b;
    return s.substr(a, b - a);
}

DigestKind parseAlgoName(std::wstring name) {
    for (auto& c : name) c = (wchar_t)towlower(c);
    if (name == L"md5")     return DigestKind::MD5;
    if (name == L"sha")     return DigestKind::Sha1;
    if (name == L"sha-1")   return DigestKind::Sha1;
    if (name == L"sha-256") return DigestKind::Sha256;
    if (name == L"sha-384") return DigestKind::Sha384;
    if (name == L"sha-512") return DigestKind::Sha512;
    return DigestKind::None;
}

}

const wchar_t* digestName(DigestKind k) {
    switch (k) {
        case DigestKind::MD5:    return L"md5";
        case DigestKind::Sha1:   return L"sha-1";
        case DigestKind::Sha256: return L"sha-256";
        case DigestKind::Sha384: return L"sha-384";
        case DigestKind::Sha512: return L"sha-512";
        default:                 return L"none";
    }
}

DigestExpectation parseDigestHeader(const std::wstring& value) {
    DigestExpectation best;
    // Split on commas at the top level (values are base64, no commas inside).
    size_t pos = 0;
    while (pos < value.size()) {
        size_t comma = value.find(L',', pos);
        if (comma == std::wstring::npos) comma = value.size();
        std::wstring entry = trim(value.substr(pos, comma - pos));
        pos = comma + 1;
        auto eq = entry.find(L'=');
        if (eq == std::wstring::npos) continue;
        std::wstring algo = trim(entry.substr(0, eq));
        std::wstring val  = trim(entry.substr(eq + 1));
        DigestKind k = parseAlgoName(algo);
        if (k == DigestKind::None) continue;
        if (strengthRank(k) <= strengthRank(best.kind)) continue;
        auto raw = base64Decode(wideToAscii(val));
        if (raw.empty()) continue;
        best.kind = k;
        best.raw  = std::move(raw);
    }
    return best;
}

DigestExpectation parseContentMd5Header(const std::wstring& value) {
    DigestExpectation out;
    auto raw = base64Decode(wideToAscii(trim(value)));
    if (raw.size() == 16) { // MD5 is 16 bytes
        out.kind = DigestKind::MD5;
        out.raw  = std::move(raw);
    }
    return out;
}

bool hashFile(const std::wstring& path, DigestKind kind,
              std::vector<uint8_t>& out, std::wstring& err) {
    const wchar_t* algId = bcryptAlgId(kind);
    if (!algId) { err = L"unsupported digest kind"; return false; }

    BCRYPT_ALG_HANDLE  hAlg  = nullptr;
    BCRYPT_HASH_HANDLE hHash = nullptr;
    NTSTATUS st = BCryptOpenAlgorithmProvider(&hAlg, algId, nullptr, 0);
    if (st < 0) { err = L"BCryptOpenAlgorithmProvider failed"; return false; }

    DWORD hashLen = 0, cb = 0;
    BCryptGetProperty(hAlg, BCRYPT_HASH_LENGTH, (PUCHAR)&hashLen,
                      sizeof(hashLen), &cb, 0);

    st = BCryptCreateHash(hAlg, &hHash, nullptr, 0, nullptr, 0, 0);
    if (st < 0) {
        err = L"BCryptCreateHash failed";
        BCryptCloseAlgorithmProvider(hAlg, 0);
        return false;
    }

    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
                           nullptr, OPEN_EXISTING,
                           FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (f == INVALID_HANDLE_VALUE) {
        err = L"could not open file for hashing";
        BCryptDestroyHash(hHash);
        BCryptCloseAlgorithmProvider(hAlg, 0);
        return false;
    }

    std::vector<uint8_t> buf(256 * 1024);
    bool ok = true;
    for (;;) {
        DWORD got = 0;
        if (!ReadFile(f, buf.data(), (DWORD)buf.size(), &got, nullptr)) {
            err = L"ReadFile failed while hashing";
            ok = false; break;
        }
        if (got == 0) break;
        if (BCryptHashData(hHash, buf.data(), got, 0) < 0) {
            err = L"BCryptHashData failed";
            ok = false; break;
        }
    }
    CloseHandle(f);

    if (ok) {
        out.assign(hashLen, 0);
        if (BCryptFinishHash(hHash, out.data(), hashLen, 0) < 0) {
            err = L"BCryptFinishHash failed";
            ok = false;
        }
    }

    BCryptDestroyHash(hHash);
    BCryptCloseAlgorithmProvider(hAlg, 0);
    return ok;
}

}
