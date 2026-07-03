cmake_minimum_required(VERSION 3.22.1)
project(wivrn-neo2)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Force optimization regardless of build type
set(CMAKE_C_FLAGS_DEBUG   "-O2 -DNDEBUG")
set(CMAKE_CXX_FLAGS_DEBUG "-O2 -DNDEBUG")

# ---- Dependencies ----

include(FetchContent)

# spdlog
FetchContent_Declare(
    spdlog
    GIT_REPOSITORY https://github.com/gabime/spdlog.git
    GIT_TAG v1.13.0
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(spdlog)

# Boost (pfr headers only)
set(BOOST_INCLUDE_LIBRARIES pfr)
FetchContent_Declare(
    Boost
    URL https://github.com/boostorg/boost/releases/download/boost-1.84.0/boost-1.84.0.tar.xz
    URL_HASH SHA256=2647bc7982a46996e07634c79e2f873a1596f7235802397360c72d2f0e1c8334
)
FetchContent_MakeAvailable(Boost)

# OpenXR loader (for XrPosef, XrFovf, XrTime types only)
FetchContent_Declare(
    openxr_loader
    GIT_REPOSITORY https://github.com/KhronosGroup/OpenXR-SDK.git
    GIT_TAG release-1.0.31
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(openxr_loader)

# ---- OpenSSL for Android ----
# Cross-compile OpenSSL using ExternalProject
set(OPENSSL_SOURCE_DIR ${CMAKE_CURRENT_BINARY_DIR}/openssl-src)
set(OPENSSL_INSTALL_DIR ${CMAKE_CURRENT_BINARY_DIR}/openssl-install)

ExternalProject_Add(openssl_external
    URL https://www.openssl.org/source/openssl-3.2.1.tar.gz
    URL_HASH SHA256=83c7329fc52d895871dbca6ac6c2f97c392a0d5e0aae6b5b803c4c63c32c3d0e
    SOURCE_DIR ${OPENSSL_SOURCE_DIR}
    INSTALL_DIR ${OPENSSL_INSTALL_DIR}
    CONFIGURE_COMMAND
        ${ANDROID_NDK}/toolchains/llvm/prebuilt/${ANDROID_HOST_TAG}/bin/llvm-config
        <SOURCE_DIR>/Configure
        android-${ANDROID_ABI}
        --prefix=<INSTALL_DIR>
        --openssldir=<INSTALL_DIR>/ssl
        no-shared
        no-tests
    BUILD_COMMAND make -j${CMAKE_BUILD_PARALLEL_LEVEL}
    INSTALL_COMMAND make install_sw
    BUILD_IN_SOURCE TRUE
)

# Create imported target for OpenSSL
add_library(openssl_ssl STATIC IMPORTED)
add_dependencies(openssl_ssl openssl_external)
set_target_properties(openssl_ssl PROPERTIES
    IMPORTED_LOCATION ${OPENSSL_INSTALL_DIR}/lib/libssl.a
)
add_library(openssl_crypto STATIC IMPORTED)
add_dependencies(openssl_crypto openssl_external)
set_target_properties(openssl_crypto PROPERTIES
    IMPORTED_LOCATION ${OPENSSL_INSTALL_DIR}/lib/libcrypto.a
)
target_include_directories(openssl_ssl INTERFACE ${OPENSSL_INSTALL_DIR}/include)
target_include_directories(openssl_crypto INTERFACE ${OPENSSL_INSTALL_DIR}/include)

# ---- Pico SDK (prebuilt .so from ALVR repo) ----
add_library(Pvr_UnitySDK SHARED IMPORTED)
set_target_properties(Pvr_UnitySDK PROPERTIES
    IMPORTED_LOCATION ${CMAKE_CURRENT_SOURCE_DIR}/jniLibs/${ANDROID_ABI}/libPvr_UnitySDK.so
)

# ---- wivrn-common (protocol library) ----
add_library(wivrn-common STATIC
    common/crypto.cpp
    common/secrets.cpp
    common/smp.cpp
    common/wivrn_sockets.cpp
    common/utils/strings.cpp
    common/utils/xdg_base_directory.cpp
    common/vk/allocation.cpp
    common/vk/error_category.cpp
    common/vk/vk_allocator.cpp
)

target_include_directories(wivrn-common PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/common
    ${CMAKE_CURRENT_SOURCE_DIR}/common/utils
)

target_link_libraries(wivrn-common PUBLIC
    openssl_ssl
    openssl_crypto
    Boost::pfr
    spdlog::spdlog
)

# Generate wivrn_config.h
configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/common/wivrn_config.h.in
    ${CMAKE_CURRENT_BINARY_DIR}/wivrn_config.h
    @ONLY
)
target_include_directories(wivrn-common PUBLIC ${CMAKE_CURRENT_BINARY_DIR})

# ---- Main client module ----
add_library(wivrn-neo2 SHARED
    neo2_session.cpp
    neo2_video_decoder.cpp
    neo2_audio.cpp
    neo2_blit.cpp
    neo2_client.cpp
    neo2_jni.cpp
)

target_link_libraries(wivrn-neo2
    wivrn-common
    openxr_loader
    Pvr_UnitySDK
    spdlog::spdlog
    Boost::pfr

    android
    mediandk
    aaudio
    log
    GLESv2
    EGL
)

# Copy Pvr_UnitySDK.so to output
add_custom_command(TARGET wivrn-neo2 POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
    $<TARGET_FILE:Pvr_UnitySDK>
    $<TARGET_FILE_DIR:wivrn-neo2>
)
