cmake_minimum_required(VERSION 3.22)
project(openxrpoc)

set(CMAKE_CXX_STANDARD 17)

find_library(log-lib log)
find_library(egl-lib EGL)
find_library(gles-lib GLESv3)
find_library(android-lib android)

add_library(openxrpoc SHARED
    main.cpp
)

target_include_directories(openxrpoc PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/openxr
)

target_link_libraries(openxrpoc
    ${log-lib}
    ${egl-lib}
    ${gles-lib}
    ${android-lib}
    dl
)
