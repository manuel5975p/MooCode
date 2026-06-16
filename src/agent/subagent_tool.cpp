#include "agent/subagent_tool.hpp"

#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "agent/agent.hpp"
#include "agent/json_util.hpp"
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

// Tools a sub-agent must never inherit: the spawning/skill-loading meta-tools.
// A sub-agent cannot spawn further sub-agents, ask the user, or load skills.
bool is_meta_tool(const std::string& name) {
    return name == "spawn_subagent" || name == "spawn_subagent_restricted" ||
           name == "ask_user" || name == "load_skill";
}

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

// Build the sub-agent's tool registry: every parent tool that is not a meta-tool
// and, when `allow` is non-null, is named in it. A null `allow` => no filtering
// beyond the meta-tool exclusion (the unrestricted spawn_subagent behavior).
ToolRegistry build_sub_registry(const ToolRegistry& parent,
                                const std::set<std::string>* allow) {
    ToolRegistry sub;
    for (const auto& t : parent.tools()) {
        if (is_meta_tool(t.spec.name)) continue;
        if (allow && !allow->contains(t.spec.name)) continue;
        sub.add(t);
    }
    return sub;
}

// The set of tool names a sub-agent could legitimately be granted (every
// non-meta tool in the parent registry). Used to validate `tools`/`permissions`
// arguments so the parent agent gets a precise error instead of a silent no-op.
std::set<std::string> grantable_tool_names(const ToolRegistry& parent) {
    std::set<std::string> out;
    for (const auto& t : parent.tools())
        if (!is_meta_tool(t.spec.name)) out.insert(t.spec.name);
    return out;
}

std::string trim_copy(std::string s) {
    const auto first = s.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    const auto last = s.find_last_not_of(" \t\r\n");
    return s.substr(first, last - first + 1);
}

// Parse an optional JSON string-array argument into a trimmed, deduped set.
// Missing/null => empty set (success). Wrong shape => recoverable Error.
std::expected<std::set<std::string>, Error> parse_name_set(
    const nlohmann::json& args, const char* key) {
    std::set<std::string> out;
    if (!args.contains(key) || args[key].is_null()) return out;
    if (!args[key].is_array())
        return std::unexpected(Error{
            .msg = std::string("argument '") + key + "' must be an array of strings",
            .code = 0});
    for (const auto& e : args[key]) {
        if (!e.is_string())
            return std::unexpected(Error{
                .msg = std::string("argument '") + key + "' must contain only strings",
                .code = 0});
        if (std::string s = trim_copy(e.get<std::string>()); !s.empty())
            out.insert(std::move(s));
    }
    return out;
}

// Comma-join a set for error messages (deterministic: std::set is ordered).
std::string join_names(const std::set<std::string>& names) {
    std::string out;
    for (const auto& n : names) {
        if (!out.empty()) out += ", ";
        out += n;
    }
    return out;
}

// Names in `want` that are not in `available`. Empty => all valid.
std::set<std::string> unknown_names(const std::set<std::string>& want,
                                    const std::set<std::string>& available) {
    std::set<std::string> bad;
    for (const auto& n : want)
        if (!available.contains(n)) bad.insert(n);
    return bad;
}

