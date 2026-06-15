#ifndef MOOCODE_PERMISSIONS_HPP
#define MOOCODE_PERMISSIONS_HPP

// Tool-call approval policy: an in-memory allowlist plus the decision glue both
// run modes share. Persistence is pluggable — a SaveFn hook is fired after every
// new always-grant, so the actual storage (TOML under ~/.moo, a test lambda,
// or nothing at all) lives outside this layer and toml.hpp stays out of it.

#include <functional>
#include <mutex>
#include <set>
#include <string>

#include "agent/types.hpp"  // ToolCall

namespace moocode {

// What the user chose when asked to approve a tool call.
enum class Approval { Once, Session, Always, Deny };

// A per-tool allowlist with two tiers: `session` (this process) and `always`
// (persisted via the save hook). A tool is allowed if it is in either.
class Permissions {
public:
    // Called with the full always-set after a new always-grant, so a caller can
    // persist it. An empty hook keeps grants in memory only.
    using SaveFn = std::function<void(const std::set<std::string>&)>;

    // Seed the always tier and (optionally) install a save hook. Both default to
    // empty for memory-only use (e.g. tests).
    explicit Permissions(std::set<std::string> always = {}, SaveFn save = {});

    // True if `tool` may run without prompting (session or always tier).
    bool allowed(const std::string& tool) const;

    // Remember `tool` for the rest of this process.
    void grant_session(const std::string& tool);

    // Remember `tool` across sessions: add to the always tier and, only if it
    // was new, fire the save hook with the full set. No-op when already known.
    void grant_always(const std::string& tool);

private:
    // Guards the two tiers: parallel tool calls (and sibling sub-agents sharing
    // one policy — see Agent::run) reach allowed()/grant_* concurrently.
    mutable std::mutex mtx_;
    std::set<std::string> session_;
    std::set<std::string> always_;
    SaveFn save_;
};

// Asks the user how to handle one tool call. Returns their choice.
using Prompter = std::function<Approval(const ToolCall&)>;

// Decide whether to run `tc`: already allowed => true with no prompt; otherwise
// prompt, record any Session/Always grant in `perms`, and return run/skip. Deny
// returns false and records nothing. pre: prompt is callable. post: no throw.
bool decide(Permissions& perms, const ToolCall& tc, const Prompter& prompt);

}  // namespace moocode

#endif  // MOOCODE_PERMISSIONS_HPP
