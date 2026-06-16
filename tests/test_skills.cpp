#include "agent/skills.hpp"

#include <expected>
#include <string>

#include <nlohmann/json.hpp>

#include "agent/tools.hpp"
#include "test_harness.hpp"

using namespace moocode;

namespace {

Tool dummy_tool(const std::string& name) {
    return Tool{
        .spec = ToolSpec{name, "d", nlohmann::json::parse(R"({"type":"object"})")},
        .run = [](const nlohmann::json&) -> std::expected<std::string, Error> {
            return std::string("ok");
        }};
}

// A skill whose loader registers one tool named `tool_name` and bumps `*runs`.
Skill tool_skill(const std::string& name, const std::string& tool_name,
                 int* runs) {
    return Skill{.name = name,
                 .description = "adds " + tool_name,
                 .load = [tool_name, runs](ToolRegistry& reg)
                     -> std::expected<std::string, Error> {
                     if (runs) ++*runs;
                     reg.add(dummy_tool(tool_name));
                     return std::string("loaded ") + tool_name;
                 }};
}

}  // namespace

TEST("skills: a registered skill starts unloaded and lists its description") {
    SkillRegistry skills;
    int runs = 0;
    skills.add(tool_skill("demo", "demo_tool", &runs));

    CHECK(skills.known("demo"));
    CHECK(!skills.is_loaded("demo"));
    auto l = skills.list();
    CHECK_EQ(l.size(), std::size_t{1});
    if (!l.empty()) {
        CHECK_EQ(l.front().name, std::string("demo"));
        CHECK(!l.front().loaded);
    }
    CHECK_EQ(runs, 0);  // registration must not run the loader
}

TEST("skills: load runs the loader once and registers its tool") {
    SkillRegistry skills;
    int runs = 0;
    skills.add(tool_skill("demo", "demo_tool", &runs));

    ToolRegistry reg;
    CHECK(!reg.has("demo_tool"));

    auto r = skills.load("demo", reg);
    CHECK(r.has_value());
    CHECK(reg.has("demo_tool"));
    CHECK(skills.is_loaded("demo"));
    CHECK_EQ(runs, 1);
}

TEST("skills: loading twice is idempotent and does not re-run the loader") {
    SkillRegistry skills;
    int runs = 0;
    skills.add(tool_skill("demo", "demo_tool", &runs));

    ToolRegistry reg;
    CHECK(skills.load("demo", reg).has_value());
    auto again = skills.load("demo", reg);
    CHECK(again.has_value());
    if (again) CHECK(again->find("already loaded") != std::string::npos);
    CHECK_EQ(runs, 1);  // loader did not run a second time
}

TEST("skills: loading an unknown skill is a recoverable error") {
    SkillRegistry skills;
    ToolRegistry reg;
    auto r = skills.load("nope", reg);
    CHECK(!r.has_value());
    if (!r) CHECK(r.error().msg.find("nope") != std::string::npos);
}

TEST("skills: a failing loader leaves the skill unloaded") {
    SkillRegistry skills;
    skills.add(Skill{.name = "boom",
                     .description = "fails",
                     .load = [](ToolRegistry&) -> std::expected<std::string, Error> {
                         return std::unexpected(Error{.msg = "kaboom", .code = 0});
                     }});
    ToolRegistry reg;
    auto r = skills.load("boom", reg);
    CHECK(!r.has_value());
    CHECK(!skills.is_loaded("boom"));
    // A retry is allowed (still not loaded).
    CHECK(!skills.load("boom", reg).has_value());
}

TEST("load_skill tool: loads a skill into the live registry by name") {
    SkillRegistry skills;
    int runs = 0;
    skills.add(tool_skill("demo", "demo_tool", &runs));

    ToolRegistry reg;
    Tool loader = load_skill_tool(skills, reg);
    CHECK_EQ(loader.spec.name, std::string("load_skill"));
    // The description should advertise the registered skill.
    CHECK(loader.spec.description.find("demo") != std::string::npos);

    auto r = loader.run(nlohmann::json{{"name", "demo"}});
    CHECK(r.has_value());
    CHECK(reg.has("demo_tool"));
    CHECK(skills.is_loaded("demo"));
}

TEST("load_skill tool: missing name argument is rejected") {
    SkillRegistry skills;
    ToolRegistry reg;
    Tool loader = load_skill_tool(skills, reg);
    auto r = loader.run(nlohmann::json{{"prompt", "oops"}});
    CHECK(!r.has_value());
}

TEST("load_skill tool: unknown skill name surfaces the registry error") {
    SkillRegistry skills;
    ToolRegistry reg;
    Tool loader = load_skill_tool(skills, reg);
    auto r = loader.run(nlohmann::json{{"name", "ghost"}});
    CHECK(!r.has_value());
    if (!r) CHECK(r.error().msg.find("ghost") != std::string::npos);
}
