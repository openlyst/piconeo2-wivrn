# Pico Native client - standalone build for Pico Neo 2 using PvrSDK-Native
# This CMakeLists.txt is self-contained and does not depend on the WiVRn root build.

cmake_minimum_required(VERSION 3.28)
project(WiVRnNeo2 VERSION 1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

include(FetchContent)

# --- Dependencies ---

# spdlog
FetchContent_Declare(spdlog EXCLUDE_FROM_ALL SYSTEM URL https://github.com/gabime/spdlog/archive/refs/tags/v1.16.0.tar.gz
    URL_HASH SHA256=8741753e488a78dd0d0024c980e1fb5b5c85888447e309d9cb9d949bdb52aa3e)

# OpenXR loader (headers + loader for XrPosef, XrFovf, XrTime)
set(DYNAMIC_LOADER ON)
FetchContent_Declare(openxr_loader EXCLUDE_FROM_ALL SYSTEM URL https://github.com/KhronosGroup/OpenXR-SDK/archive/refs/tags/release-1.1.58.tar.gz
    URL_HASH SHA256=815b933219c398314366c289221526428418d799205a73a9640c97ca4242602b)

# Boost (pfr headers only - we don't need full boost)
FetchContent_Declare(boost EXCLUDE_FROM_ALL SYSTEM URL https://github.com/boostorg/boost/releases/download/boost-1.89.0/boost-1.89.0-cmake.tar.xz
    URL_HASH SHA256=67acec02d0d118b5de9eb441f5fb707b3a1cdd884be00ca24b9a73c995511f74)

FetchContent_MakeAvailable(spdlog openxr_loader)

# Boost - just need pfr headers
set(BOOST_INCLUDE_LIBRARIES pfr)
FetchContent_MakeAvailable(boost)

# OpenSSL for Android - cross-compile from source using NDK
if(ANDROID)
    set(OPENSSL_INSTALL_DIR ${CMAKE_BINARY_DIR}/openssl-install)
    set(OPENSSL_INCLUDE_DIR ${OPENSSL_INSTALL_DIR}/include)
    set(OPENSSL_CRYPTO_LIBRARY ${OPENSSL_INSTALL_DIR}/lib/libcrypto.a)

    # Use ExternalProject to build OpenSSL for Android
    # Extract numeric API level from ANDROID_PLATFORM (e.g. android-26 -> 26)
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
            ./Configure android-arm64 no-shared no-tests no-asm
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

    # Create imported target
    file(MAKE_DIRECTORY ${OPENSSL_INCLUDE_DIR})
    add_library(OpenSSL::Crypto STATIC IMPORTED)
    set_target_properties(OpenSSL::Crypto PROPERTIES
        IMPORTED_LOCATION ${OPENSSL_CRYPTO_LIBRARY}
        INTERFACE_INCLUDE_DIRECTORIES ${OPENSSL_INCLUDE_DIR}
    )
    add_dependencies(OpenSSL::Crypto openssl_ext)
else()
    find_package(OpenSSL REQUIRED)
endif()

# Vulkan headers - Android NDK doesn't include the C++ headers
if(ANDROID)
    set(VULKAN_VERSION "1.3.268.0")
    if(NOT EXISTS ${FETCHCONTENT_BASE_DIR}/vulkan-sdk-${VULKAN_VERSION}.tar.gz)
        file(DOWNLOAD https://github.com/KhronosGroup/Vulkan-Headers/archive/vulkan-sdk-${VULKAN_VERSION}.tar.gz
            ${FETCHCONTENT_BASE_DIR}/vulkan-sdk-${VULKAN_VERSION}.tar.gz
            EXPECTED_HASH SHA256=94993cbe2b1a604c0d5d9ea37a767e1aba4d771d2bfd4ddceefd66243095164f)
    endif()
    file(ARCHIVE_EXTRACT INPUT ${FETCHCONTENT_BASE_DIR}/vulkan-sdk-${VULKAN_VERSION}.tar.gz DESTINATION ${CMAKE_BINARY_DIR})
    set(VULKAN_INCLUDE_DIR ${CMAKE_BINARY_DIR}/Vulkan-Headers-vulkan-sdk-${VULKAN_VERSION}/include)
endif()

# --- PvrSDK-Native library (from AAR) ---
set(PVR_LIB_PATH ${CMAKE_CURRENT_SOURCE_DIR}/../../external/pvrsdk-native/jni/${ANDROID_ABI}/libPvr_NativeSDK.so)
if(NOT EXISTS ${PVR_LIB_PATH})
    message(FATAL_ERROR
        "PvrSDK-Native not found.\n"
        "The Pico Neo 2 SDK is required to build the Pico native client.\n"
        "Download it from https://developer.picoxr.com/ (PvrSDK-Native 2.8.5.4 or compatible)\n"
        "and extract to external/pvrsdk-native/ so that the path matches:\n"
        "  external/pvrsdk-native/jni/${ANDROID_ABI}/libPvr_NativeSDK.so"
    )
endif()
add_library(Pvr_NativeSDK SHARED IMPORTED)
set_target_properties(Pvr_NativeSDK PROPERTIES
    IMPORTED_LOCATION ${PVR_LIB_PATH}
)

# --- wivrn-common (vendored from WiVRn common library) ---
configure_file(common/wivrn_config.h.in ${CMAKE_CURRENT_BINARY_DIR}/wivrn_config.h)

# Stub version.cpp (no git version tracking in standalone build)
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/version.cpp
    "#include \"version.h\"\n"
    "namespace wivrn {\n"
    "const char git_commit[] = \"unknown\";\n"
    "const bool is_tag = false;\n"
    "const char * display_version() { return \"standalone\"; }\n"
    "}\n"
)

