#include "arkmesh_core.h"
#include "key/key_manager.h"
#include "noise/noise_protocol.h"
#include "wireguard/wireguard_tunnel.h"
#include "api/headscale_client.h"
#include "stun/stun_client.h"
#include "utils/logger.h"
#include <thread>
#include <chrono>
#include <atomic>
#include <sstream>
#include <mutex>

namespace arkmesh {

class ArkMeshCore::Impl {
public:
    ArkMeshConfig config;
    std::unique_ptr<KeyManager> keyManager;
    std::unique_ptr<NoiseProtocol> noiseProtocol;
    std::unique_ptr<WireGuardTunnel> wireGuardTunnel;
    std::unique_ptr<HeadscaleClient> headscaleClient;
    std::unique_ptr<StunClient> stunClient;
    
    std::atomic<bool> connected{false};
    std::atomic<bool> running{false};
    std::thread keepaliveThread;
    std::thread stunThread;
    
    StatusCallback statusCallback;
    std::string currentStatus = "disconnected";
    std::vector<NodeInfo> nodes;
    mutable std::mutex nodesMutex;
    
    void notifyStatus(const std::string& status, const std::string& message) {
        currentStatus = status;
        LOGI("ArkMeshCore", "Status: " + status + " - " + message);
        if (statusCallback) {
            statusCallback(status, message);
        }
    }
    
    void keepaliveLoop() {
        LOGI("ArkMeshCore", "Keepalive loop started");
        while (running) {
            if (connected && headscaleClient) {
                if (!headscaleClient->sendKeepalive()) {
                    notifyStatus("error", "Keepalive failed, reconnecting...");
                    connected = false;
                }
            }
            std::this_thread::sleep_for(std::chrono::seconds(30));
        }
        LOGI("ArkMeshCore", "Keepalive loop stopped");
    }
    
    void stunLoop() {
        LOGI("ArkMeshCore", "STUN loop started");
        while (running) {
            if (connected && stunClient) {
                auto result = stunClient->performBindingRequest();
                if (result.success) {
                    std::string endpoint = result.publicIP + ":" + 
                                          std::to_string(result.publicPort);
                    LOGI("ArkMeshCore", "STUN public endpoint: " + endpoint);
                    if (headscaleClient) {
                        headscaleClient->updateEndpoint(endpoint);
                    }
                }
            }
            std::this_thread::sleep_for(std::chrono::minutes(5));
        }
        LOGI("ArkMeshCore", "STUN loop stopped");
    }
    
