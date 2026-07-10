#include "agent/agent.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <vector>

#include "test_harness.hpp"

using namespace moocode;

namespace {

// Provider that replays a fixed script of Turns and records every conversation
// it was asked to complete.
struct FakeProvider : Provider {
    std::vector<Turn> script;
    std::vector<Conversation> seen;
    size_t idx = 0;

    std::expected<Turn, Error> complete(const Conversation& c,
                                        const std::vector<ToolSpec>&) override {
        seen.push_back(c);
        if (idx >= script.size())
            return std::unexpected(Error{.msg = "script exhausted", .code = 0});
        return script[idx++];
    }
};

// Provider that always fails.
struct FailingProvider : Provider {
    std::expected<Turn, Error> complete(const Conversation&,
                                        const std::vector<ToolSpec>&) override {
        return std::unexpected(Error{.msg = "network down", .code = 42});
    }
};

// Provider that streams a fixed answer in fragments through complete_stream,
// to verify the agent forwards deltas. complete() is the non-streamed fallback.
struct StreamingProvider : Provider {
    std::expected<Turn, Error> complete(const Conversation&,
                                        const std::vector<ToolSpec>&) override {
        return Turn{.text = "blocking", .tool_calls = {}, .finish_reason = "stop", .usage = {}, .reasoning = {}};
    }
    std::expected<Turn, Error> complete_stream(
        const Conversation&, const std::vector<ToolSpec>&,
        const StreamFn& on, const std::atomic<bool>*) override {
        if (on) {
            on("Hel", "");
            on("lo", "");
            on("", "pondering");
        }
        return Turn{.text = "Hello", .tool_calls = {}, .finish_reason = "stop", .usage = {}, .reasoning = {}};
    }
};

ToolCall call(std::string id, std::string name, std::string args) {
    return ToolCall{.id = std::move(id), .name = std::move(name), .arguments_json = std::move(args)};
}

Tool ok_tool(std::string name, std::string reply) {
    return Tool{.spec = ToolSpec{.name = name, .description = "", .parameters = nlohmann::json::object()},
                .run = [reply](const nlohmann::json&) -> std::expected<std::string, Error> {
                    return reply;
                }};
}

}  // namespace

TEST("run: returns text immediately when no tools requested") {
    FakeProvider p;
    p.script.push_back(Turn{.text = "the answer", .tool_calls = {}, .finish_reason = "stop", .usage = {}, .reasoning = {}});
    ToolRegistry reg;
    Agent agent(p, reg, AgentConfig{});
    auto r = agent.run("question?");
    CHECK(r.has_value());
    if (r) CHECK_EQ(*r, std::string("the answer"));
}

TEST("run: executes a tool then returns the follow-up text") {
    FakeProvider p;
    Turn t1;
    t1.tool_calls.push_back(call("c1", "echo", "{}"));
    p.script.push_back(t1);
    p.script.push_back(Turn{.text = "done after tool", .tool_calls = {}, .finish_reason = "stop", .usage = {}, .reasoning = {}});
    ToolRegistry reg;
    reg.add(ok_tool("echo", "tool-output"));
    Agent agent(p, reg, AgentConfig{});
    auto r = agent.run("do it");
    CHECK(r.has_value());
    if (r) CHECK_EQ(*r, std::string("done after tool"));
}

TEST("run: builds well-formed history (system,user,assistant,tool,assistant)") {
    FakeProvider p;
    Turn t1;
    t1.tool_calls.push_back(call("c1", "echo", "{}"));
    p.script.push_back(t1);
    p.script.push_back(Turn{.text = "final", .tool_calls = {}, .finish_reason = "stop", .usage = {}, .reasoning = {}});
    ToolRegistry reg;
    reg.add(ok_tool("echo", "out"));
    AgentConfig cfg;
    cfg.system_prompt = "be helpful";
    Agent agent(p, reg, cfg);
    auto r = agent.run("hello");
    CHECK(r.has_value());
    const auto& h = agent.history();
    CHECK_EQ(h.size(), size_t{5});
    if (h.size() == 5) {
        CHECK(h[0].role() == Role::System);
        CHECK(h[1].role() == Role::User);
        CHECK(h[2].role() == Role::Assistant);
        CHECK_EQ(h[2].tool_calls().size(), size_t{1});
        CHECK(h[3].role() == Role::Tool);
        CHECK_EQ(h[3].tool_call_id(), std::string("c1"));
        CHECK_EQ(h[3].content(), std::string("out"));
        CHECK(h[4].role() == Role::Assistant);
    }
}

