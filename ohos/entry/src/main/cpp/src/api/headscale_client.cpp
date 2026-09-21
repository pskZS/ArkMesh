#include "headscale_client.h"
#include "../utils/logger.h"
#include <thread>
#include <chrono>
#include <sstream>
#include <iomanip>
#include <cstring>
#include <cerrno>
#include <cctype>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <mutex>

namespace arkmesh {

// Headscale API endpoints
static const std::string API_REGISTER = "/machine/register";
// Tailscale control protocol capability version. Keep this in sync with
// tailscale.com/tailcfg.CurrentCapabilityVersion used by the server.
static const int TAILSCALE_CURRENT_CAPABILITY_VERSION = 106;
static const std::string API_MAP = "/machine/map";
static const std::string API_STATUS = "/machine/status";
static const std::string API_DERP = "/derp-map";
static const std::string API_STUN = "/stun";

// OpenSSL global initialization (thread-safe, called once)
static std::once_flag g_sslInitFlag;
static bool g_sslInitialized = false;

static void initOpenSSL() {
    std::call_once(g_sslInitFlag, []() {
        SSL_library_init();
        SSL_load_error_strings();
        OpenSSL_add_all_algorithms();
        g_sslInitialized = true;
        LOGI("Headscale", "OpenSSL initialized for TLS/HTTPS support");
    });
}

class HeadscaleClient::Impl {
public:
    std::string serverUrl;
    std::string authKey;
    std::string machinePublicKey;
    std::string machinePrivateKey;
    std::string nodePublicKey;
    std::string nodePrivateKey;
    bool connected = false;
    StatusCallback statusCallback;
    
    // HTTP client
    struct HttpResponse {
        int statusCode;
        std::string body;
        std::map<std::string, std::string> headers;
    };
    
    // Extract host and port from server URL
    void parseUrl(const std::string& url, std::string& host, int& port, bool& useHttps, std::string& basePath) {
        // Simple URL parser: supports http://host:port/path and https://host:port/path
        size_t pos = 0;
        std::string protocol = "http";
        basePath.clear();
        
        if (url.substr(0, 8) == "https://") {
            protocol = "https";
            pos = 8;
            useHttps = true;
            port = 443;
        } else if (url.substr(0, 7) == "http://") {
            pos = 7;
            useHttps = false;
            port = 80;
        } else {
            // Default to HTTP if no protocol specified
            useHttps = false;
            port = 80;
        }
        
        // Extract host and optional port
        size_t pathPos = url.find('/', pos);
        std::string hostPort = (pathPos != std::string::npos) ? url.substr(pos, pathPos - pos) : url.substr(pos);
        
        // Tailscale's direct client builds requests as serverURL + "/machine/register".
        // Preserve any base path instead of dropping it, so reverse-proxy subpath
        // deployments (e.g. https://host/headscale) do not 404.
        if (pathPos != std::string::npos) {
            basePath = url.substr(pathPos);
            while (!basePath.empty() && basePath.back() == '/') {
                basePath.pop_back();
            }
        }
        
        size_t colonPos = hostPort.find(':');
        if (colonPos != std::string::npos) {
            host = hostPort.substr(0, colonPos);
            try {
                port = std::stoi(hostPort.substr(colonPos + 1));
            } catch (...) {
                LOGW("Headscale", "Invalid port number in URL, using default");
            }
        } else {
            host = hostPort;
        }
    }
    
    // Send HTTP request (GET or POST) with TLS support
    HttpResponse sendHttpRequest(const std::string& method, const std::string& path, const std::string& body = "") {
        HttpResponse response;
        response.statusCode = -1;
        
        std::string host;
        int port = 80;
        bool useHttps = false;
        std::string basePath;
        parseUrl(serverUrl, host, port, useHttps, basePath);
        
        // Resolve hostname using getaddrinfo (supports IPv4/IPv6, detailed errors)
        std::string portStr = std::to_string(port);
        struct addrinfo hints;
        struct addrinfo* addrRes = nullptr;
        std::memset(&hints, 0, sizeof(hints));
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;

        int gaiRet = getaddrinfo(host.c_str(), portStr.c_str(), &hints, &addrRes);
        if (gaiRet != 0 || !addrRes) {
            LOGE("Headscale", "Failed to resolve hostname " + host + ": " + std::string(gai_strerror(gaiRet)));
            return response;
        }

        // Create TCP socket
        int sock = socket(addrRes->ai_family, addrRes->ai_socktype, addrRes->ai_protocol);
        if (sock < 0) {
            LOGE("Headscale", "Failed to create TCP socket: " + std::string(strerror(errno)));
            freeaddrinfo(addrRes);
            return response;
        }

        // Set socket timeouts (10s) to avoid indefinite blocking on connect/send/receive
        struct timeval sockTimeout;
        sockTimeout.tv_sec = 10;
        sockTimeout.tv_usec = 0;
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &sockTimeout, sizeof(sockTimeout));
        setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &sockTimeout, sizeof(sockTimeout));

