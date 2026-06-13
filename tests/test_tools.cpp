#include "agent/tools.hpp"

#include "test_harness.hpp"

using namespace flagent;

namespace {
Tool echo_tool(std::string name) {
    return Tool{.spec = ToolSpec{.name = name, .description = "echoes", .parameters = nlohmann::json::object()},
                .run = [](const nlohmann::json& a) -> std::expected<std::string, Error> {
                    if (!a.contains("msg"))
                        return std::unexpected(Error{.msg = "need msg", .code = 0});
                    return a["msg"].get<std::string>();
                }};
}
}  // namespace

TEST("registry: has() reflects registration") {
    ToolRegistry r;
    CHECK(!r.has("echo"));
    r.add(echo_tool("echo"));
    CHECK(r.has("echo"));
}

TEST("registry: specs() preserves insertion order") {
    ToolRegistry r;
    r.add(echo_tool("c"));
    r.add(echo_tool("a"));
    r.add(echo_tool("b"));
    auto s = r.specs();
    CHECK_EQ(s.size(), size_t{3});
    CHECK_EQ(s[0].name, std::string("c"));
    CHECK_EQ(s[1].name, std::string("a"));
    CHECK_EQ(s[2].name, std::string("b"));
}

TEST("registry: invoke dispatches to the named tool") {
    ToolRegistry r;
    r.add(echo_tool("echo"));
    auto out = r.invoke("echo", nlohmann::json{{"msg", "hi"}});
    CHECK(out.has_value());
    if (out) CHECK_EQ(*out, std::string("hi"));
}

TEST("registry: invoke unknown tool returns Error naming it") {
    ToolRegistry r;
    auto out = r.invoke("nope", nlohmann::json::object());
    CHECK(!out.has_value());
    if (!out) CHECK(out.error().msg.find("nope") != std::string::npos);
}

TEST("registry: invoke propagates the tool's own Error") {
    ToolRegistry r;
    r.add(echo_tool("echo"));
    auto out = r.invoke("echo", nlohmann::json::object());  // no "msg"
    CHECK(!out.has_value());
}

TEST("registry: duplicate name replaces (last wins)") {
    ToolRegistry r;
    r.add(echo_tool("x"));
    Tool replacement{.spec = ToolSpec{.name = "x", .description = "v2", .parameters = nlohmann::json::object()},
                     .run = [](const nlohmann::json&) -> std::expected<std::string, Error> {
                         return std::string("replaced");
                     }};
    r.add(replacement);
    CHECK_EQ(r.specs().size(), size_t{1});
    auto out = r.invoke("x", nlohmann::json::object());
    CHECK(out.has_value());
    if (out) CHECK_EQ(*out, std::string("replaced"));
}
