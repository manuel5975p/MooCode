#ifndef FLAGENT_FSUTIL_HPP
#define FLAGENT_FSUTIL_HPP

// Shared filesystem-sandbox helpers. Header-only; depends only on the STL and
// agent_types (Error). Keeps the security-sensitive escape check in one place so
// the tool and @-mention layers cannot drift apart.

#include <expected>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#include "agent/types.hpp"  // Error

namespace flagent {

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

// Load custom system-prompt addendum from FLAGENT.md.
// Lookup order: <cwd>/.flagent/FLAGENT.md (project-local) overrides
// <home>/FLAGENT.md (global ~/.flagent/FLAGENT.md). Returns empty when absent.
inline std::string load_flagent_md(const std::string& home,
                                   const std::filesystem::path& cwd) {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::path local = cwd / ".flagent" / "FLAGENT.md";
    if (fs::exists(local, ec)) return slurp(local);
    if (!home.empty()) {
        fs::path global = fs::path(home) / "FLAGENT.md";
        if (fs::exists(global, ec)) return slurp(global);
    }
    return {};
}

}  // namespace flagent

#endif  // FLAGENT_FSUTIL_HPP
