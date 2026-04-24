#include "matata/aes.hpp"

#include <windows.h>
#include <bcrypt.h>

#include <cstring>
#include <cwctype>

namespace matata {

namespace {

int hexDigit(wchar_t c) {
    if (c >= L'0' && c <= L'9') return c - L'0';
    if (c >= L'a' && c <= L'f') return 10 + (c - L'a');
    if (c >= L'A' && c <= L'F') return 10 + (c - L'A');
    return -1;
}

}

bool parseHexIv(const std::wstring& hex, uint8_t out[16]) {
    std::wstring s = hex;
    if (s.size() >= 2 && (s[0] == L'0' && (s[1] == L'x' || s[1] == L'X')))
        s = s.substr(2);
    if (s.size() != 32) return false;
    for (int i = 0; i < 16; ++i) {
        int hi = hexDigit(s[2 * i]);
        int lo = hexDigit(s[2 * i + 1]);
        if (hi < 0 || lo < 0) return false;
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return true;
}

void deriveIvFromSequence(int64_t seq, uint8_t out[16]) {
    std::memset(out, 0, 16);
    // Big-endian write of the sequence number into the last 8 bytes.
    for (int i = 0; i < 8; ++i) {
        out[15 - i] = (uint8_t)((seq >> (i * 8)) & 0xFF);
    }
}

bool aes128CbcDecrypt(const uint8_t* key,
                      const uint8_t* iv,
                      const uint8_t* ciphertext,
                      size_t         clen,
                      std::vector<uint8_t>& plaintext,
                      std::wstring&  err) {
    if (clen == 0 || (clen % 16) != 0) {
        err = L"ciphertext length must be a non-zero multiple of 16";
        return false;
    }

    BCRYPT_ALG_HANDLE hAlg = nullptr;
    NTSTATUS st = BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_AES_ALGORITHM,
                                              nullptr, 0);
    if (st < 0) { err = L"BCryptOpenAlgorithmProvider(AES) failed"; return false; }

    if (BCryptSetProperty(hAlg, BCRYPT_CHAINING_MODE,
                          (PUCHAR)BCRYPT_CHAIN_MODE_CBC,
                          sizeof(BCRYPT_CHAIN_MODE_CBC), 0) < 0) {
        err = L"BCryptSetProperty(CHAINING_MODE=CBC) failed";
        BCryptCloseAlgorithmProvider(hAlg, 0);
        return false;
    }

    BCRYPT_KEY_HANDLE hKey = nullptr;
    if (BCryptGenerateSymmetricKey(hAlg, &hKey, nullptr, 0,
                                   (PUCHAR)key, 16, 0) < 0) {
        err = L"BCryptGenerateSymmetricKey failed";
        BCryptCloseAlgorithmProvider(hAlg, 0);
        return false;
    }

    uint8_t ivCopy[16];
    std::memcpy(ivCopy, iv, 16); // BCrypt mutates the IV.

    // First pass: size output buffer.
    DWORD outLen = 0;
    NTSTATUS dec1 = BCryptDecrypt(hKey, (PUCHAR)ciphertext, (ULONG)clen, nullptr,
                                  ivCopy, 16, nullptr, 0, &outLen,
                                  BCRYPT_BLOCK_PADDING);
    if (dec1 < 0) {
        err = L"BCryptDecrypt size-query failed";
        BCryptDestroyKey(hKey);
        BCryptCloseAlgorithmProvider(hAlg, 0);
        return false;
    }

    plaintext.assign(outLen, 0);
    std::memcpy(ivCopy, iv, 16); // reset after the size query consumed it.
    DWORD written = 0;
    NTSTATUS dec2 = BCryptDecrypt(hKey, (PUCHAR)ciphertext, (ULONG)clen, nullptr,
                                  ivCopy, 16,
                                  plaintext.data(), (ULONG)plaintext.size(),
                                  &written, BCRYPT_BLOCK_PADDING);

    BCryptDestroyKey(hKey);
    BCryptCloseAlgorithmProvider(hAlg, 0);

    if (dec2 < 0) { err = L"BCryptDecrypt failed"; return false; }
    plaintext.resize(written);
    return true;
}

}
