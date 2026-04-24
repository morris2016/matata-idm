#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace matata {

// AES-128-CBC decrypt. `key` and `iv` must be exactly 16 bytes each.
// `ciphertext` length must be a multiple of 16. Output includes
// the original plaintext with PKCS#7 padding removed.
// Returns false with `err` populated on any error.
bool aes128CbcDecrypt(const uint8_t* key,
                      const uint8_t* iv,
                      const uint8_t* ciphertext,
                      size_t         clen,
                      std::vector<uint8_t>& plaintext,
                      std::wstring&  err);

// Parse an HLS #EXT-X-KEY IV attribute in 0xHEX form into 16 bytes.
// Returns false if it doesn't look like a 32-hex-nibble value.
bool parseHexIv(const std::wstring& hex, uint8_t out[16]);

// Derive the default HLS IV from a media-sequence number:
// 16-byte big-endian sequence (zero-padded).
void deriveIvFromSequence(int64_t seq, uint8_t out[16]);

}
