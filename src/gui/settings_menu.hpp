#ifndef MOOCODE_GUI_SETTINGS_MENU_HPP
#define MOOCODE_GUI_SETTINGS_MENU_HPP

// The settings dropdown: profile, model, reasoning effort, thinking,
// temperature and theme, as one QMenu hung off a header-bar button.
//
// This is the GUI's answer to the TUI's slash commands (/provider, /model,
// /effort, /thinking, /temp, /theme) — the same knobs, reachable without
// knowing their names. It reads and writes the same ~/.moo/settings.toml, so
// the two frontends stay in sync.
//
// The menu is rebuilt from live state each time it is shown rather than kept
// in sync incrementally: it is a handful of actions, and a stale checkmark is
// exactly the bug worth designing out.

#include <QMenu>
#include <QObject>
#include <QString>

#include <optional>
#include <string>
#include <vector>

#include "agent/persist.hpp"  // Profile, Settings
#include "agent/types.hpp"    // SyntaxTheme

namespace moocode::gui {

// What the menu currently reflects, and what it edits.
struct SettingsState {
    std::string profile;                 // active profile name ("" => none)
    std::string model;                   // live model id
    std::string effort;                  // "low"/"medium"/"high"/"" (=> none)
    std::optional<bool> thinking;        // nullopt => backend default
    std::optional<double> temperature;   // nullopt => omitted from requests
    SyntaxTheme theme = SyntaxTheme::Default;
    GuiSettings fonts;                   // the [gui] table; empty/0 => defaults
};

class SettingsMenu : public QObject {
    Q_OBJECT

public:
    SettingsMenu(QMenu* menu, QObject* parent = nullptr);

    // Point the menu at the current state and profile list. Cheap; call
    // whenever either changes.
    void setState(const SettingsState& state, const std::vector<Profile>& profiles);

    const SettingsState& state() const { return state_; }

signals:
    // The endpoint changed: the caller must rebuild the provider. `profile` is
    // the profile to connect through and `model` the model to pin on it.
    void connectionRequested(const QString& profile, const QString& model);
    // A generation control changed; the caller applies it via set_params.
    void paramsChanged();
    // The theme changed; the caller re-themes the window and transcript.
    void themeChanged(SyntaxTheme theme);
    // A font family changed (or was reset); the caller re-applies both fonts.
    void fontsChanged();
    // Text size: +1 larger, -1 smaller, 0 reset. The caller does the arithmetic
    // because only it knows the startup defaults a reset must return to.
    void textSizeStep(int delta);
    // Ask the endpoint what models it serves (a network call, so the caller
    // runs it off the UI thread and reports back via setState).
    void modelsRequested();

private:
    void rebuild();
    const Profile* activeProfile() const;

    QMenu* menu_;
    SettingsState state_;
    std::vector<Profile> profiles_;
};

// The profile list to offer: the configured ones, or the built-ins when
// settings.toml has none — the same fallback listmodels --all uses.
std::vector<Profile> menu_profiles(const Settings& s);

// Write the parts of `state` that belong in settings.toml back to `home`,
// preserving everything else in the file. Best-effort, like save_settings.
void persist_settings(const std::string& home, const SettingsState& state);

}  // namespace moocode::gui

#endif  // MOOCODE_GUI_SETTINGS_MENU_HPP
