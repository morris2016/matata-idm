#include "matata/torrent.hpp"
#include "matata/bencode.hpp"
#include "matata/digest.hpp"

#include <windows.h>
#include <bcrypt.h>

#include <cstring>
#include <cwctype>

namespace matata {

namespace {

std::wstring utf8ToWide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}

// Given a bencoded blob, locate the raw bytes of the `info` dict so we
// can hash them exactly as they appear on disk (SHA-1 of that slice is
// the infoHash). This is required because re-encoding a parsed dict can
// normalise keys and break hashes on edge-case inputs.
bool locateInfoSlice(const uint8_t* data, size_t len,
                     size_t& start, size_t& length) {
    // We expect the top-level to be a dict. Scan to the 4:info key.
    if (len < 2 || data[0] != 'd') return false;
    size_t i = 1;
    while (i < len && data[i] != 'e') {
        // Parse a string key.
        size_t lenStart = i;
        int64_t klen = 0;
        while (i < len && data[i] != ':') {
            if (data[i] < '0' || data[i] > '9') return false;
            klen = klen * 10 + (data[i] - '0');
            ++i;
        }
        if (i >= len) return false;
        ++i;  // skip ':'
        if ((int64_t)(len - i) < klen) return false;
        bool isInfo = (klen == 4 && std::memcmp(data + i, "info", 4) == 0);
        i += (size_t)klen;
        (void)lenStart;

        // Now walk the value, tracking its byte span.
        size_t vStart = i;
        // Recursive descent over one bencoded value.
        // Reuse of the parser would be cleaner but we only need to know
        // where the value ENDS — so a small specialised walker:
        auto walk = [&](auto&& self) -> bool {
            if (i >= len) return false;
            uint8_t b = data[i];
            if (b == 'i') {
                ++i;
                if (i < len && data[i] == '-') ++i;
                while (i < len && data[i] != 'e') {
                    if (data[i] < '0' || data[i] > '9') return false;
                    ++i;
                }
                if (i >= len) return false;
                ++i; return true;
            }
            if (b >= '0' && b <= '9') {
                int64_t n = 0;
                while (i < len && data[i] != ':') {
                    n = n * 10 + (data[i] - '0');
                    ++i;
                }
                if (i >= len) return false;
                ++i;
                if ((int64_t)(len - i) < n) return false;
                i += (size_t)n;
                return true;
            }
            if (b == 'l' || b == 'd') {
                ++i;
                while (i < len && data[i] != 'e') {
                    if (b == 'd') {
                        // Key must be a string.
                        if (!self(self)) return false;
                    }
                    if (!self(self)) return false;
                }
                if (i >= len) return false;
                ++i; return true;
            }
            return false;
        };
        if (!walk(walk)) return false;

        if (isInfo) {
            start  = vStart;
            length = i - vStart;
            return true;
        }
    }
    return false;
}

void hexDecodeInPlace(std::string& out, const std::wstring& hex) {
    auto nib = [](wchar_t c) -> int {
        if (c >= L'0' && c <= L'9') return c - L'0';
        if (c >= L'a' && c <= L'f') return 10 + (c - L'a');
        if (c >= L'A' && c <= L'F') return 10 + (c - L'A');
        return -1;
    };
    if (hex.size() % 2) return;
    out.clear();
    out.reserve(hex.size() / 2);
    for (size_t i = 0; i < hex.size(); i += 2) {
        int hi = nib(hex[i]), lo = nib(hex[i + 1]);
        if (hi < 0 || lo < 0) { out.clear(); return; }
        out.push_back((char)((hi << 4) | lo));
    }
}

std::wstring urlDecodeW(const std::wstring& s) {
    std::wstring out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == L'%' && i + 2 < s.size()) {
            auto nib = [](wchar_t c) -> int {
                if (c >= L'0' && c <= L'9') return c - L'0';
                if (c >= L'a' && c <= L'f') return 10 + (c - L'a');
                if (c >= L'A' && c <= L'F') return 10 + (c - L'A');
                return -1;
            };
            int hi = nib(s[i+1]), lo = nib(s[i+2]);
            if (hi >= 0 && lo >= 0) {
                out.push_back((wchar_t)((hi << 4) | lo));
                i += 2;
                continue;
            }
        } else if (s[i] == L'+') {
            out.push_back(L' ');
            continue;
        }
        out.push_back(s[i]);
    }
    return out;
}

} // anon