        if (connect(sock, addrRes->ai_addr, addrRes->ai_addrlen) < 0) {
            LOGE("Headscale", "Failed to connect to " + host + ":" + portStr + ": " + std::string(strerror(errno)));
            freeaddrinfo(addrRes);
            close(sock);
            return response;
        }

        freeaddrinfo(addrRes);
        
        // Setup TLS if HTTPS
        SSL* ssl = nullptr;
        SSL_CTX* ctx = nullptr;
        if (useHttps) {
            initOpenSSL();
            
            const SSL_METHOD* method = TLS_client_method();
            if (method == nullptr) {
                LOGE("Headscale", "Failed to obtain TLS client method (OpenSSL unavailable?)");
                close(sock);
                return response;
            }

            ctx = SSL_CTX_new(method);
            if (!ctx) {
                char errBuf[256] = {0};
                ERR_error_string(ERR_get_error(), errBuf);
                LOGE("Headscale", "Failed to create SSL context: " + std::string(errBuf));
                close(sock);
                return response;
            }
            
            // Set default verify paths (system CA certs)
            SSL_CTX_set_default_verify_paths(ctx);
            
            // Optionally disable certificate verification for self-signed certs
            // SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, nullptr);
            
            ssl = SSL_new(ctx);
            if (!ssl) {
                LOGE("Headscale", "Failed to create SSL");
                SSL_CTX_free(ctx);
                close(sock);
                return response;
            }
            
            SSL_set_fd(ssl, sock);
            
            // Set SNI (Server Name Indication)
            SSL_set_tlsext_host_name(ssl, host.c_str());
            
            if (SSL_connect(ssl) <= 0) {
                int sslError = SSL_get_error(ssl, 0);
                LOGE("Headscale", "SSL handshake failed: SSL error code " + std::to_string(sslError));
                SSL_free(ssl);
                SSL_CTX_free(ctx);
                close(sock);
                return response;
            }
            
            LOGI("Headscale", "TLS handshake successful with " + host);
        }
        
        // Build HTTP request
        std::string fullPath = path;
        if (!basePath.empty()) {
            if (path.empty() || path[0] != '/') {
                fullPath = basePath + "/" + path;
            } else {
                fullPath = basePath + path;
            }
        }
        std::ostringstream request;
        request << method << " " << fullPath << " HTTP/1.1\r\n";
        request << "Host: " << host << "\r\n";
        request << "Content-Type: application/json\r\n";
        if (!body.empty()) {
            request << "Content-Length: " << body.size() << "\r\n";
        }
        request << "Connection: close\r\n";
        request << "\r\n";
        if (!body.empty()) {
            request << body;
        }
        
        std::string requestStr = request.str();
        LOGI("Headscale", (useHttps ? "HTTPS " : "HTTP ") + method + " " + fullPath + " to " + host + ":" + std::to_string(port));
        
        // Send request
        if (useHttps && ssl) {
            int written = SSL_write(ssl, requestStr.c_str(), requestStr.size());
            if (written <= 0) {
                int sslError = SSL_get_error(ssl, written);
                LOGE("Headscale", "Failed to send HTTPS request: SSL error code " + std::to_string(sslError));
            }
        } else {
            ssize_t sent = send(sock, requestStr.c_str(), requestStr.size(), 0);
            if (sent < 0) {
                LOGE("Headscale", "Failed to send HTTP request: " + std::string(strerror(errno)));
            }
        }
        
        // Receive response
        char buffer[4096];
        std::string responseStr;
        int bytesRead;
        