TEST("run: no system message when system_prompt empty") {
    FakeProvider p;
    p.script.push_back(Turn{.text = "x", .tool_calls = {}, .finish_reason = "stop", .usage = {}, .reasoning = {}});
    ToolRegistry reg;
    Agent agent(p, reg, AgentConfig{});
    auto r = agent.run("hi");
    const auto& h = agent.history();
    CHECK(h.size() >= 1);
    if (!h.empty()) CHECK(h[0].role() == Role::User);
}

TEST("append_system_prompt: before the first turn it joins the prepended message") {
    FakeProvider p;
    p.script.push_back(Turn{.text = "ok", .tool_calls = {}, .finish_reason = "stop", .usage = {}, .reasoning = {}});
    ToolRegistry reg;
    AgentConfig cfg;
    cfg.system_prompt = "base";
    Agent agent(p, reg, cfg);

    agent.append_system_prompt("extra rule");
    CHECK_EQ(agent.system_prompt(), std::string("base\n\nextra rule"));

    CHECK(agent.run("hi").has_value());
    const auto& h = agent.history();
    CHECK(!h.empty());
    if (!h.empty()) {
        CHECK(h[0].role() == Role::System);
        CHECK_EQ(h[0].content(), std::string("base\n\nextra rule"));
    }
}

TEST("append_system_prompt: mid-session it updates the live system message") {
    FakeProvider p;
    p.script.push_back(Turn{.text = "a", .tool_calls = {}, .finish_reason = "stop", .usage = {}, .reasoning = {}});
    p.script.push_back(Turn{.text = "b", .tool_calls = {}, .finish_reason = "stop", .usage = {}, .reasoning = {}});
    ToolRegistry reg;
    AgentConfig cfg;
    cfg.system_prompt = "base";
    Agent agent(p, reg, cfg);

    CHECK(agent.run("one").has_value());
    agent.append_system_prompt("loaded skill body");
    CHECK(agent.run("two").has_value());

    const auto& h = agent.history();
    CHECK(!h.empty());
    if (!h.empty()) {
        CHECK(h[0].role() == Role::System);
        CHECK_EQ(h[0].content(), std::string("base\n\nloaded skill body"));
    }
    // The provider must have seen the augmented prompt on the second request.
    CHECK(p.seen.size() >= 2);
    if (p.seen.size() >= 2) {
        const auto& last = p.seen.back();
        CHECK(!last.empty());
        if (!last.empty()) {
            CHECK(last[0].role() == Role::System);
            CHECK(last[0].content().find("loaded skill body") != std::string::npos);
        }
    }
}

TEST("append_system_prompt: inserts a system message when none existed") {
    FakeProvider p;
    p.script.push_back(Turn{.text = "a", .tool_calls = {}, .finish_reason = "stop", .usage = {}, .reasoning = {}});
    p.script.push_back(Turn{.text = "b", .tool_calls = {}, .finish_reason = "stop", .usage = {}, .reasoning = {}});
    ToolRegistry reg;
    Agent agent(p, reg, AgentConfig{});  // empty system prompt

    CHECK(agent.run("one").has_value());
    CHECK(!agent.history().empty());
    if (!agent.history().empty())
        CHECK(agent.history()[0].role() == Role::User);  // no system yet

    agent.append_system_prompt("new system");
    CHECK(!agent.history().empty());
    if (!agent.history().empty()) {
        CHECK(agent.history()[0].role() == Role::System);
        CHECK_EQ(agent.history()[0].content(), std::string("new system"));
    }
    CHECK(agent.run("two").has_value());
    CHECK(p.seen.back()[0].role() == Role::System);
}

TEST("append_system_prompt: empty extra is a no-op") {
    FakeProvider p;
    ToolRegistry reg;
    AgentConfig cfg;
    cfg.system_prompt = "base";
    Agent agent(p, reg, cfg);
    agent.append_system_prompt("");
    CHECK_EQ(agent.system_prompt(), std::string("base"));
}

