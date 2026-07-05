cmake_minimum_required(VERSION 3.22)
project(openxrpoc)

set(CMAKE_CXX_STANDARD 17)

find_library(log-lib log)
find_library(egl-lib EGL)
find_library(gles-lib GLESv3)
find_library(android-lib android)

# Import Pico's openxr_loader from jniLibs
add_library(openxr_loader SHARED IMPORTED)
set_target_properties(openxr_loader PROPERTIES
    IMPORTED_LOCATION ${CMAKE_SOURCE_DIR}/../jniLibs/${ANDROID_ABI}/libopenxr_loader.so
)

# android_native_app_glue (compiled as objects into the shared lib)
add_library(native_app_glue OBJECT
    ${ANDROID_NDK}/sources/android/native_app_glue/android_native_app_glue.c
)
target_include_directories(native_app_glue PUBLIC
    ${ANDROID_NDK}/sources/android/native_app_glue
)

add_library(openxrpoc SHARED
    main.cpp
    $<TARGET_OBJECTS:native_app_glue>
)

target_include_directories(openxrpoc PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/openxr
    ${ANDROID_NDK}/sources/android/native_app_glue
)

target_link_libraries(openxrpoc
    ${log-lib}
    ${egl-lib}
    ${gles-lib}
    ${android-lib}
    openxr_loader
)
