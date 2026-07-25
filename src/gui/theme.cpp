#include "gui/theme.hpp"

namespace moocode::gui {

const std::string& GuiTheme::token_color(TokenCategory cat) const {
    switch (cat) {
        case TokenCategory::Keyword:  return code_keyword;
        case TokenCategory::Type:     return code_type;
        case TokenCategory::Builtin:  return code_builtin;
        case TokenCategory::String:   return code_string;
        case TokenCategory::Number:   return code_number;
        case TokenCategory::Comment:  return code_comment;
        case TokenCategory::Preproc:  return code_preproc;
        case TokenCategory::Variable: return code_variable;
        case TokenCategory::Plain:    break;
    }
    return code_plain;
}

namespace {

// Default: a calm slate dark scheme. The code colours are the RGB equivalents
// of the TUI's Default token_style() palette, and code_bg is the same
// rgb(20,22,26) the TUI paints behind fenced blocks.
GuiTheme make_default() {
    GuiTheme t;
    t.window_bg = "#1b1f26";
    t.panel_bg = "#1b1f26";
    t.header_bg = "#21262f";
    t.border = "#2e3440";
    t.accent = "#56b6c2";
    t.accent_text = "#10141a";
    t.user_bubble_bg = "#262c36";
    t.muted = "#8b94a5";

    t.text = "#d8dee9";
    t.heading = "#eceff4";
    t.link = "#61afef";
    t.quote_text = "#aab3c0";
    t.rule = "#3b4250";
    t.inline_code_bg = "#262c36";
    t.inline_code_text = "#e5c07b";
    t.code_bg = "#14161a";
    t.table_head_bg = "#262c36";

    t.code_plain = "#d8dee9";
    t.code_keyword = "#c678dd";
    t.code_type = "#56b6c2";
    t.code_builtin = "#61afef";
    t.code_string = "#98c379";
    t.code_number = "#d19a66";
    t.code_comment = "#5c6773";
    t.code_preproc = "#c678dd";
    t.code_variable = "#e06c75";
    return t;
}

// Mono: no hue anywhere. The TUI's Mono theme separates categories with
// bold/dim only; the GUI has a real greyscale ramp to work with, so it
// separates them by lightness instead.
GuiTheme make_mono() {
    GuiTheme t;
    // Every channel is equal in every colour — test_gui_theme enforces it, and
    // a single off-grey would be visible as a colour cast in a code block.
    t.window_bg = "#181818";
    t.panel_bg = "#181818";
    t.header_bg = "#202020";
    t.border = "#343434";
    t.accent = "#9e9e9e";
    t.accent_text = "#181818";
    t.user_bubble_bg = "#262626";
    t.muted = "#7d7d7d";

    t.text = "#d4d4d4";
    t.heading = "#f2f2f2";
    t.link = "#bcbcbc";
    t.quote_text = "#9e9e9e";
    t.rule = "#404040";
    t.inline_code_bg = "#262626";
    t.inline_code_text = "#e4e4e4";
    t.code_bg = "#121212";
    t.table_head_bg = "#262626";

    t.code_plain = "#cacaca";
    t.code_keyword = "#ffffff";
    t.code_type = "#e8e8e8";
    t.code_builtin = "#dcdcdc";
    t.code_string = "#ababab";
    t.code_number = "#e8e8e8";
    t.code_comment = "#6e6e6e";
    t.code_preproc = "#ffffff";
    t.code_variable = "#b8b8b8";
    return t;
}

// Vivid: the high-saturation scheme, matching the TUI's Vivid token choices on
// a slightly violet ground.
GuiTheme make_vivid() {
    GuiTheme t;
    t.window_bg = "#16121d";
    t.panel_bg = "#16121d";
    t.header_bg = "#1e1829";
    t.border = "#3a2f4d";
    t.accent = "#ff79c6";
    t.accent_text = "#16121d";
    t.user_bubble_bg = "#262036";
    t.muted = "#9a8fb0";

    t.text = "#e6e0f0";
    t.heading = "#ffffff";
    t.link = "#8be9fd";
    t.quote_text = "#bfb3d4";
    t.rule = "#4a3d63";
    t.inline_code_bg = "#262036";
    t.inline_code_text = "#ffb86c";
    t.code_bg = "#12101a";
    t.table_head_bg = "#262036";

    t.code_plain = "#e6e0f0";
    t.code_keyword = "#ff79c6";
    t.code_type = "#8be9fd";
    t.code_builtin = "#6ab0f3";
    t.code_string = "#50fa7b";
    t.code_number = "#ffb86c";
    t.code_comment = "#9aa5b1";
    t.code_preproc = "#ff5555";
    t.code_variable = "#ff6e6e";
    return t;
}

// None: Default's chrome and prose, but every code category collapses to
// code_plain so fenced blocks come out uncoloured.
GuiTheme make_none() {
    GuiTheme t = make_default();
    t.code_keyword = t.code_plain;
    t.code_type = t.code_plain;
    t.code_builtin = t.code_plain;
    t.code_string = t.code_plain;
    t.code_number = t.code_plain;
    t.code_comment = t.code_plain;
    t.code_preproc = t.code_plain;
    t.code_variable = t.code_plain;
    return t;
}

}  // namespace

const GuiTheme& gui_theme(SyntaxTheme t) {
    // Function-local statics: built once, handed out by reference, never
    // mutated — the same shape builtin_profiles() uses in agent_persist.
    static const GuiTheme kDefault = make_default();
    static const GuiTheme kMono = make_mono();
    static const GuiTheme kVivid = make_vivid();
    static const GuiTheme kNone = make_none();
    switch (t) {
        case SyntaxTheme::Mono:  return kMono;
        case SyntaxTheme::Vivid: return kVivid;
        case SyntaxTheme::None:  return kNone;
        case SyntaxTheme::Default: break;
    }
    return kDefault;
}

std::string window_stylesheet(const GuiTheme& th) {
    // Qt Style Sheet syntax. Object names (#moo*) are set by the widget layer;
    // everything else is styled by class so new widgets inherit the scheme.
    std::string s;
    s += "QWidget { background: " + th.window_bg + "; color: " + th.text +
         "; }\n";

    s += "#mooHeader { background: " + th.header_bg + "; border-bottom: 1px solid " +
         th.border + "; }\n";
    s += "#mooTitle { color: " + th.heading + "; font-weight: 600; }\n";
    s += "#mooStatus, #mooChip { color: " + th.muted + "; }\n";

    s += "#mooScroll { background: " + th.panel_bg + "; border: none; }\n";
    s += "#mooScroll > QWidget > QWidget { background: " + th.panel_bg + "; }\n";
    s += "#mooTranscript { background: " + th.panel_bg + "; }\n";

    // Message cards. The user turn is a filled bubble; the assistant turn is
    // flush with the surface and carries an accent rule instead (drawn by the
    // card widget, not here).
    s += "#mooUserCard { background: " + th.user_bubble_bg +
         "; border-radius: 10px; }\n";
    s += "#mooAssistantCard, #mooReasoningCard { background: transparent; }\n";
    s += "#mooErrorCard { background: rgba(224,108,117,0.12); border-radius: 8px; "
         "}\n";
    s += "#mooRole { color: " + th.muted + "; font-weight: 600; }\n";

    // The reasoning disclosure button reads as a quiet label, not a button.
    s += "#mooReasoningToggle { background: transparent; border: none; color: " +
         th.muted + "; text-align: left; padding: 2px 0; }\n";
    s += "#mooReasoningToggle:hover { color: " + th.text + "; }\n";
    s += "#mooReasoningBody { color: " + th.muted + "; background: transparent; "
         "border: none; }\n";

    s += "#mooComposer { background: " + th.header_bg + "; border-top: 1px solid " +
         th.border + "; }\n";
    s += "#mooInput { background: " + th.window_bg + "; border: 1px solid " +
         th.border + "; border-radius: 8px; padding: 6px 8px; color: " + th.text +
         "; selection-background-color: " + th.accent + "; selection-color: " +
         th.accent_text + "; }\n";
    s += "#mooInput:focus { border: 1px solid " + th.accent + "; }\n";

    s += "QPushButton { background: " + th.user_bubble_bg + "; color: " + th.text +
         "; border: 1px solid " + th.border +
         "; border-radius: 8px; padding: 6px 14px; }\n";
    s += "QPushButton:hover { border: 1px solid " + th.accent + "; }\n";
    s += "QPushButton:disabled { color: " + th.muted + "; }\n";
    s += "#mooSend { background: " + th.accent + "; color: " + th.accent_text +
         "; border: 1px solid " + th.accent + "; font-weight: 600; }\n";
    s += "#mooSend:disabled { background: " + th.user_bubble_bg + "; color: " +
         th.muted + "; border: 1px solid " + th.border + "; }\n";

    s += "#mooSettings::menu-indicator { image: none; }\n";

    s += "QMenu { background: " + th.header_bg + "; color: " + th.text +
         "; border: 1px solid " + th.border + "; padding: 4px; }\n";
    // The right padding is what the submenu arrow is drawn into: styling
    // QMenu::item at all stops Qt reserving room for it, and 24px is not enough
    // for the widest label here ("Model  (some-long-model-id)"), whose arrow
    // then lands on top of the text and the row stops reading as a submenu.
    s += "QMenu::item { padding: 5px 40px 5px 20px; border-radius: 5px; }\n";
    s += "QMenu::right-arrow { width: 10px; height: 10px; margin-right: 12px; }\n";
    s += "QMenu::item:selected { background: " + th.accent + "; color: " +
         th.accent_text + "; }\n";
    s += "QMenu::separator { height: 1px; background: " + th.border +
         "; margin: 4px 6px; }\n";

    s += "QScrollBar:vertical { background: transparent; width: 10px; margin: 0; "
         "}\n";
    s += "QScrollBar::handle:vertical { background: " + th.border +
         "; border-radius: 5px; min-height: 28px; }\n";
    s += "QScrollBar::handle:vertical:hover { background: " + th.muted + "; }\n";
    s += "QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; "
         "}\n";
    s += "QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical { "
         "background: transparent; }\n";

    s += "QToolTip { background: " + th.header_bg + "; color: " + th.text +
         "; border: 1px solid " + th.border + "; }\n";
    return s;
}

}  // namespace moocode::gui
