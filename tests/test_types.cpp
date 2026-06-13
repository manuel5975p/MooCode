#include "agent/types.hpp"

#include <string>
#include <vector>

#include "test_harness.hpp"

using namespace flagent;

// The class migration itself is the regression net (every site compiling + the
// rest of the suite passing proves each construction used a legal field combo).
// These tests pin the factory contracts directly. NOTE: there is no death-test
// harness, so the wrong-role-access aborts (asserts, live in Debug/ASan) are not
// asserted here — only the happy-path accessors are exercised.

TEST("Message::system sets only content, role System") {
    Message m = Message::system("you are x");
    CHECK(m.role() == Role::System);
    CHECK_EQ(m.content(), std::string("you are x"));
}

TEST("Message::user sets content and optional parts, role User") {
    Message m = Message::user("hello");
    CHECK(m.role() == Role::User);
    CHECK_EQ(m.content(), std::string("hello"));
    CHECK(m.parts().empty());

    std::vector<ContentPart> parts;
    parts.push_back(ContentPart{.text = "caption", .image = std::nullopt});
    Message mm = Message::user("", parts);
    CHECK(mm.role() == Role::User);
    CHECK_EQ(mm.parts().size(), std::size_t{1});
    CHECK_EQ(mm.parts()[0].text, std::string("caption"));
}

TEST("Message::assistant sets content and tool_calls, role Assistant") {
    std::vector<ToolCall> calls;
    calls.push_back(ToolCall{.id = "c1", .name = "echo", .arguments_json = "{}"});
    Message m = Message::assistant("on it", std::move(calls));
    CHECK(m.role() == Role::Assistant);
    CHECK_EQ(m.content(), std::string("on it"));
    CHECK_EQ(m.tool_calls().size(), std::size_t{1});
    CHECK_EQ(m.tool_calls()[0].id, std::string("c1"));
}

TEST("Message::tool sets id, content, failure; role Tool") {
    Message ok = Message::tool("c1", "result");
    CHECK(ok.role() == Role::Tool);
    CHECK_EQ(ok.tool_call_id(), std::string("c1"));
    CHECK_EQ(ok.content(), std::string("result"));
    CHECK(!ok.tool_failed());

    Message bad = Message::tool("c2", "ERROR: boom", true);
    CHECK(bad.role() == Role::Tool);
    CHECK(bad.tool_failed());
    // Content beginning with "ERROR:" does NOT by itself imply failure: the bit
    // is carried as data, independent of the text.
    Message ok_err = Message::tool("c3", "ERROR: appears in output", false);
    CHECK(!ok_err.tool_failed());
}

TEST("Message::from_fields rebuilds an arbitrary persisted message") {
    std::vector<ToolCall> calls;
    calls.push_back(ToolCall{.id = "t1", .name = "n", .arguments_json = "{}"});
    Message m = Message::from_fields(Role::Assistant, "txt", std::move(calls),
                                     "", {}, false);
    CHECK(m.role() == Role::Assistant);
    CHECK_EQ(m.content(), std::string("txt"));
    CHECK_EQ(m.tool_calls().size(), std::size_t{1});
}

TEST("Message::content has a mutating overload for in-place rewrite") {
    Message m = Message::user("before");
    m.content() = "after";
    CHECK_EQ(m.content(), std::string("after"));
}

// role_str is total and allocation-free (P6 6a).
static_assert(noexcept(role_str(Role::User)),
              "role_str must be noexcept (pure switch, no allocation)");

TEST("role_str maps every role to its wire name") {
    CHECK_EQ(std::string(role_str(Role::System)), std::string("system"));
    CHECK_EQ(std::string(role_str(Role::User)), std::string("user"));
    CHECK_EQ(std::string(role_str(Role::Assistant)), std::string("assistant"));
    CHECK_EQ(std::string(role_str(Role::Tool)), std::string("tool"));
}
