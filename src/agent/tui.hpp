#ifndef MOOCODE_TUI_HPP
#define MOOCODE_TUI_HPP

// Full-screen two-pane terminal UI for the interactive session, built on FTXUI.
// The view/controller live in tui.cpp (the only place that includes FTXUI);
// this header exposes the entry point plus the two screen-free units the loop
// is built around — TuiState (the render model) and ApprovalGate (the worker↔UI
// tool-approval handshake) — so both are unit-testable without a terminal.

#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "agent/diff.hpp"            // DiffLine (needed by value in ChatEntry::diff)
#include "agent/permissions.hpp"     // Approval (used by value in ApprovalGate)
#include "agent/subagent_types.hpp"  // SubagentActivityFn, SubagentTextFn, SubagentActivityStatus
#include "agent/types.hpp"           // ToolCall, Message (needed by value)

// Forward declarations — these types appear only as references/pointers
// in tui.hpp; the full definitions are needed only in tui.cpp.
namespace moocode {
class Agent;
struct FileChange;
class QuestionGate;
class Permissions;
}  // namespace moocode

namespace moocode {

// Static text shown in the status bar, plus the home dir used for auto-save and
// the resume/continue/rewind pickers (empty => persistence disabled).
struct TuiInfo {
    std::string model;
    std::string base_url;
    std::string cwd;
    int context_window = 0;  // 0 => token count only; else "used / window"
    std::string home;        // ~/.moo (or $MOOCODE_HOME); "" disables persistence
    std::string provider;    // backend wire format label ("openai"/"anthropic")
    std::string notice;      // optional one-time info line shown in the chat log at startup
    std::string api_key;     // current session key (for /provider save); never rendered
    std::string profile;     // active profile name; "" => none. Never holds the key.
    bool debug = false;      // --debug: show incoming mouse events in the status bar
};

// Stable identity of one navigable node. Chat nodes are identified by entry
// index (the chat log is append-only; load() resets selection). Activity nodes
// are identified by tool_call_id; a subagent's internal call also carries its
// owning group id so ids cannot collide across groups.
struct NodeKey {
    enum class Pane { Chat, Activity, Subagents };
    Pane pane = Pane::Activity;
    std::size_t chat_index = 0;  // Chat only
    std::string id;              // Activity: tool_call_id of the row
    std::string parent_id;       // Activity: owning group id; "" => top level
    friend bool operator==(const NodeKey&, const NodeKey&) = default;
};

// Keyboard-focus zone in the interactive UI: the input line, the activity pane,
// the chat pane, or the subagents pane. Tab cycles through them in this order.
enum class Zone { Input, Activity, Chat, Subagents };

// Next focus zone for a Tab press: Input → Activity → Chat → Subagents → Input.
// Pure, so the cycle order is unit-testable without a terminal.
Zone next_zone(Zone z);

// What a left-click on a navigable row should do, decided from the row's key,
// whether the click landed on the fold expander glyph, and the current
// selection. Pure (no FTXUI, no state mutation) so the event loop just applies
// the verdict. A click on the already-selected row maximizes the detail pane;
// an expander click folds; any other row click selects it.
enum class RowClick { Select, Maximize, Fold };
RowClick classify_row_click(const NodeKey& row, bool expander,
                            const std::optional<NodeKey>& selection);

// The render model: an ordered conversation log (left pane) and tool-activity
// log (right pane), mutated by the agent's streaming callbacks and read by the
// renderer. Pure data + transitions — no FTXUI, no I/O, no locking (the
// controller guards it with its own mutex). All transitions are append-only
// except activity status updates and the reasoning-collapse flag.
class TuiState {
public:
    enum class ChatKind { User, Assistant, Reasoning, ErrorLine, Diff, Info, ToolUse };
    struct ChatEntry {
        ChatKind kind;
        std::string text;            // Diff: the file path (header line);
                                     // Info: a single-line annotation;
                                     // ToolUse: the tool_call_id — the matching
                                     // Activity (looked up by id) is the source
                                     // of truth for name/args/status/output.
        std::vector<DiffLine> diff;  // populated only for ChatKind::Diff
        std::size_t word_seed = 0;   // Reasoning: stable busy-word selector
    };

