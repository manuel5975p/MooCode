#ifndef MOOCODE_GUI_MARKDOWN_VIEW_HPP
#define MOOCODE_GUI_MARKDOWN_VIEW_HPP

// A read-only Markdown block inside the transcript: one message, one view.
//
// Rendering is Qt's own Markdown importer (GitHub dialect) followed by a
// theming pass that walks the resulting QTextDocument and applies colours,
// fonts and margins. The pass is necessary, not a stylistic choice: Qt honours
// QTextDocument::setDefaultStyleSheet for setHtml() only, so a
// markdown-imported document arrives with no explicit character formats at all
// (verified against Qt 6.11). Everything the importer *does* record — heading
// level, blockquote level, list membership, task-list markers, code fences plus
// their info string, table frames, horizontal rules — is reachable through the
// block/char formats, which is what makes the pass cheap.
//
// Fenced code keeps moocode's own highlighter (agent_syntax) rather than
// anything Qt provides, so a code block looks the same here as in the TUI.
//
// The view has no scrollbars of its own: it reports the document's natural
// height and the transcript scrolls as a whole, which is what makes a column of
// these read like one continuous conversation.

#include <QFont>
#include <QString>
#include <QTextBrowser>

#include "agent/types.hpp"  // SyntaxTheme

namespace moocode::gui {

class MarkdownView : public QTextBrowser {
    Q_OBJECT

public:
    explicit MarkdownView(QWidget* parent = nullptr);

    // Replace the content. `markdown` is the raw model text; it is re-parsed on
    // every call, so streaming callers should coalesce (ChatPanel does).
    void setMarkdownText(const QString& markdown);

    // Re-theme. Re-runs the whole import: the theming pass writes concrete
    // formats into the document, so there is nothing to "unstyle" — only to
    // rebuild. No-op when the theme is unchanged.
    void setTheme(SyntaxTheme theme);

    // Font for fenced code and inline code spans. The prose font is the
    // widget's own (set application-wide), but code needs a separate family, so
    // it is passed in rather than derived. An invalid/empty family falls back to
    // the platform's fixed-pitch font.
    void setMonoFont(const QFont& f);

    // Render as plain, non-Markdown text. Used where re-interpreting the text
    // as markup would be wrong or unhelpful: the user's own words, error
    // strings, and reasoning traces (half-formed prose that parses badly).
    // `dim` picks the muted colour over the body colour.
    void setPlainThemedText(const QString& text, bool dim);

    const QString& sourceText() const { return source_; }

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

protected:
    void resizeEvent(QResizeEvent* e) override;
    // The prose font comes from the widget font, and headings are sized
    // relative to it at import time, so a font change means a re-render.
    void changeEvent(QEvent* e) override;

private:
    void rerender();
    void dropEmptyLeadingCellBlocks();  // repairs an importer artifact
    void padCodeBands();        // spacer blocks above/below each fenced band
    void applyTheme();          // the document walk described above
    void highlightCodeFences();
    void syncHeight();

    QFont monoFont() const;

    QString source_;
    SyntaxTheme theme_ = SyntaxTheme::Default;
    QFont mono_;
    bool mono_set_ = false;     // false => use the platform fixed-pitch font
    bool plain_ = false;        // set by setPlainThemedText
    bool plain_dim_ = false;
    int last_height_ = 0;
};

}  // namespace moocode::gui

#endif  // MOOCODE_GUI_MARKDOWN_VIEW_HPP
