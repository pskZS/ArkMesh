#include "napi/native_api.h"
#include "arkmesh_core.h"
#include "bridge/go_probe.h"
#include "utils/logger.h"
#include <memory>
#include <thread>
#include <string>
#include <vector>
#include <sstream>

// Global ArkMeshCore instance
static std::unique_ptr<arkmesh::ArkMeshCore> g_arkmeshCore;

// Helper function to convert string to napi_value
static napi_value StringToNapi(napi_env env, const std::string& str) {
    napi_value result;
    napi_create_string_utf8(env, str.c_str(), str.length(), &result);
    return result;
}

// Helper function to get string from napi_value
static std::string NapiToString(napi_env env, napi_value value) {
    size_t len = 0;
    napi_get_value_string_utf8(env, value, nullptr, 0, &len);
    std::string str(len, 0);
    napi_get_value_string_utf8(env, value, &str[0], len + 1, &len);
    return str;
}

// NAPI: Initialize ArkMesh client
static napi_value NapiInitialize(napi_env env, napi_callback_info info) {
    napi_value result = nullptr;
    
    if (!g_arkmeshCore) {
        g_arkmeshCore = std::make_unique<arkmesh::ArkMeshCore>();
        LOGI("NAPI", "ArkMeshCore initialized via NAPI");
    }
    
    napi_create_int32(env, 0, &result);
    return result;
}

// NAPI: Connect to Headscale server
static napi_value NapiConnect(napi_env env, napi_callback_info info) {
    napi_value result = nullptr;
    
    if (g_arkmeshCore) {
        bool success = g_arkmeshCore->connect();
        if (success) {
            LOGI("NAPI", "Connect called successfully");
        } else {
            LOGE("NAPI", "Connect failed");
        }
        napi_create_int32(env, success ? 0 : -1, &result);
    } else {
        LOGE("NAPI", "Connect called but ArkMeshCore not initialized");
        napi_create_int32(env, -1, &result);
    }
    
    return result;
}

// NAPI: Disconnect from server
static napi_value NapiDisconnect(napi_env env, napi_callback_info info) {
    napi_value result = nullptr;
    
    if (g_arkmeshCore) {
        g_arkmeshCore->disconnect();
        LOGI("NAPI", "Disconnect called successfully");
        napi_create_int32(env, 0, &result);
    } else {
        LOGE("NAPI", "Disconnect called but ArkMeshCore not initialized");
        napi_create_int32(env, -1, &result);
    }
    
    return result;
}

// NAPI: Get connection status
static napi_value NapiGetStatus(napi_env env, napi_callback_info info) {
    napi_value result = nullptr;
    
    if (g_arkmeshCore) {
        std::string status = g_arkmeshCore->getStatus();
        napi_create_string_utf8(env, status.c_str(), NAPI_AUTO_LENGTH, &result);
    } else {
        napi_create_string_utf8(env, "not_initialized", NAPI_AUTO_LENGTH, &result);
    }
    
    return result;
}

// NAPI: Check if connected
static napi_value NapiIsConnected(napi_env env, napi_callback_info info) {
    napi_value result = nullptr;
    
    if (g_arkmeshCore) {
        bool connected = g_arkmeshCore->isConnected();
        napi_get_boolean(env, connected, &result);
    } else {
        napi_get_boolean(env, false, &result);
    }
    
    return result;
}

// NAPI: Get ArkMesh IP
static napi_value NapiGetTailscaleIP(napi_env env, napi_callback_info info) {
    napi_value result = nullptr;
    
    if (g_arkmeshCore) {
        std::string ip = g_arkmeshCore->getMeshIP();
        napi_create_string_utf8(env, ip.c_str(), NAPI_AUTO_LENGTH, &result);
    } else {
        napi_create_string_utf8(env, "", NAPI_AUTO_LENGTH, &result);
    }
    
    return result;
}

// NAPI: Generate machine key
static napi_value NapiGenerateMachineKey(napi_env env, napi_callback_info info) {
    napi_value result = nullptr;
    
    if (g_arkmeshCore) {
        std::string key = g_arkmeshCore->generateMachineKey();
        napi_create_string_utf8(env, key.c_str(), NAPI_AUTO_LENGTH, &result);
    } else {
        napi_create_string_utf8(env, "", NAPI_AUTO_LENGTH, &result);
    }
    
    return result;
}

// NAPI: Register with auth key
static napi_value NapiRegisterWithAuthKey(napi_env env, napi_callback_info info) {
    napi_value result = nullptr;
    size_t argc = 1;
    napi_value args[1] = {nullptr};
    
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    
    if (g_arkmeshCore && argc > 0) {
        std::string authKey = NapiToString(env, args[0]);
        bool success = g_arkmeshCore->registerWithAuthKey(authKey);
        napi_get_boolean(env, success, &result);
    } else {
        napi_get_boolean(env, false, &result);
    }
    
    return result;
}

