#pragma once
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace matata {

// A permissive bencode parser — enough to read .torrent files.
// BitTorrent's bencode types:
//   integer  - "i<N>e"
//   string   - "<len>:<bytes>"      (raw bytes — not UTF-8 safe)
//   list     - "l<items>e"
//   dict     - "d<key1><val1>...e"  keys are strings in lexicographic order

enum class BenKind { Int, Str, List, Dict };

struct BenValue {
    BenKind                              kind = BenKind::Str;
    int64_t                              i    = 0;
    std::string                          s;            // raw bytes
    std::vector<BenValue>                list;
    std::map<std::string, BenValue>      dict;
};

// Parse a bencoded blob. Returns true on success.
bool bencodeParse(const uint8_t* data, size_t len,
                  BenValue& out, std::wstring& err);

// Re-encode a BenValue (useful for hashing the info dict).
std::string bencodeEncode(const BenValue& v);

// Look up a key in a dict BenValue; returns nullptr if missing / wrong
// kind. Keys are raw bytes, not UTF-8.
const BenValue* benDictGet(const BenValue& d, const char* key);

}