bool torrentParseFile(const uint8_t* data, size_t len,
                      TorrentMeta& out, std::wstring& err) {
    BenValue root;
    if (!bencodeParse(data, len, root, err)) return false;
    if (root.kind != BenKind::Dict) { err = L"top-level is not a dict"; return false; }

    // infoHash: SHA-1 of the raw info-dict bytes.
    size_t iStart = 0, iLen = 0;
    if (!locateInfoSlice(data, len, iStart, iLen)) {
        err = L"could not locate info dict for hashing";
        return false;
    }
    // Hash that slice via digest.cpp's helpers — but hashFile works on
    // paths, so we write the slice to a temp file. Simpler: inline BCrypt
    // here. Avoiding the temp file: re-implement a tiny SHA-1 call.
    BCRYPT_ALG_HANDLE  hAlg  = nullptr;
    BCRYPT_HASH_HANDLE hHash = nullptr;
    if (BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_SHA1_ALGORITHM,
                                    nullptr, 0) < 0) {
        err = L"BCryptOpenAlgorithmProvider(SHA1) failed";
        return false;
    }
    DWORD hashLen = 20, cb = 0;
    BCryptGetProperty(hAlg, BCRYPT_HASH_LENGTH, (PUCHAR)&hashLen,
                      sizeof(hashLen), &cb, 0);
    BCryptCreateHash(hAlg, &hHash, nullptr, 0, nullptr, 0, 0);
    BCryptHashData(hHash, (PUCHAR)(data + iStart), (ULONG)iLen, 0);
    BCryptFinishHash(hHash, out.infoHash, hashLen, 0);
    BCryptDestroyHash(hHash);
    BCryptCloseAlgorithmProvider(hAlg, 0);

    const BenValue* info = benDictGet(root, "info");
    if (!info || info->kind != BenKind::Dict) { err = L"missing 'info' dict"; return false; }

    if (auto* n = benDictGet(*info, "name"))
        if (n->kind == BenKind::Str) out.name = utf8ToWide(n->s);

    if (auto* pl = benDictGet(*info, "piece length"))
        if (pl->kind == BenKind::Int) out.pieceLength = pl->i;

    if (auto* p = benDictGet(*info, "pieces"))
        if (p->kind == BenKind::Str) {
            out.pieces.assign((const uint8_t*)p->s.data(),
                              (const uint8_t*)p->s.data() + p->s.size());
        }

    // Files: single-file vs multi-file mode.
    if (auto* length = benDictGet(*info, "length")) {
        TorrentFile f;
        f.path   = out.name;
        f.length = (length->kind == BenKind::Int) ? length->i : 0;
        out.totalLength = f.length;
        out.files.push_back(std::move(f));
    } else if (auto* files = benDictGet(*info, "files")) {
        if (files->kind == BenKind::List) {
            for (auto& fe : files->list) {
                if (fe.kind != BenKind::Dict) continue;
                TorrentFile f;
                if (auto* l = benDictGet(fe, "length"))
                    if (l->kind == BenKind::Int) f.length = l->i;
                if (auto* pth = benDictGet(fe, "path"))
                    if (pth->kind == BenKind::List) {
                        std::wstring joined = out.name;
                        for (auto& comp : pth->list) {
                            if (comp.kind != BenKind::Str) continue;
                            if (!joined.empty() && joined.back() != L'/') joined += L'/';
                            joined += utf8ToWide(comp.s);
                        }
                        f.path = joined;
                    }
                out.totalLength += f.length;
                out.files.push_back(std::move(f));
            }
        }
    }

    // Trackers: top-level announce + optional announce-list.
    if (auto* ann = benDictGet(root, "announce"))
        if (ann->kind == BenKind::Str) out.trackers.push_back(utf8ToWide(ann->s));

    if (auto* al = benDictGet(root, "announce-list"))
        if (al->kind == BenKind::List) {
            for (auto& tier : al->list) {
                if (tier.kind != BenKind::List) continue;
                for (auto& t : tier.list) {
                    if (t.kind == BenKind::Str)
                        out.trackers.push_back(utf8ToWide(t.s));
                }
            }
        }

    if (auto* c = benDictGet(root, "comment"))
        if (c->kind == BenKind::Str) out.comment = utf8ToWide(c->s);
    if (auto* cb = benDictGet(root, "created by"))
        if (cb->kind == BenKind::Str) out.createdBy = utf8ToWide(cb->s);

    err.clear();
    return true;
}

bool looksLikeMagnetUri(const std::wstring& s) {
    return s.size() > 8 && _wcsnicmp(s.c_str(), L"magnet:?", 8) == 0;
}

bool torrentParseMagnet(const std::wstring& magnet,
                        TorrentMeta& out, std::wstring& err) {
    if (!looksLikeMagnetUri(magnet)) {
        err = L"not a magnet URI";
        return false;
    }
    std::wstring q = magnet.substr(8);  // strip "magnet:?"

    bool gotHash = false;
    size_t i = 0;
    while (i < q.size()) {
        size_t amp = q.find(L'&', i);
        if (amp == std::wstring::npos) amp = q.size();
        std::wstring pair = q.substr(i, amp - i);
        i = amp + 1;
        auto eq = pair.find(L'=');
        if (eq == std::wstring::npos) continue;
        std::wstring key = pair.substr(0, eq);
        std::wstring val = urlDecodeW(pair.substr(eq + 1));

        if (key == L"xt" && val.size() > 9 &&
            _wcsnicmp(val.c_str(), L"urn:btih:", 9) == 0) {
            std::wstring rest = val.substr(9);
            if (rest.size() == 40) {
                // Base16 (hex) info hash.
                std::string raw;
                hexDecodeInPlace(raw, rest);
                if (raw.size() == 20) {
                    std::memcpy(out.infoHash, raw.data(), 20);
                    gotHash = true;
                }
            } else if (rest.size() == 32) {
                // Base32 info hash — skip decoding for v0.7 (foundation
                // only). We accept hex-encoded magnet URIs and note this
                // gap if someone hits it.
                err = L"base32 info hash in magnet not supported yet";
                return false;
            }
        } else if (key == L"dn") {
            out.name = val;
        } else if (key == L"tr") {
            out.trackers.push_back(val);
        }
    }
    if (!gotHash) { err = L"magnet missing xt=urn:btih:<hash>"; return false; }
    err.clear();
    return true;
}

}