    bool attemptConnection() {
        LOGI("ArkMeshCore", "Attempting connection to: " + config.serverUrl);
        
        // Initialize components
        if (!keyManager) {
            keyManager = std::make_unique<KeyManager>();
        }
        
        // Generate or load machine key
        LOGI("ArkMeshCore", "Loading machine key...");
        KeyPair machineKey = keyManager->loadMachineKey();
        if (machineKey.publicKey.empty()) {
            LOGI("ArkMeshCore", "Generating new machine key...");
            machineKey = keyManager->generateMachineKey();
            if (machineKey.publicKey.empty()) {
                LOGE("ArkMeshCore", "Failed to generate machine key - OpenSSL may not be available");
                notifyStatus("error", "Failed to generate machine key - ensure OpenSSL is linked");
                return false;
            }
            keyManager->saveMachineKey(machineKey);
        }
        
        std::string machinePublicKey = keyManager->getMachinePublicKeyBase64();

        // Load or generate the WireGuard node key. Tailscale's RegisterRequest
        // requires both MachineKey (device identity) and NodeKey (WireGuard key).
        KeyPair nodeKey = keyManager->loadNodeKey();
        if (nodeKey.publicKey.empty()) {
            LOGI("ArkMeshCore", "Generating new node key...");
            nodeKey = keyManager->generateNodeKey();
            if (nodeKey.publicKey.empty()) {
                LOGE("ArkMeshCore", "Failed to generate node key - OpenSSL may not be available");
                notifyStatus("error", "Failed to generate node key - ensure OpenSSL is linked");
                return false;
            }
            keyManager->saveNodeKey(nodeKey);
        }

        std::string nodePublicKey = keyManager->getNodePublicKeyBase64();

        // Initialize Headscale client
        LOGI("ArkMeshCore", "Initializing Headscale client...");
        if (!headscaleClient) {
            headscaleClient = std::make_unique<HeadscaleClient>();
        }
        
        headscaleClient->setMachineKey(machinePublicKey, "");
        headscaleClient->setNodeKey(nodePublicKey, "");
        headscaleClient->setStatusCallback([this](const std::string& status, 
                                                  const std::string& message) {
            notifyStatus(status, message);
        });
        
        if (!headscaleClient->initialize(config.serverUrl)) {
            notifyStatus("error", "Failed to initialize Headscale client");
            return false;
        }
        
        // Register with auth key
        if (!config.authKey.empty()) {
            LOGI("ArkMeshCore", "Registering with auth key...");
            if (!headscaleClient->registerWithAuthKey(config.authKey)) {
                notifyStatus("error", "Failed to register with auth key");
                return false;
            }
        } else {
            LOGE("ArkMeshCore", "Auth key is empty");
            notifyStatus("error", "Auth key is required");
            return false;
        }
        
        // Get network configuration
        LOGI("ArkMeshCore", "Getting network configuration...");
        auto networkConfig = headscaleClient->getNetworkConfig();

        // Sync node list exposed to the UI
        {
            std::lock_guard<std::mutex> lock(nodesMutex);
            nodes.clear();
            for (const auto& node : networkConfig.nodes) {
                NodeInfo info;
                info.publicKey = node.publicKey;
                info.meshIP = node.meshIP;
                info.hostName = node.hostName;
                info.os = node.os;
                info.online = node.online;
                info.lastSeen = node.lastSeen;
                nodes.push_back(info);
            }
        }

        // Initialize WireGuard tunnel
        LOGI("ArkMeshCore", "Initializing WireGuard tunnel...");
        if (!wireGuardTunnel) {
            wireGuardTunnel = std::make_unique<WireGuardTunnel>();
        }
        
        WireGuardConfig wgConfig;
        wgConfig.address = "100.64.0.1/32";
        wgConfig.listenPort = 0;
        wgConfig.mtu = 1420;
        
        // Add peers from network config
        for (const auto& node : networkConfig.nodes) {
            WireGuardPeer peer;
            peer.publicKey = node.publicKey;
            peer.endpoint = node.endpoint;
            peer.allowedIPs = node.allowedIPs;
            peer.persistentKeepalive = 25;
            wgConfig.peers.push_back(peer);
        }
        
        if (!wireGuardTunnel->initialize(wgConfig)) {
            notifyStatus("error", "Failed to initialize WireGuard tunnel");
            return false;
        }
        
        if (!wireGuardTunnel->start()) {
            notifyStatus("error", "Failed to start WireGuard tunnel");
            return false;
        }
        
        // Initialize STUN client
        LOGI("ArkMeshCore", "Initializing STUN client...");
        if (!stunClient) {
            stunClient = std::make_unique<StunClient>();
        }
        
        LOGI("ArkMeshCore", "Connection successful");
        return true;
    }
};

ArkMeshCore::ArkMeshCore() : pImpl(std::make_unique<Impl>()) {
    LOGI("ArkMeshCore", "ArkMeshCore created");
}
ArkMeshCore::~ArkMeshCore() = default;

bool ArkMeshCore::initialize(const ArkMeshConfig& config) {
    pImpl->config = config;
    pImpl->notifyStatus("initialized", "ArkMesh client initialized");
    return true;
}

bool ArkMeshCore::connect() {
    if (pImpl->running) {
        LOGW("ArkMeshCore", "Connection already in progress");
        return pImpl->connected;
    }
    
    LOGI("ArkMeshCore", "Starting connection...");
    pImpl->running = true;
    pImpl->connected = false;
    
    // Perform connection synchronously
    bool success = pImpl->attemptConnection();
    
    if (success) {
        pImpl->connected = true;
        pImpl->notifyStatus("connected", "Successfully connected to Headscale");
        // Start background threads
        pImpl->keepaliveThread = std::thread(&Impl::keepaliveLoop, pImpl.get());
        pImpl->stunThread = std::thread(&Impl::stunLoop, pImpl.get());
    } else {
        pImpl->running = false;
        pImpl->notifyStatus("error", "Failed to establish connection");
    }
    
    return success;
}

void ArkMeshCore::disconnect() {
    LOGI("ArkMeshCore", "Disconnecting...");
    pImpl->running = false;
    pImpl->connected = false;
    
    if (pImpl->keepaliveThread.joinable()) {
        pImpl->keepaliveThread.detach();
    }
    
    if (pImpl->stunThread.joinable()) {
        pImpl->stunThread.detach();
    }
    
    if (pImpl->wireGuardTunnel) {
        pImpl->wireGuardTunnel->stop();
    }
    
    pImpl->notifyStatus("disconnected", "ArkMesh client disconnected");
}

bool ArkMeshCore::isConnected() const {
    return pImpl->connected;
}

std::string ArkMeshCore::getStatus() const {
    return pImpl->currentStatus;
}

std::vector<NodeInfo> ArkMeshCore::getNodes() const {
    std::lock_guard<std::mutex> lock(pImpl->nodesMutex);
    return pImpl->nodes;
}

void ArkMeshCore::setStatusCallback(StatusCallback callback) {
    pImpl->statusCallback = callback;
}

std::string ArkMeshCore::generateMachineKey() {
    if (!pImpl->keyManager) {
        pImpl->keyManager = std::make_unique<KeyManager>();
    }
    
    auto keyPair = pImpl->keyManager->generateMachineKey();
    pImpl->keyManager->saveMachineKey(keyPair);
    
    return pImpl->keyManager->getMachinePublicKeyBase64();
}

bool ArkMeshCore::registerWithAuthKey(const std::string& authKey) {
    pImpl->config.authKey = authKey;
    
    if (!pImpl->keyManager) {
        pImpl->keyManager = std::make_unique<KeyManager>();
    }
    
    if (!pImpl->headscaleClient) {
        pImpl->headscaleClient = std::make_unique<HeadscaleClient>();
    }
    
    std::string machinePublicKey = pImpl->keyManager->getMachinePublicKeyBase64();
    pImpl->headscaleClient->setMachineKey(machinePublicKey, "");

    KeyPair nodeKey = pImpl->keyManager->loadNodeKey();
    if (nodeKey.publicKey.empty()) {
        nodeKey = pImpl->keyManager->generateNodeKey();
        if (nodeKey.publicKey.empty()) {
            pImpl->notifyStatus("error", "Failed to generate node key - ensure OpenSSL is linked");
            return false;
        }
        pImpl->keyManager->saveNodeKey(nodeKey);
    }
    std::string nodePublicKey = pImpl->keyManager->getNodePublicKeyBase64();
    pImpl->headscaleClient->setNodeKey(nodePublicKey, "");

    pImpl->headscaleClient->setStatusCallback(pImpl->statusCallback);
    pImpl->headscaleClient->initialize(pImpl->config.serverUrl);
    
    return pImpl->headscaleClient->registerWithAuthKey(authKey);
}

std::string ArkMeshCore::getInterfaceName() const {
    return "arkmesh0";
}

std::string ArkMeshCore::getMeshIP() const {
    return "100.64.0.1";
}

} // namespace arkmesh
