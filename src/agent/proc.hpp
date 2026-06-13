#ifndef FLAGENT_PROC_HPP
#define FLAGENT_PROC_HPP

// One process-spawning primitive shared by run_bash and the git tools. Forks
// argv[0] (PATH-resolved via execvp) in its own process group, sets cwd, merges
// stdout+stderr, enforces a wall-clock timeout by killing the group, caps the
// captured bytes/lines, and appends an "[exit code: N]" / "[killed by signal N]"
// trailer. No shell unless argv asks for one ({"/bin/sh","-c",cmd}).

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <string>
#include <vector>

#include "agent/types.hpp"

namespace flagent {

// Default capture ceiling: 1 MiB, then a truncation marker is appended.
inline constexpr std::size_t kProcOutputCap = 1u << 20;

// Run argv with cwd, capture merged output, enforce timeout_secs. argv nonempty;
// timeout_secs > 0. Error only on pipe/fork/timeout; a nonzero child exit is a
// success whose text carries the "[exit code: N]" trailer.
std::expected<std::string, Error> run_process(
    const std::vector<std::string>& argv, const std::filesystem::path& cwd,
    int timeout_secs, std::size_t max_bytes = kProcOutputCap,
    std::int64_t max_lines = -1);

// First `limit` lines of `text` starting at 1-based `offset`. offset >= 1;
// limit < 0 means rest-of-file (run_process relies on this for max_lines=-1).
// Total: out-of-range windows clamp to what exists.
std::string select_lines(const std::string& text, std::int64_t offset,
                         std::int64_t limit);

}  // namespace flagent

#endif  // FLAGENT_PROC_HPP
