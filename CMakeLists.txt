cmake_minimum_required(VERSION 3.22.1)
project(ftl_quest3 LANGUAGES C CXX ASM)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Configuration Options
option(FTL_BUILD_STANDALONE "Build standalone ELF executable for test/adb shell" OFF)
option(FTL_ENABLE_OPENXR "Build with OpenXR Quad/Stereo Layer support for Quest 3" OFF)
option(FTL_MODE_DECOMP "Build using native decompiled C++ files" OFF)
option(FTL_MODE_LOADER "Build with in-memory Mach-O loader" ON)

# Include directories
include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/shim/include
    ${CMAKE_CURRENT_SOURCE_DIR}/decomp/include
)

# Core Shim Sources
set(SHIM_SOURCES
    shim/src/darwin_abi.cpp
    shim/src/objc_shim.cpp
    shim/src/objc_msgSend_arm64.S
    shim/src/eagl_shim.cpp
    shim/src/foundation_shim.cpp
    shim/src/uikit_shim.cpp
    shim/src/openal_shim.cpp
    shim/src/apple_string_shim.cpp
    shim/src/quest_entry.cpp
    shim/src/macho_loader.cpp
    shim/src/pvrtc_decode.cpp
)

# Decompiled Game Logic Sources
set(DECOMP_SOURCES
    decomp/src/CApp.cpp
)

set_source_files_properties(shim/src/pvrtc_decode.cpp PROPERTIES COMPILE_OPTIONS "-O2")

# Primary Output Target: Android native shared library
add_library(ftl_quest SHARED
    ${SHIM_SOURCES}
    $<$<BOOL:${FTL_MODE_DECOMP}>:${DECOMP_SOURCES}>
)

target_compile_definitions(ftl_quest PRIVATE
    FTL_QUEST3=1
    GL_GLEXT_PROTOTYPES=1
)

target_compile_options(ftl_quest PRIVATE
    -Wall
    -Wextra
    -Wno-unused-parameter
    -Wno-unused-variable
    -fvisibility=default
    -fexceptions
    -frtti
)

if(FTL_ENABLE_OPENXR)
    set(FTL_OPENXR_INCLUDE_DIR "" CACHE PATH "OpenXR headers include dir")
    set(FTL_OPENXR_LIB "" CACHE FILEPATH "libopenxr_loader.so")
    if(NOT EXISTS "${FTL_OPENXR_INCLUDE_DIR}/openxr/openxr.h" OR NOT EXISTS "${FTL_OPENXR_LIB}")
        message(FATAL_ERROR "FTL_ENABLE_OPENXR needs FTL_OPENXR_INCLUDE_DIR and FTL_OPENXR_LIB")
    endif()
    target_sources(ftl_quest PRIVATE shim/src/quest_xr.cpp)
    target_compile_definitions(ftl_quest PRIVATE FTL_ENABLE_OPENXR=1)
    target_include_directories(ftl_quest PRIVATE "${FTL_OPENXR_INCLUDE_DIR}")
    target_link_libraries(ftl_quest PRIVATE "${FTL_OPENXR_LIB}")
endif()

# System and NDK Dependencies
find_library(LOG_LIB log)
find_library(ANDROID_LIB android)
find_library(EGL_LIB EGL)
find_library(GLES3_LIB GLESv3)
find_library(AAUDIO_LIB aaudio)
find_library(Z_LIB z)

target_link_libraries(ftl_quest PRIVATE
    ${LOG_LIB}
    ${ANDROID_LIB}
    ${EGL_LIB}
    ${GLES3_LIB}
    ${AAUDIO_LIB}
    ${Z_LIB}
)

# Optional standalone ELF executable for adb shell headless / testing
if(FTL_BUILD_STANDALONE)
    add_executable(ftl_quest_cli
        ${SHIM_SOURCES}
        $<$<BOOL:${FTL_MODE_DECOMP}>:${DECOMP_SOURCES}>
    )
    target_link_libraries(ftl_quest_cli PRIVATE
        ${LOG_LIB}
        ${ANDROID_LIB}
        ${EGL_LIB}
        ${GLES3_LIB}
        ${Z_LIB}
    )
endif()
