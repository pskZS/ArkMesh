#ifndef ARKMESH_CORE_H
#define ARKMESH_CORE_H

#include <cstdint>
#include <string>
#include <vector>
#include <memory>
#include <functional>

namespace arkmesh {

// Forward declarations
class KeyManager;
class NoiseProtocol;
class WireGuardTunnel;
class HeadscaleClient;
class StunClient;

// Status callback for UI updates
using StatusCallback = std::function<void(const std::string& status, const std::string& message)>;

struct ArkMeshConfig {
    std::string serverUrl;
    std::string authKey;
    std::string userName;
    std::string machineName;
};

struct NodeInfo {
    std::string publicKey;
    std::string meshIP;
    std::string hostName;
    std::string os;
    bool online;
    int64_t lastSeen;
};

class ArkMeshCore {
public:
    ArkMeshCore();
    ~ArkMeshCore();

    // Initialize the client
    bool initialize(const ArkMeshConfig& config);
    
    // Connect to Headscale server
    bool connect();
    
    // Disconnect from server
    void disconnect();
    
    // Check if connected
    bool isConnected() const;
    
    // Get current status
    std::string getStatus() const;
    
    // Get list of nodes in the network
    std::vector<NodeInfo> getNodes() const;
    
    // Set status callback
    void setStatusCallback(StatusCallback callback);
    
    // Generate machine key (for first-time setup)
    std::string generateMachineKey();
    
    // Register with Headscale using auth key
    bool registerWithAuthKey(const std::string& authKey);
    
    // Get WireGuard interface name
    std::string getInterfaceName() const;
    
    // Get WireGuard IP address
    std::string getMeshIP() const;

private:
    class Impl;
    std::unique_ptr<Impl> pImpl;
};

} // namespace arkmesh

#endif // ARKMESH_CORE_H