// NAPI: Get node list as JSON array string
static napi_value NapiGetNodes(napi_env env, napi_callback_info info) {
    napi_value result = nullptr;

    if (!g_arkmeshCore) {
        napi_create_string_utf8(env, "[]", NAPI_AUTO_LENGTH, &result);
        return result;
    }

    std::vector<arkmesh::NodeInfo> nodes = g_arkmeshCore->getNodes();
    std::ostringstream json;
    json << "[";
    for (size_t i = 0; i < nodes.size(); ++i) {
        if (i > 0) {
            json << ",";
        }
        // Simple JSON escaping for the fields we expose
        auto escape = [](const std::string& s) {
            std::ostringstream out;
            for (char c : s) {
                switch (c) {
                    case '"': out << "\\\""; break;
                    case '\\': out << "\\\\"; break;
                    case '\n': out << "\\n"; break;
                    case '\r': out << "\\r"; break;
                    case '\t': out << "\\t"; break;
                    default: out << c; break;
                }
            }
            return out.str();
        };

        json << "{\"publicKey\":\"" << escape(nodes[i].publicKey) << "\","
             << "\"meshIP\":\"" << escape(nodes[i].meshIP) << "\","
             << "\"hostName\":\"" << escape(nodes[i].hostName) << "\","
             << "\"os\":\"" << escape(nodes[i].os) << "\","
             << "\"online\":" << (nodes[i].online ? "true" : "false") << ","
             << "\"lastSeen\":" << nodes[i].lastSeen << "}";
    }
    json << "]";

    napi_create_string_utf8(env, json.str().c_str(), NAPI_AUTO_LENGTH, &result);
    return result;
}

// NAPI: Initialize with config
static napi_value NapiInitializeWithConfig(napi_env env, napi_callback_info info) {
    napi_value result = nullptr;
    size_t argc = 1;
    napi_value args[1] = {nullptr};
    
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    
    if (g_arkmeshCore && argc > 0) {
        napi_value serverUrlValue = nullptr;
        napi_value authKeyValue = nullptr;
        napi_value userNameValue = nullptr;
        napi_value machineNameValue = nullptr;
        
        napi_get_named_property(env, args[0], "serverUrl", &serverUrlValue);
        napi_get_named_property(env, args[0], "authKey", &authKeyValue);
        napi_get_named_property(env, args[0], "userName", &userNameValue);
        napi_get_named_property(env, args[0], "machineName", &machineNameValue);
        
        arkmesh::ArkMeshConfig config;
        config.serverUrl = NapiToString(env, serverUrlValue);
        config.authKey = NapiToString(env, authKeyValue);
        config.userName = NapiToString(env, userNameValue);
        config.machineName = NapiToString(env, machineNameValue);
        
        LOGI("NAPI", "Initializing with server: " + config.serverUrl);
        bool success = g_arkmeshCore->initialize(config);
        if (success) {
            LOGI("NAPI", "Initialization successful");
        } else {
            LOGE("NAPI", "Initialization failed");
        }
        napi_get_boolean(env, success, &result);
    } else {
        LOGE("NAPI", "InitializeWithConfig called but ArkMeshCore not initialized");
        napi_get_boolean(env, false, &result);
    }
    
    return result;
}

// Module exports
static napi_value ExportModule(napi_env env, napi_value exports) {
    napi_property_descriptor desc[] = {
        { "initialize", nullptr, NapiInitialize, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "connect", nullptr, NapiConnect, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "disconnect", nullptr, NapiDisconnect, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "getStatus", nullptr, NapiGetStatus, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "isConnected", nullptr, NapiIsConnected, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "getMeshIP", nullptr, NapiGetTailscaleIP, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "getNodes", nullptr, NapiGetNodes, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "generateMachineKey", nullptr, NapiGenerateMachineKey, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "registerWithAuthKey", nullptr, NapiRegisterWithAuthKey, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "initializeWithConfig", nullptr, NapiInitializeWithConfig, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "goProbe", nullptr, gobridge::Probe, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "goStartTicker", nullptr, gobridge::StartTicker, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "goTicks", nullptr, gobridge::Ticks, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "goTlsProbe", nullptr, gobridge::TlsProbe, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "goUdpStart", nullptr, gobridge::UdpStart, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "goUdpStats", nullptr, gobridge::UdpStats, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "goUdpSendTo", nullptr, gobridge::UdpSendTo, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "goDnsLookup", nullptr, gobridge::DnsLookup, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "goTunStart", nullptr, gobridge::TunStart, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "goTunWrite", nullptr, gobridge::TunWrite, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "goTunStats", nullptr, gobridge::TunStats, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "goTunCapture", nullptr, gobridge::TunCapture, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "goTunBuildUdp", nullptr, gobridge::TunBuildUdp, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "coreSelfTest", nullptr, gobridge::CoreSelfTest, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "coreNetCheck", nullptr, gobridge::CoreNetCheck, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "coreIfProbe", nullptr, gobridge::CoreIfProbe, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "coreControlUp", nullptr, gobridge::CoreControlUp, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "coreControlStatus", nullptr, gobridge::CoreControlStatus, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "corePingPeers", nullptr, gobridge::CorePingPeers, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "coreDnsProbe", nullptr, gobridge::CoreDnsProbe, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "coreControlDown", nullptr, gobridge::CoreControlDown, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "coreAttachTun", nullptr, gobridge::CoreAttachTun, nullptr, nullptr, nullptr, napi_default, nullptr },
    };
    
    napi_define_properties(env, exports, sizeof(desc) / sizeof(desc[0]), desc);
    LOGI("NAPI", "ArkMesh NAPI module exported with " + std::to_string(sizeof(desc) / sizeof(desc[0])) + " functions");
    return exports;
}

// Register NAPI module using NAPI_MODULE macro
static napi_module arkMeshModule = {
    .nm_version = 1,
    .nm_flags = 0,
    .nm_filename = nullptr,
    .nm_register_func = ExportModule,
    .nm_modname = "ArkMesh",
    .nm_priv = nullptr,
    .reserved = {nullptr, nullptr, nullptr, nullptr}
};

extern "C" __attribute__((constructor)) void RegisterArkMeshModule() {
    napi_module_register(&arkMeshModule);
}
