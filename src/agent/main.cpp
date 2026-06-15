// moocode: a minimal C++ coding agent. Reads LLM_* env for the OpenAI-compatible
// endpoint, registers filesystem/shell tools rooted at the current directory,
// and runs the agent loop on a prompt from argv or stdin.

#include <unistd.h>

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <optional>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <string_view>

#include "ProgramOptions.hxx"
#include "agent/agent.hpp"
#include "agent/anthropic_provider.hpp"
#include "agent/builtin_tools.hpp"
#include "agent/clangd_tools.hpp"
#include "agent/diff.hpp"
#include "agent/fsutil.hpp"
#include "agent/git_tools.hpp"
#include "agent/gitea.hpp"
#include "agent/gitea_internal.hpp"
#include "agent/image_util.hpp"
#include "agent/mentions.hpp"
#include "agent/persist.hpp"
#include "agent/search.hpp"
#include "agent/http.hpp"
#include "agent/openai_provider.hpp"
#include "agent/permissions.hpp"
#include "agent/provider_factory.hpp"
#include "agent/question_tool.hpp"
#include "agent/strutil.hpp"
#include "agent/subagent_tool.hpp"
#include "agent/tools.hpp"
#include "agent/tui.hpp"

namespace {

// The persist Profile must satisfy the duck-typed contract the templated
// provider helpers (profile_kind / find_model_profile) require. Asserted here,
// where both headers are in scope, so a drift in either is a compile error.
static_assert(moocode::ProfileLike<moocode::Profile>);

using namespace moocode;

// True if an executable named `prog` is found on $PATH. No process spawn.
bool program_on_path(std::string_view prog) {
    const char* path = std::getenv("PATH");
    if (!path || !*path) return false;
    std::string_view sv(path);
    std::size_t start = 0;
    while (start <= sv.size()) {
        std::size_t colon = sv.find(':', start);
        std::string_view dir = sv.substr(
            start, colon == std::string_view::npos ? std::string_view::npos : colon - start);
        if (!dir.empty()) {
            std::filesystem::path cand = std::filesystem::path(dir) / prog;
            std::error_code ec;
            if (std::filesystem::exists(cand, ec) && ::access(cand.c_str(), X_OK) == 0)
                return true;
        }
        if (colon == std::string_view::npos) break;
        start = colon + 1;
    }
    return false;
}

// Template expanded at startup: {TOOLS} is the registered tool list, and
// {DIR}/{FILES}/{TIME}/{SYSINFO} fold in live working-directory context. See
// expand_system_prompt(). A --system override is expanded the same way.
constexpr const char* kDefaultSystemPrompt =
    "You are Moo the coding agent. No pleasantries, no hedging. Exact code,\n"
    "errors, terms.\n"
    "\n"
    "Tools:\n"
    "{TOOLS}\n"
    "\n"
    "Working directory: {DIR}\n"
    "Project: {PROJECT}\n"
    "Files:\n"
    "{FILES}\n"
    "Time: {TIME}\n"
    "System: {SYSINFO}\n"
    "\n"
    "@path -> moo auto-attaches that file below; ground your answer in it, don't\n"
    "re-read unless asked. @ takes globs and single dirs.\n"
    "\n"
    "Loop: a tool-free reply ends the turn and IS the final report. Work remaining\n"
    "-> every message carries a tool call. One call per turn unless independent.\n"
    "Keep prose minimal: no narration, recaps, or \"let me check\"; targeted reads,\n"
    "quote only decision-relevant lines.\n"
    "\n"
    "Act, verify, report. Read or grep before edit; edit -> build/test -> read\n"
    "output. Never claim a build passed, test green, or file exists unless you ran\n"
    "it this session. Tool error -> read it before retry, no blind retries.\n"
    "Re-read a file before editing if it may have changed.\n"
    "\n"
    "Code: match existing style, build system, warning flags, test target. Build\n"
    "after changes, respect -Werror. RAII, no raw owning pointers; header/ABI\n"
    "change -> check call sites. Smallest diff that solves it, no drive-by\n"
    "reformat/rename. Blocked -> stop and report, no TODO stubs passed as done.\n"
    "\n"
    "Honesty: separate verified-by-running from inferred; label inference. State\n"
    "limitations and design smells bluntly. Pivotal + costly-if-wrong + ambiguous\n"
    "-> call ask_user with a sharp question and concrete options; else pick a\n"
    "sensible default, state it, proceed. Design/plan tasks -> ask up front.\n"
    "\n"
    "Final report (the tool-free message): what changed, what you verified and\n"
    "how, what remains or is known broken. Terse.";

// Capture a command's stdout via popen, trimmed of trailing newlines.
// Best-effort: empty string on failure. pre: cmd nonnull.
std::string capture(const char* cmd) {
    std::string out;
    FILE* p = ::popen(cmd, "r");
    if (!p) return out;
    char buf[4096];
    for (std::size_t n; (n = std::fread(buf, 1, sizeof buf, p)) > 0;)
        out.append(buf, n);
    ::pclose(p);
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r'))
        out.pop_back();
    return out;
}

// OS description: Windows `ver`, macOS `sw_vers`, else `uname -a`.
std::string sysinfo() {
#if defined(_WIN32)
    return capture("ver");
#elif defined(__APPLE__)
    return capture("sw_vers");
#else
    return capture("uname -a");
#endif
}

// Local date and time, e.g. "2026-06-05 15:42:10". Empty if the clock fails.
std::string now_string() {
    std::time_t t = std::time(nullptr);
    std::tm tm{};
    if (!::localtime_r(&t, &tm)) return {};
    char buf[64];
    std::strftime(buf, sizeof buf, "%Y-%m-%d %H:%M:%S", &tm);
    return buf;
}

// Terse project-kind label from root marker files (non-recursive). Intended to
// seed future LSP wiring (e.g. clangd for the C/C++ variants).
std::string detect_project(const std::filesystem::path& dir) {
    namespace fs = std::filesystem;
    auto has = [&](const char* f) {
        std::error_code ec;
        return fs::exists(dir / f, ec);
    };
    if (has("Cargo.toml")) return "rust";
    if (has("pyproject.toml") || has("setup.py") || has("requirements.txt"))
        return "python";

    const bool cmake = has("CMakeLists.txt");
    const bool conan = has("conanfile.py") || has("conanfile.txt");
    if (cmake || conan) {
        std::string b = cmake ? "cmake" : "";
        if (conan) b += b.empty() ? "conan" : "+conan";
        return "C/C++ (" + b + ")";
    }

    // No build marker: only call it C/C++ if a source/header actually sits here.
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        const auto x = e.path().extension().string();
        if (x == ".c" || x == ".cc" || x == ".cpp" || x == ".cxx" || x == ".h" ||
            x == ".hpp" || x == ".hxx")
            return "C/C++ (unknown build)";
    }
    return "unknown";
}

