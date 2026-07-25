#include "gui/markdown_view.hpp"

#include <QEvent>
#include <QFontDatabase>
#include <QResizeEvent>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextFrame>
#include <QTextTable>

#include <algorithm>
#include <string>
#include <vector>

#include "agent/syntax_highlight.hpp"
#include "gui/theme.hpp"

namespace moocode::gui {
namespace {

// Heading scale relative to the body font, h1..h6. Six entries so a lookup by
// (level - 1) is always in range for the levels Qt's importer produces.
constexpr double kHeadingScale[6] = {1.55, 1.32, 1.16, 1.06, 1.0, 1.0};

// Vertical breathing room, in pixels, above and below a fenced code band and
// around headings. Small absolute values read better than font-relative ones
// here because the surrounding cards already carry generous padding.
constexpr int kFenceGap = 8;

// How far a fenced band's slab is inset from the message's text column, and how
// far the code inside it is inset from the slab's own edge. Two separate steps:
// the first says the code is a thing apart from the prose, the second stops the
// text touching the edge of its background — see padCodeBands for why the
// vertical half of the second cannot be a margin.
constexpr int kCodeInset = 8;
constexpr int kCodePad = 12;

QColor col(const std::string& hex) { return QColor(QString::fromStdString(hex)); }

bool is_fence(const QTextBlockFormat& bf) {
    return bf.hasProperty(QTextFormat::BlockCodeFence);
}

QString fence_language(const QTextBlockFormat& bf) {
    return bf.hasProperty(QTextFormat::BlockCodeLanguage)
               ? bf.stringProperty(QTextFormat::BlockCodeLanguage)
               : QString();
}

// Whether two blocks belong to the same fenced band. Qt puts no separator
// block between back-to-back fences, so adjacency alone would run four
// consecutive code blocks together — visually into one slab, and worse, into
// one lexer run. A change of info string is the only boundary signal the
// importer preserves (the fence marker is "`" for all of them), so two
// same-language fences in a row do still merge; that is the one case this
// cannot see, and it is harmless.
bool same_band(const QTextBlock& a, const QTextBlock& b) {
    if (!a.isValid() || !b.isValid()) return false;
    if (!is_fence(a.blockFormat()) || !is_fence(b.blockFormat())) return false;
    return fence_language(a.blockFormat()) == fence_language(b.blockFormat());
}

// A fragment is inline code when the importer gave it the monospace family but
// it is not inside a fenced block (fences are handled at block level).
bool is_inline_code(const QTextCharFormat& cf) {
    const QVariant fams = cf.fontFamilies();
    if (!fams.isValid()) return false;
    for (const QString& f : fams.toStringList())
        if (f.compare("monospace", Qt::CaseInsensitive) == 0) return true;
    return false;
}

}  // namespace

MarkdownView::MarkdownView(QWidget* parent) : QTextBrowser(parent) {
    setFrameStyle(QFrame::NoFrame);
    setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setOpenExternalLinks(true);
    setReadOnly(true);
    // Selectable so answers can be copied, but never a focus stop — Tab should
    // walk the composer, not every message in the transcript.
    setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard |
                            Qt::LinksAccessibleByMouse);
    setFocusPolicy(Qt::ClickFocus);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    viewport()->setAutoFillBackground(false);
    document()->setDocumentMargin(0);
}

void MarkdownView::setMarkdownText(const QString& markdown) {
    if (plain_ == false && markdown == source_) return;
    source_ = markdown;
    plain_ = false;
    rerender();
}

void MarkdownView::setPlainThemedText(const QString& text, bool dim) {
    if (plain_ && plain_dim_ == dim && text == source_) return;
    source_ = text;
    plain_ = true;
    plain_dim_ = dim;
    rerender();
}

void MarkdownView::setTheme(SyntaxTheme theme) {
    if (theme == theme_) return;
    theme_ = theme;
    rerender();
}

