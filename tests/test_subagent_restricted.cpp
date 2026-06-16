#include "agent/subagent_tool.hpp"

#include <algorithm>
#include <expected>
#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "agent/provider.hpp"
#include "agent/tools.hpp"
#include "test_harness.hpp"

using namespace moocode;

namespace {

// Provider that emits a scripted set of tool calls on its first turn, then a
// plain final answer. Records the tool specs it was advertised each turn so a
// test can assert which tools the sub-agent's registry exposed.
struct ScriptedProvider : Provider {
    std::string name = "m";
    std::vector<ToolCall> first_calls;
    std::vector<std::vector<ToolSpec>> seen;  // advertised specs per complete()
    int calls = 0;

    std::expected<Turn, Error> complete(
        const Conversation&, const std::vector<ToolSpec>& specs) override {
        seen.push_back(specs);
        ++calls;
        if (calls == 1)
            return Turn{.text = "",
                        .tool_calls = first_calls,
                        .finish_reason = "tool_calls",
                        .usage = {},
                        .reasoning = {}};
        return Turn{.text = "done",
                    .tool_calls = {},
                    .finish_reason = "stop",
                    .usage = {},
                    .reasoning = {}};
    }
    std::string model() const override { return name; }
};

// A tool that flips `*ran` when invoked. Empty object schema.
Tool recording_tool(const std::string& name, bool* ran) {
    return Tool{
        .spec = ToolSpec{name, "test tool",
                         nlohmann::json::parse(R"({"type":"object"})")},
        .run = [ran](const nlohmann::json&) -> std::expected<std::string, Error> {
            if (ran) *ran = true;
            return std::string("ok");
        }};
}

ToolCall call(const std::string& name) {
    return ToolCall{.id = "id_" + name, .name = name, .arguments_json = "{}"};
}

std::vector<std::string> spec_names(const std::vector<ToolSpec>& specs) {
    std::vector<std::string> out;
    for (const auto& s : specs) out.push_back(s.name);
    std::ranges::sort(out);
    return out;
}

bool has_name(const std::vector<ToolSpec>& specs, const std::string& n) {
    return std::ranges::any_of(specs,
                               [&](const ToolSpec& s) { return s.name == n; });
}

}  // namespace

TEST("restricted: tools arg limits the sub-agent's advertised registry") {
    ScriptedProvider parent;
    parent.first_calls = {call("alpha")};

    bool ran_alpha = false, ran_beta = false, ran_gamma = false;
    ToolRegistry parent_reg;
    parent_reg.add(recording_tool("alpha", &ran_alpha));
    parent_reg.add(recording_tool("beta", &ran_beta));
    parent_reg.add(recording_tool("gamma", &ran_gamma));

    SubagentConfig cfg;
    cfg.get_provider = [&parent]() -> Provider& { return parent; };
    cfg.tools = &parent_reg;
    Tool tool = spawn_subagent_restricted_tool(std::move(cfg));

    auto r = tool.run(nlohmann::json{{"prompt", "go"},
                                     {"tools", nlohmann::json::array({"alpha"})}});
    CHECK(r.has_value());
    // The sub-agent only ever saw 'alpha'.
    CHECK(!parent.seen.empty());
    if (!parent.seen.empty())
        CHECK_EQ(spec_names(parent.seen.front()),
                 std::vector<std::string>{"alpha"});
    CHECK(ran_alpha);
    CHECK(!ran_beta);
    CHECK(!ran_gamma);
}

TEST("restricted: omitted tools grants every (non-meta) tool") {
    ScriptedProvider parent;
    ToolRegistry parent_reg;
    bool a = false, b = false;
    parent_reg.add(recording_tool("alpha", &a));
    parent_reg.add(recording_tool("beta", &b));

    SubagentConfig cfg;
    cfg.get_provider = [&parent]() -> Provider& { return parent; };
    cfg.tools = &parent_reg;
    Tool tool = spawn_subagent_restricted_tool(std::move(cfg));

    auto r = tool.run(nlohmann::json{{"prompt", "go"}});
    CHECK(r.has_value());
    CHECK(!parent.seen.empty());
    if (!parent.seen.empty()) {
        CHECK(has_name(parent.seen.front(), "alpha"));
        CHECK(has_name(parent.seen.front(), "beta"));
    }
}

