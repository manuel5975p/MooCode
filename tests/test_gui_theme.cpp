// Tests for the GUI's pure colour layer. No Qt, no display: the whole point of
// keeping GuiTheme Qt-free is that the palettes and the generated stylesheet
// can be checked in an ordinary headless unit test.

#include "gui/theme.hpp"

#include <string>
#include <vector>

#include "agent/types.hpp"  // SyntaxTheme, syntax_theme_names

#include "test_harness.hpp"

using namespace moocode;
using namespace moocode::gui;

namespace {

bool looks_like_hex_colour(const std::string& s) {
    if (s.size() != 7 || s[0] != '#') return false;
    for (std::size_t i = 1; i < s.size(); ++i) {
        const char c = s[i];
        const bool hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
                         (c >= 'A' && c <= 'F');
        if (!hex) return false;
    }
    return true;
}

std::vector<std::string> all_fields(const GuiTheme& t) {
    return {t.window_bg,     t.panel_bg,       t.header_bg,        t.border,
            t.accent,        t.accent_text,    t.user_bubble_bg,   t.muted,
            t.text,          t.heading,        t.link,             t.quote_text,
            t.rule,          t.inline_code_bg, t.inline_code_text, t.code_bg,
            t.table_head_bg, t.code_plain,     t.code_keyword,     t.code_type,
            t.code_builtin,  t.code_string,    t.code_number,      t.code_comment,
            t.code_preproc,  t.code_variable};
}

const std::vector<SyntaxTheme>& all_themes() {
    static const std::vector<SyntaxTheme> v = {SyntaxTheme::Default, SyntaxTheme::Mono,
                                               SyntaxTheme::Vivid, SyntaxTheme::None};
    return v;
}

}  // namespace

TEST("every theme defines every colour, and they are all well-formed") {
    for (SyntaxTheme t : all_themes()) {
        const GuiTheme& th = gui_theme(t);
        for (const std::string& c : all_fields(th)) {
            CHECK(c.empty() == false);
            CHECK(looks_like_hex_colour(c));
        }
    }
}

TEST("gui_theme covers every name the theme table advertises") {
    // A name reachable from settings.theme with no palette behind it would fall
    // back silently, which is exactly the drift the shared table exists to stop.
    for (const std::string& name : syntax_theme_names()) {
        auto id = syntax_theme_from_name(name);
        CHECK(id.has_value());
        CHECK(gui_theme(*id).text.empty() == false);
    }
}

TEST("token_color maps each category and falls back to plain") {
    const GuiTheme& th = gui_theme(SyntaxTheme::Default);
    CHECK_EQ(th.token_color(TokenCategory::Keyword), th.code_keyword);
    CHECK_EQ(th.token_color(TokenCategory::Type), th.code_type);
    CHECK_EQ(th.token_color(TokenCategory::Builtin), th.code_builtin);
    CHECK_EQ(th.token_color(TokenCategory::String), th.code_string);
    CHECK_EQ(th.token_color(TokenCategory::Number), th.code_number);
    CHECK_EQ(th.token_color(TokenCategory::Comment), th.code_comment);
    CHECK_EQ(th.token_color(TokenCategory::Preproc), th.code_preproc);
    CHECK_EQ(th.token_color(TokenCategory::Variable), th.code_variable);
    CHECK_EQ(th.token_color(TokenCategory::Plain), th.code_plain);
}

TEST("the None theme leaves code uncoloured but still styles prose") {
    const GuiTheme& th = gui_theme(SyntaxTheme::None);
    // "none" has always meant "don't colourise code", not "no theme".
    CHECK_EQ(th.token_color(TokenCategory::Keyword), th.code_plain);
    CHECK_EQ(th.token_color(TokenCategory::String), th.code_plain);
    CHECK_EQ(th.token_color(TokenCategory::Comment), th.code_plain);
    CHECK(th.heading != th.text);
    CHECK(th.link.empty() == false);
}

TEST("Mono is greyscale everywhere") {
    // Every channel equal in every colour is what makes it monochrome; a stray
    // hue would show up as an off-grey in code.
    const GuiTheme& th = gui_theme(SyntaxTheme::Mono);
    for (const std::string& c : all_fields(th)) {
        const std::string r = c.substr(1, 2), g = c.substr(3, 2), b = c.substr(5, 2);
        CHECK_EQ(r, g);
        CHECK_EQ(g, b);
    }
}

TEST("themes are distinct objects, returned by stable reference") {
    CHECK(&gui_theme(SyntaxTheme::Default) == &gui_theme(SyntaxTheme::Default));
    CHECK(&gui_theme(SyntaxTheme::Default) != &gui_theme(SyntaxTheme::Vivid));
    CHECK(&gui_theme(SyntaxTheme::Mono) != &gui_theme(SyntaxTheme::None));
}

TEST("window_stylesheet embeds the palette and names the styled widgets") {
    const GuiTheme& th = gui_theme(SyntaxTheme::Vivid);
    const std::string css = window_stylesheet(th);
    CHECK(css.empty() == false);
    // The scheme's own colours, not another theme's.
    CHECK(css.find(th.accent) != std::string::npos);
    CHECK(css.find(th.window_bg) != std::string::npos);
    CHECK(css.find(th.border) != std::string::npos);
    // The object names the widget layer sets.
    for (const char* id : {"#mooHeader", "#mooInput", "#mooSend", "#mooScroll",
                           "#mooUserCard", "#mooRole", "#mooStatus"})
        CHECK(css.find(id) != std::string::npos);
    // Balanced rule blocks — a stray brace silently kills every later rule.
    std::size_t open = 0, close = 0;
    for (char c : css) {
        if (c == '{') ++open;
        if (c == '}') ++close;
    }
    CHECK_EQ(open, close);
}

TEST("each theme yields its own stylesheet") {
    CHECK(window_stylesheet(gui_theme(SyntaxTheme::Default)) !=
          window_stylesheet(gui_theme(SyntaxTheme::Mono)));
    CHECK(window_stylesheet(gui_theme(SyntaxTheme::Vivid)) !=
          window_stylesheet(gui_theme(SyntaxTheme::Mono)));
}
