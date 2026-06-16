# CMake toolchain for cross-compiling moocode to 64-bit Windows with the
# MinGW-w64 cross GCC (matches the w64devkit target). Use with:
#   cmake -B build-win -G Ninja \
#         -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64-x86_64.cmake
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

set(_tc x86_64-w64-mingw32)
set(CMAKE_C_COMPILER   ${_tc}-gcc)
set(CMAKE_CXX_COMPILER ${_tc}-g++)
set(CMAKE_RC_COMPILER  ${_tc}-windres)

set(CMAKE_FIND_ROOT_PATH /usr/${_tc})
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
# BOTH (not ONLY): the bundled-curl sub-builds install zlib/nghttp2 into an
# ExternalProject prefix outside the sysroot and must find them there. moocode's
# own targets don't find_package any host libraries (nlohmann is the bundled
# header; Threads adds only -pthread), so this doesn't leak host includes.
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY BOTH)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE BOTH)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE BOTH)

# Statically link the GCC/C++/winpthread runtimes so the produced .exe runs on a
# stock Windows without shipping libstdc++-6.dll / libwinpthread-1.dll alongside.
set(CMAKE_EXE_LINKER_FLAGS_INIT "-static -static-libgcc -static-libstdc++")
