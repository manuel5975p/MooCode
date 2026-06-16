#ifndef MOOCODE_PLATFORM_HPP
#define MOOCODE_PLATFORM_HPP

// Tiny cross-platform shims that paper over the POSIX vs. Windows (MinGW-w64)
// differences shared across translation units. Header-only; depends only on the
// C++ standard library so it stays free of <windows.h> in the common path.

#include <cstdlib>
#include <string>

namespace moocode {

// The current user's home directory, or "" if none is discoverable.
// POSIX: $HOME. Windows: $HOME (msys/git-bash) first, then %USERPROFILE%,
// %HOMEDRIVE%%HOMEPATH%, finally %APPDATA% so native cmd.exe still resolves one.
inline std::string user_home() {
    if (const char* h = std::getenv("HOME"); h && *h) return h;
#ifdef _WIN32
    if (const char* up = std::getenv("USERPROFILE"); up && *up) return up;
    const char* hd = std::getenv("HOMEDRIVE");
    const char* hp = std::getenv("HOMEPATH");
    if (hd && *hd && hp && *hp) return std::string(hd) + hp;
    if (const char* ad = std::getenv("APPDATA"); ad && *ad) return ad;
#endif
    return {};
}

// $PATH list separator: ':' on POSIX, ';' on Windows.
inline constexpr char kPathListSep =
#ifdef _WIN32
    ';';
#else
    ':';
#endif

// The console device that always refers to the controlling terminal even when
// stdin is redirected: "/dev/tty" on POSIX, "CONIN$" on Windows.
inline constexpr const char* kConsoleInDevice =
#ifdef _WIN32
    "CONIN$";
#else
    "/dev/tty";
#endif

}  // namespace moocode

#endif  // MOOCODE_PLATFORM_HPP
