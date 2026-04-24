#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace matata {

std::string          base64Encode(const uint8_t* data, size_t len);
std::vector<uint8_t> base64Decode(const std::string& text);

}
