#include "agent/agent.hpp"

#include <atomic>
#include <string>
#include <thread>
#include <vector>

#include "test_harness.hpp"

using namespace flagent;

namespace {

// A provider that checks the cancel flag in complete_stream, and can simulate
// a long-running operation so we can cancel mid-flight.
struct CancellableProvider : Provider {
    std::vector<Turn> script;
    size_t idx = 0;
    
    // For complete_stream: block until cancelled, then return cancelled error.
    std::atomic<bool>* block_until_cancelled = nullptr;
    
    std::expected<Turn, Error> complete(const Conversation&,
                                        const std::vector<ToolSpec>&) override {
        if (idx >= script.size())
            return std::unexpected(Error{.msg = "script exhausted", .code = 0});
        return script[idx++];
    }
    
    std::expected<Turn, Error> complete_stream(
        const Conversation&, const std::vector<ToolSpec>&,
        const StreamFn&, const std::atomic<bool>* cancel) override {
        if (!cancel) {
            // No cancel flag provided — just return from script
            if (idx >= script.size())
                return std::unexpected(Error{.msg = "script exhausted", .code = 0});
            return script[idx++];
        }
        // Busy-wait until either cancelled or block_until_cancelled is cleared
        while (block_until_cancelled && block_until_cancelled->load()) {
            if (cancel->load(std::memory_order_acquire))
                return std::unexpected(Error{.msg = "cancelled", .code = 0});
            std::this_thread::yield();
        }
        if (idx >= script.size())
            return std::unexpected(Error{.msg = "script exhausted", .code = 0});
        return script[idx++];
    }
};

}  // namespace

TEST("compact: second compact succeeds after first is cancelled") {
    CancellableProvider p;
    // Script for the summarise LLM call (during compact)
    p.script.push_back(Turn{.text = "summary text", .tool_calls = {}, .finish_reason = "stop", .usage = {}, .reasoning = {}});
    
    // Block signal: when true, complete_stream blocks until cancelled
    std::atomic<bool> block_signal{true};
    p.block_until_cancelled = &block_signal;
    
    ToolRegistry reg;  // empty — compact doesn't use tools
    AgentConfig cfg;
    cfg.system_prompt = "be helpful";
    cfg.compact_keep_tail_tokens = 1;  // tiny budget: keep only the last message
    Agent agent(p, reg, cfg);

    // Build enough history that compact() actually folds messages away (it
    // keeps a fixed recent tail verbatim, so a few-message conversation is a
    // no-op and would never reach the LLM call this test cancels).
    agent.set_history(Conversation{
        Message::system("be helpful"),
        Message::user("write a function"),
        Message::assistant("here's the code"),
        Message::user("add tests"),
        Message::assistant("here are the tests"),
        Message::user("now document it"),
        Message::assistant("documented"),
        Message::user("ship it"),
    });
    
    // Start compact in background
    std::expected<Conversation, Error> result1;
    std::thread t([&] {
        result1 = agent.compact();
    });
    
    // Let compact get into the blocked state (past the reset, into the LLM call)
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    
    // Cancel it during the LLM call
    agent.cancel();
    
    t.join();
    
    // First compact should have failed with cancellation
    CHECK(!result1.has_value());
    if (!result1) {
        CHECK(result1.error().msg.find("cancelled") != std::string::npos);
    }
    
    // cancel_ is now true from the cancel() call.  Verify that.
    CHECK(agent.cancel_flag().load());
    
    // Now unblock and reset for second compact
    block_signal.store(false);
    p.script.push_back(Turn{.text = "second summary text", .tool_calls = {}, .finish_reason = "stop", .usage = {}, .reasoning = {}});
    
    // Second compact should succeed despite cancel_ being true —
    // compact() resets cancel_ right before the LLM call.
    auto result2 = agent.compact();
    CHECK(result2.has_value());
    if (result2) {
        // System kept, summary folded in right after it, recent tail preserved.
        CHECK(result2->size() >= 3);
        CHECK(result2->at(0).role() == Role::System);
        CHECK(result2->at(1).role() == Role::User);
        CHECK(result2->at(1).content().find("Summary") != std::string::npos);
        CHECK_EQ(result2->back().content(), std::string("ship it"));
    }
    
    // cancel_ should be false after a successful compact
    CHECK(!agent.cancel_flag().load());
}

TEST("compact: persistent cancel flag does not poison subsequent compacts") {
    // Simulates the scenario where cancel_ is left true between compact calls.
    CancellableProvider p;
    p.script.push_back(Turn{.text = "summary 1", .tool_calls = {}, .finish_reason = "stop", .usage = {}, .reasoning = {}});
    p.script.push_back(Turn{.text = "summary 2", .tool_calls = {}, .finish_reason = "stop", .usage = {}, .reasoning = {}});
    p.script.push_back(Turn{.text = "summary 3", .tool_calls = {}, .finish_reason = "stop", .usage = {}, .reasoning = {}});
    
    std::atomic<bool> block_signal{false};
    p.block_until_cancelled = &block_signal;
    
    ToolRegistry reg;
    AgentConfig cfg;
    cfg.system_prompt = "be helpful";
    cfg.compact_keep_tail_tokens = 1;  // tiny budget: keep only the last message
    Agent agent(p, reg, cfg);

    agent.set_history(Conversation{
        Message::system("be helpful"),
        Message::user("task 1"),
        Message::assistant("result 1"),
        Message::user("task 2"),
        Message::assistant("result 2"),
        Message::user("task 3"),
        Message::assistant("result 3"),
        Message::user("task 4"),
    });

    // Manually set cancel_ to true to simulate a stale cancellation
    agent.cancel();
    CHECK(agent.cancel_flag().load());
    
    // First compact should succeed because compact() resets cancel_ before the LLM call
    auto r1 = agent.compact();
    CHECK(r1.has_value());
    CHECK(!agent.cancel_flag().load());
    
    // Install the compacted history
    agent.set_history(*r1);
    
    // Add more messages so the next compact has work to do
    agent.set_history(Conversation{
        Message::system("be helpful"),
        Message::user("Summary of earlier conversation:\n\nsummary 1"),
        Message::user("task 2"),
        Message::assistant("more work"),
        Message::user("task 3"),
        Message::assistant("yet more work"),
        Message::user("task 3b"),
        Message::assistant("still working"),
    });
    
    // Set cancel again
    agent.cancel();
    CHECK(agent.cancel_flag().load());
    
    // Second compact should also succeed
    auto r2 = agent.compact();
    CHECK(r2.has_value());
    
    // Set cancel again
    agent.cancel();
    CHECK(agent.cancel_flag().load());
    
    agent.set_history(*r2);
    // Need more messages for a third compact
    agent.set_history(Conversation{
        Message::system("be helpful"),
        Message::user("Summary of earlier conversation:\n\nsummary 2"),
        Message::user("task 3"),
        Message::assistant("even more"),
        Message::user("task 4"),
        Message::assistant("more still"),
        Message::user("task 5"),
        Message::assistant("final"),
    });
    
    // Third compact should also succeed
    auto r3 = agent.compact();
    CHECK(r3.has_value());
}
