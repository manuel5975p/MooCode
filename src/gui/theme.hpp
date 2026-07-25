#ifndef MOOCODE_GUI_THEME_HPP
#define MOOCODE_GUI_THEME_HPP

// Colour palettes for the Qt GUI, plus the widget-chrome stylesheet derived
// from them. Deliberately Qt-free: a palette is a bag of "#rrggbb" strings and
// the stylesheet builder returns a plain std::string, so the whole layer is
// pure and unit-testable without a display (tests/test_gui_theme.cpp). The
// widget layer does the QString/QColor conversions.
//
// The four schemes are the ones the TUI already names — SyntaxTheme
// None/Default/Mono/Vivid, resolved from settings.theme via
// syntax_theme_from_name — extended from "colours for fenced code" to a full
// prose palette, since the GUI renders headings, quotes, tables and links the
// terminal never styled.
//
// Note on rendering: Qt applies QTextDocument::setDefaultStyleSheet only to
// setHtml(), NOT to setMarkdown() — a markdown-imported document comes out with
// no explicit foregrounds at all. So prose colours here are applied
// programmatically by markdown_view.cpp walking the document, not as CSS. Only
// window_stylesheet() is a real Qt Style Sheet.

#include <string>

#include "agent/syntax_highlight.hpp"  // TokenCategory
#include "agent/types.hpp"             // SyntaxTheme

namespace moocode::gui {

// A complete colour scheme. Every field is a Qt colour literal ("#1b1f26").
// The code_* fields mirror the TUI's token_style() choices so a fenced block
// reads the same in both frontends; the rest are prose-only additions.
struct GuiTheme {
    // Window chrome.
    std::string window_bg;
    std::string panel_bg;       // chat surface behind the message cards
    std::string header_bg;
    std::string border;         // hairlines: header rule, input outline
    std::string accent;         // assistant rule, focus ring, primary button
    std::string accent_text;    // text drawn on top of `accent`
    std::string user_bubble_bg;
    std::string muted;          // status line, collapsed reasoning, placeholders

    // Prose.
    std::string text;
    std::string heading;
    std::string link;
    std::string quote_text;
    std::string rule;           // table gridlines
    std::string inline_code_bg;
    std::string inline_code_text;
    std::string code_bg;        // fenced block background
    std::string table_head_bg;

    // Fenced-code token colours. Plain doubles as the fallback for any
    // category a scheme chooses not to style.
    std::string code_plain;
    std::string code_keyword;
    std::string code_type;
    std::string code_builtin;
    std::string code_string;
    std::string code_number;
    std::string code_comment;
    std::string code_preproc;
    std::string code_variable;

    // Colour for `cat`, falling back to code_plain. Total.
    const std::string& token_color(TokenCategory cat) const;
};

// The palette for `t`. Total: every SyntaxTheme value maps to a scheme.
// SyntaxTheme::None yields a scheme whose code_* are all code_plain — "none"
// has always meant "don't colourise code", not "no theme", so prose stays
// styled.
const GuiTheme& gui_theme(SyntaxTheme t);

// Qt Style Sheet for the window chrome (panels, input box, buttons, scrollbar,
// menus), applied to the QApplication. Widget stylesheets are honoured by Qt;
// only the *document* stylesheet is not (see the note above).
std::string window_stylesheet(const GuiTheme& th);

}  // namespace moocode::gui

#endif  // MOOCODE_GUI_THEME_HPP
