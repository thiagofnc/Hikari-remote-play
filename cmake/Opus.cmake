# Fetches and builds libopus as a static library.
#
# Provides the `opus` target. Anything that links it can `#include <opus.h>`
# (upstream exports `<src>/include`, which contains opus.h, as a PUBLIC
# build-interface include directory).
#
# Included from the top-level CMakeLists.txt after the project-wide compile
# settings (runtime library, /O2 /GL, defines) and before add_subdirectory()
# of our own code, so libopus is built consistently with the rest of the
# project. Our warning flags are stripped from the opus targets below.
#
# Set FETCHCONTENT_SOURCE_DIR_OPUS=<path> to use a local checkout instead of
# downloading (useful offline). FETCHCONTENT_BASE_DIR can point at a shared
# cache directory; CI caches it.

include_guard(GLOBAL)
include(FetchContent)

set(HIKARI_OPUS_GIT_TAG "v1.5.2" CACHE STRING "libopus release tag to build")
mark_as_advanced(HIKARI_OPUS_GIT_TAG)

# Upstream options (opus/CMakeLists.txt). Cache them so the subproject's
# option() calls pick up our values.
set(OPUS_BUILD_SHARED_LIBRARY        OFF CACHE BOOL "" FORCE)
set(OPUS_BUILD_TESTING               OFF CACHE BOOL "" FORCE)
set(OPUS_BUILD_PROGRAMS              OFF CACHE BOOL "" FORCE)
set(OPUS_INSTALL_PKG_CONFIG_MODULE   OFF CACHE BOOL "" FORCE)
set(OPUS_INSTALL_CMAKE_CONFIG_MODULE OFF CACHE BOOL "" FORCE)
set(OPUS_DRED                        OFF CACHE BOOL "" FORCE)
set(OPUS_DEEP_PLC                    OFF CACHE BOOL "" FORCE)
set(OPUS_OSCE                        OFF CACHE BOOL "" FORCE)
if(MINGW)
    # _FORTIFY_SOURCE / -fstack-protector pull in libssp on MinGW, which is
    # not always available for fully static links.
    set(OPUS_FORTIFY_SOURCE  OFF CACHE BOOL "" FORCE)
    set(OPUS_STACK_PROTECTOR OFF CACHE BOOL "" FORCE)
endif()

# Force a static libopus and no upstream tests, then restore the caller's
# values afterwards.
foreach(_hk_var BUILD_SHARED_LIBS BUILD_TESTING)
    if(DEFINED ${_hk_var})
        set(_hk_saved_${_hk_var} "${${_hk_var}}")
    endif()
    set(${_hk_var} OFF)
endforeach()

# The top-level CMakeLists sets CMAKE_MSVC_RUNTIME_LIBRARY before including
# us; default to the static CRT anyway in case this file is included earlier
# or from elsewhere, so opus always matches the project (/MT, /MTd).
if(MSVC AND NOT DEFINED CMAKE_MSVC_RUNTIME_LIBRARY)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

set(_hk_opus_fetch_args
    GIT_REPOSITORY https://github.com/xiph/opus.git
    GIT_TAG        ${HIKARI_OPUS_GIT_TAG}
    GIT_SHALLOW    TRUE
    GIT_PROGRESS   FALSE)
if(CMAKE_VERSION VERSION_GREATER_EQUAL 3.24)
    # Prefer our pinned source over any system libopus found via find_package.
    list(APPEND _hk_opus_fetch_args OVERRIDE_FIND_PACKAGE)
endif()
if(CMAKE_VERSION VERSION_GREATER_EQUAL 3.28)
    # Keep libopus out of `install` and the default ALL set except what we link.
    list(APPEND _hk_opus_fetch_args EXCLUDE_FROM_ALL)
endif()

FetchContent_Declare(opus ${_hk_opus_fetch_args})

FetchContent_MakeAvailable(opus)

if(NOT TARGET opus)
    message(FATAL_ERROR "libopus fetch did not produce an `opus` target")
endif()

# Belt and braces: pin the runtime on the target itself, and replace the
# project-wide warning flags (inherited directory COMPILE_OPTIONS) with
# "no warnings" for this third-party code. Removing them instead of
# appending /W0 avoids MSVC's D9025 "overriding /W4" noise on every file.
set(_hk_opus_targets opus)
if(TARGET opus-native)
    list(APPEND _hk_opus_targets opus-native)
endif()
foreach(_hk_t IN LISTS _hk_opus_targets)
    if(MSVC)
        set_property(TARGET ${_hk_t} PROPERTY MSVC_RUNTIME_LIBRARY
            "${CMAKE_MSVC_RUNTIME_LIBRARY}")
    endif()
    get_target_property(_hk_opts ${_hk_t} COMPILE_OPTIONS)
    if(_hk_opts)
        list(FILTER _hk_opts EXCLUDE REGEX "^(/W[0-4]|/Wall|-W.*)$")
        set_property(TARGET ${_hk_t} PROPERTY COMPILE_OPTIONS ${_hk_opts})
    endif()
    if(MSVC)
        target_compile_options(${_hk_t} PRIVATE /W0)
    else()
        target_compile_options(${_hk_t} PRIVATE -w)
    endif()
endforeach()
unset(_hk_t)
unset(_hk_opts)
unset(_hk_opus_targets)

# Upstream already exports <src>/include (opus.h) as a PUBLIC build interface
# include dir; add it explicitly so `#include <opus.h>` works regardless of
# upstream changes between releases.
target_include_directories(opus SYSTEM INTERFACE
    $<BUILD_INTERFACE:${opus_SOURCE_DIR}/include>)

if(NOT TARGET Opus::opus)
    add_library(Opus::opus ALIAS opus)
endif()

foreach(_hk_var BUILD_SHARED_LIBS BUILD_TESTING)
    if(DEFINED _hk_saved_${_hk_var})
        set(${_hk_var} "${_hk_saved_${_hk_var}}")
        unset(_hk_saved_${_hk_var})
    else()
        unset(${_hk_var})
    endif()
endforeach()
unset(_hk_var)
unset(_hk_opus_fetch_args)
