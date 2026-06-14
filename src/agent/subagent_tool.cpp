#include "agent/subagent_tool.hpp"

#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "agent/agent.hpp"
#include "agent/provider.hpp"
#include "agent/strutil.hpp"
#include "agent/tools.hpp"

namespace moocode {

namespace {

constexpr const char* kDefaultSubagentPrompt =
    "You are a sub-agent spawned by another coding agent. Complete the given "
    "task and return a single final answer — do not leave work for the "
    "parent. Your conversation will be discarded; only your final text is "
    "returned. Use tools when they help, but value a precise answer. Be "
    "concise.\n"
    "\n"
    "Available tools:\n"
    "{TOOLS}\n"
    "\n"
    "Working directory: {DIR}";

// Render advertised models as "  <provider>: a, b, c" lines, one per provider.
// Providers are alphabetical (std::map order); models keep list order; exact
// duplicate model names are dropped (first occurrence wins). Deterministic.
std::string format_model_list(const std::vector<SubagentModelInfo>& models) {
    std::map<std::string, std::string> by_provider;
    std::set<std::string> seen;
    for (const auto& mi : models) {
        if (!seen.insert(mi.model).second) continue;
        std::string& line = by_provider[mi.provider];
        if (!line.empty()) line += ", ";
        line += mi.model;
    }
    std::string out;
    for (const auto& [provider, csv] : by_provider)
        out += "  " + provider + ": " + csv + "\n";
    if (!out.empty()) out.pop_back();
    return out;
}

// Build the sub-agent's tool registry: all parent tools except spawn_subagent
// and ask_user (sub-agents run in a tool invocation and cannot ask the user).
ToolRegistry build_sub_registry(const ToolRegistry& parent) {
    ToolRegistry sub;
    for (const auto& t : parent.tools())
        if (t.spec.name != "spawn_subagent" && t.spec.name != "ask_user")
            sub.add(t);
    return sub;
}

}  // namespace

Tool spawn_subagent_tool(SubagentConfig cfg) {
    nlohmann::json params = nlohmann::json::parse(R"({
        "type":"object",
        "properties":{
          "prompt":{
            "type":"string",
            "description":"Task for sub-agent. Be specific about what it should produce and any constraints."}},
        "required":["prompt"]})");

    // Advertise the selectable models in the `model` argument description so the
    // parent agent knows its options (and that some cross providers). Built once
    // at registration; the current model is intentionally not named (it would go
    // stale after a runtime /model swap).
    if (cfg.make_provider_for_model && cfg.list_models) {
        std::vector<SubagentModelInfo> models = cfg.list_models();
        if (!models.empty())
            params["properties"]["model"] = {
                {"type", "string"},
                {"description",
                 "Run sub-agent on a different model instead of same one this "
                 "agent uses. Omit to inherit current model. Available "
                 "models:\n" +
                     format_model_list(models)}};
    }

    ToolSpec spec{
        "spawn_subagent",
        "Spawn sub-agent to handle a sub-task independently. Sub-agent sees "
        "same file/shell/fetch/search tools as you but cannot spawn further "
        "sub-agents. Use for complex multi-step sub-tasks that would bloat this "
        "conversation. Sub-agent runs to completion, returns its final answer.",
        std::move(params)};

