#include "agent/tui_platform.hpp"

// Windows build: native clipboard via the Win32 clipboard API and console
// setup (UTF-8 code page + virtual-terminal processing). This is the ONE TU
// that includes <windows.h>; keeping it isolated spares tui.cpp the macro soup.

#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <string>
#include <vector>

namespace moocode::tui_platform {

bool native_clipboard_copy(std::string_view text) {
    // UTF-8 -> UTF-16 for CF_UNICODETEXT (the universally-honoured format).
    int wlen = ::MultiByteToWideChar(CP_UTF8, 0, text.data(),
                                     static_cast<int>(text.size()), nullptr, 0);
    if (wlen < 0) return false;

    // GlobalAlloc a NUL-terminated UTF-16 buffer; ownership passes to the
    // clipboard on success, so we must NOT free it after SetClipboardData wins.
    HGLOBAL h = ::GlobalAlloc(GMEM_MOVEABLE,
                              (static_cast<SIZE_T>(wlen) + 1) * sizeof(wchar_t));
    if (!h) return false;
    auto* dst = static_cast<wchar_t*>(::GlobalLock(h));
    if (!dst) { ::GlobalFree(h); return false; }
    if (wlen > 0)
        ::MultiByteToWideChar(CP_UTF8, 0, text.data(),
                              static_cast<int>(text.size()), dst, wlen);
    dst[wlen] = L'\0';
    ::GlobalUnlock(h);

    if (!::OpenClipboard(nullptr)) { ::GlobalFree(h); return false; }
    ::EmptyClipboard();
    bool ok = ::SetClipboardData(CF_UNICODETEXT, h) != nullptr;
    ::CloseClipboard();
    if (!ok) ::GlobalFree(h);  // still ours if the clipboard refused it
    return ok;
}

ConsoleGuard::ConsoleGuard() {
    // Output code page + VT processing only. Input mode is left to FTXUI, which
    // installs and restores its own console input configuration around its event
    // loop; touching it here would just fight that. The UTF-8 output code page
    // is the part FTXUI does not guarantee, and it is what makes our UTF-8 byte
    // stream render correctly (Unicode glyphs in Windows Terminal).
    HANDLE out = ::GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD out_mode = 0;
    // Only engage when stdout is a real console (not a pipe/file).
    if (out == INVALID_HANDLE_VALUE || !::GetConsoleMode(out, &out_mode)) return;

    saved_out_mode_ = out_mode;
    saved_out_cp_ = ::GetConsoleOutputCP();
    saved_in_cp_ = ::GetConsoleCP();

    ::SetConsoleOutputCP(CP_UTF8);
    ::SetConsoleCP(CP_UTF8);
    ::SetConsoleMode(out, out_mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
    active_ = true;
    (void)saved_in_mode_;  // reserved; input mode is not modified here
}

ConsoleGuard::~ConsoleGuard() {
    if (!active_) return;
    HANDLE out = ::GetStdHandle(STD_OUTPUT_HANDLE);
    if (out != INVALID_HANDLE_VALUE) ::SetConsoleMode(out, saved_out_mode_);
    ::SetConsoleOutputCP(saved_out_cp_);
    ::SetConsoleCP(saved_in_cp_);
}

}  // namespace moocode::tui_platform

#endif  // _WIN32