TEST("restricted: meta-tools are never advertised to the sub-agent") {
    ScriptedProvider parent;
    ToolRegistry parent_reg;
    parent_reg.add(recording_tool("alpha", nullptr));
    parent_reg.add(recording_tool("ask_user", nullptr));
    parent_reg.add(recording_tool("spawn_subagent", nullptr));
    parent_reg.add(recording_tool("load_skill", nullptr));

    SubagentConfig cfg;
    cfg.get_provider = [&parent]() -> Provider& { return parent; };
    cfg.tools = &parent_reg;
    Tool tool = spawn_subagent_restricted_tool(std::move(cfg));

    auto r = tool.run(nlohmann::json{{"prompt", "go"}});
    CHECK(r.has_value());
    if (!parent.seen.empty()) {
        CHECK_EQ(spec_names(parent.seen.front()),
                 std::vector<std::string>{"alpha"});
    }
}

TEST("restricted: permissions auto-approves listed, defers the rest to the gate") {
    ScriptedProvider parent;
    parent.first_calls = {call("safe"), call("danger")};

    bool ran_safe = false, ran_danger = false;
    ToolRegistry parent_reg;
    parent_reg.add(recording_tool("safe", &ran_safe));
    parent_reg.add(recording_tool("danger", &ran_danger));

    std::vector<std::string> gate_seen;
    SubagentConfig cfg;
    cfg.get_provider = [&parent]() -> Provider& { return parent; };
    cfg.tools = &parent_reg;
    // Parent gate denies everything and records what it was asked about.
    cfg.get_approve = [&gate_seen]() {
        return std::function<bool(const ToolCall&)>([&gate_seen](const ToolCall& tc) {
            gate_seen.push_back(tc.name);
            return false;
        });
    };
    Tool tool = spawn_subagent_restricted_tool(std::move(cfg));

    auto r = tool.run(nlohmann::json{
        {"prompt", "go"}, {"permissions", nlohmann::json::array({"safe"})}});
    CHECK(r.has_value());
    CHECK(ran_safe);     // pre-approved → ran without consulting the gate
    CHECK(!ran_danger);  // not pre-approved → gate consulted → denied
    // The gate saw only 'danger' (the pre-approved 'safe' bypassed it).
    CHECK_EQ(gate_seen.size(), std::size_t{1});
    if (!gate_seen.empty()) CHECK_EQ(gate_seen.front(), std::string("danger"));
}

TEST("restricted: unknown tool name in 'tools' is rejected") {
    ScriptedProvider parent;
    ToolRegistry parent_reg;
    parent_reg.add(recording_tool("alpha", nullptr));

    SubagentConfig cfg;
    cfg.get_provider = [&parent]() -> Provider& { return parent; };
    cfg.tools = &parent_reg;
    Tool tool = spawn_subagent_restricted_tool(std::move(cfg));

    auto r = tool.run(nlohmann::json{
        {"prompt", "go"}, {"tools", nlohmann::json::array({"nope"})}});
    CHECK(!r.has_value());
    if (!r) {
        CHECK(r.error().msg.find("nope") != std::string::npos);
        CHECK(r.error().msg.find("alpha") != std::string::npos);  // lists grantable
    }
    CHECK_EQ(parent.calls, 0);  // rejected before any provider call
}

TEST("restricted: unknown tool name in 'permissions' is rejected") {
    ScriptedProvider parent;
    ToolRegistry parent_reg;
    parent_reg.add(recording_tool("alpha", nullptr));

    SubagentConfig cfg;
    cfg.get_provider = [&parent]() -> Provider& { return parent; };
    cfg.tools = &parent_reg;
    Tool tool = spawn_subagent_restricted_tool(std::move(cfg));

    auto r = tool.run(nlohmann::json{
        {"prompt", "go"}, {"permissions", nlohmann::json::array({"ghost"})}});
    CHECK(!r.has_value());
    if (!r) CHECK(r.error().msg.find("ghost") != std::string::npos);
}

TEST("restricted: non-array 'tools' argument is rejected") {
    ScriptedProvider parent;
    ToolRegistry parent_reg;
    SubagentConfig cfg;
    cfg.get_provider = [&parent]() -> Provider& { return parent; };
    cfg.tools = &parent_reg;
    Tool tool = spawn_subagent_restricted_tool(std::move(cfg));

    auto r = tool.run(nlohmann::json{{"prompt", "go"}, {"tools", "alpha"}});
    CHECK(!r.has_value());
}

TEST("restricted: tool advertises tools and permissions parameters") {
    ScriptedProvider parent;
    ToolRegistry parent_reg;
    SubagentConfig cfg;
    cfg.get_provider = [&parent]() -> Provider& { return parent; };
    cfg.tools = &parent_reg;
    Tool tool = spawn_subagent_restricted_tool(std::move(cfg));

    const auto& props = tool.spec.parameters["properties"];
    CHECK(props.contains("prompt"));
    CHECK(props.contains("tools"));
    CHECK(props.contains("permissions"));
    CHECK_EQ(tool.spec.name, std::string("spawn_subagent_restricted"));
}
