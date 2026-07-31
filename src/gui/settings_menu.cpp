#include "gui/settings_menu.hpp"

#include <QActionGroup>
#include <QApplication>
#include <QFontDatabase>
#include <QFontDialog>
#include <QInputDialog>
#include <QKeySequence>
#include <QLineEdit>
#include <QMessageBox>

#include "agent/persist.hpp"

namespace moocode::gui {
namespace {

// A submenu of mutually exclusive choices. `current` is checked; picking one
// calls `on_pick` with the chosen value.
template <class F>
QMenu* choice_submenu(QMenu* parent, const QString& title,
                      const std::vector<std::pair<QString, std::string>>& items,
                      const std::string& current, F on_pick) {
    QMenu* sub = parent->addMenu(title);
    auto* group = new QActionGroup(sub);
    group->setExclusive(true);
    for (const auto& [label, value] : items) {
        QAction* a = sub->addAction(label);
        a->setCheckable(true);
        a->setChecked(value == current);
        group->addAction(a);
        const std::string v = value;
        QObject::connect(a, &QAction::triggered, sub, [on_pick, v] { on_pick(v); });
    }
    return sub;
}

// Refuse a pick that cannot render text, and say why rather than leaving the
// user to wonder what happened to their transcript.
bool accept_font(QWidget* parent, const QFont& chosen) {
    if (family_renders_text(chosen.family())) return true;
    QMessageBox::warning(
        parent, SettingsMenu::tr("Font"),
        SettingsMenu::tr(
            "“%1” has no letters — it covers only symbols or emoji.\n\n"
            "Text would be drawn in a substitute face while the spaces came "
            "from this font, which would push the words apart. Keeping the "
            "previous font.")
            .arg(chosen.family()));
    return false;
}

QString or_dash(const std::string& s) {
    return s.empty() ? QStringLiteral("—") : QString::fromStdString(s);
}

// A stored family/size pair as a QFont, filling either half from the
// application font when it is unset.
QFont effective_font(const std::string& family, int size) {
    QFont f = QApplication::font();
    // Same guard as applyFonts: a stored family that cannot render text is not
    // what the window is drawing, so seeding the dialog with it would open on a
    // lie (and on a font the user is about to be told they cannot have).
    if (family_renders_text(QString::fromStdString(family)))
        f.setFamily(QString::fromStdString(family));
    if (size > 0) f.setPointSize(size);
    return f;
}

// Menu label for a font: "Inter 13", "13" (size only), or "system default".
QString font_label(const std::string& family, int size) {
    if (family.empty() && size <= 0) return SettingsMenu::tr("system default");
    if (family.empty()) return QString::number(size);
    if (size <= 0) return QString::fromStdString(family);
    return QString("%1 %2").arg(QString::fromStdString(family)).arg(size);
}

}  // namespace

bool family_renders_text(const QString& family) {
    if (family.isEmpty()) return false;
    // writingSystems() reports what the face actually covers, and it knows
    // about fonts registered from the binary too (the bundled EB Garamond), so
    // the shipped default passes when a user picks it by name. An unknown
    // family reports nothing at all, which is the answer we want for it.
    return QFontDatabase::writingSystems(family).contains(QFontDatabase::Latin);
}

std::vector<Profile> menu_profiles(const Settings& s) {
    return s.profiles.empty() ? builtin_profiles() : s.profiles;
}

void persist_settings(const std::string& home, const SettingsState& state) {
    if (home.empty()) return;
    // Round-trip through the loaded file so fields the GUI does not edit
    // (profiles, allowed_openai_params, max_tokens, …) survive the write.
    Settings s = load_settings(home);
    s.profile = state.profile;
    s.model = state.model;
    s.effort = state.effort;
    s.theme = std::string(syntax_theme_name(state.theme));
    s.thinking = state.thinking ? (*state.thinking ? 1 : 0) : -1;
    s.temperature = state.temperature ? *state.temperature : -1;
    s.gui = state.fonts;
    s.gui.system_prompt = state.system_prompt;
    save_settings(home, s);
}

SettingsMenu::SettingsMenu(QMenu* menu, QObject* parent)
    : QObject(parent), menu_(menu) {
    // Rebuilding on aboutToShow is what keeps the checkmarks honest: the model
    // can also change from outside the menu (startup detection, a profile
    // switch pinning its own default).
    connect(menu_, &QMenu::aboutToShow, this, &SettingsMenu::rebuild);
}

void SettingsMenu::setState(const SettingsState& state,
                            const std::vector<Profile>& profiles) {
    state_ = state;
    profiles_ = profiles;
}

const Profile* SettingsMenu::activeProfile() const {
    for (const Profile& p : profiles_)
        if (p.name == state_.profile) return &p;
    return nullptr;
}

void SettingsMenu::rebuild() {
    // QMenu::clear() deletes the actions, but a submenu's QMenu is a child of
    // this menu and outlives its action — rebuilding on every show would pile
    // up an orphaned QMenu per submenu per open.
    qDeleteAll(menu_->findChildren<QMenu*>(QString(), Qt::FindDirectChildrenOnly));
    menu_->clear();

    // --- profile ---
    {
        std::vector<std::pair<QString, std::string>> items;
        items.reserve(profiles_.size());
        for (const Profile& p : profiles_)
            items.emplace_back(QString::fromStdString(p.name), p.name);
        choice_submenu(menu_, tr("Profile  (%1)").arg(or_dash(state_.profile)), items,
                       state_.profile, [this](const std::string& name) {
                           if (name == state_.profile) return;
                           state_.profile = name;
                           // Let the profile's own default model win; the caller
                           // reports back what it actually connected with.
                           const Profile* p = activeProfile();
                           state_.model = p ? p->model : std::string();
                           emit connectionRequested(QString::fromStdString(name),
                                                    QString::fromStdString(state_.model));
                       });
    }

    // --- model (of the active profile) ---
    {
        const Profile* p = activeProfile();
        std::vector<std::pair<QString, std::string>> items;
        if (p) {
            // The profile's declared model may not appear in its models list;
            // offer it either way so the live choice is always representable.
            const std::vector<std::string> models =
                filter_blacklisted(p->models, p->blacklist);
            bool has_live = false;
            for (const std::string& m : models) {
                items.emplace_back(QString::fromStdString(m), m);
                if (m == state_.model) has_live = true;
            }
            if (!state_.model.empty() && !has_live)
                items.emplace_back(QString::fromStdString(state_.model), state_.model);
        }
        QMenu* sub = choice_submenu(
            menu_, tr("Model  (%1)").arg(or_dash(state_.model)), items, state_.model,
            [this](const std::string& m) {
                if (m == state_.model) return;
                state_.model = m;
                emit connectionRequested(QString::fromStdString(state_.profile),
                                         QString::fromStdString(m));
            });

        // A profile need not declare its models, and an endpoint can serve more
        // than the ones written down — so the list is never the only way in.
        if (!items.empty()) sub->addSeparator();

        QAction* detect = sub->addAction(tr("Detect from endpoint…"));
        connect(detect, &QAction::triggered, this,
                [this] { emit modelsRequested(); });

        QAction* custom = sub->addAction(tr("Enter model id…"));
        connect(custom, &QAction::triggered, this, [this] {
            bool ok = false;
            const QString m = QInputDialog::getText(
                menu_, tr("Model"), tr("Model id:"), QLineEdit::Normal,
                QString::fromStdString(state_.model), &ok);
            const std::string want = m.trimmed().toStdString();
            if (!ok || want.empty() || want == state_.model) return;
            state_.model = want;
            emit connectionRequested(QString::fromStdString(state_.profile),
                                     QString::fromStdString(want));
        });
    }

    menu_->addSeparator();

    // --- effort ---
    choice_submenu(menu_, tr("Effort  (%1)").arg(state_.effort.empty()
                                                     ? QStringLiteral("none")
                                                     : QString::fromStdString(state_.effort)),
                   {{tr("none"), ""},
                    {tr("low"), "low"},
                    {tr("medium"), "medium"},
                    {tr("high"), "high"}},
                   state_.effort, [this](const std::string& e) {
                       if (e == state_.effort) return;
                       state_.effort = e;
                       emit paramsChanged();
                   });

    // --- thinking ---
    {
        const std::string cur =
            state_.thinking ? (*state_.thinking ? "on" : "off") : "default";
        choice_submenu(menu_, tr("Thinking  (%1)").arg(QString::fromStdString(cur)),
                       {{tr("backend default"), "default"},
                        {tr("on"), "on"},
                        {tr("off"), "off"}},
                       cur, [this](const std::string& v) {
                           std::optional<bool> next;
                           if (v == "on") next = true;
                           else if (v == "off") next = false;
                           if (next == state_.thinking) return;
                           state_.thinking = next;
                           emit paramsChanged();
                       });
    }

    // --- temperature ---
    {
        const QString label =
            state_.temperature
                ? tr("Temperature…  (%1)").arg(*state_.temperature, 0, 'g', 3)
                : tr("Temperature…  (unset)");
        QAction* a = menu_->addAction(label);
        connect(a, &QAction::triggered, this, [this] {
            bool ok = false;
            // -1 is the project's "unset" sentinel for temperature, on the wire
            // and on disk; surfacing it here keeps the control reversible.
            const double cur = state_.temperature ? *state_.temperature : -1.0;
            const double v = QInputDialog::getDouble(
                menu_, tr("Temperature"),
                tr("Sampling temperature (negative = unset, omit from requests):"),
                cur, -1.0, 2.0, 2, &ok);
            if (!ok) return;
            std::optional<double> next;
            if (v >= 0.0) next = v;
            if (next == state_.temperature) return;
            state_.temperature = next;
            emit paramsChanged();
        });
    }

    // --- system prompt ---
    {
        const QString label =
            state_.system_prompt.empty()
                ? tr("System prompt…  (none)")
                : tr("System prompt…  (%1 chars)")
                      .arg(static_cast<int>(state_.system_prompt.size()));
        QAction* a = menu_->addAction(label);
        connect(a, &QAction::triggered, this, [this] {
            bool ok = false;
            const QString text = QInputDialog::getMultiLineText(
                menu_, tr("System prompt"),
                tr("Sent as the conversation's system message.\n"
                   "Applies to the current conversation and every new one; "
                   "leave empty for none."),
                QString::fromStdString(state_.system_prompt), &ok);
            if (!ok) return;
            // Trailing whitespace only would be an invisible non-empty prompt.
            const std::string next = text.trimmed().toStdString();
            if (next == state_.system_prompt) return;
            state_.system_prompt = next;
            emit systemPromptChanged();
        });
    }

    menu_->addSeparator();

    // --- appearance: theme + fonts ---
    {
        QMenu* look = menu_->addMenu(tr("Appearance"));

        std::vector<std::pair<QString, std::string>> items;
        for (const std::string& n : syntax_theme_names())
            items.emplace_back(QString::fromStdString(n), n);
        choice_submenu(look,
                       tr("Theme  (%1)").arg(QString::fromStdString(
                           std::string(syntax_theme_name(state_.theme)))),
                       items, std::string(syntax_theme_name(state_.theme)),
                       [this](const std::string& n) {
                           auto t = syntax_theme_from_name(n);
                           if (!t || *t == state_.theme) return;
                           state_.theme = *t;
                           emit themeChanged(*t);
                       });

        look->addSeparator();

        QAction* ui_font = look->addAction(
            tr("Interface font…  (%1)").arg(font_label(state_.fonts.font,
                                                       state_.fonts.font_size)));
        connect(ui_font, &QAction::triggered, this, [this] {
            bool ok = false;
            const QFont chosen = QFontDialog::getFont(
                &ok, effective_font(state_.fonts.font, state_.fonts.font_size),
                menu_, tr("Interface font"));
            if (!ok || !accept_font(menu_, chosen)) return;
            state_.fonts.font = chosen.family().toStdString();
            state_.fonts.font_size = chosen.pointSize();
            emit fontsChanged();
        });

        QAction* chat_font = look->addAction(
            tr("Chat font…  (%1)").arg(state_.fonts.chat_font.empty() &&
                                               state_.fonts.chat_font_size <= 0
                                           ? tr("same as interface")
                                           : font_label(state_.fonts.chat_font,
                                                        state_.fonts.chat_font_size)));
        connect(chat_font, &QAction::triggered, this, [this] {
            bool ok = false;
            // Seeded from the interface font when unset, so the dialog opens on
            // what the transcript is actually drawn in today.
            const QFont current =
                state_.fonts.chat_font.empty() && state_.fonts.chat_font_size <= 0
                    ? effective_font(state_.fonts.font, state_.fonts.font_size)
                    : effective_font(state_.fonts.chat_font, state_.fonts.chat_font_size);
            const QFont chosen =
                QFontDialog::getFont(&ok, current, menu_, tr("Chat font"));
            if (!ok || !accept_font(menu_, chosen)) return;
            state_.fonts.chat_font = chosen.family().toStdString();
            state_.fonts.chat_font_size = chosen.pointSize();
            emit fontsChanged();
        });

        QAction* code_font = look->addAction(
            tr("Code font…  (%1)").arg(font_label(state_.fonts.mono_font,
                                                  state_.fonts.mono_font_size)));
        connect(code_font, &QAction::triggered, this, [this] {
            bool ok = false;
            QFont current = effective_font(state_.fonts.mono_font,
                                           state_.fonts.mono_font_size);
            if (state_.fonts.mono_font.empty())
                current = QFontDatabase::systemFont(QFontDatabase::FixedFont);
            // Monospace-only: a proportional face in a code block destroys the
            // alignment the highlighting is there to make readable.
            const QFont chosen = QFontDialog::getFont(
                &ok, current, menu_, tr("Code font"),
                QFontDialog::MonospacedFonts);
            if (!ok || !accept_font(menu_, chosen)) return;
            state_.fonts.mono_font = chosen.family().toStdString();
            state_.fonts.mono_font_size = chosen.pointSize();
            emit fontsChanged();
        });

        look->addSeparator();

        QAction* bigger = look->addAction(tr("Larger text"));
        bigger->setShortcut(QKeySequence::ZoomIn);
        connect(bigger, &QAction::triggered, this, [this] { emit textSizeStep(1); });

        QAction* smaller = look->addAction(tr("Smaller text"));
        smaller->setShortcut(QKeySequence::ZoomOut);
        connect(smaller, &QAction::triggered, this, [this] { emit textSizeStep(-1); });

        QAction* reset = look->addAction(tr("Reset appearance"));
        connect(reset, &QAction::triggered, this, [this] {
            state_.fonts = GuiSettings{};
            emit fontsChanged();
        });
    }
}

}  // namespace moocode::gui
