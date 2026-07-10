#ifndef MOOCODE_AGENT_HPP
#define MOOCODE_AGENT_HPP

// The loop that turns a Provider + ToolRegistry into an agent: send the
// conversation, run any requested tools, feed results back, repeat until the
// model answers without requesting tools (or a safety backstop trips).

// Header guard for the agent module: declares the agentic loop interface.

#include <atomic>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <string>

#include "agent/provider.hpp"
#include "agent/tools.hpp"
#include "agent/types.hpp"

namespace moocode {

struct AgentConfig {
    std::optional<std::uint32_t> max_iterations;  // nullopt => unlimited; else runaway backstop
    std::size_t max_tool_output = 32 * 1024;  // per-result cap (truncates, notes)
    std::string system_prompt;                // prepended if non-empty
    // When false (--no-tools), the tool JSON schema is omitted from every
    // request — the model is told nothing about tools and cannot call them
    // (chat-only). The registry stays populated; only its advertisement stops.
    bool advertise_tools = true;
    // compact() keeps the most recent messages whose cumulative estimated token
    // count fits this budget verbatim; everything older is folded into one
    // summary. The single most recent message is always kept even if it alone
    // exceeds the budget. Estimate mirrors estimated_tokens (≈ chars/4).
    std::size_t compact_keep_tail_tokens = 4096;
};

// Rough token estimate: sum of character lengths / 4, same heuristic the TUI
// status bar and compact() use. Uses std::size_t internally to avoid overflow
// on extremely large conversations. pre: n >= 0.
int estimated_tokens(const Conversation& conv);

class Agent {
public:
    // Borrowed references must outlive the Agent.
    Agent(Provider& provider, ToolRegistry& tools, AgentConfig config);

    // Called for each message appended to history (assistant turns and tool
    // results), so a CLI can stream progress. Optional.
    using ProgressFn = std::function<void(const Message&)>;
    void on_message(ProgressFn fn);

    // Called with (answer, reasoning) fragments as each assistant turn streams
    // in, for live output. Optional; when unset the loop still streams but
    // discards the fragments.
    void on_delta(StreamFn fn);

    // Called before each tool call; return false to skip it (the loop feeds an
    // "ERROR: user declined…" result back so the model can react). Optional;
    // unset => every tool runs.
    using ApprovalFn = std::function<bool(const ToolCall&)>;
    void on_approve(ApprovalFn fn);

    // Called after each turn that reports token usage, for a live gauge.
    // Optional.
    using UsageFn = std::function<void(const Usage&)>;
    void on_usage(UsageFn fn);

    // Called right before every provider request (including each mid-turn
    // tool-use round-trip) to pull any user messages the caller buffered while
    // the turn was in flight. The returned messages are appended to the
    // conversation so they ride the very next API call rather than waiting for
    // the turn to finish — the "flush the whole buffer at the next opportunity"
    // semantics the TUI wants. Returns empty when nothing is buffered. Runs on
    // the same thread as run(). Optional; installed only on the interactive
    // (top-level) agent, never on sub-agents.
    using InjectFn = std::function<std::vector<Message>()>;
    void on_inject(InjectFn fn);

    // Cancel plumbing: expose the internal flag so a sub-agent can poll the
    // parent's cancellation (one Esc aborts both). Thread-safe.
    const std::atomic<bool>& cancel_flag() const { return cancel_; }

    // Install an external cancel source that will be polled alongside the
    // internal flag in run(). The sub-agent tool uses this to let a parent Esc
    // interrupt the sub-agent's loop and in-flight HTTP request.
    void watch_cancel(const std::atomic<bool>& src) { watch_cancel_ = &src; }

    // Lazy accessors used by spawn_subagent at tool-invocation time (the
    // callbacks may be installed after the sub-agent tool is registered).
    const ApprovalFn& approve_callback() const { return approve_; }
    const UsageFn& usage_callback() const { return on_usage_; }

