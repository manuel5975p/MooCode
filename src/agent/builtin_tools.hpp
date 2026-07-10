#ifndef MOOCODE_BUILTIN_TOOLS_HPP
#define MOOCODE_BUILTIN_TOOLS_HPP

// The standard tool set that makes moocode a usable coding agent: filesystem
// read/write/edit/list plus a sandboxed shell. All filesystem paths are
// confined under `root`; the shell runs with `root` as its working directory.

#include <cstddef>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>

#include "agent/tools.hpp"

namespace moocode {

// A file mutation reported by write_file/edit_file, carrying enough to compute a
// diff. `old_content` is the file's prior contents ("" if it did not exist).
struct FileChange {
    std::string path;
    std::string old_content;
    std::string new_content;
};
// Fired after a successful write/edit. The consumer (TUI or CLI) computes and
// renders the diff; builtin_tools intentionally does not depend on agent_diff.
using FileChangeFn = std::function<void(const FileChange&)>;

// Asked to approve a single out-of-root access at the moment a tool resolves a
// path that escapes the confinement `root` (and the matching static
// allow_*_outside_root permission is off). `kind` is "read" or "write";
// `resolved` is the canonical target; `requested` is the raw path argument.
// Returns true to permit this access. This is the interactive "outside root"
// permission tier — distinct from approving the tool itself — so a user can
// grant filesystem-escape access once/for the session/always without blanket
// unlocking the sandbox. An empty callback means "deny" (the historical
// hard-fail behavior).
using EscapeApprovalFn = std::function<bool(
    std::string_view kind, const std::filesystem::path& resolved,
    const std::string& requested)>;

struct ToolOptions {
    std::filesystem::path root = ".";  // confinement root for file ops & bash cwd
    std::size_t max_read_bytes = 256 * 1024;  // read_file cap (truncates, notes it)
    int bash_timeout_secs = 30;               // run_bash wall-clock limit
    FileChangeFn on_file_change;  // optional; fired after a successful write/edit
    bool rtk = false;  // when true, run_bash rewrites simple cmds to `rtk <cmd>`
    // Sandbox-relaxation permissions, granted independently. When set, the
    // corresponding tools may resolve paths outside the confinement `root`:
    //   allow_read_outside_root  -> read_file, list_dir
    //   allow_write_outside_root -> write_file, edit_file
    // run_bash is unaffected (it has always run scoped only by its cwd).
    bool allow_read_outside_root = false;
    bool allow_write_outside_root = false;
    // Consulted at resolve time when a path escapes `root` and the static
    // permission above is off: the interactive "outside root" approval tier. See
    // EscapeApprovalFn. Empty (the default) preserves the old hard-fail deny.
    EscapeApprovalFn approve_escape;
};

// Individual factories (each independently testable).
Tool read_file_tool(ToolOptions opts);
Tool write_file_tool(ToolOptions opts);
Tool edit_file_tool(ToolOptions opts);
Tool list_dir_tool(ToolOptions opts);
Tool run_bash_tool(ToolOptions opts);

// Register the full builtin set into `reg`.
void register_builtin_tools(ToolRegistry& reg, ToolOptions opts);

}  // namespace moocode

#endif  // MOOCODE_BUILTIN_TOOLS_HPP
