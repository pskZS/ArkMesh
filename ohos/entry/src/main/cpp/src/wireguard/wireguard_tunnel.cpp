#include "wireguard_tunnel.h"
#include "../utils/logger.h"
#include "../utils/portable_crypto.h"
#include <thread>
#include <atomic>
#include <mutex>
#include <cstring>
#include <chrono>
#include <algorithm>
#include <unistd.h>
#include <errno.h>

namespace arkmesh {

// WireGuard constants
static const uint32_t WG_MAGIC = 0x01000001;
static const size_t WG_HEADER_SIZE = 16;
static const size_t WG_MAX_PACKET_SIZE = 65535;
static const size_t WG_KEY_SIZE = 32;

// Packet types
enum class PacketType : uint8_t {
    HandshakeInitiation = 1,
    HandshakeResponse = 2,
    CookieReply = 3,
    Data = 4
};

struct WireGuardHandshakeState {
    std::vector<uint8_t> ephemeralPrivate;
    std::vector<uint8_t> ephemeralPublic;
    std::vector<uint8_t> hash;
    std::vector<uint8_t> chainingKey;
    uint64_t lastTimestamp;
    bool initiated;
    bool complete;
};

// ChaCha20-Poly1305 encryption helper. The 16-byte authentication tag is
// appended to the ciphertext, matching the original OpenSSL-based layout.
static bool chacha20poly1305_encrypt(const uint8_t* key, const uint8_t* nonce,
                                      const uint8_t* plaintext, size_t plaintextLen,
                                      const uint8_t* ad, size_t adLen,
                                      uint8_t* ciphertext, size_t* ciphertextLen) {
    uint8_t tag[16];
    if (!arkmesh::chacha20poly1305_encrypt(key, nonce, plaintext, plaintextLen,
                                           ad, adLen, ciphertext, tag)) {
        return false;
    }
    std::memcpy(ciphertext + plaintextLen, tag, 16);
    *ciphertextLen = plaintextLen + 16;
    return true;
}

static bool chacha20poly1305_decrypt(const uint8_t* key, const uint8_t* nonce,
                                      const uint8_t* ciphertext, size_t ciphertextLen,
                                      const uint8_t* ad, size_t adLen,
                                      uint8_t* plaintext, size_t* plaintextLen) {
    if (ciphertextLen < 16) return false;
    size_t dataLen = ciphertextLen - 16;
    const uint8_t* tag = ciphertext + dataLen;

    if (!arkmesh::chacha20poly1305_decrypt(key, nonce, ciphertext, dataLen,
                                           ad, adLen, tag, plaintext)) {
        return false;
    }
    *plaintextLen = dataLen;
    return true;
}

class WireGuardTunnel::Impl {
public:
    WireGuardConfig config;
    std::atomic<bool> running{false};
    std::atomic<int> tunFd{-1};
    std::thread receiveThread;
    std::thread handshakeThread;
    std::mutex configMutex;
    std::mutex statsMutex;
    std::mutex recvCallbackMutex;
    TunnelStats stats{};
    TunPacketCallback receiveCallback;
    std::vector<WireGuardPeer> peers;
    std::vector<uint8_t> sendNonce;
    std::vector<uint8_t> receiveNonce;
    std::vector<uint8_t> sessionKey; // Symmetric key for data encryption
    
    void receiveLoop() {
        LOGI("WireGuard", "Receive loop started");
        while (running) {
            int fd = tunFd.load();
            if (fd < 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            }

            std::vector<uint8_t> buffer(WG_MAX_PACKET_SIZE);
            ssize_t n = ::read(fd, buffer.data(), buffer.size());
            if (n < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                    continue;
                }
                LOGE("WireGuard", "TUN read error: " + std::string(strerror(errno)));
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
                continue;
            }
            if (n == 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                continue;
            }

            buffer.resize(static_cast<size_t>(n));
            std::vector<uint8_t> decrypted = buffer;
            if (!decryptDataPacket(decrypted)) {
                LOGE("WireGuard", "Failed to decrypt packet");
                continue;
            }

            {
                std::lock_guard<std::mutex> lock(recvCallbackMutex);
                if (receiveCallback) {
                    receiveCallback(decrypted);
                }
            }

            {
                std::lock_guard<std::mutex> lock(statsMutex);
                stats.rxBytes += static_cast<uint64_t>(n);
            }
        }
        LOGI("WireGuard", "Receive loop stopped");
    }
    
