#ifndef MOOCODE_TUI_PLATFORM_HPP
#define MOOCODE_TUI_PLATFORM_HPP

// Platform glue for the interactive TUI that must touch OS APIs directly:
// the native clipboard and console-mode setup. Kept behind this header so
// tui.cpp (which pulls in FTXUI) never includes <windows.h> and its macros.
// POSIX builds get trivial no-op/false implementations; Windows builds get the
// Win32 versions. Declarations use only stdlib types so this header is clean.

#include <string_view>

namespace moocode::tui_platform {

// Copy UTF-8 `text` to the OS clipboard via a native API. Returns false when no
// native clipboard is available (POSIX): the caller then falls back to OSC 52.
// On Windows this uses the Win32 clipboard, which works in both cmd.exe and
// Windows Terminal (neither needs OSC 52, which legacy conhost ignores anyway).
bool native_clipboard_copy(std::string_view text);

// RAII console configuration. On Windows the constructor switches the console
// to the UTF-8 code page and enables virtual-terminal processing (so FTXUI's
// ANSI output renders), saving the prior state; the destructor restores it.
// No-op on POSIX. One instance should live for the duration of the TUI.
class ConsoleGuard {
public:
    ConsoleGuard();
    ~ConsoleGuard();
    ConsoleGuard(const ConsoleGuard&) = delete;
    ConsoleGuard& operator=(const ConsoleGuard&) = delete;

private:
#ifdef _WIN32
    bool active_ = false;
    unsigned long saved_out_mode_ = 0;  // DWORD
    unsigned long saved_in_mode_ = 0;
    unsigned int saved_out_cp_ = 0;     // UINT
    unsigned int saved_in_cp_ = 0;
#endif
};

}  // namespace moocode::tui_platform

#endif  // MOOCODE_TUI_PLATFORM_HPP