    // Finalise the system prompt after construction (required for the
    // init-order decoupling that lets spawn_subagent capture &agent).
    void set_system_prompt(std::string s) { config_.system_prompt = std::move(s); }

    const std::string& system_prompt() const { return config_.system_prompt; }

    // Append `extra` to the system prompt for the rest of the session. Updates
    // the future-turn source (config_.system_prompt) and, when run() has already
    // materialized the prompt as the conversation's leading system message, that
    // message too — so the augmentation reaches the very next request. A blank
    // line separates it from existing content. A SystemPrompt-effect skill
    // routes its body here. No-op on empty `extra`. Note: mutating the system
    // prompt mid-session invalidates any provider-side prompt cache once.
    // pre: no run() in progress (mutates the conversation).
    void append_system_prompt(std::string extra);

    // Cancel the current run() mid-flight. The in-flight HTTP request is aborted
    // and run() returns an Error. No-op when no run is in progress. Thread-safe.
    void cancel();

    // Run the loop to completion. Returns the model's final text, or an Error on
    // provider failure / max_iterations exhaustion. pre: user_prompt nonempty.
    // When `user_parts` is non-empty the user message is built as multimodal
    // (text + images) rather than a plain string.
    std::expected<std::string, Error> run(std::string user_prompt,
                                          std::vector<ContentPart> user_parts = {});

    // Conversation accumulated so far (for inspection and tests).
    const Conversation& history() const { return conv_; }

    // The active provider, for live generation-control changes (the TUI
    // /effort, /temp and /thinking commands call set_params on it). Must only be
    // mutated while no run() is in progress.
    Provider& provider() { return *provider_; }

    // Replace the active provider (TUI /provider or /model switching wire
    // format). Takes ownership; the borrowed startup provider is simply no longer
    // referenced. Carry generation params over from the old provider before
    // calling if they must survive the swap. pre: no run() in progress, p
    // non-null. post: provider() returns *p.
    void set_provider(std::unique_ptr<Provider> p) {
        owned_ = std::move(p);
        provider_ = owned_.get();
    }

    // Replace the conversation wholesale, for /resume, /continue and /rewind.
    // The next run() appends to this history; since the system prompt is only
    // prepended when the conversation is empty, a loaded non-empty conversation
    // keeps its original system prompt. Fires no callbacks (the caller rebuilds
    // any UI state explicitly).
    void set_history(Conversation conv) { conv_ = std::move(conv); }

    // Compress the conversation into a short summary, replacing history with
    // [system?, user("Summary of earlier conversation: …"), recent_tail…].
    // The most recent messages that fit config.compact_keep_tail_tokens are
    // preserved verbatim so the live request and latest tool exchange survive;
    // everything older is folded into the summary (the summarise prompt keeps
    // the original task). The kept tail never begins with an orphaned tool
    // result, so tool_use/tool_result pairing stays valid. Streams the summary
    // via on_delta so the TUI can show progress. The cancel() flag is honored.
    // Returns the new history on success, or an Error (in which case the
    // history is unchanged). No-op (returns the unchanged history) when the
    // whole conversation already fits the tail budget — nothing worth folding.
    // `instructions`, when non-empty, is appended to the summarise prompt so
    // the user can guide what the summary should focus on (e.g. "/compact
    // keep the bug-fix context").
    // pre: no run() in progress. post: history() reflects the compaction.
    std::expected<Conversation, Error> compact(
        std::string_view instructions = {});

private:
    Provider* provider_;                  // active provider (borrowed or owned_)
    std::unique_ptr<Provider> owned_;     // set by set_provider; null while borrowing
    ToolRegistry& tools_;
    AgentConfig config_;
    Conversation conv_;
    ProgressFn on_message_;
    StreamFn on_delta_;
    ApprovalFn approve_;
    UsageFn on_usage_;
    InjectFn inject_;
    std::atomic<bool> cancel_{false};
    const std::atomic<bool>* watch_cancel_ = nullptr;

    void append(Message m);
};

}  // namespace moocode

#endif  // MOOCODE_AGENT_HPP
