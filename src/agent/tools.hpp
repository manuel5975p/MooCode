#ifndef MOOCODE_TOOLS_HPP
#define MOOCODE_TOOLS_HPP

// A registry mapping tool names to (schema, callable). The agent advertises
// specs() to the model and dispatches model-chosen calls through invoke().

#include <expected>
#include <functional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "agent/types.hpp"

namespace moocode {

struct Tool {
    ToolSpec spec;
    // Run the tool against parsed JSON args. Returns result text on success, or
    // an Error whose msg is fed back to the model so it can self-correct.
    std::function<std::expected<std::string, Error>(const nlohmann::json&)> run;
};

class ToolRegistry {
public:
    // Register a tool. A duplicate name replaces the prior entry (last wins).
    void add(Tool tool);

    // True if a tool with this name is registered.
    bool has(const std::string& name) const;

    // Advertised specs in registration order (deterministic for stable output).
    std::vector<ToolSpec> specs() const;

    // Dispatch `name` with `args`. Error if the tool is unknown, otherwise the
    // tool's own result/Error. pre: none. post: never throws.
    std::expected<std::string, Error> invoke(const std::string& name,
                                             const nlohmann::json& args) const;

    // Registered tools in insertion order (for sub-agent filtering).
    const std::vector<Tool>& tools() const { return tools_; }

private:
    std::vector<Tool> tools_;  // insertion order preserved
};

// Render registered tool names as "  - name: description" lines. When
// `desc_clip` > 0, each description is trimmed to its first line and capped
// at `desc_clip` bytes with an ellipsis (for the system-prompt listing where
// descriptions are also sent in the JSON schema). When 0, the full description
// is used (for sub-agent model listings). Trailing newline is dropped.
std::string tool_list(const ToolRegistry& reg, std::size_t desc_clip = 0);

}  // namespace moocode

#endif  // MOOCODE_TOOLS_HPP
