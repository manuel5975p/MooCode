#ifndef MOOCODE_SKILLS_HPP
#define MOOCODE_SKILLS_HPP

// Dynamically loadable "skills": named bundles of extra capability that are NOT
// active by default (so they stay out of the system prompt and the advertised
// tool list) and are pulled in on demand. Loading a skill runs its loader, which
// typically registers one or more tools into the live ToolRegistry; the next
// model turn then sees the new tools.
//
// Two front-ends share one SkillRegistry: the `load_skill` tool (the model can
// self-load a skill mid-run) and the TUI's `/skill` command (the user loads one
// between turns). Both call SkillRegistry::load, which is idempotent per skill.

#include <expected>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "agent/tools.hpp"
#include "agent/types.hpp"  // Error

namespace moocode {

// How a loaded skill makes itself felt — the contract its load() return value
// obeys, and what the caller must do with it.
enum class SkillEffect {
    // Tool-registering skill: load() mutated the registry; its return value is a
    // mere status note. The caller just displays it.
    Tools,
    // Prompt-injecting skill: load() returns the prompt body, which the caller
    // must feed into the conversation as a message so the model sees it next
    // turn (the load_skill tool result, or the TUI inject buffer).
    InjectMessage,
    // System-prompt skill: load() has already appended the body to the live
    // system prompt itself (via a sink wired in at registration); its return
    // value is a status note. The caller treats it like a Tools skill — the
    // effect is persistent and out-of-band, not a one-off message.
    SystemPrompt,
};

// One loadable skill. `load` mutates the target registry (adds tools) and
// returns a short human-readable note on success, or an Error. It is invoked at
// most once per skill (the registry guards re-loads), so it need not be
// idempotent itself.
struct Skill {
    std::string name;
    std::string description;
    std::function<std::expected<std::string, Error>(ToolRegistry&)> load;
    // What loading this skill does and how the caller must treat load()'s
    // return value. See SkillEffect. Defaults to a plain tool-registering skill.
    SkillEffect effect = SkillEffect::Tools;
};

// A registry of available (initially unloaded) skills. Thread-safe for its own
// bookkeeping; the caller must ensure the ToolRegistry passed to load() is not
// being concurrently read by a running agent (in moocode both front-ends load
// only while no turn is in flight or on the turn's own thread).
class SkillRegistry {
public:
    // Register an available skill. A duplicate name replaces the prior entry.
    void add(Skill skill);

    // True if a skill with this name is registered (loaded or not).
    bool known(const std::string& name) const;

    // True if the named skill has been loaded this session.
    bool is_loaded(const std::string& name) const;

    // The effect of the named skill. Unknown name => Tools (the inert default).
    // Lets a front-end branch on how to handle load()'s return value.
    SkillEffect effect_of(const std::string& name) const;

    // True iff the named skill is InjectMessage — its load() return value is
    // body text the caller must feed into the conversation as a message, not a
    // mere status note. SystemPrompt skills return false here (they augment the
    // system prompt themselves, out-of-band). Unknown name => false. Lets the
    // /skill front-end decide whether to surface the loaded text to the model
    // versus just displaying a confirmation.
    bool injects_prompt(const std::string& name) const;

    // (name, description, loaded) for every registered skill, in registration
    // order — for the `/skill` listing and the load_skill tool description.
    struct Listing {
        std::string name;
        std::string description;
        bool loaded;
    };
    std::vector<Listing> list() const;

    // Load `name` into `reg`. Unknown name => Error. Already loaded => a benign
    // "already loaded" note (no re-run). Otherwise runs the loader; on success
    // marks the skill loaded and returns its note, on failure leaves it unloaded
    // and propagates the Error. post: never throws.
    std::expected<std::string, Error> load(const std::string& name,
                                           ToolRegistry& reg);

private:
    struct Entry {
        Skill skill;
        bool loaded = false;
    };
    mutable std::mutex mtx_;
    std::vector<Entry> entries_;  // registration order preserved
};

// A skill authored as a plain Markdown file (Claude-Skill style). An optional
// leading `---` frontmatter block may set `effect: system` to make loading the
// skill append its body to the system prompt instead of injecting a message
// (the default). The topmost heading plus the first paragraph below it form a
// terse `description`; everything after that first blank line is the `prompt`.
// `name` is the file's stem.
struct MarkdownSkill {
    std::string name;
    std::string description;
    std::string prompt;
    // InjectMessage (the default for an author who writes no frontmatter) or
    // SystemPrompt when the frontmatter declares `effect: system`. Never Tools.
    SkillEffect effect = SkillEffect::InjectMessage;
};

// Parse a Markdown skill document. An optional leading `---`…`---` frontmatter
// block is consumed first; its `effect:` key (value containing "system" =>
// SystemPrompt) selects the delivery mechanism, defaulting to InjectMessage.
// The first non-blank block after it (a `#`-heading and the contiguous lines
// under it, up to the first blank line) becomes a single collapsed-whitespace
// `description`; the remainder, trimmed, becomes `prompt`. pre: `name` is the
// intended skill name (file stem). post: on success both description and prompt
// are non-empty. Returns an Error for empty input, a missing description, or an
// empty body.
std::expected<MarkdownSkill, Error> parse_markdown_skill(std::string name,
                                                         std::string_view content);

// Build a Skill from a parsed MarkdownSkill. Registers no tools. For an
// InjectMessage skill the loader returns the prompt body (surfaced as the
// injected result). For a SystemPrompt skill the loader instead calls
// `system_sink` with the body — wire it to Agent::append_system_prompt — and
// returns a short status note; if `system_sink` is empty it degrades to
// returning the body (so the text is never silently lost). Total.
Skill make_markdown_skill(MarkdownSkill md,
                          std::function<void(std::string)> system_sink = {});

// Scan `dir` for `*.md` files and register each as a prompt skill in `skills`,
// in sorted (deterministic) filename order. SystemPrompt skills are wired to
// `system_sink` (see make_markdown_skill). A duplicate name overrides a
// previously registered skill (last wins, per SkillRegistry::add). Files that
// fail to parse are skipped. Missing/unreadable `dir` is not an error. Returns
// the names of the skills registered, in registration order.
std::vector<std::string> load_markdown_skills(
    SkillRegistry& skills, const std::filesystem::path& dir,
    std::function<void(std::string)> system_sink = {});

// Build the always-present `load_skill` tool. It loads a named skill into `reg`
// via `skills`. Both references must outlive the returned tool. The tool's
// description lists the available skills (snapshotted at build time).
Tool load_skill_tool(SkillRegistry& skills, ToolRegistry& reg);

}  // namespace moocode

#endif  // MOOCODE_SKILLS_HPP
