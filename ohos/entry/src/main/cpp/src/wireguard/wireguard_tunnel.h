#ifndef WIREGUARD_TUNNEL_H
#define WIREGUARD_TUNNEL_H

#include <string>
#include <vector>
#include <memory>
#include <functional>

namespace arkmesh {

// WireGuard peer configuration
struct WireGuardPeer {
    std::string publicKey;           // Base64 encoded public key
    std::string presharedKey;        // Base64 encoded pre-shared key (optional)
    std::string endpoint;            // IP:port
    std::vector<std::string> allowedIPs;  // Allowed IP ranges
    uint64_t persistentKeepalive;    // Keepalive interval in seconds
    uint64_t lastHandshake;          // Timestamp of last handshake
    uint64_t rxBytes;               // Received bytes
    uint64_t txBytes;              // Transmitted bytes
};

struct WireGuardConfig {
    std::string privateKey;          // Base64 encoded private key
    std::string address;             // IP address with CIDR (e.g., "100.64.0.1/32")
    uint16_t listenPort;             // UDP listen port (0 for random)
    uint16_t mtu;                   // MTU size
    std::vector<WireGuardPeer> peers;
};

// Callback for receiving packets from TUN device
using TunPacketCallback = std::function<void(const std::vector<uint8_t>& packet)>;

class WireGuardTunnel {
public:
    WireGuardTunnel();
    ~WireGuardTunnel();

    // Initialize the tunnel with configuration
    bool initialize(const WireGuardConfig& config);
    
    // Start the tunnel
    bool start();
    
    // Stop the tunnel
    void stop();
    
    // Check if tunnel is running
    bool isRunning() const;
    
    // Send a packet through the tunnel
    bool sendPacket(const std::vector<uint8_t>& packet);
    
    // Receive a packet from the tunnel (non-blocking)
    std::vector<uint8_t> receivePacket();
    
    // Set callback for receiving packets
    void setReceiveCallback(TunPacketCallback callback);
    
    // Add or update a peer
    bool addPeer(const WireGuardPeer& peer);
    bool removePeer(const std::string& publicKey);
    
    // Get tunnel statistics
    struct TunnelStats {
        uint64_t rxPackets;
        uint64_t txPackets;
        uint64_t rxBytes;
        uint64_t txBytes;
        uint64_t droppedPackets;
        uint64_t errors;
    };
    TunnelStats getStats() const;
    
    // Get the TUN device file descriptor
    int getTunFd() const;
    
    // Set the TUN device file descriptor (from VpnExtensionAbility)
    void setTunFd(int fd);
    
    // Get current configuration
    WireGuardConfig getConfig() const;
    
    // Perform handshake with all peers
    bool performHandshakes();
    
    // Update peer endpoint (for NAT traversal)
    bool updatePeerEndpoint(const std::string& publicKey, const std::string& endpoint);

private:
    class Impl;
    std::unique_ptr<Impl> pImpl;
};

} // namespace arkmesh

#endif // WIREGUARD_TUNNEL_H
