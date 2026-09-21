#include "stun_client.h"
#include "../utils/logger.h"
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <cstring>
#include <chrono>
#include <cstdlib>

namespace arkmesh {

// STUN message types
static const uint16_t STUN_BINDING_REQUEST = 0x0001;
static const uint16_t STUN_BINDING_RESPONSE = 0x0101;
static const uint32_t STUN_MAGIC_COOKIE = 0x2112A442;

// STUN attribute types
static const uint16_t STUN_ATTR_MAPPED_ADDRESS = 0x0001;
static const uint16_t STUN_ATTR_XOR_MAPPED_ADDRESS = 0x0020;
static const uint16_t STUN_ATTR_ERROR_CODE = 0x0009;
static const uint16_t STUN_ATTR_UNKNOWN_ATTRIBUTES = 0x000A;

class StunClient::Impl {
public:
    std::string stunServer;
    uint16_t stunPort = 3478;
    uint16_t localPort = 0;
    int sock = -1;
    
    bool createSocket() {
        sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (sock < 0) {
            LOGE("STUN", "Failed to create UDP socket");
            return false;
        }
        
        // Bind to local port
        if (localPort > 0) {
            struct sockaddr_in localAddr;
            std::memset(&localAddr, 0, sizeof(localAddr));
            localAddr.sin_family = AF_INET;
            localAddr.sin_port = htons(localPort);
            localAddr.sin_addr.s_addr = INADDR_ANY;
            
            if (bind(sock, reinterpret_cast<struct sockaddr*>(&localAddr), 
                     sizeof(localAddr)) < 0) {
                LOGE("STUN", "Failed to bind to local port " + std::to_string(localPort));
                ::close(sock);
                sock = -1;
                return false;
            }
        }
        
        // Set timeout
        struct timeval tv;
        tv.tv_sec = 5;
        tv.tv_usec = 0;
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        
        return true;
    }
    
    bool sendSTUNRequest() {
        if (sock < 0) {
            return false;
        }
        
        // Build STUN binding request
        struct stun_header {
            uint16_t type;
            uint16_t length;
            uint32_t magicCookie;
            uint8_t transactionId[12];
        };
        
        struct stun_header request;
        request.type = htons(STUN_BINDING_REQUEST);
        request.length = 0;
        request.magicCookie = htonl(STUN_MAGIC_COOKIE);
        
        // Generate transaction ID
        for (int i = 0; i < 12; i++) {
            request.transactionId[i] = static_cast<uint8_t>(rand() % 256);
        }
        
        // Resolve STUN server address
        struct hostent* hostInfo = gethostbyname(stunServer.c_str());
        if (!hostInfo) {
            LOGE("STUN", "Failed to resolve STUN server: " + stunServer);
            return false;
        }
        
        struct sockaddr_in serverAddr;
        std::memset(&serverAddr, 0, sizeof(serverAddr));
        serverAddr.sin_family = AF_INET;
        serverAddr.sin_port = htons(stunPort);
        std::memcpy(&serverAddr.sin_addr, hostInfo->h_addr_list[0], hostInfo->h_length);
        
        // Send request
        if (sendto(sock, &request, sizeof(request), 0,
                   reinterpret_cast<struct sockaddr*>(&serverAddr), sizeof(serverAddr)) < 0) {
            LOGE("STUN", "Failed to send STUN request");
            return false;
        }
        
        return true;
    }
    