TEST("run: tool error becomes a tool message and the loop continues") {
    FakeProvider p;
    Turn t1;
    t1.tool_calls.push_back(call("c1", "boom", "{}"));
    p.script.push_back(t1);
    p.script.push_back(Turn{.text = "recovered", .tool_calls = {}, .finish_reason = "stop", .usage = {}, .reasoning = {}});
    ToolRegistry reg;
    reg.add(Tool{.spec = ToolSpec{.name = "boom", .description = "", .parameters = nlohmann::json::object()},
                 .run = [](const nlohmann::json&) -> std::expected<std::string, Error> {
                     return std::unexpected(Error{.msg = "kaboom", .code = 0});
                 }});
    Agent agent(p, reg, AgentConfig{});
    auto r = agent.run("go");
    CHECK(r.has_value());
    if (r) CHECK_EQ(*r, std::string("recovered"));
    // The tool result fed back must mention the error.
    const auto& h = agent.history();
    bool found = false;
    for (const auto& m : h)
        if (m.role() == Role::Tool && m.content().find("kaboom") != std::string::npos)
            found = true;
    CHECK(found);
}

TEST("run: a failing tool sets tool_failed on its Tool message") {
    FakeProvider p;
    Turn t1;
    t1.tool_calls.push_back(call("c1", "boom", "{}"));
    p.script.push_back(t1);
    p.script.push_back(Turn{.text = "done", .tool_calls = {}, .finish_reason = "stop", .usage = {}, .reasoning = {}});
    ToolRegistry reg;
    reg.add(Tool{.spec = ToolSpec{.name = "boom", .description = "", .parameters = nlohmann::json::object()},
                 .run = [](const nlohmann::json&) -> std::expected<std::string, Error> {
                     return std::unexpected(Error{.msg = "kaboom", .code = 0});
                 }});
    Agent agent(p, reg, AgentConfig{});
    auto r = agent.run("go");
    CHECK(r.has_value());
    const auto& h = agent.history();
    bool checked = false;
    for (const auto& m : h)
        if (m.role() == Role::Tool) { CHECK(m.tool_failed()); checked = true; }
    CHECK(checked);
}

// The exact bug item 1 fixes: a tool that SUCCEEDS but whose output happens to
// begin with "ERROR: " must NOT be flagged as failed (failure is now data, not
// re-derived from the result prefix).
TEST("run: a tool succeeding with 'ERROR:' text is NOT tool_failed") {
    FakeProvider p;
    Turn t1;
    t1.tool_calls.push_back(call("c1", "echo", "{}"));
    p.script.push_back(t1);
    p.script.push_back(Turn{.text = "ok", .tool_calls = {}, .finish_reason = "stop", .usage = {}, .reasoning = {}});
    ToolRegistry reg;
    reg.add(ok_tool("echo", "ERROR: this is normal output"));
    Agent agent(p, reg, AgentConfig{});
    auto r = agent.run("go");
    CHECK(r.has_value());
    const auto& h = agent.history();
    bool checked = false;
    for (const auto& m : h)
        if (m.role() == Role::Tool) {
            CHECK(!m.tool_failed());                                 // success
            CHECK(m.content().find("ERROR:") != std::string::npos); // content kept
            checked = true;
        }
    CHECK(checked);
}

TEST("run: a plain successful tool is not tool_failed") {
    FakeProvider p;
    Turn t1;
    t1.tool_calls.push_back(call("c1", "echo", "{}"));
    p.script.push_back(t1);
    p.script.push_back(Turn{.text = "ok", .tool_calls = {}, .finish_reason = "stop", .usage = {}, .reasoning = {}});
    ToolRegistry reg;
    reg.add(ok_tool("echo", "all good"));
    Agent agent(p, reg, AgentConfig{});
    auto r = agent.run("go");
    CHECK(r.has_value());
    for (const auto& m : agent.history())
        if (m.role() == Role::Tool) CHECK(!m.tool_failed());
}

TEST("run: unknown tool reported back to the model, loop continues") {
    FakeProvider p;
    Turn t1;
    t1.tool_calls.push_back(call("c1", "ghost", "{}"));
    p.script.push_back(t1);
    p.script.push_back(Turn{.text = "ok", .tool_calls = {}, .finish_reason = "stop", .usage = {}, .reasoning = {}});
    ToolRegistry reg;  // empty
    Agent agent(p, reg, AgentConfig{});
    auto r = agent.run("go");
    CHECK(r.has_value());
    const auto& h = agent.history();
    bool found = false;
    for (const auto& m : h)
        if (m.role() == Role::Tool && m.content().find("ghost") != std::string::npos)
            found = true;
    CHECK(found);
}

