# BundledCurl.cmake — build a feature-stripped, fully static libcurl from source.
#
# moocode only ever performs HTTPS GET/POST with custom headers, a write
# callback, a timeout and gzip/deflate decompression (see agent/http.cpp). The
# distro libcurl drags in ~20 shared objects for protocols we never touch
# (HTTP/3, SSH, IMAP/SMTP/POP3/RTSP/LDAP, krb5, brotli, zstd, idn2, psl, …).
#
# This module instead builds, as static archives via ExternalProject:
#   zlib  ->  nghttp2  ->  OpenSSL  ->  curl
# and exposes the result as the imported target CURL::libcurl, so the rest of
# the tree (src/CMakeLists.txt links CURL::libcurl) is untouched. The only
# runtime shared deps left are the C/C++ runtime (libc/libstdc++/libm/libgcc).
#
# Pinned, hash-verified release tarballs are reused from ${PROJECT_SOURCE_DIR}/
# .deps-cache when present, otherwise downloaded on first configure.

include(ExternalProject)
include(ProcessorCount)

ProcessorCount(_bc_nproc)
if(_bc_nproc EQUAL 0)
  set(_bc_nproc 2)
endif()

set(_bc_install   ${CMAKE_BINARY_DIR}/_static_curl)        # install prefix for all four
set(_bc_lib       ${_bc_install}/lib)
set(_bc_inc       ${_bc_install}/include)
set(_bc_download  ${PROJECT_SOURCE_DIR}/.deps-cache)       # pre-seeded tarball cache

# INTERFACE_INCLUDE_DIRECTORIES of an imported target must exist at configure
# time; the archives themselves are produced later by the ExternalProject steps.
file(MAKE_DIRECTORY ${_bc_inc})

# Sub-builds ship cmake_minimum_required() values below CMake 4's floor; allow
# them rather than patching upstream.
set(_bc_common_cmake_args
  -DCMAKE_POLICY_VERSION_MINIMUM=3.5
  -DCMAKE_BUILD_TYPE=Release
  -DCMAKE_POSITION_INDEPENDENT_CODE=ON
  -DBUILD_SHARED_LIBS=OFF
  -DCMAKE_INSTALL_PREFIX=${_bc_install}
  -DCMAKE_INSTALL_LIBDIR=lib                 # never lib64, so paths are stable
)
# Propagate the cross toolchain (and a generator matching the outer build) to
# the nested CMake sub-builds, so zlib/nghttp2/curl cross-compile for Windows
# too. Absolute path: ExternalProject runs the sub-cmake from its own dir.
if(CMAKE_TOOLCHAIN_FILE)
  get_filename_component(_bc_toolchain "${CMAKE_TOOLCHAIN_FILE}" ABSOLUTE
                         BASE_DIR "${CMAKE_BINARY_DIR}")
  list(APPEND _bc_common_cmake_args "-DCMAKE_TOOLCHAIN_FILE=${_bc_toolchain}")
endif()

# --- zlib (gzip/deflate for CURLOPT_ACCEPT_ENCODING) ------------------------
# zlib's CMake names the static archive `z` on UNIX but `zlibstatic` on Windows.
if(WIN32)
  set(_bc_zlib_a ${_bc_lib}/libzlibstatic.a)
else()
  set(_bc_zlib_a ${_bc_lib}/libz.a)
endif()
ExternalProject_Add(zlib_ep
  URL              https://github.com/madler/zlib/releases/download/v1.3.1/zlib-1.3.1.tar.gz
  URL_HASH         SHA256=9a93b2b7dfdac77ceba5a558a580e74667dd6fede4585b91eefb60f03b72df23
  DOWNLOAD_DIR     ${_bc_download}
  CMAKE_ARGS       ${_bc_common_cmake_args}
  BUILD_BYPRODUCTS ${_bc_zlib_a}
  UPDATE_DISCONNECTED ON
)

# --- nghttp2 (HTTP/2; lib only, no apps/tests) ------------------------------
ExternalProject_Add(nghttp2_ep
  URL              https://github.com/nghttp2/nghttp2/releases/download/v1.65.0/nghttp2-1.65.0.tar.xz
  URL_HASH         SHA256=f1b9df5f02e9942b31247e3d415483553bc4ac501c87aa39340b6d19c92a9331
  DOWNLOAD_DIR     ${_bc_download}
  CMAKE_ARGS       ${_bc_common_cmake_args}
                   -DENABLE_LIB_ONLY=ON
                   -DBUILD_STATIC_LIBS=ON   # 1.65 gates the .a on this, not ENABLE_STATIC_LIB
                   -DENABLE_DOC=OFF
                   -DBUILD_TESTING=OFF
  BUILD_BYPRODUCTS ${_bc_lib}/libnghttp2.a
  UPDATE_DISCONNECTED ON
)

