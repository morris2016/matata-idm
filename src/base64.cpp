#include "matata/base64.hpp"

namespace matata {

namespace {
constexpr char  kEnc[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
constexpr int kDecSentinel = -1;

int decVal(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return 26 + (c - 'a');
    if (c >= '0' && c <= '9') return 52 + (c - '0');
    if (c == '+') return 62;
    if (c == '/') return 63;
    return kDecSentinel;
}
}

std::string base64Encode(const uint8_t* data, size_t len) {
    std::string out;
    out.reserve(((len + 2) / 3) * 4);
    for (size_t i = 0; i < len; i += 3) {
        uint32_t v = (uint32_t)data[i] << 16;
        int n = 1;
        if (i + 1 < len) { v |= (uint32_t)data[i+1] << 8; ++n; }
        if (i + 2 < len) { v |= (uint32_t)data[i+2];      ++n; }
        out.push_back(kEnc[(v >> 18) & 0x3F]);
        out.push_back(kEnc[(v >> 12) & 0x3F]);
        out.push_back(n > 1 ? kEnc[(v >> 6) & 0x3F] : '=');
        out.push_back(n > 2 ? kEnc[v & 0x3F]        : '=');
    }
    return out;
}

std::vector<uint8_t> base64Decode(const std::string& text) {
    std::vector<uint8_t> out;
    out.reserve((text.size() / 4) * 3);
    uint32_t buf = 0;
    int      bits = 0;
    for (char c : text) {
        if (c == '=' || c == '\r' || c == '\n' || c == ' ') continue;
        int v = decVal(c);
        if (v == kDecSentinel) continue; // skip junk (tolerant)
        buf = (buf << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back((uint8_t)((buf >> bits) & 0xFF));
        }
    }
    return out;
}

}