// Expand {TOOLS}/{DIR}/{PROJECT}/{FILES}/{TIME}/{SYSINFO} in a system-prompt
// template with live startup context. Unknown placeholders are left untouched.
// Called once per process, after the tool registry is fully populated. When
// `advertise_tools` is false (--no-tools) the {TOOLS} listing is replaced by a
// note instead of the tool list, so the model isn't told about tools it has no
// schema to call.
void expand_system_prompt(std::string& s, const std::filesystem::path& dir,
                          const ToolRegistry& reg, bool advertise_tools) {
    substitute(s, "{TOOLS}",
               advertise_tools ? tool_list(reg, 72)
                               : std::string("(tool use disabled via --no-tools)"));
    substitute(s, "{DIR}", dir.string());
    substitute(s, "{PROJECT}", detect_project(dir));
    substitute(s, "{FILES}", capture("ls"));
    substitute(s, "{TIME}", now_string());
    substitute(s, "{SYSINFO}", sysinfo());
}

// Build the option parser. Long forms double as the help text; --base-url,
// --api-key and --model are config overrides that win over the LLM_* env vars.
po::parser make_parser() {
    po::parser p;
    p[""].description("Prompt (taken from stdin if none given)");
    p["yes"].abbreviation('y').description(
        "Run shell commands without confirmation");
    p["system"].abbreviation('s').type(po::string).description(
        "Override the system prompt");
    p["no-tools"].description(
        "Chat-only: omit the JSON tool schema from every request and the tool "
        "list from the system prompt, so the model cannot call tools");
    p["max-iters"].abbreviation('n').type(po::i32).fallback(0).description(
        "Max agent iterations (0 = unlimited, the default)");
    p["base-url"].abbreviation('b').type(po::string).description(
        "Override LLM_BASE_URL (default https://api.minimax.io/v1)");
    p["api-key"].abbreviation('k').type(po::string).description(
        "Override LLM_API_KEY (bearer token)");
    p["model"].abbreviation('m').type(po::string).description(
        "Override LLM_MODEL (default MiniMax-M3)");
    p["provider"].abbreviation('p').type(po::string).description(
        "Backend: openai | anthropic | auto, or a preset "
        "(minimax | deepseek-pro | deepseek-flash) that sets base-url + model");
    p["profile"].type(po::string).description(
        "Use a named profile from settings.toml [profiles.<name>] "
        "(supplies kind/base-url/model + the credentials.toml key)");
    p["max-tokens"].type(po::i32).fallback(0).description(
        "Max output tokens (Anthropic requires this; default 8192, 0 = default)");
    p["effort"].abbreviation('e').type(po::string).description(
        "Reasoning effort: low | medium | high | none "
        "(OpenAI reasoning_effort; Anthropic thinking budget)");
    p["temperature"].abbreviation('t').type(po::f64).description(
        "Sampling temperature (default 0.0; forced to 1.0 when thinking is on)");
    p["thinking"].description("Force extended thinking / reasoning ON");
    p["no-thinking"].description("Force extended thinking / reasoning OFF");
    p["rtk"].description("Force rtk output-compaction ON (rewrite simple bash commands to `rtk <cmd>`)");
    p["no-rtk"].description("Force rtk output-compaction OFF");
    p["context-window"].abbreviation('c').type(po::i32).fallback(0).description(
        "Context window size for the TUI token gauge (0 = show count only)");
    p["debug"].description(
        "TUI diagnostics: show incoming mouse events in the status bar");
    p["help"].abbreviation('h').description("Show this help");
    return p;
}

std::string read_all_stdin() {
    std::ostringstream ss;
    ss << std::cin.rdbuf();
    return ss.str();
}