void MarkdownView::setMonoFont(const QFont& f) {
    if (mono_set_ && f == mono_) return;
    mono_ = f;
    mono_set_ = true;
    rerender();
}

// The configured code font, or the platform's fixed-pitch font sized to sit
// just under the prose font (a monospace face at the same point size reads
// heavier than the surrounding text).
QFont MarkdownView::monoFont() const {
    if (mono_set_) return mono_;
    QFont f = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    const qreal pt = document()->defaultFont().pointSizeF();
    if (pt > 0) f.setPointSizeF(pt * 0.94);
    return f;
}

void MarkdownView::changeEvent(QEvent* e) {
    QTextBrowser::changeEvent(e);
    if (e->type() == QEvent::FontChange) rerender();
}

void MarkdownView::rerender() {
    const GuiTheme& th = gui_theme(theme_);

    // The default font must be installed before the import: Qt's Markdown
    // importer sizes headings relative to whatever the document's default font
    // is at import time.
    QFont base = font();
    document()->setDefaultFont(base);

    if (plain_) {
        setPlainText(source_);
        QTextCursor all(document());
        all.select(QTextCursor::Document);
        QTextCharFormat cf;
        cf.setForeground(col(plain_dim_ ? th.muted : th.text));
        all.mergeCharFormat(cf);
    } else {
        // NoHTML: model output is untrusted text, and a chat transcript has no
        // reason to let it inject raw markup into the view.
        document()->setMarkdown(source_,
                                QTextDocument::MarkdownFeatures(
                                    QTextDocument::MarkdownDialectGitHub) |
                                    QTextDocument::MarkdownNoHTML);
        applyTheme();
    }
    syncHeight();
}

// Qt's Markdown importer can leave an empty leading block inside a table cell —
// reproducibly in the first cell when a blockquote immediately precedes the
// table (Qt 6.11). It renders as a blank line above that one cell's text,
// making the whole row look misaligned. The block carries no content, so
// deleting it loses nothing.
void MarkdownView::dropEmptyLeadingCellBlocks() {
    QTextDocument* doc = document();
    for (QTextFrame* frame : doc->rootFrame()->childFrames()) {
        auto* table = qobject_cast<QTextTable*>(frame);
        if (!table) continue;
        for (int r = 0; r < table->rows(); ++r) {
            for (int c = 0; c < table->columns(); ++c) {
                QTextTableCell cell = table->cellAt(r, c);
                if (!cell.isValid()) continue;
                int blocks = 0;
                for (QTextFrame::iterator it = cell.begin(); !it.atEnd(); ++it)
                    ++blocks;
                QTextCursor cc = cell.firstCursorPosition();
                // deleteChar() on an empty block removes the paragraph
                // separator, pulling the following block up into its place.
                while (blocks > 1 && cc.block().text().isEmpty()) {
                    cc.deleteChar();
                    --blocks;
                }
            }
        }
    }
}