TEST("run: malformed tool arguments reported, loop continues") {
    FakeProvider p;
    Turn t1;
    t1.tool_calls.push_back(call("c1", "echo", "{not json"));
    p.script.push_back(t1);
    p.script.push_back(Turn{.text = "ok", .tool_calls = {}, .finish_reason = "stop", .usage = {}, .reasoning = {}});
    ToolRegistry reg;
    reg.add(ok_tool("echo", "should-not-run"));
    Agent agent(p, reg, AgentConfig{});
    auto r = agent.run("go");
    CHECK(r.has_value());
    const auto& h = agent.history();
    bool tool_msg = false;
    for (const auto& m : h)
        if (m.role() == Role::Tool) tool_msg = true;
    CHECK(tool_msg);  // a tool result (the parse error) was fed back
}

TEST("run: max_iterations is enforced") {
    FakeProvider p;
    // Always request a tool, never finishing.
    for (int i = 0; i < 100; ++i) {
        Turn t;
        t.tool_calls.push_back(call("c", "echo", "{}"));
        p.script.push_back(t);
    }
    ToolRegistry reg;
    reg.add(ok_tool("echo", "again"));
    AgentConfig cfg;
    cfg.max_iterations = 3;
    Agent agent(p, reg, cfg);
    auto r = agent.run("loop forever");
    CHECK(!r.has_value());
    if (!r) CHECK(r.error().msg.find("iteration") != std::string::npos);
    // Provider consulted at most max_iterations times.
    CHECK(p.seen.size() <= 3);
}

TEST("run: provider error propagates") {
    FailingProvider p;
    ToolRegistry reg;
    Agent agent(p, reg, AgentConfig{});
    auto r = agent.run("hi");
    CHECK(!r.has_value());
    if (!r) {
        CHECK_EQ(r.error().msg, std::string("network down"));
        CHECK_EQ(r.error().code, 42);
    }
}

TEST("run: oversized tool output is truncated") {
    FakeProvider p;
    Turn t1;
    t1.tool_calls.push_back(call("c1", "big", "{}"));
    p.script.push_back(t1);
    p.script.push_back(Turn{.text = "done", .tool_calls = {}, .finish_reason = "stop", .usage = {}, .reasoning = {}});
    ToolRegistry reg;
    reg.add(ok_tool("big", std::string(10000, 'z')));
    AgentConfig cfg;
    cfg.max_tool_output = 100;
    Agent agent(p, reg, cfg);
    auto r = agent.run("go");
    CHECK(r.has_value());
    const auto& h = agent.history();
    for (const auto& m : h)
        if (m.role() == Role::Tool) {
            CHECK(m.content().size() < 10000);
            CHECK(m.content().find("truncat") != std::string::npos);
        }
}

TEST("run: progress callback fires for assistant and tool messages") {
    FakeProvider p;
    Turn t1;
    t1.tool_calls.push_back(call("c1", "echo", "{}"));
    p.script.push_back(t1);
    p.script.push_back(Turn{.text = "done", .tool_calls = {}, .finish_reason = "stop", .usage = {}, .reasoning = {}});
    ToolRegistry reg;
    reg.add(ok_tool("echo", "out"));
    Agent agent(p, reg, AgentConfig{});
    int assistant = 0, tool = 0;
    agent.on_message([&](const Message& m) {
        if (m.role() == Role::Assistant) ++assistant;
        if (m.role() == Role::Tool) ++tool;
    });
    auto r = agent.run("go");
    CHECK(r.has_value());
    CHECK(assistant >= 2);
    CHECK(tool >= 1);
}

TEST("run: on_delta receives streamed answer and reasoning fragments") {
    StreamingProvider p;
    ToolRegistry reg;
    Agent agent(p, reg, AgentConfig{});
    std::string answer, reasoning;
    agent.on_delta([&](std::string_view a, std::string_view r) {
        answer.append(a);
        reasoning.append(r);
    });
    auto res = agent.run("hi");
    CHECK(res.has_value());
    if (res) CHECK_EQ(*res, std::string("Hello"));
    CHECK_EQ(answer, std::string("Hello"));
    CHECK_EQ(reasoning, std::string("pondering"));
}

