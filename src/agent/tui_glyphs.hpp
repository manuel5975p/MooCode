#ifndef MOOCODE_TUI_GLYPHS_HPP
#define MOOCODE_TUI_GLYPHS_HPP

// Decorative-glyph policy for the TUI. The interface uses box-drawing chars,
// arrows, marks and emoji as chrome (separators, fold carets, status marks,
// attach/image badges). Modern terminals (Windows Terminal, every Unix term)
// render them; the legacy Windows console (conhost with a raster/Consolas font)
// cannot — emoji in particular show as tofu. In ASCII mode every decorative
// codepoint is transliterated to a plain-ASCII stand-in.
//
// Mode is auto-detected once (see detect_glyph_mode): ASCII on a legacy Windows
// console, Unicode elsewhere, overridable via $MOOCODE_GLYPHS / $MOOCODE_ASCII.
// Plain text (prose, code, file contents, the Swiss-German spinner phrases) is
// never touched — only the fixed set of chrome codepoints below.

#include <cstdint>
#include <string>
#include <string_view>

namespace moocode {

enum class GlyphMode { Unknown = 0, Unicode, Ascii };

// Decide the glyph mode from the environment (called once, then cached):
//   $MOOCODE_GLYPHS=ascii|unicode  — explicit override (wins)
//   $MOOCODE_ASCII=1               — force ASCII
//   Windows without $WT_SESSION    — ASCII (legacy conhost)
//   otherwise                      — Unicode
GlyphMode detect_glyph_mode();

// The active mode (lazily resolved from detect_glyph_mode on first use).
GlyphMode glyph_mode();

// Override the active mode. Set once at TUI startup; also used by tests.
void set_glyph_mode(GlyphMode mode);

// ASCII stand-in for a decorative codepoint, or nullptr if `cp` is not one of
// the chrome glyphs (so it should be kept verbatim).
const char* ascii_fold(std::uint32_t cp);

// Transliterate the decorative glyphs in `s` to ASCII when the active mode is
// ASCII; in Unicode mode `s` is returned unchanged. Safe on arbitrary UTF-8.
std::string glyphify(std::string_view s);

}  // namespace moocode

#endif  // MOOCODE_TUI_GLYPHS_HPP