// Pad a fenced band vertically, by adding an empty block carrying the band's
// background above its first line and below its last.
//
// Qt paints a block background over the block's text rect; its margins sit
// outside the fill, so no block format can put space between the code and the
// top or bottom edge of its own slab. (A QTextFrame's background does cover its
// padding, but QTextCursor::insertFrame strips the fence properties off the
// blocks it swallows, which is what the highlighter reads.) A spacer block is
// the cheap way to buy the same look: it holds no text, and it is deliberately
// not marked as a fence, so neither the band walk nor the highlighter sees it.
//
// Runs last, after every pass that walks the document by position, because it
// inserts blocks.
void MarkdownView::padCodeBands() {
    QTextDocument* doc = document();
    const GuiTheme& th = gui_theme(theme_);

    // One insertion shifts every position after it, so collect first and then
    // apply back to front.
    std::vector<std::pair<int, int>> bands;  // first/last block position
    for (QTextBlock b = doc->begin(); b.isValid();) {
        if (!is_fence(b.blockFormat())) {
            b = b.next();
            continue;
        }
        QTextBlock c = b, last = b;
        while (c.isValid() && same_band(b, c)) {
            last = c;
            c = c.next();
        }
        bands.emplace_back(b.position(), last.position());
        b = c;
    }

    QTextBlockFormat spacer;
    spacer.setBackground(col(th.code_bg));
    spacer.setLeftMargin(kCodeInset);
    spacer.setRightMargin(kCodeInset);
    // An empty block is as tall as the font it would have used, so the height
    // is bought with a small font rather than a fixed line height: a fixed line
    // height shorter than the text next to it makes the layout overlap the two
    // and clips the code's last line.
    QTextCharFormat spacer_char;
    {
        QFont f = document()->defaultFont();
        f.setPixelSize(kCodePad);
        spacer_char.setFont(f);
    }
    const QTextCharFormat plain;

    for (auto it = bands.rbegin(); it != bands.rend(); ++it) {
        const QTextBlock first = doc->findBlock(it->first);
        const QTextBlock last = doc->findBlock(it->second);
        if (!first.isValid() || !last.isValid()) continue;

        // insertBlock splits at the cursor and gives the format it is handed to
        // the block *after* the split; the block left behind keeps the one it
        // had. Below the band that lands the spacer straight away. Above it,
        // the code is the later of the two, so the code's format is the one to
        // pass and the leftover block is restyled afterwards.
        //
        // Below goes first: inserting there cannot move the band's own start.
        QTextBlockFormat below = spacer;
        below.setBottomMargin(kFenceGap);
        QTextCursor bc(doc);
        bc.setPosition(last.position() + last.length() - 1);
        bc.insertBlock(below, plain);
        bc.setBlockCharFormat(spacer_char);

        QTextCursor ac(doc);
        ac.setPosition(first.position());
        ac.insertBlock(first.blockFormat(), plain);
        QTextBlockFormat above = spacer;
        above.setTopMargin(kFenceGap);
        QTextCursor sp(doc);
        sp.setPosition(it->first);
        sp.setBlockFormat(above);
        sp.setBlockCharFormat(spacer_char);
    }
}

