#ifndef MOOCODE_FSUTIL_HPP
#define MOOCODE_FSUTIL_HPP

// Shared filesystem-sandbox helpers. Header-only; depends only on the STL and
// agent_types (Error). Keeps the security-sensitive escape check in one place so
// the tool and @-mention layers cannot drift apart.

#include <expected>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#include "agent/types.hpp"  // Error

namespace moocode {

// Resolve `rel` under `root`, optionally rejecting any path that escapes the
// sandbox. When `confine` is false the path is still normalized (so callers get
// a canonical absolute path) but escapes are permitted — this is how the
// separately-grantable read/write-outside-root permissions relax confinement.
// An absolute `rel` replaces `root` per the usual `operator/` semantics.
// pre: root names an existing/absolute-resolvable directory.
// post: on success the returned path is canonical; when `confine`, it is `root`
//       or strictly within it.
inline std::expected<std::filesystem::path, Error> resolve_in_root(
    const std::filesystem::path& root, const std::string& rel,
    bool confine = true) {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::path base = fs::weakly_canonical(fs::absolute(root, ec), ec);
    if (ec) return std::unexpected(Error{.msg = "cannot resolve sandbox root", .code = 0});
    fs::path target = fs::weakly_canonical(base / rel, ec);
    if (ec) return std::unexpected(Error{.msg = "cannot resolve path: " + rel, .code = 0});

    if (!confine) return target;

    const std::string b = base.string();
    const std::string t = target.string();
    // The byte after the matched root prefix must be a path separator so that
    // "/rootfoo" cannot pass as living under "/root". Accept both '/' and the
    // native separator (Windows weakly_canonical yields backslashes).
    constexpr char kSep = static_cast<char>(fs::path::preferred_separator);
    bool inside = (t == b) ||
                  (t.size() > b.size() && t.compare(0, b.size(), b) == 0 &&
                   (t[b.size()] == '/' || t[b.size()] == kSep));
    if (!inside)
        return std::unexpected(Error{.msg = "path escapes the sandbox root: " + rel, .code = 0});
    return target;
}

// Slurp a whole file as binary. Empty string on open/read failure. Total.
inline std::string slurp(const std::filesystem::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(f), {});
}

// Load custom system-prompt addendum from MOO.md.
// When present, both <home>/MOO.md (~/.moo/MOO.md) and <cwd>/MOO.md
// (PWD/MOO.md) are concatenated: global first, then PWD after.
// If PWD/MOO.md is missing, <cwd>/.moo/MOO.md serves as fallback.
// Returns empty when none are present.
inline std::string load_moocode_md(const std::string& home,
                                   const std::filesystem::path& cwd) {
    namespace fs = std::filesystem;
    std::error_code ec;
    std::string result;

    // 1. Global: ~/.moo/MOO.md goes first.
    if (!home.empty()) {
        fs::path global = fs::path(home) / "MOO.md";
        if (fs::exists(global, ec)) result = slurp(global);
    }

    // 2. Project root: PWD/MOO.md (preferred), or PWD/.moo/MOO.md (fallback).
    fs::path root_md = cwd / "MOO.md";
    std::string proj;
    if (fs::exists(root_md, ec))
        proj = slurp(root_md);
    else {
        fs::path local = cwd / ".moo" / "MOO.md";
        if (fs::exists(local, ec)) proj = slurp(local);
    }

    if (!proj.empty()) {
        if (!result.empty()) result += "\n\n";
        result += std::move(proj);
    }

    return result;
}

}  // namespace moocode

#endif  // MOOCODE_FSUTIL_HPP
