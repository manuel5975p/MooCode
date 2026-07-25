#include "gui/chat_panel.hpp"

#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QScrollBar>
#include <QToolButton>
#include <QVBoxLayout>

#include "gui/markdown_view.hpp"
#include "gui/theme.hpp"

namespace moocode::gui {
namespace {

// Coalescing window for streamed text. Long enough that a fast token stream
// costs a handful of re-parses per second, short enough to still read as live.
constexpr int kFlushMs = 80;

// How close to the bottom still counts as "following" the stream, in pixels.
constexpr int kFollowSlack = 24;

}  // namespace

ChatPanel::ChatPanel(QWidget* parent) : QScrollArea(parent) {
    setObjectName("mooScroll");
    setWidgetResizable(true);
    setFrameStyle(QFrame::NoFrame);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

    container_ = new QWidget(this);
    container_->setObjectName("mooTranscript");
    column_ = new QVBoxLayout(container_);
    column_->setContentsMargins(20, 16, 20, 16);
    column_->setSpacing(14);
    column_->addStretch(1);  // keeps cards top-aligned while the transcript is short
    setWidget(container_);

    flush_timer_.setInterval(kFlushMs);
    connect(&flush_timer_, &QTimer::timeout, this, &ChatPanel::flushStream);

    // Let the user scroll back mid-stream without being yanked to the bottom;
    // scrolling back down re-arms following.
    connect(verticalScrollBar(), &QScrollBar::valueChanged, this,
            [this](int) { follow_ = atBottom(); });
}

bool ChatPanel::atBottom() const {
    const QScrollBar* sb = verticalScrollBar();
    return sb->value() >= sb->maximum() - kFollowSlack;
}

void ChatPanel::scrollToBottomIfFollowing() {
    if (!follow_) return;
    // Deferred: the layout has not yet grown to fit the text just inserted, so
    // maximum() is still the old one.
    QTimer::singleShot(0, this, [this] {
        verticalScrollBar()->setValue(verticalScrollBar()->maximum());
    });
}

// Build one card and place it in the column. `accent_rule` prefixes the card
// with a thin vertical stripe (assistant turns). `at` is the insertion index,
// or -1 for "just before the trailing stretch".
QWidget* ChatPanel::addCard(const QString& object_name, const QString& role_label,
                            MarkdownView** body_out, bool accent_rule, int at) {
    // With a rule, the row is a stripe + card; without, the card *is* the row.
    QWidget* row = accent_rule ? new QWidget(container_) : nullptr;
    auto* card = new QFrame(row ? row : container_);
    card->setObjectName(object_name);

    auto* box = new QVBoxLayout(card);
    box->setContentsMargins(14, 10, 14, 10);
    box->setSpacing(4);
    if (!role_label.isEmpty()) {
        auto* role = new QLabel(role_label, card);
        role->setObjectName("mooRole");
        if (prose_set_) role->setFont(prose_);
        prose_widgets_.push_back(role);
        box->addWidget(role);
    }
    auto* body = new MarkdownView(card);
    body->setTheme(theme_);
    if (mono_set_) body->setMonoFont(mono_);
    if (prose_set_) body->setFont(prose_);
    box->addWidget(body);
    views_.push_back(body);
    prose_widgets_.push_back(body);
    if (body_out) *body_out = body;

    if (row) {
        auto* rowbox = new QHBoxLayout(row);
        rowbox->setContentsMargins(0, 0, 0, 0);
        rowbox->setSpacing(10);
        auto* rule = new QFrame(row);
        rule->setFixedWidth(3);
        rule->setStyleSheet(
            QString("background: %1; border-radius: 1px;")
                .arg(QString::fromStdString(gui_theme(theme_).accent)));
        rowbox->addWidget(rule);
        rowbox->addWidget(card, 1);
    }

    QWidget* placed = row ? row : static_cast<QWidget*>(card);
    column_->insertWidget(at < 0 ? column_->count() - 1 : at, placed);
    return placed;
}

// The container-level row a body widget sits in, i.e. the thing to remove from
// the column. Walks up because an accented card is nested one level deeper.
QWidget* ChatPanel::rowOf(QWidget* body) const {
    QWidget* w = body ? body->parentWidget() : nullptr;
    while (w && w->parentWidget() != container_) w = w->parentWidget();
    return w;
}

void ChatPanel::addUserMessage(const QString& text) {
    MarkdownView* body = nullptr;
    addCard("mooUserCard", QString(), &body, /*accent_rule=*/false);
    // Shown as typed: this is the user's text, not model Markdown, and quietly
    // reinterpreting it as markup would be surprising.
    body->setPlainThemedText(text, /*dim=*/false);
    scrollToBottomIfFollowing();
}

void ChatPanel::addErrorMessage(const QString& text) {
    MarkdownView* body = nullptr;
    addCard("mooErrorCard", QStringLiteral("error"), &body, /*accent_rule=*/false);
    body->setPlainThemedText(text, /*dim=*/false);
    scrollToBottomIfFollowing();
}

void ChatPanel::addInfoMessage(const QString& text) {
    MarkdownView* body = nullptr;
    addCard("mooAssistantCard", QString(), &body, /*accent_rule=*/false);
    body->setPlainThemedText(text, /*dim=*/true);
    scrollToBottomIfFollowing();
}

void ChatPanel::beginAssistantMessage() {
    answer_buf_.clear();
    reasoning_buf_.clear();
    stream_reasoning_ = nullptr;

    MarkdownView* body = nullptr;
    addCard("mooAssistantCard", QStringLiteral("moocode"), &body,
            /*accent_rule=*/true);
    stream_body_ = body;
    dirty_ = false;
    flush_timer_.start();
    scrollToBottomIfFollowing();
}

void ChatPanel::appendAnswer(const QString& fragment) {
    answer_buf_ += fragment;
    dirty_ = true;
}

void ChatPanel::appendReasoning(const QString& fragment) {
    if (!stream_reasoning_) {
        // Created lazily and inserted above the answer card: most turns have
        // no reasoning, and an empty disclosure row above every answer would
        // just be noise.
        QWidget* answer_row = rowOf(stream_body_);
        const int at = answer_row ? column_->indexOf(answer_row) : -1;
        MarkdownView* body = nullptr;
        addCard("mooReasoningCard", QString(), &body, /*accent_rule=*/false, at);

        // Collapsed by default — a chain of thought is context, not the answer.
        auto* toggle = new QToolButton(body->parentWidget());
        toggle->setObjectName("mooReasoningToggle");
        toggle->setText(QStringLiteral("▸ thinking"));
        toggle->setCheckable(true);
        toggle->setCursor(Qt::PointingHandCursor);
        auto* box = qobject_cast<QVBoxLayout*>(body->parentWidget()->layout());
        if (box) box->insertWidget(0, toggle);
        body->setVisible(false);
        connect(toggle, &QToolButton::toggled, this, [toggle, body](bool on) {
            toggle->setText(on ? QStringLiteral("▾ thinking")
                               : QStringLiteral("▸ thinking"));
            body->setVisible(on);
        });

        stream_reasoning_ = body;
    }
    reasoning_buf_ += fragment;
    dirty_ = true;
}

void ChatPanel::flushStream() {
    if (!dirty_) return;
    dirty_ = false;
    if (stream_reasoning_ && !reasoning_buf_.isEmpty())
        stream_reasoning_->setPlainThemedText(reasoning_buf_, /*dim=*/true);
    if (stream_body_) stream_body_->setMarkdownText(answer_buf_);
    scrollToBottomIfFollowing();
}

void ChatPanel::endAssistantMessage(const QString& final_text) {
    flush_timer_.stop();
    if (!final_text.isEmpty()) answer_buf_ = final_text;
    dirty_ = true;
    flushStream();

    if (stream_body_ && answer_buf_.isEmpty()) {
        // Nothing was produced (cancelled, or an empty turn): drop the card
        // rather than leave a stray "moocode" header behind.
        QWidget* row = rowOf(stream_body_);
        views_.removeAll(stream_body_);
        if (row) {
            column_->removeWidget(row);
            row->deleteLater();
        }
    }
    stream_body_ = nullptr;
    stream_reasoning_ = nullptr;
}

void ChatPanel::setTheme(SyntaxTheme theme) {
    if (theme == theme_) return;
    theme_ = theme;
    for (MarkdownView* v : views_)
        if (v) v->setTheme(theme);
}

void ChatPanel::setProseFont(const QFont& f) {
    prose_ = f;
    prose_set_ = f.resolveMask() != 0;
    for (const QPointer<QWidget>& w : prose_widgets_)
        if (w) w->setFont(f);
}

void ChatPanel::setMonoFont(const QFont& f) {
    if (mono_set_ && f == mono_) return;
    mono_ = f;
    mono_set_ = true;
    for (MarkdownView* v : views_)
        if (v) v->setMonoFont(f);
}

}  // namespace moocode::gui