// Shared spawn body for both the unrestricted and restricted tools. When
// `restricted` is true, the `tools` and `permissions` arguments are honored:
// `tools` filters the sub-agent's registry and `permissions` names tools it may
// run without prompting (others fall back to the parent's approval gate).
std::expected<std::string, Error> run_subagent(const SubagentConfig& cfg,
                                               const nlohmann::json& args,
                                               bool restricted) {
    // --- Validate args ---
    if (!args.is_object() || !args.contains("prompt") ||
        !args["prompt"].is_string())
        return std::unexpected(
            Error{.msg = "missing or non-string argument: prompt", .code = 0});
    std::string prompt = args["prompt"].get<std::string>();
    if (prompt.empty())
        return std::unexpected(Error{.msg = "prompt must not be empty", .code = 0});

    // --- Restricted-only args: tool subset + auto-approve allowlist ---
    std::set<std::string> tool_subset;   // empty => inherit all
    std::set<std::string> auto_approve;  // empty => nothing pre-approved
    if (restricted) {
        auto ts = parse_name_set(args, "tools");
        if (!ts) return std::unexpected(ts.error());
        tool_subset = std::move(*ts);
        auto pa = parse_name_set(args, "permissions");
        if (!pa) return std::unexpected(pa.error());
        auto_approve = std::move(*pa);

        const std::set<std::string> grantable =
            grantable_tool_names(cfg.tools ? *cfg.tools : ToolRegistry{});
        if (auto bad = unknown_names(tool_subset, grantable); !bad.empty())
            return std::unexpected(Error{
                .msg = "unknown tool name(s) in 'tools': " + join_names(bad) +
                       ". Grantable tools: " + join_names(grantable),
                .code = 0});
        if (auto bad = unknown_names(auto_approve, grantable); !bad.empty())
            return std::unexpected(Error{
                .msg = "unknown tool name(s) in 'permissions': " + join_names(bad) +
                       ". Grantable tools: " + join_names(grantable),
                .code = 0});
    }

    // --- Resolve which provider the sub-agent runs on ---
    // No `model` (or it names the parent's current model) => reuse the parent's
    // live provider. Otherwise validate the name against the advertised list and
    // build a fresh, locally-owned provider for this run only.
    std::unique_ptr<Provider> owned_provider;  // non-null only on override
    Provider* provider = nullptr;

    std::string sel;
    if (args.contains("model") && !args["model"].is_null()) {
        if (!args["model"].is_string())
            return std::unexpected(
                Error{.msg = "argument 'model' must be a string", .code = 0});
        sel = trim_copy(args["model"].get<std::string>());
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
                .msg = "sub-agent model selection is not available here", .code = 0});
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
        cfg.tools ? *cfg.tools : ToolRegistry{},
        (restricted && !tool_subset.empty()) ? &tool_subset : nullptr);

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
    // Unrestricted: install the parent's gate verbatim. Restricted: wrap it so
    // tools in `permissions` run without prompting and everything else defers to
    // the parent's gate (which may prompt the user). When no parent gate exists
    // (e.g. --yes / headless), unlisted tools simply run, matching the default.
    Agent::ApprovalFn parent_gate;
    if (cfg.get_approve) parent_gate = cfg.get_approve();
    if (restricted) {
        if (!auto_approve.empty() || parent_gate)
            sub_agent.on_approve(
                [auto_approve = std::move(auto_approve),
                 parent_gate = std::move(parent_gate)](const ToolCall& tc) -> bool {
                    if (auto_approve.contains(tc.name)) return true;
                    if (parent_gate) return parent_gate(tc);
                    return true;
                });
    } else if (parent_gate) {
        sub_agent.on_approve(std::move(parent_gate));
    }

    // --- Subagent activity + text callbacks (progressive nested entries) ---
    if ((cfg.on_activity && *cfg.on_activity) ||
        (cfg.on_text && *cfg.on_text)) {
        sub_agent.on_message([cb_act = cfg.on_activity,
                              cb_txt = cfg.on_text](const Message& m) {
            if (m.role() == Role::Assistant) {
                // Forward text first so the TUI can create the turn before tool
                // calls arrive. The two callbacks fire sequentially on the same
                // worker thread; each acquires state_mtx independently, so the
                // turn is guaranteed to exist when the tool-call callbacks land.
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
}

// Add the optional `model` property to a spawn tool's parameter schema, when the
// config advertises selectable models. Shared by both tool builders.
void maybe_add_model_param(nlohmann::json& params, const SubagentConfig& cfg) {
    if (!cfg.make_provider_for_model || !cfg.list_models) return;
    std::vector<SubagentModelInfo> models = cfg.list_models();
    if (models.empty()) return;
    params["properties"]["model"] = {
        {"type", "string"},
        {"description",
         "Run sub-agent on a different model instead of same one this agent "
         "uses. Omit to inherit current model. Available models:\n" +
             format_model_list(models)}};
}

}  // namespace

Tool spawn_subagent_tool(SubagentConfig cfg) {
    nlohmann::json params = json::parse_or(R"({
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
    maybe_add_model_param(params, cfg);

    ToolSpec spec{
        "spawn_subagent",
        "Spawn sub-agent to handle a sub-task independently. Sub-agent sees "
        "same file/shell/fetch/search tools as you but cannot spawn further "
        "sub-agents. Use for complex multi-step sub-tasks that would bloat this "
        "conversation. Sub-agent runs to completion, returns its final answer.",
        std::move(params)};

    return Tool{
        .spec = std::move(spec),
        .run = [cfg = std::move(cfg)](const nlohmann::json& args)
                   -> std::expected<std::string, Error> {
            return run_subagent(cfg, args, /*restricted=*/false);
        }};
}

Tool spawn_subagent_restricted_tool(SubagentConfig cfg) {
    nlohmann::json params = json::parse_or(R"({
        "type":"object",
        "properties":{
          "prompt":{
            "type":"string",
            "description":"Task for sub-agent. Be specific about what it should produce and any constraints."},
          "tools":{
            "type":"array",
            "items":{"type":"string"},
            "description":"Restrict the sub-agent to ONLY these tools (by name). Omit or leave empty to grant every tool you have. Names must match tools you can use; meta-tools (spawn_subagent*, ask_user, load_skill) are never granted."},
          "permissions":{
            "type":"array",
            "items":{"type":"string"},
            "description":"Tool names the sub-agent may run WITHOUT asking for approval. Tools not listed still go through the normal approval flow. Use to pre-authorize safe, read-only tools for an autonomous sub-agent."}},
        "required":["prompt"]})");

    maybe_add_model_param(params, cfg);

    ToolSpec spec{
        "spawn_subagent_restricted",
        "Spawn a sub-agent with a restricted tool set and explicit "
        "auto-approval list. Like spawn_subagent, but you pass `tools` (the "
        "subset of tools the sub-agent may see) and `permissions` (tools it may "
        "run without prompting). Use to sandbox an autonomous sub-agent to just "
        "the capabilities a task needs.",
        std::move(params)};

    return Tool{
        .spec = std::move(spec),
        .run = [cfg = std::move(cfg)](const nlohmann::json& args)
                   -> std::expected<std::string, Error> {
            return run_subagent(cfg, args, /*restricted=*/true);
        }};
}

}  // namespace moocode
