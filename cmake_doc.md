# CMake build system

`moocode` is a C++23 project (see top-level `CMakeLists.txt`) with one
`ExternalProject`-based dependency graph and one `FetchContent`-based
dependency. The top-level file is intentionally short; the heavy lifting
lives in `cmake/BundledCurl.cmake`.

## `CMakeLists.txt` (root)

### Toolchain
- `cmake_minimum_required(VERSION 3.20)`, C++23 (`std::expected`),
  extensions off.
- Default build type `Release` when none is set; respects existing
  multi-config generators via the `CMAKE_CONFIGURATION_TYPES` guard.
- Project-wide `-Wall -Wextra -Wpedantic` — see "Warnings" below.

### Dependencies

| Dependency     | Source                                            | Knobs                                                                              |
|----------------|---------------------------------------------------|------------------------------------------------------------------------------------|
| libcurl        | `BundledCurl.cmake` (default) or system `find_package(CURL REQUIRED)` | `-DMOOCODE_BUNDLED_CURL=OFF` falls back to distro libcurl.                |
| nlohmann_json  | system, `find_package`                            | required                                                                           |
| FTXUI v6.1.9   | `FetchContent` from pinned tarball, SHA256-pinned | examples/docs/tests/install all forced OFF; see "FTXUI warnings" below.            |

The libcurl option is the only user-facing build toggle:
- `MOOCODE_BUNDLED_CURL=ON` (default): include `cmake/BundledCurl.cmake`,
  which builds a feature-stripped, fully static libcurl from source and
  exposes it as the imported target `CURL::libcurl`.
- `MOOCODE_BUNDLED_CURL=OFF`: `find_package(CURL REQUIRED)` is used
  instead; the system target (e.g. `CURL::libcurl` from a distro
  `CURLConfig.cmake`) is linked in `src/CMakeLists.txt`.

### Libraries, executable, tests
- `add_subdirectory(src)` defines the project targets (including
  `agent_http`, which links `CURL::libcurl`).
- When the bundled curl is in use, `add_dependencies(agent_http
  ${BUNDLED_CURL_TARGET})` is added; `BUNDLED_CURL_TARGET` is exported
  from `BundledCurl.cmake` (currently `curl_ep`) so the link step waits
  on the `ExternalProject` archive being produced.
- `include(CTest)` enables `BUILD_TESTING` (default `ON`);
  `add_subdirectory(tests)` is gated on it.

### Warnings
- `add_compile_options(-Wall -Wextra -Wpedantic)` is placed **after**
  `FetchContent_MakeAvailable(ftxui)`. Per the comment, this scopes the
  flags to moocode's own targets only — FTXUI's own sources are built
  with their upstream flags and are not expected to be flag-clean
  against `-Wpedantic`.
- The top-level file does **not** set `-Werror`; any target that wants
  it does so in `src/CMakeLists.txt`.

---

## `cmake/BundledCurl.cmake`

### Why it exists
`moocode` only does HTTPS GET/POST with custom headers, a write callback,
a timeout, and gzip/deflate decompression (`src/agent/http.cpp`). A
distro `libcurl.so` drags in ~20 shared objects for protocols and
features the agent never uses: HTTP/3, QUIC, SSH, IMAP/SMTP/POP3/RTSP,
LDAP, GSSAPI/krb5, brotli, zstd, IDN2, PSL, …

This module replaces that with a **fully static** build of exactly
HTTP+HTTPS+zlib+HTTP/2+OpenSSL, exposed as the imported target
`CURL::libcurl` so the rest of the tree is untouched
(`src/CMakeLists.txt` keeps linking `CURL::libcurl`). The only runtime
shared deps left are the C/C++ runtime (`libc`, `libstdc++`, `libm`,
`libgcc`).

### What it builds

```
zlib  ->  nghttp2  ->  OpenSSL  ->  curl
```

