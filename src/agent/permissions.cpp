#include "agent/permissions.hpp"

#include <utility>

#include <nlohmann/json.hpp>

namespace moocode {

namespace {

// True if `entry` ("tool" or "tool(pattern)") permits `tool` with `subject`.
bool entry_permits(const std::string& entry, const std::string& tool,
                   const std::string& subject) {
    if (entry == tool) return true;
    if (entry.size() < tool.size() + 2 || entry.back() != ')') return false;
    if (entry.compare(0, tool.size(), tool) != 0 || entry[tool.size()] != '(')
        return false;
    const std::string_view pattern(entry.data() + tool.size() + 1,
                                   entry.size() - tool.size() - 2);
    return glob_match(pattern, subject);
}

}  // namespace

bool glob_match(std::string_view pattern, std::string_view subject) {
    // Iterative wildcard match: on mismatch, retry from the last '*' with the
    // subject advanced by one (classic two-pointer backtracking, O(n*m)).
    std::size_t p = 0, s = 0;
    std::size_t star = std::string_view::npos, mark = 0;
    while (s < subject.size()) {
        if (p < pattern.size() && pattern[p] != '*' && pattern[p] == subject[s]) {
            ++p, ++s;
        } else if (p < pattern.size() && pattern[p] == '*') {
            star = p++;
            mark = s;
        } else if (star != std::string_view::npos) {
            p = star + 1;
            s = ++mark;
        } else {
            return false;
        }
    }
    while (p < pattern.size() && pattern[p] == '*') ++p;
    return p == pattern.size();
}

std::string permission_subject(const ToolCall& tc) {
    const auto j =
        nlohmann::json::parse(tc.arguments_json, nullptr, /*allow_exceptions=*/false);
    // Not a JSON object: the *_outside_root escape prompts carry the resolved
    // path verbatim — match patterns against it directly.
    if (!j.is_object()) return tc.arguments_json;
    for (const char* key : {"cmd", "path"})
        if (auto it = j.find(key); it != j.end() && it->is_string())
            return it->get<std::string>();
    return {};
}

Permissions::Permissions(std::set<std::string> always, SaveFn save)
    : always_(std::move(always)), save_(std::move(save)) {}

bool Permissions::allowed(const std::string& tool, const std::string& subject) const {
    std::lock_guard lk(mtx_);
    if (session_.count(tool) != 0 || always_.count(tool) != 0) return true;
    // Pattern entries are rare (hand-written in permissions.toml or via the
    // VS Code permission editor); a linear scan over both tiers is fine.
    for (const std::set<std::string>* tier : {&session_, &always_})
        for (const std::string& e : *tier)
            if (entry_permits(e, tool, subject)) return true;
    return false;
}

void Permissions::grant_session(const std::string& tool) {
    std::lock_guard lk(mtx_);
    session_.insert(tool);
}

void Permissions::grant_always(const std::string& tool) {
    std::set<std::string> snapshot;
    {
        std::lock_guard lk(mtx_);
        if (!always_.insert(tool).second) return;  // already known: nothing to persist
        snapshot = always_;  // copy under the lock; fire the hook unlocked
    }
    if (save_) save_(snapshot);
}

bool decide(Permissions& perms, const ToolCall& tc, const Prompter& prompt) {
    if (perms.allowed(tc.name, permission_subject(tc))) return true;
    switch (prompt(tc)) {
        case Approval::Deny: return false;
        case Approval::Once: return true;
        case Approval::Session: perms.grant_session(tc.name); return true;
        case Approval::Always: perms.grant_always(tc.name); return true;
    }
    return false;  // unreachable; all enumerators handled
}

}  // namespace moocode
