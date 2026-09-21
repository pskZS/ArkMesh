#include <vector>
#include <string>
#include <cstdlib>
#include <cstdint>
#include <cctype>
#include "portable_crypto.h"

namespace arkmesh {

// Base64 encoding
std::string base64Encode(const std::vector<uint8_t>& data) {
    static const std::string base64_chars = 
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    
    std::string encoded;
    int i = 0;
    uint8_t array3[3];
    uint8_t array4[4];
    
    for (uint8_t c : data) {
        array3[i++] = c;
        if (i == 3) {
            array4[0] = (array3[0] & 0xfc) >> 2;
            array4[1] = ((array3[0] & 0x03) << 4) + ((array3[1] & 0xf0) >> 4);
            array4[2] = ((array3[1] & 0x0f) << 2) + ((array3[2] & 0xc0) >> 6);
            array4[3] = array3[2] & 0x3f;
            
            for (int j = 0; j < 4; j++) {
                encoded += base64_chars[array4[j]];
            }
            i = 0;
        }
    }
    
    if (i != 0) {
        for (int j = i; j < 3; j++) {
            array3[j] = 0;
        }
        
        array4[0] = (array3[0] & 0xfc) >> 2;
        array4[1] = ((array3[0] & 0x03) << 4) + ((array3[1] & 0xf0) >> 4);
        array4[2] = ((array3[1] & 0x0f) << 2) + ((array3[2] & 0xc0) >> 6);
        
        for (int j = 0; j < (i + 1); j++) {
            encoded += base64_chars[array4[j]];
        }
        
        while ((i++) < 3) {
            encoded += '=';
        }
    }
    
    return encoded;
}

// Base64 decoding
std::vector<uint8_t> base64Decode(const std::string& encoded) {
    static const std::string base64_chars = 
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    
    std::vector<uint8_t> decoded;
    uint32_t buffer = 0;
    int bits = 0;
    
    for (char c : encoded) {
        if (c == '=' || c == '\n' || c == '\r' || c == ' ' || c == '\t') {
            continue;
        }
        
        size_t idx = base64_chars.find(c);
        if (idx == std::string::npos) {
            continue; // Skip invalid characters
        }
        
        buffer = (buffer << 6) | static_cast<uint32_t>(idx);
        bits += 6;
        
        if (bits >= 8) {
            bits -= 8;
            decoded.push_back(static_cast<uint8_t>((buffer >> bits) & 0xFF));
        }
    }
    
    return decoded;
}

// SHA256 hash
std::vector<uint8_t> sha256(const std::vector<uint8_t>& data) {
    std::vector<uint8_t> hash(32);
    arkmesh::sha256(data.data(), data.size(), hash.data());
    return hash;
}

// Generate random bytes
std::vector<uint8_t> generateRandomBytes(size_t length) {
    std::vector<uint8_t> bytes(length);
    if (length > 0 && !arkmesh::secure_random(bytes.data(), length)) {
        // Never fall back to a weak PRNG; return empty on failure so callers
        // can detect the error instead of silently using predictable data.
        return {};
    }
    return bytes;
}

} // namespace arkmesh