All four are built as static archives via `ExternalProject_Add` and
installed under `${CMAKE_BINARY_DIR}/_static_curl`. The order is
encoded with `DEPENDS` (curl depends on the other three) and matched by
the link order on `CURL::libcurl`'s `INTERFACE` libraries.

### Pinned versions and integrity

| Component | Version     | Source                                                              |
|-----------|-------------|---------------------------------------------------------------------|
| zlib      | 1.3.1       | `https://github.com/madler/zlib/releases/download/v1.3.1/zlib-1.3.1.tar.gz` |
| nghttp2   | 1.65.0      | `https://github.com/nghttp2/nghttp2/releases/download/v1.65.0/nghttp2-1.65.0.tar.xz` |
| OpenSSL   | 3.5.0       | `https://github.com/openssl/openssl/releases/download/openssl-3.5.0/openssl-3.5.0.tar.gz` |
| curl      | 8.15.0      | `https://curl.se/download/curl-8.15.0.tar.xz`                       |

Every tarball is downloaded with `URL_HASH SHA256=…`. The hashes are
listed in the module for review. Tarballs are cached in
`${PROJECT_SOURCE_DIR}/.deps-cache` and reused across rebuilds via
`DOWNLOAD_DIR` + `UPDATE_DISCONNECTED ON`.

### Common configure flags
All CMake-based sub-builds (zlib, nghttp2, curl) get:

```
-DCMAKE_POLICY_VERSION_MINIMUM=3.5   # sub-builds use a floor below CMake 4
-DCMAKE_BUILD_TYPE=Release
-DCMAKE_POSITION_INDEPENDENT_CODE=ON
-DBUILD_SHARED_LIBS=OFF
-DCMAKE_INSTALL_PREFIX=${_bc_install}
-DCMAKE_INSTALL_LIBDIR=lib           # never lib64 -> stable paths
```

The `CMAKE_POLICY_VERSION_MINIMUM=3.5` override is a deliberate choice:
it avoids patching upstream `cmake_minimum_required()` values without
silently letting the new CMake 4 policies apply.

### Per-component notes

#### zlib
Powers `CURLOPT_ACCEPT_ENCODING` (gzip/deflate). `BUILD_BYPRODUCTS
${_bc_lib}/libz.a`.

#### nghttp2
HTTP/2 support, library only.
- `ENABLE_LIB_ONLY=ON`, `BUILD_STATIC_LIBS=ON` (the latter gates `.a`
  output on 1.65, not `ENABLE_STATIC_LIB`),
- `ENABLE_DOC=OFF`, `BUILD_TESTING=OFF`,
- `BUILD_BYPRODUCTS ${_bc_lib}/libnghttp2.a`.

#### OpenSSL
Built **in source** with Perl `Configure` (not CMake):
- `--prefix=${_bc_install}`, `--openssldir=${_bc_install}/ssl`,
  `--libdir=lib`,
- `no-shared no-apps no-tests no-docs no-legacy`,
- target triplet `linux-x86_64` (hard-coded),
- `make -j${_bc_nproc}` for the build, `make install_sw` for install
  (skips docs, matches `no-docs`),
- `BUILD_BYPRODUCTS ${_bc_lib}/libssl.a ${_bc_lib}/libcrypto.a`,
- `ProcessorCount` is used to size `-j`; falls back to `2` if it
  returns `0`.

Known limits: the triplet is x86_64-Linux only. Cross-compiling or
other targets will need a Configure line edit.

#### CA trust store
A statically linked curl has no distro default. The module probes, in
order, for the first existing file:
- `/etc/ssl/certs/ca-certificates.crt` (Debian/Ubuntu/Arch)
- `/etc/pki/tls/certs/ca-bundle.crt` (RHEL/Fedora)
- `/etc/ssl/ca-bundle.pem`, `/etc/ssl/cert.pem`

