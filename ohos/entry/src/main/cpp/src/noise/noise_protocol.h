#ifndef NOISE_PROTOCOL_H
#define NOISE_PROTOCOL_H

#include <string>
#include <vector>
#include <memory>

namespace arkmesh {

// Noise protocol implementation for ArkMesh control plane
// Uses Noise_XX pattern with Curve25519, ChaCha20-Poly1305

class NoiseProtocol {
public:
    NoiseProtocol();
    ~NoiseProtocol();

    // Initialize Noise protocol with our private key
    bool initialize(const std::vector<uint8_t>& privateKey);
    
    // Perform handshake with server
    // Returns the encrypted initial message to send to server
    std::vector<uint8_t> initiateHandshake();
    
    // Process server's response during handshake
    bool processHandshakeResponse(const std::vector<uint8_t>& response);
    
    // Encrypt data for sending to server
    std::vector<uint8_t> encrypt(const std::vector<uint8_t>& plaintext);
    
    // Decrypt data received from server
    std::vector<uint8_t> decrypt(const std::vector<uint8_t>& ciphertext);
    
    // Check if handshake is complete
    bool isHandshakeComplete() const;
    
    // Get the server's public key
    std::vector<uint8_t> getServerPublicKey() const;
    
    // Set the server's public key (for known server case)
    void setServerPublicKey(const std::vector<uint8_t>& pubKey);

private:
    class Impl;
    std::unique_ptr<Impl> pImpl;
};

} // namespace arkmesh

#endif // NOISE_PROTOCOL_H
