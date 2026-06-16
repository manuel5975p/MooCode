#include "agent/onboarding.hpp"

#include <algorithm>
#include <filesystem>
#include <istream>
#include <ostream>
#include <string>
#include <string_view>

#include "agent/provider.hpp"  // make ProviderKind / detect_provider_kind / names
#include "agent/strutil.hpp"   // to_lower

namespace moocode {

namespace {

namespace fs = std::filesystem;

// Trim leading/trailing ASCII whitespace.
std::string trim(std::string_view s) {
    std::size_t b = 0, e = s.size();
    while (b < e && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r' || s[b] == '\n')) ++b;
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r' ||
                     s[e - 1] == '\n'))
        --e;
    return std::string(s.substr(b, e - b));
}

// Read one trimmed line. Returns false at EOF (so the caller can abort cleanly),
// in which case `out_line` is left empty.
bool read_line(std::istream& in, std::string& out_line) {
    std::string raw;
    if (!std::getline(in, raw)) {
        out_line.clear();
        return false;
    }
    out_line = trim(raw);
    return true;
}

// Prompt with a default shown in brackets; an empty answer keeps the default.
// Returns false at EOF.
bool prompt_default(std::istream& in, std::ostream& out, std::string_view label,
                    const std::string& def, std::string& result) {
    out << label;
    if (!def.empty()) out << " [" << def << "]";
    out << ": ";
    out.flush();
    std::string line;
    if (!read_line(in, line)) return false;
    result = line.empty() ? def : line;
    return true;
}

// Parse the kind a user typed for a custom endpoint. Empty/"auto" => detect from
// the URL. Returns the concrete wire-format name to store in the profile.
std::string resolve_kind(std::string_view typed, const std::string& base_url,
                         const std::string& model) {
    ProviderChoice c = parse_provider_choice(typed);
    ProviderKind k = c == ProviderChoice::Anthropic ? ProviderKind::Anthropic
                     : c == ProviderChoice::OpenAI  ? ProviderKind::OpenAI
                     : c == ProviderChoice::Gemini  ? ProviderKind::Gemini
                                  : detect_provider_kind(base_url, model);
    return provider_kind_name(k);
}

// Resolve the user's model pick against the listed ids: a 1-based index into
// `models`, or a typed name (returned verbatim, so the user can override the
// listing), or empty to take `fallback`. pre: fallback chosen by the caller.
std::string resolve_model_choice(std::string_view input,
                                 const std::vector<std::string>& models,
                                 const std::string& fallback) {
    std::string s = trim(input);
    if (s.empty()) return fallback;
    // All-digits => index into the list (1-based).
    if (std::ranges::all_of(s, [](char c) { return c >= '0' && c <= '9'; })) {
        long idx = std::stol(s);
        if (idx >= 1 && static_cast<std::size_t>(idx) <= models.size())
            return models[static_cast<std::size_t>(idx) - 1];
    }
    return s;  // a typed model name (may be one not advertised)
}

constexpr std::size_t kMaxListed = 40;  // cap the printed model list

}  // namespace

std::expected<std::vector<std::string>, Error> default_model_lister(
    const ProviderConnection& conn) {
    return make_provider(conn, GenerationParams{})->list_models();
}

bool onboarding_needed(const std::string& home, bool interactive,
                       bool has_explicit_config) {
    if (!interactive || home.empty() || has_explicit_config) return false;
    std::error_code ec;
    return !fs::exists(fs::path(home) / "settings.toml", ec);
}

Settings onboarding_settings(const std::string& profile_name,
                             const std::string& kind,
                             const std::string& base_url,
                             const std::string& model,
                             const std::vector<std::string>& models) {
    Settings s;
    Profile p;
    p.name = profile_name;
    p.kind = kind;
    p.base_url = base_url;
    p.model = model;
    p.models = models;
    s.profiles.push_back(std::move(p));
    s.profile = profile_name;
    s.model = model;
    s.provider = kind;
    return s;
}

