cmake_minimum_required(VERSION 3.22)
project(wivrn_pvr)

set(CMAKE_CXX_STANDARD 20)
add_compile_options(-fconstexpr-steps=268435456)

find_library(log-lib log)
find_library(egl-lib EGL)
find_library(gles-lib GLESv3)
find_library(android-lib android)
find_library(jnigraphics-lib jnigraphics)
find_library(media-lib mediandk)
find_library(aaudio-lib aaudio)

# Pvr Unity SDK + tracking module (from ALVR reference, 32-bit armeabi-v7a)
add_library(Pvr_UnitySDK SHARED IMPORTED)
set_target_properties(Pvr_UnitySDK PROPERTIES
    IMPORTED_LOCATION ${CMAKE_CURRENT_SOURCE_DIR}/../jniLibs/${ANDROID_ABI}/libPvr_UnitySDK.so
)
add_library(tracking_module SHARED IMPORTED)
set_target_properties(tracking_module PROPERTIES
    IMPORTED_LOCATION ${CMAKE_CURRENT_SOURCE_DIR}/../jniLibs/${ANDROID_ABI}/libtracking_module.so
)

# spdlog and boost::pfr are header-only - vendored copies in third_party/
set(THIRD_PARTY_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
set(SPDLOG_INCLUDE_DIR ${THIRD_PARTY_DIR})
set(BOOST_INCLUDE_DIR ${THIRD_PARTY_DIR})

# OpenSSL for Android
set(OPENSSL_INSTALL_DIR ${CMAKE_BINARY_DIR}/openssl-install)
set(OPENSSL_INCLUDE_DIR ${OPENSSL_INSTALL_DIR}/include)
set(OPENSSL_CRYPTO_LIBRARY ${OPENSSL_INSTALL_DIR}/lib/libcrypto.a)

string(REGEX REPLACE "android-([0-9]+)" "\\1" ANDROID_API_LEVEL ${ANDROID_PLATFORM})
set(NDK_TOOLCHAIN ${ANDROID_NDK}/toolchains/llvm/prebuilt/${ANDROID_HOST_TAG})

if(ANDROID_ABI STREQUAL "armeabi-v7a")
    set(OPENSSL_TARGET "linux-armv4")
    set(NDK_CC ${NDK_TOOLCHAIN}/bin/armv7a-linux-androideabi${ANDROID_API_LEVEL}-clang)
elseif(ANDROID_ABI STREQUAL "arm64-v8a")
    set(OPENSSL_TARGET "linux-aarch64")
    set(NDK_CC ${NDK_TOOLCHAIN}/bin/aarch64-linux-android${ANDROID_API_LEVEL}-clang)
else()
    message(FATAL_ERROR "Unsupported ANDROID_ABI: ${ANDROID_ABI}")
endif()

include(ExternalProject)
ExternalProject_Add(openssl_ext
    URL https://github.com/openssl/openssl/releases/download/openssl-3.4.1/openssl-3.4.1.tar.gz
    URL_HASH SHA256=002a2d6b30b58bf4bea46c43bdd96365aaf8daa6c428782aa4feee06da197df3
    CONFIGURE_COMMAND
        ${CMAKE_COMMAND} -E env
        ANDROID_NDK_ROOT=${ANDROID_NDK}
        ANDROID_NDK_HOME=${ANDROID_NDK}
        PATH=${NDK_TOOLCHAIN}/bin:$ENV{PATH}
        ./Configure ${OPENSSL_TARGET} no-shared no-tests no-asm
        --prefix=${OPENSSL_INSTALL_DIR}
        -D__ANDROID_API__=${ANDROID_API_LEVEL}
    BUILD_COMMAND make -j${CMAKE_BUILD_PARALLEL_LEVEL}
        CROSS_COMPILE=
        CC=${NDK_CC}
        AR=${NDK_TOOLCHAIN}/bin/llvm-ar
        RANLIB=${NDK_TOOLCHAIN}/bin/llvm-ranlib
    INSTALL_COMMAND make install_sw
    BUILD_IN_SOURCE true
    BUILD_BYPRODUCTS ${OPENSSL_CRYPTO_LIBRARY}
)
file(MAKE_DIRECTORY ${OPENSSL_INCLUDE_DIR})
add_library(OpenSSL::Crypto STATIC IMPORTED)
set_target_properties(OpenSSL::Crypto PROPERTIES
    IMPORTED_LOCATION ${OPENSSL_CRYPTO_LIBRARY}
    INTERFACE_INCLUDE_DIRECTORIES ${OPENSSL_INCLUDE_DIR}
)
add_dependencies(OpenSSL::Crypto openssl_ext)

# Vulkan headers (needed by wivrn-common) - use system headers
set(VULKAN_INCLUDE_DIR /usr/include)

# --- wivrn-common (vendored from pico_native) ---
set(PICO_NATIVE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../../../../pico_native)

configure_file(${PICO_NATIVE_DIR}/common/wivrn_config.h.in ${CMAKE_BINARY_DIR}/wivrn_config.h)

file(WRITE ${CMAKE_BINARY_DIR}/version.cpp
    "#include \"version.h\"\n"
    "namespace wivrn {\n"
    "const char git_commit[] = \"unknown\";\n"
    "const bool is_tag = false;\n"
    "const char * display_version() { return \"standalone\"; }\n"
    "}\n"
)

add_library(wivrn-common STATIC EXCLUDE_FROM_ALL
    ${PICO_NATIVE_DIR}/common/crypto.cpp
    ${PICO_NATIVE_DIR}/common/smp.cpp
    ${PICO_NATIVE_DIR}/common/secrets.cpp
    ${PICO_NATIVE_DIR}/common/wivrn_sockets.cpp
    ${CMAKE_BINARY_DIR}/version.cpp
)
target_include_directories(wivrn-common PUBLIC
    ${PICO_NATIVE_DIR}/common
    ${CMAKE_BINARY_DIR}
    ${PICO_NATIVE_DIR}/external
    ${CMAKE_CURRENT_SOURCE_DIR}
)
target_include_directories(wivrn-common SYSTEM PUBLIC ${VULKAN_INCLUDE_DIR} ${SPDLOG_INCLUDE_DIR} ${BOOST_INCLUDE_DIR})
target_compile_definitions(wivrn-common PUBLIC VMA_VULKAN_VERSION=1001000 VULKAN_HPP_NO_STRUCT_CONSTRUCTORS SPDLOG_FMT_EXTERNAL FMT_HEADER_ONLY)
target_link_libraries(wivrn-common PUBLIC OpenSSL::Crypto)

# --- Native PVR client (pico_native core) ---
set(PVR_CORE_SOURCES
    ${PICO_NATIVE_DIR}/src/core/main.cpp
    ${PICO_NATIVE_DIR}/src/core/pico_client.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/core/pico_jni.cpp
    ${PICO_NATIVE_DIR}/src/core/pico_render.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/core/pico_render_thread.cpp
    ${PICO_NATIVE_DIR}/src/net/wivrn_client_pico.cpp
    ${PICO_NATIVE_DIR}/src/video/pico_decoder.cpp
    ${PICO_NATIVE_DIR}/src/audio/pico_audio.cpp
    ${PICO_NATIVE_DIR}/src/render/pico_blit.cpp
    ${PICO_NATIVE_DIR}/src/render/pico_lobby.cpp
    ${PICO_NATIVE_DIR}/src/tracking/pico_tracking.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/streaming/eye_tracking.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/globals.cpp
)

add_library(wivrn_pvr SHARED
    ${PVR_CORE_SOURCES}
)

target_include_directories(wivrn_pvr PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/core
    ${CMAKE_CURRENT_SOURCE_DIR}/openxr
    ${CMAKE_CURRENT_SOURCE_DIR}/streaming
    ${PICO_NATIVE_DIR}/common
    ${PICO_NATIVE_DIR}/src/core
    ${PICO_NATIVE_DIR}/src/net
    ${PICO_NATIVE_DIR}/src/video
    ${PICO_NATIVE_DIR}/src/audio
    ${PICO_NATIVE_DIR}/src/render
    ${PICO_NATIVE_DIR}/src/tracking
    ${CMAKE_BINARY_DIR}
)

target_include_directories(wivrn_pvr SYSTEM PRIVATE ${SPDLOG_INCLUDE_DIR} ${BOOST_INCLUDE_DIR})

target_compile_definitions(wivrn_pvr PRIVATE SPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_DEBUG SPDLOG_FMT_EXTERNAL FMT_HEADER_ONLY)
target_compile_options(wivrn_pvr PRIVATE
    "-include${CMAKE_CURRENT_SOURCE_DIR}/openxr_ext_stubs.h"
    "-include${CMAKE_CURRENT_SOURCE_DIR}/pvr_stubs.h"
)

target_link_libraries(wivrn_pvr
    ${log-lib}
    ${egl-lib}
    ${gles-lib}
    ${android-lib}
    ${jnigraphics-lib}
    ${media-lib}
    ${aaudio-lib}
    wivrn-common
    Pvr_UnitySDK
    tracking_module
)
