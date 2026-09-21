#ifndef HEADSCALE_CLIENT_H
#define HEADSCALE_CLIENT_H

#include <string>
#include <vector>
#include <memory>
#include <functional>
#include <map>

namespace arkmesh {

struct NodeConfig {
    std::string id;
    std::string name;
    std::string publicKey;
    std::string meshIP;
    std::string hostName;
    std::string os;
    bool online;
    int64_t lastSeen;
    std::vector<std::string> allowedIPs;
    std::string endpoint;
};

struct NetworkConfig {
    std::vector<NodeConfig> nodes;
    std::vector<std::string> routes;
    std::string dnsConfig;
};

class HeadscaleClient {
public:
    HeadscaleClient();
    ~HeadscaleClient();

    // Initialize with server URL
    bool initialize(const std::string& serverUrl);
    
    // Set authentication key (for pre-auth key registration)
    void setAuthKey(const std::string& authKey);
    
    // Set machine key
    void setMachineKey(const std::string& publicKey, const std::string& privateKey);
    
    // Set node key (WireGuard node key, sent as NodeKey in RegisterRequest)
    void setNodeKey(const std::string& publicKey, const std::string& privateKey);
    
    // Register with Headscale using auth key
    bool registerWithAuthKey(const std::string& authKey);
    
    // Get network configuration from server
    NetworkConfig getNetworkConfig();
    
    // Send keepalive/ping to server
    bool sendKeepalive();
    
    // Get list of nodes
    std::vector<NodeConfig> getNodes();
    
    // Get own node configuration
    NodeConfig getSelfConfig();
    
    // Update endpoint (for NAT traversal)
    bool updateEndpoint(const std::string& endpoint);
    
    // Send STUN binding request
    bool sendSTUNRequest();
    
    // Get DERP map
    std::string getDERPMap();
    
    // Set status callback
    using StatusCallback = std::function<void(const std::string& status, const std::string& message)>;
    void setStatusCallback(StatusCallback callback);
    
    // Get the server URL
    std::string getServerUrl() const;
    
    // Check if connected
    bool isConnected() const;

private:
    class Impl;
    std::unique_ptr<Impl> pImpl;
};

} // namespace arkmesh

#endif // HEADSCALE_CLIENT_H
