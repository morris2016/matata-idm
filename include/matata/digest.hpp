#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace matata {

enum class DigestKind {
    None,
    MD5,
    Sha1,
    Sha256,
    Sha384,
    Sha512,
};

const wchar_t* digestName(DigestKind k);

// Parse an RFC 3230 Digest header value (e.g. "sha-256=base64, md5=base64")
// and pick the strongest algorithm we understand. Returns {None, {}} if
// nothing usable is found.
struct DigestExpectation {
    DigestKind           kind = DigestKind::None;
    std::vector<uint8_t> raw;   // raw digest bytes
};

DigestExpectation parseDigestHeader(const std::wstring& value);

// Parse a Content-MD5 header (base64-encoded MD5).
DigestExpectation parseContentMd5Header(const std::wstring& value);

// Compute the digest of a file. Returns true on success.
bool hashFile(const std::wstring& path, DigestKind kind,
              std::vector<uint8_t>& out, std::wstring& err);

}
