cmake_minimum_required(VERSION 3.10)
project(ArkMeshHarmony)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Find NAPI headers
find_path(NAPI_INCLUDE_DIR napi/native_api.h
    PATHS ${OHOS_NDK_ROOT}/native/sysroot/usr/include
    ${OHOS_NDK_ROOT}/sysroot/usr/include
    ${OHOS_SDK}/native/sysroot/usr/include
)

# Try to find OpenSSL in multiple locations:
# 1. Project-local prebuilt directories (both "third_party" and legacy "thirdparty")
# 2. OpenHarmony SDK native sysroot (usually does not ship OpenSSL)
# 3. No default system paths; a stub must never be silently used for HTTPS

# Determine architecture
if(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64")
    set(OHOS_ARCH "arm64-v8a")
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64")
    set(OHOS_ARCH "x86_64")
else()
    set(OHOS_ARCH "arm64-v8a")  # Default to arm64
endif()

message(STATUS "Building for architecture: ${OHOS_ARCH}")

option(ARKMESH_USE_OPENSSL_STUB
       "Use the non-functional OpenSSL stub when real OpenSSL is unavailable" OFF)

# Force a fresh search on every configure. Otherwise a previous failed search
# is cached as *-NOTFOUND and adding OpenSSL later will not be picked up.
unset(OPENSSL_INCLUDE_DIR CACHE)
unset(OPENSSL_SSL_LIBRARY CACHE)
unset(OPENSSL_CRYPTO_LIBRARY CACHE)

set(OPENSSL_PROJECT_ROOTS
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/openssl/${OHOS_ARCH}"
    "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/openssl/${OHOS_ARCH}"
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/openssl_install/${OHOS_ARCH}"
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/openssl_install"
)

set(OPENSSL_SEARCH_PATHS
    ${OPENSSL_PROJECT_ROOTS}
    ${OHOS_NDK_ROOT}/native/sysroot/usr
    ${OHOS_SDK}/native/sysroot/usr
)

set(OPENSSL_INCLUDE_DIR "")
set(OPENSSL_SSL_LIBRARY "")
set(OPENSSL_CRYPTO_LIBRARY "")

foreach(root ${OPENSSL_PROJECT_ROOTS})
    if(NOT OPENSSL_INCLUDE_DIR AND EXISTS "${root}/include/openssl/ssl.h")
        set(OPENSSL_INCLUDE_DIR "${root}/include")
    endif()

    foreach(libdir lib lib64)
        if(NOT OPENSSL_SSL_LIBRARY AND EXISTS "${root}/${libdir}/libssl.a")
            set(OPENSSL_SSL_LIBRARY "${root}/${libdir}/libssl.a")
        endif()
        if(NOT OPENSSL_CRYPTO_LIBRARY AND EXISTS "${root}/${libdir}/libcrypto.a")
            set(OPENSSL_CRYPTO_LIBRARY "${root}/${libdir}/libcrypto.a")
        endif()
    endforeach()
endforeach()

if(OPENSSL_INCLUDE_DIR AND OPENSSL_SSL_LIBRARY AND OPENSSL_CRYPTO_LIBRARY)
    message(STATUS "✅ Found OpenSSL:")
    message(STATUS "   Headers: ${OPENSSL_INCLUDE_DIR}")
    message(STATUS "   SSL library: ${OPENSSL_SSL_LIBRARY}")
    message(STATUS "   Crypto library: ${OPENSSL_CRYPTO_LIBRARY}")
    set(HAS_OPENSSL ON)
else()
    if(ARKMESH_USE_OPENSSL_STUB)
        message(WARNING "Building with OpenSSL stub. HTTPS/TLS and crypto will fail at runtime.")
        set(HAS_OPENSSL OFF)
    else()
        message(FATAL_ERROR
            "OpenSSL not found for ${OHOS_ARCH}.\n"
            "The app cannot connect to a Headscale HTTPS server without OpenSSL.\n"
            "Build OpenSSL according to docs/OPENSSL_BUILD_GUIDE.md and place the "
            "arm64-v8a/x86_64 outputs under:\n"
            "  ${CMAKE_CURRENT_SOURCE_DIR}/third_party/openssl/${OHOS_ARCH}\n"
            "or configure with -DARKMESH_USE_OPENSSL_STUB=ON for a non-functional build.")
    endif()
endif()

# Source files
set(ARKMESH_SOURCES
    napi_init.cpp
    src/bridge/go_probe.cpp
    src/key/key_manager.cpp
    src/noise/noise_protocol.cpp
    src/wireguard/wireguard_tunnel.cpp
    src/api/headscale_client.cpp
    src/stun/stun_client.cpp
    src/utils/crypto_utils.cpp
    src/utils/portable_crypto.cpp
    arkmesh_core.cpp
)

if(NOT HAS_OPENSSL)
    list(APPEND ARKMESH_SOURCES third_party/openssl_stub/openssl_stub.cpp)
endif()

# Add native library
add_library(ArkMesh SHARED ${ARKMESH_SOURCES})

# Include directories
target_include_directories(ArkMesh PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${NAPI_INCLUDE_DIR}
)

if(HAS_OPENSSL)
    target_include_directories(ArkMesh PUBLIC ${OPENSSL_INCLUDE_DIR})
    target_compile_definitions(ArkMesh PUBLIC HAS_OPENSSL)
else()
    target_include_directories(ArkMesh PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/third_party/openssl_stub)
endif()

# Link libraries
if(HAS_OPENSSL)
    target_link_libraries(ArkMesh
        c++_shared
        z
        ace_napi.z
        hilog_ndk.z
        ${OPENSSL_SSL_LIBRARY}     # Use found OpenSSL SSL library
        ${OPENSSL_CRYPTO_LIBRARY}  # Use found OpenSSL Crypto library
    )
    message(STATUS "✅ OpenSSL libraries linked successfully")
else()
    target_link_libraries(ArkMesh
        c++_shared
        z
        ace_napi.z
        hilog_ndk.z
    )
endif()

# === Go 运行时桥接库 (spike) ===
# 仅 arm64 提供预编译产物。
# 用独立 custom target 而不是 POST_BUILD: POST_BUILD 只在 ArkMesh 重新链接时才跑,
# 而 .so 是外部构建产物, 换新 .so 后 ArkMesh 不会重链 -> HAP 里还是旧库。
function(bundle_go_lib LIB_PATH)
    if(NOT EXISTS "${LIB_PATH}")
        message(WARNING "Go library missing: ${LIB_PATH}")
        return()
    endif()
    get_filename_component(LIB_NAME "${LIB_PATH}" NAME)
    add_custom_target(bundle_${LIB_NAME} ALL
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${LIB_PATH}"
            "$<TARGET_FILE_DIR:ArkMesh>/${LIB_NAME}"
        DEPENDS "${LIB_PATH}"
        COMMENT "Bundle ${LIB_NAME} into native libs output")
    add_dependencies(bundle_${LIB_NAME} ArkMesh)
    message(STATUS "✅ Go library will be bundled: ${LIB_PATH}")
endfunction()

if(OHOS_ARCH STREQUAL "arm64-v8a")
    bundle_go_lib("${CMAKE_CURRENT_SOURCE_DIR}/third_party/go/${OHOS_ARCH}/libgohello.so")
    # Route B: ArkMesh 的 Go 核心 (tailscale: control/wgengine/netstack/DERP/MagicDNS)
    bundle_go_lib("${CMAKE_CURRENT_SOURCE_DIR}/third_party/go/${OHOS_ARCH}/libarkmesh_core.so")
endif()
