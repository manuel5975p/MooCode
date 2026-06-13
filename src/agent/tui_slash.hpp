#ifndef FLAGENT_TUI_SLASH_HPP
#define FLAGENT_TUI_SLASH_HPP

// Pure decision logic for the TUI's generation-control slash commands
// (/effort, /thinking, /temp, /theme). Each function parses one argument and
// returns what the handler should do — generation params to apply, info/error
// lines to push, an optional theme to set — without touching the render state,
// the provider, the mutex, or any I/O. The thin registered handler in tui.cpp
// applies the result (locks, set_params, push_info/push_error, persist). This
// keeps the parse/validate decisions reviewable and unit-testable in isolation.

#include <optional>
#include <string>
#include <string_view>

#include "agent/provider.hpp"  // GenerationParams
#include "agent/tui.hpp"       // SyntaxTheme

namespace flagent {

// Outcome of parsing a generation-control argument. When `error` is non-empty
// the handler pushes it (usage line) and applies nothing. Otherwise it applies
// `params`, pushes `info`, and — only when `reasoning_hint` is true — may append
// the "model may ignore reasoning controls" note for OpenAI non-reasoning models.
struct GenControlResult {
    std::string error;              // non-empty => usage error, apply nothing
    std::string info;               // success note to push
    GenerationParams params;        // params to apply on success
    bool reasoning_hint = false;    // success enabled reasoning => maybe warn
};

// Parse /effort: none|off => thinking=false; low|medium|high|minimal => effort=a;
// anything else => usage error. reasoning_hint set when an effort level was set.
GenControlResult parse_effort(std::string_view arg);

// Parse /thinking: on|true|1 / off|false|0 => thinking flag; else usage error.
// reasoning_hint set when thinking was turned on.
GenControlResult parse_thinking(std::string_view arg);

// Parse /temp: a finite number => temperature; empty/non-numeric => usage error.
// Never sets reasoning_hint.
GenControlResult parse_temp(std::string_view arg);

// Outcome of parsing a /theme argument. Exactly one of {listing, error, theme}
// is meaningful: empty arg => `listing` requested (handler builds the list,
// since it needs the active theme); unknown name => `error`; else `theme` set.
struct ThemeResult {
    bool list = false;                 // empty arg: handler should list themes
    std::string error;                 // non-empty => unknown-theme error
    std::optional<SyntaxTheme> theme;  // set => apply + persist this theme
};

// Parse /theme: empty => list; a known name => that theme; else an error.
ThemeResult parse_theme(std::string_view arg);

}  // namespace flagent

#endif  // FLAGENT_TUI_SLASH_HPP
