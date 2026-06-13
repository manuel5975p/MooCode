#ifndef FLAGENT_BUILTIN_TOOLS_HPP
#define FLAGENT_BUILTIN_TOOLS_HPP

// The standard tool set that makes flagent a usable coding agent: filesystem
// read/write/edit/list plus a sandboxed shell. All filesystem paths are
// confined under `root`; the shell runs with `root` as its working directory.

#include <cstddef>
#include <filesystem>
#include <functional>
#include <string>

#include "agent/tools.hpp"

namespace flagent {

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

struct ToolOptions {
    std::filesystem::path root = ".";  // confinement root for file ops & bash cwd
    std::size_t max_read_bytes = 256 * 1024;  // read_file cap (truncates, notes it)
    int bash_timeout_secs = 30;               // run_bash wall-clock limit
    FileChangeFn on_file_change;  // optional; fired after a successful write/edit
    bool rtk = false;  // when true, run_bash rewrites simple cmds to `rtk <cmd>`
};

// Individual factories (each independently testable).
Tool read_file_tool(ToolOptions opts);
Tool write_file_tool(ToolOptions opts);
Tool edit_file_tool(ToolOptions opts);
Tool list_dir_tool(ToolOptions opts);
Tool run_bash_tool(ToolOptions opts);

// Register the full builtin set into `reg`.
void register_builtin_tools(ToolRegistry& reg, ToolOptions opts);

}  // namespace flagent

#endif  // FLAGENT_BUILTIN_TOOLS_HPP
