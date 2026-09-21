#include "noise_protocol.h"
#include "../utils/logger.h"
#include "../utils/portable_crypto.h"
#include <cstring>
#include <algorithm>

namespace arkmesh {

// Noise protocol constants
static const size_t KEY_SIZE = 32;
static const size_t NONCE_SIZE = 12;
static const size_t TAG_SIZE = 16;
static const size_t PUBLIC_KEY_SIZE = 32;

class NoiseProtocol::Impl {
public:
    std::vector<uint8_t> privateKey;
    std::vector<uint8_t> publicKey;
    std::vector<uint8_t> serverPublicKey;
    std::vector<uint8_t> handshakeHash;
    std::vector<uint8_t> chainingKey;
    std::vector<uint8_t> encryptionKey;
    std::vector<uint8_t> decryptionKey;
    std::vector<uint8_t> encryptionNonce;
    std::vector<uint8_t> decryptionNonce;
    bool handshakeComplete = false;
    
    void initializeCipher(const std::vector<uint8_t>& key) {
        encryptionKey = key;
        decryptionKey = key;
        encryptionNonce.resize(NONCE_SIZE, 0);
        decryptionNonce.resize(NONCE_SIZE, 0);
    }
    
    void incrementNonce(std::vector<uint8_t>& nonce) {
        for (int i = NONCE_SIZE - 1; i >= 0; i--) {
            nonce[i]++;
            if (nonce[i] != 0) break;
        }
    }
};

NoiseProtocol::NoiseProtocol() : pImpl(std::make_unique<Impl>()) {
    LOGI("Noise", "Noise protocol handler created");
}
NoiseProtocol::~NoiseProtocol() = default;

bool NoiseProtocol::initialize(const std::vector<uint8_t>& privateKey) {
    if (privateKey.size() != 32) {
        LOGE("Noise", "Invalid private key size: " + std::to_string(privateKey.size()));
        return false;
    }
    
    pImpl->privateKey = privateKey;
    
    // Derive public key
    pImpl->publicKey.resize(32);
    if (!arkmesh::x25519_public_key(privateKey.data(), pImpl->publicKey.data())) {
        LOGE("Noise", "Failed to derive X25519 public key");
        return false;
    }
    
    LOGI("Noise", "Noise protocol initialized with public key");
    return true;
}

std::vector<uint8_t> NoiseProtocol::initiateHandshake() {
    // Noise_XX pattern: first message is our ephemeral public key
    // For ArkMesh, we start with the machine public key
    std::vector<uint8_t> message;
    message.reserve(PUBLIC_KEY_SIZE);
    message.insert(message.end(), pImpl->publicKey.begin(), pImpl->publicKey.end());
    
    return message;
}

bool NoiseProtocol::processHandshakeResponse(const std::vector<uint8_t>& response) {
    if (response.size() < PUBLIC_KEY_SIZE) {
        return false;
    }
    
    // Extract server's public key
    pImpl->serverPublicKey.assign(response.begin(), response.begin() + PUBLIC_KEY_SIZE);
    
    // Perform X25519 key exchange
    uint8_t sharedSecret[32];
    if (!arkmesh::x25519_shared_secret(pImpl->privateKey.data(), pImpl->serverPublicKey.data(), sharedSecret)) {
        LOGE("Noise", "X25519 key exchange failed during handshake");
        return false;
    }
    
    // Derive encryption keys using HKDF
    std::vector<uint8_t> key(KEY_SIZE);
    const char* info = "ArkMeshNoise-v1";
    arkmesh::hkdf_sha256(nullptr, 0, sharedSecret, 32,
                         reinterpret_cast<const uint8_t*>(info), std::strlen(info),
                         key.data(), KEY_SIZE);
    
    // Clear sensitive data
    std::memset(sharedSecret, 0, sizeof(sharedSecret));
    
    pImpl->initializeCipher(key);
    pImpl->handshakeComplete = true;
    
    LOGI("Noise", "Noise handshake completed successfully");
    return true;
}

std::vector<uint8_t> NoiseProtocol::encrypt(const std::vector<uint8_t>& plaintext) {
    if (!pImpl->handshakeComplete) {
        LOGE("Noise", "Cannot encrypt: handshake not complete");
        return {};
    }
    
    std::vector<uint8_t> ciphertext(plaintext.size() + TAG_SIZE);
    uint8_t tag[TAG_SIZE];
    if (!arkmesh::chacha20poly1305_encrypt(
            pImpl->encryptionKey.data(), pImpl->encryptionNonce.data(),
            plaintext.data(), plaintext.size(), nullptr, 0,
            ciphertext.data(), tag)) {
        LOGE("Noise", "ChaCha20-Poly1305 encrypt failed");
        return {};
    }
    
    // Append tag to ciphertext.
    std::copy(tag, tag + TAG_SIZE, ciphertext.begin() + plaintext.size());
    
    // Increment nonce.
    pImpl->incrementNonce(pImpl->encryptionNonce);
    
    return ciphertext;
}

std::vector<uint8_t> NoiseProtocol::decrypt(const std::vector<uint8_t>& ciphertext) {
    if (!pImpl->handshakeComplete || ciphertext.size() < TAG_SIZE) {
        LOGE("Noise", "Cannot decrypt: handshake not complete or ciphertext too short");
        return {};
    }
    
    size_t ciphertextLen = ciphertext.size() - TAG_SIZE;
    std::vector<uint8_t> plaintext(ciphertextLen);
    
    if (!arkmesh::chacha20poly1305_decrypt(
            pImpl->decryptionKey.data(), pImpl->decryptionNonce.data(),
            ciphertext.data(), ciphertextLen, nullptr, 0,
            ciphertext.data() + ciphertextLen, plaintext.data())) {
        LOGE("Noise", "ChaCha20-Poly1305 decrypt failed (authentication tag mismatch)");
        return {};
    }
    
    // Increment nonce.
    pImpl->incrementNonce(pImpl->decryptionNonce);
    
    return plaintext;
}

bool NoiseProtocol::isHandshakeComplete() const {
    return pImpl->handshakeComplete;
}

std::vector<uint8_t> NoiseProtocol::getServerPublicKey() const {
    return pImpl->serverPublicKey;
}

void NoiseProtocol::setServerPublicKey(const std::vector<uint8_t>& pubKey) {
    pImpl->serverPublicKey = pubKey;
}

} // namespace arkmesh