    enum class Status { Running, Ok, Failed };
    struct Activity {
        std::string id;    // tool_call_id, used to match the later result
        std::string name;  // tool name
        std::string args;  // tree-row summary: the most informative arg
                           // (tool_arg_summary), one line, never wraps
        Status status = Status::Running;
        std::string args_full;    // sanitized full args (capped), detail pane
        std::string result_full;  // sanitized full result (capped), detail pane
        std::chrono::steady_clock::time_point started{};   // stamped at call
        std::chrono::steady_clock::time_point finished{};  // stamped at result
    };

    // One turn of a sub-agent's internal conversation: an assistant text
    // response (the model's prose before any tool calls this turn) followed by
    // zero or more tool calls with their results.
    struct SubagentTurn {
        // Assistant prose emitted before the tool calls of this turn.
        // Empty when the model called tools without saying anything. Newlines
        // are preserved so paragraph_block() can handle wrapping. Byte-capped
        // at 128 KiB via full_text() on insertion.
        std::string assistant_text;

        // Tool calls made in this turn, in order. Empty for the final
        // text-only answer (the answer is held in assistant_text; the group's
        // result_full is the canonical copy for the RESULT section).
        std::vector<Activity> calls;
    };

    // One spawned sub-agent, shown as a foldable subtree under its
    // spawn_subagent row. Internal calls are recorded here (not in `activity_`)
    // so the top level stays a flat view of the parent's own calls. Every call
    // is retained for investigation; per-entry full-text caps bound memory.
    struct SubagentGroup {
        std::string id;           // spawn_subagent tool_call_id (parent-side)
        std::string label;        // short task description (from the prompt arg)
        std::string model;        // e.g. "claude-opus-4-8"; set at creation, fixed
        std::string prompt_full;  // full prompt (capped), group detail view
        std::string result_full;  // final spawn_subagent result (capped)
        Status status = Status::Running;
        bool expanded = true;      // tree fold state
        bool user_toggled = false; // user took control; completion won't fold
        std::vector<SubagentTurn> turns;  // every internal turn, in order
    };

    // Append a user turn. Seals any in-progress assistant/reasoning run.
    void push_user(std::string text);

    // Fold one streamed fragment in: `answer` extends the trailing assistant
    // entry (starting one if needed); `reasoning` extends the trailing reasoning
    // entry with newlines collapsed to spaces. Either may be empty.
    void apply_delta(std::string_view answer, std::string_view reasoning);

    // Fold an appended history message in. An assistant turn seals the current
    // prose run and adds one Running activity per tool_call; a tool-role message
    // flips the matching activity (by tool_call_id) to Ok/Failed with a preview.
    void apply_message(const Message& m);

    // Append a red error line. Seals any in-progress run.
    void push_error(std::string text);

    // Append a dimmed info line (e.g. "📎 2 file(s) attached: a.cpp, b.md").
    // Seals any in-progress run so the note appears as its own entry.
    void push_info(std::string text);

    // Append a colored file-diff block (path header + the line diff). Seals any
    // in-progress run so the diff appears as its own entry.
    void push_diff(std::string path, std::vector<DiffLine> diff);

    // Rebuild the whole render model from a loaded conversation (for /resume,
    // /continue, /rewind): clears chat + activity, then replays user/assistant
    // prose as chat entries and tool_calls/results as activities (matched by id).
    // Reasoning is never persisted, so no reasoning entries appear.
    void load(const Conversation& conv);

    void toggle_reasoning() { reasoning_collapsed_ = !reasoning_collapsed_; }
    // Latch a fresh busy-word seed on the idle→running edge so the status-bar
    // word stays stable for the whole turn.
    void set_running(bool r) {
        if (r && !running_) busy_seed_ = chat_.size();
        running_ = r;
    }
    void set_usage(int tokens) { tokens_ = tokens; has_tokens_ = true; }