OnboardingOutcome run_onboarding(const std::string& home, std::istream& in,
                                 std::ostream& out, const ModelLister& lister) {
    OnboardingOutcome outcome;
    const std::vector<Profile>& presets = builtin_profiles();

    out << "\n=== moocode first-run setup ===\n"
        << "No configuration found under " << home << ".\n"
        << "Let's connect to a model provider. Press Enter to accept a [default];"
        << " type 's' at any prompt to skip and use built-in defaults.\n\n";

    // --- Step 1: pick a known provider or a custom endpoint -------------------
    out << "Providers:\n";
    for (std::size_t i = 0; i < presets.size(); ++i)
        out << "  " << (i + 1) << ") " << presets[i].name << "  ("
            << presets[i].base_url << ")\n";
    out << "  c) custom endpoint\n"
        << "  s) skip\n";

    std::string profile_name = "custom";
    std::string kind;       // wire-format name to store
    std::string base_url;   // seed endpoint
    std::string model;      // suggested default model
    std::vector<std::string> seed_models;

    {
        std::string sel;
        if (!prompt_default(in, out, "Choose [1-" + std::to_string(presets.size()) +
                                         ", c, s]",
                            "1", sel)) {
            outcome.cancelled = true;
            return outcome;
        }
        std::string low = to_lower(sel);
        if (low == "s" || low == "skip") {
            outcome.cancelled = true;
            out << "Skipped — using built-in defaults.\n";
            return outcome;
        }
        if (low == "c" || low == "custom") {
            // Custom: take the endpoint now; kind is detected (or overridden) once
            // we know the URL, inside the probe loop below.
            profile_name = "custom";
        } else {
            // A preset number; out-of-range falls back to the first.
            std::size_t idx = 0;
            if (std::ranges::all_of(low, [](char c) { return c >= '0' && c <= '9'; }) &&
                !low.empty())
                idx = static_cast<std::size_t>(std::stol(low)) - 1;
            if (idx >= presets.size()) idx = 0;
            const Profile& p = presets[idx];
            profile_name = p.name;
            kind = p.kind;
            base_url = p.base_url;
            model = p.model;
            seed_models = p.models;
        }
    }

    // --- Step 2: endpoint + key + probe, looping so the user can adapt --------
    std::string api_key;
    std::vector<std::string> models;
    for (;;) {
        if (!prompt_default(in, out, "API endpoint (base URL)", base_url, base_url) ||
            to_lower(base_url) == "s") {
            outcome.cancelled = true;
            return outcome;
        }
        // For a custom endpoint, confirm/override the wire format now that the
        // URL is known. Presets carry their own kind, so don't re-ask there.
        if (kind.empty() || profile_name == "custom") {
            std::string detected = resolve_kind("", base_url, model);
            std::string typed;
            if (!prompt_default(in, out,
                                "Wire format (openai/anthropic/gemini)", detected,
                                typed)) {
                outcome.cancelled = true;
                return outcome;
            }
            kind = resolve_kind(typed, base_url, model);
        }

        std::string key_prompt = api_key.empty()
                                     ? std::string("API key")
                                     : std::string("API key [keep current]");
        out << key_prompt << ": ";
        out.flush();
        std::string key_in;
        if (!read_line(in, key_in)) {
            outcome.cancelled = true;
            return outcome;
        }
        if (to_lower(key_in) == "s") {
            outcome.cancelled = true;
            return outcome;
        }
        if (!key_in.empty()) api_key = key_in;

        out << "Probing " << base_url << " …\n";
        out.flush();
        ProviderConnection conn;
        conn.kind = parse_provider_choice(kind) == ProviderChoice::Anthropic
                        ? ProviderKind::Anthropic
                    : parse_provider_choice(kind) == ProviderChoice::Gemini
                        ? ProviderKind::Gemini
                        : ProviderKind::OpenAI;
        conn.base_url = base_url;
        normalize_base_url(conn.base_url);
        conn.api_key = api_key;
        conn.model = model;

        auto listed = lister(conn);
        if (!listed) {
            out << "  ✗ " << listed.error().msg << "\n";
            std::string again;
            if (!prompt_default(in, out, "Retry with a different endpoint/key?",
                                "y", again)) {
                outcome.cancelled = true;
                return outcome;
            }
            std::string low = to_lower(again);
            if (low == "n" || low == "no" || low == "s" || low == "skip") {
                outcome.cancelled = true;
                out << "Skipped — using built-in defaults.\n";
                return outcome;
            }
            continue;  // loop: re-prompt endpoint (prefilled) + key
        }

        models = std::move(*listed);
        if (models.empty()) {
            out << "  ⚠ endpoint replied with no model list; you can type a model "
                   "name manually.\n";
        } else {
            out << "  ✓ " << models.size() << " model(s) available:\n";
            for (std::size_t i = 0; i < models.size() && i < kMaxListed; ++i)
                out << "    " << (i + 1) << ") " << models[i] << "\n";
            if (models.size() > kMaxListed)
                out << "    … (" << (models.size() - kMaxListed) << " more)\n";
        }
        break;
    }

    // --- Step 3: choose the default model -------------------------------------
    // Default to the preset's suggested model when the endpoint advertises it,
    // else the first listed id, else whatever the user types.
    std::string fallback = model;
    {
        const std::string want = to_lower(model);
        bool present = std::ranges::any_of(
            models, [&](const std::string& m) { return to_lower(m) == want; });
        if (!present) fallback = models.empty() ? model : models.front();
    }
    {
        std::string pick;
        if (!prompt_default(in, out, "Default model (number or name)", fallback,
                            pick)) {
            outcome.cancelled = true;
            return outcome;
        }
        model = resolve_model_choice(pick, models, fallback);
    }
    if (model.empty()) {
        out << "No model chosen — using built-in defaults.\n";
        outcome.cancelled = true;
        return outcome;
    }

    // --- Persist --------------------------------------------------------------
    // Keep the chosen model first in the profile's list so /model + autocomplete
    // surface it, but otherwise preserve the discovered set (or the seed list).
    std::vector<std::string> profile_models = models.empty() ? seed_models : models;
    if (auto it = std::ranges::find(profile_models, model);
        it != profile_models.end())
        std::rotate(profile_models.begin(), it, it + 1);
    else
        profile_models.insert(profile_models.begin(), model);

    Settings s = onboarding_settings(profile_name, kind, base_url, model,
                                     profile_models);
    save_settings(home, s);
    save_credential(home, profile_name, api_key);

    out << "\nSaved to " << home << "/settings.toml (key in credentials.toml, "
        << "chmod 0600).\n"
        << "Provider " << profile_name << " · " << kind << " · " << model
        << "\n\n";

    outcome.completed = true;
    outcome.profile = profile_name;
    outcome.model = model;
    return outcome;
}

}  // namespace moocode