// Ask the user how to handle a tool call, reading a single key from the
// controlling terminal (so it works even when the prompt arrived via stdin).
// No tty (e.g. CI) => Deny.
Approval tty_approve(const ToolCall& tc) {
    std::fprintf(stderr, "\n  approve %s(%s)?\n  [y]once [s]session [a]always [N]deny ",
                 tc.name.c_str(), tc.arguments_json.c_str());
    std::fflush(stderr);
    FILE* tty = std::fopen("/dev/tty", "r");
    if (!tty) return Approval::Deny;
    int c = std::fgetc(tty);
    // Drain the rest of the line so a trailing newline can't auto-answer the
    // next prompt (matters when several tool calls are gated in one run).
    for (int d = c; d != '\n' && d != EOF; d = std::fgetc(tty)) {
    }
    std::fclose(tty);
    switch (c) {
        case 'y': case 'Y': return Approval::Once;
        case 's': case 'S': return Approval::Session;
        case 'a': case 'A': return Approval::Always;
        default: return Approval::Deny;
    }
}

// Path of the persisted Tavily monthly-quota counter under ~/.moo (empty
// when no home is available).
std::string search_quota_path(const std::string& home) {
    return home.empty() ? std::string() : home + "/search_quota.json";
}

// One-time, best-effort import of the pre-~/.moo legacy dotfiles. Never
// deletes the originals; only fills in a not-yet-present target.
void migrate_legacy(const std::string& home) {
    if (home.empty()) return;
    const char* h = std::getenv("HOME");
    if (!h) return;
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::create_directories(home, ec);
    auto import = [&](const std::string& legacy, const std::string& dst) {
        if (fs::exists(dst, ec) || !fs::exists(legacy, ec)) return;
        fs::copy_file(legacy, dst, ec);
    };
    const std::string base = h;
    // Legacy permissions were one tool per line; convert to permissions.toml.
    std::string perms_toml = home + "/permissions.toml";
    std::string legacy_perms = base + "/.moo_permissions";
    if (!fs::exists(perms_toml, ec) && fs::exists(legacy_perms, ec)) {
        std::ifstream in(legacy_perms);
        std::set<std::string> always;
        for (std::string line; std::getline(in, line);)
            if (!line.empty()) always.insert(line);
        save_permissions(home, always);
    }
    import(base + "/.moo_history", home + "/history");
    import(base + "/.moo_search_quota.json", home + "/search_quota.json");
}

// Web-search config from the environment: SEARXNG_URL (primary, default
// localhost:8080), TAVILY_API_KEY (optional fallback), TAVILY_MONTHLY_LIMIT
// (fallback budget, default 1000 to fit Tavily's free tier).
SearchConfig search_config_from_env(const std::string& home) {
    SearchConfig cfg;
    cfg.searxng_url = env_or("SEARXNG_URL", "http://localhost:8080");
    cfg.tavily_api_key = env_or("TAVILY_API_KEY", "");
    cfg.quota_file = search_quota_path(home);
    if (const char* lim = std::getenv("TAVILY_MONTHLY_LIMIT")) {
        const int n = std::atoi(lim);
        if (n > 0) cfg.tavily_monthly_limit = n;
    }
    return cfg;
}

}  // namespace