    // The parent agent's current model, used as the fallback model for a
    // spawn_subagent group when the call carries no explicit "model" arg.
    // Set whenever the live model changes (and before each turn).
    void set_parent_model(std::string m) { parent_model_ = std::move(m); }
    const std::string& parent_model() const { return parent_model_; }
    int tokens() const { return tokens_; }
    bool has_tokens() const { return has_tokens_; }

    // Active code-block colour scheme, read by the renderer.
    void set_syntax_theme(SyntaxTheme t) { syntax_theme_ = t; }
    SyntaxTheme syntax_theme() const { return syntax_theme_; }

    const std::vector<ChatEntry>& chat() const { return chat_; }
    const std::vector<Activity>& activity() const { return activity_; }
    const std::vector<SubagentGroup>& subagents() const { return subagents_; }
    bool running() const { return running_; }
    bool reasoning_collapsed() const { return reasoning_collapsed_; }
    // True while the trailing chat entry is a reasoning block still receiving
    // deltas; false once sealed or before any reasoning has arrived.
    bool has_active_reasoning() const { return reasoning_open_; }
    std::size_t busy_seed() const { return busy_seed_; }

    // Called by the subagent activity callback (worker thread, state_mtx held).
    // Routes a sub-agent's own tool call into the current turn of the active
    // SubagentGroup: a Running status appends a call; Ok/Failed searches all
    // turns for the matching call by id. `args`/`result` are raw — formatting
    // is done here.
    void push_subagent_activity(std::string id, std::string name,
                                std::string args, Status status,
                                std::string result);

    // Called from the sub-agent text callback (worker thread, state_mtx held).
    // Creates a new SubagentTurn in the active group with the assistant's prose.
    // No-op when no group is running. Text is sanitized and capped at 128 KiB.
    void push_subagent_text(std::string text);

    // Flip any sub-agent calls still Running to Failed. Called when a
    // spawn_subagent completes (cancelled or finished), so no stranded spinners
    // remain. `id` names the finished group, or "" to sweep every group.
    void resolve_subagent_orphans(std::string_view id = {});

    // Fold state of a spawn_subagent group; marks the group user-controlled so
    // completion no longer auto-folds it. Unknown id => no-op.
    void set_expanded(std::string_view group_id, bool expanded);

    // --- Selection / navigation (pure; controller drives, renderer reads) ---

    // Flattened navigation order of one pane. Chat: every entry, in order.
    // Activity: each top-level call, an expanded spawn_subagent group's
    // internal calls directly after their parent row. Deterministic.
    std::vector<NodeKey> nav_nodes(NodeKey::Pane pane) const;

    const std::optional<NodeKey>& selection() const { return selection_; }
    void select(NodeKey k) { selection_ = std::move(k); }
    void clear_selection() { selection_.reset(); }

    // Move selection one step within its pane. False (and no change) at a
    // boundary, when nothing is selected, or when the key no longer resolves.
    bool select_next();
    bool select_prev();

    // Select the newest node of `pane` (used when a zone is entered with no
    // usable selection). No-op when the pane is empty.
    void select_last(NodeKey::Pane pane);

    // The node the detail pane should show: the selection when it still
    // resolves, else the most recent top-level activity, else nullopt.
    std::optional<NodeKey> detail_node() const;

    // Resolve a key to its row; nullptr when it no longer exists.
    const Activity* find_activity(const NodeKey& k) const;
    const SubagentGroup* find_group(std::string_view id) const;

    // Shared by detail_for_group and detail_plain_text: when true, the last
    // turn's assistant_text should be shown as an ASSISTANT block rather than
    // silently deduped against result_full in the RESULT section.
    // pre: turn_idx < g.turns.size() && turn_idx is the last turn.
    bool should_show_last_text(const SubagentGroup& g,
                               std::size_t turn_idx) const;

private:
    std::vector<ChatEntry> chat_;
    std::vector<Activity> activity_;
    std::vector<SubagentGroup> subagents_;
    std::optional<NodeKey> selection_;  // navigable-node selection (or none)
    bool running_ = false;
    bool reasoning_collapsed_ = true;  // default: show token estimate, F2 expands
    int tokens_ = 0;
    bool has_tokens_ = false;
    std::size_t busy_seed_ = 0;    // status-bar busy-word selector, per turn
    std::string parent_model_;     // fallback model for spawn_subagent groups
    bool answer_open_ = false;     // trailing chat entry is an extendable answer
    bool reasoning_open_ = false;  // trailing chat entry is an extendable reasoning
    SyntaxTheme syntax_theme_ = SyntaxTheme::Default;  // code-block colours