        if (useHttps && ssl) {
            while ((bytesRead = SSL_read(ssl, buffer, sizeof(buffer) - 1)) > 0) {
                buffer[bytesRead] = 0;
                responseStr += buffer;
            }
            if (bytesRead < 0) {
                int sslError = SSL_get_error(ssl, bytesRead);
                LOGE("Headscale", "SSL_read failed: SSL error code " + std::to_string(sslError));
            }
        } else {
            while ((bytesRead = recv(sock, buffer, sizeof(buffer) - 1, 0)) > 0) {
                buffer[bytesRead] = 0;
                responseStr += buffer;
            }
            if (bytesRead < 0) {
                LOGE("Headscale", "recv failed: " + std::string(strerror(errno)));
            }
        }
        
        // Cleanup
        if (ssl) {
            SSL_shutdown(ssl);
            SSL_free(ssl);
        }
        if (ctx) {
            SSL_CTX_free(ctx);
        }
        close(sock);
        
        // Parse response
        if (responseStr.empty()) {
            LOGE("Headscale", "Empty HTTP response from " + host + ":" + std::to_string(port) + " (connection failed or timed out)");
        }

        size_t httpPos = responseStr.find("HTTP/1.1 ");
        if (httpPos != std::string::npos) {
            size_t statusStart = httpPos + 9;
            size_t statusEnd = responseStr.find(" ", statusStart);
            if (statusEnd != std::string::npos) {
                try {
                    response.statusCode = std::stoi(responseStr.substr(statusStart, statusEnd - statusStart));
                    LOGI("Headscale", "HTTP response status: " + std::to_string(response.statusCode));
                } catch (...) {
                    LOGE("Headscale", "Failed to parse HTTP status code");
                }
            }
        }
        
        size_t bodyStart = responseStr.find("\r\n\r\n");
        if (bodyStart != std::string::npos) {
            response.body = responseStr.substr(bodyStart + 4);
        }
        
        return response;
    }
    
    HttpResponse httpPost(const std::string& path, const std::string& body) {
        return sendHttpRequest("POST", path, body);
    }
    
    HttpResponse httpGet(const std::string& path) {
        return sendHttpRequest("GET", path);
    }
    
    void notifyStatus(const std::string& status, const std::string& message) {
        if (statusCallback) {
            statusCallback(status, message);
        }
    }
};

HeadscaleClient::HeadscaleClient() : pImpl(std::make_unique<Impl>()) {
    LOGI("Headscale", "Headscale client created");
}
HeadscaleClient::~HeadscaleClient() = default;

bool HeadscaleClient::initialize(const std::string& serverUrl) {
    pImpl->serverUrl = serverUrl;
    LOGI("Headscale", "Headscale client initialized with URL: " + serverUrl);
    return true;
}

void HeadscaleClient::setAuthKey(const std::string& authKey) {
    pImpl->authKey = authKey;
}

void HeadscaleClient::setMachineKey(const std::string& publicKey, const std::string& privateKey) {
    pImpl->machinePublicKey = publicKey;
    pImpl->machinePrivateKey = privateKey;
}

void HeadscaleClient::setNodeKey(const std::string& publicKey, const std::string& privateKey) {
    pImpl->nodePublicKey = publicKey;
    pImpl->nodePrivateKey = privateKey;
}

bool HeadscaleClient::registerWithAuthKey(const std::string& authKey) {
    pImpl->authKey = authKey;
    
    auto ensureKeyPrefix = [](const std::string& key, const std::string& prefix) {
        if (key.compare(0, prefix.size(), prefix) == 0) {
            return key;
        }
        return prefix + key;
    };

    std::string nodeKey = ensureKeyPrefix(pImpl->nodePublicKey, "nodekey:");
    std::string machineKey = ensureKeyPrefix(pImpl->machinePublicKey, "mkey:");

    // Tailscale's RegisterRequest carries the pre-auth key in Auth.AuthKey,
    // not as a `?key=` query parameter. The control endpoint is always
    // POST /machine/register with a JSON body.
    std::ostringstream requestBody;
    requestBody << "{";
    requestBody << "\"Version\":" << TAILSCALE_CURRENT_CAPABILITY_VERSION << ",";
    requestBody << "\"NodeKey\":\"" << nodeKey << "\",";
    requestBody << "\"MachineKey\":\"" << machineKey << "\",";
    if (!authKey.empty()) {
        requestBody << "\"Auth\":{\"AuthKey\":\"" << authKey << "\"},";
    }
    requestBody << "\"Hostinfo\":{";
    requestBody << "\"OS\":\"harmonyos\",";
    requestBody << "\"Hostname\":\"harmony-device\"";
    requestBody << "}";
    requestBody << "}";

    auto response = pImpl->httpPost(API_REGISTER, requestBody.str());
    
    if (response.statusCode == 200) {
        // A 200 with a non-empty AuthURL means the node still needs browser
        // authorization. In that case, registration is not complete.
        if (response.body.find("\"AuthURL\":\"") != std::string::npos &&
            response.body.find("\"AuthURL\":\"\"") == std::string::npos) {
            LOGE("Headscale", "Registration returned AuthURL; interactive login required");
            pImpl->notifyStatus("error", "Server requires interactive authentication");
            return false;
        }
        pImpl->connected = true;
        LOGI("Headscale", "Successfully registered with auth key");
        pImpl->notifyStatus("connected", "Successfully registered with Headscale");
        return true;
    }
    
    LOGE("Headscale", "Registration failed with status: " + std::to_string(response.statusCode));
    pImpl->notifyStatus("error", "Failed to register: " + std::to_string(response.statusCode));
    return false;
}