    return Tool{
        .spec = std::move(spec),
        .run = [cfg = std::move(cfg)](
                   const nlohmann::json& args) -> std::expected<std::string, Error> {
            // --- Validate args ---
            if (!args.is_object() || !args.contains("prompt") ||
                !args["prompt"].is_string())
                return std::unexpected(
                    Error{.msg = "missing or non-string argument: prompt", .code = 0});
            std::string prompt = args["prompt"].get<std::string>();
            if (prompt.empty())
                return std::unexpected(Error{.msg = "prompt must not be empty", .code = 0});

            // --- Resolve which provider the sub-agent runs on ---
            // No `model` (or it names the parent's current model) => reuse the
            // parent's live provider (today's behavior). Otherwise validate the
            // name against the advertised list and build a fresh, locally-owned
            // provider for this run only.
            std::unique_ptr<Provider> owned_provider;  // non-null only on override
            Provider* provider = nullptr;

            std::string sel;
            if (args.contains("model") && !args["model"].is_null()) {
                if (!args["model"].is_string())
                    return std::unexpected(
                        Error{.msg = "argument 'model' must be a string", .code = 0});
                sel = args["model"].get<std::string>();
                const auto first = sel.find_first_not_of(" \t\r\n");
                const auto last = sel.find_last_not_of(" \t\r\n");
                sel = (first == std::string::npos) ? std::string{}
                                                   : sel.substr(first, last - first + 1);
            }

            Provider& parent_provider = cfg.get_provider();
            if (sel.empty() || to_lower(sel) == to_lower(parent_provider.model())) {
                provider = &parent_provider;
            } else if (!cfg.make_provider_for_model || !cfg.list_models) {
                return std::unexpected(Error{
                    .msg = "sub-agent model selection is not available here", .code = 0});
            } else {
                std::vector<SubagentModelInfo> models = cfg.list_models();
                if (models.empty())
                    return std::unexpected(Error{
                        .msg = "sub-agent model selection is not available here",
                        .code = 0});
                const SubagentModelInfo* hit = nullptr;
                const std::string want = to_lower(sel);
                for (const auto& mi : models) {
                    if (to_lower(mi.model) == want) {
                        hit = &mi;
                        break;
                    }
                    if (to_lower(mi.provider + ":" + mi.model) == want ||
                        to_lower(mi.provider + ": " + mi.model) == want) {
                        hit = &mi;
                        break;
                    }
                }
                if (!hit)
                    return std::unexpected(Error{
                        .msg = "unknown model '" + sel + "'. Available models:\n" +
                               format_model_list(models),
                        .code = 0});
                auto built = cfg.make_provider_for_model(hit->model);  // canonical name
                if (!built) return std::unexpected(built.error());
                owned_provider = std::move(*built);
                provider = owned_provider.get();
            }

            // --- Build sub-agent tool registry ---
            ToolRegistry sub_reg = build_sub_registry(
                cfg.tools ? *cfg.tools : ToolRegistry{});

            // --- Build system prompt ---
            std::string sys_prompt = cfg.system_prompt.empty()
                                         ? std::string(kDefaultSubagentPrompt)
                                         : cfg.system_prompt;
            substitute(sys_prompt, "{TOOLS}", tool_list(sub_reg));
            substitute(sys_prompt, "{DIR}", cfg.root.string());

            // --- Build sub-agent ---
            AgentConfig acfg;
            acfg.system_prompt = std::move(sys_prompt);
            acfg.max_tool_output = cfg.max_tool_output;
            // No iteration cap — the sub-agent runs until the model stops.
            acfg.max_iterations = std::nullopt;

            Agent sub_agent(*provider, sub_reg, std::move(acfg));

            // --- Cancel plumbing ---
            if (cfg.parent_cancel) sub_agent.watch_cancel(*cfg.parent_cancel);

            // --- Approval gate ---
            if (cfg.get_approve) {
                if (auto gate = cfg.get_approve(); gate)
                    sub_agent.on_approve(std::move(gate));
            }

            // --- Subagent activity + text callbacks (progressive nested entries) ---
            if ((cfg.on_activity && *cfg.on_activity) ||
                (cfg.on_text     && *cfg.on_text)) {
                sub_agent.on_message([cb_act = cfg.on_activity,
                                      cb_txt = cfg.on_text](const Message& m) {
                    if (m.role() == Role::Assistant) {
                        // Forward text first so the TUI can create the turn
                        // before tool calls arrive. The two callbacks fire
                        // sequentially on the same worker thread; each acquires
                        // state_mtx independently, so the turn is guaranteed to
                        // exist when the tool-call callbacks land.
                        if (cb_txt && *cb_txt && !m.content().empty())
                            (*cb_txt)(m.content());
                        if (cb_act && *cb_act) {
                            for (const auto& tc : m.tool_calls())
                                (*cb_act)(tc.id, tc.name, tc.arguments_json,
                                          SubagentActivityStatus::Running, "");
                        }
                    } else if (m.role() == Role::Tool) {
                        if (cb_act && *cb_act) {
                            const bool failed = m.tool_failed();
                            (*cb_act)(m.tool_call_id(), "", "",
                                      failed ? SubagentActivityStatus::Failed
                                             : SubagentActivityStatus::Ok,
                                      m.content());
                        }
                    }
                });
            }

            // --- Accumulate usage for advisory logging ---
            int total_tokens = 0;
            sub_agent.on_usage([&](const Usage& u) {
                if (u.present) total_tokens += u.total_tokens;
            });

            // --- Run the sub-agent ---
            auto result = sub_agent.run(std::move(prompt));

            // --- Log advisory usage (not forwarded to the gauge) ---
            if (cfg.get_on_usage && total_tokens > 0) {
                if (auto log = cfg.get_on_usage(); log) {
                    Usage summary;
                    summary.total_tokens = total_tokens;
                    summary.present = true;
                    log(summary);
                }
            }

            return result;
        }};
}

}  // namespace moocode
