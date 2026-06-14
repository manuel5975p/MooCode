#include "agent/git_tools.hpp"

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "agent/json_util.hpp"
#include "agent/proc.hpp"

namespace moocode {

namespace {

// Holds the config + transport for the lifetime of the tool lambdas, which
// outlive the git_tools() call. Shared so every tool sees the same instance.
struct GitCtx {
    GitConfig cfg;
    GitRunFn run;
};

// {"rtk","git",sub...} when rtk_available, else {"git",sub...}.
std::vector<std::string> git_argv(const GitConfig& cfg,
                                  const std::vector<std::string>& sub) {
    std::vector<std::string> argv;
    if (cfg.rtk_available) argv.push_back("rtk");
    argv.push_back("git");
    argv.insert(argv.end(), sub.begin(), sub.end());
    return argv;
}

// Optional bool arg: true only when present and a JSON true.
bool opt_bool(const nlohmann::json& j, const char* key) {
    auto it = j.find(key);
    return it != j.end() && it->is_boolean() && it->get<bool>();
}

// Optional positive-integer arg with a default fallback.
int opt_count(const nlohmann::json& j, const char* key, int dflt) {
    auto it = j.find(key);
    if (it == j.end() || !it->is_number_integer()) return dflt;
    const int v = it->get<int>();
    return v > 0 ? v : dflt;
}

nlohmann::json empty_params() {
    return nlohmann::json{{"type", "object"}, {"properties", nlohmann::json::object()}};
}

}  // namespace

std::vector<Tool> git_tools(GitConfig cfg, GitRunFn run) {
    auto ctx = std::make_shared<GitCtx>(GitCtx{std::move(cfg), std::move(run)});
    if (!ctx->run) {
        ctx->run = [ctx](const std::vector<std::string>& argv,
                         const std::filesystem::path& cwd)
            -> std::expected<std::string, Error> {
            return run_process(argv, cwd, ctx->cfg.timeout_secs);
        };
    }

    std::vector<Tool> tools;

    tools.push_back(Tool{
        ToolSpec{"git_status",
                 "Show repo working-tree status. Read-only.",
                 empty_params()},
        [ctx](const nlohmann::json&) -> std::expected<std::string, Error> {
            return ctx->run(git_argv(ctx->cfg, {"status"}), ctx->cfg.root);
        }});

    tools.push_back(Tool{
        ToolSpec{"git_diff",
                 "Show working-tree changes, or staged with `staged`, "
                 "optionally limited to `path`. Read-only.",
                 nlohmann::json{
                     {"type", "object"},
                     {"properties",
                      {{"staged",
                        {{"type", "boolean"},
                         {"description", "show staged (index) changes"}}},
                       {"path",
                        {{"type", "string"},
                         {"description", "limit diff to this path"}}}}}}},
        [ctx](const nlohmann::json& args) -> std::expected<std::string, Error> {
            std::vector<std::string> sub{"diff"};
            if (opt_bool(args, "staged")) sub.push_back("--staged");
            if (auto path = ::moocode::json::get_string_opt(args, "path").value_or(std::nullopt); path && !path->empty()) {
                sub.push_back("--");
                sub.push_back(*path);
            }
            return ctx->run(git_argv(ctx->cfg, sub), ctx->cfg.root);
        }});

    tools.push_back(Tool{
        ToolSpec{"git_log",
                 "Show recent commits (default 20), optionally limited to "
                 "`path`. Read-only.",
                 nlohmann::json{
                     {"type", "object"},
                     {"properties",
                      {{"count",
                        {{"type", "integer"},
                         {"description", "commits to show (default 20)"}}},
                       {"path",
                        {{"type", "string"},
                         {"description", "limit log to this path"}}}}}}},
        [ctx](const nlohmann::json& args) -> std::expected<std::string, Error> {
            const int count = opt_count(args, "count", 20);
            std::vector<std::string> sub{"log", "-n", std::to_string(count)};
            if (auto path = ::moocode::json::get_string_opt(args, "path").value_or(std::nullopt); path && !path->empty()) {
                sub.push_back("--");
                sub.push_back(*path);
            }
            return ctx->run(git_argv(ctx->cfg, sub), ctx->cfg.root);
        }});

    tools.push_back(Tool{
        ToolSpec{"git_show",
                 "Show commit (or revision) in full with its diff. Read-only.",
                 nlohmann::json{
                     {"type", "object"},
                     {"properties",
                      {{"rev",
                        {{"type", "string"},
                         {"description", "revision to show (e.g. HEAD, sha, tag)"}}}}},
                     {"required", nlohmann::json::array({"rev"})}}},
        [ctx](const nlohmann::json& args) -> std::expected<std::string, Error> {
            auto rev = ::moocode::json::get_string_opt(args, "rev").value_or(std::nullopt);
            if (!rev || rev->empty())
                return std::unexpected(Error{.msg = "git_show: 'rev' is required", .code = 0});
            return ctx->run(git_argv(ctx->cfg, {"show", *rev}), ctx->cfg.root);
        }});

    tools.push_back(Tool{
        ToolSpec{"git_branch",
                 "List repo branches. Read-only.",
                 empty_params()},
        [ctx](const nlohmann::json&) -> std::expected<std::string, Error> {
            return ctx->run(git_argv(ctx->cfg, {"branch"}), ctx->cfg.root);
        }});

    return tools;
}

}  // namespace moocode
