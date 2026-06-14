#ifndef MOOCODE_RTK_HPP
#define MOOCODE_RTK_HPP

// Pure decision: should a run_bash command be wrapped as `rtk <cmd>` to shrink
// its output? Only simple single-command invocations of an allowlisted program
// qualify; anything with shell metacharacters (pipe/redirect/compound/subshell)
// is left verbatim. No I/O, no rtk-presence check — that gate lives at the call
// site (ToolOptions::rtk). git is deliberately excluded: it has its own tools.

#include <optional>
#include <string>
#include <string_view>

namespace moocode {

// Rewritten "rtk <cmd>" when `cmd` is a bare allowlisted invocation, else
// nullopt (run verbatim). The leading "\ " raw-escape yields nullopt here; the
// call site strips it via rtk_strip_escape before running raw. Total; no throw.
std::optional<std::string> rtk_rewrite(std::string_view cmd);

// If `cmd` begins with the raw-escape "\ " (backslash, space), return the
// remainder with that prefix removed; else return `cmd` unchanged. The run_bash
// call site applies this so the escape never reaches /bin/sh. Total.
std::string rtk_strip_escape(std::string_view cmd);

}  // namespace moocode

#endif  // MOOCODE_RTK_HPP
