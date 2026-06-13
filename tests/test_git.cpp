#include "agent/git_tools.hpp"

#include <filesystem>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "test_harness.hpp"

using namespace flagent;
using nlohmann::json;

namespace {
// Fake runner: records argv, returns canned text. No real git, no rtk.
struct FakeRunner {
    std::vector<std::vector<std::string>> calls;
    std::string result = "OUT";
    GitRunFn fn() {
        return [this](const std::vector<std::string>& argv,
                      const std::filesystem::path&) -> std::expected<std::string, Error> {
            calls.push_back(argv);
            return result;
        };
    }
};
Tool find_tool(const std::vector<Tool>& ts, const std::string& name) {
    for (const Tool& t : ts)
        if (t.spec.name == name) return t;
    return Tool{};
}
}  // namespace

TEST("git_status_builds_argv_without_rtk") {
    FakeRunner r;
    GitConfig cfg;
    cfg.rtk_available = false;
    cfg.root = ".";
    auto tools = git_tools(cfg, r.fn());
    auto out = find_tool(tools, "git_status").run({});
    CHECK(out.has_value());
    CHECK_EQ(r.calls.size(), size_t(1));
    CHECK(r.calls[0] == (std::vector<std::string>{"git", "status"}));
}

TEST("git_diff_uses_rtk_and_staged_path") {
    FakeRunner r;
    GitConfig cfg;
    cfg.rtk_available = true;
    cfg.root = ".";
    auto tools = git_tools(cfg, r.fn());
    auto out = find_tool(tools, "git_diff").run({{"staged", true}, {"path", "src/a.cpp"}});
    CHECK(out.has_value());
    CHECK(r.calls[0] == (std::vector<std::string>{"rtk", "git", "diff", "--staged", "--", "src/a.cpp"}));
}

TEST("git_log_count_and_path") {
    FakeRunner r;
    GitConfig cfg;
    cfg.rtk_available = false;
    cfg.root = ".";
    auto tools = git_tools(cfg, r.fn());
    CHECK(find_tool(tools, "git_log").run({{"count", 5}, {"path", "src"}}).has_value());
    CHECK(r.calls[0] == (std::vector<std::string>{"git", "log", "-n", "5", "--", "src"}));
}

TEST("git_log_default_count") {
    FakeRunner r;
    GitConfig cfg;
    cfg.rtk_available = false;
    cfg.root = ".";
    auto tools = git_tools(cfg, r.fn());
    CHECK(find_tool(tools, "git_log").run({}).has_value());
    CHECK(r.calls[0] == (std::vector<std::string>{"git", "log", "-n", "20"}));
}

TEST("git_show_requires_rev") {
    FakeRunner r;
    GitConfig cfg;
    cfg.rtk_available = false;
    cfg.root = ".";
    auto tools = git_tools(cfg, r.fn());
    auto bad = find_tool(tools, "git_show").run({});  // missing rev -> Error, no call
    CHECK(!bad.has_value());
    CHECK_EQ(r.calls.size(), size_t(0));
    CHECK(find_tool(tools, "git_show").run({{"rev", "HEAD~1"}}).has_value());
    CHECK(r.calls[0] == (std::vector<std::string>{"git", "show", "HEAD~1"}));
}

TEST("git_branch_argv") {
    FakeRunner r;
    GitConfig cfg;
    cfg.rtk_available = false;
    cfg.root = ".";
    auto tools = git_tools(cfg, r.fn());
    CHECK(find_tool(tools, "git_branch").run({}).has_value());
    CHECK(r.calls[0] == (std::vector<std::string>{"git", "branch"}));
}

TEST("git_tool_set_is_read_only") {
    GitConfig cfg;
    FakeRunner r;
    auto tools = git_tools(cfg, r.fn());
    CHECK_EQ(tools.size(), size_t(5));  // status, diff, log, show, branch
    for (const Tool& t : tools) {
        CHECK(t.spec.name.find("commit") == std::string::npos);
        CHECK(t.spec.name.find("push") == std::string::npos);
    }
}
