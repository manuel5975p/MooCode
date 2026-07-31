#ifndef MOOCODE_GUI_MAIN_WINDOW_HPP
#define MOOCODE_GUI_MAIN_WINDOW_HPP

// The window: header bar (model chip + settings dropdown), transcript,
// composer. Owns the AgentBridge and translates its signals into transcript
// updates.

#include <QFont>
#include <QMainWindow>
#include <QString>
#include <QStringList>

#include <map>
#include <string>
#include <vector>

#include "agent/persist.hpp"
#include "agent/provider_factory.hpp"
#include "gui/image_attach.hpp"
#include "gui/settings_menu.hpp"

class QAction;
class QHBoxLayout;
class QLabel;
class QMenu;
class QMimeData;
class QPlainTextEdit;
class QPushButton;
class QToolButton;

namespace moocode::gui {

class AgentBridge;
class ChatPanel;

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    // `home` is the resolved ~/.moo directory ("" => persistence disabled).
    MainWindow(std::string home, Settings settings, ProviderConnection conn,
               GenerationParams params, SettingsState state,
               std::string system_prompt, QWidget* parent = nullptr);
    ~MainWindow() override;

    // Open the most recent conversation saved in the working directory (the
    // --continue flag). Returns false when there is none.
    bool continueLastConversation();

protected:
    bool eventFilter(QObject* obj, QEvent* e) override;
    void closeEvent(QCloseEvent* e) override;

private slots:
    void onSend();
    void onStop();
    void onStarted();
    void onFinished(const QString& answer);
    void onFailed(const QString& message);
    void onUsage(int prompt_tokens, int completion_tokens);
    void onConnectionRequested(const QString& profile, const QString& model);
    void onParamsChanged();
    void onThemeChanged(SyntaxTheme theme);
    void onFontsChanged();
    void onTextSizeStep(int delta);
    void onModelsRequested();
    void onModelsDetected(const QStringList& models);
    void onSystemPromptChanged();
    void onNewConversation();
    void onBrowseConversations();

private:
    // Rebuilt on every show, so a conversation saved by the TUI in another
    // window appears without restarting.
    void rebuildConversationsMenu();
    // Adopt a saved conversation: hand its history to the agent and repaint the
    // transcript from it. Reports failures into the transcript, never a dialog.
    void openConversation(const std::string& path);
    // Repaint the transcript from `conv` (no history change).
    void renderConversation(const Conversation& conv);

    // Stage every image in `md` (a clipboard paste or a drop) on the composer,
    // reporting whatever could not be read into the transcript.
    void attachFromMime(const QMimeData* md);
    // Repaint the row of staged-image chips; hides it when nothing is staged.
    void rebuildAttachStrip();
    // Drop every staged image. Ids restart, so the "[image #N]" references a
    // turn sends are numbered from one within that turn.
    void clearAttachments();

    void applyTheme();
    void applyFonts();
    void syncInputHeight();
    void updateChips();
    void setBusyUi(bool busy);
    GenerationParams currentParams() const;
    ProviderConnection connectionFor(const std::string& profile,
                                     const std::string& model) const;
    void autosave();

    std::string home_;
    Settings settings_;
    // The live profile list. Held rather than recomputed from settings_ because
    // model detection updates it in place, and when settings.toml declares no
    // profiles the list is the built-in fallback, which has nowhere else to live.
    std::vector<Profile> profiles_;
    std::map<std::string, std::string> credentials_;
    ProviderConnection conn_;
    GenerationParams params_;

    AgentBridge* bridge_ = nullptr;
    ChatPanel* chat_ = nullptr;
    SettingsMenu* settings_menu_ = nullptr;

    QWidget* header_ = nullptr;
    QLabel* title_ = nullptr;
    QLabel* chip_ = nullptr;
    QLabel* status_ = nullptr;
    QToolButton* settings_button_ = nullptr;
    QToolButton* conversations_button_ = nullptr;
    QMenu* conversations_menu_ = nullptr;
    // Owned by the window, not by the menu it appears in: a QAction only reaches
    // its shortcut from a widget that is actually shown, and a popup menu is not
    // one until it pops up. QMenu::clear() leaves a foreign-parented action
    // alone, so the rebuild can keep re-adding this one.
    QAction* new_conversation_ = nullptr;
    QPlainTextEdit* input_ = nullptr;
    QPushButton* send_ = nullptr;
    QPushButton* stop_ = nullptr;

    // Images pasted or dropped onto the composer, waiting for the turn that
    // will carry them. The chip strip above the input is their only handle —
    // unlike the TUI, where the handle is an `[img#N]` marker inside the text.
    QWidget* attach_strip_ = nullptr;
    QHBoxLayout* attach_box_ = nullptr;
    std::vector<StagedImage> attachments_;
    int next_attach_id_ = 1;

    // The application's fonts as they were before any user override, so
    // "reset appearance" has something true to return to.
    QFont default_ui_font_;
    QFont default_mono_font_;

    int in_tokens_ = 0;
    int out_tokens_ = 0;
    std::string conv_path_;     // autosave target, minted on the first turn
    std::string conv_created_;  // stamped once, so rewrites keep the original
};

}  // namespace moocode::gui

#endif  // MOOCODE_GUI_MAIN_WINDOW_HPP