NetworkConfig HeadscaleClient::getNetworkConfig() {
    NetworkConfig config;
    
    auto response = pImpl->httpGet(API_MAP);
    
    if (response.statusCode == 200) {
        LOGI("Headscale", "Network config received, body size: " + std::to_string(response.body.size()));
        
        // Simple JSON-like parsing to extract node information
        // In production, use a proper JSON library
        const std::string& body = response.body;
        
        // Look for node entries in the response
        // Expected format (simplified):
        // {"nodes": [{"publicKey": "...", "meshIP": "...", "endpoint": "..."}]}
        
        size_t nodesPos = body.find("\"nodes\"");
        if (nodesPos != std::string::npos) {
            size_t arrayStart = body.find('[', nodesPos);
            size_t arrayEnd = body.find(']', arrayStart);
            
            if (arrayStart != std::string::npos && arrayEnd != std::string::npos) {
                std::string nodesArray = body.substr(arrayStart, arrayEnd - arrayStart + 1);
                LOGI("Headscale", "Found nodes array: " + nodesArray.substr(0, 200));
                
                // Parse individual nodes (simplified)
                size_t nodePos = 0;
                while ((nodePos = nodesArray.find("\"publicKey\"", nodePos)) != std::string::npos) {
                    NodeConfig node;
                    
                    // Extract public key
                    size_t keyStart = nodesArray.find('"', nodePos + 11);
                    size_t keyEnd = std::string::npos;
                    if (keyStart != std::string::npos) {
                        keyStart = nodesArray.find('"', keyStart + 1);
                        keyEnd = nodesArray.find('"', keyStart + 1);
                        if (keyStart != std::string::npos && keyEnd != std::string::npos) {
                            node.publicKey = nodesArray.substr(keyStart + 1, keyEnd - keyStart - 1);
                        }
                    }
                    
                    // Extract mesh IP
                    size_t ipPos = nodesArray.find("\"meshIP\"", nodePos);
                    if (ipPos != std::string::npos) {
                        size_t ipStart = nodesArray.find('"', ipPos + 8);
                        if (ipStart != std::string::npos) {
                            ipStart = nodesArray.find('"', ipStart + 1);
                            size_t ipEnd = nodesArray.find('"', ipStart + 1);
                            if (ipStart != std::string::npos && ipEnd != std::string::npos) {
                                node.meshIP = nodesArray.substr(ipStart + 1, ipEnd - ipStart - 1);
                            }
                        }
                    }
                    
                    // Extract endpoint
                    size_t epPos = nodesArray.find("\"endpoint\"", nodePos);
                    if (epPos != std::string::npos) {
                        size_t epStart = nodesArray.find('"', epPos + 10);
                        if (epStart != std::string::npos) {
                            epStart = nodesArray.find('"', epStart + 1);
                            size_t epEnd = nodesArray.find('"', epStart + 1);
                            if (epStart != std::string::npos && epEnd != std::string::npos) {
                                node.endpoint = nodesArray.substr(epStart + 1, epEnd - epStart - 1);
                            }
                        }
                    }

                    // Extract hostName
                    size_t hnPos = nodesArray.find("\"hostName\"", nodePos);
                    if (hnPos != std::string::npos) {
                        size_t hnStart = nodesArray.find('"', hnPos + 10);
                        if (hnStart != std::string::npos) {
                            hnStart = nodesArray.find('"', hnStart + 1);
                            size_t hnEnd = nodesArray.find('"', hnStart + 1);
                            if (hnStart != std::string::npos && hnEnd != std::string::npos) {
                                node.hostName = nodesArray.substr(hnStart + 1, hnEnd - hnStart - 1);
                            }
                        }
                    }

                    // Extract os
                    size_t osPos = nodesArray.find("\"os\"", nodePos);
                    if (osPos != std::string::npos) {
                        size_t osStart = nodesArray.find('"', osPos + 4);
                        if (osStart != std::string::npos) {
                            osStart = nodesArray.find('"', osStart + 1);
                            size_t osEnd = nodesArray.find('"', osStart + 1);
                            if (osStart != std::string::npos && osEnd != std::string::npos) {
                                node.os = nodesArray.substr(osStart + 1, osEnd - osStart - 1);
                            }
                        }
                    }

                    // Extract online status
                    size_t onPos = nodesArray.find("\"online\"", nodePos);
                    if (onPos != std::string::npos) {
                        size_t onStart = nodesArray.find(':', onPos);
                        if (onStart != std::string::npos) {
                            node.online = (nodesArray.find("true", onStart) != std::string::npos &&
                                           nodesArray.find("true", onStart) < nodesArray.find('}', onStart));
                        }
                    }

                    // Extract lastSeen
                    size_t lsPos = nodesArray.find("\"lastSeen\"", nodePos);
                    if (lsPos != std::string::npos) {
                        size_t lsStart = nodesArray.find(':', lsPos);
                        if (lsStart != std::string::npos) {
                            try {
                                node.lastSeen = std::stoll(nodesArray.substr(lsStart + 1));
                            } catch (...) {
                                node.lastSeen = 0;
                            }
                        }
                    }
                    
                    if (!node.publicKey.empty()) {
                        LOGI("Headscale", "Parsed node: publicKey=" + node.publicKey.substr(0, 20) + 
                             "..., meshIP=" + node.meshIP + ", endpoint=" + node.endpoint);
                        config.nodes.push_back(node);
                    }
                    
                    if (keyEnd != std::string::npos) {
                        nodePos = keyEnd + 1;
                    } else {
                        nodePos += 1;
                    }
                }
            }
        }
        
        LOGI("Headscale", "Parsed " + std::to_string(config.nodes.size()) + " nodes from network config");
    } else {
        LOGW("Headscale", "Failed to get network config, status: " + std::to_string(response.statusCode));
    }
    
    return config;
}

