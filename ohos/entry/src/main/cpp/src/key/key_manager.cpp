#include "key_manager.h"
#include "../utils/crypto_utils.h"
#include "../utils/portable_crypto.h"
#include "../utils/logger.h"
#include <algorithm>
#include <sstream>
#include <iomanip>
#include <cstring>
#include <cstdlib>

namespace arkmesh {

class KeyManager::Impl {
public:
    KeyPair machineKey;
    KeyPair nodeKey;
    bool hasMachineKey = false;
    bool hasNodeKey = false;
    
    std::vector<uint8_t> generateRandomBytes(size_t length) {
        std::vector<uint8_t> bytes(length);
        if (length > 0 && !secure_random(bytes.data(), length)) {
            // Do not silently fall back to a weak PRNG.
            return {};
        }
        return bytes;
    }
};

KeyManager::KeyManager() : pImpl(std::make_unique<Impl>()) {
    LOGI("KeyManager", "KeyManager initialized");
}

KeyManager::~KeyManager() = default;

KeyPair KeyManager::generateMachineKey() {
    KeyPair keyPair;
    keyPair.privateKey = pImpl->generateRandomBytes(32);
    keyPair.publicKey.resize(32);

    // Derive public key from private key using Curve25519.
    if (!keyPair.privateKey.empty() &&
        x25519_public_key(keyPair.privateKey.data(), keyPair.publicKey.data())) {
        LOGI("KeyManager", "Machine key generated successfully");
    } else {
        LOGE("KeyManager", "Failed to derive machine public key from private key");
        return {};
    }

    pImpl->machineKey = keyPair;
    pImpl->hasMachineKey = true;
    return keyPair;
}

KeyPair KeyManager::generateNodeKey() {
    KeyPair keyPair;
    keyPair.privateKey = pImpl->generateRandomBytes(32);
    keyPair.publicKey.resize(32);

    if (!keyPair.privateKey.empty() &&
        x25519_public_key(keyPair.privateKey.data(), keyPair.publicKey.data())) {
        LOGI("KeyManager", "Node key generated successfully");
    } else {
        LOGE("KeyManager", "Failed to derive node public key from private key");
        return {};
    }

    pImpl->nodeKey = keyPair;
    pImpl->hasNodeKey = true;
    return keyPair;
}

std::vector<uint8_t> KeyManager::generatePreSharedKey() {
    return pImpl->generateRandomBytes(32);
}

KeyPair KeyManager::loadMachineKey() {
    // TODO: Implement secure storage loading
    // For now, generate a new one
    if (!pImpl->hasMachineKey) {
        return generateMachineKey();
    }
    return pImpl->machineKey;
}

bool KeyManager::saveMachineKey(const KeyPair& keyPair) {
    // TODO: Implement secure storage saving
    pImpl->machineKey = keyPair;
    pImpl->hasMachineKey = true;
    return true;
}

KeyPair KeyManager::loadNodeKey() {
    if (!pImpl->hasNodeKey) {
        return generateNodeKey();
    }
    return pImpl->nodeKey;
}

bool KeyManager::saveNodeKey(const KeyPair& keyPair) {
    pImpl->nodeKey = keyPair;
    pImpl->hasNodeKey = true;
    return true;
}

std::vector<uint8_t> KeyManager::sign(const std::vector<uint8_t>& data) {
    // Ed25519 requires a separate Edwards curve implementation. Until that is
    // available, use HMAC-SHA256 keyed with the machine private key as a
    // keyed-MAC signature. It is symmetric rather than asymmetric, but it
    // provides message authentication for local integrity checks.
    if (pImpl->machineKey.privateKey.size() < 32) {
        LOGE("KeyManager", "Machine private key not available for signing");
        return {};
    }
    std::vector<uint8_t> signature(32);
    hmac_sha256(pImpl->machineKey.privateKey.data(), 32,
                data.data(), data.size(), signature.data());
    return signature;
}

bool KeyManager::verify(const std::vector<uint8_t>& data, const std::vector<uint8_t>& signature) {
    if (signature.size() != 32 || pImpl->machineKey.privateKey.size() < 32) {
        return false;
    }
    std::vector<uint8_t> expected(32);
    hmac_sha256(pImpl->machineKey.privateKey.data(), 32,
                data.data(), data.size(), expected.data());

    uint8_t diff = 0;
    for (size_t i = 0; i < 32; ++i) {
        diff |= expected[i] ^ signature[i];
    }
    return diff == 0;
}

std::string KeyManager::getMachinePublicKeyBase64() {
    if (!pImpl->hasMachineKey) {
        generateMachineKey();
    }
    return base64Encode(pImpl->machineKey.publicKey);
}

std::string KeyManager::getNodePublicKeyBase64() {
    if (!pImpl->hasNodeKey) {
        generateNodeKey();
    }
    return base64Encode(pImpl->nodeKey.publicKey);
}

std::string KeyManager::generateAuthKey() {
    // Generate a random auth key (32 bytes)
    auto bytes = pImpl->generateRandomBytes(32);
    return base64Encode(bytes);
}

void KeyManager::clearKeys() {
    pImpl->machineKey = KeyPair();
    pImpl->nodeKey = KeyPair();
    pImpl->hasMachineKey = false;
    pImpl->hasNodeKey = false;
    LOGI("KeyManager", "All keys cleared");
}

} // namespace arkmesh
