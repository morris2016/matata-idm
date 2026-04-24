#include "matata/bencode.hpp"

#include <cstring>
#include <sstream>

namespace matata {

namespace {

struct Cursor {
    const uint8_t* p;
    size_t         left;
};

bool parseValue(Cursor& c, BenValue& out, std::wstring& err);

bool parseInt(Cursor& c, int64_t& out, std::wstring& err) {
    if (c.left == 0 || *c.p != 'i') { err = L"expected 'i'"; return false; }
    ++c.p; --c.left;
    bool neg = false;
    if (c.left > 0 && *c.p == '-') { neg = true; ++c.p; --c.left; }
    int64_t v = 0;
    bool any = false;
    while (c.left > 0 && *c.p != 'e') {
        if (*c.p < '0' || *c.p > '9') { err = L"int: bad digit"; return false; }
        v = v * 10 + (*c.p - '0');
        ++c.p; --c.left;
        any = true;
    }
    if (c.left == 0 || *c.p != 'e') { err = L"int: missing 'e'"; return false; }
    ++c.p; --c.left;
    if (!any) { err = L"int: empty"; return false; }
    out = neg ? -v : v;
    return true;
}

bool parseStr(Cursor& c, std::string& out, std::wstring& err) {
    int64_t len = 0;
    bool any = false;
    while (c.left > 0 && *c.p != ':') {
        if (*c.p < '0' || *c.p > '9') { err = L"str: bad length char"; return false; }
        len = len * 10 + (*c.p - '0');
        ++c.p; --c.left;
        any = true;
    }
    if (!any || c.left == 0 || *c.p != ':') { err = L"str: missing ':'"; return false; }
    ++c.p; --c.left;
    if ((int64_t)c.left < len || len < 0) { err = L"str: length exceeds buffer"; return false; }
    out.assign((const char*)c.p, (size_t)len);
    c.p += len; c.left -= (size_t)len;
    return true;
}

bool parseList(Cursor& c, BenValue& out, std::wstring& err) {
    if (c.left == 0 || *c.p != 'l') { err = L"expected 'l'"; return false; }
    ++c.p; --c.left;
    while (c.left > 0 && *c.p != 'e') {
        BenValue v;
        if (!parseValue(c, v, err)) return false;
        out.list.push_back(std::move(v));
    }
    if (c.left == 0) { err = L"list: missing 'e'"; return false; }
    ++c.p; --c.left;
    return true;
}

bool parseDict(Cursor& c, BenValue& out, std::wstring& err) {
    if (c.left == 0 || *c.p != 'd') { err = L"expected 'd'"; return false; }
    ++c.p; --c.left;
    while (c.left > 0 && *c.p != 'e') {
        std::string key;
        if (!parseStr(c, key, err)) return false;
        BenValue v;
        if (!parseValue(c, v, err)) return false;
        out.dict[std::move(key)] = std::move(v);
    }
    if (c.left == 0) { err = L"dict: missing 'e'"; return false; }
    ++c.p; --c.left;
    return true;
}

bool parseValue(Cursor& c, BenValue& out, std::wstring& err) {
    if (c.left == 0) { err = L"unexpected EOF"; return false; }
    uint8_t b = *c.p;
    if (b == 'i') { out.kind = BenKind::Int;  return parseInt(c, out.i, err); }
    if (b == 'l') { out.kind = BenKind::List; return parseList(c, out, err); }
    if (b == 'd') { out.kind = BenKind::Dict; return parseDict(c, out, err); }
    if (b >= '0' && b <= '9') {
        out.kind = BenKind::Str;
        return parseStr(c, out.s, err);
    }
    err = L"unexpected token";
    return false;
}

}

bool bencodeParse(const uint8_t* data, size_t len,
                  BenValue& out, std::wstring& err) {
    Cursor c{ data, len };
    if (!parseValue(c, out, err)) return false;
    return true;
}

std::string bencodeEncode(const BenValue& v) {
    std::ostringstream os;
    switch (v.kind) {
    case BenKind::Int:
        os << 'i' << v.i << 'e';
        break;
    case BenKind::Str:
        os << v.s.size() << ':' << v.s;
        break;
    case BenKind::List:
        os << 'l';
        for (auto& e : v.list) os << bencodeEncode(e);
        os << 'e';
        break;
    case BenKind::Dict:
        os << 'd';
        // dict must be in lexicographic order of keys; std::map gives us that.
        for (auto& kv : v.dict) {
            os << kv.first.size() << ':' << kv.first
               << bencodeEncode(kv.second);
        }
        os << 'e';
        break;
    }
    return os.str();
}

const BenValue* benDictGet(const BenValue& d, const char* key) {
    if (d.kind != BenKind::Dict) return nullptr;
    auto it = d.dict.find(key);
    return it == d.dict.end() ? nullptr : &it->second;
}

}