`/etc/ssl/certs` is also probed as `CURL_CA_PATH`. If neither is found,
a `WARNING` is emitted; HTTPS will then need `CURLOPT_CAINFO` or
`SSL_CERT_FILE` at runtime.

### curl: what is turned off
The protocol list is reduced to HTTP/HTTPS only. Every other
`CURL_DISABLE_*` is `ON`:

`DICT FILE FTP GOPHER IMAP LDAP LDAPS MQTT POP3 RTSP SMB SMTP TELNET
TFTP WEBSOCKETS`

Also off: `USE_NGHTTP3`, `USE_NGTCP2`, `USE_QUICHE` (HTTP/3 / QUIC),
`CURL_USE_LIBSSH2`, `CURL_USE_LIBSSH`, `CURL_USE_LIBPSL`, `USE_LIBIDN2`,
`CURL_BROTLI`, `CURL_ZSTD`, `CURL_USE_GSSAPI`, `USE_WIN32_LDAP`,
`ENABLE_UNIX_SOCKETS`.

On: `CURL_USE_OPENSSL`, `OPENSSL_USE_STATIC_LIBS`, `CURL_ZLIB` +
`ZLIB_USE_STATIC_LIBS`, `USE_NGHTTP2`. `BUILD_CURL_EXE=OFF`,
`BUILD_STATIC_LIBS=ON`, `BUILD_TESTING=OFF`.

### Imported target: `CURL::libcurl`

```cmake
add_library(CURL::libcurl STATIC IMPORTED GLOBAL)
IMPORTED_LOCATION             ${_bc_lib}/libcurl.a
INTERFACE_INCLUDE_DIRECTORIES ${_bc_inc}
INTERFACE_COMPILE_DEFINITIONS CURL_STATICLIB
INTERFACE_LINK_LIBRARIES      libnghttp2 libssl libcrypto libz
                              Threads::Threads
                              ${CMAKE_DL_LIBS}
```

Notes:
- `INTERFACE_INCLUDE_DIRECTORIES` must exist at configure time, so
  `file(MAKE_DIRECTORY ${_bc_inc})` is run up front. The `.a` files
  themselves are produced later by the `ExternalProject` steps.
- The **link order is significant**: libcurl pulls in nghttp2, which
  pulls in ssl, which pulls in crypto, and z is needed for
  `CURLOPT_ACCEPT_ENCODING`. `Threads::Threads` and `${CMAKE_DL_LIBS}`
  (i.e. `dl` on Linux) are required by static OpenSSL.
- `CURL_STATICLIB` must be defined when including `<curl/curl.h>` from a
  static build; the `INTERFACE_COMPILE_DEFINITIONS` makes this
  automatic for every consumer.

### Coordinating with the rest of the build
`BundledCurl.cmake` does **not** declare a dependency on the consumer
target itself. The root `CMakeLists.txt` is responsible:

```cmake
if(MOOCODE_BUNDLED_CURL)
  add_dependencies(agent_http ${BUNDLED_CURL_TARGET})
endif()
```

`BUNDLED_CURL_TARGET` is set to `curl_ep` (the umbrella `ExternalProject`
target — depending on it transitively pulls in zlib, nghttp2, and
OpenSSL via `DEPENDS`).

### Variables exported
| Variable             | Set to          | Purpose                                                        |
|----------------------|-----------------|----------------------------------------------------------------|
| `CURL::libcurl`      | imported target | Linked by `src/CMakeLists.txt` for `agent_http`.               |
| `BUNDLED_CURL_TARGET`| `curl_ep`       | Argument to `add_dependencies(agent_http …)`.                  |

### How to wipe and rebuild
The four static archives and their headers live in
`${CMAKE_BINARY_DIR}/_static_curl/`. Removing the build directory
(starting from `${CMAKE_BINARY_DIR}/_static_curl/`) is the cleanest
reset. Pre-seeded tarballs in `.deps-cache/` are reused; remove them
too to force a re-download.
