#ifndef STUN_CLIENT_H
#define STUN_CLIENT_H

#include <string>
#include <vector>
#include <utility>
#include <memory>

namespace arkmesh {

struct STUNResult {
    std::string publicIP;
    uint16_t publicPort;
    std::string localIP;
    uint16_t localPort;
    bool success;
    std::string error;
};

class StunClient {
public:
    StunClient();
    ~StunClient();

    // Initialize with STUN server address
    bool initialize(const std::string& stunServer, uint16_t stunPort = 3478);
    
    // Perform STUN binding request
    // Returns the public endpoint (IP:port) of this device
    STUNResult performBindingRequest();
    
    // Perform multiple STUN requests to different servers
    std::vector<STUNResult> performMultipleRequests(
        const std::vector<std::pair<std::string, uint16_t>>& servers);
    
    // Get NAT type (Full Cone, Restricted Cone, Port Restricted, Symmetric)
    enum class NATType {
        Unknown,
        OpenInternet,
        FullCone,
        RestrictedCone,
        PortRestrictedCone,
        Symmetric,
        Blocked
    };
    
    NATType detectNATType();
    
    // Set local port for binding
    void setLocalPort(uint16_t port);
    
    // Get local port
    uint16_t getLocalPort() const;
    
    // Close the STUN client
    void close();

private:
    class Impl;
    std::unique_ptr<Impl> pImpl;
};

} // namespace arkmesh

#endif // STUN_CLIENT_H
