#include "agent/skills.hpp"

#include <cstdio>
#include <expected>
#include <filesystem>
#include <fstream>
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

// ---- Markdown skills -------------------------------------------------------

TEST("markdown: heading + first paragraph become the description, body the prompt") {
    auto md = parse_markdown_skill(
        "git-helper",
        "# Git helper\nAdvanced git surgery.\n\nYou are a git expert.\nRebase carefully.\n");
    CHECK(md.has_value());
    if (md) {
        CHECK_EQ(md->name, std::string("git-helper"));
        CHECK_EQ(md->description, std::string("Git helper Advanced git surgery."));
        CHECK_EQ(md->prompt, std::string("You are a git expert.\nRebase carefully."));
    }
}

TEST("markdown: heading-only description, body after blank line") {
    auto md = parse_markdown_skill("p", "# Just a title\n\nThe whole prompt body.");
    CHECK(md.has_value());
    if (md) {
        CHECK_EQ(md->description, std::string("Just a title"));
        CHECK_EQ(md->prompt, std::string("The whole prompt body."));
    }
}

TEST("markdown: leading blank lines and CRLF are tolerated") {
    auto md = parse_markdown_skill("p", "\r\n\r\n# Title\r\n\r\nBody line.\r\n");
    CHECK(md.has_value());
    if (md) {
        CHECK_EQ(md->description, std::string("Title"));
        CHECK_EQ(md->prompt, std::string("Body line."));
    }
}

TEST("markdown: a body with no description heading still parses leniently") {
    auto md = parse_markdown_skill("p", "Short blurb.\n\nThe body.");
    CHECK(md.has_value());
    if (md) {
        CHECK_EQ(md->description, std::string("Short blurb."));
        CHECK_EQ(md->prompt, std::string("The body."));
    }
}

TEST("markdown: empty input is rejected") {
    CHECK(!parse_markdown_skill("p", "").has_value());
    CHECK(!parse_markdown_skill("p", "\n\n  \n").has_value());
}

TEST("markdown: a description with no body is rejected") {
    auto md = parse_markdown_skill("p", "# Only a heading, no blank-separated body\n");
    CHECK(!md.has_value());
}

TEST("markdown: a parsed skill loads by returning its prompt body, no tools") {
    auto md = parse_markdown_skill("greet", "# Greeter\n\nSay hello warmly.");
    CHECK(md.has_value());
    if (!md) return;

    SkillRegistry skills;
    skills.add(make_markdown_skill(*md));
    CHECK(skills.known("greet"));

    ToolRegistry reg;
    const std::size_t before = reg.tools().size();
    auto r = skills.load("greet", reg);
    CHECK(r.has_value());
    if (r) CHECK_EQ(*r, std::string("Say hello warmly."));
    CHECK_EQ(reg.tools().size(), before);  // prompt skills register no tools
}

TEST("markdown skills are flagged injects_prompt; tool skills are not") {
    SkillRegistry skills;
    int runs = 0;
    skills.add(tool_skill("demo", "demo_tool", &runs));
    auto md = parse_markdown_skill("greet", "# Greeter\n\nSay hello.");
    CHECK(md.has_value());
    if (md) skills.add(make_markdown_skill(*md));

    CHECK(skills.injects_prompt("greet"));    // prompt skill -> inject body
    CHECK(!skills.injects_prompt("demo"));     // tool skill -> status note only
    CHECK(!skills.injects_prompt("unknown"));  // unknown -> false

    CHECK(skills.effect_of("greet") == SkillEffect::InjectMessage);
    CHECK(skills.effect_of("demo") == SkillEffect::Tools);
    CHECK(skills.effect_of("unknown") == SkillEffect::Tools);  // unknown default
}

// ---- System-prompt skills (frontmatter) ------------------------------------

TEST("markdown: no frontmatter defaults to InjectMessage effect") {
    auto md = parse_markdown_skill("p", "# Title\n\nBody.");
    CHECK(md.has_value());
    if (md) CHECK(md->effect == SkillEffect::InjectMessage);
}