    void handshakeLoop() {
        LOGI("WireGuard", "Handshake loop started");
        while (running) {
            // Perform handshakes with peers that need them
            for (auto& peer : peers) {
                // Check if handshake is needed
                auto now = std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count();
                
                if (now - static_cast<int64_t>(peer.lastHandshake) > 120) {
                    // Perform handshake
                    LOGI("WireGuard", "Performing handshake with peer: " + peer.endpoint);
                }
            }
            
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
        LOGI("WireGuard", "Handshake loop stopped");
    }
    
    std::vector<uint8_t> createHandshakeInitiation(const WireGuardPeer& peer) {
        LOGI("WireGuard", "Creating handshake initiation");
        std::vector<uint8_t> packet(WG_HEADER_SIZE + 148);
        
        // Magic
        packet[0] = 0x01;
        packet[1] = 0x00;
        packet[2] = 0x00;
        packet[3] = 0x00;
        
        // Reserved
        packet[4] = 0x00;
        packet[5] = 0x00;
        packet[6] = 0x00;
        packet[7] = 0x00;
        
        // Sender index
        uint32_t senderIndex = 0; // Generate unique index
        std::memcpy(&packet[8], &senderIndex, sizeof(senderIndex));
        
        // Ephemeral public key (32 bytes)
        // Hash (32 bytes)
        // Encrypted static public key (32 bytes)
        // Encrypted timestamp (12 bytes)
        // MAC1 (16 bytes)
        // MAC2 (16 bytes)
        
        return packet;
    }
    
    bool encryptDataPacket(std::vector<uint8_t>& packet) {
        if (sessionKey.empty()) {
            LOGE("WireGuard", "No session key available");
            return false;
        }
        
        // Increment nonce
        for (int i = sendNonce.size() - 1; i >= 0; i--) {
            sendNonce[i]++;
            if (sendNonce[i] != 0) break;
        }
        
        // Encrypt packet content
        // In real implementation, use WireGuard data packet format
        // For now, return success
        return true;
    }
    
    bool decryptDataPacket(std::vector<uint8_t>& packet) {
        if (sessionKey.empty()) {
            LOGE("WireGuard", "No session key available");
            return false;
        }
        
        // Decrypt packet content
        // In real implementation, use WireGuard data packet format
        // For now, return success
        return true;
    }
};

WireGuardTunnel::WireGuardTunnel() : pImpl(std::make_unique<Impl>()) {}
WireGuardTunnel::~WireGuardTunnel() = default;

bool WireGuardTunnel::initialize(const WireGuardConfig& config) {
    std::lock_guard<std::mutex> lock(pImpl->configMutex);
    pImpl->config = config;
    pImpl->peers = config.peers;
    pImpl->sendNonce.resize(12, 0);
    pImpl->receiveNonce.resize(12, 0);
    
    LOGI("WireGuard", "Tunnel initialized with address: " + config.address);
    return true;
}

bool WireGuardTunnel::start() {
    if (pImpl->running) {
        return true;
    }
    
    pImpl->running = true;
    
    // Start receive thread
    pImpl->receiveThread = std::thread(&Impl::receiveLoop, pImpl.get());
    
    // Start handshake thread
    pImpl->handshakeThread = std::thread(&Impl::handshakeLoop, pImpl.get());
    
    LOGI("WireGuard", "Tunnel started");
    return true;
}

void WireGuardTunnel::stop() {
    pImpl->running = false;
    
    if (pImpl->receiveThread.joinable()) {
        pImpl->receiveThread.join();
    }
    
    if (pImpl->handshakeThread.joinable()) {
        pImpl->handshakeThread.join();
    }
    
    LOGI("WireGuard", "Tunnel stopped");
}

bool WireGuardTunnel::isRunning() const {
    return pImpl->running;
}

bool WireGuardTunnel::sendPacket(const std::vector<uint8_t>& packet) {
    if (!pImpl->running) {
        return false;
    }

    int fd = pImpl->tunFd.load();
    if (fd < 0) {
        LOGE("WireGuard", "TUN fd is not set");
        return false;
    }

    // Encrypt packet
    std::vector<uint8_t> encryptedPacket = packet;
    if (!pImpl->encryptDataPacket(encryptedPacket)) {
        LOGE("WireGuard", "Failed to encrypt packet");
        return false;
    }

    ssize_t n = ::write(fd, encryptedPacket.data(), encryptedPacket.size());
    if (n < 0) {
        LOGE("WireGuard", "TUN write error: " + std::string(strerror(errno)));
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(pImpl->statsMutex);
        pImpl->stats.txPackets++;
        pImpl->stats.txBytes += packet.size();
    }

    return true;
}

std::vector<uint8_t> WireGuardTunnel::receivePacket() {
    if (!pImpl->running) {
        return {};
    }

    int fd = pImpl->tunFd.load();
    if (fd < 0) {
        return {};
    }

    std::vector<uint8_t> buffer(WG_MAX_PACKET_SIZE);
    ssize_t n = ::read(fd, buffer.data(), buffer.size());
    if (n <= 0) {
        return {};
    }

    buffer.resize(static_cast<size_t>(n));
    std::vector<uint8_t> decrypted = buffer;
    if (!pImpl->decryptDataPacket(decrypted)) {
        return {};
    }

    {
        std::lock_guard<std::mutex> lock(pImpl->statsMutex);
        pImpl->stats.rxPackets++;
        pImpl->stats.rxBytes += static_cast<uint64_t>(n);
    }

    return decrypted;
}

void WireGuardTunnel::setReceiveCallback(TunPacketCallback callback) {
    pImpl->receiveCallback = callback;
}

bool WireGuardTunnel::addPeer(const WireGuardPeer& peer) {
    std::lock_guard<std::mutex> lock(pImpl->configMutex);
    
    // Remove existing peer with same public key
    removePeer(peer.publicKey);
    
    pImpl->peers.push_back(peer);
    pImpl->config.peers.push_back(peer);
    
    LOGI("WireGuard", "Peer added: " + peer.endpoint);
    return true;
}

bool WireGuardTunnel::removePeer(const std::string& publicKey) {
    std::lock_guard<std::mutex> lock(pImpl->configMutex);
    
    auto it = std::remove_if(pImpl->peers.begin(), pImpl->peers.end(),
                              [&publicKey](const WireGuardPeer& peer) {
                                  return peer.publicKey == publicKey;
                              });
    pImpl->peers.erase(it, pImpl->peers.end());
    
    auto it2 = std::remove_if(pImpl->config.peers.begin(), pImpl->config.peers.end(),
                               [&publicKey](const WireGuardPeer& peer) {
                                   return peer.publicKey == publicKey;
                               });
    pImpl->config.peers.erase(it2, pImpl->config.peers.end());
    
    LOGI("WireGuard", "Peer removed");
    return true;
}

WireGuardTunnel::TunnelStats WireGuardTunnel::getStats() const {
    std::lock_guard<std::mutex> lock(pImpl->statsMutex);
    return pImpl->stats;
}

int WireGuardTunnel::getTunFd() const {
    return pImpl->tunFd.load();
}

void WireGuardTunnel::setTunFd(int fd) {
    pImpl->tunFd = fd;
}

WireGuardConfig WireGuardTunnel::getConfig() const {
    std::lock_guard<std::mutex> lock(pImpl->configMutex);
    return pImpl->config;
}

bool WireGuardTunnel::performHandshakes() {
    std::lock_guard<std::mutex> lock(pImpl->configMutex);
    
    for (const auto& peer : pImpl->peers) {
        // Initiate handshake with each peer
        auto packet = pImpl->createHandshakeInitiation(peer);
        // Send packet to peer endpoint
        LOGI("WireGuard", "Initiating handshake with: " + peer.endpoint);
    }
    
    return true;
}

bool WireGuardTunnel::updatePeerEndpoint(const std::string& publicKey, const std::string& endpoint) {
    std::lock_guard<std::mutex> lock(pImpl->configMutex);
    
    for (auto& peer : pImpl->peers) {
        if (peer.publicKey == publicKey) {
            peer.endpoint = endpoint;
            LOGI("WireGuard", "Peer endpoint updated: " + endpoint);
            return true;
        }
    }
    
    return false;
}

} // namespace arkmesh
