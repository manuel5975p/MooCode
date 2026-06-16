#include "agent/tui_platform.hpp"

// POSIX build: there is no portable native clipboard, and FTXUI configures the
// terminal itself, so both pieces are inert. Clipboard copy is handled by the
// OSC 52 path in tui.cpp; this returning false routes the caller there.

#ifndef _WIN32

namespace moocode::tui_platform {

bool native_clipboard_copy(std::string_view) { return false; }

ConsoleGuard::ConsoleGuard() = default;
ConsoleGuard::~ConsoleGuard() = default;

}  // namespace moocode::tui_platform

#endif  // !_WIN32
