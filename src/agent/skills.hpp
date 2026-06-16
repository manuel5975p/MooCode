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
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include "agent/tools.hpp"
#include "agent/types.hpp"  // Error

namespace moocode {

// One loadable skill. `load` mutates the target registry (adds tools) and
// returns a short human-readable note on success, or an Error. It is invoked at
// most once per skill (the registry guards re-loads), so it need not be
// idempotent itself.
struct Skill {
    std::string name;
    std::string description;
    std::function<std::expected<std::string, Error>(ToolRegistry&)> load;
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

// Build the always-present `load_skill` tool. It loads a named skill into `reg`
// via `skills`. Both references must outlive the returned tool. The tool's
// description lists the available skills (snapshotted at build time).
Tool load_skill_tool(SkillRegistry& skills, ToolRegistry& reg);

}  // namespace moocode

#endif  // MOOCODE_SKILLS_HPP