# --- OpenSSL (TLS; Perl Configure, no CMake) --------------------------------
# Skipped on Windows: there curl uses Schannel (the OS TLS stack), so no OpenSSL
# sub-build is needed and the Win32 crypto libs are linked instead (see below).
if(NOT WIN32)
# no-apps/no-tests/no-docs slash build time; no-shared gives static archives;
# --libdir=lib keeps install paths predictable.
#
# OpenSSL's Perl Configure needs an explicit platform/arch target; the wrong one
# emits assembly for the wrong ISA (e.g. x86_64 .s on Apple Silicon -> inline-asm
# build errors). Pick it from the host OS + processor.
set(_bc_host_proc "${CMAKE_HOST_SYSTEM_PROCESSOR}")
if(NOT _bc_host_proc)
  set(_bc_host_proc "${CMAKE_SYSTEM_PROCESSOR}")
endif()
string(TOLOWER "${_bc_host_proc}" _bc_host_proc)
if(CMAKE_HOST_APPLE)
  if(_bc_host_proc MATCHES "arm|aarch64")
    set(_bc_openssl_target darwin64-arm64-cc)
  else()
    set(_bc_openssl_target darwin64-x86_64-cc)
  endif()
elseif(_bc_host_proc MATCHES "arm|aarch64")
  set(_bc_openssl_target linux-aarch64)
else()
  set(_bc_openssl_target linux-x86_64)
endif()
message(STATUS "BundledCurl: OpenSSL target = ${_bc_openssl_target}")

ExternalProject_Add(openssl_ep
  URL              https://github.com/openssl/openssl/releases/download/openssl-3.5.0/openssl-3.5.0.tar.gz
  URL_HASH         SHA256=344d0a79f1a9b08029b0744e2cc401a43f9c90acd1044d09a530b4885a8e9fc0
  DOWNLOAD_DIR     ${_bc_download}
  BUILD_IN_SOURCE  1
  CONFIGURE_COMMAND perl <SOURCE_DIR>/Configure
                       --prefix=${_bc_install}
                       --openssldir=${_bc_install}/ssl
                       --libdir=lib
                       no-shared no-apps no-tests no-docs no-legacy
                       ${_bc_openssl_target}
  BUILD_COMMAND     make -j${_bc_nproc}
  INSTALL_COMMAND   make install_sw
  BUILD_BYPRODUCTS  ${_bc_lib}/libssl.a ${_bc_lib}/libcrypto.a
  UPDATE_DISCONNECTED ON
)

endif()  # NOT WIN32 (OpenSSL sub-build)

# --- CA trust store: bake the system bundle into the static curl ------------
# A statically linked curl has no distro default; point it at whatever the host
# ships so HTTPS verification works without per-call CURLOPT_CAINFO. On Windows
# Schannel uses the OS certificate store, so no bundle is needed.
set(_bc_ca_bundle "")
set(_bc_ca_path "")
if(NOT WIN32)
  foreach(_c /etc/ssl/certs/ca-certificates.crt /etc/pki/tls/certs/ca-bundle.crt
             /etc/ssl/ca-bundle.pem /etc/ssl/cert.pem)
    if(EXISTS ${_c})
      set(_bc_ca_bundle ${_c})
      break()
    endif()
  endforeach()
  if(IS_DIRECTORY /etc/ssl/certs)
    set(_bc_ca_path /etc/ssl/certs)
  endif()
  if(_bc_ca_bundle)
    message(STATUS "BundledCurl: CA bundle = ${_bc_ca_bundle}")
  else()
    message(WARNING "BundledCurl: no system CA bundle found; HTTPS may fail "
                    "unless CURLOPT_CAINFO / SSL_CERT_FILE is set at runtime")
  endif()
endif()

# --- curl (HTTP/HTTPS only; everything else disabled) -----------------------
# TLS backend differs by platform: static OpenSSL on POSIX, native Schannel on
# Windows (so no OpenSSL archives are needed in the link there).
if(WIN32)
  # Schannel TLS; NGHTTP2_STATICLIB tells curl's http2.c to declare the nghttp2
  # symbols as plain statics (not dllimport) so they resolve against libnghttp2.a.
  set(_bc_curl_tls -DCURL_USE_SCHANNEL=ON -DCMAKE_C_FLAGS=-DNGHTTP2_STATICLIB)
  set(_bc_curl_deps zlib_ep nghttp2_ep)
else()
  set(_bc_curl_tls -DCURL_USE_OPENSSL=ON -DOPENSSL_ROOT_DIR=${_bc_install}
                   -DOPENSSL_USE_STATIC_LIBS=ON)
  set(_bc_curl_deps zlib_ep nghttp2_ep openssl_ep)
