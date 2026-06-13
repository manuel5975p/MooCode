#include "agent/tools.hpp"

#include <algorithm>

namespace flagent {

void ToolRegistry::add(Tool tool) {
    auto it = std::ranges::find_if(
        tools_, [&](const Tool& t) { return t.spec.name == tool.spec.name; });
    if (it != tools_.end())
        *it = std::move(tool);  // last registration wins
    else
        tools_.push_back(std::move(tool));
}

bool ToolRegistry::has(const std::string& name) const {
    return std::ranges::any_of(tools_,
                               [&](const Tool& t) { return t.spec.name == name; });
}

std::vector<ToolSpec> ToolRegistry::specs() const {
    std::vector<ToolSpec> out;
    out.reserve(tools_.size());
    for (const auto& t : tools_) out.push_back(t.spec);
    return out;
}

std::expected<std::string, Error> ToolRegistry::invoke(
    const std::string& name, const nlohmann::json& args) const {
    auto it = std::ranges::find_if(
        tools_, [&](const Tool& t) { return t.spec.name == name; });
    if (it == tools_.end())
        return std::unexpected(Error{.msg = "unknown tool: " + name, .code = 0});
    return it->run(args);
}

}  // namespace flagent
