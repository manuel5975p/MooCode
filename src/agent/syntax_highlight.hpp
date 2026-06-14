#ifndef MOOCODE_AGENT_SYNTAX_HIGHLIGHT_HPP
#define MOOCODE_AGENT_SYNTAX_HIGHLIGHT_HPP

// Pure, terminal-agnostic syntax highlighter for fenced code blocks. Produces
// per-line token spans so a UI layer can map categories to colours; it knows
// nothing about FTXUI. Exists so the lexing logic is unit-testable in isolation
// from the screen.

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace moocode {

// Languages we currently colourise. None means "leave as plain text".
enum class Language : std::uint8_t { None, Bash, Python, Cpp };

// Lexical role of a span, used by the renderer to pick a colour.
enum class TokenCategory : std::uint8_t {
    Plain,
    Keyword,
    Type,
    Builtin,
    String,
    Number,
    Comment,
    Preproc,
    Variable,
};

// One contiguous run of characters sharing a category. Never spans a newline.
struct HlSpan {
    std::string text;
    TokenCategory category = TokenCategory::Plain;
};

// Map a fence info-string tag ("cpp", "py", "sh", ...) to a Language.
// Case-insensitive; returns Language::None for anything unsupported.
Language language_from_tag(std::string_view tag);

// Tokenise a whole (possibly multi-line) code block. Returns one span list per
// input line (split on '\n'; a trailing newline does not add an empty line).
// Lexer state (block comments, triple-quoted strings) carries across lines.
// lang == None yields a single Plain span per line.
std::vector<std::vector<HlSpan>> highlight_block(std::string_view code,
                                                 Language lang);

}  // namespace moocode

#endif  // MOOCODE_AGENT_SYNTAX_HIGHLIGHT_HPP