TEST("run: multiple tool calls in one turn all execute in order") {
    FakeProvider p;
    Turn t1;
    t1.tool_calls.push_back(call("c1", "a", "{}"));
    t1.tool_calls.push_back(call("c2", "b", "{}"));
    p.script.push_back(t1);
    p.script.push_back(Turn{.text = "done", .tool_calls = {}, .finish_reason = "stop", .usage = {}, .reasoning = {}});
    ToolRegistry reg;
    reg.add(ok_tool("a", "ra"));
    reg.add(ok_tool("b", "rb"));
    Agent agent(p, reg, AgentConfig{});
    auto r = agent.run("go");
    CHECK(r.has_value());
    const auto& h = agent.history();
    std::vector<std::string> tool_ids;
    for (const auto& m : h)
        if (m.role() == Role::Tool) tool_ids.push_back(m.tool_call_id());
    CHECK_EQ(tool_ids.size(), size_t{2});
    if (tool_ids.size() == 2) {
        CHECK_EQ(tool_ids[0], std::string("c1"));
        CHECK_EQ(tool_ids[1], std::string("c2"));
    }
}

TEST("run: on_approve decline skips the tool and feeds back an error") {
    FakeProvider p;
    Turn t1;
    t1.tool_calls.push_back(call("c1", "echo", "{}"));
    p.script.push_back(t1);
    p.script.push_back(Turn{.text = "after", .tool_calls = {}, .finish_reason = "stop", .usage = {}, .reasoning = {}});
    ToolRegistry reg;
    bool ran = false;
    reg.add(Tool{.spec = ToolSpec{.name = "echo", .description = "", .parameters = nlohmann::json::object()},
                 .run = [&](const nlohmann::json&) -> std::expected<std::string, Error> {
                     ran = true;
                     return "out";
                 }});
    Agent agent(p, reg, AgentConfig{});
    agent.on_approve([](const ToolCall&) { return false; });  // deny everything
    auto r = agent.run("go");
    CHECK(r.has_value());
    CHECK(!ran);  // tool body never executed
    const auto& h = agent.history();
    bool declined = false;
    for (const auto& m : h)
        if (m.role() == Role::Tool && m.content().find("declined") != std::string::npos)
            declined = true;
    CHECK(declined);
}

TEST("run: on_approve allow runs the tool normally") {
    FakeProvider p;
    Turn t1;
    t1.tool_calls.push_back(call("c1", "echo", "{}"));
    p.script.push_back(t1);
    p.script.push_back(Turn{.text = "done", .tool_calls = {}, .finish_reason = "stop", .usage = {}, .reasoning = {}});
    ToolRegistry reg;
    reg.add(ok_tool("echo", "out"));
    Agent agent(p, reg, AgentConfig{});
    agent.on_approve([](const ToolCall&) { return true; });
    auto r = agent.run("go");
    CHECK(r.has_value());
    if (r) CHECK_EQ(*r, std::string("done"));
}

TEST("set_history then run appends without a duplicate system prompt") {
    FakeProvider p;
    p.script.push_back(Turn{.text = "continued", .tool_calls = {}, .finish_reason = "stop", .usage = {}, .reasoning = {}});
    ToolRegistry reg;
    AgentConfig cfg;
    cfg.system_prompt = "be helpful";
    Agent agent(p, reg, cfg);

    // A loaded conversation already carries its own system + prior turns.
    Conversation loaded;
    loaded.push_back(Message::system("original system"));
    loaded.push_back(Message::user("earlier"));
    loaded.push_back(Message::assistant("earlier answer"));
    agent.set_history(loaded);

    auto r = agent.run("next question");
    CHECK(r.has_value());
    const auto& h = agent.history();
    // No new system prompt prepended (history was non-empty); the new user turn
    // and assistant answer are appended after the loaded ones.
    int system_count = 0;
    for (const auto& m : h)
        if (m.role() == Role::System) ++system_count;
    CHECK_EQ(system_count, 1);
    CHECK(h[0].role() == Role::System);
    CHECK_EQ(h[0].content(), std::string("original system"));
    CHECK(h[3].role() == Role::User);
    CHECK_EQ(h[3].content(), std::string("next question"));
    CHECK(h[4].role() == Role::Assistant);
}

TEST("set_history with empty conversation re-prepends the system prompt") {
    FakeProvider p;
    p.script.push_back(Turn{.text = "x", .tool_calls = {}, .finish_reason = "stop", .usage = {}, .reasoning = {}});
    ToolRegistry reg;
    AgentConfig cfg;
    cfg.system_prompt = "sys";
    Agent agent(p, reg, cfg);
    agent.set_history(Conversation{});  // empty
    auto r = agent.run("hi");
    CHECK(r.has_value());
    const auto& h = agent.history();
    CHECK(!h.empty());
    if (!h.empty()) CHECK(h[0].role() == Role::System);
}

