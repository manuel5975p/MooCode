#include "gui/main_window.hpp"

#include <QApplication>
#include <QCloseEvent>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QMenu>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <ctime>
#include <filesystem>
#include <utility>

#include "gui/agent_bridge.hpp"
#include "gui/chat_panel.hpp"
#include "gui/theme.hpp"

namespace moocode::gui {
namespace {

// The composer grows with the text up to this many lines, then scrolls — long
// enough to draft a paragraph, short enough to leave the transcript readable.
constexpr int kMaxInputLines = 6;

// Vertical padding inside the composer, on top of the text lines themselves.
constexpr int kInputPadding = 18;

// Bounds for the text-size steps. Below ~7pt the UI stops being legible; above
// ~32pt a chat window stops being usable.
constexpr int kMinFontPt = 7;
constexpr int kMaxFontPt = 32;

// The bundled prose face, registered on first use. Returns the family name the
// font database ended up with, or an empty string if the resource would not
// load — in which case the caller keeps the platform's own default rather than
// naming a family that is not there.
//
// All four faces report the same family ("EB Garamond"), so requesting bold or
// italic on it resolves to the real cut rather than a synthesised slant.
QString bundled_prose_family() {
    static const QString family = [] {
        static constexpr const char* kFaces[] = {
            ":/fonts/EBGaramond-Regular.ttf", ":/fonts/EBGaramond-Italic.ttf",
            ":/fonts/EBGaramond-SemiBold.ttf",
            ":/fonts/EBGaramond-SemiBoldItalic.ttf"};
        QString found;
        for (const char* face : kFaces) {
            const int id =
                QFontDatabase::addApplicationFont(QString::fromLatin1(face));
            if (id < 0) continue;
            const QStringList families = QFontDatabase::applicationFontFamilies(id);
            if (found.isEmpty() && !families.isEmpty()) found = families.front();
        }
        return found;
    }();
    return family;
}

// Garamond is a small-on-the-body face: at the point size a UI sans is drawn
// at, it reads about a step smaller. Only the unpinned default is scaled — a
// size the user chose is used as chosen.
constexpr double kProseSizeScale = 1.15;

// ISO-8601 UTC "now" for conversation created/updated stamps, matching the
// format the TUI writes so both frontends' files list interchangeably.
std::string now_iso() {
    std::time_t t = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    if (::gmtime_s(&tm, &t) != 0) return std::string();
#else
    if (!::gmtime_r(&t, &tm)) return std::string();
#endif
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%SZ", &tm);
    return std::string(buf);
}

}  // namespace

MainWindow::MainWindow(std::string home, Settings settings, ProviderConnection conn,
                       GenerationParams params, SettingsState state,
                       std::string system_prompt, QWidget* parent)
    : QMainWindow(parent),
      home_(std::move(home)),
      settings_(std::move(settings)),
      conn_(std::move(conn)),
      params_(std::move(params)) {
    credentials_ = load_credentials(home_);
    profiles_ = menu_profiles(settings_);
    // Captured before any override is applied, so "reset appearance" restores
    // the window's own default rather than a hardcoded guess. For prose that
    // default is the bundled face, not the platform's, so it is folded in here
    // and not in applyFonts — otherwise it would override a user who had
    // deliberately picked something else.
    default_ui_font_ = QApplication::font();
    if (const QString prose = bundled_prose_family(); !prose.isEmpty()) {
        default_ui_font_.setFamily(prose);
        const qreal pt = default_ui_font_.pointSizeF();
        if (pt > 0) default_ui_font_.setPointSizeF(pt * kProseSizeScale);
    }
    default_mono_font_ = QFontDatabase::systemFont(QFontDatabase::FixedFont);

    setWindowTitle(QStringLiteral("moocode"));
    resize(940, 720);

    auto* central = new QWidget(this);
    auto* root = new QVBoxLayout(central);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    // --- header ---
    header_ = new QWidget(central);
    header_->setObjectName("mooHeader");
    auto* hbox = new QHBoxLayout(header_);
    hbox->setContentsMargins(16, 10, 12, 10);
    hbox->setSpacing(10);
    title_ = new QLabel(QStringLiteral("moocode"), header_);
    title_->setObjectName("mooTitle");
    chip_ = new QLabel(header_);
    chip_->setObjectName("mooChip");
    hbox->addWidget(title_);
    hbox->addWidget(chip_, 1);

    settings_button_ = new QToolButton(header_);
    settings_button_->setObjectName("mooSettings");
    settings_button_->setText(QStringLiteral("Settings  ▾"));
    settings_button_->setPopupMode(QToolButton::InstantPopup);
    settings_button_->setCursor(Qt::PointingHandCursor);
    auto* menu = new QMenu(settings_button_);
    settings_button_->setMenu(menu);
    hbox->addWidget(settings_button_);
    root->addWidget(header_);

    settings_menu_ = new SettingsMenu(menu, this);
    settings_menu_->setState(state, profiles_);

    // --- transcript ---
    chat_ = new ChatPanel(central);
    chat_->setTheme(state.theme);
    root->addWidget(chat_, 1);

    // --- composer ---
    auto* composer = new QWidget(central);
    composer->setObjectName("mooComposer");
    auto* cbox = new QVBoxLayout(composer);
    cbox->setContentsMargins(16, 10, 16, 10);
    cbox->setSpacing(6);

    auto* rowbox = new QHBoxLayout();
    rowbox->setSpacing(8);
    input_ = new QPlainTextEdit(composer);
    input_->setObjectName("mooInput");
    input_->setPlaceholderText(
        QStringLiteral("Message moocode…    (Enter to send, Shift+Enter for a newline)"));
    input_->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    input_->installEventFilter(this);
    // Height tracks the content, between one line and kMaxInputLines. The
    // metrics are read on each change rather than captured, so a font change
    // does not leave this recomputing heights from the old line height.
    connect(input_->document(), &QTextDocument::contentsChanged, this,
            [this] { syncInputHeight(); });
    syncInputHeight();
    rowbox->addWidget(input_, 1);

    auto* buttons = new QVBoxLayout();
    buttons->setSpacing(6);
    send_ = new QPushButton(QStringLiteral("Send"), composer);
    send_->setObjectName("mooSend");
    send_->setCursor(Qt::PointingHandCursor);
    stop_ = new QPushButton(QStringLiteral("Stop"), composer);
    stop_->setCursor(Qt::PointingHandCursor);
    stop_->setEnabled(false);
    buttons->addWidget(send_);
    buttons->addWidget(stop_);
    buttons->addStretch(1);
    rowbox->addLayout(buttons);
    cbox->addLayout(rowbox);

    status_ = new QLabel(composer);
    status_->setObjectName("mooStatus");
    cbox->addWidget(status_);
    root->addWidget(composer);

    setCentralWidget(central);

    // --- wiring ---
    bridge_ = new AgentBridge(conn_, params_, std::move(system_prompt), this);
    connect(send_, &QPushButton::clicked, this, &MainWindow::onSend);
    connect(stop_, &QPushButton::clicked, this, &MainWindow::onStop);
    connect(bridge_, &AgentBridge::started, this, &MainWindow::onStarted);
    connect(bridge_, &AgentBridge::answerDelta, chat_, &ChatPanel::appendAnswer);
    connect(bridge_, &AgentBridge::reasoningDelta, chat_, &ChatPanel::appendReasoning);
    connect(bridge_, &AgentBridge::finished, this, &MainWindow::onFinished);
    connect(bridge_, &AgentBridge::failed, this, &MainWindow::onFailed);
    connect(bridge_, &AgentBridge::usage, this, &MainWindow::onUsage);
    connect(settings_menu_, &SettingsMenu::connectionRequested, this,
            &MainWindow::onConnectionRequested);
    connect(settings_menu_, &SettingsMenu::paramsChanged, this,
            &MainWindow::onParamsChanged);
    connect(settings_menu_, &SettingsMenu::themeChanged, this,
            &MainWindow::onThemeChanged);
    connect(settings_menu_, &SettingsMenu::fontsChanged, this,
            &MainWindow::onFontsChanged);
    connect(settings_menu_, &SettingsMenu::textSizeStep, this,
            &MainWindow::onTextSizeStep);
    connect(settings_menu_, &SettingsMenu::modelsRequested, this,
            &MainWindow::onModelsRequested);
    connect(bridge_, &AgentBridge::modelsDetected, this,
            &MainWindow::onModelsDetected);

    applyTheme();
    applyFonts();
    updateChips();
    input_->setFocus();
}

MainWindow::~MainWindow() = default;

void MainWindow::applyTheme() {
    const GuiTheme& th = gui_theme(settings_menu_->state().theme);
    qApp->setStyleSheet(QString::fromStdString(window_stylesheet(th)));
}

// Size the composer to its content, between one line and kMaxInputLines.
void MainWindow::syncInputHeight() {
    if (!input_) return;
    const int line_h = input_->fontMetrics().lineSpacing();
    const int lines = std::max(1, static_cast<int>(input_->document()->lineCount()));
    input_->setMinimumHeight(line_h + kInputPadding);
    input_->setMaximumHeight(line_h * kMaxInputLines + kInputPadding);
    input_->setFixedHeight(line_h * std::min(lines, kMaxInputLines) + kInputPadding);
}

void MainWindow::applyFonts() {
    const GuiSettings& f = settings_menu_->state().fonts;

    // Prose: set application-wide, which reaches every widget that has not been
    // given an explicit font — including the MarkdownViews, whose documents are
    // sized from their widget font.
    QFont ui = default_ui_font_;
    if (!f.font.empty()) ui.setFamily(QString::fromStdString(f.font));
    if (f.font_size > 0) ui.setPointSize(f.font_size);
    qApp->setFont(ui);
    if (input_) {
        input_->setFont(ui);  // explicit, so the metrics below are already current
        syncInputHeight();
    }

    // Transcript prose, when it is to differ from the chrome. Both halves
    // default to the interface font, so leaving this unset keeps one face
    // across the whole window — which is the shipped default.
    QFont chat = ui;
    const bool chat_split = !f.chat_font.empty() || f.chat_font_size > 0;
    if (!f.chat_font.empty()) chat.setFamily(QString::fromStdString(f.chat_font));
    if (f.chat_font_size > 0) chat.setPointSize(f.chat_font_size);
    // A default-constructed QFont is the "inherit" signal, so only a split
    // actually overrides the transcript.
    if (chat_) chat_->setProseFont(chat_split ? chat : QFont());

    // Code: an explicit family, or the platform fixed-pitch face tracking the
    // prose size so the two stay in proportion. It tracks the transcript's
    // prose, not the chrome's — a code block sits among the messages.
    QFont mono = default_mono_font_;
    if (!f.mono_font.empty()) mono.setFamily(QString::fromStdString(f.mono_font));
    if (f.mono_font_size > 0) {
        mono.setPointSize(f.mono_font_size);
    } else {
        const qreal pt = chat.pointSizeF();
        if (pt > 0) mono.setPointSizeF(pt * 0.94);
    }
    if (chat_) chat_->setMonoFont(mono);
}

void MainWindow::updateChips() {
    const SettingsState& st = settings_menu_->state();
    QString chip = bridge_->model();
    if (chip.isEmpty()) chip = QStringLiteral("(no model)");
    if (!st.profile.empty())
        chip += QStringLiteral("  ·  ") + QString::fromStdString(st.profile);
    chip_->setText(chip);

    QString s = QStringLiteral("%1 in / %2 out").arg(in_tokens_).arg(out_tokens_);
    if (!st.effort.empty())
        s += QStringLiteral("  ·  effort %1").arg(QString::fromStdString(st.effort));
    if (st.thinking)
        s += *st.thinking ? QStringLiteral("  ·  thinking on")
                          : QStringLiteral("  ·  thinking off");
    if (st.temperature)
        s += QStringLiteral("  ·  t %1").arg(*st.temperature, 0, 'g', 3);
    status_->setText(s);
}

void MainWindow::setBusyUi(bool busy) {
    send_->setEnabled(!busy);
    stop_->setEnabled(busy);
    // Changing the endpoint mid-turn would race the worker thread; the bridge
    // ignores such calls, so disable the control rather than fail silently.
    settings_button_->setEnabled(!busy);
}

GenerationParams MainWindow::currentParams() const {
    const SettingsState& st = settings_menu_->state();
    GenerationParams gp = params_;
    // Engaged even when empty: set_params treats std::nullopt as "leave the
    // provider's current value alone", so a disengaged optional could never
    // turn a control back off. An engaged empty effort clears it.
    gp.effort = st.effort;
    gp.thinking = st.thinking;
    gp.temperature = st.temperature;
    return gp;
}

ProviderConnection MainWindow::connectionFor(const std::string& profile,
                                             const std::string& model) const {
    ProviderConnection c = conn_;
    for (const Profile& p : profiles_) {
        if (p.name != profile) continue;
        c.kind = profile_kind(p);
        c.base_url = p.base_url;
        c.thinking_type = p.thinking_type;
        if (auto it = credentials_.find(p.name); it != credentials_.end())
            c.api_key = it->second;
        break;
    }
    if (!model.empty()) c.model = model;
    normalize_base_url(c.base_url);
    return c;
}

void MainWindow::onSend() {
    const QString text = input_->toPlainText().trimmed();
    if (text.isEmpty() || bridge_->busy()) return;
    input_->clear();
    chat_->addUserMessage(text);
    chat_->beginAssistantMessage();
    bridge_->send(text);
}

void MainWindow::onStop() { bridge_->cancel(); }

void MainWindow::onStarted() { setBusyUi(true); }

void MainWindow::onFinished(const QString& answer) {
    chat_->endAssistantMessage(answer);
    setBusyUi(false);
    updateChips();
    autosave();
}

void MainWindow::onFailed(const QString& message) {
    // Close the streaming card first so a partial answer is kept rather than
    // discarded, then report why it stopped.
    chat_->endAssistantMessage(QString());
    chat_->addErrorMessage(message);
    setBusyUi(false);
    updateChips();
}

void MainWindow::onUsage(int prompt_tokens, int completion_tokens) {
    in_tokens_ += prompt_tokens;
    out_tokens_ += completion_tokens;
    updateChips();
}

void MainWindow::onConnectionRequested(const QString& profile, const QString& model) {
    SettingsState st = settings_menu_->state();
    st.profile = profile.toStdString();
    // A profile can pin a temperature its endpoint requires (see the builtin
    // kimi entry, whose endpoint accepts only 1.0); adopt it before connecting
    // so the very first request after the switch is valid.
    for (const Profile& p : profiles_)
        if (p.name == st.profile && p.temperature >= 0) st.temperature = p.temperature;
    // Update first so currentParams() below reflects the new profile.
    settings_menu_->setState(st, profiles_);

    conn_ = connectionFor(st.profile, model.toStdString());
    bridge_->reconnect(conn_, currentParams());

    // Report back what was actually connected: a profile switch with no pinned
    // model leaves it empty, and the menu should show that, not a stale id.
    st.model = bridge_->model().toStdString();
    settings_menu_->setState(st, profiles_);

    persist_settings(home_, settings_menu_->state());
    updateChips();
}

void MainWindow::onParamsChanged() {
    // Rebuild rather than merge. set_params can only add or overwrite a
    // control, never remove one, so merging could not honour "effort: none" or
    // "thinking: backend default" — a fresh provider starts with nothing set
    // and then takes exactly the controls that are still on. Building one is
    // just an object construction; no I/O, no request.
    bridge_->reconnect(conn_, currentParams());
    persist_settings(home_, settings_menu_->state());
    updateChips();
}

void MainWindow::onThemeChanged(SyntaxTheme theme) {
    applyTheme();
    chat_->setTheme(theme);
    persist_settings(home_, settings_menu_->state());
}

void MainWindow::onFontsChanged() {
    applyFonts();
    // The stylesheet carries no font rules, but re-applying it is how Qt is
    // told to restyle after an application font change.
    applyTheme();
    persist_settings(home_, settings_menu_->state());
}

void MainWindow::onTextSizeStep(int delta) {
    SettingsState st = settings_menu_->state();
    if (delta == 0) {
        st.fonts.font_size = 0;
        st.fonts.chat_font_size = 0;
        st.fonts.mono_font_size = 0;
    } else {
        // Step from whatever is in effect, which for an unset size is the
        // platform default rather than zero.
        const int base = st.fonts.font_size > 0 ? st.fonts.font_size
                                                : default_ui_font_.pointSize();
        st.fonts.font_size = std::clamp(base + delta, kMinFontPt, kMaxFontPt);
        // The transcript and the code only follow along if they were pinned
        // separately; unpinned, applyFonts() derives them from the prose size
        // anyway and stepping them here would pin them by accident.
        if (st.fonts.chat_font_size > 0)
            st.fonts.chat_font_size =
                std::clamp(st.fonts.chat_font_size + delta, kMinFontPt, kMaxFontPt);
        if (st.fonts.mono_font_size > 0)
            st.fonts.mono_font_size =
                std::clamp(st.fonts.mono_font_size + delta, kMinFontPt, kMaxFontPt);
    }
    settings_menu_->setState(st, profiles_);
    applyFonts();
    applyTheme();
    persist_settings(home_, settings_menu_->state());
}

void MainWindow::onModelsRequested() {
    if (bridge_->busy()) return;
    chat_->addInfoMessage(tr("Asking %1 which models it serves…")
                              .arg(QString::fromStdString(conn_.base_url)));
    bridge_->detectModels();
}

void MainWindow::onModelsDetected(const QStringList& models) {
    setBusyUi(false);
    SettingsState st = settings_menu_->state();

    std::vector<std::string> ids;
    ids.reserve(static_cast<std::size_t>(models.size()));
    for (const QString& m : models) ids.push_back(m.toStdString());

    Profile* live = nullptr;
    for (Profile& p : profiles_)
        if (p.name == st.profile) live = &p;

    if (live) {
        // Honour the profile's blacklist, the same filter the TUI's detection
        // applies — advertised-but-dead ids should not reappear in the picker.
        live->models = filter_blacklisted(ids, live->blacklist);
        ids = live->models;
        // Persist only into a profile the user actually configured. Writing
        // detected models into a built-in fallback would materialise the whole
        // built-in table into a settings.toml that never had one.
        for (Profile& p : settings_.profiles) {
            if (p.name != live->name) continue;
            p.models = live->models;
            save_settings(home_, settings_);
            break;
        }
    }

    settings_menu_->setState(st, profiles_);
    chat_->addInfoMessage(
        ids.empty() ? tr("The endpoint advertised no models.")
                    : tr("%1 models available — pick one under Settings ▸ Model.")
                          .arg(ids.size()));
    updateChips();
}

void MainWindow::autosave() {
    if (home_.empty()) return;
    const Conversation& conv = bridge_->history();
    if (conv.empty()) return;

    std::error_code ec;
    const std::string cwd = std::filesystem::current_path(ec).string();
    if (conv_path_.empty()) {
        const std::string dir = conversations_dir(home_);
        if (dir.empty()) return;
        conv_path_ = dir + "/" + new_conversation_id(cwd) + ".toml";
    }

    if (conv_created_.empty()) conv_created_ = now_iso();
    ConvMeta meta;
    meta.cwd = cwd;
    meta.model = bridge_->model().toStdString();
    meta.created = conv_created_;
    meta.updated = now_iso();
    for (const Message& m : conv) {
        if (m.role() != Role::User) continue;
        meta.title = m.content().substr(0, 80);
        break;
    }
    // Best-effort, like the TUI's autosave: a transcript is not worth an error
    // dialog mid-conversation.
    (void)save_conversation(conv_path_, conv, meta);
}

bool MainWindow::eventFilter(QObject* obj, QEvent* e) {
    if (obj == input_ && e->type() == QEvent::KeyPress) {
        auto* ke = static_cast<QKeyEvent*>(e);
        const bool is_return =
            ke->key() == Qt::Key_Return || ke->key() == Qt::Key_Enter;
        // Enter sends; every modifier combination inserts a newline instead, so
        // the usual Shift+Enter reflex works and Ctrl+Enter does not surprise.
        if (is_return && ke->modifiers() == Qt::NoModifier) {
            onSend();
            return true;
        }
    }
    return QMainWindow::eventFilter(obj, e);
}

void MainWindow::closeEvent(QCloseEvent* e) {
    // Wait, don't just ask: cancel() only raises a flag, and autosave() reads
    // the conversation the worker thread is still appending to.
    bridge_->stopAndWait();
    autosave();
    QMainWindow::closeEvent(e);
}

}  // namespace moocode::gui
