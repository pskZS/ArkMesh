#ifndef CRYPTO_UTILS_H
#define CRYPTO_UTILS_H

#include <string>
#include <vector>

namespace arkmesh {

// Base64 encoding
std::string base64Encode(const std::vector<uint8_t>& data);

// Base64 decoding
std::vector<uint8_t> base64Decode(const std::string& encoded);

// SHA256 hash
std::vector<uint8_t> sha256(const std::vector<uint8_t>& data);

// Generate random bytes
std::vector<uint8_t> generateRandomBytes(size_t length);

} // namespace arkmesh

#endif // CRYPTO_UTILS_H