TEST("run: on_usage fires with the turn's reported usage") {
    FakeProvider p;
    Turn t{"hi", {}, "stop", {}, {}};
    t.usage = Usage{.prompt_tokens = 10, .completion_tokens = 4, .total_tokens = 14, .present = true};
    p.script.push_back(t);
    ToolRegistry reg;
    Agent agent(p, reg, AgentConfig{});
    int total = -1;
    agent.on_usage([&](const Usage& u) { total = u.total_tokens; });
    auto r = agent.run("go");
    CHECK(r.has_value());
    CHECK_EQ(total, 14);
}

// The inject hook is polled before every provider request, so a message
// buffered mid-turn rides the *next* request rather than waiting for the turn
// to end. Returning the message on the second poll (i.e. after the first tool
// round) proves it lands in the follow-up request, not the first.
TEST("run: inject hook flushes buffered messages into the next request") {
    FakeProvider p;
    Turn t1;
    t1.tool_calls.push_back(call("c1", "echo", "{}"));
    p.script.push_back(t1);
    p.script.push_back(Turn{.text = "done", .tool_calls = {}, .finish_reason = "stop", .usage = {}, .reasoning = {}});
    ToolRegistry reg;
    reg.add(ok_tool("echo", "ok"));
    Agent agent(p, reg, AgentConfig{});
    int polls = 0;
    agent.on_inject([&]() -> std::vector<Message> {
        if (++polls == 2) return {Message::user("BUFFERED")};
        return {};
    });
    auto r = agent.run("start");
    CHECK(r.has_value());
    // Two requests were made; the first must NOT carry the buffered message, the
    // second must, as its final user turn.
    CHECK(p.seen.size() == size_t{2});
    if (p.seen.size() == 2) {
        bool in_first = false;
        for (const auto& m : p.seen[0])
            if (m.content() == "BUFFERED") in_first = true;
        CHECK(!in_first);
        const auto& second = p.seen[1];
        CHECK(!second.empty());
        if (!second.empty()) {
            CHECK(second.back().role() == Role::User);
            CHECK_EQ(second.back().content(), std::string("BUFFERED"));
        }
    }
}

// Several tool calls in one turn run on their own threads in parallel. Each tool
// blocks until all three have entered, with a timeout: under real concurrency
// every call observes the full count and returns ":together"; were they run
// serially the first would wait alone, time out, and return ":alone". Results
// are appended in request order regardless.
TEST("run: tool calls in one turn run concurrently, results stay ordered") {
    FakeProvider p;
    Turn t1;
    t1.tool_calls.push_back(call("c1", "t1", "{}"));
    t1.tool_calls.push_back(call("c2", "t2", "{}"));
    t1.tool_calls.push_back(call("c3", "t3", "{}"));
    p.script.push_back(t1);
    p.script.push_back(Turn{.text = "done", .tool_calls = {}, .finish_reason = "stop", .usage = {}, .reasoning = {}});

    std::mutex m;
    std::condition_variable cv;
    int started = 0;
    auto barrier_tool = [&](std::string name) {
        return Tool{
            .spec = ToolSpec{.name = name, .description = "",
                             .parameters = nlohmann::json::object()},
            .run = [&, name](const nlohmann::json&) -> std::expected<std::string, Error> {
                std::unique_lock lk(m);
                ++started;
                cv.notify_all();
                bool all = cv.wait_for(lk, std::chrono::seconds(3),
                                       [&] { return started == 3; });
                return name + (all ? ":together" : ":alone");
            }};
    };
    ToolRegistry reg;
    reg.add(barrier_tool("t1"));
    reg.add(barrier_tool("t2"));
    reg.add(barrier_tool("t3"));
    Agent agent(p, reg, AgentConfig{});
    auto r = agent.run("go");
    CHECK(r.has_value());
    const auto& h = agent.history();
    // no system prompt => user, assistant(3 calls), tool c1, c2, c3, assistant.
    CHECK_EQ(h.size(), size_t{6});
    if (h.size() == 6) {
        CHECK_EQ(h[1].tool_calls().size(), size_t{3});
        CHECK_EQ(h[2].tool_call_id(), std::string("c1"));
        CHECK_EQ(h[2].content(), std::string("t1:together"));
        CHECK_EQ(h[3].tool_call_id(), std::string("c2"));
        CHECK_EQ(h[3].content(), std::string("t2:together"));
        CHECK_EQ(h[4].tool_call_id(), std::string("c3"));
        CHECK_EQ(h[4].content(), std::string("t3:together"));
    }
}