    void seal();  // end any in-progress answer/reasoning run
};

// True when the lower pane should show the foldable subagents browser rather
// than a single node's I/O: the subagents zone is focused, nothing is selected,
// or the selection points at a subagent group. A plain (non-subagent) activity
// row or a chat entry yields false. Pure, so the choice is unit-testable.
bool show_subagents_browser(Zone zone, const TuiState& st);

// Compact token count for the status bar: exact below 1000, else one-decimal
// "k" (e.g. 12300 -> "12.3k"). pre: n >= 0.
std::string human_tokens(int n);

// Compact duration for detail headers: "417ms" under 1 s, "1.2s" under 60 s,
// else "2m03s". pre: ms >= 0.
std::string human_duration(std::chrono::milliseconds ms);

// The last `n` lines of `text`, trailing blank lines dropped, joined with '\n'.
// Empty when `text` is empty or all-blank. Used for the gray output preview
// under an inline tool-use row in the chat pane. Pure, so it is unit-testable.
std::string last_lines(std::string_view text, std::size_t n);

// One-line tree-row summary of a tool call's JSON arguments: the value of the
// most informative key ("path", "cmd", "url", …), else the raw args. Sanitized
// and capped so it always renders as a single non-wrapping row.
std::string tool_arg_summary(std::string_view arguments_json);

// Synchronises a blocking worker-thread tool-approval request with the UI
// thread's modal. The worker calls request() and blocks; the UI sees pending()
// and calls answer(); release() unblocks any waiter as Deny (shutdown).
class ApprovalGate {
public:
    // Worker thread: publish `tc`, fire the request notifier, and block until
    // the UI answers or the gate is released. Returns the user's choice (Deny
    // if released). post: pending() is false on return.
    Approval request(ToolCall tc);

    // UI thread: a call is awaiting a decision / its details.
    bool pending() const;
    // Snapshot the pending call, if any. Returns nullopt when nothing is
    // awaiting — callers must not assume a default-constructed ToolCall.
    std::optional<ToolCall> pending_call() const;

    // UI thread: resolve the pending request.
    void answer(Approval a);

    // Unblock any pending request as Deny; future requests also Deny.
    void release();

    // Callback fired (once, unlocked) when a request begins waiting, so the
    // controller can post a redraw to raise the modal.
    void on_request(std::function<void()> fn) { notify_ = std::move(fn); }

private:
    // Held for the whole of request() so concurrent approvals (tool calls now
    // run in parallel, and sibling sub-agents share one gate — see Agent::run)
    // queue behind one another instead of clobbering the single call slot: one
    // modal is shown at a time.
    std::mutex serialize_;
    mutable std::mutex m_;
    std::condition_variable cv_;
    std::optional<ToolCall> call_;
    std::optional<Approval> answer_;
    bool released_ = false;
    std::function<void()> notify_;
};

// Run the interactive two-pane TUI to completion. Installs on_delta/on_message/
// on_usage on `agent`; when `perms` is non-null, also installs an on_approve
// gate that prompts via a modal for any tool not already allowed. `perms` null
// => no gating (e.g. --yes). When `sink` is non-null it is repointed at the
// TUI's file-change handler so write_file/edit_file render colored diffs in the
// log (the tool closures call through *sink). Returns a process exit code.
// pre: stdout is a tty.
int run_tui(Agent& agent, Permissions* perms, TuiInfo info,
            std::shared_ptr<std::function<void(const FileChange&)>> sink = {},
            QuestionGate* question_gate_ptr = {},
            std::shared_ptr<SubagentActivityFn> on_subagent_activity = {},
            std::shared_ptr<SubagentTextFn> on_subagent_text = {});

}  // namespace moocode

#endif  // MOOCODE_TUI_HPP
