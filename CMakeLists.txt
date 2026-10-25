cmake_minimum_required(VERSION 3.21)

project(Hikari
    VERSION 0.1.2
    DESCRIPTION "Low-latency Nintendo Switch 2 remote play"
    LANGUAGES C CXX)

if(NOT WIN32)
    message(FATAL_ERROR "Hikari desktop apps target Windows 10/11. Build the firmware with ESP-IDF in firmware/esp32-s3.")
endif()

option(HIKARI_WITH_OPUS "Build Opus audio support (fetches libopus)" ON)
option(HIKARI_BUILD_TESTS "Build unit tests" ON)
option(HIKARI_BUILD_TOOLS "Build probe/benchmark tools" ON)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_C_STANDARD 99)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# Ship self-contained executables: no VC++ redistributable required.
if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    add_compile_options(/W4 /permissive- /utf-8 /MP /Zc:__cplusplus
        /wd4100 /wd4201 /wd4324)
    add_compile_options($<$<CONFIG:Release>:/O2> $<$<CONFIG:Release>:/GL>)
    add_link_options($<$<CONFIG:Release>:/LTCG> $<$<CONFIG:Release>:/OPT:REF> $<$<CONFIG:Release>:/OPT:ICF>)
else()
    add_compile_options(-Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers)
    add_link_options(-static)
endif()

# Release builds pass the tag's version (e.g. 1.2.0) to stamp the binaries.
set(HIKARI_VERSION_OVERRIDE "" CACHE STRING "Version string to embed instead of PROJECT_VERSION")
if(HIKARI_VERSION_OVERRIDE)
    set(HIKARI_VERSION_STRING ${HIKARI_VERSION_OVERRIDE})
else()
    set(HIKARI_VERSION_STRING ${PROJECT_VERSION})
endif()

add_compile_definitions(
    WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE
    _WIN32_WINNT=0x0A00 WINVER=0x0A00 NTDDI_VERSION=0x0A000006
    HIKARI_VERSION="${HIKARI_VERSION_STRING}")

if(HIKARI_WITH_OPUS)
    include(cmake/Opus.cmake)
endif()

add_subdirectory(shared)
add_subdirectory(media)
add_subdirectory(host)
add_subdirectory(ui)
add_subdirectory(client)
add_subdirectory(app)

if(HIKARI_BUILD_TOOLS)
    add_subdirectory(tools)
endif()

if(HIKARI_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