void MarkdownView::applyTheme() {
    const GuiTheme& th = gui_theme(theme_);
    QTextDocument* doc = document();
    const qreal base_pt = doc->defaultFont().pointSizeF();

    const QFont mono = monoFont();

    QTextCursor cur(doc);
    cur.beginEditBlock();

    // 0. Repair the import. Structural, and must run before anything that
    //    walks blocks by position, since it deletes some.
    dropEmptyLeadingCellBlocks();

    // 1. Base pass: give every character an explicit foreground. The importer
    //    leaves foregrounds unset, which would otherwise fall back to the
    //    widget palette and ignore the theme entirely.
    {
        QTextCursor all(doc);
        all.select(QTextCursor::Document);
        QTextCharFormat cf;
        cf.setForeground(col(th.text));
        all.mergeCharFormat(cf);
    }

    // 2. Block pass.
    for (QTextBlock b = doc->begin(); b.isValid(); b = b.next()) {
        const QTextBlockFormat bf = b.blockFormat();

        QTextCursor bc(b);
        bc.setPosition(b.position());
        bc.setPosition(b.position() + b.length() - 1, QTextCursor::KeepAnchor);

        if (qobject_cast<QTextTable*>(doc->frameAt(b.position()))) {
            // Cells carry their own padding; any block margin inherited from
            // the text before the table only makes the row ragged.
            QTextBlockFormat nbf = bf;
            nbf.setTopMargin(0);
            nbf.setBottomMargin(0);
            nbf.setLeftMargin(0);
            nbf.setRightMargin(0);
            nbf.clearProperty(QTextFormat::BlockQuoteLevel);
            bc.setBlockFormat(nbf);
            continue;
        }

        if (const int level = bf.headingLevel(); level > 0) {
            QTextCharFormat cf;
            cf.setForeground(col(th.heading));
            cf.setFontWeight(QFont::DemiBold);
            if (base_pt > 0)
                cf.setFontPointSize(base_pt * kHeadingScale[std::clamp(level, 1, 6) - 1]);
            bc.mergeCharFormat(cf);

            QTextBlockFormat nbf = bf;
            nbf.setTopMargin(level <= 2 ? kFenceGap * 1.5 : kFenceGap);
            nbf.setBottomMargin(kFenceGap * 0.5);
            bc.setBlockFormat(nbf);
            continue;
        }

        if (is_fence(bf)) {
            QTextBlockFormat nbf = bf;
            nbf.setBackground(col(th.code_bg));
            // The vertical gap and the slab's own top/bottom padding are the
            // spacer blocks' job (padCodeBands); a margin here would only
            // stripe the background between lines.
            nbf.setTopMargin(0);
            nbf.setBottomMargin(0);
            nbf.setLeftMargin(kCodeInset);
            nbf.setRightMargin(kCodeInset);
            nbf.setTextIndent(kCodePad);
            // The importer marks fenced lines non-breakable. The view has no
            // scrollbars of its own, so an over-long line would not scroll —
            // it would simply be cut off at the edge of the card.
            nbf.setNonBreakableLines(false);
            bc.setBlockFormat(nbf);

            QTextCharFormat cf;
            cf.setFont(mono);
            cf.setForeground(col(th.code_plain));
            bc.mergeCharFormat(cf);
            continue;
        }

        if (bf.hasProperty(QTextFormat::BlockQuoteLevel) &&
            bf.intProperty(QTextFormat::BlockQuoteLevel) > 0) {
            // Qt has no per-side block border, so a quote is marked by colour
            // and italics on top of the indent the importer already applied.
            QTextCharFormat cf;
            cf.setForeground(col(th.quote_text));
            cf.setFontItalic(true);
            bc.mergeCharFormat(cf);
            continue;
        }
    }

    // 3. Inline pass: code spans and links, which live in char formats and so
    //    survive the base pass only if re-applied after it.
    for (QTextBlock b = doc->begin(); b.isValid(); b = b.next()) {
        if (is_fence(b.blockFormat())) continue;
        for (QTextBlock::iterator it = b.begin(); !it.atEnd(); ++it) {
            const QTextFragment frag = it.fragment();
            if (!frag.isValid() || frag.length() == 0) continue;
            const QTextCharFormat cf = frag.charFormat();

            QTextCharFormat merged;
            bool touched = false;
            if (cf.isAnchor()) {
                merged.setForeground(col(th.link));
                merged.setFontUnderline(true);
                touched = true;
            } else if (is_inline_code(cf)) {
                merged.setForeground(col(th.inline_code_text));
                merged.setBackground(col(th.inline_code_bg));
                merged.setFont(mono);
                touched = true;
            }
            if (!touched) continue;

            QTextCursor fc(doc);
            fc.setPosition(frag.position());
            fc.setPosition(frag.position() + frag.length(), QTextCursor::KeepAnchor);
            fc.mergeCharFormat(merged);
        }
    }

    // 4. Tables: gridlines plus a tinted header row.
    for (QTextFrame* frame : doc->rootFrame()->childFrames()) {
        auto* table = qobject_cast<QTextTable*>(frame);
        if (!table) continue;
        QTextTableFormat tf = table->format();
        tf.setBorder(1);
        tf.setBorderBrush(col(th.rule));
        tf.setBorderStyle(QTextFrameFormat::BorderStyle_Solid);
        tf.setBorderCollapse(true);
        tf.setCellPadding(6);
        tf.setCellSpacing(0);
        tf.setTopMargin(kFenceGap);
        tf.setBottomMargin(kFenceGap);
        table->setFormat(tf);

        for (int c = 0; c < table->columns(); ++c) {
            QTextTableCell cell = table->cellAt(0, c);
            if (!cell.isValid()) continue;
            // Round-trip through QTextTableCellFormat: assigning a plain
            // QTextCharFormat back onto a cell drops the cell-specific
            // properties (padding, spans, alignment) and the header row comes
            // out ragged.
            QTextTableCellFormat cf = cell.format().toTableCellFormat();
            cf.setBackground(col(th.table_head_bg));
            cell.setFormat(cf);
            // The cell format colours the cell, not the text already in it.
            QTextCursor hc = cell.firstCursorPosition();
            hc.setPosition(cell.lastCursorPosition().position(),
                           QTextCursor::KeepAnchor);
            QTextCharFormat hf;
            hf.setForeground(col(th.heading));
            hf.setFontWeight(QFont::DemiBold);
            hc.mergeCharFormat(hf);
        }
    }

    highlightCodeFences();
    // Last: it inserts blocks, so it must follow every pass above that walks
    // the document by position.
    padCodeBands();
    cur.endEditBlock();
}