bool HeadscaleClient::sendKeepalive() {
    auto response = pImpl->httpGet(API_STATUS);
    bool success = response.statusCode == 200;
    if (!success) {
        LOGW("Headscale", "Keepalive failed with status: " + std::to_string(response.statusCode));
    }
    return success;
}

std::vector<NodeConfig> HeadscaleClient::getNodes() {
    // Reuse the same network config parsing logic to avoid duplication.
    return getNetworkConfig().nodes;
}

NodeConfig HeadscaleClient::getSelfConfig() {
    NodeConfig config;
    // A minimal self view: in a full Tailscale-compatible implementation this
    // would come from the map response's "self" section. Expose what we know.
    config.publicKey = pImpl->machinePublicKey;
    config.hostName = "harmony-device";
    config.os = "harmonyos";
    config.online = pImpl->connected;
    config.lastSeen = 0;
    return config;
}

bool HeadscaleClient::updateEndpoint(const std::string& endpoint) {
    std::ostringstream requestBody;
    requestBody << "{";
    requestBody << "  \"endpoint\": \"" << endpoint << "\"";
    requestBody << "}";
    
    LOGI("Headscale", "Updating endpoint: " + endpoint);
    auto response = pImpl->httpPost(API_STATUS, requestBody.str());
    bool success = response.statusCode == 200;
    if (!success) {
        LOGE("Headscale", "Failed to update endpoint: " + std::to_string(response.statusCode));
    }
    return success;
}

bool HeadscaleClient::sendSTUNRequest() {
    // Send STUN binding request to server
    // This is handled by the STUN client
    return true;
}

std::string HeadscaleClient::getDERPMap() {
    auto response = pImpl->httpGet(API_DERP);
    
    if (response.statusCode == 200) {
        return response.body;
    }
    
    return "";
}

void HeadscaleClient::setStatusCallback(StatusCallback callback) {
    pImpl->statusCallback = callback;
}

std::string HeadscaleClient::getServerUrl() const {
    return pImpl->serverUrl;
}

bool HeadscaleClient::isConnected() const {
    return pImpl->connected;
}

} // namespace arkmesh
