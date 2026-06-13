#include "agent/subagent_tool.hpp"

#include <expected>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "agent/provider.hpp"
#include "agent/tools.hpp"
#include "test_harness.hpp"

using namespace flagent;

namespace {

// Provider that returns one fixed final answer (no tool calls) and reports a
// model name. Records how many times complete() ran so a test can assert the
// parent provider was (or wasn't) used.
struct CannedProvider : Provider {
    std::string answer;
    std::string name;
    int calls = 0;

    std::expected<Turn, Error> complete(const Conversation&,
                                        const std::vector<ToolSpec>&) override {
        ++calls;
        return Turn{.text = answer, .tool_calls = {}, .finish_reason = "stop", .usage = {}, .reasoning = {}};
    }
    std::string model() const override { return name; }
};

using MakeFn = std::function<std::expected<std::unique_ptr<Provider>, Error>(
    const std::string&)>;

// Build a spawn_subagent tool around `parent` with optional model callbacks and
// an empty sub-tool registry.
Tool make_tool(CannedProvider& parent,
               std::function<std::vector<SubagentModelInfo>()> list_models,
               MakeFn make_for_model) {
    SubagentConfig cfg;
    cfg.get_provider = [&parent]() -> Provider& { return parent; };
    cfg.tools = nullptr;  // => empty sub-registry
    cfg.list_models = std::move(list_models);
    cfg.make_provider_for_model = std::move(make_for_model);
    return spawn_subagent_tool(std::move(cfg));
}

std::vector<SubagentModelInfo> three_models() {
    return {{"MiniMax-M3", "openai"},
            {"deepseek-v4-flash", "openai"},
            {"claude-haiku", "anthropic"}};
}

}  // namespace

TEST("subagent: model arg routes to the override provider") {
    CannedProvider parent;
    parent.answer = "parent-answer";
    parent.name = "MiniMax-M3";

    std::string requested;
    auto tool = make_tool(parent, three_models,
        [&](const std::string& m) -> std::expected<std::unique_ptr<Provider>, Error> {
            requested = m;
            auto p = std::make_unique<CannedProvider>();
            p->answer = "override-answer";
            p->name = m;
            return p;
        });

    auto r = tool.run(nlohmann::json{{"prompt", "do it"}, {"model", "deepseek-v4-flash"}});
    CHECK(r.has_value());
    if (r) CHECK_EQ(*r, std::string("override-answer"));
    CHECK_EQ(requested, std::string("deepseek-v4-flash"));
    CHECK_EQ(parent.calls, 0);  // parent provider never used
}

TEST("subagent: requested model is canonicalized to the profile's casing") {
    CannedProvider parent;
    parent.answer = "parent-answer";
    parent.name = "MiniMax-M3";

    std::string requested;
    auto tool = make_tool(parent, three_models,
        [&](const std::string& m) -> std::expected<std::unique_ptr<Provider>, Error> {
            requested = m;
            auto p = std::make_unique<CannedProvider>();
            p->answer = "override-answer";
            p->name = m;
            return p;
        });

    // Request differs in case from the canonical "deepseek-v4-flash". The builder
    // must receive the canonical name from list_models(), not the raw request.
    auto r = tool.run(nlohmann::json{{"prompt", "do it"}, {"model", "DEEPSEEK-V4-FLASH"}});
    CHECK(r.has_value());
    if (r) CHECK_EQ(*r, std::string("override-answer"));
    CHECK_EQ(requested, std::string("deepseek-v4-flash"));
    CHECK_EQ(parent.calls, 0);
}

