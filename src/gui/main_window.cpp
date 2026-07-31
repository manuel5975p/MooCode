#include "gui/main_window.hpp"

#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QCloseEvent>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QListWidget>
#include <QMenu>
#include <QMimeData>
#include <QPixmap>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <cstddef>
#include <ctime>
#include <filesystem>
#include <utility>

#include "gui/agent_bridge.hpp"
#include "gui/chat_panel.hpp"
#include "gui/conversation_import.hpp"
#include "gui/theme.hpp"

namespace moocode::gui {
namespace {

// The composer grows with the text up to this many lines, then scrolls — long
// enough to draft a paragraph, short enough to leave the transcript readable.
constexpr int kMaxInputLines = 6;

// Vertical padding inside the composer, on top of the text lines themselves.
constexpr int kInputPadding = 18;

// Long edge of a staged image's chip thumbnail in the composer.
constexpr int kChipThumbSide = 34;

// How many images one turn may carry. Not a protocol limit — a guard against a
// stray multi-select drop turning into a request nobody meant to pay for.
constexpr std::size_t kMaxAttachments = 8;

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

// How many recent conversations the header menu lists inline before sending
// the user to the full browser.
constexpr int kRecentInMenu = 8;

// One line for a conversation in the menu / browser.
QString summary_label(const ConvSummary& s) {
    const QString title = s.title.empty()
                              ? QObject::tr("(untitled)")
                              : QString::fromStdString(s.title).simplified();
    return QStringLiteral("%1   ·  %2  (%3)")
        .arg(title, QString::fromStdString(s.updated))
        .arg(static_cast<int>(s.count));
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

    conversations_button_ = new QToolButton(header_);
    conversations_button_->setObjectName("mooSettings");  // same header styling
    conversations_button_->setText(QStringLiteral("Chats  ▾"));
    conversations_button_->setPopupMode(QToolButton::InstantPopup);
    conversations_button_->setCursor(Qt::PointingHandCursor);
    conversations_menu_ = new QMenu(conversations_button_);
    conversations_button_->setMenu(conversations_menu_);
    new_conversation_ = new QAction(tr("New conversation"), this);
    new_conversation_->setShortcut(QKeySequence::New);
    connect(new_conversation_, &QAction::triggered, this,
            &MainWindow::onNewConversation);
    addAction(new_conversation_);  // so Ctrl+N works without opening the menu
    // Rebuilt on show rather than kept in sync: the list also changes from
    // outside this window (the TUI saving in the same directory).
    connect(conversations_menu_, &QMenu::aboutToShow, this,
            &MainWindow::rebuildConversationsMenu);
    hbox->addWidget(conversations_button_);

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

    // Staged images sit above the input, in the order they will be sent. Hidden
    // while empty so the composer keeps its usual height.
    attach_strip_ = new QWidget(composer);
    attach_strip_->setObjectName("mooAttachStrip");
    attach_box_ = new QHBoxLayout(attach_strip_);
    attach_box_->setContentsMargins(0, 0, 0, 0);
    attach_box_->setSpacing(6);
    attach_strip_->setVisible(false);
    cbox->addWidget(attach_strip_);

    auto* rowbox = new QHBoxLayout();
    rowbox->setSpacing(8);
    input_ = new QPlainTextEdit(composer);
    input_->setObjectName("mooInput");
    input_->setPlaceholderText(QStringLiteral(
        "Message moocode…    (Enter to send, Shift+Enter for a newline, "
        "Ctrl+V to paste an image)"));
    input_->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    input_->installEventFilter(this);
    // Drops land on the viewport, not on the QPlainTextEdit itself: it is a
    // QAbstractScrollArea, and the viewport is the widget the drag actually
    // enters. Key presses still go to input_, which has the focus.
    input_->setAcceptDrops(true);
    input_->viewport()->installEventFilter(this);
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
    connect(settings_menu_, &SettingsMenu::systemPromptChanged, this,
            &MainWindow::onSystemPromptChanged);
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
    // Each family is honoured only if it can actually render text. The stored
    // value may predate the check, come from another machine that had the font,
    // or have been hand-edited — and applying an unusable family is not a
    // cosmetic mistake but the "every word drifts apart" bug (see
    // family_renders_text). The pinned *size* still applies either way.
    QFont ui = default_ui_font_;
    if (family_renders_text(QString::fromStdString(f.font)))
        ui.setFamily(QString::fromStdString(f.font));
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
    const bool chat_family = family_renders_text(QString::fromStdString(f.chat_font));
    const bool chat_split = chat_family || f.chat_font_size > 0;
    if (chat_family) chat.setFamily(QString::fromStdString(f.chat_font));
    if (f.chat_font_size > 0) chat.setPointSize(f.chat_font_size);
    // A default-constructed QFont is the "inherit" signal, so only a split
    // actually overrides the transcript.
    if (chat_) chat_->setProseFont(chat_split ? chat : QFont());

    // Code: an explicit family, or the platform fixed-pitch face tracking the
    // prose size so the two stay in proportion. It tracks the transcript's
    // prose, not the chrome's — a code block sits among the messages.
    QFont mono = default_mono_font_;
    if (family_renders_text(QString::fromStdString(f.mono_font)))
        mono.setFamily(QString::fromStdString(f.mono_font));
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
    // Changing the endpoint — or the conversation — mid-turn would race the
    // worker thread; the bridge ignores such calls, so disable the controls
    // rather than fail silently.
    settings_button_->setEnabled(!busy);
    conversations_button_->setEnabled(!busy);
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

void MainWindow::attachFromMime(const QMimeData* md) {
    AttachResult r = attach_images(md);
    for (const QString& e : r.errors) chat_->addErrorMessage(e);
    for (StagedImage& s : r.images) {
        if (attachments_.size() >= kMaxAttachments) {
            chat_->addInfoMessage(
                tr("At most %1 images per message — the rest were not attached.")
                    .arg(static_cast<int>(kMaxAttachments)));
            break;
        }
        s.id = next_attach_id_++;
        attachments_.push_back(std::move(s));
    }
    rebuildAttachStrip();
}

void MainWindow::rebuildAttachStrip() {
    while (QLayoutItem* item = attach_box_->takeAt(0)) {
        if (QWidget* w = item->widget()) w->deleteLater();
        delete item;
    }
    for (const StagedImage& s : attachments_) {
        auto* chip = new QWidget(attach_strip_);
        chip->setObjectName("mooAttachChip");
        auto* box = new QHBoxLayout(chip);
        box->setContentsMargins(4, 3, 4, 3);
        box->setSpacing(6);
        if (!s.preview.isNull()) {
            auto* thumb = new QLabel(chip);
            thumb->setPixmap(QPixmap::fromImage(s.preview).scaled(
                kChipThumbSide, kChipThumbSide, Qt::KeepAspectRatio,
                Qt::SmoothTransformation));
            box->addWidget(thumb);
        }
        auto* name = new QLabel(s.label, chip);
        name->setObjectName("mooAttachName");
        box->addWidget(name);
        auto* remove = new QToolButton(chip);
        remove->setObjectName("mooAttachRemove");
        remove->setText(QStringLiteral("✕"));
        remove->setCursor(Qt::PointingHandCursor);
        remove->setToolTip(tr("Remove this image"));
        const int id = s.id;
        connect(remove, &QToolButton::clicked, this, [this, id] {
            std::erase_if(attachments_,
                          [id](const StagedImage& a) { return a.id == id; });
            rebuildAttachStrip();
        });
        box->addWidget(remove);
        // Chips are shown at their natural width; the picture is the point, and
        // stretching them would make one image look like a progress bar.
        chip->setToolTip(QStringLiteral("%1  ·  %2")
                             .arg(s.label,
                                  QString::fromStdString(s.block.media_type)));
        attach_box_->addWidget(chip);
    }
    attach_box_->addStretch(1);
    attach_strip_->setVisible(!attachments_.empty());
}

void MainWindow::clearAttachments() {
    attachments_.clear();
    next_attach_id_ = 1;
    rebuildAttachStrip();
}

void MainWindow::onSend() {
    const QString text = input_->toPlainText().trimmed();
    if (bridge_->busy()) return;
    if (text.isEmpty() && attachments_.empty()) return;

    // Agent::run needs prose, so an image-only turn is given the same
    // "[image #N]" reference the TUI's chips leave behind once stripped. It is
    // also what the conversation file records for the turn: the bytes are not
    // saved, so without it a resumed transcript would show a blank user card.
    QString prose = text;
    if (prose.isEmpty()) {
        QStringList refs;
        for (const StagedImage& s : attachments_)
            refs << QStringLiteral("[image #%1]").arg(s.id);
        prose = refs.join(QLatin1Char(' '));
    }

    // Text first, then one part per image — the order the model sees them in.
    // Left empty for a text-only turn, which keeps that path byte-identical to
    // what it was before images existed.
    std::vector<ContentPart> parts;
    QVector<QImage> thumbs;
    if (!attachments_.empty()) {
        parts.push_back(ContentPart{.text = prose.toStdString(), .image = {}});
        for (StagedImage& s : attachments_) {
            parts.push_back(
                ContentPart{.text = {}, .image = std::move(s.block)});
            thumbs.push_back(s.preview);
        }
    }

    input_->clear();
    chat_->addUserMessage(prose, thumbs);
    clearAttachments();
    chat_->beginAssistantMessage();
    bridge_->send(prose, std::move(parts));
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

void MainWindow::onSystemPromptChanged() {
    const std::string& prompt = settings_menu_->state().system_prompt;
    if (!bridge_->setSystemPrompt(prompt)) return;  // busy; the menu is disabled
    persist_settings(home_, settings_menu_->state());
    chat_->addInfoMessage(prompt.empty()
                              ? tr("System prompt cleared.")
                              : tr("System prompt set (%1 characters).")
                                    .arg(static_cast<int>(prompt.size())));
}

void MainWindow::rebuildConversationsMenu() {
    conversations_menu_->clear();

    conversations_menu_->addAction(new_conversation_);

    std::error_code ec;
    const std::string cwd = std::filesystem::current_path(ec).string();
    const std::string dir = conversations_dir(home_);
    const std::vector<ConvSummary> here = list_conversations(dir, cwd);

    if (!here.empty()) {
        conversations_menu_->addSeparator();
        // The heading is a disabled action rather than a section title so the
        // whole menu reads the same on every platform style.
        QAction* head = conversations_menu_->addAction(tr("Recent in this folder"));
        head->setEnabled(false);
        const int n = std::min<int>(kRecentInMenu, static_cast<int>(here.size()));
        for (int i = 0; i < n; ++i) {
            const ConvSummary& s = here[static_cast<std::size_t>(i)];
            QAction* a = conversations_menu_->addAction(summary_label(s));
            a->setCheckable(true);
            a->setChecked(s.path == conv_path_);
            const std::string path = s.path;
            connect(a, &QAction::triggered, this,
                    [this, path] { openConversation(path); });
        }
    }

    conversations_menu_->addSeparator();
    QAction* browse = conversations_menu_->addAction(tr("All conversations…"));
    connect(browse, &QAction::triggered, this, &MainWindow::onBrowseConversations);
}

void MainWindow::onNewConversation() {
    if (!bridge_->setHistory({})) return;
    chat_->clear();
    clearAttachments();  // staged for a conversation that no longer exists
    conv_path_.clear();
    conv_created_.clear();
    in_tokens_ = 0;
    out_tokens_ = 0;
    updateChips();
    input_->setFocus();
}

bool MainWindow::continueLastConversation() {
    std::error_code ec;
    const std::vector<ConvSummary> here = list_conversations(
        conversations_dir(home_), std::filesystem::current_path(ec).string());
    if (here.empty()) return false;
    openConversation(here.front().path);
    return true;
}

void MainWindow::onBrowseConversations() {
    // Everything, not just this folder: the point of the browser is reaching
    // the conversation the recents list does not have.
    const std::vector<ConvSummary> all =
        list_conversations(conversations_dir(home_), std::string());
    if (all.empty()) {
        chat_->addInfoMessage(tr("No saved conversations yet."));
        return;
    }

    QDialog dlg(this);
    dlg.setWindowTitle(tr("Conversations"));
    dlg.resize(640, 420);
    auto* box = new QVBoxLayout(&dlg);
    auto* list = new QListWidget(&dlg);
    for (const ConvSummary& s : all) list->addItem(summary_label(s));
    list->setCurrentRow(0);
    box->addWidget(list, 1);
    auto* buttons = new QDialogButtonBox(
        QDialogButtonBox::Open | QDialogButtonBox::Cancel, &dlg);
    box->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    connect(list, &QListWidget::itemDoubleClicked, &dlg, &QDialog::accept);

    if (dlg.exec() != QDialog::Accepted) return;
    const int row = list->currentRow();
    if (row < 0 || row >= static_cast<int>(all.size())) return;
    openConversation(all[static_cast<std::size_t>(row)].path);
}

void MainWindow::openConversation(const std::string& path) {
    if (bridge_->busy()) return;
    auto loaded = load_conversation_with_meta(path);
    if (!loaded) {
        chat_->addErrorMessage(QString::fromStdString(loaded.error().msg));
        return;
    }

    bool folded = false;
    Conversation conv = to_chat_only(loaded->first, folded);
    if (!bridge_->setHistory(conv)) return;

    chat_->clear();
    renderConversation(conv);

    // Continue in the same file only when nothing was lost in the rewrite.
    // Saving a folded conversation back over its source would replace the
    // TUI's tool calls and results with this transcript's prose summary of
    // them — silently, and the next TUI /resume would find them gone.
    conv_path_ = folded ? std::string() : path;
    conv_created_ = folded ? std::string() : loaded->second.created;
    // The counters measure this window's spend; a resumed conversation has
    // none yet, and the saved file does not record the old totals.
    in_tokens_ = 0;
    out_tokens_ = 0;
    updateChips();

    if (folded)
        chat_->addInfoMessage(
            tr("Resumed from a tool-using session: the tool calls and results "
               "were folded into the transcript above, and this window will "
               "save its continuation as a new conversation."));
    input_->setFocus();
}

void MainWindow::renderConversation(const Conversation& conv) {
    for (const Message& m : conv) {
        switch (m.role()) {
            case Role::System:
                // Held in the history, not shown: it is configuration (Settings
                // ▸ System prompt), not something either party said.
                break;
            case Role::User: {
                // Multimodal user turns keep only their text: the image bytes
                // are not saved with the conversation.
                std::string text = m.content();
                if (text.empty())
                    for (const ContentPart& p : m.parts())
                        if (!p.text.empty()) text += p.text;
                chat_->addUserMessage(QString::fromStdString(text));
                break;
            }
            case Role::Assistant:
                chat_->addAssistantMessage(QString::fromStdString(m.content()),
                                           QString::fromStdString(m.reasoning()));
                break;
            case Role::Tool:
                break;  // to_chat_only() has already folded these away
        }
    }
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
        // Paste: an image in the clipboard is attached instead of pasted, since
        // QPlainTextEdit would either drop it or insert its file path as text.
        // The image wins over any text the same payload also carries (copying a
        // picture from a browser offers both, and the picture is what was
        // meant); the context menu's own Paste is not filtered, so the text is
        // still reachable.
        if (ke->matches(QKeySequence::Paste)) {
            const QMimeData* md = QApplication::clipboard()->mimeData();
            if (has_attachable_image(md)) {
                attachFromMime(md);
                return true;
            }
        }
    }

    // Dropping an image file onto the composer attaches it, rather than
    // inserting the file:// URL as text. Both DragEnter and DragMove must accept
    // or no drop is ever delivered.
    if (obj == input_->viewport()) {
        switch (e->type()) {
            case QEvent::DragEnter:
            case QEvent::DragMove: {
                auto* de = static_cast<QDragMoveEvent*>(e);
                if (has_attachable_image(de->mimeData())) {
                    de->acceptProposedAction();
                    return true;
                }
                break;
            }
            case QEvent::Drop: {
                auto* de = static_cast<QDropEvent*>(e);
                if (has_attachable_image(de->mimeData())) {
                    attachFromMime(de->mimeData());
                    de->acceptProposedAction();
                    input_->setFocus();
                    return true;
                }
                break;
            }
            default:
                break;
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
