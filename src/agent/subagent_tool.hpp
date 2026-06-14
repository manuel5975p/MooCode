#ifndef MOOCODE_SUBAGENT_TOOL_HPP
#define MOOCODE_SUBAGENT_TOOL_HPP

// The spawn_subagent tool: lets the parent agent offload a multi-step sub-task
// to a fresh Agent that runs inside a single tool invocation. The sub-agent
// gets all parent tools except spawn_subagent itself.

#include <atomic>
#include <cstddef>
#include <expected>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "agent/provider.hpp"
#include "agent/subagent_types.hpp"
#include "agent/tools.hpp"

namespace moocode {

// One advertised model and the provider it belongs to. Feeds the spawn_subagent
// `model` argument description (built at registration) and the unknown-model
// error message (built at spawn). Keeps subagent_tool free of profile types.
struct SubagentModelInfo {
    std::string model;     // e.g. "deepseek-v4-flash"
    std::string provider;  // wire-format label, e.g. "openai" / "anthropic"
};

struct SubagentConfig {
    // Returns the live provider (survives TUI /provider swaps at runtime).
    std::function<Provider&()> get_provider;

    // The parent's tool registry — the sub-agent gets all tools except this one.
    // Pointer because a reference member prevents default construction + rebind.
    const ToolRegistry* tools = nullptr;

    // Sandbox root for the sub-agent's {DIR} expansion and tool confinement.
    std::filesystem::path root = ".";

    // Per-turn tool-output byte cap for the sub-agent (copied into AgentConfig).
    std::size_t max_tool_output = 32 * 1024;

    // System prompt for sub-agents. If empty, a built-in prompt is used.
    std::string system_prompt;

    // Optional shared cancel flag (the parent's). When non-null, the sub-agent
    // polls it between turns and aborts when it turns true.
    const std::atomic<bool>* parent_cancel = nullptr;

    // Lazy accessors (read at spawn time, not snapshotted at registration).
    // Return the parent's current approval gate, or an empty function when none
    // is installed. The sub-agent installs it iff the result is non-null.
    std::function<std::function<bool(const ToolCall&)>()> get_approve;

    // Return the parent's current usage callback, or an empty function when
    // none is installed. Used only to log an advisory "sub-agent used N tokens"
    // line; the parent's conversation-occupancy gauge is NOT fed sub-agent
    // tokens (they aren't in the parent's context window).
    std::function<std::function<void(const Usage&)>()> get_on_usage;

    // Optional callback for progressive nested activity entries in the TUI.
    // Allocated (as shared_ptr) before tool registration; the TUI writes the
    // real callback into it before the first agent.run(). When null (CLI / --yes
    // / tests), the subagent tool skips the activity wiring.
    std::shared_ptr<SubagentActivityFn> on_activity;

    // Optional callback for sub-agent assistant text (prose between tool calls
    // or the final answer). Allocated as shared_ptr alongside on_activity; the
    // TUI writes the real callback into it before the first agent.run(). When
    // null, assistant text is not forwarded (no regression for CLI/headless).
    std::shared_ptr<SubagentTextFn> on_text;

    // Models the parent may request via the `model` argument, used to build the
    // argument description (at registration) and the unknown-model error (at
    // spawn). Read lazily. Null / empty => model selection is not advertised.
    std::function<std::vector<SubagentModelInfo>()> list_models;

    // Build a fresh, owned Provider for an already-validated model name, or an
    // Error. Null => model selection disabled. The returned provider is owned by
    // the tool only for the sub-agent's synchronous run.
    std::function<std::expected<std::unique_ptr<Provider>, Error>(
        const std::string& model)>
        make_provider_for_model;
};

// Build the spawn_subagent tool.
Tool spawn_subagent_tool(SubagentConfig cfg);

}  // namespace moocode

#endif  // MOOCODE_SUBAGENT_TOOL_HPP
