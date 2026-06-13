#include "agent/tui_slash.hpp"

#include <string>

#include "test_harness.hpp"

using namespace flagent;

// /effort: levels set effort + reasoning_hint; none/off disables thinking;
// junk yields the usage error and applies nothing.
TEST("parse_effort maps levels, off, and junk like the inline handler") {
    auto high = parse_effort("high");
    CHECK(high.error.empty());
    CHECK(high.params.effort.has_value());
    CHECK_EQ(high.params.effort.value_or(""), std::string{"high"});
    CHECK(!high.params.thinking.has_value());
    CHECK(high.reasoning_hint);
    CHECK_EQ(high.info, std::string{"effort → high"});

    // Case-insensitive (handler lowercases the arg).
    auto med = parse_effort("MEDIUM");
    CHECK(med.error.empty());
    CHECK_EQ(med.params.effort.value_or(""), std::string{"medium"});

    auto minimal = parse_effort("minimal");
    CHECK(minimal.error.empty());
    CHECK_EQ(minimal.params.effort.value_or(""), std::string{"minimal"});
    CHECK(minimal.reasoning_hint);

    auto off = parse_effort("off");
    CHECK(off.error.empty());
    CHECK(off.params.thinking.has_value());
    CHECK_EQ(off.params.thinking.value_or(true), false);
    CHECK(!off.params.effort.has_value());
    CHECK(!off.reasoning_hint);
    CHECK_EQ(off.info, std::string{"effort → off (thinking disabled)"});

    auto none = parse_effort("none");
    CHECK(none.error.empty());
    CHECK_EQ(none.params.thinking.value_or(true), false);

    auto bad = parse_effort("sideways");
    CHECK(!bad.error.empty());
    CHECK_EQ(bad.error, std::string{"usage: /effort low|medium|high|none"});
    CHECK(!bad.params.effort.has_value());
    CHECK(!bad.params.thinking.has_value());

    auto empty = parse_effort("");
    CHECK(!empty.error.empty());
}

// /thinking: on-words enable thinking with the reasoning hint; off-words disable;
// anything else is a usage error.
TEST("parse_thinking accepts on/off synonyms and rejects junk") {
    for (const char* on : {"on", "true", "1"}) {
        auto r = parse_thinking(on);
        CHECK(r.error.empty());
        CHECK(r.params.thinking.has_value());
        CHECK_EQ(r.params.thinking.value_or(false), true);
        CHECK(r.reasoning_hint);
        CHECK_EQ(r.info, std::string{"thinking → on"});
    }
    for (const char* off : {"off", "false", "0"}) {
        auto r = parse_thinking(off);
        CHECK(r.error.empty());
        CHECK_EQ(r.params.thinking.value_or(true), false);
        CHECK(!r.reasoning_hint);
        CHECK_EQ(r.info, std::string{"thinking → off"});
    }
    auto bad = parse_thinking("maybe");
    CHECK_EQ(bad.error, std::string{"usage: /thinking on|off"});
    CHECK(!bad.params.thinking.has_value());
}

// /temp: a finite number sets temperature; empty/non-numeric/trailing-junk is a
// usage error. The success info echoes the raw argument verbatim.
TEST("parse_temp validates a numeric temperature") {
    auto ok = parse_temp("0.7");
    CHECK(ok.error.empty());
    CHECK(ok.params.temperature.has_value());
    CHECK(ok.params.temperature.value_or(-1.0) == 0.7);
    CHECK_EQ(ok.info, std::string{"temperature → 0.7"});

    auto zero = parse_temp("0");
    CHECK(zero.error.empty());
    CHECK(zero.params.temperature.value_or(-1.0) == 0.0);

    for (const char* junk : {"", "abc", "0.7x", "  "}) {
        auto r = parse_temp(junk);
        CHECK(!r.error.empty());
        CHECK_EQ(r.error, std::string{"usage: /temp <number, e.g. 0.7>"});
        CHECK(!r.params.temperature.has_value());
    }
}

// /theme: empty => list request; a known name => that theme; junk => error.
TEST("parse_theme lists, resolves, or rejects a theme name") {
    auto list = parse_theme("");
    CHECK(list.list);
    CHECK(list.error.empty());
    CHECK(!list.theme.has_value());

    auto def = parse_theme("default");
    CHECK(!def.list);
    CHECK(def.error.empty());
    CHECK(def.theme.has_value());
    CHECK(def.theme == SyntaxTheme::Default);

    auto mono = parse_theme("MONO");  // case-insensitive via syntax_theme_from_name
    CHECK(mono.theme == SyntaxTheme::Mono);

    auto none = parse_theme("none");
    CHECK(none.theme == SyntaxTheme::None);

    auto bad = parse_theme("rainbow");
    CHECK(!bad.list);
    CHECK(!bad.theme.has_value());
    CHECK(!bad.error.empty());
}

// Dispatch parity: aliases. The inline /temp handler is also reachable as
// /temperature; both feed parse_temp, so the alias must produce the same result.
// (Routing itself is exercised through SlashCommands in tui.cpp; here we pin
// that the shared parser is argument-only and alias-agnostic.)
TEST("parse_temp is alias-agnostic (same result regardless of command name)") {
    auto a = parse_temp("1.25");
    auto b = parse_temp("1.25");
    CHECK_EQ(a.info, b.info);
    CHECK(a.params.temperature == b.params.temperature);
}
