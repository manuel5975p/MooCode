// moogui — the Qt chat window onto moocode.
//
// Resolves a connection exactly the way listmodels and moocode do (profile >
// flags > LLM_* env), then hands it to a MainWindow. Chat only: no tools, no
// activity pane, no permission gate — see agent_bridge.hpp for why that is a
// deliberate boundary rather than an omission.

#include <QApplication>
#include <QIcon>

#include <cstdio>
#include <cstdlib>
#include <string>

#include "ProgramOptions.hxx"
#include "agent/persist.hpp"
#include "agent/provider.hpp"
#include "agent/provider_factory.hpp"
#include "gui/main_window.hpp"
#include "gui/settings_menu.hpp"

using namespace moocode;

namespace {

po::parser make_parser() {
    po::parser p;
    p["profile"].abbreviation('P').type(po::string).description(
        "Connect through this settings.toml [profiles.<name>]");
    p["model"].abbreviation('m').type(po::string).description(
        "Model id (overrides the profile's default)");
    p["base-url"].abbreviation('b').type(po::string).description(
        "Endpoint base URL (overrides the profile; else LLM_BASE_URL)");
    p["api-key"].abbreviation('k').type(po::string).description(
        "API key (overrides the profile's stored key; else LLM_API_KEY)");
    p["provider"].abbreviation('p').type(po::string).description(
        "Wire format: openai | anthropic | gemini | auto (default auto)");
    p["system"].type(po::string).description("System prompt (default: none)");
    p["help"].abbreviation('h').description("Show this help");
    return p;
}

// Non-const: po::parser::operator[] is a non-const accessor.
std::string flag(po::parser& cli, const char* name) {
    return cli[name].was_set() ? cli[name].get().string : std::string();
}

// Same precedence as listmodels: an explicit --provider wins, else the
// profile's declared kind, else detection from the base_url.
ProviderKind resolve_kind(const std::string& choice_str,
                          const std::string& profile_kind_str,
                          const std::string& base_url, const std::string& model) {
    ProviderChoice explicit_choice = parse_provider_choice(choice_str);
    ProviderChoice choice = explicit_choice != ProviderChoice::Auto
                                ? explicit_choice
                                : parse_provider_choice(profile_kind_str);
    switch (choice) {
        case ProviderChoice::Anthropic: return ProviderKind::Anthropic;
        case ProviderChoice::OpenAI:    return ProviderKind::OpenAI;
        case ProviderChoice::Gemini:    return ProviderKind::Gemini;
        default: return detect_provider_kind(base_url, model);
    }
}

}  // namespace

int main(int argc, char** argv) {
    po::parser cli = make_parser();
    if (!cli(argc, argv)) return 2;
    if (cli["help"].was_set()) {
        std::printf(
            "moogui — a chat window onto moocode\n\n"
            "usage: moogui [--profile <name>] [--model <id>] [--base-url URL]\n"
            "              [--api-key KEY] [--provider KIND] [--system TEXT]\n\n"
            "With no arguments, uses the active profile from settings.toml, else\n"
            "LLM_BASE_URL/LLM_API_KEY. Settings changed in the window are written\n"
            "back to settings.toml and shared with the moocode TUI.\n");
        return 0;
    }

    const std::string home = moocode_home();
    Settings settings = load_settings(home);
    const std::map<std::string, std::string> creds = load_credentials(home);

    const std::string flag_profile = flag(cli, "profile");
    const std::string flag_base = flag(cli, "base-url");
    const std::string flag_key = flag(cli, "api-key");
    const std::string flag_model = flag(cli, "model");
    const std::string flag_kind = flag(cli, "provider");

    const std::string want =
        !flag_profile.empty() ? flag_profile : settings.profile;

    ProviderConnection conn;
    gui::SettingsState state;
    const std::vector<Profile> profiles = gui::menu_profiles(settings);

    const Profile* active = nullptr;
    for (const Profile& p : profiles)
        if (p.name == want) active = &p;
    if (!want.empty() && !active) {
        std::fprintf(stderr, "no such profile: %s\n", want.c_str());
        return 2;
    }

    if (active) {
        conn.base_url = flag_base.empty() ? active->base_url : flag_base;
        conn.model = !flag_model.empty()   ? flag_model
                     : !settings.model.empty() && flag_profile.empty()
                         ? settings.model
                         : active->model;
        conn.thinking_type = active->thinking_type;
        if (!flag_key.empty()) {
            conn.api_key = flag_key;
        } else if (auto it = creds.find(active->name); it != creds.end()) {
            conn.api_key = it->second;
        }
        conn.kind = resolve_kind(flag_kind, active->kind, conn.base_url, conn.model);
        state.profile = active->name;
    } else {
        const char* env_base = std::getenv("LLM_BASE_URL");
        const char* env_key = std::getenv("LLM_API_KEY");
        conn.base_url = !flag_base.empty() ? flag_base : (env_base ? env_base : "");
        conn.api_key = !flag_key.empty() ? flag_key : (env_key ? env_key : "");
        conn.model = !flag_model.empty() ? flag_model : settings.model;
        conn.kind = resolve_kind(flag_kind, "", conn.base_url, conn.model);
    }

    if (conn.base_url.empty()) {
        std::fprintf(stderr,
                     "no base_url: set one with --base-url, LLM_BASE_URL, or by "
                     "configuring a profile in %s/settings.toml\n",
                     home.empty() ? "~/.moo" : home.c_str());
        return 2;
    }
    normalize_base_url(conn.base_url);
    conn.max_tokens = settings.max_tokens;
    conn.allowed_openai_params = settings.allowed_openai_params;
    conn.drop_reasoning_effort = settings.drop_reasoning_effort;

    // Generation controls: settings first, then the active profile's pins (a
    // profile can require a specific temperature — see the builtin kimi entry).
    GenerationParams gp;
    if (!settings.effort.empty()) gp.effort = settings.effort;
    if (settings.thinking >= 0) gp.thinking = settings.thinking == 1;
    if (settings.temperature >= 0) gp.temperature = settings.temperature;
    if (settings.max_tokens > 0) gp.max_tokens = settings.max_tokens;
    if (active) {
        if (active->thinking >= 0 && settings.thinking < 0)
            gp.thinking = active->thinking == 1;
        if (active->temperature >= 0 && settings.temperature < 0)
            gp.temperature = active->temperature;
    }

    state.model = conn.model;
    state.effort = settings.effort;
    state.thinking = gp.thinking;
    state.temperature = gp.temperature;
    if (auto t = syntax_theme_from_name(settings.theme)) state.theme = *t;
    state.fonts = settings.gui;

    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("moocode"));
    QApplication::setWindowIcon(QIcon(QStringLiteral(":/moocode.jpg")));

    gui::MainWindow w(home, std::move(settings), std::move(conn), std::move(gp),
                      std::move(state), flag(cli, "system"));
    w.show();
    return app.exec();
}