int main(int argc, char** argv) {
    po::parser cli = make_parser();
    if (!cli(argc, argv))
        return 2;  // the parser already reported the error to stderr
    if (cli["help"].was_set()) {
        std::cerr << cli;
        std::fprintf(stderr,
                     "\nConfig precedence: flags > LLM_* env vars > "
                     "~/.moo/settings.toml > defaults.\n"
                     "  LLM_BASE_URL  (OpenAI default https://api.minimax.io/v1;\n"
                     "                 Anthropic default https://api.anthropic.com/v1)\n"
                     "  LLM_API_KEY   API key for the endpoint (Bearer for OpenAI,\n"
                     "                 x-api-key for Anthropic)\n"
                     "  LLM_MODEL     model name (OpenAI default MiniMax-M3;\n"
                     "                 Anthropic default claude-sonnet-4-6)\n"
                     "  LLM_PROVIDER  openai | anthropic | auto (default auto)\n"
                     "Named profiles live in ~/.moo/settings.toml "
                     "[profiles.<name>];\n"
                     "their API keys live in ~/.moo/credentials.toml (chmod "
                     "0600).\n"
                     "Select one with --profile <name> or profile = \"<name>\".\n"
                     "State lives under ~/.moo (override with $MOOCODE_HOME).\n");
        return 0;
    }

    // Prompt: join positional arguments, else read stdin. With no argv prompt
    // and a terminal on stdin we enter an interactive REPL instead (below).
    std::string prompt;
    for (const auto& word : cli[""].to_vector<po::string>()) {
        if (!prompt.empty()) prompt += ' ';
        prompt += word;
    }
    const bool interactive = prompt.empty() && ::isatty(STDIN_FILENO);
    if (prompt.empty() && !interactive)
        // Piped/redirected: read the whole stream so multi-line prompts work.
        prompt = read_all_stdin();
    // Trim trailing whitespace/newline from the prompt.
    while (!prompt.empty() && (prompt.back() == '\n' || prompt.back() == '\r' ||
                               prompt.back() == ' '))
        prompt.pop_back();
    if (prompt.empty() && !interactive) {
        std::fprintf(stderr, "error: empty prompt\n");
        return 2;
    }

    const bool yes = cli["yes"].was_set();
    const bool advertise_tools = !cli["no-tools"].was_set();
    std::string system = cli["system"].was_set()
                             ? cli["system"].get().string
                             : std::string(kDefaultSystemPrompt);

    // Persistent state under ~/.moo ($MOOCODE_HOME). Import legacy dotfiles
    // once, then load settings as the layer below env and flags.
    const std::string home = moocode_home();
    migrate_legacy(home);
    const Settings settings = load_settings(home);

    // Resolve config with precedence flags > LLM_* env > profile/preset >
    // settings.toml > defaults. Track whether each value came from an explicit
    // user override (flag/env) so the profile/preset can override only the
    // settings.toml layer — explicit user choices must always win.
    //
    // `pick` collapses the repeated "flag, else non-empty env, else unset"
    // ladder into one place, evaluating getenv exactly once per field.
    struct Picked {
        std::string value;
        bool set;  // a flag or a non-empty env var supplied this value
    };
    auto pick = [&](const char* opt, const char* env) -> Picked {
        if (cli[opt].was_set()) return {cli[opt].get().string, true};
        const char* e = std::getenv(env);
        if (e && *e) return {std::string(e), true};
        return {{}, false};
    };

    const Picked base_url_pick = pick("base-url", "LLM_BASE_URL");
    const Picked model_pick = pick("model", "LLM_MODEL");
    const Picked api_key_pick = pick("api-key", "LLM_API_KEY");
    const Picked provider_pick = pick("provider", "LLM_PROVIDER");

    const bool base_url_from_user = base_url_pick.set;
    const bool model_from_user = model_pick.set;
    const bool api_key_from_user = api_key_pick.set;

    std::string base_url =
        base_url_from_user ? base_url_pick.value : settings.base_url;  // maybe ""
    std::string api_key = api_key_pick.value;  // "" when unset
    std::string model =
        model_from_user ? model_pick.value : settings.model;  // possibly ""

    // Provider: flag > LLM_PROVIDER env > settings. The value is either a wire
    // format (openai/anthropic/auto) or a named preset (minimax/deepseek-pro/
    // deepseek-flash) that bundles a base URL + model. Auto detects the wire
    // format from the (explicit) base_url; an empty base_url stays OpenAI.
    std::string provider_str =
        provider_pick.set ? provider_pick.value : settings.provider;

    // Active profile: --profile flag > settings.profile. A profile sits below
    // explicit flags/env: it supplies kind and fills any still-empty
    // base_url/model, and its credentials.toml key is the api_key fallback after
    // --api-key / LLM_API_KEY. It is skipped when --provider/-p names a preset
    // (the two are mutually exclusive ways to bundle base_url+model).
    std::string active_profile = cli["profile"].was_set()
                                     ? cli["profile"].get().string
                                     : settings.profile;
    const Profile* profile = nullptr;
    if (!active_profile.empty())
        for (const Profile& p : settings.profiles)
            if (p.name == active_profile) { profile = &p; break; }

    ProviderKind kind;
    if (auto preset = lookup_preset(provider_str)) {
        // A preset fills base_url/model only when the user didn't set them
        // explicitly via flag/env — but it overrides settings.toml values.
        kind = preset->kind;
        if (!base_url_from_user) base_url = preset->base_url;
        if (!model_from_user) model = preset->model;
    } else if (profile) {
        // Profile-driven: kind from the profile (parse "openai"/"anthropic";
        // empty/auto => detect from base_url). Explicit --provider still wins.
        if (!base_url_from_user) base_url = profile->base_url;
        if (!model_from_user) model = profile->model;

        // When the user explicitly sets a model that belongs to a different
        // profile, switch to that profile's endpoint, wire format, and key
        // (mirrors what the TUI's /model command does). Models known to the
        // active profile stay with it even if another profile also lists them.
        const Profile* eff = profile;
        // `all` lives as long as `eff` may point into it.
        std::vector<Profile> all;
        if (model_from_user) {
            // Stay with the active profile if the model is its own.
            bool in_active = false;
            auto model_lower = to_lower(model);
            for (const std::string& m : profile->models)
                if (to_lower(m) == model_lower) { in_active = true; break; }
            if (!in_active) {
                all = settings.profiles;
                if (all.empty()) all = builtin_profiles();
                if (const Profile* mp = find_model_profile(model, all)) {
                    eff = mp;
                    if (!base_url_from_user) base_url = mp->base_url;
                }
            }
        }

        ProviderChoice explicit_choice = parse_provider_choice(provider_str);
        ProviderChoice prof_choice = parse_provider_choice(eff->kind);
        ProviderChoice choice = explicit_choice != ProviderChoice::Auto
                                    ? explicit_choice
                                    : prof_choice;
        kind = choice == ProviderChoice::Anthropic ? ProviderKind::Anthropic
               : choice == ProviderChoice::OpenAI  ? ProviderKind::OpenAI
               : choice == ProviderChoice::Gemini  ? ProviderKind::Gemini
                                                   : detect_provider_kind(base_url, model);
        // The effective profile's stored key is the last api_key fallback.
        if (!api_key_from_user) {
            auto creds = load_credentials(home);
            if (auto it = creds.find(eff->name); it != creds.end())
                api_key = it->second;
        }
        // If we switched to a model-matched profile, reflect it in the UI.
        if (eff != profile) active_profile = eff->name;
    } else {
        ProviderChoice choice = parse_provider_choice(provider_str);
        if (choice == ProviderChoice::Auto && !model.empty()) {
            // Model→profile lookup: search profiles for a matching model to
            // auto-select the right endpoint and wire format. This mirrors the
            // TUI's /model command: known model names carry their provider's
            // kind + base_url + API key. Settings profiles win over built-ins;
            // explicit flags (-b/-k) still override the profile's values.
            std::vector<Profile> all = settings.profiles;
            if (all.empty()) all = builtin_profiles();
            const Profile* mp = find_model_profile(model, all);
            if (mp) {
                kind = profile_kind(*mp);
                if (!base_url_from_user) base_url = mp->base_url;
                if (!api_key_from_user) {
                    auto creds = load_credentials(home);
                    if (auto it = creds.find(mp->name); it != creds.end())
                        api_key = it->second;
                }
            } else {
                kind = detect_provider_kind(base_url, model);
            }
        } else {
            kind = choice == ProviderChoice::Anthropic ? ProviderKind::Anthropic
                   : choice == ProviderChoice::OpenAI  ? ProviderKind::OpenAI
                   : choice == ProviderChoice::Gemini  ? ProviderKind::Gemini
                                                       : detect_provider_kind(base_url, model);
        }
    }

    // Now fill provider-specific defaults for anything left unset, then strip a
    // trailing slash so "<base>/<endpoint>" is well-formed.
    if (base_url.empty()) {
        switch (kind) {
            case ProviderKind::Anthropic: base_url = "https://api.anthropic.com/v1"; break;
            case ProviderKind::Gemini:
                base_url = "https://generativelanguage.googleapis.com/v1beta"; break;
            case ProviderKind::OpenAI: base_url = "https://api.minimax.io/v1"; break;
        }
    }
    if (model.empty()) {
        switch (kind) {
            case ProviderKind::Anthropic: model = "claude-sonnet-4-6"; break;
            case ProviderKind::Gemini: model = "gemini-3.5-flash"; break;
            case ProviderKind::OpenAI: model = "MiniMax-M3"; break;
        }
    }
    normalize_base_url(base_url);

    // max_tokens (Anthropic output cap): flag > settings > built-in default.
    int max_tokens = cli["max-tokens"].was_set() ? cli["max-tokens"].get().i32
                                                 : settings.max_tokens;

    // max_iterations: flag > settings > unlimited. A non-positive value (the
    // default, or --max-iters 0) leaves the backstop disabled (nullopt).
    std::optional<std::uint32_t> max_iterations;
    if (cli["max-iters"].was_set()) {
        if (int v = cli["max-iters"].get().i32; v > 0)
            max_iterations = static_cast<std::uint32_t>(v);
    } else if (settings.max_iterations > 0) {
        max_iterations = static_cast<std::uint32_t>(settings.max_iterations);
    }
    // context_window (TUI gauge): flag > settings > 0.
    int context_window = cli["context-window"].was_set()
                             ? cli["context-window"].get().i32
                             : settings.context_window;

    http::global_init();

    // A swappable file-change sink: the CLI prints a colored diff to stderr; the
    // TUI repoints it at its own handler. The tool closures call through *sink,
    // so it must be set before register_builtin_tools captures opts.
    auto sink = std::make_shared<FileChangeFn>();
    *sink = [](const FileChange& fc) {
        auto d = elide_context(diff_lines(fc.old_content, fc.new_content),
                               kDiffContext);
        std::fprintf(stderr, "\n\033[2m--- %s ---\033[0m\n", fc.path.c_str());
        std::string ansi = render_ansi_diff(d);
        std::fwrite(ansi.data(), 1, ansi.size(), stderr);
        std::fflush(stderr);
    };

    ToolOptions opts;
    opts.root = std::filesystem::current_path();
    opts.on_file_change = [sink](const FileChange& fc) {
        if (*sink) (*sink)(fc);
    };

    // rtk output-compaction: detection (PATH) AND config (flag > env > settings >
    // default-on). The tools only ever see the AND-ed result.
    const bool rtk_available = program_on_path("rtk");
    int rtk_cfg = settings.rtk;  // -1 unset / 0 off / 1 on
    if (const char* e = std::getenv("MOOCODE_RTK"); e && *e)
        rtk_cfg = (std::string_view(e) == "0" || std::string_view(e) == "false") ? 0 : 1;
    if (cli["rtk"].was_set()) rtk_cfg = 1;
    if (cli["no-rtk"].was_set()) rtk_cfg = 0;
    const bool rtk_on = (rtk_cfg != 0);  // default-on when unset (-1)
    opts.rtk = rtk_available && rtk_on;

    ToolRegistry reg;
    register_builtin_tools(reg, opts);
    reg.add(web_search_tool(search_config_from_env(home)));
    reg.add(web_fetch_tool());

    // Gitea inspection tools (repos, PRs, commits, diffs, files). Every tool
    // takes an optional per-call `url`, so they are always registered;
    // MOOCODE_GITEA_URL supplies the default instance and MOOCODE_GITEA_TOKEN
    // its credential. MOOCODE_GITEA_AUTH names a .env-style file
    // (GITEA_USER=... / GITEA_PASS=...) used as Basic auth when no token is
    // set. Both credentials are sent only to the configured instance's origin.
    {
        GiteaConfig gcfg;
        gcfg.base_url = env_or("MOOCODE_GITEA_URL", "");
        gcfg.token = env_or("MOOCODE_GITEA_TOKEN", "");
        if (const std::string auth_file = env_or("MOOCODE_GITEA_AUTH", "");
            !auth_file.empty()) {
            const GiteaBasicAuth auth = parse_gitea_auth(slurp(auth_file));
            if (auth.user.empty() && auth.pass.empty())
                std::fprintf(stderr,
                             "warning: MOOCODE_GITEA_AUTH file '%s' is unreadable or "
                             "has no GITEA_USER/GITEA_PASS\n",
                             auth_file.c_str());
            gcfg.auth_user = auth.user;
            gcfg.auth_pass = auth.pass;
        }
        for (Tool& t : gitea_tools(std::move(gcfg))) reg.add(std::move(t));
    }

    // Read-only local-git tools (status/diff/log/show/branch). Always registered;
    // rtk-backed when rtk is on PATH. Confined to the project root.
    {
        GitConfig gcfg;
        gcfg.root = opts.root;
        gcfg.rtk_available = rtk_available;
        gcfg.timeout_secs = opts.bash_timeout_secs;
        for (Tool& t : git_tools(std::move(gcfg))) reg.add(std::move(t));
    }

    // clangd-backed code-intelligence tools (find_references, go_to_definition,
    // go_to_implementation, hover, list_symbols, call_hierarchy, rename). clangd
    // spawns lazily on first use; the session is held in main scope so its
    // destructor shuts clangd down deterministically at exit. Registered before
    // spawn_subagent so sub-agents inherit them and the {TOOLS} prompt lists them.
    ClangdToolsConfig ccfg;
    ccfg.root = opts.root;
    ccfg.clangd_path = env_or("CLANGD_PATH", "clangd");
    {
        std::string ccdir = env_or("CLANGD_COMPILE_COMMANDS_DIR", "");
        std::error_code cec;
        if (!ccdir.empty())
            ccfg.compile_commands_dir = std::filesystem::path(ccdir);
        else if (std::filesystem::exists(opts.root / "build" / "compile_commands.json", cec))
            ccfg.compile_commands_dir = opts.root / "build";
        else if (std::filesystem::exists(opts.root / "compile_commands.json", cec))
            ccfg.compile_commands_dir = opts.root;
        int wait_ms = std::atoi(env_or("CLANGD_INDEX_WAIT_MS", "0").c_str());
        if (wait_ms > 0) ccfg.index_wait_ms = wait_ms;
    }
    auto clangd_session = make_clangd_session(ccfg);
    register_clangd_tools(reg, clangd_session, opts);

    // ask_user tool: lets the LLM ask the user a pick-one question. The gate
    // blocks the worker thread until the TUI modal resolves; nullptr in
    // non-interactive mode (falls back to stdin/stderr).
    QuestionGate question_gate;
    reg.add(ask_user_tool(interactive ? &question_gate : nullptr));

    // Generation controls (flags > settings): effort, temperature, thinking
    // toggle, and the shared max_tokens cap. All opt-in; applied uniformly via
    // the provider's set_params so each backend takes what it supports.
    GenerationParams gp;
    {
        std::string effort = cli["effort"].was_set() ? cli["effort"].get().string
                                                     : settings.effort;
        std::string e = effort;
        for (char& ch : e) ch = static_cast<char>(std::tolower(
                               static_cast<unsigned char>(ch)));
        if (e == "none" || e == "off")
            gp.thinking = false;  // explicit disable, no effort level
        else if (!effort.empty())
            gp.effort = effort;
    }
    if (cli["temperature"].was_set())
        gp.temperature = cli["temperature"].get().f64;
    else if (settings.temperature >= 0)
        gp.temperature = settings.temperature;
    if (cli["thinking"].was_set())
        gp.thinking = true;  // explicit flags win over effort/settings
    else if (cli["no-thinking"].was_set())
        gp.thinking = false;
    else if (!gp.thinking.has_value() && settings.thinking >= 0)
        gp.thinking = settings.thinking != 0;
    // Default thinking ON when nothing explicitly configured.
    if (!gp.thinking.has_value()) gp.thinking = true;
    if (max_tokens > 0) gp.max_tokens = max_tokens;

    // Build the backend by wire format via the shared factory (single source of
    // truth with the TUI's runtime provider swaps). Both satisfy Provider&, so
    // the agent loop is identical; only the request/response shape differs.
    std::unique_ptr<Provider> provider = make_provider(
        ProviderConnection{.kind = kind, .base_url = base_url, .api_key = api_key, .model = model, .max_tokens = max_tokens}, gp);

    // Soft warning: reasoning controls were requested on an OpenAI-compatible
    // model that probably ignores reasoning_effort/thinking. (Anthropic always
    // supports thinking; --no-thinking is a harmless no-op, so neither warns.)
    std::string reasoning_warn;
    const bool wants_reasoning =
        gp.effort.has_value() || (gp.thinking.has_value() && *gp.thinking);
    if (wants_reasoning && kind == ProviderKind::OpenAI &&
        !openai_model_likely_reasoning(model))
        reasoning_warn =
            "note: --effort/--thinking may be ignored by '" + model +
            "' — OpenAI-compatible models that aren't reasoning models drop them";

    // Construct agent first with a placeholder system prompt, so the
    // spawn_subagent tool can capture &agent for lazy accessors (get_provider,
    // parent_cancel, get_approve, get_on_usage). The real prompt is expanded
    // and pushed in after spawn_subagent registration.
    AgentConfig acfg;
    acfg.system_prompt = "";  // placeholder; set after {TOOLS} expansion
    acfg.max_iterations = max_iterations;
    acfg.advertise_tools = advertise_tools;
    Agent agent(*provider, reg, acfg);
    // Let long clangd reads abort promptly when the user cancels a turn.
    clangd_session->watch_cancel(&agent.cancel_flag());

    // Register spawn_subagent last — its SubagentConfig captures a reference to
    // `reg`, so tools added after would leak into sub-agents.
    auto on_sub_activity = std::make_shared<SubagentActivityFn>();
    auto on_sub_text      = std::make_shared<SubagentTextFn>();

    // Startup snapshot for sub-agent model overrides. spawn_subagent is
    // registered once here; the TUI runs this same agent, so a profile added via
    // the TUI's /provider save mid-session is not reflected until restart (see
    // the design spec). Sub-agent override providers start with clean generation
    // params — effort/temperature/thinking are not inherited across providers.
    std::vector<Profile> sub_profiles =
        settings.profiles.empty() ? builtin_profiles() : settings.profiles;
    std::map<std::string, std::string> sub_creds = load_credentials(home);
    {
        SubagentConfig scfg;
        scfg.get_provider = [&agent]() -> Provider& { return agent.provider(); };
        scfg.tools = &reg;
        scfg.root = opts.root;
        scfg.parent_cancel = &agent.cancel_flag();
        // Lazy — callbacks may be installed after this registration.
        scfg.get_approve = [&agent]() { return agent.approve_callback(); };
        scfg.get_on_usage = [&agent]() { return agent.usage_callback(); };
        scfg.on_activity = on_sub_activity;
        scfg.on_text      = on_sub_text;
        // Advertised model list (for the tool description + unknown-model error).
        scfg.list_models = [sub_profiles]() {
            std::vector<SubagentModelInfo> out;
            for (const Profile& p : sub_profiles) {
                const char* prov = provider_kind_name(profile_kind(p));
                for (const std::string& m : p.models)
                    out.push_back(SubagentModelInfo{.model = m, .provider = prov});
            }
            return out;
        };
        // Build a fresh provider for a validated model. Key resolution mirrors
        // the TUI's key_for_profile, but falls back to the session key when the
        // stored per-profile credential is missing or empty.
        scfg.make_provider_for_model =
            [sub_profiles, sub_creds, key = api_key](const std::string& model)
            -> std::expected<std::unique_ptr<Provider>, Error> {
            const Profile* p = find_model_profile(model, sub_profiles);
            if (!p)
                return std::unexpected(
                    Error{.msg = "unknown model '" + model + "'", .code = 0});
            std::string base = p->base_url;
            normalize_base_url(base);
            std::string api = key;
            if (auto it = sub_creds.find(p->name);
                it != sub_creds.end() && !it->second.empty())
                api = it->second;
            ProviderConnection conn{.kind = profile_kind(*p),
                                    .base_url = base,
                                    .api_key = api,
                                    .model = model,
                                    .max_tokens = 0};
            return make_provider(conn, GenerationParams{});
        };
        reg.add(spawn_subagent_tool(std::move(scfg)));
    }

    // Fold live context (tools, cwd, ls, time, OS) into the prompt template.
    // This runs after spawn_subagent registration so {TOOLS} includes it.
    expand_system_prompt(system, opts.root, reg, advertise_tools);

    // Append project-local or global MOO.md when present.
    if (std::string fm = load_moocode_md(home, opts.root); !fm.empty())
        system += "\n\n" + std::move(fm);

    // Push the final prompt into the already-constructed agent.
    agent.set_system_prompt(system);
    // Permissions: seed from permissions.toml; persist new always-grants back.
    Permissions perms{load_permissions(home),
                      [home](const std::set<std::string>& s) {
                          save_permissions(home, s);
                      }};

    // @-mentions are resolved against the tool sandbox root, so a file the user
    // can @-mention is one a model tool call could also read. The same byte/file
    // caps as the in-loop tools apply, so a single @-bomb can't blow up the
    // context: per-file 64 KiB, total 256 KiB, max 32 attachments.
    MentionOptions mopt;
    mopt.root = opts.root;
    mopt.max_file_bytes = 64 * 1024;
    mopt.max_total_bytes = 256 * 1024;
    mopt.max_files = 32;

    // Interactive on a terminal: the full-screen TUI. It installs its own
    // streaming callbacks and, unless --yes, a four-way modal approval gate, and
    // repoints `sink` so file edits render as colored diffs in the log.
    if (interactive) {
        TuiInfo info{model,
                     base_url,
                     opts.root.string(),
                     context_window,
                     home,
                     provider_kind_name(kind),
                     reasoning_warn,
                     api_key,
                     active_profile};
        info.debug = cli["debug"].was_set();
        int code = run_tui(agent, yes ? nullptr : &perms, info, sink, &question_gate,
                         on_sub_activity, on_sub_text);
        http::global_cleanup();
        return code;
    }

    // Non-interactive (argv prompt or piped stdin): gate every tool via /dev/tty
    // unless --yes, then stream the single run plainly to stdout/stderr.
    // A one-line dim banner mirrors the TUI status bar's provider/model chip.
    std::fprintf(stderr, "\033[2m[%s · %s]\033[0m\n", provider_kind_name(kind),
                 model.c_str());
    if (!reasoning_warn.empty())
        std::fprintf(stderr, "\033[2m%s\033[0m\n", reasoning_warn.c_str());
    if (!yes)
        agent.on_approve(
            [&perms](const ToolCall& tc) {
                if (tc.name == "ask_user") return true;
                return decide(perms, tc, tty_approve);
            });

    // Live output: the answer streams to stdout as tokens arrive. The reasoning
    // (chain of thought) is NOT printed verbatim — it can be enormous and the
    // raw dump both floods the terminal and stalls behind its own I/O. Instead a
    // single dimmed line on stderr is rewritten in place with a running estimate
    // of the reasoning length (~4 bytes/token, since the server only reports real
    // usage once the stream ends). `reasoning_open` tracks that in-progress line
    // so it can be closed (reset + newline) before answer/tool output interleaves.
    auto reasoning_open = std::make_shared<bool>(false);
    auto reasoning_chars = std::make_shared<std::size_t>(0);
    std::function<void()> close_reasoning = [reasoning_open, reasoning_chars] {
        if (*reasoning_open) {
            std::fputs("\033[0m\n", stderr);  // reset dim, end the line
            std::fflush(stderr);
            *reasoning_open = false;
            *reasoning_chars = 0;
        }
    };

    agent.on_delta([reasoning_open, reasoning_chars, close_reasoning](
                       std::string_view answer, std::string_view reasoning) {
        if (!reasoning.empty()) {
            *reasoning_open = true;
            *reasoning_chars += reasoning.size();
            std::size_t approx_tokens = (*reasoning_chars + 3) / 4;
            // \r rewrites the line, \033[K clears any shorter previous render.
            std::fprintf(stderr, "\r\033[2m💭 thinking… (~%zu tokens)\033[0m\033[K",
                         approx_tokens);
            std::fflush(stderr);
        }
        if (!answer.empty()) {
            close_reasoning();
            std::fwrite(answer.data(), 1, answer.size(), stdout);
            std::fflush(stdout);
        }
    });

    // The tool calls being made are shown on stderr (assistant prose has already
    // streamed via on_delta; the often-large tool results are omitted).
    agent.on_message([close_reasoning](const Message& m) {
        if (m.role() != Role::Assistant || m.tool_calls().empty()) return;
        close_reasoning();
        for (const auto& tc : m.tool_calls())
            std::fprintf(stderr, "\n  -> %s(%s)\n", tc.name.c_str(),
                         tc.arguments_json.c_str());
    });

    // Once a turn completes the server reports real usage; print the exact
    // completion-token count (supersedes the live reasoning estimate above).
    agent.on_usage([close_reasoning](const Usage& u) {
        close_reasoning();
        std::fprintf(stderr, "\033[2m[%d completion tokens]\033[0m\n",
                     u.completion_tokens);
        std::fflush(stderr);
    });

    // Expand any @path tokens in the prompt (read-only, sandboxed; same rules
    // as the tool layer). Report a one-line summary on stderr so the user can
    // see what was attached before the model's answer streams in.
    auto expansion = expand_mentions(prompt, mopt);
    if (!expansion.entries.empty()) {
        std::size_t ok = 0, err = 0;
        for (const auto& e : expansion.entries) {
            if (e.error.empty())
                ++ok;
            else
                ++err;
        }
        std::fprintf(stderr,
                     "\033[2m📎 %zu file(s) attached via @-mentions (%zu B, %zu "
                     "error(s)):\033[0m\n",
                     ok, expansion.total_bytes, err);
        for (const auto& e : expansion.entries) {
            if (!e.error.empty()) {
                std::fprintf(stderr, "\033[2m  - %s: %s\033[0m\n", e.path.c_str(),
                             e.error.c_str());
            } else {
                std::fprintf(stderr, "\033[2m  - %s  (%s, %zu lines, %zu B)\033[0m\n",
                             e.path.c_str(), e.kind.c_str(), e.lines, e.bytes);
            }
        }
        std::fflush(stderr);
    }

    // Build multimodal content parts: text first, then images from @-mentions
    // and from direct image-path scanning in the prompt.
    std::vector<ContentPart> user_parts;
    user_parts.push_back(ContentPart{expansion.prompt, std::nullopt});
    for (auto& img : expansion.images)
        user_parts.push_back(ContentPart{"", std::move(img)});
    auto scanned = scan_image_paths(prompt, opts.root);
    for (auto& img : scanned.images)
        user_parts.push_back(ContentPart{"", std::move(img)});
    if (!scanned.images.empty()) {
        std::fprintf(stderr, "\033[2m🖼 %zu image(s) detected in prompt\033[0m\n",
                     scanned.images.size());
        for (const auto& e : scanned.errors)
            std::fprintf(stderr, "\033[2m  ! %s\033[0m\n", e.c_str());
        std::fflush(stderr);
    }

    auto result = agent.run(expansion.prompt, std::move(user_parts));
    close_reasoning();
    http::global_cleanup();

    if (!result) {
        std::fprintf(stderr, "\nagent error: %s\n", result.error().msg.c_str());
        return 1;
    }
    std::fputc('\n', stdout);  // terminate the streamed answer line
    return 0;
}
