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
#include "gui/settings_menu.hpp"

class QLabel;
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

private:
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
    QPlainTextEdit* input_ = nullptr;
    QPushButton* send_ = nullptr;
    QPushButton* stop_ = nullptr;

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
