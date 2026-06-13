// Tests for the TUI's screen-free units — TuiState (the render model fed by
// the agent's streaming callbacks), ApprovalGate (the worker↔UI tool-approval
// handshake) and sanitize_tui_text — plus the width/clipping behaviour of the
// patched FTXUI (see cmake/PatchFTXUI.cmake), exercised on an off-screen
// ftxui::Screen.

#include "agent/tui.hpp"

#include "agent/image_chip.hpp"   // pasted-image chip helpers
#include "agent/permissions.hpp"  // Approval (also re-exported via tui.hpp)
#include "agent/tui_text.hpp"     // sanitize_tui_text

#include <chrono>
#include <thread>

#include <ftxui/dom/elements.hpp>  // hbox, text, size
#include <ftxui/dom/node.hpp>      // Render
#include <ftxui/screen/screen.hpp>
#include <ftxui/screen/string.hpp>  // string_width

#include "test_harness.hpp"

using namespace flagent;
using Kind = TuiState::ChatKind;
using Status = TuiState::Status;

namespace {

// Build an assistant message carrying one tool call.
Message assistant_call(std::string id, std::string name, std::string args) {
    std::vector<ToolCall> calls;
    calls.push_back(ToolCall{.id = std::move(id), .name = std::move(name), .arguments_json = std::move(args)});
    return Message::assistant("", std::move(calls));
}

Message tool_result(std::string id, std::string content, bool failed = false) {
    return Message::tool(id, content, failed);
}

}  // namespace

TEST("syntax theme names round-trip through id mapping") {
    for (const std::string& name : syntax_theme_names()) {
        auto id = syntax_theme_from_name(name);
        CHECK(id.has_value());
        CHECK_EQ(std::string(syntax_theme_name(*id)), name);
    }
    // The four expected schemes are present.
    auto names = syntax_theme_names();
    CHECK_EQ(names.size(), std::size_t{4});
    CHECK(syntax_theme_from_name("default").has_value());
    CHECK(syntax_theme_from_name("none").has_value());
}

TEST("syntax_theme_from_name is case-insensitive and rejects junk") {
    CHECK(syntax_theme_from_name("VIVID") == SyntaxTheme::Vivid);
    CHECK(syntax_theme_from_name("Mono") == SyntaxTheme::Mono);
    CHECK(syntax_theme_from_name("nope").has_value() == false);
    CHECK(syntax_theme_from_name("").has_value() == false);
}

TEST("TuiState defaults to the Default theme and is settable") {
    TuiState s;
    CHECK(s.syntax_theme() == SyntaxTheme::Default);
    s.set_syntax_theme(SyntaxTheme::Mono);
    CHECK(s.syntax_theme() == SyntaxTheme::Mono);
}

TEST("apply_delta accumulates answer into one assistant entry") {
    TuiState s;
    s.apply_delta("Hel", "");
    s.apply_delta("lo", "");
    CHECK_EQ(s.chat().size(), std::size_t{1});
    CHECK(s.chat()[0].kind == Kind::Assistant);
    CHECK_EQ(s.chat()[0].text, std::string("Hello"));
}

TEST("apply_delta routes reasoning separately with newlines collapsed") {
    TuiState s;
    s.apply_delta("", "think\ning");  // reasoning fragment with a newline
    s.apply_delta("answer", "");
    CHECK_EQ(s.chat().size(), std::size_t{2});
    CHECK(s.chat()[0].kind == Kind::Reasoning);
    CHECK_EQ(s.chat()[0].text, std::string("think ing"));  // \n -> space
    CHECK(s.chat()[1].kind == Kind::Assistant);
    CHECK_EQ(s.chat()[1].text, std::string("answer"));
}

TEST("load replays assistant reasoning before its prose") {
    TuiState s;
    Conversation conv;
    conv.push_back(Message::assistant("the answer", {}, "my thoughts"));
    s.load(conv);
    CHECK_EQ(s.chat().size(), std::size_t{2});
    CHECK(s.chat()[0].kind == Kind::Reasoning);
    CHECK_EQ(s.chat()[0].text, std::string("my thoughts"));
    CHECK(s.chat()[1].kind == Kind::Assistant);
    CHECK_EQ(s.chat()[1].text, std::string("the answer"));
}

TEST("load omits an empty reasoning block") {
    TuiState s;
    Conversation conv;
    conv.push_back(Message::assistant("just answer"));
    s.load(conv);
    CHECK_EQ(s.chat().size(), std::size_t{1});
    CHECK(s.chat()[0].kind == Kind::Assistant);
}

TEST("push_user seals the prior run so the next delta starts fresh") {
    TuiState s;
    s.apply_delta("first", "");
    s.push_user("again");
    s.apply_delta("second", "");
    CHECK_EQ(s.chat().size(), std::size_t{3});
    CHECK(s.chat()[0].kind == Kind::Assistant);
    CHECK(s.chat()[1].kind == Kind::User);
    CHECK(s.chat()[2].kind == Kind::Assistant);
    CHECK_EQ(s.chat()[2].text, std::string("second"));
}

TEST("apply_message of an assistant turn seals the prose run") {
    TuiState s;
    s.apply_delta("prose", "");
    s.apply_message(assistant_call("c1", "list_dir", "{}"));
    s.apply_delta("more", "");  // next turn's prose must be a new entry
    CHECK_EQ(s.chat().size(), std::size_t{2});
    CHECK_EQ(s.chat()[0].text, std::string("prose"));
    CHECK_EQ(s.chat()[1].text, std::string("more"));
}

TEST("tool calls become Running activities, then flip on their result") {
    TuiState s;
    s.apply_message(assistant_call("c1", "read_file", "{\"path\":\"llm.md\"}"));
    CHECK_EQ(s.activity().size(), std::size_t{1});
    CHECK_EQ(s.activity()[0].name, std::string("read_file"));
    CHECK_EQ(s.activity()[0].args, std::string("llm.md"));  // salient arg only
    CHECK(s.activity()[0].status == Status::Running);

    s.apply_message(tool_result("c1", "file contents here"));
    CHECK(s.activity()[0].status == Status::Ok);
    CHECK_EQ(s.activity()[0].result_full, std::string("file contents here"));
}

TEST("a tool_failed result marks the activity Failed") {
    TuiState s;
    s.apply_message(assistant_call("c1", "run_bash", "{\"cmd\":\"x\"}"));
    s.apply_message(tool_result("c1", "ERROR: command failed", /*failed=*/true));
    CHECK(s.activity()[0].status == Status::Failed);
}

