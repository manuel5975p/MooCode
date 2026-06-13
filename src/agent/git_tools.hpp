#ifndef FLAGENT_GIT_TOOLS_HPP
#define FLAGENT_GIT_TOOLS_HPP

// Read-only local-git inspection tools confined to a project root: git_status,
// git_diff, git_log, git_show, git_branch. Output goes through `rtk git ...`
// when rtk is on PATH, else plain `git ...`. No mutating subcommands exist here
// (the project never commits). A GitRunFn seam makes argv construction testable
// without a real repository or rtk binary.

#include <expected>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include "agent/tools.hpp"
#include "agent/types.hpp"

namespace flagent {

struct GitConfig {
    std::filesystem::path root = ".";  // cwd for every git invocation
    bool rtk_available = false;        // prefix argv with "rtk" when true
    int timeout_secs = 30;
};

// Run an argv with the given cwd and return merged output. Default binds
// run_process; tests inject a fake. pre: argv nonempty.
using GitRunFn = std::function<std::expected<std::string, Error>(
    const std::vector<std::string>& argv, const std::filesystem::path& cwd)>;

// Build the read-only git tools (git_status, git_diff, git_log, git_show,
// git_branch). `run` empty => real run_process transport. post: 5 tools.
std::vector<Tool> git_tools(GitConfig cfg, GitRunFn run = {});

}  // namespace flagent

#endif  // FLAGENT_GIT_TOOLS_HPP
