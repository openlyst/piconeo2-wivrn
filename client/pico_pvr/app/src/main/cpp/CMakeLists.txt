cmake_minimum_required(VERSION 3.22)
project(pico_pvr)

set(CMAKE_CXX_STANDARD 20)
add_compile_options(-fconstexpr-steps=268435456)

find_library(log-lib log)
find_library(egl-lib EGL)
find_library(gles-lib GLESv3)
find_library(android-lib android)

# Pvr Native SDK (tracking, ATW, controllers)
add_library(Pvr_NativeSDK SHARED IMPORTED)
set_target_properties(Pvr_NativeSDK PROPERTIES
    IMPORTED_LOCATION ${CMAKE_CURRENT_SOURCE_DIR}/../jniLibs/${ANDROID_ABI}/libPvr_NativeSDK.so
)

# spdlog and boost::pfr are header-only - vendored copies in third_party/
set(THIRD_PARTY_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
set(SPDLOG_INCLUDE_DIR ${THIRD_PARTY_DIR})
set(BOOST_INCLUDE_DIR ${THIRD_PARTY_DIR})

add_library(pico_pvr SHARED
    main.cpp
    lobby.cpp
)

target_include_directories(pico_pvr PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_include_directories(pico_pvr SYSTEM PRIVATE ${SPDLOG_INCLUDE_DIR} ${BOOST_INCLUDE_DIR})

target_compile_definitions(pico_pvr PRIVATE SPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_DEBUG SPDLOG_FMT_EXTERNAL FMT_HEADER_ONLY)

target_link_libraries(pico_pvr
    ${log-lib}
    ${egl-lib}
    ${gles-lib}
    ${android-lib}
    Pvr_NativeSDK
)

