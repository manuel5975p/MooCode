cmake_minimum_required(VERSION 3.20)
project(moocode LANGUAGES CXX)

# --- Global conventions -----------------------------------------------------
set(CMAKE_CXX_STANDARD 23)            # std::expected
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

# --- LTO_MINSIZE: link-time-optimized, size-minimal release build ------------
# A custom build type that pushes for the smallest possible binary: -Oz, NDEBUG,
# full LTO and a stripped (-s) link. The LTO spelling differs per compiler.
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
  set(_MOOCODE_LTO_FLAGS "-flto -flto-partition=one")
elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  set(_MOOCODE_LTO_FLAGS "-flto=full")
else()
  set(_MOOCODE_LTO_FLAGS "-flto")
endif()
set(CMAKE_CXX_FLAGS_LTO_MINSIZE
    "-Oz -DNDEBUG ${_MOOCODE_LTO_FLAGS}"
    CACHE STRING "Flags used by the C++ compiler during LTO_MINSIZE builds." FORCE)
set(CMAKE_C_FLAGS_LTO_MINSIZE
    "-Oz -DNDEBUG ${_MOOCODE_LTO_FLAGS}"
    CACHE STRING "Flags used by the C compiler during LTO_MINSIZE builds." FORCE)
set(CMAKE_EXE_LINKER_FLAGS_LTO_MINSIZE
    "${_MOOCODE_LTO_FLAGS} -s"
    CACHE STRING "Flags used by the linker during LTO_MINSIZE builds." FORCE)
set(CMAKE_SHARED_LINKER_FLAGS_LTO_MINSIZE
    "${_MOOCODE_LTO_FLAGS} -s"
    CACHE STRING "Flags used by the linker during LTO_MINSIZE shared-lib builds." FORCE)
mark_as_advanced(
  CMAKE_CXX_FLAGS_LTO_MINSIZE
  CMAKE_C_FLAGS_LTO_MINSIZE
  CMAKE_EXE_LINKER_FLAGS_LTO_MINSIZE
  CMAKE_SHARED_LINKER_FLAGS_LTO_MINSIZE)
# Single-config generators show LTO_MINSIZE alongside the stock build types.
set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS
             Debug Release RelWithDebInfo MinSizeRel LTO_MINSIZE)

# --- Dependencies -----------------------------------------------------------
# libcurl: by default build a feature-stripped, fully static one from source so
# the binary carries no HTTP/3 / SSH / krb5 / brotli / … shared objects. Set
# -DMOOCODE_BUNDLED_CURL=OFF to fall back to the system libcurl instead.
option(MOOCODE_BUNDLED_CURL "Build a static, minimal libcurl from source" ON)
# Treat warnings as errors for moocode's own targets. Default OFF: the toolchain
# is unpinned, so a newer GCC/Clang warning must never brick a stock build. The
# dev/CI preset opts in.
option(MOOCODE_WERROR "Treat warnings as errors for moocode's own targets" OFF)
# Sanitizers for moocode's own targets. A STRING (not a bool) so the mutually
# exclusive "address;undefined" and "thread" are both expressible. Empty (the
# default) keeps today's behaviour.
set(MOOCODE_SANITIZE "" CACHE STRING "Sanitizers for moocode targets: address;undefined | thread")
if(MOOCODE_BUNDLED_CURL)
  list(APPEND CMAKE_MODULE_PATH ${PROJECT_SOURCE_DIR}/cmake)
  include(BundledCurl)
else()
  find_package(CURL REQUIRED)
endif()
find_package(nlohmann_json REQUIRED)

# FTXUI: the terminal-UI toolkit behind the interactive TUI. Fetched and built
# from the pinned v6.1.9 release tarball (examples/docs/tests off). Declared
# before the project's strict warning flags so its sources build flag-clean.
# Three local rendering fixes (glyph widths, half-glyph clipping, synchronized
# updates) are applied at populate time — see cmake/PatchFTXUI.cmake.
include(FetchContent)
FetchContent_Declare(ftxui
  URL      https://github.com/ArthurSonzogni/FTXUI/archive/refs/tags/v6.1.9.tar.gz
  URL_HASH SHA256=45819c1e54914783d4a1ca5633885035d74146778a1f74e1213cdb7b76340e71
  PATCH_COMMAND ${CMAKE_COMMAND} -P ${PROJECT_SOURCE_DIR}/cmake/PatchFTXUI.cmake
)
set(FTXUI_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(FTXUI_BUILD_DOCS     OFF CACHE BOOL "" FORCE)
set(FTXUI_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
set(FTXUI_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(ftxui)

# Project-wide warning flags apply only to targets defined from here down, i.e.
# moocode's own libraries — not the fetched FTXUI sources above.
add_compile_options(-Wall -Wextra -Wpedantic)
if(MOOCODE_WERROR)
  add_compile_options($<$<COMPILE_LANGUAGE:CXX>:-Werror>)
endif()
if(MOOCODE_SANITIZE)
  # MOOCODE_SANITIZE is a CMake list ("address;undefined"); -fsanitize wants a
  # comma-separated value, so join before handing it to the compiler/linker.
  string(REPLACE ";" "," _MOOCODE_SANITIZE_FLAG "${MOOCODE_SANITIZE}")
  add_compile_options(-fsanitize=${_MOOCODE_SANITIZE_FLAG} -fno-omit-frame-pointer -g)
  add_link_options(-fsanitize=${_MOOCODE_SANITIZE_FLAG})
endif()

# --- Libraries & executable -------------------------------------------------
add_subdirectory(src)

# agent_http links the imported CURL::libcurl (an archive produced by the
# bundled ExternalProject); make it wait for that build to finish.
if(MOOCODE_BUNDLED_CURL)
  add_dependencies(agent_http ${BUNDLED_CURL_TARGET})
endif()

# --- Tests ------------------------------------------------------------------
# Declared before include(CTest) so this OFF default wins over CTest's ON one;
# pass -DBUILD_TESTING=ON to build and register the test executables.
option(BUILD_TESTING "Build the test suite" OFF)
include(CTest)                        # honours the BUILD_TESTING option above
if(BUILD_TESTING)
  add_subdirectory(tests)
endif()
