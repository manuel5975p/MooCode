#include "agent/agent.hpp"

#include <atomic>
#include <string>
#include <vector>

#include "test_harness.hpp"

using namespace flagent;

namespace {

// A provider that returns a short scripted summary for the one summarise call
// compact() makes, and records the body it was asked to summarise so tests can
// assert what was folded away vs. kept verbatim.
struct SummaryProvider : Provider {
    std::string summary = "SHORT SUMMARY";
    std::string captured_body;  // user prompt content sent to summarise
    int calls = 0;

    std::expected<Turn, Error> complete(const Conversation&,
                                        const std::vector<ToolSpec>&) override {
        return std::unexpected(Error{.msg = "unused", .code = 0});
    }

    std::expected<Turn, Error> complete_stream(
        const Conversation& conv, const std::vector<ToolSpec>&,
        const StreamFn&, const std::atomic<bool>*) override {
        ++calls;
        if (!conv.empty()) captured_body = conv.back().content();
        return Turn{.text = summary, .tool_calls = {}, .finish_reason = "stop",
                    .usage = {}, .reasoning = {}};
    }
};

Message sys(std::string c) {
    return Message::system(std::move(c));
}
Message usr(std::string c) {
    return Message::user(std::move(c));
}
Message asst(std::string c, std::vector<ToolCall> tcs = {}) {
    return Message::assistant(std::move(c), std::move(tcs));
}
Message tool(std::string id, std::string c) {
    return Message::tool(std::move(id), std::move(c));
}

// A realistic single-user-turn agentic session: one prompt, then a long loop of
// assistant+tool steps carrying the bulk of the tokens. `steps` pairs.
Conversation single_turn_loop(std::size_t steps, std::size_t bulk_chars) {
    Conversation c;
    c.push_back(sys("you are helpful"));
    c.push_back(usr("please refactor module X"));
    for (std::size_t i = 0; i < steps; ++i) {
        std::string id = "call_" + std::to_string(i);
        c.push_back(asst("step " + std::to_string(i),
                         {ToolCall{.id = id, .name = "read_file",
                                   .arguments_json = "{}"}}));
        c.push_back(tool(id, std::string(bulk_chars, 'x')));
    }
    c.push_back(asst("done refactoring"));
    return c;
}

}  // namespace

// The reported bug: a single user turn followed by a long tool loop must be
// compacted, not returned unchanged.
TEST("compact: single user turn with long tool loop shrinks") {
    SummaryProvider p;
    ToolRegistry reg;
    AgentConfig cfg;
    cfg.system_prompt = "you are helpful";
    cfg.compact_keep_tail_tokens = 500;  // ~2000 chars of recent tail
    Agent agent(p, reg, cfg);

    auto conv = single_turn_loop(/*steps=*/10, /*bulk_chars=*/2000);
    agent.set_history(conv);

    const int before = Agent::estimated_tokens(conv);
    auto out = agent.compact();
    CHECK(out.has_value());
    if (!out) return;

    CHECK_EQ(p.calls, 1);  // the summarise call actually happened
    const int after = Agent::estimated_tokens(*out);
    CHECK(after < before / 2);  // a real, large reduction

    // System preserved at the front, summary folded in right after it.
    CHECK(out->size() >= 2);
    CHECK(out->at(0).role() == Role::System);
    CHECK(out->at(1).role() == Role::User);
    CHECK(out->at(1).content().find("Summary") != std::string::npos);
    CHECK(out->at(1).content().find("SHORT SUMMARY") != std::string::npos);

    // The most recent message is kept verbatim.
    CHECK_EQ(out->back().content(), conv.back().content());
}

