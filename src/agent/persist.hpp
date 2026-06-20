#ifndef MOOCODE_PERSIST_HPP
#define MOOCODE_PERSIST_HPP

// On-disk state under ~/.moo (settings, the tool allowlist, and saved
// conversations), all stored as TOML. This header is deliberately toml-free —
// toml.hpp (489 KB) is confined to persist.cpp, the single TU that includes it,
// mirroring how FTXUI is confined to tui.cpp. Everything here degrades to a
// no-op / empty result when no home directory is available, matching the
// pre-existing empty-path behaviour, so callers never need to special-case it.

#include <cstddef>
#include <expected>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "agent/types.hpp"  // Conversation, Error

namespace moocode {

// Resolved ~/.moo directory: $MOOCODE_HOME if set, else $HOME/.moo, else
// "" (no home => persistence disabled). Does not create anything.
std::string moocode_home();

// A named connection profile from settings.toml [profiles.<name>]. The api_key
// is NOT stored here; it is resolved separately from credentials.toml so secrets
// stay out of settings.toml. `kind` is "openai"/"anthropic"/"" ("" => infer).
// This layer stays provider-agnostic: kind is a plain string, not ProviderKind,
// so agent_persist needn't depend on agent_provider.
struct Profile {
    std::string name;                 // the [profiles.<name>] table key
    std::string kind;                 // "openai" | "anthropic" | "" (infer from base_url)
    std::string base_url;
    std::string model;                // default model when this profile is selected
    std::vector<std::string> models;  // models this provider serves (/model lookup + autocomplete)
    int thinking = -1;                // -1 => unset (use global default), 0 => off, 1 => on
    bool drop_thinking_tag = false;   // omit the thinking field entirely (for endpoints that reject it)
    std::string thinking_type = "enabled";  // thinking.type when reasoning is ON ("enabled" DeepSeek, "adaptive" MiniMax)
};

// Built-in profiles matching the former hardcoded model_registry, used when no
// [profiles.*] are configured. These give out-of-the-box model→endpoint mapping
// so e.g. `-m claude-sonnet-4-6` auto-selects the Anthropic endpoint. The list
// is a const reference to a function-local static; never written to disk.
const std::vector<Profile>& builtin_profiles();

// Configuration persisted in settings.toml. An empty string / zero means "not
// set in the file" so callers can layer it under env/flag overrides.
struct Settings {
    std::string base_url;     // empty => unset
    std::string model;        // empty => unset
    std::string provider;     // "openai"/"anthropic"/"auto"; empty => unset
    int context_window = 0;   // 0 => unset
    int max_iterations = 0;   // 0 => unset
    int max_tokens = 0;       // 0 => unset (output-token cap)
    std::string effort;       // reasoning effort "low"/"medium"/"high"; empty => unset
    double temperature = -1;  // < 0 => unset (0 is a valid temperature)
    int thinking = -1;        // -1 => unset, 0 => off, 1 => on
    int rtk = -1;             // rtk command-rewrite: -1 unset, 0 off, 1 on
    std::string theme;        // code-block syntax theme name; empty => unset
    std::string profile;            // active profile name; empty => none
    std::vector<Profile> profiles;  // [profiles.*] tables, sorted by name
};

// Load settings.toml (missing or malformed => default-constructed, never errors).
Settings load_settings(const std::string& home);
// Write settings.toml, creating `home` if needed. Best-effort; never throws.
// Never emits api_key: secrets live only in credentials.toml.
void save_settings(const std::string& home, const Settings& s);

// credentials.toml: profile-name -> api_key. A separate file stored 0600 so
// secrets stay out of settings.toml. Missing/malformed => empty map, never errors.
std::map<std::string, std::string> load_credentials(const std::string& home);

// Upsert one profile's api_key and rewrite credentials.toml (0600 perms,
// best-effort). Merges into the existing map so other keys are preserved.
// Deterministic key order. Empty home / empty name => no-op.
void save_credential(const std::string& home, const std::string& name,
                     const std::string& key);

// The always-allow tool set <-> permissions.toml ("always = [...]"). Kept as a
// plain set so agent_persist needn't depend on agent_permissions.
std::set<std::string> load_permissions(const std::string& home);
void save_permissions(const std::string& home, const std::set<std::string>& always);

// Per-conversation metadata stored in the [meta] table.
struct ConvMeta {
    std::string cwd;
    std::string model;
    std::string created;  // ISO-8601 UTC, e.g. "2026-06-06T14:30:12Z"
    std::string updated;
    std::string title;    // first user line, clipped
};

// One entry in a conversation listing (cheap to compute without a full load).
struct ConvSummary {
    std::string path;
    std::string title;
    std::string updated;
    std::size_t count = 0;  // message count
};

// The conversations/ subdirectory under `home` ("" when home is empty).
std::string conversations_dir(const std::string& home);

// Mint a conversation id "<UTC yyyymmdd-hhmmss>-<8-hex cwd hash>". Lexical sort
// equals chronological order. Uses std::time and std::hash<std::string>.
std::string new_conversation_id(const std::string& cwd);

// Serialise `conv` + `meta` to `path` as TOML. Creates parent dirs. Error on a
// write failure or empty path. pre: none. post: file replaced atomically-ish.
std::expected<void, Error> save_conversation(const std::string& path,
                                             const Conversation& conv,
                                             const ConvMeta& meta);

// Load just the conversation from `path`. Error on missing file / parse failure.
std::expected<Conversation, Error> load_conversation(const std::string& path);

// Load the conversation together with its [meta] so the TUI can adopt the
// title/model and keep saving to the same file.
std::expected<std::pair<Conversation, ConvMeta>, Error> load_conversation_with_meta(
    const std::string& path);

// Summaries of every conversation in `dir`, most-recent-first (by meta.updated
// descending, ties broken by path). A non-empty `cwd` keeps only conversations
// whose meta.cwd matches. A missing directory yields {}. pre: none.
std::vector<ConvSummary> list_conversations(const std::string& dir,
                                            const std::string& cwd);

// Check TOML syntax without throwing. Returns std::nullopt when `text` is
// valid TOML; otherwise returns the parser's error description. Wraps
// toml::parse() inside persist.cpp (the only TU with toml.hpp) so other TUs
// can validate settings.toml without including the 489 KB toml.hpp header.
std::optional<std::string> toml_check_syntax(std::string_view text);

}  // namespace moocode

#endif  // MOOCODE_PERSIST_HPP
