#include "agent/permissions.hpp"

#include <set>
#include <string>

#include "test_harness.hpp"

using namespace flagent;

namespace {
ToolCall call(std::string name) { return ToolCall{.id = "id", .name = std::move(name), .arguments_json = "{}"}; }
}  // namespace

TEST("Permissions: unknown tool is not allowed") {
    Permissions p;
    CHECK(!p.allowed("read_file"));
}

TEST("Permissions: session grant is remembered in-process") {
    Permissions p;
    p.grant_session("read_file");
    CHECK(p.allowed("read_file"));
    CHECK(!p.allowed("run_bash"));
}

TEST("Permissions: seeded always-set is honored") {
    Permissions p{std::set<std::string>{"edit_file", "list_dir"}};
    CHECK(p.allowed("edit_file"));
    CHECK(p.allowed("list_dir"));
    CHECK(!p.allowed("run_bash"));
}

TEST("Permissions: always grant fires the save hook with the full set") {
    std::set<std::string> saved;
    int calls = 0;
    Permissions p{{}, [&](const std::set<std::string>& s) { saved = s; ++calls; }};
    p.grant_always("edit_file");
    CHECK(p.allowed("edit_file"));
    CHECK_EQ(calls, 1);
    CHECK(saved.count("edit_file") == 1);
}

TEST("Permissions: re-granting a known tool does not re-fire the save hook") {
    int calls = 0;
    Permissions p{{}, [&](const std::set<std::string>&) { ++calls; }};
    p.grant_always("list_dir");
    p.grant_always("list_dir");
    CHECK_EQ(calls, 1);  // only the first (new) grant persists
}

TEST("Permissions: a memory-only object never tries to persist") {
    Permissions p;  // empty save hook
    p.grant_always("write_file");
    CHECK(p.allowed("write_file"));  // remembered in memory, no crash
}

TEST("decide: already-allowed tool runs without prompting") {
    Permissions p;
    p.grant_session("read_file");
    bool prompted = false;
    bool run = decide(p, call("read_file"),
                      [&](const ToolCall&) { prompted = true; return Approval::Deny; });
    CHECK(run);
    CHECK(!prompted);
}

TEST("decide: Once runs but records nothing") {
    Permissions p;
    CHECK(decide(p, call("run_bash"), [](const ToolCall&) { return Approval::Once; }));
    CHECK(!p.allowed("run_bash"));  // not remembered
}

TEST("decide: Session runs and remembers for the process") {
    Permissions p;
    CHECK(decide(p, call("run_bash"), [](const ToolCall&) { return Approval::Session; }));
    CHECK(p.allowed("run_bash"));
}

TEST("decide: Always runs and persists via the hook") {
    std::set<std::string> saved;
    Permissions p{{}, [&](const std::set<std::string>& s) { saved = s; }};
    CHECK(decide(p, call("write_file"), [](const ToolCall&) { return Approval::Always; }));
    CHECK(p.allowed("write_file"));
    CHECK(saved.count("write_file") == 1);
}

TEST("decide: Deny does not run and records nothing") {
    Permissions p;
    CHECK(!decide(p, call("run_bash"), [](const ToolCall&) { return Approval::Deny; }));
    CHECK(!p.allowed("run_bash"));
}
