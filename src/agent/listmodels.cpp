// listmodels — standalone endpoint probe. Resolves a connection the same way
// moocode does (profile > flags > LLM_* env), then calls Provider::list_models()
// and prints the ids one per line. No agent loop, no tools, no TUI: this is the
// "is my key/base_url/wire format right?" utility, and the plain one-id-per-line
// stdout is meant to be piped.
//
// Deliberately does NOT apply a profile's `blacklist`: the point is to show what
// the endpoint actually advertises, including entries moocode would filter out.

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

#include "ProgramOptions.hxx"
#include "agent/persist.hpp"
#include "agent/provider.hpp"
#include "agent/provider_factory.hpp"
#include "agent/strutil.hpp"

using namespace moocode;

namespace {

po::parser make_parser() {
    po::parser p;
    p["profile"].abbreviation('P').type(po::string).description(
        "Probe this settings.toml [profiles.<name>] (key from credentials.toml)");
    p["all"].abbreviation('a').description(
        "Probe every configured profile in turn (built-ins when none are "
        "configured); failures are reported per profile and do not abort");
    p["base-url"].abbreviation('b').type(po::string).description(
        "Endpoint base URL (overrides the profile; else LLM_BASE_URL)");
    p["api-key"].abbreviation('k').type(po::string).description(
        "API key (overrides the profile's stored key; else LLM_API_KEY)");
    p["provider"].abbreviation('p').type(po::string).description(
        "Wire format: openai | anthropic | gemini | auto (default auto: "
        "detect from base-url)");
    p["help"].abbreviation('h').description("Show this help");
    return p;
}

// Resolve `kind` the same way main.cpp does: an explicit --provider wins, else
// the profile's declared kind, else detection from the base_url.
ProviderKind resolve_kind(const std::string& choice_str,
                          const std::string& profile_kind,
                          const std::string& base_url) {
    ProviderChoice explicit_choice = parse_provider_choice(choice_str);
    ProviderChoice choice = explicit_choice != ProviderChoice::Auto
                                ? explicit_choice
                                : parse_provider_choice(profile_kind);
    switch (choice) {
        case ProviderChoice::Anthropic: return ProviderKind::Anthropic;
        case ProviderChoice::OpenAI:    return ProviderKind::OpenAI;
        case ProviderChoice::Gemini:    return ProviderKind::Gemini;
        default: return detect_provider_kind(base_url, /*model=*/"");
    }
}

const char* kind_name(ProviderKind k) {
    switch (k) {
        case ProviderKind::Anthropic: return "anthropic";
        case ProviderKind::Gemini:    return "gemini";
        default:                      return "openai";
    }
}

// Probe one connection. Returns false on error (already reported to stderr), so
// --all can keep going and main() can pick a nonzero exit.
bool probe(const std::string& label, ProviderConnection conn) {
    if (conn.base_url.empty()) {
        std::fprintf(stderr, "%s: no base_url (set --base-url, LLM_BASE_URL, "
                             "or use --profile)\n", label.c_str());
        return false;
    }
    normalize_base_url(conn.base_url);
    std::unique_ptr<Provider> prov = make_provider(conn, GenerationParams{});
    auto models = prov->list_models();
    if (!models) {
        std::fprintf(stderr, "%s: %s\n", label.c_str(), models.error().msg.c_str());
        return false;
    }
    // Header to stderr so stdout stays a clean, pipeable id list.
    std::fprintf(stderr, "\033[2m[%s · %s · %zu models]\033[0m\n", label.c_str(),
                 kind_name(conn.kind), models->size());
    for (const std::string& m : *models) std::printf("%s\n", m.c_str());
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    po::parser cli = make_parser();
    if (!cli(argc, argv)) return 2;
    if (cli["help"].was_set()) {
        std::cout << "listmodels — list the models an endpoint advertises\n\n"
                  << "usage: listmodels [--profile <name> | --all] "
                     "[--base-url URL] [--api-key KEY] [--provider KIND]\n\n"
                  << cli << "\nWith no arguments, uses the active profile from "
                     "settings.toml, else LLM_BASE_URL/LLM_API_KEY.\n";
        return 0;
    }

    const std::string home = moocode_home();
    const Settings settings = load_settings(home);
    const std::map<std::string, std::string> creds = load_credentials(home);

    const std::string flag_base = cli["base-url"].was_set()
                                      ? cli["base-url"].get().string
                                      : std::string();
    const std::string flag_key = cli["api-key"].was_set()
                                     ? cli["api-key"].get().string
                                     : std::string();
    const std::string flag_kind = cli["provider"].was_set()
                                      ? cli["provider"].get().string
                                      : std::string();

    // Build a connection from a profile, with flags overriding each field.
    auto from_profile = [&](const Profile& p) {
        ProviderConnection c;
        c.base_url = flag_base.empty() ? p.base_url : flag_base;
        c.model = p.model;  // unused by list_models, but keeps the ctor honest
        c.thinking_type = p.thinking_type;
        if (!flag_key.empty()) {
            c.api_key = flag_key;
        } else if (auto it = creds.find(p.name); it != creds.end()) {
            c.api_key = it->second;
        }
        c.kind = resolve_kind(flag_kind, p.kind, c.base_url);
        return c;
    };

    if (cli["all"].was_set()) {
        std::vector<Profile> all = settings.profiles;
        if (all.empty()) all = builtin_profiles();
        bool any_ok = false;
        for (const Profile& p : all)
            if (probe(p.name, from_profile(p))) any_ok = true;
        return any_ok ? 0 : 1;
    }

    // Single probe: --profile > settings.profile > bare flags/env.
    const std::string want = cli["profile"].was_set()
                                 ? cli["profile"].get().string
                                 : settings.profile;
    if (!want.empty()) {
        for (const Profile& p : settings.profiles)
            if (p.name == want) return probe(p.name, from_profile(p)) ? 0 : 1;
        for (const Profile& p : builtin_profiles())
            if (p.name == want) return probe(p.name, from_profile(p)) ? 0 : 1;
        std::fprintf(stderr, "no such profile: %s\n", want.c_str());
        return 2;
    }

    ProviderConnection c;
    const char* env_base = get_env("LLM_BASE_URL");
    const char* env_key = get_env("LLM_API_KEY");
    c.base_url = !flag_base.empty() ? flag_base : (env_base ? env_base : "");
    c.api_key = !flag_key.empty() ? flag_key : (env_key ? env_key : "");
    c.kind = resolve_kind(flag_kind, /*profile_kind=*/"", c.base_url);
    // Label copied before the move: `c` is gone by the time probe reads it.
    const std::string label = c.base_url;
    return probe(label, std::move(c)) ? 0 : 1;
}
