cmake_minimum_required(VERSION 3.22)
project(pico_oxr)

set(CMAKE_CXX_STANDARD 20)
add_compile_options(-fconstexpr-steps=268435456)

find_library(log-lib log)
find_library(egl-lib EGL)
find_library(gles-lib GLESv3)
find_library(android-lib android)
find_library(jnigraphics-lib jnigraphics)
find_library(media-lib mediandk)
find_library(aaudio-lib aaudio)

add_library(openxr_loader SHARED IMPORTED)
set_target_properties(openxr_loader PROPERTIES
    IMPORTED_LOCATION ${CMAKE_CURRENT_SOURCE_DIR}/../jniLibs/${ANDROID_ABI}/libopenxr_loader.so
)

add_library(native_app_glue OBJECT
    ${ANDROID_NDK}/sources/android/native_app_glue/android_native_app_glue.c
)
target_include_directories(native_app_glue PUBLIC
    ${ANDROID_NDK}/sources/android/native_app_glue
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
set(NDK_CC ${NDK_TOOLCHAIN}/bin/aarch64-linux-android${ANDROID_API_LEVEL}-clang)
include(ExternalProject)
ExternalProject_Add(openssl_ext
    URL https://github.com/openssl/openssl/releases/download/openssl-3.4.1/openssl-3.4.1.tar.gz
    URL_HASH SHA256=002a2d6b30b58bf4bea46c43bdd96365aaf8daa6c428782aa4feee06da197df3
    CONFIGURE_COMMAND
        ${CMAKE_COMMAND} -E env
        ANDROID_NDK_ROOT=${ANDROID_NDK}
        ANDROID_NDK_HOME=${ANDROID_NDK}
        PATH=${NDK_TOOLCHAIN}/bin:$ENV{PATH}
        ./Configure linux-aarch64 no-shared no-tests no-asm
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
target_compile_options(wivrn-common PUBLIC -include ${CMAKE_CURRENT_SOURCE_DIR}/openxr_ext_stubs.h)
target_link_libraries(wivrn-common PUBLIC OpenSSL::Crypto)

# --- Streaming source files ---
set(STREAMING_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/streaming/streaming_client.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/streaming/oxr_blit.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/streaming/oxr_tracking.cpp
    ${PICO_NATIVE_DIR}/src/net/wivrn_client_pico.cpp
    ${PICO_NATIVE_DIR}/src/video/pico_decoder.cpp
    ${PICO_NATIVE_DIR}/src/audio/pico_audio.cpp
    ${PICO_NATIVE_DIR}/src/render/pico_blit.cpp
    ${PICO_NATIVE_DIR}/src/tracking/pico_tracking.cpp
)

add_library(pico_oxr SHARED
    main.cpp
    lobby.cpp
    ${STREAMING_SOURCES}
    $<TARGET_OBJECTS:native_app_glue>
)

target_include_directories(pico_oxr PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/openxr
    ${CMAKE_CURRENT_SOURCE_DIR}/streaming
    ${ANDROID_NDK}/sources/android/native_app_glue
    ${PICO_NATIVE_DIR}/common
    ${PICO_NATIVE_DIR}/src/core
    ${PICO_NATIVE_DIR}/src/net
    ${PICO_NATIVE_DIR}/src/video
    ${PICO_NATIVE_DIR}/src/audio
    ${PICO_NATIVE_DIR}/src/render
    ${PICO_NATIVE_DIR}/src/tracking
    ${CMAKE_BINARY_DIR}
)

target_include_directories(pico_oxr SYSTEM PRIVATE ${SPDLOG_INCLUDE_DIR} ${BOOST_INCLUDE_DIR})

target_compile_definitions(pico_oxr PRIVATE SPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_DEBUG SPDLOG_FMT_EXTERNAL FMT_HEADER_ONLY)
target_compile_options(pico_oxr PRIVATE
    "-include${CMAKE_CURRENT_SOURCE_DIR}/openxr_ext_stubs.h"
    "-include${CMAKE_CURRENT_SOURCE_DIR}/pvr_stubs.h"
)

target_link_libraries(pico_oxr
    ${log-lib}
    ${egl-lib}
    ${gles-lib}
    ${android-lib}
    ${jnigraphics-lib}
    ${media-lib}
    ${aaudio-lib}
    openxr_loader
    wivrn-common
)
