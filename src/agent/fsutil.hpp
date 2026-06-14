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

// Resolve `rel` under `root` and reject any path that escapes the sandbox.
// pre: root names an existing/absolute-resolvable directory.
// post: on success the returned path is `root` or strictly within it.
inline std::expected<std::filesystem::path, Error> resolve_in_root(
    const std::filesystem::path& root, const std::string& rel) {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::path base = fs::weakly_canonical(fs::absolute(root, ec), ec);
    if (ec) return std::unexpected(Error{.msg = "cannot resolve sandbox root", .code = 0});
    fs::path target = fs::weakly_canonical(base / rel, ec);
    if (ec) return std::unexpected(Error{.msg = "cannot resolve path: " + rel, .code = 0});

    const std::string b = base.string();
    const std::string t = target.string();
    bool inside = (t == b) ||
                  (t.size() > b.size() && t.compare(0, b.size(), b) == 0 &&
                   t[b.size()] == '/');
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
// Lookup order: <cwd>/MOO.md (project root), then
// <cwd>/.moo/MOO.md (project-local hidden), then
// <home>/MOO.md (global ~/.moo/MOO.md). First match wins.
// Returns empty when none are present.
inline std::string load_moocode_md(const std::string& home,
                                   const std::filesystem::path& cwd) {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::path root_md = cwd / "MOO.md";
    if (fs::exists(root_md, ec)) return slurp(root_md);
    fs::path local = cwd / ".moo" / "MOO.md";
    if (fs::exists(local, ec)) return slurp(local);
    if (!home.empty()) {
        fs::path global = fs::path(home) / "MOO.md";
        if (fs::exists(global, ec)) return slurp(global);
    }
    return {};
}

}  // namespace moocode

#endif  // MOOCODE_FSUTIL_HPP