// The verbatim tail must never begin with an orphaned tool result, or the
// provider rejects the unpaired tool_use/tool_result.
TEST("compact: tail never starts with an orphan tool result") {
    SummaryProvider p;
    ToolRegistry reg;
    AgentConfig cfg;
    cfg.compact_keep_tail_tokens = 800;  // boundary likely splits a tool pair
    Agent agent(p, reg, cfg);

    auto conv = single_turn_loop(/*steps=*/12, /*bulk_chars=*/1500);
    agent.set_history(conv);

    auto out = agent.compact();
    CHECK(out.has_value());
    if (!out) return;

    // Walk the kept tail (everything after the leading system?+summary) and
    // verify no Tool message appears without a preceding tool_call in the kept
    // region.
    bool seen_pending_call = false;
    std::size_t start = (out->front().role() == Role::System) ? 2 : 1;
    for (std::size_t i = start; i < out->size(); ++i) {
        const auto& m = out->at(i);
        if (m.role() == Role::Assistant && !m.tool_calls().empty())
            seen_pending_call = true;
        if (m.role() == Role::Tool) {
            CHECK(seen_pending_call);  // its tool_call must be in the kept tail
        }
    }
}

// Multi-turn: messages after the last user turn must survive (the old design
// dropped them).
TEST("compact: recent messages after the last user turn are preserved") {
    SummaryProvider p;
    ToolRegistry reg;
    AgentConfig cfg;
    cfg.system_prompt = "sys";
    cfg.compact_keep_tail_tokens = 300;
    Agent agent(p, reg, cfg);

    Conversation conv;
    conv.push_back(sys("sys"));
    conv.push_back(usr("task 1"));
    for (int i = 0; i < 6; ++i) {
        std::string id = "a" + std::to_string(i);
        conv.push_back(asst("work", {ToolCall{.id = id, .name = "x",
                                               .arguments_json = "{}"}}));
        conv.push_back(tool(id, std::string(1500, 'y')));
    }
    conv.push_back(usr("task 2 — the current request"));
    conv.push_back(asst("answer to task 2"));
    agent.set_history(conv);

    auto out = agent.compact();
    CHECK(out.has_value());
    if (!out) return;

    // The final assistant answer must still be present verbatim.
    CHECK_EQ(out->back().content(), std::string("answer to task 2"));
}

// The kept verbatim tail honours the token budget (within one message of
// slack, since the most recent message is always kept).
TEST("compact: kept tail respects the token budget") {
    SummaryProvider p;
    ToolRegistry reg;
    AgentConfig cfg;
    cfg.system_prompt = "sys";
    cfg.compact_keep_tail_tokens = 500;  // budget = 2000 chars
    Agent agent(p, reg, cfg);

    // 30 uniform 400-char (~100-token) assistant messages after one user turn.
    Conversation conv;
    conv.push_back(sys("sys"));
    conv.push_back(usr("kick off"));
    for (int i = 0; i < 30; ++i)
        conv.push_back(asst(std::string(400, 'a' + (i % 26))));
    agent.set_history(conv);

    auto out = agent.compact();
    CHECK(out.has_value());
    if (!out) return;

    // Sum the verbatim tail (everything after system + summary).
    std::size_t start = (out->front().role() == Role::System) ? 2 : 1;
    Conversation tail(out->begin() + static_cast<long>(start), out->end());
    const int tail_tok = Agent::estimated_tokens(tail);
    CHECK(tail_tok >= 100);                 // at least the most recent message
    CHECK(tail_tok <= 500 + 100);           // budget + one-message slack
    CHECK(Agent::estimated_tokens(*out) < Agent::estimated_tokens(conv) / 2);
}

// Too short to bother: compaction is a no-op and makes no provider call.
TEST("compact: short conversation is a no-op") {
    SummaryProvider p;
    ToolRegistry reg;
    AgentConfig cfg;
    cfg.system_prompt = "sys";
    Agent agent(p, reg, cfg);

    Conversation conv{sys("sys"), usr("hello"), asst("hi")};
    agent.set_history(conv);

    auto out = agent.compact();
    CHECK(out.has_value());
    if (out) CHECK_EQ(out->size(), conv.size());
    CHECK_EQ(p.calls, 0);
}
