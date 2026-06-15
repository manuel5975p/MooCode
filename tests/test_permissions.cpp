#include "agent/permissions.hpp"

#include <atomic>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "test_harness.hpp"

using namespace moocode;

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

// Sibling sub-agents share one Permissions and reach decide()/grant_* on their
// own threads (tool calls run in parallel — see Agent::run). Hammer it from many
// threads: under ThreadSanitizer this flags any unsynchronised tier access, and
// every granted tool must end up allowed (no lost insert).
TEST("Permissions: concurrent grants are race-free and all stick") {
    std::atomic<int> saves{0};
    Permissions p{{}, [&](const std::set<std::string>&) {
                      saves.fetch_add(1, std::memory_order_relaxed); }};
    constexpr int kThreads = 8;
    std::vector<std::thread> ts;
    for (int t = 0; t < kThreads; ++t) {
        ts.emplace_back([&, t] {
            for (int i = 0; i < 50; ++i) {
                std::string tool = "tool_" + std::to_string((t + i) % 16);
                // Mix readers and both grant tiers, as decide() would.
                (void)p.allowed(tool);
                if (i % 2) p.grant_session(tool);
                else p.grant_always(tool);
            }
        });
    }
    for (auto& th : ts) th.join();
    for (int k = 0; k < 16; ++k)
        CHECK(p.allowed("tool_" + std::to_string(k)));
    // Each distinct always-grant fires the save hook exactly once.
    CHECK_EQ(saves.load(), 16);
}