add_library(wivrn-common STATIC EXCLUDE_FROM_ALL
    common/crypto.cpp
    common/smp.cpp
    common/secrets.cpp
    common/wivrn_sockets.cpp
    common/utils/strings.cpp
    common/utils/xdg_base_directory.cpp
    ${CMAKE_CURRENT_BINARY_DIR}/version.cpp
)
target_include_directories(wivrn-common PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/common
    ${CMAKE_CURRENT_BINARY_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/external
)
if(ANDROID)
    target_include_directories(wivrn-common SYSTEM PUBLIC ${VULKAN_INCLUDE_DIR})
    target_compile_definitions(wivrn-common PUBLIC VMA_VULKAN_VERSION=1001000)
endif()
target_link_libraries(wivrn-common PUBLIC Boost::pfr OpenSSL::Crypto spdlog::spdlog)
target_compile_definitions(wivrn-common PUBLIC VULKAN_HPP_NO_STRUCT_CONSTRUCTORS)

# --- Client module ---
add_library(wivrn-neo2 MODULE)

target_sources(wivrn-neo2 PRIVATE
    src/core/main.cpp
    src/core/pico_client.cpp
    src/core/pico_jni.cpp
    src/core/pico_render.cpp
    src/net/wivrn_client_pico.cpp
    src/video/pico_decoder.cpp
    src/audio/pico_audio.cpp
    src/render/pico_blit.cpp
    src/tracking/pico_tracking.cpp
    src/render/pico_lobby.cpp
)

target_include_directories(wivrn-neo2 PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/common
    ${CMAKE_CURRENT_SOURCE_DIR}/../../external/pico-native-sdk/include
    ${CMAKE_CURRENT_BINARY_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/src/core
    ${CMAKE_CURRENT_SOURCE_DIR}/src/audio
    ${CMAKE_CURRENT_SOURCE_DIR}/src/video
    ${CMAKE_CURRENT_SOURCE_DIR}/src/render
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tracking
    ${CMAKE_CURRENT_SOURCE_DIR}/src/net
)

target_link_libraries(wivrn-neo2
    wivrn-common
    openxr_loader
    Pvr_NativeSDK
    spdlog::spdlog
)

# Android libraries
find_library(ANDROID_LIBRARY NAMES android REQUIRED)
find_library(MEDIA_LIBRARY NAMES mediandk REQUIRED)
find_library(AAUDIO_LIBRARY NAMES aaudio REQUIRED)
find_library(LOG_LIBRARY NAMES log REQUIRED)
find_library(GLES_LIBRARY NAMES GLESv2 REQUIRED)
find_library(EGL_LIBRARY NAMES EGL REQUIRED)
find_library(JNIGRAPHICS_LIBRARY NAMES jnigraphics REQUIRED)
target_link_libraries(wivrn-neo2
    ${ANDROID_LIBRARY}
    ${MEDIA_LIBRARY}
    ${AAUDIO_LIBRARY}
    ${LOG_LIBRARY}
    ${GLES_LIBRARY}
    ${EGL_LIBRARY}
    ${JNIGRAPHICS_LIBRARY}
)

# Copy Pvr_NativeSDK.so to output
get_target_property(pvr_lib_path Pvr_NativeSDK IMPORTED_LOCATION)
add_custom_command(TARGET wivrn-neo2 POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
    ${pvr_lib_path} $<TARGET_FILE_DIR:wivrn-neo2>
)

target_compile_definitions(wivrn-neo2 PRIVATE
    -DSPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_DEBUG
)