TEST("markdown: frontmatter effect:system selects SystemPrompt; body intact") {
    auto md = parse_markdown_skill(
        "persona", "---\neffect: system\n---\n# Persona\n\nYou are terse.");
    CHECK(md.has_value());
    if (md) {
        CHECK(md->effect == SkillEffect::SystemPrompt);
        CHECK_EQ(md->description, std::string("Persona"));
        CHECK_EQ(md->prompt, std::string("You are terse."));
    }
}

TEST("markdown: a non-system effect value stays InjectMessage") {
    auto md = parse_markdown_skill(
        "p", "---\neffect: message\n---\n# T\n\nBody.");
    CHECK(md.has_value());
    if (md) CHECK(md->effect == SkillEffect::InjectMessage);
}

TEST("markdown: unterminated frontmatter is not consumed (leniently parsed)") {
    // No closing fence: the `---` lines are treated as ordinary content, so the
    // first line becomes the description and the effect stays the default.
    auto md = parse_markdown_skill("p", "---\neffect: system\n# T\n\nBody.");
    CHECK(md.has_value());
    if (md) {
        CHECK(md->effect == SkillEffect::InjectMessage);
        CHECK(md->prompt.find("Body.") != std::string::npos);
    }
}

TEST("markdown: a SystemPrompt skill routes its body to the sink, not the result") {
    auto md = parse_markdown_skill(
        "persona", "---\neffect: system\n---\n# Persona\n\nBe terse.");
    CHECK(md.has_value());
    if (!md) return;

    std::string captured;
    SkillRegistry skills;
    skills.add(make_markdown_skill(
        *md, [&captured](std::string s) { captured = std::move(s); }));

    CHECK(skills.effect_of("persona") == SkillEffect::SystemPrompt);
    CHECK(!skills.injects_prompt("persona"));  // not a message-injecting skill

    ToolRegistry reg;
    const std::size_t before = reg.tools().size();
    auto r = skills.load("persona", reg);
    CHECK(r.has_value());
    CHECK_EQ(reg.tools().size(), before);           // registers no tools
    CHECK_EQ(captured, std::string("Be terse."));    // body went to the sink
    if (r) CHECK(*r != std::string("Be terse."));    // result is a status note
}

TEST("markdown: a SystemPrompt skill with no sink degrades to returning the body") {
    auto md = parse_markdown_skill(
        "persona", "---\neffect: system\n---\n# Persona\n\nBe terse.");
    CHECK(md.has_value());
    if (!md) return;

    SkillRegistry skills;
    skills.add(make_markdown_skill(*md));  // no sink supplied
    ToolRegistry reg;
    auto r = skills.load("persona", reg);
    CHECK(r.has_value());
    if (r) CHECK_EQ(*r, std::string("Be terse."));  // body, not silently dropped
}

TEST("load_markdown_skills: scans *.md, sorted, skips junk; missing dir is fine") {
    namespace fs = std::filesystem;
    SkillRegistry empty;
    CHECK(load_markdown_skills(empty, "/no/such/dir/here").empty());

    fs::path dir = fs::temp_directory_path() / "moocode_md_skills_test";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    auto put = [&](const char* fname, const char* body) {
        std::ofstream(dir / fname) << body;
    };
    put("beta.md", "# Beta\n\nBeta prompt.");
    put("alpha.md", "# Alpha\n\nAlpha prompt.");
    put("broken.md", "");                 // unparseable -> skipped
    put("notes.txt", "# Not a skill\n\nx"); // wrong extension -> ignored

    SkillRegistry skills;
    auto names = load_markdown_skills(skills, dir);
    CHECK_EQ(names.size(), std::size_t{2});
    if (names.size() == 2) {
        CHECK_EQ(names[0], std::string("alpha"));  // sorted by filename
        CHECK_EQ(names[1], std::string("beta"));
    }
    CHECK(skills.known("alpha"));
    CHECK(skills.known("beta"));
    CHECK(!skills.known("broken"));
    CHECK(!skills.known("notes"));

    fs::remove_all(dir, ec);
}