// A successful tool whose output begins with "ERROR:" must stay Ok: status now
// follows the typed tool_failed bit, not the content prefix.
TEST("a successful result starting 'ERROR:' stays Ok") {
    TuiState s;
    s.apply_message(assistant_call("c1", "grep", "{\"q\":\"x\"}"));
    s.apply_message(tool_result("c1", "ERROR: appears in matched text", /*failed=*/false));
    CHECK(s.activity()[0].status == Status::Ok);
}

TEST("a result for an unknown id changes nothing") {
    TuiState s;
    s.apply_message(assistant_call("c1", "list_dir", "{}"));
    s.apply_message(tool_result("nope", "stray"));
    CHECK(s.activity()[0].status == Status::Running);
    CHECK(s.activity()[0].result_full.empty());
}

TEST("tool_arg_summary picks the most informative argument") {
    // Target-like keys beat content-like ones; edit_file shows its path, not
    // the replacement text.
    CHECK_EQ(tool_arg_summary(R"({"new":"x","old":"y","path":"src/a.cpp"})"),
             std::string("src/a.cpp"));
    CHECK_EQ(tool_arg_summary(R"({"cmd":"ninja -C build"})"),
             std::string("ninja -C build"));
    CHECK_EQ(tool_arg_summary(R"({"url":"http://x/y"})"),
             std::string("http://x/y"));
    CHECK_EQ(tool_arg_summary(R"({"prompt":"fix the build"})"),
             std::string("fix the build"));
    // Non-object, unparseable, or no matching key: raw args, first line only.
    CHECK_EQ(tool_arg_summary("[1,2,3]"), std::string("[1,2,3]"));
    CHECK_EQ(tool_arg_summary("not json\nmore"), std::string("not json…"));
    CHECK_EQ(tool_arg_summary(R"({"lines":3})"), std::string(R"({"lines":3})"));
    // Long values are capped to one line with an ellipsis.
    std::string long_cmd(300, 'x');
    std::string summary = tool_arg_summary("{\"cmd\":\"" + long_cmd + "\"}");
    CHECK_EQ(summary.size(), std::size_t{200 + 3});  // 200 + "…" (3 bytes)
    CHECK(summary.ends_with("…"));
}

TEST("activity rows carry the arg summary, full args go to the detail side") {
    TuiState s;
    s.apply_message(assistant_call(
        "c1", "edit_file", R"({"path":"src/a.cpp","old":"x","new":"y"})"));
    CHECK_EQ(s.activity()[0].args, std::string("src/a.cpp"));
    CHECK_EQ(s.activity()[0].args_full,
             std::string(R"({"path":"src/a.cpp","old":"x","new":"y"})"));
}

TEST("activity retains full args and result for the detail view") {
    TuiState s;
    std::string big_args = "{\"cmd\":\"" + std::string(200 * 1024, 'a') + "\"}";
    s.apply_message(assistant_call("t1", "run_bash", big_args));
    {
        const auto& a = s.activity()[0];
        CHECK(a.args_full.size() <= std::size_t{128 * 1024});
        CHECK(a.args_full.starts_with("{\"cmd\":\"aaa"));  // head survives
        CHECK(a.args_full.ends_with("aaa\"}"));            // tail survives
        CHECK(a.result_full.empty());                      // nothing yet
    }
    std::string big_out = "HEAD\n" + std::string(300 * 1024, 'x') + "\nTAIL";
    s.apply_message(tool_result("t1", big_out));
    {
        const auto& a = s.activity()[0];
        CHECK(a.result_full.size() <= std::size_t{128 * 1024});
        CHECK(a.result_full.starts_with("HEAD\n"));
        CHECK(a.result_full.ends_with("\nTAIL"));
    }
}

TEST("short args/results are retained verbatim in full form") {
    TuiState s;
    s.apply_message(assistant_call("t1", "read_file", R"({"path":"a.cpp"})"));
    s.apply_message(tool_result("t1", "line1\nline2"));
    const auto& a = s.activity()[0];
    CHECK_EQ(a.args_full, std::string(R"({"path":"a.cpp"})"));
    CHECK_EQ(a.result_full, std::string("line1\nline2"));  // newlines preserved
}

TEST("activity stamps start and finish times") {
    TuiState s;
    s.apply_message(assistant_call("t1", "read_file", "{}"));
    CHECK(s.activity()[0].started != std::chrono::steady_clock::time_point{});
    CHECK(s.activity()[0].finished == std::chrono::steady_clock::time_point{});
    s.apply_message(tool_result("t1", "ok"));
    CHECK(s.activity()[0].finished >= s.activity()[0].started);
}

TEST("spawn_subagent opens a group labelled from its prompt") {
    TuiState s;
    s.apply_message(
        assistant_call("sa1", "spawn_subagent", R"({"prompt":"Audit the parser"})"));
    // The parent's own call still shows in the flat activity log...
    CHECK_EQ(s.activity().size(), std::size_t{1});
    CHECK_EQ(s.activity()[0].name, std::string("spawn_subagent"));
    // ...and a sub-agent group is opened, labelled from the prompt.
    CHECK_EQ(s.subagents().size(), std::size_t{1});
    CHECK_EQ(s.subagents()[0].label, std::string("Audit the parser"));
    CHECK(s.subagents()[0].status == Status::Running);
    CHECK_EQ(s.subagents()[0].turns.size(), std::size_t{0});
    CHECK(s.subagents()[0].expanded);  // running groups start unfolded
    CHECK_EQ(s.subagents()[0].prompt_full, std::string("Audit the parser"));
}

TEST("sub-agent tool calls route into the group, not the activity log") {
    TuiState s;
    s.apply_message(assistant_call("sa1", "spawn_subagent", R"({"prompt":"go"})"));
    s.push_subagent_text("I'll start by reading the file.");
    s.push_subagent_activity("n1", "read_file", "{}", Status::Running, "");
    s.push_subagent_activity("n1", "", "", Status::Ok, "ok line\nmore");
    // The nested call did not leak into the flat activity log.
    CHECK_EQ(s.activity().size(), std::size_t{1});
    const auto& g = s.subagents()[0];
    CHECK_EQ(g.turns.size(), std::size_t{1});
    CHECK_EQ(g.turns[0].calls.size(), std::size_t{1});
    CHECK_EQ(g.turns[0].assistant_text, std::string("I'll start by reading the file."));
    CHECK(g.turns[0].calls[0].status == Status::Ok);
    // Full result retained for the detail pane, newlines preserved.
    CHECK_EQ(g.turns[0].calls[0].result_full, std::string("ok line\nmore"));
}

TEST("selecting a sub-agent group resolves to its prompt and history") {
    // Regression: clicking a spawn_subagent row must surface the sub-agent's
    // prompt and full internal history in the detail pane. detail_node() is the
    // pure contract render_detail consumes, so lock it here.
    TuiState s;
    s.apply_message(
        assistant_call("sa1", "spawn_subagent", R"({"prompt":"Audit the parser"})"));
    s.push_subagent_text("Reading the file first.");
    s.push_subagent_activity("n1", "read_file", R"({"path":"p.cpp"})",
                             Status::Running, "");
    s.push_subagent_activity("n1", "", "", Status::Ok, "line one\nline two");
    s.apply_message(tool_result("sa1", "audit complete"));

    // A click selects the group's top-level row (no parent_id).
    NodeKey group_key{NodeKey::Pane::Activity, 0, "sa1", ""};
    s.select(group_key);

    auto dn = s.detail_node();
    CHECK(dn.has_value());
    CHECK(*dn == group_key);

    const auto* g = s.find_group(dn->id);
    CHECK(g != nullptr);
    CHECK_EQ(g->prompt_full, std::string("Audit the parser"));
    CHECK_EQ(g->turns.size(), std::size_t{1});
    CHECK_EQ(g->turns[0].assistant_text, std::string("Reading the file first."));
    CHECK_EQ(g->turns[0].calls.at(0).result_full, std::string("line one\nline two"));
    CHECK_EQ(g->result_full, std::string("audit complete"));
}

TEST("selecting a nested sub-agent call resolves to that call's I/O") {
    TuiState s;
    s.apply_message(assistant_call("sa1", "spawn_subagent", R"({"prompt":"go"})"));
    s.push_subagent_text("running a command");
    s.push_subagent_activity("n1", "run_bash", R"({"command":"ls"})",
                             Status::Running, "");
    s.push_subagent_activity("n1", "", "", Status::Ok, "a.cpp\nb.cpp");

    // A nested call carries the group id as parent_id.
    NodeKey child_key{NodeKey::Pane::Activity, 0, "n1", "sa1"};
    s.select(child_key);

    auto dn = s.detail_node();
    CHECK(dn.has_value());
    const auto* a = s.find_activity(*dn);
    CHECK(a != nullptr);
    CHECK_EQ(a->name, std::string("run_bash"));
    CHECK_EQ(a->result_full, std::string("a.cpp\nb.cpp"));
}

TEST("group retains every call for investigation") {
    TuiState s;
    s.apply_message(assistant_call("sa1", "spawn_subagent", R"({"prompt":"go"})"));
    s.push_subagent_text("starting work");
    for (int i = 0; i < 20; ++i) {
        std::string id = "n" + std::to_string(i);
        s.push_subagent_activity(id, "list_dir", "{}", Status::Running, "");
        s.push_subagent_activity(id, "", "", Status::Ok, "done");
    }
    s.push_subagent_text("halfway done");
    for (int i = 20; i < 40; ++i) {
        std::string id = "n" + std::to_string(i);
        s.push_subagent_activity(id, "list_dir", "{}", Status::Running, "");
        s.push_subagent_activity(id, "", "", Status::Ok, "done");
    }
    const auto& g = s.subagents()[0];
    CHECK_EQ(g.turns.size(), std::size_t{2});
    CHECK_EQ(g.turns[0].calls.size(), std::size_t{20});
    CHECK_EQ(g.turns[1].calls.size(), std::size_t{20});
    CHECK(g.turns[0].calls.front().status == Status::Ok);
    CHECK(g.turns[1].calls.back().status == Status::Ok);
}

TEST("finishing spawn_subagent closes the group and strands no spinners") {
    TuiState s;
    s.apply_message(assistant_call("sa1", "spawn_subagent", R"({"prompt":"go"})"));
    s.push_subagent_text("let me run a command");
    s.push_subagent_activity("n1", "run_bash", "{}", Status::Running, "");
    // Sub-agent cancelled mid-call: the parent's spawn result arrives first.
    s.apply_message(tool_result("sa1", "sub-agent result"));
    CHECK(s.subagents()[0].status == Status::Ok);
    CHECK(s.subagents()[0].turns[0].calls[0].status == Status::Failed);  // no orphan spinner
    CHECK(!s.subagents()[0].expanded);  // completion folds an untouched group
    CHECK_EQ(s.subagents()[0].result_full, std::string("sub-agent result"));
}

TEST("two sequential sub-agents keep separate groups") {
    TuiState s;
    s.apply_message(assistant_call("sa1", "spawn_subagent", R"({"prompt":"first"})"));
    s.push_subagent_text("working on first");
    s.push_subagent_activity("a1", "read_file", "{}", Status::Running, "");
    s.apply_message(tool_result("sa1", "done one"));
    s.apply_message(assistant_call("sa2", "spawn_subagent", R"({"prompt":"second"})"));
    s.push_subagent_text("working on second");
    s.push_subagent_activity("b1", "list_dir", "{}", Status::Running, "");
    CHECK_EQ(s.subagents().size(), std::size_t{2});
    CHECK_EQ(s.subagents()[0].turns[0].calls.size(), std::size_t{1});
    CHECK_EQ(s.subagents()[1].turns[0].calls.size(), std::size_t{1});
    // The active group is the latest still-running one; first stayed closed.
    CHECK(s.subagents()[0].status == Status::Ok);
    CHECK(s.subagents()[1].status == Status::Running);
}

TEST("a user-toggled group stays unfolded on completion") {
    TuiState s;
    s.apply_message(assistant_call("sa1", "spawn_subagent", R"({"prompt":"x"})"));
    s.set_expanded("sa1", true);  // user takes control (even if already true)
    s.apply_message(tool_result("sa1", "done"));
    CHECK(s.subagents()[0].expanded);
}

TEST("set_expanded folds and unfolds; unknown id is a no-op") {
    TuiState s;
    s.apply_message(assistant_call("sa1", "spawn_subagent", R"({"prompt":"x"})"));
    s.set_expanded("sa1", false);
    CHECK(!s.subagents()[0].expanded);
    s.set_expanded("sa1", true);
    CHECK(s.subagents()[0].expanded);
    s.set_expanded("nope", false);  // must not crash or change anything
    CHECK(s.subagents()[0].expanded);
}

TEST("nav_nodes: chat pane lists every entry in order") {
    TuiState s;
    s.push_user("hi");
    s.apply_delta("answer", "");
    s.push_error("boom");
    auto nodes = s.nav_nodes(NodeKey::Pane::Chat);
    CHECK_EQ(nodes.size(), std::size_t{3});
    CHECK(nodes[0].pane == NodeKey::Pane::Chat);
    CHECK_EQ(nodes[0].chat_index, std::size_t{0});
    CHECK_EQ(nodes[2].chat_index, std::size_t{2});
}

TEST("nav_nodes: activity tree is flat — sub-agent calls are not nested") {
    TuiState s;
    s.apply_message(assistant_call("t1", "read_file", "{}"));
    s.apply_message(assistant_call("sa1", "spawn_subagent", R"({"prompt":"go"})"));
    s.push_subagent_text("going");
    s.push_subagent_activity("n1", "list_dir", "{}", Status::Running, "");
    s.push_subagent_text("next");
    s.push_subagent_activity("n2", "run_bash", "{}", Status::Running, "");
    s.apply_message(assistant_call("t2", "edit_file", "{}"));
    auto nodes = s.nav_nodes(NodeKey::Pane::Activity);
    // Only the parent's own three top-level calls — internal calls live in the
    // subagents pane now, and fold state no longer affects the activity walk.
    CHECK_EQ(nodes.size(), std::size_t{3});
    CHECK_EQ(nodes[0].id, std::string("t1"));
    CHECK_EQ(nodes[1].id, std::string("sa1"));
    CHECK_EQ(nodes[2].id, std::string("t2"));
    s.set_expanded("sa1", false);
    CHECK_EQ(s.nav_nodes(NodeKey::Pane::Activity).size(), std::size_t{3});
}

TEST("nav_nodes: subagents pane lists each group once, in order") {
    TuiState s;
    s.apply_message(assistant_call("sa1", "spawn_subagent", R"({"prompt":"first"})"));
    s.apply_message(tool_result("sa1", "done one"));
    s.apply_message(assistant_call("sa2", "spawn_subagent", R"({"prompt":"second"})"));
    auto nodes = s.nav_nodes(NodeKey::Pane::Subagents);
    CHECK_EQ(nodes.size(), std::size_t{2});
    CHECK(nodes[0].pane == NodeKey::Pane::Subagents);
    CHECK_EQ(nodes[0].id, std::string("sa1"));
    CHECK_EQ(nodes[1].id, std::string("sa2"));
}

TEST("select_next/prev walk the pane and stop at the ends") {
    TuiState s;
    s.apply_message(assistant_call("t1", "a", "{}"));
    s.apply_message(assistant_call("t2", "b", "{}"));
    CHECK(!s.selection().has_value());
    s.select_last(NodeKey::Pane::Activity);
    CHECK_EQ(s.selection()->id, std::string("t2"));
    CHECK(!s.select_next());  // already at the end; unchanged
    CHECK(s.select_prev());
    CHECK_EQ(s.selection()->id, std::string("t1"));
    CHECK(!s.select_prev());  // at the start; unchanged
    CHECK_EQ(s.selection()->id, std::string("t1"));
}

TEST("collapsing a group snaps an inside selection to the parent row") {
    TuiState s;
    s.apply_message(assistant_call("sa1", "spawn_subagent", R"({"prompt":"x"})"));
    s.push_subagent_text("going");
    s.push_subagent_activity("n1", "read_file", "{}", Status::Running, "");
    s.select(NodeKey{NodeKey::Pane::Activity, 0, "n1", "sa1"});
    s.set_expanded("sa1", false);
    CHECK_EQ(s.selection()->id, std::string("sa1"));
    CHECK(s.selection()->parent_id.empty());
}

TEST("detail_node: selection wins, else newest activity, else nothing") {
    TuiState s;
    CHECK(!s.detail_node().has_value());
    s.apply_message(assistant_call("t1", "a", "{}"));
    s.apply_message(assistant_call("t2", "b", "{}"));
    CHECK_EQ(s.detail_node()->id, std::string("t2"));  // live default: newest
    s.select(NodeKey{NodeKey::Pane::Activity, 0, "t1", ""});
    CHECK_EQ(s.detail_node()->id, std::string("t1"));
    s.clear_selection();
    CHECK_EQ(s.detail_node()->id, std::string("t2"));
}

TEST("find_activity resolves nested calls; stale keys resolve to null") {
    TuiState s;
    s.apply_message(assistant_call("sa1", "spawn_subagent", R"({"prompt":"x"})"));
    s.push_subagent_text("going");
    s.push_subagent_activity("n1", "read_file", "{}", Status::Running, "");
    NodeKey nested{NodeKey::Pane::Activity, 0, "n1", "sa1"};
    CHECK(s.find_activity(nested) != nullptr);
    CHECK_EQ(s.find_activity(nested)->name, std::string("read_file"));
    NodeKey stale{NodeKey::Pane::Activity, 0, "zz", ""};
    CHECK(s.find_activity(stale) == nullptr);
    CHECK(s.find_group("sa1") != nullptr);
    CHECK(s.find_group("zz") == nullptr);
}

TEST("selection survives appended activity (id-keyed, not positional)") {
    TuiState s;
    s.apply_message(assistant_call("t1", "a", "{}"));
    s.select_last(NodeKey::Pane::Activity);
    s.apply_message(assistant_call("t2", "b", "{}"));
    CHECK_EQ(s.selection()->id, std::string("t1"));
    CHECK(s.find_activity(*s.selection()) != nullptr);
}

TEST("toggle_reasoning flips the collapse flag") {
    TuiState s;
    CHECK(s.reasoning_collapsed());  // default: show the token estimate
    s.toggle_reasoning();
    CHECK(!s.reasoning_collapsed());
    s.toggle_reasoning();
    CHECK(s.reasoning_collapsed());
}

TEST("push_error appends a red error line and seals the run") {
    TuiState s;
    s.apply_delta("partial", "");
    s.push_error("boom");
    s.apply_delta("next", "");
    CHECK_EQ(s.chat().size(), std::size_t{3});
    CHECK(s.chat()[1].kind == Kind::ErrorLine);
    CHECK_EQ(s.chat()[1].text, std::string("boom"));
    CHECK(s.chat()[2].kind == Kind::Assistant);
}

TEST("ApprovalGate: allow-once unblocks the worker with Once") {
    ApprovalGate gate;
    Approval got = Approval::Deny;
    std::thread worker(
        [&] { got = gate.request(ToolCall{.id = "c1", .name = "run_bash", .arguments_json = R"({"cmd":"ls"})"}); });
    for (int i = 0; i < 1000 && !gate.pending(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    CHECK(gate.pending());
    auto pc = gate.pending_call();
    CHECK(pc.has_value());
    CHECK_EQ(pc->name, std::string("run_bash"));
    gate.answer(Approval::Once);
    worker.join();
    CHECK(got == Approval::Once);
    CHECK(!gate.pending());
}

TEST("ApprovalGate: always is delivered to the worker") {
    ApprovalGate gate;
    Approval got = Approval::Deny;
    std::thread worker([&] { got = gate.request(ToolCall{.id = "c1", .name = "edit_file", .arguments_json = "{}"}); });
    for (int i = 0; i < 1000 && !gate.pending(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    gate.answer(Approval::Always);
    worker.join();
    CHECK(got == Approval::Always);
}

TEST("ApprovalGate: release unblocks a pending request as Deny") {
    ApprovalGate gate;
    Approval got = Approval::Once;
    std::thread worker([&] { got = gate.request(ToolCall{.id = "c1", .name = "x", .arguments_json = "{}"}); });
    for (int i = 0; i < 1000 && !gate.pending(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    gate.release();
    worker.join();
    CHECK(got == Approval::Deny);
}

TEST("ApprovalGate: requests after release decline immediately") {
    ApprovalGate gate;
    gate.release();
    CHECK(gate.request(ToolCall{.id = "c1", .name = "x", .arguments_json = "{}"}) == Approval::Deny);
}

TEST("TuiState::load rebuilds chat prose and tool activity, no reasoning") {
    Conversation conv;
    conv.push_back(Message::system("system prompt"));
    conv.push_back(Message::user("do a thing"));
    {
        std::vector<ToolCall> calls;
        calls.push_back(ToolCall{.id = "c1", .name = "read_file", .arguments_json = "{\"path\":\"x\"}"});
        conv.push_back(Message::assistant("on it", std::move(calls)));
    }
    conv.push_back(Message::tool("c1", "the contents"));
    conv.push_back(Message::assistant("all done"));

    TuiState s;
    s.load(conv);

    // System is not shown; user + two assistant prose entries appear in order.
    const auto& chat = s.chat();
    int users = 0, assistants = 0, reasonings = 0;
    for (const auto& e : chat) {
        if (e.kind == Kind::User) ++users;
        if (e.kind == Kind::Assistant) ++assistants;
        if (e.kind == Kind::Reasoning) ++reasonings;
    }
    CHECK_EQ(users, 1);
    CHECK_EQ(assistants, 2);
    CHECK_EQ(reasonings, 0);

    // The tool call became an activity, flipped to Ok by its result.
    CHECK_EQ(s.activity().size(), std::size_t{1});
    if (!s.activity().empty()) {
        CHECK_EQ(s.activity()[0].name, std::string("read_file"));
        CHECK(s.activity()[0].status == Status::Ok);
        CHECK_EQ(s.activity()[0].result_full, std::string("the contents"));
    }
}

TEST("TuiState::load clears prior state") {
    TuiState s;
    s.apply_delta("stale", "");
    s.apply_message(assistant_call("old", "run_bash", "{}"));
    Conversation conv;
    conv.push_back(Message::user("fresh"));
    s.load(conv);
    CHECK_EQ(s.chat().size(), std::size_t{1});
    CHECK(s.chat()[0].kind == Kind::User);
    CHECK_EQ(s.chat()[0].text, std::string("fresh"));
    CHECK(s.activity().empty());
}

TEST("push_diff appends a Diff entry and seals the run") {
    TuiState s;
    s.apply_delta("partial", "");
    std::vector<DiffLine> d{{DiffLine::Op::Add, "new line"},
                            {DiffLine::Op::Del, "old line"}};
    s.push_diff("file.txt", d);
    s.apply_delta("after", "");  // next prose must be a fresh entry
    CHECK_EQ(s.chat().size(), std::size_t{3});
    CHECK(s.chat()[1].kind == Kind::Diff);
    CHECK_EQ(s.chat()[1].text, std::string("file.txt"));
    CHECK_EQ(s.chat()[1].diff.size(), std::size_t{2});
    CHECK(s.chat()[2].kind == Kind::Assistant);
    CHECK_EQ(s.chat()[2].text, std::string("after"));
}

TEST("human_tokens: exact below 1000, k-suffixed above") {
    CHECK_EQ(human_tokens(0), std::string("0"));
    CHECK_EQ(human_tokens(999), std::string("999"));
    CHECK_EQ(human_tokens(12300), std::string("12.3k"));
    CHECK_EQ(human_tokens(128000), std::string("128.0k"));
}

TEST("human_duration formats ms, seconds and minutes") {
    using std::chrono::milliseconds;
    CHECK_EQ(human_duration(milliseconds{0}), std::string("0ms"));
    CHECK_EQ(human_duration(milliseconds{417}), std::string("417ms"));
    CHECK_EQ(human_duration(milliseconds{1234}), std::string("1.2s"));
    CHECK_EQ(human_duration(milliseconds{59'440}), std::string("59.4s"));
    CHECK_EQ(human_duration(milliseconds{123'000}), std::string("2m03s"));
}

TEST("TuiState: usage starts absent and set_usage records it") {
    TuiState s;
    CHECK(!s.has_tokens());
    s.set_usage(1500);
    CHECK(s.has_tokens());
    CHECK_EQ(s.tokens(), 1500);
}

// ---- pasted-image chips (agent/image_chip.hpp) ---------------------------

TEST("image_chip: formats a pure-ASCII marker") {
    CHECK_EQ(image_chip(1), std::string("[img#1]"));
    CHECK_EQ(image_chip(42), std::string("[img#42]"));
}

TEST("extract_chip_ids: order preserved, deduplicated") {
    auto ids = extract_chip_ids("look at [img#2] then [img#1] and [img#2] again");
    CHECK_EQ(ids.size(), std::size_t{2});
    CHECK_EQ(ids[0], 2);
    CHECK_EQ(ids[1], 1);
}

TEST("extract_chip_ids: empty when none / malformed") {
    CHECK_EQ(extract_chip_ids("plain prose, no chips").size(), std::size_t{0});
    CHECK_EQ(extract_chip_ids("[img#] [img#x] [img#1 broken").size(),
             std::size_t{0});
}

TEST("extract_chip_ids: a broken (partially edited) chip detaches") {
    // The closing bracket was backspaced away -> no longer a valid handle.
    CHECK_EQ(extract_chip_ids("hi [img#3").size(), std::size_t{0});
}

TEST("chip_ending_at: matches a complete chip just left of the cursor") {
    std::string t = "a [img#7]";
    auto r = chip_ending_at(t, t.size());  // cursor right after ']'
    CHECK(r.has_value());
    CHECK_EQ(r->first, std::size_t{2});
    CHECK_EQ(r->second, t.size());
    CHECK_EQ(t.substr(r->first, r->second - r->first), std::string("[img#7]"));
}

TEST("chip_ending_at: no match mid-chip, at start, or off the bracket") {
    std::string t = "x [img#7] y";
    CHECK(!chip_ending_at(t, 0).has_value());        // cursor at very start
    CHECK(!chip_ending_at(t, 5).has_value());        // inside the chip
    CHECK(!chip_ending_at(t, t.size()).has_value()); // trailing " y", not ']'
    CHECK(!chip_ending_at("no chip here", 4).has_value());
}

TEST("chip_ending_at: picks the chip ending exactly at the cursor") {
    std::string t = "[img#1][img#2]";
    auto r1 = chip_ending_at(t, 7);  // after first chip's ']'
    CHECK(r1.has_value());
    CHECK_EQ(r1->first, std::size_t{0});
    CHECK_EQ(r1->second, std::size_t{7});
    auto r2 = chip_ending_at(t, t.size());  // after second chip's ']'
    CHECK(r2.has_value());
    CHECK_EQ(r2->first, std::size_t{7});
}

TEST("strip_chips: rewrites chips, leaves other text intact") {
    CHECK_EQ(strip_chips("explain [img#1] and [img#2]"),
             std::string("explain [image #1] and [image #2]"));
    CHECK_EQ(strip_chips("no chips at all"), std::string("no chips at all"));
    CHECK_EQ(strip_chips("[img#] stays, [img#9] goes"),
             std::string("[img#] stays, [image #9] goes"));
}

// --- sanitize_tui_text -------------------------------------------------------
// The sanitizer keeps the FTXUI grid width-predictable: every byte it lets
// through must occupy exactly the number of cells FTXUI computes for it, in
// any reasonable terminal. See tui_text.hpp.

TEST("sanitize_tui_text passes plain text through") {
    CHECK_EQ(sanitize_tui_text(""), std::string(""));
    CHECK_EQ(sanitize_tui_text("hello world\nsecond line"),
             std::string("hello world\nsecond line"));
    // CJK and width-table emoji are width-2 on both sides — kept verbatim.
    CHECK_EQ(sanitize_tui_text("日本語 ✅ \U0001F600"),
             std::string("日本語 ✅ \U0001F600"));
}

TEST("sanitize_tui_text strips ANSI escape sequences") {
    CHECK_EQ(sanitize_tui_text("\x1b[31mred\x1b[0m"), std::string("red"));
    CHECK_EQ(sanitize_tui_text("\x1b[1;38;5;196mX\x1b[m"), std::string("X"));
    CHECK_EQ(sanitize_tui_text("\x1b]0;title\x07rest"), std::string("rest"));
    CHECK_EQ(sanitize_tui_text("\x1b]8;;http://x\x1b\\link\x1b]8;;\x1b\\"),
             std::string("link"));
    CHECK_EQ(sanitize_tui_text("a\x1b"), std::string("a"));   // truncated ESC
    CHECK_EQ(sanitize_tui_text("a\x1b["), std::string("a"));  // truncated CSI
    CHECK_EQ(sanitize_tui_text("a\x1b(B z"), std::string("a z"));  // charset
}

TEST("sanitize_tui_text expands tabs and drops stray controls") {
    CHECK_EQ(sanitize_tui_text("a\tb"), std::string("a    b"));
    CHECK_EQ(sanitize_tui_text("line1\r\nline2\r"), std::string("line1\nline2"));
    CHECK_EQ(sanitize_tui_text("a\x07\x08\x0b\x0c\x7f"), std::string("a"));
}

TEST("sanitize_tui_text drops width-ambiguous joiners and selectors") {
    // VS16 flips U+26A0 to a two-cell emoji in many terminals while FTXUI
    // keeps counting one — the classic drifting-separator glyph.
    CHECK_EQ(sanitize_tui_text("\u26A0\uFE0F"), std::string("\u26A0"));
    // ZWJ gets its own width-1 cell in FTXUI but renders zero-wide.
    CHECK_EQ(sanitize_tui_text("\U0001F468\u200D\U0001F469"),
             std::string("\U0001F468\U0001F469"));
    CHECK_EQ(sanitize_tui_text("\U0001F44D\U0001F3FC"),
             std::string("\U0001F44D"));               // skin-tone modifier
    CHECK_EQ(sanitize_tui_text("1\uFE0F\u20E3"), std::string("1"));  // keycap
    CHECK_EQ(sanitize_tui_text("a\u200Bb\u00ADc"), std::string("abc"));
    CHECK_EQ(sanitize_tui_text("x\u202Ey\u2066z"), std::string("xyz"));
    CHECK_EQ(sanitize_tui_text("\uFEFFbom"), std::string("bom"));
}

TEST("sanitize_tui_text keeps genuinely zero-width combining marks") {
    CHECK_EQ(sanitize_tui_text("é"), std::string("é"));
}

TEST("sanitize_tui_text drops invalid UTF-8 bytes") {
    CHECK_EQ(sanitize_tui_text("a\x80z"), std::string("az"));
    CHECK_EQ(sanitize_tui_text("a\xc3"), std::string("a"));
    CHECK_EQ(sanitize_tui_text("a\xe2\x82"), std::string("a"));
    CHECK_EQ(sanitize_tui_text(std::string_view("a\xc0\xafz", 4)),
             std::string("az"));                       // overlong slash
    CHECK_EQ(sanitize_tui_text("a\xed\xa0\x80z"), std::string("az"));  // surrogate
}

TEST("TuiState sanitizes model and tool text at ingress") {
    TuiState s;
    s.push_user("hi\t\x1b[31mthere");
    CHECK_EQ(s.chat().back().text, std::string("hi    there"));
    s.apply_delta("warn \u26A0\uFE0F", "");
    CHECK_EQ(s.chat().back().text, std::string("warn \u26A0"));
    s.apply_message(assistant_call("c1", "run_bash", "{\"cmd\":\"x\u200D\"}"));
    CHECK_EQ(s.activity().back().args, std::string("x"));  // ZWJ stripped
    s.apply_message(tool_result("c1", "out\x1b[0;32mgreen"));
    CHECK_EQ(s.activity().back().result_full, std::string("outgreen"));
    s.push_info("note\u200B");
    CHECK_EQ(s.chat().back().text, std::string("note"));
    s.push_error("err\tx");
    CHECK_EQ(s.chat().back().text, std::string("err    x"));
}

// --- patched FTXUI behaviour -------------------------------------------------
// These exercise the fixes cmake/PatchFTXUI.cmake applies to the fetched
// FTXUI sources; they fail against a pristine v6.1.9.

TEST("FTXUI width table covers Unicode 14-16 wide emoji") {
    CHECK_EQ(ftxui::string_width("\U0001FAE0"), 2);  // melting face
    CHECK_EQ(ftxui::string_width("\U0001FAF6"), 2);  // heart hands
    CHECK_EQ(ftxui::string_width("\U0001F979"), 2);  // face holding back tears
    CHECK_EQ(ftxui::string_width("\U0001FA89"), 2);  // harp (Unicode 16)
    // Established widths are untouched.
    CHECK_EQ(ftxui::string_width("\U0001F600"), 2);
    CHECK_EQ(ftxui::string_width("中"), 2);
    CHECK_EQ(ftxui::string_width("a"), 1);
}

TEST("load clears selection and rebuilds full I/O from history") {
    TuiState s;
    s.apply_message(assistant_call("t1", "read_file", "{}"));
    s.select(NodeKey{NodeKey::Pane::Activity, 0, "t1", ""});
    Conversation conv;
    conv.push_back(Message::user("hi"));
    conv.push_back(assistant_call("t9", "run_bash", R"({"cmd":"ls"})"));
    conv.push_back(tool_result("t9", "a.txt\nb.txt"));
    s.load(conv);
    CHECK(!s.selection().has_value());
    CHECK_EQ(s.activity().size(), std::size_t{1});
    CHECK_EQ(s.activity()[0].args_full, std::string(R"({"cmd":"ls"})"));
    CHECK_EQ(s.activity()[0].result_full, std::string("a.txt\nb.txt"));
    CHECK_EQ(s.detail_node()->id, std::string("t9"));  // fallback: newest
}

TEST("a nav-stale selection cannot step but still resolves its data") {
    // Selection inside a group whose spawn_subagent completes: the group
    // auto-folds WITHOUT snapping (only user folds snap), so the node leaves
    // the nav order while its data lives on.
    TuiState s;
    s.apply_message(assistant_call("sa1", "spawn_subagent", R"({"prompt":"x"})"));
    s.push_subagent_text("going");
    s.push_subagent_activity("n1", "read_file", "{}", Status::Running, "");
    s.select(NodeKey{NodeKey::Pane::Activity, 0, "n1", "sa1"});
    s.apply_message(tool_result("sa1", "done"));   // completes + auto-folds
    CHECK(!s.subagents()[0].expanded);
    CHECK(!s.select_next());                       // stale in nav order
    CHECK(!s.select_prev());
    CHECK(s.find_activity(*s.selection()) != nullptr);  // data still resolves
    CHECK_EQ(s.detail_node()->id, std::string("n1"));   // detail still shows it
}

TEST("FTXUI clipped fullwidth glyph does not eat the neighbouring pixel") {
    using namespace ftxui;
    // "ああ" needs 4 cells but gets 3: the second あ would sit in the last
    // column with its reserved cell clipped away. Unpatched, Screen::ToString
    // then skips the very pixel holding the separator "|".
    auto doc = hbox({text("ああ") | size(WIDTH, EQUAL, 3), text("|")});
    Screen scr(4, 1);
    Render(scr, doc);
    CHECK_EQ(scr.PixelAt(3, 0).character, std::string("|"));
    // The clipped cell keeps the default (empty => printed as a space) pixel
    // rather than half of the glyph.
    CHECK_EQ(scr.PixelAt(2, 0).character, std::string(""));
    CHECK(scr.ToString().find('|') != std::string::npos);
}

// --- P8.3: pure event sub-decisions extracted from the CatchEvent loop -------

TEST("next_zone cycles Input -> Activity -> Chat -> Subagents -> Input") {
    CHECK(next_zone(Zone::Input) == Zone::Activity);
    CHECK(next_zone(Zone::Activity) == Zone::Chat);
    CHECK(next_zone(Zone::Chat) == Zone::Subagents);
    CHECK(next_zone(Zone::Subagents) == Zone::Input);
}

TEST("classify_row_click folds, maximizes, or selects like the inline loop") {
    NodeKey row;
    row.pane = NodeKey::Pane::Activity;
    row.id = "tc1";

    // Expander glyph click always folds, regardless of selection.
    CHECK(classify_row_click(row, /*expander=*/true, std::nullopt) ==
          RowClick::Fold);
    CHECK(classify_row_click(row, /*expander=*/true, row) == RowClick::Fold);

    // No selection (or a different row selected) => select the clicked row.
    CHECK(classify_row_click(row, /*expander=*/false, std::nullopt) ==
          RowClick::Select);
    NodeKey other;
    other.pane = NodeKey::Pane::Activity;
    other.id = "tc2";
    CHECK(classify_row_click(row, /*expander=*/false, other) ==
          RowClick::Select);

    // Clicking the already-selected row inspects it (maximize).
    CHECK(classify_row_click(row, /*expander=*/false, row) ==
          RowClick::Maximize);

    // Chat rows compare by chat_index, not id: same index => maximize.
    NodeKey chat;
    chat.pane = NodeKey::Pane::Chat;
    chat.chat_index = 3;
    NodeKey chat_same;
    chat_same.pane = NodeKey::Pane::Chat;
    chat_same.chat_index = 3;
    CHECK(classify_row_click(chat, false, chat_same) == RowClick::Maximize);
    NodeKey chat_diff;
    chat_diff.pane = NodeKey::Pane::Chat;
    chat_diff.chat_index = 4;
    CHECK(classify_row_click(chat, false, chat_diff) == RowClick::Select);
}

// --- Turn-based sub-agent behaviour -----------------------------------------

TEST("sub-agent with text and tool calls across multiple turns") {
    TuiState s;
    s.apply_message(assistant_call("sa1", "spawn_subagent", R"({"prompt":"go"})"));
    // Turn 1: text + tool call
    s.push_subagent_text("Let me read the file first.");
    s.push_subagent_activity("n1", "read_file", "{}", Status::Running, "");
    s.push_subagent_activity("n1", "", "", Status::Ok, "file contents");
    // Turn 2: text + two tool calls
    s.push_subagent_text("Now I'll edit and run.");
    s.push_subagent_activity("n2", "edit_file", "{}", Status::Running, "");
    s.push_subagent_activity("n2", "", "", Status::Ok, "edited");
    s.push_subagent_activity("n3", "run_bash", "{}", Status::Running, "");
    s.push_subagent_activity("n3", "", "", Status::Ok, "done");
    // Turn 3: final text-only answer
    s.push_subagent_text("All done, the build passed.");
    s.apply_message(tool_result("sa1", "All done, the build passed."));
    const auto& g = s.subagents()[0];
    CHECK_EQ(g.turns.size(), std::size_t{3});
    // Turn 0: text + 1 call
    CHECK_EQ(g.turns[0].assistant_text, std::string("Let me read the file first."));
    CHECK_EQ(g.turns[0].calls.size(), std::size_t{1});
    // Turn 1: text + 2 calls
    CHECK_EQ(g.turns[1].assistant_text, std::string("Now I'll edit and run."));
    CHECK_EQ(g.turns[1].calls.size(), std::size_t{2});
    // Turn 2: text only, no calls
    CHECK_EQ(g.turns[2].assistant_text, std::string("All done, the build passed."));
    CHECK_EQ(g.turns[2].calls.size(), std::size_t{0});
}

TEST("sub-agent that fails mid-run shows last text and error in detail_plain_text") {
    TuiState s;
    s.apply_message(assistant_call("sa1", "spawn_subagent", R"({"prompt":"go"})"));
    s.push_subagent_text("Trying something risky.");
    s.push_subagent_activity("n1", "run_bash", "{}", Status::Running, "");
    // The parent's spawn result arrives as failed.
    s.apply_message(tool_result("sa1", "sub-agent error", /*failed=*/true));
    const auto& g = s.subagents()[0];
    CHECK_EQ(g.turns.size(), std::size_t{1});
    CHECK_EQ(g.turns[0].assistant_text, std::string("Trying something risky."));
    CHECK_EQ(g.turns[0].calls.size(), std::size_t{1});
    CHECK(g.status == Status::Failed);
    CHECK_EQ(g.result_full, std::string("sub-agent error"));
    // The last turn's assistant_text differs from result_full, so it should
    // be shown (not deduped against RESULT).
    CHECK(s.should_show_last_text(g, 0));
}

TEST("sub-agent that calls tools without preceding text creates empty turn") {
    TuiState s;
    s.apply_message(assistant_call("sa1", "spawn_subagent", R"({"prompt":"go"})"));
    // Sub-agent calls a tool without any assistant text first.
    s.push_subagent_activity("n1", "list_dir", "{}", Status::Running, "");
    s.push_subagent_activity("n1", "", "", Status::Ok, "dir listing");
    s.apply_message(tool_result("sa1", "done"));
    const auto& g = s.subagents()[0];
    CHECK_EQ(g.turns.size(), std::size_t{1});
    CHECK(g.turns[0].assistant_text.empty());  // empty fallback turn
    CHECK_EQ(g.turns[0].calls.size(), std::size_t{1});
    CHECK(g.turns[0].calls[0].status == Status::Ok);
}

// --- Running-subagent dashboard (detail pane) -------------------------------

TEST("spawn_subagent records the model from its args") {
    TuiState s;
    s.set_parent_model("claude-opus-4-8");
    s.apply_message(assistant_call(
        "sa1", "spawn_subagent",
        R"({"prompt":"go","model":"claude-sonnet-4-6"})"));
    // Explicit arg wins over the parent fallback.
    CHECK_EQ(s.subagents()[0].model, std::string("claude-sonnet-4-6"));
}

TEST("spawn_subagent falls back to the parent model when no model arg") {
    TuiState s;
    s.set_parent_model("claude-opus-4-8");
    s.apply_message(assistant_call("sa1", "spawn_subagent", R"({"prompt":"go"})"));
    CHECK_EQ(s.subagents()[0].model, std::string("claude-opus-4-8"));
}

TEST("spawn_subagent model is empty with no arg and no parent model") {
    TuiState s;
    s.apply_message(assistant_call("sa1", "spawn_subagent", R"({"prompt":"go"})"));
    CHECK_EQ(s.subagents()[0].model, std::string(""));
}

TEST("detail_node resolves a focused subagents-pane selection to the group") {
    // The maximize/copy path goes through detail_node()/render_detail; a
    // Subagents-pane key must resolve so Enter inspects the group's history.
    TuiState s;
    s.apply_message(assistant_call("sa1", "spawn_subagent", R"({"prompt":"go"})"));
    s.select(NodeKey{NodeKey::Pane::Subagents, 0, "sa1", ""});
    auto dn = s.detail_node();
    CHECK(dn.has_value());
    CHECK(dn->pane == NodeKey::Pane::Subagents);
    CHECK_EQ(dn->id, std::string("sa1"));
    // A stale Subagents key falls back to the newest activity instead.
    s.select(NodeKey{NodeKey::Pane::Subagents, 0, "nope", ""});
    CHECK_EQ(s.detail_node()->id, std::string("sa1"));  // the spawn row
}

TEST("show_subagents_browser: zone focus and empty selection force the browser") {
    TuiState s;
    CHECK(show_subagents_browser(Zone::Subagents, s));  // zone wins outright
    CHECK(show_subagents_browser(Zone::Activity, s));   // nothing selected
}

TEST("show_subagents_browser: a plain selected call shows its own I/O") {
    TuiState s;
    s.apply_message(assistant_call("c1", "read_file", R"({"path":"x"})"));
    s.select(NodeKey{NodeKey::Pane::Activity, 0, "c1", ""});
    CHECK(!show_subagents_browser(Zone::Activity, s));
    // ...but a stale selection falls back to the browser.
    s.select(NodeKey{NodeKey::Pane::Activity, 0, "gone", ""});
    CHECK(show_subagents_browser(Zone::Activity, s));
}

TEST("show_subagents_browser: a selected subagent group shows the browser") {
    TuiState s;
    s.apply_message(assistant_call("sa1", "spawn_subagent", R"({"prompt":"go"})"));
    s.select(NodeKey{NodeKey::Pane::Activity, 0, "sa1", ""});  // tree group row
    CHECK(show_subagents_browser(Zone::Activity, s));
    s.select(NodeKey{NodeKey::Pane::Subagents, 0, "sa1", ""});  // pane focus
    CHECK(show_subagents_browser(Zone::Subagents, s));
}
