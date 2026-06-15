#include "agent/permissions.hpp"

#include <utility>

namespace moocode {

Permissions::Permissions(std::set<std::string> always, SaveFn save)
    : always_(std::move(always)), save_(std::move(save)) {}

bool Permissions::allowed(const std::string& tool) const {
    std::lock_guard lk(mtx_);
    return session_.count(tool) != 0 || always_.count(tool) != 0;
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
    if (perms.allowed(tc.name)) return true;
    switch (prompt(tc)) {
        case Approval::Deny: return false;
        case Approval::Once: return true;
        case Approval::Session: perms.grant_session(tc.name); return true;
        case Approval::Always: perms.grant_always(tc.name); return true;
    }
    return false;  // unreachable; all enumerators handled
}

}  // namespace moocode
