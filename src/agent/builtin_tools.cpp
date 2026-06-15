#include "agent/builtin_tools.hpp"

#include "agent/proc.hpp"  // run_process, select_lines, kProcOutputCap

#include "agent/fsutil.hpp"
#include "agent/json_util.hpp"
#include "agent/rtk.hpp"  // rtk_rewrite, rtk_strip_escape
#include "agent/strutil.hpp"  // default_trunc_marker

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

namespace moocode {

namespace {

using Result = std::expected<std::string, Error>;
// Lift a filesystem std::error_code into our Error type with a context prefix.
// Returns void-success on clear ec so it composes through .and_then().
std::expected<void, Error> ec_check(const std::error_code& ec,
                                    std::string ctx) {
    if (!ec) return {};
    return std::unexpected(Error{.msg = std::move(ctx) + ": " + ec.message(), .code = ec.value()});
}

// Verify `p` names a regular file. ENOENT collapses into "not a file" so the
// common-case message stays terse; any other ec (EACCES, EIO, ELOOP, ...) is a
// real filesystem failure that must not be silently misreported as "not found"
// — surface it verbatim. `orig` is the user-visible path used in messages.
std::expected<fs::path, Error> require_regular_file(fs::path p,
                                                    const std::string& orig) {
    std::error_code ec;
    if (fs::is_regular_file(p, ec)) return p;
    if (ec && ec != std::errc::no_such_file_or_directory)
        return std::unexpected(
            Error{.msg = "cannot stat " + orig + ": " + ec.message(), .code = ec.value()});
    return std::unexpected(Error{.msg = "not a file: " + orig, .code = 0});
}

// As above for directories.
std::expected<fs::path, Error> require_directory(fs::path p,
                                                 const std::string& orig) {
    std::error_code ec;
    if (fs::is_directory(p, ec)) return p;
    if (ec && ec != std::errc::no_such_file_or_directory)
        return std::unexpected(
            Error{.msg = "cannot stat " + orig + ": " + ec.message(), .code = ec.value()});
    return std::unexpected(Error{.msg = "not a directory: " + orig, .code = 0});
}

// --- run_bash helpers -------------------------------------------------------

constexpr size_t kBashOutputCap = kProcOutputCap;  // 1 MiB captured, then truncated

// Shell wrapper kept for the run_bash tool: /bin/sh -c cmd through run_process.
Result run_command(const std::string& cmd, const fs::path& cwd, int timeout_secs,
                   size_t max_bytes = kProcOutputCap, int64_t max_lines = -1) {
    return run_process({"/bin/sh", "-c", cmd}, cwd, timeout_secs, max_bytes,
                       max_lines);
}

}  // namespace

Tool read_file_tool(ToolOptions opts) {
    ToolSpec spec{
        "read_file",
        "Read file contents. `offset` (1-based start line) + `lines` (max "
        "lines) read that window; omit both for whole file. Sandboxed to "
        "project root; big reads truncated with marker.",
        json::parse_or(R"({
            "type":"object",
            "properties":{
              "path":{"type":"string","description":"file path"},
              "offset":{"type":"integer","description":"1-based start line"},
              "lines":{"type":"integer","description":"max lines to return"}},
            "required":["path"]})")};
    return Tool{.spec = std::move(spec),
        .run = [opts](const nlohmann::json& a) -> Result {
        return ::moocode::json::arg_string(a, "path").and_then([&](std::string path) -> Result {
            return resolve_in_root(opts.root, path)
                .and_then([&](fs::path r) {
                    return require_regular_file(std::move(r), path);
                })
                .and_then([&](fs::path resolved) -> Result {
                    std::string content = slurp(resolved);

                    // Optional line-window: clamp offset >= 1 and a negative/
                    // absent `lines` to "rest of file".
                    bool has_offset = a.contains("offset") &&
                                      a["offset"].is_number_integer();
                    bool has_lines = a.contains("lines") &&
                                     a["lines"].is_number_integer();
                    if (has_offset || has_lines) {
                        int64_t offset =
                            has_offset ? a["offset"].get<int64_t>() : 1;
                        if (offset < 1) offset = 1;
                        int64_t limit =
                            has_lines ? a["lines"].get<int64_t>() : -1;
                        if (has_lines && limit < 0) limit = 0;
                        content = select_lines(content, offset, limit);
                    }

                    if (content.size() > opts.max_read_bytes) {
                        size_t full = content.size();
                        content.resize(opts.max_read_bytes);
                        content += default_trunc_marker(opts.max_read_bytes, full);
                    }
                    return content;
                });
        });
    }};
}

Tool write_file_tool(ToolOptions opts) {
    ToolSpec spec{
        "write_file",
        "Create or overwrite file with `content`. Sandboxed to project root; "
        "parent dirs created.",
        json::parse_or(R"({
            "type":"object",
            "properties":{
              "path":{"type":"string"},
              "content":{"type":"string"}},
            "required":["path","content"]})")};
    return Tool{.spec = std::move(spec),
        .run = [opts](const nlohmann::json& a) -> Result {
        auto path = ::moocode::json::arg_string(a, "path");
        if (!path) return std::unexpected(path.error());
        auto content = ::moocode::json::arg_string(a, "content");
        if (!content) return std::unexpected(content.error());

        return resolve_in_root(opts.root, *path)
            .and_then([&](fs::path resolved) -> Result {
                // Snapshot prior contents for the change callback. The stat is
                // best-effort: any real I/O failure (perms, EIO) re-surfaces at
                // the open/write below, so we treat "not a regular file" — for
                // any reason, including ENOENT — uniformly as "new file".
                std::error_code ec;
                bool is_file = fs::is_regular_file(resolved, ec);
                std::string old = is_file ? slurp(resolved) : std::string();

                ec.clear();
                fs::create_directories(resolved.parent_path(), ec);
                if (auto r = ec_check(
                        ec, "cannot create parent directories for " + *path);
                    !r)
                    return std::unexpected(r.error());

                std::ofstream f(resolved, std::ios::binary | std::ios::trunc);
                if (!f)
                    return std::unexpected(
                        Error{.msg = "cannot open for write: " + *path, .code = 0});
                f.write(content->data(),
                        static_cast<std::streamsize>(content->size()));
                if (!f)
                    return std::unexpected(Error{.msg = "write failed: " + *path, .code = 0});
                f.close();
                if (opts.on_file_change)
                    opts.on_file_change(
                        FileChange{.path = *path, .old_content = std::move(old), .new_content = *content});
                return "wrote " + std::to_string(content->size()) + " bytes to " +
                       *path;
            });
    }};
}

Tool edit_file_tool(ToolOptions opts) {
    ToolSpec spec{
        "edit_file",
        "Replace text `old` with `new` in file. `old` must occur exactly "
        "once, else no change + error. Sandboxed to project root.",
        json::parse_or(R"({
            "type":"object",
            "properties":{
              "path":{"type":"string"},
              "old":{"type":"string","description":"exact unique text to replace"},
              "new":{"type":"string","description":"replacement text"}},
            "required":["path","old","new"]})")};
    return Tool{.spec = std::move(spec),
        .run = [opts](const nlohmann::json& a) -> Result {
        auto path = ::moocode::json::arg_string(a, "path");
        if (!path) return std::unexpected(path.error());
        auto old_s = ::moocode::json::arg_string(a, "old");
        if (!old_s) return std::unexpected(old_s.error());
        auto new_s = ::moocode::json::arg_string(a, "new");
        if (!new_s) return std::unexpected(new_s.error());
        if (old_s->empty())
            return std::unexpected(Error{.msg = "`old` must be non-empty", .code = 0});

        return resolve_in_root(opts.root, *path)
            .and_then(
                [&](fs::path r) { return require_regular_file(std::move(r), *path); })
            .and_then([&](fs::path resolved) -> Result {
                std::string content = slurp(resolved);
                size_t count = 0;
                for (size_t pos = content.find(*old_s); pos != std::string::npos;
                     pos = content.find(*old_s, pos + old_s->size()))
                    ++count;
                if (count == 0)
                    return std::unexpected(
                        Error{.msg = "`old` string not found in " + *path, .code = 0});
                if (count > 1)
                    return std::unexpected(
                        Error{.msg = "`old` string is not unique (" +
                                  std::to_string(count) + " matches) in " + *path +
                                  "; include more surrounding context",
                            .code = 0});

                std::string before = content;  // whole-file snapshot for the diff
                size_t pos = content.find(*old_s);
                content.replace(pos, old_s->size(), *new_s);
                std::ofstream f(resolved, std::ios::binary | std::ios::trunc);
                if (!f)
                    return std::unexpected(
                        Error{.msg = "cannot open for write: " + *path, .code = 0});
                f.write(content.data(),
                        static_cast<std::streamsize>(content.size()));
                if (!f) return std::unexpected(Error{.msg = "write failed: " + *path, .code = 0});
                f.close();
                if (opts.on_file_change)
                    opts.on_file_change(
                        FileChange{.path = *path, .old_content = std::move(before), .new_content = std::move(content)});
                return "edited " + *path;
            });
    }};
}

Tool list_dir_tool(ToolOptions opts) {
    ToolSpec spec{
        "list_dir",
        "List dir entries, sorted, trailing / on subdirs. Sandboxed to "
        "project root.",
        json::parse_or(R"({
            "type":"object",
            "properties":{"path":{"type":"string","description":"directory path"}},
            "required":["path"]})")};
    return Tool{.spec = std::move(spec),
        .run = [opts](const nlohmann::json& a) -> Result {
        return ::moocode::json::arg_string(a, "path").and_then([&](std::string path) -> Result {
            return resolve_in_root(opts.root, path)
                .and_then(
                    [&](fs::path r) { return require_directory(std::move(r), path); })
                .and_then([&](fs::path resolved) -> Result {
                    // Manual iteration with error_code: a permission error in
                    // mid-iteration must surface as Error, not a partial list.
                    std::error_code ec;
                    fs::directory_iterator it(resolved, ec);
                    if (auto r = ec_check(ec, "cannot list " + path); !r)
                        return std::unexpected(r.error());

                    std::vector<std::string> names;
                    const fs::directory_iterator end;
                    for (; it != end; it.increment(ec)) {
                        if (auto r = ec_check(ec, "iteration error in " + path);
                            !r)
                            return std::unexpected(r.error());
                        std::error_code st_ec;
                        bool is_dir = it->is_directory(st_ec);
                        if (auto r = ec_check(
                                st_ec, "cannot stat entry " +
                                           it->path().filename().string() +
                                           " in " + path);
                            !r)
                            return std::unexpected(r.error());
                        std::string name = it->path().filename().string();
                        if (is_dir) name += "/";
                        names.push_back(std::move(name));
                    }
                    std::ranges::sort(names);
                    std::string out;
                    for (const auto& n : names) {
                        out += n;
                        out += "\n";
                    }
                    return out;
                });
        });
    }};
}

Tool run_bash_tool(ToolOptions opts) {
    ToolSpec spec{
        "run_bash",
        "Run shell command via /bin/sh from project root. Returns merged "
        "stdout+stderr + exit code; output capped, command killed on timeout. "
        "`max_bytes` + `max_lines` set output cap — defaults: 1 MiB, unlimited "
        "lines. Prefer git_* tools over `git` here; they return compact, "
        "token-optimized output.",
        json::parse_or(R"json({
            "type":"object",
            "properties":{
              "cmd":{"type":"string","description":"shell command"},
              "max_bytes":{"type":"integer","description":"max output bytes (default 1 MiB)"},
              "max_lines":{"type":"integer","description":"max output lines"}},
            "required":["cmd"]})json")};
    return Tool{.spec = std::move(spec),
        .run = [opts](const nlohmann::json& a) -> Result {
        return ::moocode::json::arg_string(a, "cmd").and_then([&](std::string cmd) -> Result {
            // rtk rewrite: only when enabled+available (opts.rtk already ANDs
            // detection with config, set in main). A leading "\ " escape forces
            // raw output (the escape is stripped, no rewrite).
            if (opts.rtk) {
                std::string stripped = rtk_strip_escape(cmd);
                if (stripped.size() != cmd.size()) {
                    cmd = std::move(stripped);  // escape present -> run raw
                } else if (auto rw = rtk_rewrite(cmd)) {
                    cmd = std::move(*rw);
                }
            }
            size_t max_bytes = kBashOutputCap;
            if (a.contains("max_bytes") && a["max_bytes"].is_number_integer()) {
                auto mb = a["max_bytes"].get<int64_t>();
                if (mb > 0) max_bytes = static_cast<size_t>(mb);
            }
            int64_t max_lines = -1;
            if (a.contains("max_lines") && a["max_lines"].is_number_integer()) {
                auto ml = a["max_lines"].get<int64_t>();
                if (ml > 0) max_lines = ml;
            }
            return run_command(cmd, opts.root, opts.bash_timeout_secs,
                               max_bytes, max_lines);
        });
    }};
}

void register_builtin_tools(ToolRegistry& reg, ToolOptions opts) {
    reg.add(read_file_tool(opts));
    reg.add(write_file_tool(opts));
    reg.add(edit_file_tool(opts));
    reg.add(list_dir_tool(opts));
    reg.add(run_bash_tool(opts));
}

}  // namespace moocode
