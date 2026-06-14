#ifndef MOOCODE_TUI_TEXT_HPP
#define MOOCODE_TUI_TEXT_HPP

// Text sanitizer for the FTXUI render model. FTXUI repaints the terminal by
// overwriting the previous frame in place, assuming its computed glyph widths
// match the terminal's. Codepoints the two disagree on (zero-width joiners,
// variation selectors, raw escape sequences, tabs) shift every cell after
// them on that row — drifting pane separators and leaving stale-cell
// artifacts. Sanitizing at the render-model boundary keeps the grid honest.

#include <string>
#include <string_view>

namespace moocode {

// Make `s` width-predictable for the TUI grid: strips ANSI/OSC escape
// sequences and C0/C1 controls (newline survives), expands tabs to four
// spaces, and drops zero-width or presentation-changing codepoints (Unicode
// Default_Ignorables incl. ZWJ + variation selectors, keycap, skin tones).
// Total; invalid UTF-8 bytes are dropped, output is valid UTF-8.
std::string sanitize_tui_text(std::string_view s);

}  // namespace moocode

#endif  // MOOCODE_TUI_TEXT_HPP
