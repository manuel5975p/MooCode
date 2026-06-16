#include "agent/skills.hpp"

#include <algorithm>
#include <utility>

#include "agent/json_util.hpp"

namespace moocode {

void SkillRegistry::add(Skill skill) {
    std::lock_guard lk(mtx_);
    auto it = std::ranges::find_if(
        entries_, [&](const Entry& e) { return e.skill.name == skill.name; });
    if (it != entries_.end())
        it->skill = std::move(skill);  // last registration wins, keep loaded flag
    else
        entries_.push_back(Entry{.skill = std::move(skill), .loaded = false});
}

bool SkillRegistry::known(const std::string& name) const {
    std::lock_guard lk(mtx_);
    return std::ranges::any_of(
        entries_, [&](const Entry& e) { return e.skill.name == name; });
}

bool SkillRegistry::is_loaded(const std::string& name) const {
    std::lock_guard lk(mtx_);
    auto it = std::ranges::find_if(
        entries_, [&](const Entry& e) { return e.skill.name == name; });
    return it != entries_.end() && it->loaded;
}

std::vector<SkillRegistry::Listing> SkillRegistry::list() const {
    std::lock_guard lk(mtx_);
    std::vector<Listing> out;
    out.reserve(entries_.size());
    for (const auto& e : entries_)
        out.push_back(Listing{.name = e.skill.name,
                              .description = e.skill.description,
                              .loaded = e.loaded});
    return out;
}

std::expected<std::string, Error> SkillRegistry::load(const std::string& name,
                                                      ToolRegistry& reg) {
    // Find the entry and snapshot its loader under the lock; run the loader
    // outside the lock so a loader that itself touches the registry can't
    // deadlock, then re-lock to flip the loaded flag.
    std::function<std::expected<std::string, Error>(ToolRegistry&)> loader;
    {
        std::lock_guard lk(mtx_);
        auto it = std::ranges::find_if(
            entries_, [&](const Entry& e) { return e.skill.name == name; });
        if (it == entries_.end())
            return std::unexpected(Error{.msg = "unknown skill: " + name, .code = 0});
        if (it->loaded)
            return "skill '" + name + "' is already loaded";
        loader = it->skill.load;
    }

    if (!loader)
        return std::unexpected(
            Error{.msg = "skill '" + name + "' has no loader", .code = 0});

    auto result = loader(reg);
    if (!result) return result;  // leave unloaded on failure

    {
        std::lock_guard lk(mtx_);
        auto it = std::ranges::find_if(
            entries_, [&](const Entry& e) { return e.skill.name == name; });
        if (it != entries_.end()) it->loaded = true;
    }
    return result;
}

namespace {

// "  - name: description [loaded]" lines for the tool description. Deterministic
// (registration order). Trailing newline dropped.
std::string format_skill_list(const std::vector<SkillRegistry::Listing>& l) {
    std::string out;
    for (const auto& s : l) {
        out += "  - " + s.name;
        if (!s.description.empty()) out += ": " + s.description;
        if (s.loaded) out += " [loaded]";
        out += '\n';
    }
    if (!out.empty()) out.pop_back();
    return out;
}

}  // namespace

Tool load_skill_tool(SkillRegistry& skills, ToolRegistry& reg) {
    std::string desc =
        "Load an optional skill, registering its extra tools for use on the "
        "next turn. Skills are not active by default. Available skills:\n" +
        format_skill_list(skills.list());

    nlohmann::json params = json::parse_or(R"({
        "type":"object",
        "properties":{
          "name":{"type":"string","description":"Name of the skill to load."}},
        "required":["name"]})");

    ToolSpec spec{"load_skill", std::move(desc), std::move(params)};

    return Tool{
        .spec = std::move(spec),
        .run = [&skills, &reg](const nlohmann::json& args)
                   -> std::expected<std::string, Error> {
            if (!args.is_object() || !args.contains("name") ||
                !args["name"].is_string())
                return std::unexpected(
                    Error{.msg = "missing or non-string argument: name", .code = 0});
            std::string name = args["name"].get<std::string>();
            const auto first = name.find_first_not_of(" \t\r\n");
            const auto last = name.find_last_not_of(" \t\r\n");
            name = (first == std::string::npos)
                       ? std::string{}
                       : name.substr(first, last - first + 1);
            if (name.empty())
                return std::unexpected(Error{.msg = "skill name must not be empty", .code = 0});
            return skills.load(name, reg);
        }};
}

}  // namespace moocode