void MarkdownView::highlightCodeFences() {
    const GuiTheme& th = gui_theme(theme_);
    QTextDocument* doc = document();

    QTextBlock b = doc->begin();
    while (b.isValid()) {
        if (!is_fence(b.blockFormat())) {
            b = b.next();
            continue;
        }
        // Gather the whole band: consecutive fenced blocks sharing an info
        // string. Highlighting them as one string is what keeps the lexer's
        // cross-line state (block comments, triple-quoted strings) correct.
        const QString lang_tag = fence_language(b.blockFormat());
        std::vector<QTextBlock> band;
        QString joined;
        for (QTextBlock c = b; c.isValid() && is_fence(c.blockFormat()) &&
                               fence_language(c.blockFormat()) == lang_tag;
             c = c.next()) {
            if (!band.empty()) joined += '\n';
            joined += c.text();
            band.push_back(c);
        }

        const Language lang = language_from_tag(lang_tag.toStdString());
        if (lang != Language::None) {
            const auto lines = highlight_block(joined.toStdString(), lang);
            const std::size_t n = std::min(lines.size(), band.size());
            for (std::size_t i = 0; i < n; ++i) {
                const QTextBlock& blk = band[i];
                int pos = blk.position();
                const int end = blk.position() + blk.length() - 1;
                for (const HlSpan& sp : lines[i]) {
                    // Span text is UTF-8 bytes; cursor positions are UTF-16
                    // code units, so the QString length is the one to advance
                    // by. Clamp in case the two ever disagree.
                    const int len = QString::fromStdString(sp.text).size();
                    if (len <= 0) continue;
                    const int stop = std::min(pos + len, end);
                    if (pos >= stop) break;
                    if (sp.category != TokenCategory::Plain) {
                        QTextCursor sc(doc);
                        sc.setPosition(pos);
                        sc.setPosition(stop, QTextCursor::KeepAnchor);
                        QTextCharFormat cf;
                        cf.setForeground(col(th.token_color(sp.category)));
                        sc.mergeCharFormat(cf);
                    }
                    pos = stop;
                }
            }
        }

        b = band.empty() ? b.next() : band.back().next();
    }
}

void MarkdownView::syncHeight() {
    const int w = viewport()->width();
    if (w <= 0) return;
    document()->setTextWidth(w);
    const int h = static_cast<int>(document()->size().height()) + 1;
    if (h == last_height_) return;
    last_height_ = h;
    setFixedHeight(h);
    updateGeometry();
}

QSize MarkdownView::sizeHint() const {
    return QSize(QTextBrowser::sizeHint().width(), std::max(last_height_, 1));
}

QSize MarkdownView::minimumSizeHint() const {
    return QSize(0, std::max(last_height_, 1));
}

void MarkdownView::resizeEvent(QResizeEvent* e) {
    QTextBrowser::resizeEvent(e);
    // Width changes reflow the document, which changes its height; recompute
    // rather than letting the fixed height go stale.
    syncHeight();
}

}  // namespace moocode::gui