    STUNResult parseSTUNResponse() {
        STUNResult result;
        result.success = false;
        
        char buffer[2048];
        struct sockaddr_in fromAddr;
        socklen_t fromLen = sizeof(fromAddr);
        
        int bytesRead = recvfrom(sock, buffer, sizeof(buffer), 0,
                                reinterpret_cast<struct sockaddr*>(&fromAddr), &fromLen);
        
        if (bytesRead < 20) {
            result.error = "Invalid response size";
            LOGE("STUN", "Invalid STUN response size: " + std::to_string(bytesRead));
            return result;
        }
        
        // Parse STUN header
        uint16_t msgType = ntohs(*reinterpret_cast<uint16_t*>(buffer));
        uint16_t msgLength = ntohs(*reinterpret_cast<uint16_t*>(buffer + 2));
        uint32_t magicCookie = ntohl(*reinterpret_cast<uint32_t*>(buffer + 4));
        
        if (magicCookie != STUN_MAGIC_COOKIE) {
            result.error = "Invalid magic cookie";
            LOGE("STUN", "Invalid STUN magic cookie");
            return result;
        }
        
        if (msgType != STUN_BINDING_RESPONSE) {
            result.error = "Not a binding response";
            LOGE("STUN", "Unexpected STUN response type: " + std::to_string(msgType));
            return result;
        }
        
        // Parse attributes
        size_t offset = 20;
        while (offset + 4 <= static_cast<size_t>(bytesRead)) {
            uint16_t attrType = ntohs(*reinterpret_cast<uint16_t*>(buffer + offset));
            uint16_t attrLength = ntohs(*reinterpret_cast<uint16_t*>(buffer + offset + 2));
            
            if (attrType == STUN_ATTR_XOR_MAPPED_ADDRESS || attrType == STUN_ATTR_MAPPED_ADDRESS) {
                uint8_t family = buffer[offset + 5];
                uint16_t port = ntohs(*reinterpret_cast<uint16_t*>(buffer + offset + 6));
                
                if (family == 0x01) { // IPv4
                    uint32_t ip = ntohl(*reinterpret_cast<uint32_t*>(buffer + offset + 8));
                    struct in_addr addr;
                    addr.s_addr = htonl(ip);
                    result.publicIP = inet_ntoa(addr);
                    result.publicPort = port;
                    result.success = true;
                    break;
                }
            }
            
            offset += 4 + attrLength;
            if (offset % 4 != 0) {
                offset += 4 - (offset % 4); // Padding
            }
        }
        
        // Get local endpoint
        struct sockaddr_in localAddr;
        socklen_t localLen = sizeof(localAddr);
        if (getsockname(sock, reinterpret_cast<struct sockaddr*>(&localAddr), &localLen) == 0) {
            result.localIP = inet_ntoa(localAddr.sin_addr);
            result.localPort = ntohs(localAddr.sin_port);
        }
        
        return result;
    }
};

StunClient::StunClient() : pImpl(std::make_unique<Impl>()) {
    LOGI("STUN", "STUN client created");
}
StunClient::~StunClient() = default;

bool StunClient::initialize(const std::string& stunServer, uint16_t stunPort) {
    pImpl->stunServer = stunServer;
    pImpl->stunPort = stunPort;
    LOGI("STUN", "STUN client initialized with server: " + stunServer + ":" + std::to_string(stunPort));
    return true;
}

STUNResult StunClient::performBindingRequest() {
    STUNResult result;
    result.success = false;
    
    if (!pImpl->createSocket()) {
        result.error = "Failed to create socket";
        LOGE("STUN", "Failed to create STUN socket");
        return result;
    }
    
    if (!pImpl->sendSTUNRequest()) {
        result.error = "Failed to send STUN request";
        LOGE("STUN", "Failed to send STUN request");
        ::close(pImpl->sock);
        pImpl->sock = -1;
        return result;
    }
    
    result = pImpl->parseSTUNResponse();
    
    ::close(pImpl->sock);
    pImpl->sock = -1;
    
    if (result.success) {
        LOGI("STUN", "STUN binding success: public endpoint " + result.publicIP + ":" + std::to_string(result.publicPort));
    } else {
        LOGE("STUN", "STUN binding failed: " + result.error);
    }
    
    return result;
}

std::vector<STUNResult> StunClient::performMultipleRequests(
    const std::vector<std::pair<std::string, uint16_t>>& servers) {
    
    std::vector<STUNResult> results;
    
    for (const auto& server : servers) {
        initialize(server.first, server.second);
        results.push_back(performBindingRequest());
    }
    
    return results;
}

StunClient::NATType StunClient::detectNATType() {
    // Simplified NAT type detection
    // In a real implementation, this would use multiple STUN servers
    // and different binding patterns to determine the NAT type
    
    return NATType::Unknown;
}

void StunClient::setLocalPort(uint16_t port) {
    pImpl->localPort = port;
}

uint16_t StunClient::getLocalPort() const {
    return pImpl->localPort;
}

void StunClient::close() {
    if (pImpl->sock >= 0) {
        ::close(pImpl->sock);
        pImpl->sock = -1;
    }
}

} // namespace arkmesh
