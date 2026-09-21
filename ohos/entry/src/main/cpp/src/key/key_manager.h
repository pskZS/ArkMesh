#ifndef KEY_MANAGER_H
#define KEY_MANAGER_H

#include <string>
#include <vector>
#include <memory>

namespace arkmesh {

// Curve25519 key pair
struct KeyPair {
    std::vector<uint8_t> publicKey;
    std::vector<uint8_t> privateKey;
};

class KeyManager {
public:
    KeyManager();
    ~KeyManager();

    // Generate a new Curve25519 key pair for machine identity
    KeyPair generateMachineKey();
    
    // Generate a new Curve25519 key pair for node (WireGuard)
    KeyPair generateNodeKey();
    
    // Generate a new pre-shared key
    std::vector<uint8_t> generatePreSharedKey();
    
    // Load machine key from secure storage
    KeyPair loadMachineKey();
    
    // Save machine key to secure storage
    bool saveMachineKey(const KeyPair& keyPair);
    
    // Load node key from secure storage
    KeyPair loadNodeKey();
    
    // Save node key to secure storage
    bool saveNodeKey(const KeyPair& keyPair);
    
    // Sign data with machine private key
    std::vector<uint8_t> sign(const std::vector<uint8_t>& data);
    
    // Verify signature with machine public key
    bool verify(const std::vector<uint8_t>& data, const std::vector<uint8_t>& signature);
    
    // Get machine public key as base64 string
    std::string getMachinePublicKeyBase64();
    
    // Get node public key as base64 string
    std::string getNodePublicKeyBase64();
    
    // Generate auth key (for pre-auth key registration)
    std::string generateAuthKey();
    
    // Clear all keys (for logout)
    void clearKeys();

private:
    class Impl;
    std::unique_ptr<Impl> pImpl;
};

} // namespace arkmesh

#endif // KEY_MANAGER_H
