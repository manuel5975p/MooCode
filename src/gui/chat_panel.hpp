#ifndef MOOCODE_GUI_CHAT_PANEL_HPP
#define MOOCODE_GUI_CHAT_PANEL_HPP

// The transcript: a scrolling column of message cards.
//
// One MarkdownView per message rather than one document for the whole
// conversation. That costs cross-message text selection, and buys the thing
// streaming actually needs: re-parsing only the message being written. Feeding
// every delta through a single whole-transcript document would re-parse the
// entire conversation per token.
//
// Even per-message, re-parsing on every delta is too much — deltas arrive far
// faster than anyone can read. Streaming text is buffered and flushed on a
// coalescing timer (kFlushMs), with a final exact render when the turn ends.

#include <QFont>
#include <QPointer>
#include <QScrollArea>
#include <QString>
#include <QTimer>
#include <QVector>

#include "agent/types.hpp"  // SyntaxTheme

class QVBoxLayout;
class QWidget;

namespace moocode::gui {

class MarkdownView;

class ChatPanel : public QScrollArea {
    Q_OBJECT

public:
    explicit ChatPanel(QWidget* parent = nullptr);

    void addUserMessage(const QString& text);
    void addErrorMessage(const QString& text);
    // A dim, non-Markdown note from the app itself (e.g. model detection
    // results) rather than from either party in the conversation.
    void addInfoMessage(const QString& text);

    // Open an assistant card and make it the streaming target.
    void beginAssistantMessage();
    void appendAnswer(const QString& fragment);
    void appendReasoning(const QString& fragment);
    // Close the streaming card, rendering `final_text` verbatim when non-empty
    // (the accumulated deltas are used otherwise). Drops an empty card.
    void endAssistantMessage(const QString& final_text);

    void setTheme(SyntaxTheme theme);

    // Font for fenced/inline code in every message, present and future.
    void setMonoFont(const QFont& f);

    // Font for the transcript's prose, separately from the rest of the window.
    // Applied to each message widget rather than to their common parent:
    // QStyleSheetStyle gives every widget an explicit font of its own while an
    // application stylesheet is set, which leaves nothing for a parent font to
    // propagate into. A default-constructed QFont resolves nothing and hands
    // the transcript back to the application font, which is what "unset" means
    // for this setting.
    void setProseFont(const QFont& f);

private:
    QWidget* addCard(const QString& object_name, const QString& role_label,
                     MarkdownView** body_out, bool accent_rule, int at = -1);
    QWidget* rowOf(QWidget* body) const;
    void flushStream();
    void scrollToBottomIfFollowing();
    bool atBottom() const;

    QWidget* container_ = nullptr;
    QVBoxLayout* column_ = nullptr;
    SyntaxTheme theme_ = SyntaxTheme::Default;
    QFont mono_;
    bool mono_set_ = false;  // false => views keep their own default
    QFont prose_;
    bool prose_set_ = false;  // false => widgets keep the application font

    // Every MarkdownView in the transcript, so a theme change can reach them.
    QVector<MarkdownView*> views_;
    // Every widget the prose font applies to: the views plus the role labels.
    // Guarded pointers, because a cancelled turn deletes its card.
    QVector<QPointer<QWidget>> prose_widgets_;

    // Streaming state for the open assistant card.
    MarkdownView* stream_body_ = nullptr;
    MarkdownView* stream_reasoning_ = nullptr;
    QString answer_buf_;
    QString reasoning_buf_;
    bool dirty_ = false;
    bool follow_ = true;  // stick to the bottom until the user scrolls away
    QTimer flush_timer_;
};

}  // namespace moocode::gui

#endif  // MOOCODE_GUI_CHAT_PANEL_HPP
