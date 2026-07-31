// Tests for folding a saved (tool-using) conversation into one the chat-only
// GUI can send. No Qt, no display — the rules live in a Qt-free unit precisely
// so they can be checked here.

#include "gui/conversation_import.hpp"

#include <string>

#include "agent/types.hpp"

#include "test_harness.hpp"

using namespace moocode;
using namespace moocode::gui;

namespace {

bool contains(const std::string& hay, const std::string& needle) {
    return hay.find(needle) != std::string::npos;
}

bool has_tool_traffic(const Conversation& c) {
    for (const Message& m : c) {
        if (m.role() == Role::Tool) return true;
        if (m.role() == Role::Assistant && !m.tool_calls().empty()) return true;
    }
    return false;
}

}  // namespace

TEST("a chat-only conversation passes through unchanged and unflagged") {
    Conversation in = {Message::system("be terse"), Message::user("hi"),
                       Message::assistant("hello", {}, "thought")};
    bool folded = true;
    const Conversation out = to_chat_only(in, folded);

    CHECK(folded == false);  // the caller may keep saving over the same file
    CHECK_EQ(out.size(), in.size());
    CHECK(out[0].role() == Role::System);
    CHECK_EQ(out[1].content(), std::string("hi"));
    CHECK_EQ(out[2].content(), std::string("hello"));
    CHECK_EQ(out[2].reasoning(), std::string("thought"));
}

TEST("tool calls and results fold into the assistant turn that caused them") {
    Conversation in = {
        Message::user("fix the parser"),
        Message::assistant("Reading it.",
                           {ToolCall{.id = "c1",
                                     .name = "read_file",
                                     .arguments_json = "{\"path\":\"a.cpp\"}"}},
                           "thinking"),
        Message::tool("c1", "line one\nline two"),
        Message::assistant("Fixed."),
    };
    bool folded = false;
    const Conversation out = to_chat_only(in, folded);

    CHECK(folded);
    CHECK(has_tool_traffic(out) == false);  // nothing left to be rejected
    CHECK_EQ(out.size(), std::size_t(3));
    CHECK(out[1].role() == Role::Assistant);
    CHECK(contains(out[1].content(), "Reading it."));
    CHECK(contains(out[1].content(), "read_file"));
    CHECK(contains(out[1].content(), "a.cpp"));
    CHECK(contains(out[1].content(), "line one\nline two"));
    CHECK_EQ(out[1].reasoning(), std::string("thinking"));
    CHECK_EQ(out[2].content(), std::string("Fixed."));
}

TEST("a bare tool-call turn keeps the call and drops nothing else") {
    Conversation in = {Message::user("ls"),
                       Message::assistant("", {ToolCall{.id = "c1",
                                                        .name = "list_dir",
                                                        .arguments_json = "{}"}}),
                       Message::tool("c1", "a.cpp")};
    bool folded = false;
    const Conversation out = to_chat_only(in, folded);

    CHECK(folded);
    CHECK_EQ(out.size(), std::size_t(2));  // user + one merged assistant turn
    CHECK(contains(out[1].content(), "list_dir"));
    CHECK(contains(out[1].content(), "a.cpp"));
}

TEST("an assistant turn that said and called nothing is dropped") {
    Conversation in = {Message::user("hi"), Message::assistant(""),
                       Message::assistant("hello")};
    bool folded = false;
    const Conversation out = to_chat_only(in, folded);

    CHECK(folded == false);  // nothing was rewritten, only an empty turn skipped
    CHECK_EQ(out.size(), std::size_t(2));
    CHECK_EQ(out[1].content(), std::string("hello"));
}

TEST("an orphaned tool result still lands somewhere") {
    // A file rewound between the call and its result, or truncated at the
    // front: the result must not vanish, and must not stay a Tool message.
    Conversation in = {Message::tool("c1", "orphan output"),
                       Message::user("what happened?")};
    bool folded = false;
    const Conversation out = to_chat_only(in, folded);

    CHECK(folded);
    CHECK_EQ(out.size(), std::size_t(2));
    CHECK(out[0].role() == Role::Assistant);
    CHECK(contains(out[0].content(), "orphan output"));
}

TEST("a long tool result is clipped, and says how much was cut") {
    const std::string big(kToolResultClip + 500, 'x');
    Conversation in = {Message::user("read it"),
                       Message::assistant("ok", {ToolCall{.id = "c1",
                                                          .name = "read_file",
                                                          .arguments_json = "{}"}}),
                       Message::tool("c1", big)};
    bool folded = false;
    const Conversation out = to_chat_only(in, folded);

    CHECK(folded);
    CHECK(out[1].content().size() < big.size());
    CHECK(contains(out[1].content(), "500 more characters"));
}

TEST("tool arguments are folded onto one line") {
    Conversation in = {
        Message::user("edit"),
        Message::assistant(
            "",
            {ToolCall{.id = "c1",
                      .name = "edit_file",
                      .arguments_json = "{\n  \"path\": \"a.cpp\",\n"
                                        "  \"old\": \"one\"\n}"}})};
    bool folded = false;
    const Conversation out = to_chat_only(in, folded);

    CHECK(folded);
    // The rendered call is a single inline-code span, so a newline in the raw
    // JSON would break the Markdown rather than the line.
    CHECK(out[1].content().find('\n') == std::string::npos);
}