TEST("subagent: unknown model is rejected with the model list") {
    CannedProvider parent;
    parent.answer = "x";
    parent.name = "MiniMax-M3";

    bool built = false;
    auto tool = make_tool(parent, three_models,
        [&](const std::string&) -> std::expected<std::unique_ptr<Provider>, Error> {
            built = true;
            return std::unexpected(Error{.msg = "must not be called", .code = 0});
        });

    auto r = tool.run(nlohmann::json{{"prompt", "go"}, {"model", "gpt-5-turbo"}});
    CHECK(!r.has_value());
    if (!r) {
        CHECK(r.error().msg.find("gpt-5-turbo") != std::string::npos);
        CHECK(r.error().msg.find("deepseek-v4-flash") != std::string::npos);
    }
    CHECK(!built);                // membership check fails before the builder
    CHECK_EQ(parent.calls, 0);
}

TEST("subagent: omitted model uses the parent provider") {
    CannedProvider parent;
    parent.answer = "from-parent";
    parent.name = "MiniMax-M3";

    bool built = false;
    auto tool = make_tool(parent, three_models,
        [&](const std::string&) -> std::expected<std::unique_ptr<Provider>, Error> {
            built = true;
            return std::make_unique<CannedProvider>();
        });

    auto r = tool.run(nlohmann::json{{"prompt", "go"}});
    CHECK(r.has_value());
    if (r) CHECK_EQ(*r, std::string("from-parent"));
    CHECK(!built);
    CHECK_EQ(parent.calls, 1);
}

TEST("subagent: model equal to the parent model reuses the parent") {
    CannedProvider parent;
    parent.answer = "from-parent";
    parent.name = "MiniMax-M3";

    bool built = false;
    auto tool = make_tool(parent, three_models,
        [&](const std::string&) -> std::expected<std::unique_ptr<Provider>, Error> {
            built = true;
            return std::make_unique<CannedProvider>();
        });

    // case-insensitive match against the parent's current model
    auto r = tool.run(nlohmann::json{{"prompt", "go"}, {"model", "minimax-m3"}});
    CHECK(r.has_value());
    if (r) CHECK_EQ(*r, std::string("from-parent"));
    CHECK(!built);
}

TEST("subagent: model arg without a resolver reports unavailable") {
    CannedProvider parent;
    parent.answer = "x";
    parent.name = "MiniMax-M3";

    SubagentConfig cfg;
    cfg.get_provider = [&parent]() -> Provider& { return parent; };
    cfg.tools = nullptr;
    // list_models + make_provider_for_model left null
    auto tool = spawn_subagent_tool(std::move(cfg));

    auto r = tool.run(nlohmann::json{{"prompt", "go"}, {"model", "deepseek-v4-flash"}});
    CHECK(!r.has_value());
    if (!r) CHECK(r.error().msg.find("not available") != std::string::npos);
}

TEST("subagent: non-string model argument is rejected") {
    CannedProvider parent;
    parent.name = "MiniMax-M3";
    auto tool = make_tool(parent, three_models,
        [](const std::string& m) -> std::expected<std::unique_ptr<Provider>, Error> {
            auto p = std::make_unique<CannedProvider>();
            p->name = m;
            return p;
        });

    auto r = tool.run(nlohmann::json{{"prompt", "go"}, {"model", 123}});
    CHECK(!r.has_value());
}

TEST("subagent: model param description advertises grouped models") {
    CannedProvider parent;
    parent.name = "MiniMax-M3";
    auto tool = make_tool(parent, three_models,
        [](const std::string& m) -> std::expected<std::unique_ptr<Provider>, Error> {
            auto p = std::make_unique<CannedProvider>();
            p->name = m;
            return p;
        });

    const auto& params = tool.spec.parameters;
    CHECK(params.contains("properties"));
    CHECK(params["properties"].contains("model"));
    if (params["properties"].contains("model")) {
        std::string desc =
            params["properties"]["model"]["description"].get<std::string>();
        CHECK(desc.find("MiniMax-M3") != std::string::npos);
        CHECK(desc.find("deepseek-v4-flash") != std::string::npos);
        CHECK(desc.find("openai:") != std::string::npos);
        CHECK(desc.find("anthropic:") != std::string::npos);
    }
}