endif()
ExternalProject_Add(curl_ep
  DEPENDS          ${_bc_curl_deps}
  URL              https://curl.se/download/curl-8.15.0.tar.xz
  URL_HASH         SHA256=6cd0a8a5b126ddfda61c94dc2c3fc53481ba7a35461cf7c5ab66aa9d6775b609
  DOWNLOAD_DIR     ${_bc_download}
  CMAKE_ARGS       ${_bc_common_cmake_args}
                   -DCMAKE_PREFIX_PATH=${_bc_install}
                   -DBUILD_CURL_EXE=OFF
                   -DBUILD_STATIC_LIBS=ON
                   -DBUILD_TESTING=OFF
                   # TLS backend (OpenSSL on POSIX, Schannel on Windows)
                   ${_bc_curl_tls}
                   # zlib + HTTP/2, both static
                   -DCURL_ZLIB=ON
                   -DZLIB_ROOT=${_bc_install}
                   -DZLIB_USE_STATIC_LIBS=ON
                   -DUSE_NGHTTP2=ON
                   # things we deliberately drop
                   -DUSE_NGHTTP3=OFF -DUSE_NGTCP2=OFF -DUSE_QUICHE=OFF
                   -DCURL_USE_LIBSSH2=OFF -DCURL_USE_LIBSSH=OFF
                   -DCURL_USE_LIBPSL=OFF -DUSE_LIBIDN2=OFF
                   -DCURL_BROTLI=OFF -DCURL_ZSTD=OFF
                   -DCURL_USE_GSSAPI=OFF -DUSE_WIN32_LDAP=OFF
                   -DENABLE_UNIX_SOCKETS=OFF
                   # disable every protocol except HTTP/HTTPS
                   -DCURL_DISABLE_DICT=ON  -DCURL_DISABLE_FILE=ON
                   -DCURL_DISABLE_FTP=ON   -DCURL_DISABLE_GOPHER=ON
                   -DCURL_DISABLE_IMAP=ON  -DCURL_DISABLE_LDAP=ON
                   -DCURL_DISABLE_LDAPS=ON -DCURL_DISABLE_MQTT=ON
                   -DCURL_DISABLE_POP3=ON  -DCURL_DISABLE_RTSP=ON
                   -DCURL_DISABLE_SMB=ON   -DCURL_DISABLE_SMTP=ON
                   -DCURL_DISABLE_TELNET=ON -DCURL_DISABLE_TFTP=ON
                   -DCURL_DISABLE_WEBSOCKETS=ON
                   -DCURL_CA_BUNDLE=${_bc_ca_bundle}
                   -DCURL_CA_PATH=${_bc_ca_path}
  BUILD_BYPRODUCTS ${_bc_lib}/libcurl.a
  UPDATE_DISCONNECTED ON
)

# --- imported target consumed as CURL::libcurl ------------------------------
# Static link order matters: libcurl -> nghttp2 -> ssl -> crypto -> z, then the
# pthread/dl that static OpenSSL needs.
find_package(Threads REQUIRED)
add_library(CURL::libcurl STATIC IMPORTED GLOBAL)
set_target_properties(CURL::libcurl PROPERTIES
  IMPORTED_LOCATION             ${_bc_lib}/libcurl.a
  INTERFACE_INCLUDE_DIRECTORIES ${_bc_inc}
  INTERFACE_COMPILE_DEFINITIONS CURL_STATICLIB
)
if(WIN32)
  # Schannel build: no OpenSSL archives; link the Win32 socket + crypto libs
  # that a static libcurl with Schannel/SSPI references.
  target_link_libraries(CURL::libcurl INTERFACE
    ${_bc_lib}/libnghttp2.a
    ${_bc_zlib_a}
    # ws2_32: sockets; secur32: Schannel SSPI; crypt32/bcrypt: cert store + crypto;
    # iphlpapi: if_nametoindex; wldap32/advapi32/normaliz: curl misc deps.
    ws2_32 secur32 crypt32 bcrypt iphlpapi wldap32 advapi32 normaliz
    Threads::Threads
  )
else()
  target_link_libraries(CURL::libcurl INTERFACE
    ${_bc_lib}/libnghttp2.a
    ${_bc_lib}/libssl.a
    ${_bc_lib}/libcrypto.a
    ${_bc_zlib_a}
    Threads::Threads
    ${CMAKE_DL_LIBS}
  )
endif()

# macOS: bundled curl's macos.c reads the system proxy config via
# SystemConfiguration (SCDynamicStoreCopyProxies) and CoreFoundation (CFRelease).
# A shared system libcurl pulls these frameworks in transitively; our static
# archive does not, so link them explicitly or the final exe fails with
# "Undefined symbols: _CFRelease, _SCDynamicStoreCopyProxies".
if(APPLE)
  target_link_libraries(CURL::libcurl INTERFACE
    "-framework CoreFoundation"
    "-framework SystemConfiguration"
  )
endif()

# Name of the umbrella ExternalProject target; the consumer (agent_http) must
# add_dependencies() on it so the archives exist before it links.
set(BUNDLED_CURL_TARGET curl_ep)
