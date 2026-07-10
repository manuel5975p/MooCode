#include "agent/tui.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <deque>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <queue>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#ifndef _WIN32
#include <fcntl.h>   // open (OSC 52 clipboard write to /dev/tty)
#include <unistd.h>  // write, close, STDOUT_FILENO
#endif

#include "agent/anthropic_provider.hpp"  // ProviderKind, provider_kind_name
#include "agent/agent.hpp"
#include "agent/builtin_tools.hpp"
#include "agent/image_chip.hpp"
#include "agent/paste_chip.hpp"        // collapsed-paste chip helpers
#include "agent/image_util.hpp"
#include "agent/json_util.hpp"  // parse spawn_subagent args for the group label
#include "agent/mentions.hpp"
#include "agent/openai_provider.hpp"  // openai_model_likely_reasoning
#include "agent/persist.hpp"
#include "agent/platform.hpp"          // user_home, console helpers
#include "agent/provider_factory.hpp"  // ProviderConnection, make_provider
#include "agent/question_tool.hpp"
#include "agent/strutil.hpp"           // to_lower
#include "agent/settings_editor.hpp"  // SettingsForm, FieldDef, FormAction
#include "agent/syntax_highlight.hpp"  // highlight_block, language_from_tag
#include "agent/skills.hpp"            // SkillRegistry, /skill command
#include "agent/tui_glyphs.hpp"        // glyphify, glyph mode
#include "agent/tui_platform.hpp"      // native clipboard, console setup
#include "agent/tui_slash.hpp"         // parse_effort/_thinking/_temp/_theme
#include "agent/tui_text.hpp"          // sanitize_tui_text
#include "agent/tui_worker.hpp"        // TuiWorker, WorkItem

#include <ftxui/component/component.hpp>
#include <ftxui/component/component_options.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/box.hpp>
#include <ftxui/screen/color.hpp>
#include <ftxui/screen/terminal.hpp>

namespace moocode {

namespace {

// Collapse runs of newlines/CRs into single spaces (reasoning is shown as one
// flowing block, matching the original CLI behaviour).
std::string collapse_newlines(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) out += (c == '\n' || c == '\r') ? ' ' : c;
    return out;
}

// Full-fidelity text for the detail pane: sanitized like everything entering
// the render model, middle-elided only past kFullCap so memory stays bounded
// while both ends survive. Newlines are preserved.
constexpr std::size_t kFullCap = 128 * 1024;
std::string full_text(std::string_view s) {
    return sanitize_tui_text(elide_middle(s, kFullCap, (kFullCap - 3) / 2));
}

// Full prompt text of a spawn_subagent call (fallback: raw args), for the
// group's detail view. Unlike subagent_label, not clipped to one line.
std::string subagent_prompt(std::string_view arguments_json) {
    if (auto j = json::parse(arguments_json); j)
        if (auto p = json::get_string_opt(*j, "prompt"); p && *p && !(*p)->empty())
            return **p;
    return std::string(arguments_json);
}

// Model a spawn_subagent group runs under: the call's explicit `model` arg when
// present and non-empty, else `fallback` (the parent agent's current model).
// Pre: arguments_json is the raw JSON from the tool call.
std::string subagent_model(std::string_view arguments_json,
                           std::string_view fallback) {
    if (auto j = json::parse(arguments_json); j)
        if (auto m = json::get_string_opt(*j, "model"); m && *m && !(*m)->empty())
            return sanitize_tui_text(**m);
    return sanitize_tui_text(std::string(fallback));
}

// Short, human label for a spawn_subagent group, taken from its `prompt`
// argument (first line, capped). Falls back to the raw args when the JSON has
// no usable prompt. Used only as a pane header — never affects behaviour.
// Pre: arguments_json is the raw JSON from the tool call.
std::string subagent_label(std::string_view arguments_json) {
    return sanitize_tui_text(one_line(subagent_prompt(arguments_json), 80));
}

// Playful Swiss-German "busy" words, sprinkled in while the agent works. Picked
// by an index the caller derives from a counter, so the choice drifts over time.
const std::vector<std::string>& busy_words() {
    static const std::vector<std::string> w = {
        "Am chuchichäschtle", "Am bümpernüssle", "Am grindumehaue", "Am chnuuschte",
        "Am hundsverloche", "Am brötle", "Am figurettle", "Am hinderwäldle",
        "Am rööschte", "Am schöggele", "Am bibääpele", "Am süücheibe", "Am plämperle",
        "Am grosschotze", "Am drischisse", "Am rätschbäsele", "Am tschumple", "Am tüpflischisse",
        "Am umesuddle", "Am veschlimmbessere", "Am ineworgle", "Am schafseckle", "Am toblerönle",
        "Am Lindt & Sprüngle", "Am caquelon rüere", "Am velööle", "Am sürmle", "Am aateigge",
        "Am gigampfe", "Am schnuddere", "Am böögge", "Am höie", "Am rübis und stübise", 
    };
    return w;
}

// Pick a busy word by an arbitrary counter (frame, token count, …).
const std::string& busy_word(std::size_t n) {
    const auto& w = busy_words();
    return w[n % w.size()];
}

// ASCII spinner glyph cycling | / - \ with the frame counter (4 even-width
// frames so the rotation reads smoothly).
const char* spin_glyph(std::size_t frame) {
    static const char* g[] = {"|", "/", "-", "\\"};
    return g[frame % 4];
}

// One-line description of a mouse event for the --debug status chip:
// "L·prs 45,12" = button · motion, then frame coordinates.
std::string describe_mouse(const ftxui::Mouse& m) {
    const char* b = m.button == ftxui::Mouse::Left        ? "L"
                    : m.button == ftxui::Mouse::Middle    ? "M"
                    : m.button == ftxui::Mouse::Right     ? "R"
                    : m.button == ftxui::Mouse::WheelUp   ? "WUp"
                    : m.button == ftxui::Mouse::WheelDown ? "WDn"
                                                          : "-";
    const char* mo = m.motion == ftxui::Mouse::Pressed    ? "prs"
                     : m.motion == ftxui::Mouse::Released ? "rel"
                                                          : "mov";
    return std::string(b) + "·" + mo + " " + std::to_string(m.x) + "," +
           std::to_string(m.y);
}

// One-line description of a key event for the --debug status chip: the raw
// input bytes shown as printable ASCII (control bytes / ESC as caret or hex)
// plus a hex dump, so an unhandled key (e.g. Shift+Enter) reveals exactly what
// the terminal sends — which is what we need to bind it. Pure.
std::string describe_key(const ftxui::Event& e) {
    const std::string& in = e.input();
    std::string readable;
    std::string hex;
    static const char* digits = "0123456789abcdef";
    for (unsigned char c : in) {
        if (c == 0x1B) readable += "ESC";
        else if (c < 0x20) { readable += '^'; readable += char('@' + c); }
        else if (c == 0x7F) readable += "DEL";
        else readable += char(c);
        if (!hex.empty()) hex += ' ';
        hex += digits[c >> 4];
        hex += digits[c & 0xF];
    }
    const char* kind = e.is_character() ? "chr" : "spc";
    return std::string(kind) + " [" + readable + "] " + hex;
}

// Parse a kitty keyboard-protocol "CSI u" key report: ESC [ <codepoint> [;
// <modifiers>] u — e.g. "\x1B[13;2u" is Shift+Enter. Returns {codepoint,
// modifiers}; modifiers is the protocol's 1 + bitmask form (1 == no modifier,
// bit 0 Shift, bit 1 Alt, bit 2 Ctrl). nullopt when `s` is not that exact
// shape, so it doubles as the discriminator (legacy keys never match). A
// ":event-type" suffix on the modifier field (release/repeat reports) is
// ignored. Pure.
std::optional<std::pair<int, int>> parse_kitty_key(std::string_view s) {
    if (s.size() < 4 || s[0] != '\x1B' || s[1] != '[' || s.back() != 'u')
        return std::nullopt;
    const std::string_view body = s.substr(2, s.size() - 3);  // between '[' and 'u'
    auto to_int = [](std::string_view t, int& out) {
        if (t.empty()) return false;
        int v = 0;
        for (char c : t) {
            if (c < '0' || c > '9') return false;
            v = v * 10 + (c - '0');
        }
        out = v;
        return true;
    };
    int codepoint = 0;
    int modifiers = 1;
    const std::size_t semi = body.find(';');
    if (semi == std::string_view::npos)
        return to_int(body, codepoint) ? std::optional{std::pair{codepoint, modifiers}}
                                       : std::nullopt;
    if (!to_int(body.substr(0, semi), codepoint)) return std::nullopt;
    std::string_view mods = body.substr(semi + 1);
    if (const std::size_t colon = mods.find(':'); colon != std::string_view::npos)
        mods = mods.substr(0, colon);  // drop the event-type sub-parameter
    if (!to_int(mods, modifiers)) return std::nullopt;
    return std::pair{codepoint, modifiers};
}

// Mask an API key for display: "sk-…<last4>" (or "(none)" / "(set)" for short
// keys). Never reveal the full secret in the TUI/logs. Total.
std::string mask_key(std::string_view key) {
    if (key.empty()) return "(none)";
    if (key.size() <= 4) return "(set)";
    return "sk-…" + std::string(key.substr(key.size() - 4));
}

// Built-in profiles equivalent to the former hardcoded model_registry, so the
// out-of-the-box model→endpoint→kind mappings are unchanged when the user has
// configured no [profiles.*] of their own. Held in memory only; never written.
const std::vector<Profile>& default_profiles() {
    return builtin_profiles();
}

}  // namespace

// --- TuiState ---------------------------------------------------------------

void TuiState::seal() {
    answer_open_ = false;
    reasoning_open_ = false;
}

// Every external string is sanitized as it enters the render model (and only
// here — the renderer trusts the model): the FTXUI grid must never see bytes
// whose terminal width differs from FTXUI's computed width (tui_text.hpp).

void TuiState::push_user(std::string text) {
    seal();
    chat_.push_back({ChatKind::User, sanitize_tui_text(text), {}});
}

void TuiState::push_error(std::string text) {
    seal();
    chat_.push_back({ChatKind::ErrorLine, sanitize_tui_text(text), {}});
}

void TuiState::push_info(std::string text) {
    seal();
    chat_.push_back({ChatKind::Info, sanitize_tui_text(text), {}});
}

void TuiState::push_diff(std::string path, std::vector<DiffLine> diff) {
    seal();
    for (auto& d : diff) d.text = sanitize_tui_text(d.text);
    chat_.push_back({ChatKind::Diff, sanitize_tui_text(path), std::move(diff)});
}

void TuiState::load(const Conversation& conv) {
    chat_.clear();
    activity_.clear();
    subagents_.clear();
    selection_.reset();
    answer_open_ = false;
    reasoning_open_ = false;
    for (const Message& m : conv) {
        switch (m.role()) {
            case Role::User:
                chat_.push_back({ChatKind::User, sanitize_tui_text(m.content()), {}});
                break;
            case Role::Assistant:
                // Reasoning precedes prose, mirroring the live stream order.
                if (!m.reasoning().empty())
                    chat_.push_back({ChatKind::Reasoning,
                                     sanitize_tui_text(m.reasoning()), {},
                                     chat_.size()});
                if (!m.content().empty())
                    chat_.push_back(
                        {ChatKind::Assistant, sanitize_tui_text(m.content()), {}});
                // tool_calls -> Running activities (apply_message handles this).
                apply_message(m);
                break;
            case Role::Tool:
                apply_message(m);  // flips the matching activity by id
                break;
            case Role::System:
                break;  // system prompt is not shown in the chat log
        }
    }
    seal();  // nothing is mid-stream after a load
}

void TuiState::apply_delta(std::string_view answer, std::string_view reasoning) {
    if (!reasoning.empty()) {
        if (!reasoning_open_) {
            // Stamp a stable word seed at creation so the displayed Swiss word
            // stays put for this whole block (rather than churning per delta).
            chat_.push_back({ChatKind::Reasoning, "", {}, chat_.size()});
            reasoning_open_ = true;
            answer_open_ = false;
        }
        // Per-fragment sanitizing assumes fragments split on codepoint
        // boundaries — true for JSON-decoded SSE deltas, which are complete
        // JSON strings (and so valid UTF-8) by construction.
        chat_.back().text += sanitize_tui_text(collapse_newlines(reasoning));
    }
    if (!answer.empty()) {
        if (!answer_open_) {
            chat_.push_back({ChatKind::Assistant, "", {}});
            answer_open_ = true;
            reasoning_open_ = false;
        }
        chat_.back().text.append(sanitize_tui_text(answer));
    }
}

void TuiState::apply_message(const Message& m) {
    if (m.role() == Role::Assistant) {
        seal();  // this turn's prose is complete; the next delta starts fresh
        for (const auto& tc : m.tool_calls()) {
            // The row shows only the salient arg; full args go to args_full
            // for the detail pane.
            Activity a;
            a.id = tc.id;
            a.name = tc.name;
            a.args = tool_arg_summary(tc.arguments_json);
            a.args_full = full_text(tc.arguments_json);
            a.started = std::chrono::steady_clock::now();
            activity_.push_back(std::move(a));
            // Mirror the call inline in the chat log, referencing the activity
            // by id so status/output stay single-sourced (the Tool result flips
            // the activity; the inline row re-reads it on the next render).
            chat_.push_back({ChatKind::ToolUse, tc.id, {}});
            // A spawn_subagent call opens a group; its internal tool calls land
            // there via push_subagent_activity rather than in `activity_`.
            if (tc.name == "spawn_subagent") {
                SubagentGroup g;
                g.id = tc.id;
                g.label = subagent_label(tc.arguments_json);
                g.model = subagent_model(tc.arguments_json, parent_model_);
                g.prompt_full = full_text(subagent_prompt(tc.arguments_json));
                subagents_.push_back(std::move(g));
            }
        }
        return;
    }
    if (m.role() == Role::Tool) {
        const bool failed = m.tool_failed();
        for (auto& a : activity_) {
            if (a.id != m.tool_call_id()) continue;
            a.status = failed ? Status::Failed : Status::Ok;
            a.result_full = full_text(m.content());
            a.finished = std::chrono::steady_clock::now();
            // When spawn_subagent completes (success, error, or cancel), close
            // its group and flip any stranded Running calls to Failed.
            if (a.name == "spawn_subagent") {
                for (auto& g : subagents_)
                    if (g.id == a.id) {
                        g.status = a.status;
                        g.result_full = a.result_full;
                        // Fold a completed group the user never touched, so
                        // finished noise collapses but investigation state is
                        // never yanked away.
                        if (!g.user_toggled) g.expanded = false;
                    }
                resolve_subagent_orphans(a.id);
            }
            break;
        }
    }
}

void TuiState::push_subagent_text(std::string text) {
    SubagentGroup* g = nullptr;
    for (auto& cand : subagents_)
        if (cand.status == Status::Running) g = &cand;
    if (!g) return;
    SubagentTurn turn;
    // Preserve newlines (no collapse_newlines) — paragraph_block handles
    // wrapping.  Apply the same byte cap as every other render-model entry.
    turn.assistant_text = full_text(text);
    g->turns.push_back(std::move(turn));
}

void TuiState::push_subagent_activity(std::string id, std::string name,
                                       std::string args, Status status,
                                       std::string result) {
    // Sub-agents run sequentially within one spawn_subagent tool call, so the
    // active group is the most recent one still Running.
    SubagentGroup* g = nullptr;
    for (auto& cand : subagents_)
        if (cand.status == Status::Running) g = &cand;
    if (!g) return;  // no open group — drop rather than mis-attribute

    if (status == Status::Running) {
        // Append to the current turn (created by push_subagent_text).
        // If no turn exists yet (sub-agent called tools without text first),
        // create an empty turn as a fallback.
        if (g->turns.empty())
            g->turns.push_back(SubagentTurn{});
        Activity a;
        a.id = std::move(id);
        a.name = std::move(name);
        a.args = tool_arg_summary(args);
        a.args_full = full_text(args);
        a.started = std::chrono::steady_clock::now();
        g->turns.back().calls.push_back(std::move(a));
        return;
    }
    // Tool result: search all turns (not just the current one) for the
    // matching call — in theory a provider could emit results out of order,
    // and this costs nothing.
    for (auto it = g->turns.rbegin(); it != g->turns.rend(); ++it) {
        for (auto cit = it->calls.rbegin(); cit != it->calls.rend(); ++cit) {
            if (cit->id != id) continue;
            cit->status = status;
            cit->result_full = full_text(result);
            cit->finished = std::chrono::steady_clock::now();
            return;
        }
    }
}

void TuiState::resolve_subagent_orphans(std::string_view id) {
    for (auto& g : subagents_) {
        if (!id.empty() && g.id != id) continue;
        for (auto& turn : g.turns)
            for (auto& a : turn.calls)
                if (a.status == Status::Running) a.status = Status::Failed;
    }
}

bool TuiState::should_show_last_text(const SubagentGroup& g,
                                      std::size_t turn_idx) const {
    // pre: turn_idx is the last turn index
    // Returns true when the last turn's assistant_text should be shown rather
    // than silently deduped against result_full in the RESULT section.
    if (g.status == Status::Running) return true;
    // Finished: show when the last turn has tool calls or its text differs
    // from result_full (e.g. sub-agent failed — result_full is the error msg).
    const auto& turn = g.turns[turn_idx];
    return !turn.calls.empty() || turn.assistant_text != g.result_full;
}

void TuiState::set_expanded(std::string_view group_id, bool expanded) {
    for (auto& g : subagents_) {
        if (g.id != group_id) continue;
        g.expanded = expanded;
        g.user_toggled = true;
        // Folding away the selected child would orphan the selection — snap it
        // to the group's own row instead.
        if (!expanded && selection_ && selection_->parent_id == g.id)
            selection_ = NodeKey{NodeKey::Pane::Activity, 0, g.id, ""};
        return;  // ids are unique; no need to scan further
    }
}

std::vector<NodeKey> TuiState::nav_nodes(NodeKey::Pane pane) const {
    std::vector<NodeKey> out;
    if (pane == NodeKey::Pane::Chat) {
        out.reserve(chat_.size());
        for (std::size_t i = 0; i < chat_.size(); ++i)
            out.push_back({NodeKey::Pane::Chat, i, "", ""});
        return out;
    }
    if (pane == NodeKey::Pane::Subagents) {
        out.reserve(subagents_.size());
        for (const auto& g : subagents_)
            out.push_back({NodeKey::Pane::Subagents, 0, g.id, ""});
        return out;
    }
    // Activity: a flat list of the parent's own top-level calls. A sub-agent's
    // internal calls live in the subagents pane now, not the tree.
    out.reserve(activity_.size());
    for (const auto& a : activity_)
        out.push_back({NodeKey::Pane::Activity, 0, a.id, ""});
    return out;
}

bool TuiState::select_next() {
    if (!selection_) return false;
    auto nodes = nav_nodes(selection_->pane);
    auto it = std::find(nodes.begin(), nodes.end(), *selection_);
    if (it == nodes.end() || it + 1 == nodes.end()) return false;
    selection_ = *(it + 1);
    return true;
}

bool TuiState::select_prev() {
    if (!selection_) return false;
    auto nodes = nav_nodes(selection_->pane);
    auto it = std::find(nodes.begin(), nodes.end(), *selection_);
    if (it == nodes.end() || it == nodes.begin()) return false;
    selection_ = *(it - 1);
    return true;
}

void TuiState::select_last(NodeKey::Pane pane) {
    auto nodes = nav_nodes(pane);
    if (!nodes.empty()) selection_ = nodes.back();
}

const TuiState::Activity* TuiState::find_activity(const NodeKey& k) const {
    if (k.pane != NodeKey::Pane::Activity) return nullptr;
    if (k.parent_id.empty()) {
        for (const auto& a : activity_)
            if (a.id == k.id) return &a;
        return nullptr;
    }
    const SubagentGroup* g = find_group(k.parent_id);
    if (!g) return nullptr;
    for (const auto& turn : g->turns)
        for (const auto& c : turn.calls)
            if (c.id == k.id) return &c;
    return nullptr;
}

const TuiState::SubagentGroup* TuiState::find_group(std::string_view id) const {
    for (const auto& g : subagents_)
        if (g.id == id) return &g;
    return nullptr;
}

std::optional<NodeKey> TuiState::detail_node() const {
    if (selection_) {
        if (selection_->pane == NodeKey::Pane::Chat &&
            selection_->chat_index < chat_.size()) {
            // An inline tool-use row resolves to its Activity so the detail pane
            // shows the call's full I/O (or its sub-agent group), exactly as if
            // the Activity-pane row had been clicked.
            const auto& e = chat_[selection_->chat_index];
            if (e.kind == ChatKind::ToolUse)
                return NodeKey{NodeKey::Pane::Activity, 0, e.text, ""};
            return selection_;
        }
        if (selection_->pane == NodeKey::Pane::Activity &&
            find_activity(*selection_))
            return selection_;
        if (selection_->pane == NodeKey::Pane::Subagents &&
            find_group(selection_->id))
            return selection_;
    }
    if (!activity_.empty())
        return NodeKey{NodeKey::Pane::Activity, 0, activity_.back().id, ""};
    return std::nullopt;
}

// The current tool a running sub-agent group is executing: the most recent
// Running call, scanning turns then calls back-to-front. Empty (with empty arg)
// when the agent is between calls. Drives the collapsed-row live hint in the
// subagents browser.
std::pair<std::string, std::string> running_subagent_tool(
    const TuiState::SubagentGroup& g) {
    for (auto t = g.turns.rbegin(); t != g.turns.rend(); ++t)
        for (auto a = t->calls.rbegin(); a != t->calls.rend(); ++a)
            if (a->status == TuiState::Status::Running)
                return {a->name, a->args};
    return {};
}

// --- human_tokens -----------------------------------------------------------

std::string human_tokens(int n) {
    if (n < 1000) return std::to_string(n);
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.1fk", n / 1000.0);
    return buf;
}

// --- human_duration ---------------------------------------------------------

std::string human_duration(std::chrono::milliseconds ms) {
    const long long n = ms.count();
    char buf[32];
    if (n < 1000) return std::to_string(n) + "ms";
    if (n < 60'000) {
        std::snprintf(buf, sizeof buf, "%.1fs", static_cast<double>(n) / 1000.0);
        return buf;
    }
    std::snprintf(buf, sizeof buf, "%lldm%02llds", n / 60'000,
                  (n % 60'000) / 1000);
    return buf;
}

// --- tool_arg_summary ---------------------------------------------------------

std::string tool_arg_summary(std::string_view arguments_json) {
    // Keys tried in order; the first non-empty string value wins. Target-like
    // keys come first so file tools show their path, then command/content-like
    // ones. A non-object / unparseable / no-match falls back to the raw args.
    static constexpr std::string_view kKeys[] = {
        "path", "file", "cmd", "command", "url", "query",
        "prompt", "question", "symbol", "pattern", "name"};
    if (auto j = json::parse(arguments_json); j && j->is_object())
        for (std::string_view k : kKeys)
            if (auto v = json::get_string_opt(*j, k); v && *v && !(*v)->empty())
                return sanitize_tui_text(one_line(**v, 200));
    return sanitize_tui_text(one_line(arguments_json, 200));
}

std::string last_lines(std::string_view text, std::size_t n) {
    if (n == 0) return {};
    // Split on '\n', stripping any trailing '\r', then drop trailing blank
    // lines so a tool whose output ends in newlines doesn't preview as blanks.
    std::vector<std::string_view> lines;
    std::size_t pos = 0;
    while (pos <= text.size()) {
        std::size_t nl = text.find('\n', pos);
        std::string_view seg = text.substr(
            pos, nl == std::string_view::npos ? std::string_view::npos : nl - pos);
        if (!seg.empty() && seg.back() == '\r') seg.remove_suffix(1);
        lines.push_back(seg);
        if (nl == std::string_view::npos) break;
        pos = nl + 1;
    }
    while (!lines.empty() && lines.back().empty()) lines.pop_back();
    if (lines.empty()) return {};
    std::size_t start = lines.size() > n ? lines.size() - n : 0;
    std::string out;
    for (std::size_t i = start; i < lines.size(); ++i) {
        if (i != start) out += '\n';
        out += lines[i];
    }
    return out;
}

// --- syntax theme names -----------------------------------------------------
// One ordered table is the single source of truth for both directions of the
// name↔id mapping plus the /theme listing, so the set can never drift.
namespace {
constexpr std::pair<SyntaxTheme, std::string_view> kThemeTable[] = {
    {SyntaxTheme::Default, "default"},
    {SyntaxTheme::Mono, "mono"},
    {SyntaxTheme::Vivid, "vivid"},
    {SyntaxTheme::None, "none"},
};
}  // namespace

std::string_view syntax_theme_name(SyntaxTheme t) {
    for (const auto& [id, name] : kThemeTable)
        if (id == t) return name;
    return "default";
}

std::optional<SyntaxTheme> syntax_theme_from_name(std::string_view name) {
    const std::string want = to_lower(name);
    for (const auto& [id, n] : kThemeTable)
        if (n == want) return id;
    return std::nullopt;
}

std::vector<std::string> syntax_theme_names() {
    std::vector<std::string> names;
    for (const auto& [id, name] : kThemeTable) names.emplace_back(name);
    return names;
}

// --- ApprovalGate -----------------------------------------------------------

Approval ApprovalGate::request(ToolCall tc) {
    // Serialise concurrent requests: only one approval is in flight at a time.
    std::lock_guard serial(serialize_);
    std::unique_lock lk(m_);
    if (released_) return Approval::Deny;
    call_ = std::move(tc);
    answer_.reset();
    lk.unlock();
    if (notify_) notify_();  // ask the UI to redraw (don't hold the lock)
    lk.lock();
    cv_.wait(lk, [&] { return answer_.has_value() || released_; });
    Approval result = released_ ? Approval::Deny : answer_.value_or(Approval::Deny);
    call_.reset();
    answer_.reset();
    return result;
}

bool ApprovalGate::pending() const {
    std::lock_guard lk(m_);
    return call_.has_value();
}

std::optional<ToolCall> ApprovalGate::pending_call() const {
    std::lock_guard lk(m_);
    return call_;
}

void ApprovalGate::answer(Approval a) {
    {
        std::lock_guard lk(m_);
        answer_ = a;
    }
    cv_.notify_all();
}

void ApprovalGate::release() {
    {
        std::lock_guard lk(m_);
        released_ = true;
    }
    cv_.notify_all();
}

// --- View + controller ------------------------------------------------------

namespace {

using namespace ftxui;

// text() for decorative chrome (separators, carets, status marks, help lines):
// folds the glyphs to ASCII in ASCII mode, passes through verbatim otherwise.
// Use for any string literal/variable that carries chrome glyphs and does NOT
// already flow through sanitize_tui_text (which folds dynamic text itself).
inline Element gtext(std::string s) { return text(glyphify(s)); }

// Blinking text-edit block cursor, ASCII-folded ("▌" -> "|" in ASCII mode).
inline std::string block_cursor(bool on) { return on ? glyphify("▌") : std::string(" "); }

// ~/.moo/history (empty when persistence is off). One-time best-effort
// migration of the legacy ~/.moo_history file the first time it is needed.
std::string history_path(const std::string& home) {
    if (home.empty()) return {};
    std::string path = home + "/history";
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) {
        std::string h = user_home();
        if (!h.empty()) {
            std::string legacy = h + "/.moo_history";
            if (std::filesystem::exists(legacy, ec)) {
                std::filesystem::create_directories(home, ec);
                std::filesystem::copy_file(legacy, path, ec);  // never delete legacy
            }
        }
    }
    return path;
}

std::vector<std::string> load_history(const std::string& path) {
    std::vector<std::string> out;
    if (path.empty()) return out;
    std::ifstream in(path);
    for (std::string line; std::getline(in, line);)
        if (!line.empty()) out.push_back(line);
    return out;
}

void append_history(const std::string& path, const std::string& line) {
    if (path.empty()) return;
    std::ofstream out(path, std::ios::app);
    out << line << '\n';
}

// True when only whitespace — nothing worth sending to the model.
bool is_blank(const std::string& s) {
    return s.find_first_not_of(" \t\r\n") == std::string::npos;
}

// Map a token category to an FTXUI decorator under the given theme. Plain — and
// any category a theme chooses not to style — returns `nothing`, so that text
// renders in the terminal's default foreground (no block-wide base colour). A
// theme may compose style + colour via Decorator's operator|.
Decorator token_style(SyntaxTheme theme, TokenCategory cat) {
    using C = TokenCategory;
    switch (theme) {
        case SyntaxTheme::None:
            return nothing;  // no highlighting at all
        case SyntaxTheme::Mono:
            // Structure-only: dim comments, embolden keywords, rest default.
            if (cat == C::Comment) return dim;
            if (cat == C::Keyword || cat == C::Preproc) return bold;
            return nothing;
        case SyntaxTheme::Vivid:
            switch (cat) {
                case C::Keyword:  return Decorator(bold) | color(Color::MagentaLight);
                case C::Type:     return Decorator(bold) | color(Color::CyanLight);
                case C::Builtin:  return color(Color::BlueLight);
                case C::String:   return color(Color::GreenLight);
                case C::Number:   return Decorator(bold) | color(Color::Orange1);
                case C::Comment:  return color(Color::GrayLight);
                case C::Preproc:  return Decorator(bold) | color(Color::RedLight);
                case C::Variable: return color(Color::Red);
                case C::Plain:    break;
            }
            return nothing;
        case SyntaxTheme::Default:
            break;
    }
    // Default theme: tasteful accents, non-keyword prose stays terminal-default.
    switch (cat) {
        case C::Keyword:  return color(Color::Magenta);
        case C::Type:     return color(Color::CyanLight);
        case C::Builtin:  return color(Color::Blue);
        case C::String:   return color(Color::GreenLight);
        case C::Number:   return color(Color::Orange1);
        case C::Comment:  return color(Color::GrayDark);
        case C::Preproc:  return color(Color::Magenta);
        case C::Variable: return color(Color::RedLight);
        case C::Plain:    break;
    }
    return nothing;
}

// Render one fenced code segment as a bordered block. The opening fence's info
// string (the first line, e.g. "bash") is stripped — it never appears as code,
// recognised language or not — and used to pick syntax highlighting. Plain text
// keeps the terminal default; only highlighted tokens are coloured.
Element render_code_block(const std::string& seg, SyntaxTheme theme) {
    std::string body = seg;
    Language lang = Language::None;
    // The info string is the remainder of the fence line, so it only exists
    // when there is a newline separating it from the code below.
    if (std::size_t nl = seg.find('\n'); nl != std::string::npos) {
        std::string info = seg.substr(0, nl);
        // First whitespace-delimited token of the info string is the language.
        std::size_t b = info.find_first_not_of(" \t\r");
        if (b != std::string::npos) {
            std::size_t e = info.find_first_of(" \t\r", b);
            lang = language_from_tag(info.substr(
                b, e == std::string::npos ? std::string::npos : e - b));
        }
        body = seg.substr(nl + 1);
    }

    Elements lines;
    for (const auto& spans : highlight_block(body, lang)) {
        if (spans.empty()) {
            lines.push_back(text(""));
            continue;
        }
        Elements pieces;
        for (const auto& s : spans)
            pieces.push_back(text(s.text) | token_style(theme, s.category));
        lines.push_back(hbox(std::move(pieces)));
    }
    return vbox(std::move(lines)) | bgcolor(Color::RGB(20, 22, 26)) | border;
}

// Render assistant prose, styling ```fenced``` segments as code blocks. Even
// segments are prose paragraphs, odd segments are fenced code.
Element render_markdownish(const std::string& prose, SyntaxTheme theme) {
    Elements parts;
    std::size_t pos = 0;
    bool code = false;
    while (pos <= prose.size()) {
        std::size_t fence = prose.find("```", pos);
        std::string seg = prose.substr(pos, fence == std::string::npos
                                                ? std::string::npos
                                                : fence - pos);
        if (!seg.empty()) {
            if (code)
                parts.push_back(render_code_block(seg, theme));
            else
                parts.push_back(paragraph(seg));
        }
        if (fence == std::string::npos) break;
        pos = fence + 3;
        code = !code;
    }
    if (parts.empty()) parts.push_back(text(""));
    return vbox(std::move(parts));
}

// A bordered file-diff block: bold path header, then one line per diff entry
// styled +green / -red / dim-context, reusing the code-block border style.
Element render_diff_block(const std::string& path,
                          const std::vector<DiffLine>& diff, bool full) {
    Elements lines;
    lines.push_back(text(path) | bold | color(Color::CyanLight));
    lines.push_back(separator());
    std::vector<DiffLine> elided;
    if (!full) elided = elide_context(diff, kDiffContext);
    for (const auto& d : full ? diff : elided) {
        switch (d.op) {
            case DiffLine::Op::Add:
                lines.push_back(text("+ " + d.text) | color(Color::GreenLight));
                break;
            case DiffLine::Op::Del:
                lines.push_back(text("- " + d.text) | color(Color::RedLight));
                break;
            case DiffLine::Op::Context:
                lines.push_back(text("  " + d.text) | dim);
                break;
        }
    }
    return vbox(std::move(lines)) | border;
}

// One frame's mouse hit-targets. reflect() stores references to these Boxes,
// so entries must keep stable addresses while the frame renders — hence deque.
// UI-thread only: written by the render lambda, read by CatchEvent.
struct Hit {
    ftxui::Box box;
    NodeKey key;
    bool expander = false;  // clicking toggles fold instead of selecting
};
using HitMap = std::deque<Hit>;

// Defined below; render_chat renders inline tool-use rows using both.
Element status_glyph(TuiState::Status s, std::size_t frame, bool quiet);
Element paragraph_block(std::string_view body);

Element render_chat(const TuiState& st, std::size_t frame, HitMap& hits,
                    bool follow) {
    Elements out;
    const auto& sel = st.selection();
    for (std::size_t i = 0; i < st.chat().size(); ++i) {
        const auto& e = st.chat()[i];
        Elements ev;
        switch (e.kind) {
            case TuiState::ChatKind::User:
                ev.push_back(gtext("▌ you") | bold | color(Color::Green));
                ev.push_back(paragraph(e.text) | color(Color::GrayLight));
                break;
            case TuiState::ChatKind::Assistant:
                ev.push_back(gtext("▌ moocode") | bold | color(Color::CyanLight));
                ev.push_back(render_markdownish(e.text, st.syntax_theme()));
                break;
            case TuiState::ChatKind::Reasoning:
                if (st.reasoning_collapsed()) {
                    // Default view: a live ~token estimate (~4 bytes/token), not
                    // the raw chain of thought. F2 reveals the full text.
                    std::size_t approx = (e.text.size() + 3) / 4;
                    // Only the actively-streaming reasoning block spins; past
                    // (completed) blocks show a static indicator.
                    const bool is_active =
                        (&e == &st.chat().back()) && st.has_active_reasoning();
                    if (is_active) {
                        ev.push_back(text(std::string(spin_glyph(frame)) + " " +
                                          busy_word(e.word_seed) + "    ~" +
                                          std::to_string(approx) +
                                          " tokens  F2 to show") |
                                     dim);
                    } else {
                        ev.push_back(
                            text(busy_word(e.word_seed) + "  ~" +
                                 std::to_string(approx) + " tokens  F2 to show") |
                            dim);
                    }
                } else {
                    ev.push_back(paragraph(glyphify("· ") + e.text) | dim);
                }
                break;
            case TuiState::ChatKind::ErrorLine:
                ev.push_back(paragraph(e.text) | color(Color::Red));
                break;
            case TuiState::ChatKind::Info:
                ev.push_back(paragraph(e.text) | dim | color(Color::CyanLight));
                break;
            case TuiState::ChatKind::Diff:
                ev.push_back(render_diff_block(e.text, e.diff, /*full=*/false));
                break;
            case TuiState::ChatKind::ToolUse: {
                // e.text holds the tool_call_id; the Activity is the source of
                // truth for name/args/status/output (set by apply_message).
                const TuiState::Activity* a =
                    st.find_activity({NodeKey::Pane::Activity, 0, e.text, ""});
                if (!a) break;
                // ✓/✗/spinner  ToolName(args) — name in blue, args dimmed.
                ev.push_back(hbox({
                    status_glyph(a->status, frame, /*quiet=*/false),
                    text(" "),
                    text(a->name) | bold | color(Color::Blue),
                    text("(" + a->args + ")") | color(Color::GrayLight),
                }));
                // Last 3 lines of output in gray, indented under the row.
                if (a->status != TuiState::Status::Running) {
                    std::string tail = last_lines(a->result_full, 3);
                    if (!tail.empty())
                        ev.push_back(hbox({text("  "),
                                           paragraph_block(tail) |
                                               color(Color::GrayDark)}));
                }
                break;
            }
        }
        NodeKey key{NodeKey::Pane::Chat, i, "", ""};
        if (sel && *sel == key && !ev.empty()) {
            // Selection highlight: the entry's first line only — inverting a
            // whole prose block would be unreadable.
            ev.front() = ev.front() | inverted;
            if (follow) ev.front() = ev.front() | focus;
        }
        hits.push_back({{}, key, /*expander=*/false});
        out.push_back(vbox(std::move(ev)) | reflect(hits.back().box));
        out.push_back(separatorEmpty());
    }
    if (out.empty())
        out.push_back(text("Ask moocode anything about this directory.") | dim);
    return vbox(std::move(out));
}

// Status glyph for one activity row: a spinning mark while running, ✓/✗ once
// resolved. `quiet` dims the running spinner to a dot (used for nested rows).
Element status_glyph(TuiState::Status s, std::size_t frame, bool quiet) {
    switch (s) {
        case TuiState::Status::Running:
            return quiet ? gtext("·") | dim
                         : text(spin_glyph(frame)) | color(Color::Yellow);
        case TuiState::Status::Ok:
            return gtext("✓") | color(Color::Green);
        case TuiState::Status::Failed:
            return gtext("✗") | color(Color::Red);
    }
    return text(" ");
}

// Multi-line text as wrapped paragraphs, one per line (blank lines preserved,
// trailing \r dropped). Full weight — no dim, no indent.
Element paragraph_block(std::string_view body) {
    Elements lines;
    std::size_t pos = 0;
    while (true) {
        std::size_t nl = body.find('\n', pos);
        std::string_view seg = body.substr(
            pos, nl == std::string_view::npos ? std::string_view::npos : nl - pos);
        if (!seg.empty() && seg.back() == '\r') seg.remove_suffix(1);
        lines.push_back(seg.empty() ? text("") : paragraph(std::string(seg)));
        if (nl == std::string_view::npos) break;
        pos = nl + 1;
    }
    return vbox(std::move(lines));
}

// Args for the detail pane: pretty-printed when they parse as JSON (the
// 128 KiB cap can cut the middle of huge args), raw text otherwise.
// dump re-emits raw DEL/C1 bytes (only <0x20 is escaped), so re-sanitize.
std::string pretty_args(const std::string& args_full) {
    if (auto j = json::parse(args_full)) return sanitize_tui_text(json::dump_pretty(*j));
    return args_full;
}

// Plain-text rendering of the focused detail node, for the Ctrl+Y clipboard
// yank. Mirrors render_detail's content (minus glyphs/colour) so what lands on
// the clipboard is what the detail pane shows. Empty when nothing is focused.
std::string detail_plain_text(const TuiState& st) {
    auto key = st.detail_node();
    if (!key) return {};
    if (key->pane == NodeKey::Pane::Chat) {
        const auto& e = st.chat()[key->chat_index];
        if (e.kind != TuiState::ChatKind::Diff) return e.text;
        std::string out = e.text;  // path header
        for (const auto& dl : e.diff) {
            char mark = dl.op == DiffLine::Op::Add   ? '+'
                        : dl.op == DiffLine::Op::Del ? '-'
                                                     : ' ';
            out += '\n';
            out += mark;
            out += dl.text;
        }
        return out;
    }
    if (key->pane == NodeKey::Pane::Subagents || key->parent_id.empty())
        if (const auto* g = st.find_group(key->id)) {
            std::string out = "PROMPT\n" + g->prompt_full;
            for (std::size_t i = 0; i < g->turns.size(); ++i) {
                const auto& turn = g->turns[i];
                bool is_last = (i + 1 == g->turns.size());
                if ((!is_last || st.should_show_last_text(*g, i)) &&
                    !turn.assistant_text.empty())
                    out += "\n\nASSISTANT\n" + turn.assistant_text;
                for (const auto& c : turn.calls) {
                    out += "\n\n" + c.name + "\nINPUT\n" + pretty_args(c.args_full);
                    if (c.status != TuiState::Status::Running)
                        out += "\nOUTPUT\n" + c.result_full;
                }
            }
            if (g->status != TuiState::Status::Running)
                out += "\n\nRESULT\n" + g->result_full;
            return out;
        }
    if (const auto* a = st.find_activity(*key)) {
        std::string out = a->name + "\n\nINPUT\n" + pretty_args(a->args_full);
        if (a->status != TuiState::Status::Running)
            out += "\n\nOUTPUT\n" + a->result_full;
        return out;
    }
    return {};
}

// The detail pane proper: the full I/O of the focused node, the on-screen mirror
// of detail_plain_text (the Ctrl+Y yank). A chat entry renders its prose/diff; a
// spawn_subagent group renders its PROMPT then every internal turn (assistant
// prose + each tool call's INPUT/OUTPUT) and a closing RESULT — so clicking a
// sub-agent shows its prompt and history; a plain activity renders INPUT/OUTPUT.
// Falls back to the live running-sub-agent dashboard when nothing is focused.
// Bold cyan section label used throughout the detail/subagents render.
Element detail_header(std::string_view s) {
    return text(std::string(s)) | bold | color(Color::CyanLight);
}

// One tool call's INPUT (+ OUTPUT once finished) block.
Element detail_call_block(const TuiState::Activity& c, std::size_t frame) {
    Elements b;
    b.push_back(hbox({status_glyph(c.status, frame, /*quiet=*/false), text(" "),
                      text(c.name) | bold}));
    b.push_back(detail_header("INPUT"));
    b.push_back(paragraph_block(pretty_args(c.args_full)));
    if (c.status != TuiState::Status::Running) {
        b.push_back(detail_header("OUTPUT"));
        b.push_back(paragraph_block(c.result_full));
    }
    return vbox(std::move(b));
}

// A sub-agent group's full history: PROMPT, each turn's ASSISTANT prose and
// tool calls, and a closing RESULT once finished. Shared by the maximize/copy
// path (render_detail) and the inline subagents browser.
Element render_group_body(const TuiState& st, const TuiState::SubagentGroup& g,
                          std::size_t frame) {
    Elements out;
    out.push_back(detail_header("PROMPT"));
    out.push_back(paragraph_block(g.prompt_full));
    for (std::size_t i = 0; i < g.turns.size(); ++i) {
        const auto& turn = g.turns[i];
        bool is_last = (i + 1 == g.turns.size());
        if ((!is_last || st.should_show_last_text(g, i)) &&
            !turn.assistant_text.empty()) {
            out.push_back(text(""));
            out.push_back(detail_header("ASSISTANT"));
            out.push_back(render_markdownish(turn.assistant_text, st.syntax_theme()));
        }
        for (const auto& c : turn.calls) {
            out.push_back(text(""));
            out.push_back(detail_call_block(c, frame));
        }
    }
    if (g.status != TuiState::Status::Running) {
        out.push_back(text(""));
        out.push_back(detail_header("RESULT"));
        out.push_back(paragraph_block(g.result_full));
    }
    return vbox(std::move(out));
}

Element render_detail(const TuiState& st, std::size_t frame) {
    auto key = st.detail_node();
    if (!key) return text("");

    if (key->pane == NodeKey::Pane::Chat) {
        const auto& e = st.chat()[key->chat_index];
        if (e.kind == TuiState::ChatKind::Diff)
            return render_diff_block(e.text, e.diff, /*full=*/true);
        return render_markdownish(e.text, st.syntax_theme());
    }

    // A spawn_subagent group (selected in the tree or focused in the browser):
    // prompt + the whole internal history.
    if (key->pane == NodeKey::Pane::Subagents || key->parent_id.empty())
        if (const auto* g = st.find_group(key->id))
            return render_group_body(st, *g, frame);

    // A plain activity row (top-level call or a sub-agent's nested call).
    if (const auto* a = st.find_activity(*key)) return detail_call_block(*a, frame);
    return text("");
}

// The subagents pane: every sub-agent group as a foldable row. Collapsed shows
// a one-line header (label · model · status, plus the live tool for a running
// group); expanded appends the group's full history (render_group_body). Each
// row deposits a fold hitbox on its ▶/▼ glyph (pushed BEFORE the row hitbox so a
// glyph click folds rather than selects) and a select hitbox on the header.
// `focus` (a Subagents key) marks the keyboard-focused group.
Element render_subagent_browser(const TuiState& st, std::size_t frame,
                                HitMap& hits,
                                const std::optional<NodeKey>& focus) {
    Elements out;
    for (const auto& g : st.subagents()) {
        NodeKey key{NodeKey::Pane::Subagents, 0, g.id, ""};
        hits.push_back({{}, key, /*expander=*/true});
        Element glyph = gtext(g.expanded ? "▼ " : "▶ ") | reflect(hits.back().box);
        Elements head_cells{std::move(glyph),
                            text(g.label.empty() ? "sub-agent" : g.label) | bold};
        if (!g.model.empty())
            head_cells.push_back(text("  " + g.model) | color(Color::GrayLight));
        // Running + collapsed: surface the tool it is executing right now so the
        // folded row keeps the old live-dashboard value.
        if (!g.expanded && g.status == TuiState::Status::Running) {
            auto [tool, arg] = running_subagent_tool(g);
            std::string hint = tool.empty() ? "" : "  ↳ " + tool;
            if (!hint.empty()) head_cells.push_back(gtext(hint) | dim);
        }
        head_cells.push_back(text(" ") | flex);
        head_cells.push_back(status_glyph(g.status, frame, /*quiet=*/false));
        Element head = hbox(std::move(head_cells));
        if (focus && *focus == key) head = head | inverted;
        hits.push_back({{}, key, /*expander=*/false});
        out.push_back(std::move(head) | reflect(hits.back().box));
        if (g.expanded) {
            out.push_back(render_group_body(st, g, frame));
            out.push_back(text(""));  // breathing room before the next group
        }
    }
    if (out.empty()) out.push_back(text("no sub-agents yet") | dim);
    return vbox(std::move(out));
}

// Copy `content` to the system clipboard via OSC 52, writing straight to the
// terminal fd. OSC 52 is terminal-driven (works locally and over SSH) and does
// not disturb FTXUI's screen buffer. Sent raw: real terminals act on it, and
// tmux with `set-clipboard on` (default `external`) intercepts it and forwards
// to the outer terminal — a DCS passthrough wrapper would instead bypass that.
// Returns false if the terminal fd is unwritable or content is empty.
// Write `seq` straight to the controlling terminal, bypassing FTXUI's screen
// buffer (so it neither disturbs the frame nor is escaped by it). Used for OSC
// 52 clipboard writes and for the DEC private modes FTXUI does not manage
// (bracketed paste, modifyOtherKeys). Returns false if the terminal fd is
// unwritable. POSIX writes to /dev/tty (falling back to stdout); Windows has no
// /dev/tty, so it writes to stdout, which ConsoleGuard has put in VT mode.
bool write_tty_raw(std::string_view seq) {
    if (seq.empty()) return false;
#ifdef _WIN32
    return std::fwrite(seq.data(), 1, seq.size(), stdout) == seq.size() &&
           std::fflush(stdout) == 0;
#else
    int fd = ::open("/dev/tty", O_WRONLY | O_CLOEXEC);
    bool owned = fd >= 0;
    if (!owned) fd = STDOUT_FILENO;
    const char* p = seq.data();
    std::size_t left = seq.size();
    bool ok = true;
    while (left > 0) {
        ssize_t n = ::write(fd, p, left);
        if (n <= 0) { ok = false; break; }
        p += n;
        left -= static_cast<std::size_t>(n);
    }
    if (owned) ::close(fd);
    return ok;
#endif
}

bool osc52_copy(const std::string& content) {
    if (content.empty()) return false;
#ifdef _WIN32
    // Legacy conhost ignores OSC 52, so use the native Win32 clipboard (works
    // in both cmd.exe and Windows Terminal).
    return tui_platform::native_clipboard_copy(content);
#else
    return write_tty_raw("\033]52;c;" + base64_encode(content) + "\a");
#endif
}

// The unified activity tree: top-level calls in order; an expanded
// spawn_subagent group nests its internal calls directly underneath. Every row
// deposits a mouse hitbox; group expander glyphs get their own, pushed BEFORE
// the row's so a click on the glyph folds rather than selects (click
// resolution takes the first containing box). `follow` marks the selected row
// with the focus decorator so keyboard moves scroll it into view.
Element render_activity_tree(const TuiState& st, std::size_t frame,
                             HitMap& hits, bool follow) {
    const auto& sel = st.selection();
    Elements out;

    auto add_row = [&](Element row, const NodeKey& key) {
        if (sel && *sel == key) {
            row = row | inverted;
            if (follow) row = row | focus;
        }
        hits.push_back({{}, key, /*expander=*/false});
        out.push_back(std::move(row) | reflect(hits.back().box));
    };

    // A flat list of the parent's own top-level calls. A spawn_subagent row is
    // an ordinary row — its sub-agent's internal calls live in the subagents
    // pane now, not nested here. One non-wrapping line per call ("name
    // salient-arg"; args is a capped one-liner, text() clips at the pane edge).
    // Results never render here — the subagents/detail pane owns output — so the
    // tree stays a scannable list and every line in it is a clickable row.
    for (const auto& a : st.activity()) {
        NodeKey key{NodeKey::Pane::Activity, 0, a.id, ""};
        add_row(hbox({gtext("· ") | dim, text(a.name) | bold,
                      text(" " + a.args) | color(Color::GrayLight) | flex,
                      text(" "), status_glyph(a.status, frame, /*quiet=*/false)}),
                key);
    }
    if (out.empty()) out.push_back(text("no tool activity yet") | dim);
    return vbox(std::move(out));
}

// Non-modal word-completion popup (same visual as @-complete), shared by slash
// commands with argument autocomplete. The renderer reads this struct; the
// slash-command registry writes it via a pointer.
struct ModelComplete {
    bool active = false;
    std::vector<std::string> items;  // matching candidates
    int sel = 0;
    std::string prefix;  // active command prefix, e.g. "/model " or "/provider "
    std::string label;   // popup window title, e.g. "model" / "provider"
};

// --- Slash command registry -------------------------------------------------
// Holds all TUI slash commands with their handlers and optional argument
// completers, so adding a new command is a one-line registration rather than
// touching the dispatch chain.
struct SlashCommands {
    using Handler = std::function<void(std::string_view arg)>;
    using Completer = std::function<std::vector<std::string>()>;

    struct Entry {
        std::string name;
        std::vector<std::string> aliases;
        Handler handler;
        Completer completer;  // nullptr => no completion
    };

    SlashCommands(std::initializer_list<Entry> e, std::string* input,
                  int* cursor, std::mutex* smtx, ModelComplete* mc)
        : entries_(e), input_(input), cursor_(cursor),
          state_mtx_(smtx), model_complete_(mc) {}

    // Handle a slash-command line (pre: line[0] == '/'). Returns true if the
    // command was recognised and handled, false when unknown.
    bool dispatch(std::string_view line) {
        // Split "/cmd arg" into command + trimmed argument.
        std::string_view cmd = line, arg;
        if (auto sp = line.find(' '); sp != std::string_view::npos) {
            cmd = line.substr(0, sp);
            arg = line.substr(sp + 1);
            auto a = arg.find_first_not_of(" \t");
            auto b = arg.find_last_not_of(" \t");
            arg = (a == std::string_view::npos) ? std::string_view{}
                                                  : arg.substr(a, b - a + 1);
        }
        for (const Entry& e : entries_) {
            bool match = (cmd == e.name);
            if (!match)
                for (const std::string& a : e.aliases)
                    if (cmd == a) { match = true; break; }
            if (!match) continue;
            e.handler(arg);
            return true;
        }
        return false;
    }

    // Refresh the autocomplete popup from the input line and cursor position.
    // Called on every input change. Thread-safe: touches model_complete_ under
    // state_mtx_ (the caller must NOT hold state_mtx_).
    void refresh_complete() {
        std::string_view line = *input_;
        int cur = std::max(0, *cursor_);

        // Command-name completion: input starts with '/' and has no space yet.
        if (!line.empty() && line[0] == '/' &&
            line.find(' ') == std::string_view::npos &&
            static_cast<std::size_t>(cur) == line.size()) {
            std::string want = to_lower(line);
            std::vector<std::string> matches;
            for (const Entry& e : entries_) {
                if (to_lower(e.name).compare(0, want.size(), want) == 0)
                    matches.push_back(e.name);
                for (const std::string& a : e.aliases)
                    if (to_lower(a).compare(0, want.size(), want) == 0)
                        matches.push_back(a);
            }
            std::lock_guard lk(*state_mtx_);
            if (matches.empty()) {
                *model_complete_ = ModelComplete{};
            } else {
                model_complete_->active = true;
                model_complete_->items = std::move(matches);
                model_complete_->prefix = "";
                model_complete_->label = "command";
                if (model_complete_->sel >=
                    static_cast<int>(model_complete_->items.size()))
                    model_complete_->sel = 0;
            }
            return;
        }

        // Argument completion for commands that have a completer.
        for (const Entry& e : entries_) {
            if (!e.completer) continue;
            std::string prefix = e.name + " ";
            std::size_t plen = prefix.size();
            if (line.size() < plen || line.substr(0, plen) != prefix) continue;
            if (static_cast<std::size_t>(cur) < plen) continue;  // cursor before arg
            std::string_view arg_span =
                line.substr(plen, static_cast<std::size_t>(cur) - plen);
            if (arg_span.find(' ') != std::string_view::npos)
                continue;  // cursor past the first word
            std::string want = to_lower(arg_span);
            std::vector<std::string> candidates = e.completer();
            // Prefix matches first, then substring matches (for e.g. "sonnet" → "claude-sonnet-4-6").
            std::vector<std::string> prefix_matches, substr_matches;
            for (const std::string& c : candidates) {
                std::string cl = to_lower(c);
                if (cl.size() >= want.size() &&
                    cl.compare(0, want.size(), want) == 0)
                    prefix_matches.push_back(c);
                else if (!want.empty() && cl.find(want) != std::string::npos)
                    substr_matches.push_back(c);
            }
            for (const std::string& c : substr_matches) prefix_matches.push_back(c);
            std::lock_guard lk(*state_mtx_);
            if (prefix_matches.empty()) {
                *model_complete_ = ModelComplete{};
            } else {
                model_complete_->active = true;
                model_complete_->items = std::move(prefix_matches);
                model_complete_->prefix = prefix;
                model_complete_->label = e.name.substr(1);  // strip the '/'
                if (model_complete_->sel >=
                    static_cast<int>(model_complete_->items.size()))
                    model_complete_->sel = 0;
            }
            return;  // first match wins
        }
        // Nothing matched: dismiss the popup.
        std::lock_guard lk(*state_mtx_);
        *model_complete_ = ModelComplete{};
    }

    // Accept the selected completion (splice into input). pre: model_complete->active.
    void accept_complete() {
        std::string chosen, prefix;
        {
            std::lock_guard lk(*state_mtx_);
            if (!model_complete_->active || model_complete_->sel < 0 ||
                model_complete_->sel >=
                    static_cast<int>(model_complete_->items.size()))
                return;
            chosen = model_complete_->items[model_complete_->sel];
            prefix = model_complete_->prefix;
        }
        std::string_view line = *input_;
        auto arg_start = prefix.size();
        auto arg_end = line.find(' ', arg_start);
        if (arg_end == std::string_view::npos) arg_end = line.size();
        *input_ = std::string(line.substr(0, arg_start)) + chosen + " " +
                  std::string(line.substr(arg_end));
        *cursor_ = static_cast<int>(arg_start + chosen.size() + 1);
        {
            std::lock_guard lk(*state_mtx_);
            *model_complete_ = ModelComplete{};
        }
    }

private:
    std::vector<Entry> entries_;
    std::string* input_;
    int* cursor_;
    std::mutex* state_mtx_;
    ModelComplete* model_complete_;
};

// Hit-test: which pane does a mouse position target?
enum class Pane { Chat, Activity, Detail, None };

Pane wheel_target(int mouse_x, int mouse_y, int term_w, int term_h,
                  int activity_w, int detail_h) {
    int body_top = 2;           // status bar + separator
    int body_bot = term_h - 1;  // above footer
    int activity_left = term_w - activity_w;
    if (mouse_y < body_top || mouse_y >= body_bot) return Pane::None;
    if (mouse_x >= activity_left) {
        int detail_top = body_bot - detail_h;
        return (mouse_y >= detail_top) ? Pane::Detail : Pane::Activity;
    }
    return Pane::Chat;
}

}  // namespace

Zone next_zone(Zone z) {
    return z == Zone::Input      ? Zone::Activity
           : z == Zone::Activity ? Zone::Chat
           : z == Zone::Chat     ? Zone::Subagents
                                 : Zone::Input;
}

RowClick classify_row_click(const NodeKey& row, bool expander,
                            const std::optional<NodeKey>& selection) {
    // Expander hits precede their row's in the hit map, so a glyph click folds
    // rather than selects. A click on the already-selected row inspects it
    // (maximize); any other row click selects it.
    if (expander) return RowClick::Fold;
    if (selection && *selection == row) return RowClick::Maximize;
    return RowClick::Select;
}

bool show_subagents_browser(Zone zone, const TuiState& st) {
    if (zone == Zone::Subagents) return true;
    const auto& sel = st.selection();
    if (!sel) return true;
    if (sel->pane == NodeKey::Pane::Subagents) return true;
    if (sel->pane == NodeKey::Pane::Activity && sel->parent_id.empty() &&
        st.find_group(sel->id))
        return true;  // a spawn_subagent group row is selected
    // A chat entry or plain call that still resolves shows its own content;
    // an unresolvable selection falls back to the browser.
    if (sel->pane == NodeKey::Pane::Chat)
        return sel->chat_index >= st.chat().size();
    return st.find_activity(*sel) == nullptr;
}

int run_tui(Agent& agent, Permissions* perms, TuiInfo info,
            std::shared_ptr<FileChangeFn> sink,
            QuestionGate* question_gate_ptr,
            std::shared_ptr<SubagentActivityFn> on_subagent_activity,
            std::shared_ptr<SubagentTextFn> on_subagent_text,
            SkillRegistry* skills, ToolRegistry* tool_reg,
            std::shared_ptr<std::function<bool(
                std::string_view, const std::filesystem::path&,
                const std::string&)>>
                escape_approver) {
    // Status-bar strings render verbatim every frame; sanitize them once.
    // (api_key/home are never rendered; profile/model may be swapped later but
    // only with values from settings pickers, typed input, or these fields.)
    info.model = sanitize_tui_text(info.model);
    info.base_url = sanitize_tui_text(info.base_url);
    info.cwd = sanitize_tui_text(info.cwd);
    info.provider = sanitize_tui_text(info.provider);
    info.profile = sanitize_tui_text(info.profile);

    // Resolve the decorative-glyph mode once, on this (main) thread, before any
    // worker thread renders — so ASCII fallback on a legacy Windows console is
    // consistent across the whole session.
    set_glyph_mode(detect_glyph_mode());

    // Configure the console for UTF-8 + VT output (Windows); no-op on POSIX.
    // Lives for the whole TUI; restores the prior console state on return.
    tui_platform::ConsoleGuard console_guard;

    TuiState state;
    std::mutex state_mtx;
    ApprovalGate gate;

    // One-time startup note (e.g. "reasoning controls may be ignored by …").
    if (!info.notice.empty()) state.push_info(info.notice);

    // Apply the persisted code-block syntax theme (defaults to Default).
    if (!info.home.empty())
        if (auto t = syntax_theme_from_name(load_settings(info.home).theme))
            state.set_syntax_theme(*t);

    auto screen = ScreenInteractive::Fullscreen();
    screen.ForceHandleCtrlC(false);  // we handle Ctrl+C ourselves (double-press)

    auto post = [&screen] { screen.PostEvent(Event::Custom); };
    gate.on_request(post);

    // QuestionGate: the ask_user tool blocks the worker thread on this gate;
    // the UI thread resolves it via the question modal. When no gate was passed
    // (non-interactive), create a local one so the rest of the code path stays
    // uniform — but the tool will see nullptr and fall back to stdin.
    QuestionGate local_question_gate;
    QuestionGate& question_gate = question_gate_ptr ? *question_gate_ptr
                                                     : local_question_gate;
    if (question_gate_ptr) question_gate.on_request(post);

    // Conversation identity for auto-save. conv_path is minted lazily on the
    // first user submit (unless a conversation is loaded, which adopts its file).
    // meta/conv_path are read+written from both the UI and worker threads, so
    // guard them with state_mtx (the same lock the render model uses).
    std::string conv_path;       // guarded by state_mtx
    ConvMeta meta;               // guarded by state_mtx
    meta.cwd = info.cwd;
    meta.model = info.model;

    // ISO-8601 UTC "now" for created/updated stamps.
    auto now_iso = [] {
        std::time_t t = std::time(nullptr);
        std::tm tm{};
#ifdef _WIN32
        if (::gmtime_s(&tm, &t) != 0) return std::string();
#else
        if (!::gmtime_r(&t, &tm)) return std::string();
#endif
        char buf[32];
        std::strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%SZ", &tm);
        return std::string(buf);
    };

    // Persist the current history to conv_path. Called on the worker thread (no
    // UI race on history) after each run, and once on clean exit. No-op when
    // persistence is off or no conversation has been started yet.
    auto save_now = [&] {
        std::string path;
        ConvMeta m;
        {
            std::lock_guard lk(state_mtx);
            if (conv_path.empty()) return;
            m = meta;
            m.updated = now_iso();
            meta.updated = m.updated;
            path = conv_path;
        }
        if (auto r = save_conversation(path, agent.history(), m); !r) {
            std::lock_guard lk(state_mtx);
            state.push_error("autosave failed: " + r.error().msg);
        }
    };

    // File-change diffs: when the sink is provided, point it at a handler that
    // pushes a colored diff entry into the chat log (worker thread => lock+post).
    if (sink)
        *sink = [&](const FileChange& fc) {
            auto d = diff_lines(fc.old_content, fc.new_content);
            {
                std::lock_guard lk(state_mtx);
                state.push_diff(fc.path, std::move(d));
            }
            post();
        };

    // Live token accounting for the status bar, split into input (prompt /
    // context) and output (generated). `last_input` holds the prompt size of the
    // call in flight; `last_output` the last real completion count. While a call
    // streams we estimate output from the bytes seen (~4 bytes/token) so the
    // counter climbs in real time, and re-estimate input from the conversation
    // at the first delta of each API call (the prior answer + tool results have
    // already been appended). On_usage then overwrites both with the API's real
    // counts. `turn_chars == 0` marks the first delta of a fresh call: usage
    // resets it after every reply.
    int last_input = 0;
    int last_output = 0;
    int turn_chars = 0;
    agent.on_delta([&](std::string_view a, std::string_view r) {
        {
            std::lock_guard lk(state_mtx);
            state.apply_delta(a, r);
            if (turn_chars == 0)  // first delta of this call: refresh input est.
                last_input = estimated_tokens(agent.history());
            turn_chars += static_cast<int>(a.size() + r.size());
            last_output = std::max(1, turn_chars / 4);
            state.set_usage(last_input, last_output);
        }
        post();
    });
    agent.on_message([&](const Message& m) {
        {
            std::lock_guard lk(state_mtx);
            state.apply_message(m);
        }
        post();
    });
    agent.on_usage([&](const Usage& u) {
        {
            std::lock_guard lk(state_mtx);
            last_input = u.prompt_tokens;
            last_output = u.completion_tokens;
            state.set_usage(last_input, last_output);
            turn_chars = 0;  // next call streams its own live estimate
        }
        post();
    });

    // Subagent activity: when a shared_ptr was provided (TUI path), write the
    // real callback into it so the subagent tool can push nested entries.
    if (on_subagent_activity) {
        *on_subagent_activity = [&state, &state_mtx, &post](
            std::string id, std::string name, std::string args,
            SubagentActivityStatus st, std::string preview) {
            TuiState::Status s = (st == SubagentActivityStatus::Running)
                ? TuiState::Status::Running
                : (st == SubagentActivityStatus::Ok)
                    ? TuiState::Status::Ok
                    : TuiState::Status::Failed;
            {
                std::lock_guard lk(state_mtx);
                state.push_subagent_activity(std::move(id), std::move(name),
                                             std::move(args), s,
                                             std::move(preview));
            }
            post();
        };
    }

    // Subagent text: when a shared_ptr was provided, push assistant prose into
    // the active group's turn list so the detail pane shows the full
    // conversation (not just tool calls).
    if (on_subagent_text) {
        *on_subagent_text = [&state, &state_mtx, &post](std::string text) {
            {
                std::lock_guard lk(state_mtx);
                state.push_subagent_text(std::move(text));
            }
            post();
        };
    }

    // Gate every tool that isn't already allowed, prompting through the modal.
    if (perms) {
        agent.on_approve([perms, &gate](const ToolCall& tc) {
            if (tc.name == "ask_user") return true;  // the question IS the interaction
            return decide(*perms, tc,
                          [&gate](const ToolCall& c) { return gate.request(c); });
        });

        // The "outside root" permission tier: when a filesystem tool resolves a
        // path that escapes the project root, prompt through the same modal but
        // under a distinct `<kind>_outside_root` key, so granting escape access
        // is separate from allowing the tool itself. Runs on the worker thread
        // (gate.request blocks it until the UI answers), matching on_approve.
        if (escape_approver) {
            *escape_approver = [perms, &gate](std::string_view kind,
                                              const std::filesystem::path& resolved,
                                              const std::string&) {
                ToolCall tc;
                tc.name = std::string(kind) + "_outside_root";
                tc.arguments_json = resolved.string();
                return decide(*perms, tc,
                              [&gate](const ToolCall& c) { return gate.request(c); });
            };
        }
    }

    // Result of a worker-thread model-detection (list_models) call, handed back
    // to the UI thread which raises the selection picker / reports the outcome.
    // Guarded by state_mtx; `pending` is drained at the top of CatchEvent.
    struct DetectResult {
        bool pending = false;
        bool ok = false;
        bool silent = false;              // background startup refresh, no picker
        std::string error;                // set when !ok
        std::string base;                 // endpoint queried, for messages
        std::string profile;              // profile to attach models to ("" => none)
        std::vector<std::string> models;  // detected ids (may be empty)
        // Startup all-profiles sweep: per-profile (name, ids) to apply in place.
        std::vector<std::pair<std::string, std::vector<std::string>>> bulk;
    };
    DetectResult detect;

    std::atomic<std::size_t> frame{0};  // spinner tick, advanced off the render path

    // Messages typed while the agent is streaming. This is a *buffer*, not a
    // strict queue: the agent's inject hook flushes the whole buffer into the
    // very next provider request — including each mid-turn tool-use round-trip,
    // not just after the turn ends — and the worker flushes anything typed after
    // the turn's final request as a fresh turn. @-mention expansion and image
    // scanning are done at submit time (UI thread); the worker / inject hook
    // just hand agent.run() the pre-expanded data.
    struct QueuedPrompt {
        std::string expanded_prompt;            // after @-mention expansion
        std::vector<ContentPart> user_parts;    // text + images
        std::string original_line;              // what the user typed (for history / chat)
        std::vector<MentionEntry> entries;      // for the 📎 info line
        std::size_t total_bytes = 0;            // for the info line
        std::vector<ImageBlock> scanned_images; // from scan_image_paths
        std::vector<std::string> scan_errors;   // from scan_image_paths
    };
    std::deque<QueuedPrompt> pending_queue;  // guarded by state_mtx
    // Set by Esc: drop the buffer instead of flushing it, so queued messages
    // never auto-fire after an interrupt. Guarded by state_mtx.
    bool discard_buffer = false;

    // Mirror a buffered prompt into the chat log exactly as submit() does: the
    // user line, then the @-mention / image attachment summaries. Used by both
    // the inject hook (mid-turn flush) and the worker (post-turn flush).
    // pre: caller holds state_mtx.
    auto show_buffered = [&](const QueuedPrompt& qp) {
        state.set_parent_model(info.model);
        state.push_user(qp.original_line);
        if (!qp.entries.empty()) {
            std::size_t ok = 0, err = 0;
            for (const auto& e : qp.entries) {
                if (e.error.empty()) ++ok;
                else ++err;
            }
            std::ostringstream ai;
            ai << "📎 " << ok << " file(s) attached via @-mentions ("
               << qp.total_bytes << " B, " << err << " error(s))";
            for (const auto& e : qp.entries) {
                if (!e.error.empty())
                    ai << "\n  ! " << e.path << ": " << e.error;
                else
                    ai << "\n  · " << e.path << "  (" << e.kind << ", "
                       << e.lines << " lines)";
            }
            state.push_info(ai.str());
        }
        if (!qp.scanned_images.empty()) {
            std::ostringstream si;
            si << "🖼 " << qp.scanned_images.size()
               << " image(s) detected in prompt";
            for (const auto& img : qp.scanned_images)
                si << "\n  · " << img.media_type << " ("
                   << human_tokens(static_cast<int>(img.base64_data.size() / 4))
                   << " tok)";
            for (const auto& e : qp.scan_errors) si << "\n  ! " << e;
            state.push_info(si.str());
        }
    };

    // Flush the whole buffer into the next provider request. Runs on the worker
    // thread (inside agent.run); takes state_mtx to read+clear the buffer and to
    // mirror each prompt into the chat log. Returns nothing once Esc has marked
    // the buffer for discard.
    agent.on_inject([&]() -> std::vector<Message> {
        std::vector<Message> out;
        std::lock_guard lk(state_mtx);
        if (discard_buffer) return out;
        while (!pending_queue.empty()) {
            QueuedPrompt qp = std::move(pending_queue.front());
            pending_queue.pop_front();
            show_buffered(qp);  // reads original_line/entries/images (not moved below)
            out.push_back(Message::user(std::move(qp.expanded_prompt),
                                        std::move(qp.user_parts)));
        }
        return out;
    });

    // Worker thread: drains submitted work items (prompts and compactions) and
    // runs each so the UI stays responsive. Streaming callbacks (above) push into
    // `state` and request redraws. The per-kind bodies live in these closures;
    // TuiWorker owns the queue + thread (see tui_worker.hpp). They touch the
    // shared render state via state_mtx exactly as the original inline thread did.
    TuiWorker::Deps worker_deps;
    worker_deps.run_prompt = [&](std::string text,
                                 std::vector<ContentPart> user_parts) {
        {
            std::lock_guard lk(state_mtx);
            state.set_running(true);
            discard_buffer = false;  // fresh submit clears any prior Esc
        }
        post();

        // Run the primary prompt. Messages typed *during* the turn are folded
        // into it by the inject hook (flushed at the next provider request).
        // After it returns, loop to start a fresh turn for anything typed after
        // the turn's final request — the tail the inject hook can't reach —
        // until the buffer drains or Esc clears it.
        auto res = agent.run(text, std::move(user_parts));
        for (;;) {
            const bool cancelled =
                agent.cancel_flag().load(std::memory_order_relaxed);
            if (cancelled || !res) {
                {
                    std::lock_guard lk(state_mtx);
                    if (!res) state.push_error("error: " + res.error().msg);
                    // Interrupt or error: drop the buffer so queued messages
                    // don't auto-fire after the failure.
                    pending_queue.clear();
                    discard_buffer = false;
                    state.set_running(false);
                }
                save_now();  // outside the lock: save_now() takes state_mtx
                post();
                return;
            }
            save_now();  // persist the completed turn (history is settled)

            QueuedPrompt qp;
            {
                std::lock_guard lk(state_mtx);
                if (discard_buffer || pending_queue.empty()) {
                    state.set_running(false);
                    break;
                }
                qp = std::move(pending_queue.front());
                pending_queue.pop_front();
                show_buffered(qp);
            }
            post();
            res = agent.run(std::move(qp.expanded_prompt),
                            std::move(qp.user_parts));
        }
        post();
    };
    worker_deps.run_detect = [&](std::string profile, bool silent) {
        // Detect models off the UI thread (the blocking GET would freeze
        // the render loop) so the spinner keeps moving. The UI thread has
        // already set running=true + pushed a "detecting …" line (foreground
        // path only). We only stash the result here; raising the picker /
        // reporting happens on the UI thread (CatchEvent), where the switch
        // closures are live. `silent` requests the in-place refresh instead.
        auto models = agent.provider().list_models();
        std::string base = agent.provider().base_url();
        std::lock_guard lk(state_mtx);
        if (!silent) state.set_running(false);
        detect = DetectResult{};
        detect.pending = true;
        detect.silent = silent;
        detect.base = std::move(base);
        detect.profile = std::move(profile);
        if (models) {
            detect.ok = true;
            detect.models = std::move(*models);
        } else {
            detect.ok = false;
            detect.error = models.error().msg;
        }
        post();
    };
    worker_deps.run_detect_all = [&]() {
        // Startup sweep: refresh every profile's model set from its own endpoint.
        // Each profile gets a provider built from its kind/base_url + per-profile
        // credential, queried off the UI thread. Profiles snapshotted under the
        // lock first; the (blocking) network calls run unlocked; results are
        // stashed for the UI thread to apply silently (no picker, no model swap).
        std::string home;
        {
            std::lock_guard lk(state_mtx);
            home = info.home;
        }
        std::vector<Profile> snapshot =
            home.empty() ? default_profiles() : load_settings(home).profiles;
        if (snapshot.empty()) snapshot = default_profiles();
        auto creds = home.empty() ? std::map<std::string, std::string>{}
                                  : load_credentials(home);
        std::vector<std::pair<std::string, std::vector<std::string>>> found;
        for (const Profile& p : snapshot) {
            ProviderConnection conn;
            conn.kind = profile_kind(p);
            conn.base_url = p.base_url;
            conn.model = p.model;
            if (auto it = creds.find(p.name); it != creds.end())
                conn.api_key = it->second;
            auto prov = make_provider(conn, GenerationParams{});
            auto models = prov->list_models();
            if (models && !models->empty())
                found.emplace_back(p.name, std::move(*models));
        }
        std::lock_guard lk(state_mtx);
        detect = DetectResult{};
        detect.pending = true;
        detect.silent = true;
        detect.bulk = std::move(found);
        post();
    };
    worker_deps.run_compact = [&](std::string instructions) {
        // Compact: the UI thread has already set running=true and
        // pushed a "compacting …" info line. The blocking LLM summarise
        // call lives here so the UI stays responsive and Esc→cancel
        // works. On completion we update history, save, and report.
        const int before_tok = estimated_tokens(agent.history());
        auto new_conv = agent.compact(instructions);
        if (!new_conv) {
            std::lock_guard lk(state_mtx);
            state.push_error("compact failed: " + new_conv.error().msg);
        } else {
            agent.set_history(*new_conv);
            save_now();  // persist the shortened conversation in place
            std::lock_guard lk(state_mtx);
            state.load(*new_conv);
            const int after_tok = estimated_tokens(*new_conv);
            state.push_info("compacted: " + human_tokens(before_tok) + " → " +
                            human_tokens(after_tok) + " tok");
        }
        {
            std::lock_guard lk(state_mtx);
            state.set_running(false);
        }
        post();
    };
    TuiWorker worker(std::move(worker_deps));

    // Fetch every profile's live model list in the background on startup, so
    // /model + @-autocomplete reflect each endpoint without a manual /provider
    // models. Silent: it refreshes the profiles' model sets in place and never
    // raises the picker or switches the live model. Per-profile failures (no
    // key, offline) stay quiet — the seed lists in settings.toml keep working.
    worker.submit({WorkItem::DetectAllModels, {}, {}, /*silent=*/true});

    // Refresher: while a turn is running, nudge a redraw so the spinner spins
    // even when no token has arrived yet. 200 ms = 5 Hz — the spinner glyph
    // advances once per tick, lazy enough to stay off the CPU when the network
    // stall is the bottleneck.
    std::atomic<bool> ui_alive{true};
    std::thread refresher([&] {
        while (ui_alive.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            bool running;
            {
                std::lock_guard lk(state_mtx);
                running = state.running();
            }
            if (running) {
                frame.fetch_add(1, std::memory_order_relaxed);
                post();
            }
        }
    });

    // Input + history.
    const std::string hist_path = history_path(info.home);
    std::vector<std::string> history = load_history(hist_path);
    std::size_t hist_pos = history.size();
    std::string input_content;

    // Generic modal list overlay (resume/rewind). Driven entirely from the UI
    // thread, but read in the renderer, so guard it with state_mtx.
    struct Picker {
        bool active = false;
        std::string title;
        std::vector<std::string> items;
        std::vector<std::function<void()>> on_choose;  // parallel to items
        int sel = 0;
    } picker;

    // Question modal state for the ask_user tool. The question+options come from
    // the gate each frame; the UI thread owns sel / custom_mode / custom_text.
    // Guarded by state_mtx (read in renderer, written in CatchEvent).
    struct QuestionModal {
        int sel = 0;
        bool custom_mode = false;
        std::string custom_text;
    } question_modal;
    // Track pending state across frames so we can reset sel on new questions.
    bool question_was_pending = false;
    // Settings editor form state, managed by the /settings slash command.
    // The screen-free SettingsForm lives in settings_editor.{hpp,cpp}.
    SettingsForm settings_form;

    // Non-modal @-completion popup, anchored above the input line. Recomputed by
    // the Input's on_change from input_content + cursor_pos; read in the renderer
    // and CatchEvent. Guarded by state_mtx like picker.
    struct Complete {
        bool active = false;
        std::vector<MentionCompletion> items;
        MentionContext ctx;
        int sel = 0;
    } complete;

    // Slash-command argument-completion popup (driven by SlashCommands).
    ModelComplete model_complete;

    int cursor_pos = 0;  // FTXUI Input cursor (glyph index; ASCII paths => byte index)

    // Clipboard images pasted via Ctrl+V. Each carries a stable id matching an
    // `[img#id]` chip spliced into the prompt; the chip is the image's handle.
    // reconcile_images() keeps this in sync with the chips still in the text, so
    // erasing a chip detaches its image. Drained, in chip order, on submit.
    struct PastedImage {
        int id;
        ImageBlock block;
    };
    std::vector<PastedImage> clip_images;
    int next_image_id = 1;

    // Large bracketed pastes collapsed to an atomic `[Pasted text #N]` marker in
    // the prompt (see paste_chip.hpp). The marker is the paste's handle: it shows
    // compactly in the line and the chat log, behaves as a single character for
    // cursor motion/deletion, survives recall from the input history, and is
    // expanded back to the full bytes only in the text sent to the model. The
    // id→content map persists for the whole session (it is *not* cleared on
    // submit) so a paste whose marker comes back via the history (ArrowUp) still
    // resolves to its bytes on a re-submit. Ids never reset, keeping every
    // history entry's marker unambiguous.
    struct PasteBlock {
        int id;
        std::string content;
    };
    std::vector<PasteBlock> paste_blocks;
    int next_paste_id = 1;
    // Collapse a paste only once it is genuinely large; smaller pastes splice in
    // verbatim, exactly as before.
    constexpr std::size_t kPasteCollapseBytes = 200;
    // Resolve a paste id to its stored bytes (for expand_paste_chips on submit).
    auto paste_lookup = [&](int id) -> std::optional<std::string_view> {
        auto it = std::ranges::find(paste_blocks, id, &PasteBlock::id);
        if (it == paste_blocks.end()) return std::nullopt;
        return std::string_view(it->content);
    };

    // Try to read an image from the system clipboard (wl-paste → xclip).
    // Returns the ImageBlock on success. Best-effort; errors are ignored.
    auto read_clipboard_image = []() -> std::optional<ImageBlock> {
        auto try_tool = [](const char* cmd) -> std::optional<std::string> {
            FILE* p = popen(cmd, "r");
            if (!p) return std::nullopt;
            std::string data;
            char buf[65536];
            while (std::size_t nr = std::fread(buf, 1, sizeof buf, p))
                data.append(buf, nr);
            int rc = pclose(p);
            return (rc == 0 && !data.empty()) ? std::optional(data) : std::nullopt;
        };
        auto raw = try_tool("wl-paste --type image/png 2>/dev/null");
        if (!raw) raw = try_tool("xclip -selection clipboard -t image/png -o 2>/dev/null");
        if (!raw) return std::nullopt;
        // Detect media type from magic bytes.
        std::string mt = "image/png";
        if (raw->size() >= 3 &&
            static_cast<unsigned char>((*raw)[0]) == 0xFF &&
            static_cast<unsigned char>((*raw)[1]) == 0xD8)
            mt = "image/jpeg";
        else if (raw->size() >= 3 && (*raw)[0] == 'G' && (*raw)[1] == 'I' && (*raw)[2] == 'F')
            mt = "image/gif";
        else if (raw->size() >= 4 && (*raw)[0] == 'R' && (*raw)[1] == 'I' &&
                 (*raw)[2] == 'F' && (*raw)[3] == 'F')
            mt = "image/webp";
        return ImageBlock{base64_encode(*raw), std::move(mt)};
    };

    // Read an image off the clipboard and splice a visible `[img#id]` chip into
    // the prompt at the cursor. The chip — plain text in input_content — is the
    // image's handle: erasing it detaches the image (see reconcile_images()).
    auto scan_clipboard = [&] {
        auto img = read_clipboard_image();
        if (!img) return;
        const int id = next_image_id++;
        const std::string media = img->media_type;
        const int tok = static_cast<int>(img->base64_data.size() / 4);
        clip_images.push_back(PastedImage{id, std::move(*img)});
        // cursor_pos is a byte offset into input_content (FTXUI Input
        // convention); the chip is ASCII so byte length == glyph length.
        const std::string chip = image_chip(id);
        const std::size_t at =
            std::min(static_cast<std::size_t>(std::max(0, cursor_pos)),
                     input_content.size());
        input_content.insert(at, chip);
        cursor_pos = static_cast<int>(at + chip.size());
        std::lock_guard lk(state_mtx);
        state.push_info("🖼 " + chip + " attached (" + media + ", " +
                        human_tokens(tok) + " tok) — Backspace to remove");
        post();
    };

    // Detach any pasted image whose chip is no longer present in the prompt.
    // Run after every edit so erasing, cutting, or breaking a chip drops it.
    auto reconcile_images = [&] {
        if (clip_images.empty()) return;
        const auto present = extract_chip_ids(input_content);
        std::erase_if(clip_images, [&](const PastedImage& p) {
            return std::ranges::find(present, p.id) == present.end();
        });
    };

    // Clear input and reset image-chip state. Paste blocks are deliberately kept
    // (and next_paste_id keeps climbing): a `[Pasted text #N]` marker recalled
    // from the history must still expand on a later submit, so its bytes have to
    // outlive the input that produced them.
    auto clear_input = [&] {
        input_content.clear();
        cursor_pos = 0;
        clip_images.clear();
        next_image_id = 1;
    };

    // Adopt a loaded conversation: swap the agent's history, rebuild the render
    // model, and adopt its meta + file so subsequent saves overwrite it.
    auto load_into = [&](const std::string& path) {
        auto loaded = load_conversation_with_meta(path);
        if (!loaded) {
            std::lock_guard lk(state_mtx);
            state.push_error("error: " + loaded.error().msg);
            return;
        }
        agent.set_history(loaded->first);
        std::lock_guard lk(state_mtx);
        state.load(loaded->first);
        meta = loaded->second;
        if (meta.cwd.empty()) meta.cwd = info.cwd;
        if (meta.model.empty()) meta.model = info.model;
        conv_path = path;
    };

    // Provider profiles drive endpoint-aware /model and /provider. Loaded once
    // from settings.toml; falls back to the built-in defaults (equivalent to the
    // former hardcoded model_registry) when the user configured none, so
    // behaviour is unchanged out of the box. Credentials (per-profile API keys)
    // are kept separate and never rendered. `profiles` is mutable so /provider
    // save can add to it in memory.
    std::vector<Profile> profiles = load_settings(info.home).profiles;
    if (profiles.empty()) profiles = default_profiles();
    std::map<std::string, std::string> credentials = load_credentials(info.home);
    // Request-param passthrough allowlists, carried into any provider rebuilt on
    // a runtime wire-format switch so /model swaps keep the (url,model) gating.
    std::vector<AllowedOpenAiParams> allowed_openai_params =
        load_settings(info.home).allowed_openai_params;
    // Same for the per-(url,model) reasoning_effort drop list.
    std::vector<ModelEndpoint> drop_reasoning_effort =
        load_settings(info.home).drop_reasoning_effort;

    // Flat, sorted, de-duplicated model name list for /model autocomplete.
    auto rebuild_model_names = [&] {
        std::vector<std::string> names;
        for (const Profile& p : profiles)
            for (const std::string& m : p.models) names.push_back(m);
        std::ranges::sort(names);
        names.erase(std::ranges::unique(names).begin(), names.end());
        return names;
    };
    std::vector<std::string> model_names = rebuild_model_names();

    // Find the profile whose `models` contains `name` (case-insensitive). The
    // first match in profile order wins. Returns nullptr when no profile serves
    // it (then /model only changes the model string, keeping the endpoint).
    auto find_model_profile = [&](std::string_view name) -> const Profile* {
        return moocode::find_model_profile(name, profiles);
    };

    // The blacklist of the named profile (empty/unknown name => the active
    // profile's, else none). Detected model sets are filtered through this so a
    // dead-but-advertised id never reaches the picker, autocomplete, or disk.
    auto blacklist_for = [&](const std::string& name) -> std::vector<std::string> {
        const std::string& key = name.empty() ? info.profile : name;
        for (const Profile& p : profiles)
            if (p.name == key) return p.blacklist;
        return {};
    };

    // The session key for the active profile (credentials.toml else the startup
    // key). Used to seed the rebuilt provider's auth on a /model or /provider
    // switch. Empty profile name => the current session key.
    auto key_for_profile = [&](const std::string& name) -> std::string {
        if (auto it = credentials.find(name); it != credentials.end())
            return it->second;
        return info.api_key;
    };

    // Switch the active connection: rebuild the provider object when the wire
    // format changes (so requests use the right format), else just retune the
    // existing one. Carries the live generation params (effort/temp/thinking)
    // across a rebuild so the TUI's session controls survive. Updates info.* so
    // the status bar and a later /provider save reflect the new connection.
    // pre: no run() in progress (slash commands are refused while busy).
    auto switch_connection = [&](ProviderKind k, const std::string& base,
                                 const std::string& key, const std::string& mdl,
                                 const std::string& profile_name,
                                 const std::string& thinking_type = {}) {
        const bool same_kind = provider_kind_name(k) == info.provider;
        if (same_kind) {
            agent.provider().set_model(mdl);
            agent.provider().set_base_url(base);
            agent.provider().set_api_key(key);
            // Same wire format but possibly a different endpoint's thinking
            // dialect (e.g. z.ai/DeepSeek want "enabled", MiniMax wants
            // "adaptive"); retune it so the next request is accepted.
            agent.provider().set_thinking_type(thinking_type);
        } else {
            // carry includes max_tokens, so the connection's max_tokens stays 0
            // (provider default) and set_params(carry) reapplies the live cap.
            GenerationParams carry = agent.provider().params();
            agent.set_provider(
                make_provider(ProviderConnection{.kind = k, .base_url = base, .api_key = key, .model = mdl, .max_tokens = 0, .thinking_type = thinking_type, .allowed_openai_params = allowed_openai_params, .drop_reasoning_effort = drop_reasoning_effort}, carry));
        }
        info.model = mdl;
        info.base_url = base;
        info.api_key = key;
        info.provider = provider_kind_name(k);
        info.profile = profile_name;
        meta.model = mdl;
    };

    // Apply a model chosen from the detection picker: switch the live model and,
    // when a profile is named, replace its model + model set and persist them to
    // settings.toml. Runs on the UI thread from the picker callback (state_mtx
    // not held by the caller), so it takes the lock itself.
    auto apply_detected_models = [&](const std::string& profile_name,
                                     const std::string& chosen,
                                     const std::vector<std::string>& all) {
        std::lock_guard lk(state_mtx);
        agent.provider().set_model(chosen);
        info.model = chosen;
        meta.model = chosen;
        if (!profile_name.empty()) {
            Profile* slot = nullptr;
            for (Profile& p : profiles)
                if (p.name == profile_name) { slot = &p; break; }
            if (slot) {
                slot->model = chosen;
                slot->models = all;
            } else {
                profiles.push_back(Profile{.name = profile_name,
                                           .kind = info.provider,
                                           .base_url = info.base_url,
                                           .model = chosen,
                                           .models = all});
            }
            info.profile = profile_name;
            if (!info.home.empty()) {
                Settings s = load_settings(info.home);
                bool merged = false;
                for (Profile& p : s.profiles)
                    if (p.name == profile_name) {
                        p.kind = info.provider;
                        p.base_url = info.base_url;
                        p.model = chosen;
                        p.models = all;
                        merged = true;
                        break;
                    }
                if (!merged)
                    s.profiles.push_back(Profile{profile_name, info.provider,
                                                 info.base_url, chosen, all});
                save_settings(info.home, s);
            }
        }
        model_names = rebuild_model_names();
        state.push_info("model → " + chosen +
                        (profile_name.empty()
                             ? std::string()
                             : "  (" + std::to_string(all.size()) +
                                   " models saved to '" + profile_name + "')"));
    };

    // Queue a model-detection request for the live endpoint, attaching results
    // to `profile_name` ("" => just the live model). Shows a visible "detecting"
    // state; the worker fetches and the UI raises a picker on completion — never
    // a silent background change. pre: caller holds state_mtx (all callers are
    // slash handlers that lock it for their whole body), so this must not relock.
    auto request_detection = [&](const std::string& profile_name) {
        state.set_running(true);
        state.push_info("detecting models from " + info.base_url + " …");
        worker.submit({WorkItem::DetectModels, profile_name, {}});
        post();
    };

    // Profile names + subcommands for /provider autocomplete (re-reads profiles
    // each frame so /provider save additions are picked up live).
    auto profile_names = [&] {
        std::vector<std::string> names{"list", "models", "save"};
        for (const Profile& p : profiles) names.push_back(p.name);
        std::ranges::sort(names);
        return names;
    };

    // Slash command registry: one registration per command with an optional
    // argument completer. The dispatch and autocomplete are driven from here;
    // adding a new command is a single Entry push.
    SlashCommands slash(
        {
            {"/model", {},
             [&](std::string_view arg) {
                 std::lock_guard lk(state_mtx);
                 // Split off a leading subcommand word ("blacklist"/"unblacklist").
                 const std::string a(arg);
                 std::string sub = a, rest;
                 if (auto sp = a.find_first_of(" \t"); sp != std::string::npos) {
                     sub = a.substr(0, sp);
                     auto b = a.find_first_not_of(" \t", sp);
                     rest = (b == std::string::npos) ? std::string() : a.substr(b);
                 }
                 const std::string subl = to_lower(sub);
                 if (subl == "blacklist" || subl == "unblacklist") {
                     const bool add = subl == "blacklist";
                     auto ieq = [](std::string_view x, std::string_view y) {
                         return to_lower(x) == to_lower(y);
                     };
                     Profile* slot = nullptr;
                     for (Profile& p : profiles)
                         if (p.name == info.profile) { slot = &p; break; }
                     if (!slot) {
                         state.push_error(
                             "/model " + subl +
                             " needs an active saved profile (use /provider "
                             "save <name> first)");
                         return;
                     }
                     std::string id = rest;
                     if (id.empty() && add) id = info.model;  // default: current
                     if (id.empty()) {  // bare /model blacklist => list it
                         std::string msg = "blacklist for '" + slot->name + "':";
                         if (slot->blacklist.empty()) msg += " (empty)";
                         for (const std::string& b : slot->blacklist)
                             msg += "\n  " + b;
                         state.push_info(msg);
                         return;
                     }
                     if (add) {
                         if (std::ranges::none_of(slot->blacklist,
                                 [&](const std::string& b) { return ieq(b, id); }))
                             slot->blacklist.push_back(id);
                         std::erase_if(slot->models,
                             [&](const std::string& m) { return ieq(m, id); });
                     } else {
                         std::erase_if(slot->blacklist,
                             [&](const std::string& b) { return ieq(b, id); });
                     }
                     // Don't leave the profile's saved default pointing at a
                     // blacklisted id, or startup would re-select it.
                     if (add && ieq(slot->model, id))
                         slot->model = slot->models.empty()
                                           ? std::string()
                                           : slot->models.front();
                     model_names = rebuild_model_names();
                     if (!info.home.empty()) {  // persist blacklist + model set
                         Settings s = load_settings(info.home);
                         bool merged = false;
                         for (Profile& p : s.profiles)
                             if (p.name == slot->name) {
                                 p.blacklist = slot->blacklist;
                                 p.models = slot->models;
                                 p.model = slot->model;
                                 merged = true;
                                 break;
                             }
                         if (!merged) s.profiles.push_back(*slot);
                         save_settings(info.home, s);
                     }
                     // If the just-blacklisted id is the live model, move off it.
                     if (add && ieq(info.model, id)) {
                         std::string next;
                         if (!slot->model.empty() && !ieq(slot->model, id))
                             next = slot->model;
                         else if (!slot->models.empty())
                             next = slot->models.front();
                         if (!next.empty()) {
                             agent.provider().set_model(next);
                             info.model = next;
                             meta.model = next;
                             state.push_info("blacklisted " + id +
                                             " — model → " + next);
                         } else {
                             state.push_error(
                                 "blacklisted " + id +
                                 " (still the live model; no replacement left)");
                         }
                     } else {
                         state.push_info((add ? "blacklisted " : "un-blacklisted ") +
                                         id + "  ('" + slot->name + "')");
                     }
                     return;
                 }
                 if (arg.empty()) {
                     state.push_error(
                         "usage: /model <name> | /model blacklist <id> | "
                         "/model unblacklist <id>");
                 } else if (const Profile* p = find_model_profile(arg)) {
                     ProviderKind k = profile_kind(*p);
                     std::string key = key_for_profile(p->name);
                     switch_connection(k, p->base_url, key, std::string(arg),
                                       p->name, p->thinking_type);
                     state.push_info(
                         "model → " + std::string(arg) + "  (" + info.provider +
                         " · " + info.base_url +
                         (key.empty() ? "" : " · key " + mask_key(key)) + ")");
                 } else {
                     agent.provider().set_model(std::string(arg));
                     info.model = arg;
                     meta.model = arg;
                     state.push_info("model → " + std::string(arg));
                 }
             },
             [&] {
                 std::vector<std::string> c = model_names;
                 c.push_back("blacklist");
                 c.push_back("unblacklist");
                 return c;
             }},
            {"/provider", {},
             [&](std::string_view arg) {
                 std::lock_guard lk(state_mtx);
                 const std::string sa = std::string(arg);
                 const std::string sub = to_lower(arg);
                 std::string sub_word = sa, sub_rest;
                 if (auto sp = sa.find(' '); sp != std::string::npos) {
                     sub_word = sa.substr(0, sp);
                     auto a = sa.find_first_not_of(" \t", sp);
                     sub_rest = (a == std::string::npos) ? std::string()
                                                         : sa.substr(a);
                 }
                 if (arg.empty() || sub == "list") {
                     std::string msg = "profiles:";
                     if (profiles.empty()) msg += " (none)";
                     for (const Profile& p : profiles) {
                         msg += "\n  " + p.name +
                                (p.name == info.profile ? "  *" : "");
                         msg += "  (" +
                                (p.kind.empty() ? std::string("auto") : p.kind) +
                                " · " + p.base_url + ")";
                     }
                     state.push_info(msg);
                 } else if (sub == "models" || sub == "refresh") {
                     // Detect models from the live endpoint, attaching the
                     // result to the active profile (if any).
                     request_detection(info.profile);
                 } else if (to_lower(sub_word) == "save") {
                     std::string name = sub_rest.empty() ? info.profile : sub_rest;
                     if (name.empty()) {
                         state.push_error(
                             "usage: /provider save <name> (no active profile)");
                     } else {
                         Profile* slot = nullptr;
                         for (Profile& p : profiles)
                             if (p.name == name) { slot = &p; break; }
                         if (!slot) {
                             profiles.push_back(
                                 Profile{.name = name,
                                         .kind = info.provider,
                                         .base_url = info.base_url,
                                         .model = info.model,
                                         .models = {info.model}});
                         } else {
                             slot->kind = info.provider;
                             slot->base_url = info.base_url;
                             slot->model = info.model;
                             if (std::ranges::find(slot->models, info.model) ==
                                 slot->models.end())
                                 slot->models.push_back(info.model);
                         }
                         model_names = rebuild_model_names();
                         info.profile = name;
                         credentials[name] = info.api_key;
                         if (!info.home.empty()) {
                             Settings s = load_settings(info.home);
                             Profile np{name, info.provider, info.base_url,
                                        info.model, {info.model}};
                             bool merged = false;
                             for (Profile& p : s.profiles)
                                 if (p.name == name) {
                                     p.kind = np.kind;
                                     p.base_url = np.base_url;
                                     p.model = np.model;
                                     if (std::ranges::find(p.models, info.model) ==
                                         p.models.end())
                                         p.models.push_back(info.model);
                                     merged = true;
                                     break;
                                 }
                             if (!merged) s.profiles.push_back(np);
                             s.profile = name;
                             save_settings(info.home, s);
                             save_credential(info.home, name, info.api_key);
                         }
                         state.push_info("saved profile '" + name + "' (key " +
                                         mask_key(info.api_key) + ")");
                         // Default autodetect: offer the endpoint's full model
                         // set so the profile isn't stuck with just one entry.
                         request_detection(name);
                     }
                 } else {
                     const Profile* p = nullptr;
                     for (const Profile& q : profiles)
                         if (to_lower(q.name) == sub) { p = &q; break; }
                     if (!p) {
                         state.push_error("unknown profile: " + sa);
                     } else {
                         ProviderKind k = profile_kind(*p);
                         std::string key = key_for_profile(p->name);
                         std::string mdl = p->model.empty() ? info.model : p->model;
                         // Decide up front whether this profile's model set is
                         // unusable (no models / missing key / model absent) so
                         // we can autodetect after switching.
                         const bool needs_detect =
                             p->models.empty() || p->model.empty() ||
                             std::ranges::find(p->models, p->model) ==
                                 p->models.end();
                         const std::string prof_name = p->name;
                         switch_connection(k, p->base_url, key, mdl, prof_name,
                                           p->thinking_type);
                         state.push_info(
                             "provider → " + prof_name + "  (" + info.provider +
                             " · " + info.base_url + " · " + info.model +
                             (key.empty() ? "" : " · key " + mask_key(key)) + ")");
                         if (needs_detect) {
                             state.push_info(
                                 "no usable model set for '" + prof_name +
                                 "' — detecting from the endpoint");
                             request_detection(prof_name);
                         }
                     }
                 }
             },
             profile_names},
            // Generation-control handlers: the parse/validate decision lives in
            // tui_slash.cpp (pure, unit-tested); the thin closure here applies
            // the result under state_mtx and adds the OpenAI reasoning-hint note.
            {"/effort", {},
             [&](std::string_view arg) {
                 GenControlResult r = parse_effort(arg);
                 std::lock_guard lk(state_mtx);
                 if (!r.error.empty()) {
                     state.push_error(r.error);
                 } else {
                     agent.provider().set_params(r.params);
                     state.push_info(r.info);
                     if (r.reasoning_hint && info.provider == "openai" &&
                         !openai_model_likely_reasoning(info.model))
                         state.push_info("note: " + info.model +
                                         " may ignore reasoning controls");
                 }
             },
             {}},
            {"/thinking", {},
             [&](std::string_view arg) {
                 GenControlResult r = parse_thinking(arg);
                 std::lock_guard lk(state_mtx);
                 if (!r.error.empty()) {
                     state.push_error(r.error);
                 } else {
                     agent.provider().set_params(r.params);
                     state.push_info(r.info);
                     if (r.reasoning_hint && info.provider == "openai" &&
                         !openai_model_likely_reasoning(info.model))
                         state.push_info("note: " + info.model +
                                         " may ignore reasoning controls");
                 }
             },
             {}},
            {"/temp", {"/temperature"},
             [&](std::string_view arg) {
                 GenControlResult r = parse_temp(arg);
                 std::lock_guard lk(state_mtx);
                 if (!r.error.empty()) {
                     state.push_error(r.error);
                 } else {
                     agent.provider().set_params(r.params);
                     state.push_info(r.info);
                 }
             },
             {}},
            {"/theme", {},
             [&](std::string_view arg) {
                 ThemeResult r = parse_theme(arg);
                 std::lock_guard lk(state_mtx);
                 if (r.list) {
                     std::string list;
                     for (const std::string& n : syntax_theme_names()) {
                         if (!list.empty()) list += ", ";
                         list += n;
                         if (syntax_theme_from_name(n) == state.syntax_theme())
                             list += " (active)";
                     }
                     state.push_info("code-block themes: " + list);
                     return;
                 }
                 if (!r.error.empty()) {
                     state.push_error(r.error);
                     return;
                 }
                 const SyntaxTheme t = *r.theme;
                 state.set_syntax_theme(t);
                 state.push_info("theme → " +
                                 std::string(syntax_theme_name(t)));
                 if (!info.home.empty()) {
                     Settings s = load_settings(info.home);
                     s.theme = std::string(syntax_theme_name(t));
                     save_settings(info.home, s);
                 }
             },
             [&] { return syntax_theme_names(); }},
            {"/settings", {},
             [&](std::string_view /*arg*/) {
                 std::lock_guard lk(state_mtx);
                 if (settings_form.active) {
                     // Close: persist if dirty.
                     FormAction act = settings_form_close(settings_form);
                     if (act == FormAction::Saved && !info.home.empty()) {
                         save_settings(info.home, settings_form.draft);
                         state.push_info("settings saved to " + info.home +
                                         "/settings.toml");
                         // Reload profiles and theme from the saved settings.
                         profiles = load_settings(info.home).profiles;
                         if (profiles.empty()) profiles = default_profiles();
                         model_names = rebuild_model_names();
                         if (auto t = syntax_theme_from_name(
                                 settings_form.draft.theme))
                             state.set_syntax_theme(*t);
                     }
                 } else {
                     // Open: populate from current settings.
                     Settings s = load_settings(info.home);
                     settings_form = settings_form_build(s);
                     settings_form.active = true;
                 }
             },
             {}},
            {"/skill", {"/skills"},
             [&](std::string_view arg) {
                 std::lock_guard lk(state_mtx);
                 if (!skills || !tool_reg) {
                     state.push_error("skills are not available in this session");
                     return;
                 }
                 // No argument: list registered skills and their load state.
                 if (arg.empty()) {
                     std::string msg = "skills:";
                     auto listing = skills->list();
                     if (listing.empty()) msg += " (none)";
                     for (const auto& s : listing) {
                         msg += "\n  " + s.name + (s.loaded ? "  *" : "");
                         if (!s.description.empty()) msg += "  — " + s.description;
                     }
                     msg += "\n(usage: /skill <name> to load)";
                     state.push_info(msg);
                     return;
                 }
                 // Load the named skill into the live tool registry. Safe here:
                 // /skill is refused mid-turn (see submit), so no worker thread
                 // is reading the registry concurrently.
                 const std::string name(arg);
                 auto r = skills->load(name, *tool_reg);
                 if (!r) {
                     state.push_error("/skill: " + r.error().msg);
                     return;
                 }
                 // A prompt-injecting skill's result is body text the model must
                 // see. The /skill listing and tool registry alone don't carry it
                 // into the conversation, so queue it on the inject buffer: it
                 // rides the next provider request (flushed by on_inject inside
                 // agent.run), which also guarantees the system prompt is already
                 // in place. A tool-registering skill just reports a status note.
                 if (skills->injects_prompt(name)) {
                     QueuedPrompt qp;
                     qp.expanded_prompt = *r;  // full body -> model
                     qp.original_line = "↳ skill ‘" + name + "’ active";  // -> chat log
                     pending_queue.push_back(std::move(qp));
                     state.push_info("/skill " + name +
                                     " loaded — active on your next message");
                 } else {
                     state.push_info("/skill " + name + ": " + *r);
                 }
             },
             [&]() -> std::vector<std::string> {
                 std::vector<std::string> names;
                 if (skills)
                     for (const auto& s : skills->list()) names.push_back(s.name);
                 return names;
             }},
        },
        &input_content, &cursor_pos, &state_mtx, &model_complete);

    auto submit = [&] {
        std::string line = input_content;
        if (is_blank(line)) return;
        if (line == "/exit" || line == "/quit" || line == "/q") {
            screen.Exit();
            return;
        }

        bool busy;
        {
            std::lock_guard lk(state_mtx);
            busy = state.running();
        }

        // Slash commands act on the conversation before anything is sent to
        // the model. Conversation-modifying ones are refused while a turn is
        // in flight; generation controls and read-only commands are let through
        // (they only affect the next turn, never the in-flight one).
        if (!line.empty() && line[0] == '/') {
            if (busy) {
                // Commands safe to run mid-turn: generation controls, display-
                // only, or settings (which don't touch conversation history).
                auto safe_while_busy = [&] {
                    for (auto cmd : {"/model", "/provider", "/effort",
                                     "/thinking", "/temp", "/temperature",
                                     "/theme", "/system", "/settings"}) {
                        if (line == cmd || line.starts_with(std::string(cmd) + " "))
                            return true;
                    }
                    return false;
                };
                if (!safe_while_busy()) {
                    std::lock_guard lk(state_mtx);
                    state.push_error("busy: finish the current turn first");
                    input_content.clear();
                    post();
                    return;
                }
            }
            // No-argument commands that act on the conversation state directly.
            if (line == "/continue") {
                auto sums = list_conversations(conversations_dir(info.home),
                                               info.cwd);
                if (sums.empty()) {
                    std::lock_guard lk(state_mtx);
                    state.push_error(
                        "no previous conversation in this directory");
                } else {
                    load_into(sums.front().path);
                }
                input_content.clear();
                post();
                return;
            }
            if (line == "/resume") {
                auto sums = list_conversations(conversations_dir(info.home),
                                               info.cwd);
                std::lock_guard lk(state_mtx);
                if (sums.empty()) {
                    state.push_error("no saved conversations in this directory");
                } else {
                    picker = Picker{};
                    picker.active = true;
                    picker.title = " resume conversation ";
                    for (const auto& s : sums) {
                        std::string label =
                            (s.title.empty() ? std::string("(untitled)")
                                             : s.title) +
                            "  · " + s.updated + "  (" +
                            std::to_string(s.count) + ")";
                        picker.items.push_back(std::move(label));
                        std::string p = s.path;
                        picker.on_choose.push_back(
                            [&load_into, p] { load_into(p); });
                    }
                }
                input_content.clear();
                post();
                return;
            }
            if (line == "/rewind") {
                const Conversation& h = agent.history();
                std::lock_guard lk(state_mtx);
                picker = Picker{};
                for (std::size_t i = 0; i < h.size(); ++i) {
                    if (h[i].role() != Role::User) continue;
                    picker.items.push_back(one_line(h[i].content(), 70));
                    const std::size_t idx = i;
                    std::string txt = h[i].content();
                    picker.on_choose.push_back([&, idx, txt] {
                        Conversation trunc(agent.history().begin(),
                                           agent.history().begin() + idx);
                        agent.set_history(trunc);
                        {
                            std::lock_guard lk2(state_mtx);
                            state.load(trunc);
                        }
                        input_content = txt;
                        cursor_pos = static_cast<int>(txt.size());
                    });
                }
                if (picker.items.empty()) {
                    state.push_error("nothing to rewind to");
                } else {
                    picker.active = true;
                    picker.title = " rewind to message ";
                }
                input_content.clear();
                post();
                return;
            }
            if (line == "/clear") {
                save_now();
                {
                    std::lock_guard lk(state_mtx);
                    conv_path.clear();
                    meta = ConvMeta{};
                    meta.cwd = info.cwd;
                    meta.model = info.model;
                    agent.set_history({});
                    state.load({});
                    state.push_info("conversation cleared — starting fresh");
                }
                clear_input();  // also drops any pasted-image chips
                post();
                return;
            }
            if (line == "/compact" ||
                line.starts_with("/compact ")) {
                const std::size_t before = agent.history().size();
                const int before_tok =
                    estimated_tokens(agent.history());
                {
                    std::lock_guard lk(state_mtx);
                    if (before == 0) {
                        state.push_error("nothing to compact");
                        input_content.clear();
                        post();
                        return;
                    }
                    state.push_info("compacting " +
                                    human_tokens(before_tok) + " tok…");
                    state.set_running(true);
                }
                // Additional instructions: everything after "/compact ".
                std::string instructions;
                if (line.size() > 8 && line[8] == ' ') {
                    auto start = line.find_first_not_of(' ', 8);
                    if (start != std::string::npos)
                        instructions = line.substr(start);
                }
                input_content.clear();
                post();
                worker.submit({WorkItem::Compact, std::move(instructions), {}});
                return;
            }
            if (line == "/system") {
                std::lock_guard lk(state_mtx);
                std::string sp = agent.system_prompt();
                state.push_info("system prompt (" +
                                human_tokens(static_cast<int>(sp.size())) +
                                " chars):\n" + sp);
                input_content.clear();
                post();
                return;
            }
            if (line == "/paste") {
                auto img = read_clipboard_image();
                std::lock_guard lk(state_mtx);
                if (!img) {
                    state.push_error(
                        "no image on clipboard, or clipboard tool "
                        "(wl-paste / xclip) not installed");
                } else {
                    state.set_parent_model(info.model);
                    state.push_user("/paste (clipboard image)");
                    state.push_info(
                        "🖼 pasted clipboard image (" + img->media_type + ", " +
                        human_tokens(
                            static_cast<int>(img->base64_data.size() / 4)) +
                        " tok)");
                    std::vector<ContentPart> parts;
                    parts.push_back(ContentPart{
                        "The user pasted this image from their clipboard.",
                        std::nullopt});
                    parts.push_back(ContentPart{"", std::move(*img)});
                    worker.submit({WorkItem::Prompt, "/paste", std::move(parts)});
                }
                input_content.clear();
                post();
                return;
            }
            // All argument-bearing commands (including generation controls and
            // provider/model switching) dispatch through the registry.
            if (slash.dispatch(line)) {
                input_content.clear();
                post();
                return;
            }
            // Unknown slash command.
            std::lock_guard lk(state_mtx);
            state.push_error("unknown command: " + line);
            input_content.clear();
            post();
            return;
        }

        // @-mentions: read-only file auto-attach. The user's typed prompt is
        // what they see in the chat log; the worker gets the augmented version
        // (original + an "Attached files" section) so the model can ground
        // its answer in the file contents. Same byte/file caps as the CLI.
        // Expansion runs on the UI thread whether or not the agent is busy —
        // when busy the result is enqueued for the next turn(s).
        MentionOptions mopt;
        mopt.root = std::filesystem::path(info.cwd);
        mopt.max_file_bytes = 64 * 1024;
        mopt.max_total_bytes = 256 * 1024;
        mopt.max_files = 32;
        auto expansion = expand_mentions(line, mopt);

        // Pasted-image chips: capture the ids in chip order *before* rewriting
        // the `[img#id]` markers to the readable `[image #id]` for the model.
        const std::vector<int> chip_ids = extract_chip_ids(line);
        expansion.prompt = strip_chips(expansion.prompt);

        // Collapsed-paste markers: the displayed/history `line` keeps the compact
        // `[Pasted text #N]`, but the model gets the full bytes spliced back in.
        // A marker the session no longer knows (e.g. recalled from a prior run's
        // on-disk history) is left as-is rather than dropped.
        expansion.prompt = expand_paste_chips(expansion.prompt, paste_lookup);

        // Build multimodal content parts: text part first, then images from
        // @-mentions, direct image-path scanning, and pasted chips.
        std::vector<ContentPart> user_parts;
        user_parts.push_back(ContentPart{expansion.prompt, std::nullopt});
        // Images from @-mentions (e.g. @screenshot.png).
        for (auto& img : expansion.images)
            user_parts.push_back(ContentPart{"", std::move(img)});
        // Images from direct path detection (non-@ paths in the prompt).
        auto scanned = scan_image_paths(line, std::filesystem::path(info.cwd));
        for (auto& img : scanned.images)
            user_parts.push_back(ContentPart{"", std::move(img)});
        // Images pasted via Ctrl+V, in the order their chips appear. Chips the
        // user erased were already detached by reconcile_images(), so every id
        // that survives here still resolves to an attached image.
        for (int id : chip_ids) {
            auto it = std::ranges::find(clip_images, id, &PastedImage::id);
            if (it != clip_images.end())
                user_parts.push_back(ContentPart{"", std::move(it->block)});
        }
        clip_images.clear();

        history.push_back(line);
        hist_pos = history.size();
        append_history(hist_path, line);

        if (busy) {
            // Agent is mid-turn: buffer the message. The inject hook flushes the
            // whole buffer into the next provider request (the next tool round
            // or, failing that, a fresh turn), so it rides along as soon as the
            // API is called again — it is never held back to a strict per-turn
            // queue. Buffered messages never start a new conversation file
            // (conv_path is already minted by the first non-busy submit).
            QueuedPrompt qp;
            qp.expanded_prompt = std::move(expansion.prompt);
            qp.user_parts = std::move(user_parts);
            qp.original_line = line;
            qp.entries = std::move(expansion.entries);
            qp.total_bytes = expansion.total_bytes;
            qp.scanned_images = std::move(scanned.images);
            qp.scan_errors = std::move(scanned.errors);
            {
                std::lock_guard lk(state_mtx);
                // Truncated one-line preview so the user sees what they typed
                // while the current turn is still streaming.
                std::string preview = line;
                for (auto& c : preview)
                    if (c == '\n' || c == '\r') c = ' ';
                constexpr std::size_t max_preview = 100;
                if (preview.size() > max_preview) {
                    preview.resize(max_preview);
                    preview += "…";
                }
                pending_queue.push_back(std::move(qp));
                std::ostringstream qi;
                qi << "⏳ buffered (" << pending_queue.size()
                   << " pending, sent at next request): \"" << preview << "\"";
                state.push_info(qi.str());
            }
            clear_input();
            post();
            return;
        }

        {
            std::lock_guard lk(state_mtx);
            // Mark running here, on the UI thread, before the worker picks the
            // item up — otherwise a second submit in the gap before the worker
            // sets it would read busy==false and start a second turn instead of
            // buffering. The worker re-asserts it (idempotent).
            state.set_running(true);
            discard_buffer = false;  // a fresh, non-busy submit isn't an interrupt
            // The live model is the fallback for any spawn_subagent group this
            // turn opens without an explicit "model" arg. The model only changes
            // while idle, so stamping it at submit time is always current.
            state.set_parent_model(info.model);
            state.push_user(line);
            if (!expansion.entries.empty()) {
                std::size_t ok = 0, err = 0;
                for (const auto& e : expansion.entries) {
                    if (e.error.empty())
                        ++ok;
                    else
                        ++err;
                }
                std::ostringstream info;
                info << "📎 " << ok << " file(s) attached via @-mentions ("
                     << expansion.total_bytes << " B, " << err << " error(s))";
                for (const auto& e : expansion.entries) {
                    if (!e.error.empty()) {
                        info << "\n  ! " << e.path << ": " << e.error;
                    } else {
                        info << "\n  · " << e.path << "  (" << e.kind << ", "
                             << e.lines << " lines)";
                    }
                }
                state.push_info(info.str());
            }
            // Show scanned image info.
            if (!scanned.images.empty()) {
                std::ostringstream img_info;
                img_info << "🖼 " << scanned.images.size()
                         << " image(s) detected in prompt";
                for (const auto& img : scanned.images)
                    img_info << "\n  · " << img.media_type << " ("
                             << human_tokens(
                                    static_cast<int>(img.base64_data.size() / 4))
                             << " tok)";
                if (!scanned.errors.empty()) {
                    for (const auto& e : scanned.errors)
                        img_info << "\n  ! " << e;
                }
                state.push_info(img_info.str());
            }
            // Mint the conversation file lazily on the first user turn (unless a
            // conversation was loaded, which already set conv_path).
            if (conv_path.empty() && !info.home.empty()) {
                std::string id = new_conversation_id(info.cwd);
                conv_path = conversations_dir(info.home) + "/" + id + ".toml";
                if (meta.created.empty()) meta.created = now_iso();
                if (meta.title.empty()) meta.title = one_line(line, 80);
            }
        }
        clear_input();
        worker.submit({WorkItem::Prompt, std::move(expansion.prompt),
                       std::move(user_parts)});
        post();
    };

    auto refresh_complete = [&] {
        // Caller holds no lock; we take state_mtx. Cheap: one directory read.
        MentionContext ctx =
            mention_context_at(input_content,
                               static_cast<std::size_t>(std::max(0, cursor_pos)));
        std::lock_guard lk(state_mtx);
        if (!ctx.active) { complete = Complete{}; return; }
        complete.ctx = ctx;
        complete.items =
            complete_mention(ctx.typed, std::filesystem::path(info.cwd));
        complete.active = !complete.items.empty();
        if (complete.sel >= static_cast<int>(complete.items.size())) complete.sel = 0;
    };

    auto accept_complete = [&] {
        // pre: complete.active. Splice the selection, re-trigger for dir drill-in.
        MentionCompletion c;
        MentionContext ctx;
        {
            std::lock_guard lk(state_mtx);
            if (!complete.active || complete.sel < 0 ||
                complete.sel >= static_cast<int>(complete.items.size()))
                return;
            c = complete.items[complete.sel];
            ctx = complete.ctx;
        }
        auto [out, cur] = apply_completion(input_content, ctx, c);
        if (!c.is_dir) { out.insert(cur, " "); ++cur; }  // file => trailing space
        input_content = std::move(out);
        cursor_pos = static_cast<int>(cur);
        refresh_complete();  // dir => repopulate (drill-in); file => usually hides
    };

    InputOption iopt;
    iopt.content = &input_content;
    iopt.cursor_position = &cursor_pos;
    iopt.placeholder = glyphify("ask moocode…  (/exit quit · @ attach · Alt/Shift/Ctrl+Enter newline · Ctrl+V paste img)");
    iopt.multiline = false;
    iopt.on_change = [&] {
        refresh_complete();
        slash.refresh_complete();
        // An edit may have erased or broken an image chip; detach any image
        // whose chip is gone. Pasting plain text no longer grabs the clipboard:
        // images attach only on an explicit Ctrl+V (see the CatchEvent handler).
        reconcile_images();
    };
    iopt.on_enter = submit;
    auto input = Input(iopt);

    // --debug: count + describe incoming mouse events for the status-bar chip.
    // Staying at "none yet" while clicking/scrolling means the terminal never
    // sends mouse reports (the app does request them at startup).
    std::string last_mouse = "none yet";
    int mouse_events = 0;
    std::string last_key = "none yet";  // --debug: last key event's raw bytes
    int key_events = 0;

    // Bracketed-paste capture (UI thread only). A paste arrives wrapped in
    // ESC[200~ … ESC[201~ (we enable DEC mode 2004 around the loop); every byte
    // between the markers is buffered here and spliced in as one edit, so the
    // newlines in a multi-line paste never reach the Input's on_enter and fire
    // one sent message per line.
    bool pasting = false;
    std::string paste_buf;

    float chat_scroll = 1.0f;      // 0 = top, 1 = bottom
    float activity_scroll = 1.0f;  // activity tree
    float detail_scroll = 0.0f;    // detail pane (top-anchored)
    float detail_max_scroll = 0.0f;  // maximize overlay
    bool detail_max = false;         // full-screen inspect overlay up?
    // Keyboard focus zone (Zone, namespace scope). Input owns typing;
    // Activity/Chat own selection nav.
    Zone zone = Zone::Input;
    // True while selection (via the focus decorator) drives that pane's
    // scrolling; wheel/PgUp flips back to the manual fraction.
    bool follow_chat = false;
    bool follow_act = false;
    HitMap hits;  // last rendered frame's row hitboxes (UI thread only)
    std::optional<NodeKey> last_detail;  // detail target; scroll resets on change
    // The kitty keyboard protocol keeps a separate flag stack for the main and
    // alternate screens, so the push must happen *after* FTXUI has switched to
    // the alt screen (in screen.Loop's Install) — pushing it before the loop
    // lands on the main screen and never reaches the app. Done once, from the
    // first render (UI thread, alt screen already active). The alt screen's
    // stack is discarded by the terminal on exit, so no in-loop pop is needed.
    bool kitty_pushed = false;

    // Per-pane viewport extents, captured each frame via reflect, plus the
    // content's natural height measured via ComputeRequirement(). reflecting the
    // *content* box is useless: inside a frame the content node is handed the
    // viewport-sized box, so reflect always reports the viewport height — never
    // the true line count. The natural height (requirement().min_y) is what the
    // scroll maths needs, so each render stamps it into *_ch below.
    ftxui::Box chat_view, act_view, detail_view, dmax_view;
    int chat_ch = 0, act_ch = 0, detail_ch = 0, dmax_ch = 0;

    // The *_scroll values are a top-anchored fraction of the scroll range
    // (0 = top, 1 = bottom) — that is what the wheel/PgUp handlers step by
    // (frac() = lines / range). focusPositionRelative, however, takes a point
    // expressed as a fraction of the *content* height, which `frame` then
    // centres in the viewport and clamps: dy = clamp(ch*f - (vh-1)/2, 0, ch-vh).
    // Feeding the raw fraction in left a half-viewport dead zone at each end and
    // scrolled ch/(ch-vh)× too fast in between. Invert that mapping here so p
    // lands exactly on dy = p*(ch-vh): f = (p*(ch-vh) + (vh-1)/2) / ch.
    // ch is the measured natural height; vh comes from the reflected viewport.
    auto focus_frac = [](float p, int ch, const ftxui::Box& view) -> float {
        const int vh = view.y_max - view.y_min + 1;
        const int range = ch - vh;
        if (range <= 0) return 0.f;
        const float dy = std::clamp(p, 0.f, 1.f) * static_cast<float>(range);
        return (dy + static_cast<float>(vh - 1) * 0.5f) / static_cast<float>(ch);
    };

    // True wrapped height of a pane's content, in rows. A freshly built element
    // under-reports requirement().min_y for wrapped text (paragraph / markdown):
    // wrapping is only resolved once a width is assigned, so min_y counts each
    // paragraph as one row until then. Lay the element out once at the real pane
    // width — the flexbox caches that width — and re-read min_y to get the
    // wrapped count. The subsequent real render reuses the cached width, so
    // focusPositionRelative positions by this same height and focus_frac stays
    // exact. width<=0 (pane not yet measured) falls back to the bare min_y.
    auto measure_h = [](const ftxui::Element& el, int width) -> int {
        el->ComputeRequirement();
        if (width > 0) {
            el->SetBox(ftxui::Box{0, width - 1, 0, 1 << 20});
            el->ComputeRequirement();
        }
        return el->requirement().min_y;
    };

    auto root = Renderer(input, [&] {
        // Enable the kitty keyboard protocol now that the alt screen is live
        // (see kitty_pushed). One-shot; the bytes are a zero-width mode set, so
        // interleaving with the frame FTXUI is about to draw is harmless.
        if (!kitty_pushed) {
            write_tty_raw("\033[>1u");
            kitty_pushed = true;
        }
        const std::size_t frame_now = frame.load(std::memory_order_relaxed);
        std::lock_guard lk(state_mtx);

        hits.clear();
        // New inspect target => start its detail at the top.
        if (auto dk = state.detail_node(); dk != last_detail) {
            detail_scroll = 0.f;
            detail_max_scroll = 0.f;
            last_detail = dk;
        }

        // Token meter: ↑input (against the context window) and ↓output, the
        // latter a live ~4 bytes/token estimate while streaming, corrected to
        // the API's count once the reply lands.
        std::string tok = glyphify("▣ ");
        if (state.has_tokens()) {
            tok += glyphify("↑") + human_tokens(state.input_tokens());
            if (info.context_window > 0)
                tok += "/" + human_tokens(info.context_window);
            tok += glyphify(" ↓") + human_tokens(state.output_tokens());
        } else {
            tok += glyphify("—");
        }
        tok += " tok ";

        // Compact reasoning-control chip (effort / thinking), shown only when a
        // control is set. Reading the provider's params here is a plain read;
        // set_params only runs on this (UI) thread while idle.
        std::string ctl;
        std::string live_provider;
        std::string live_model;
        {
            // Read the live provider directly so the status bar never drifts from
            // what's actually on the wire: set_params/set_model/set_provider all
            // run on this (UI) thread while idle, and complete() only reads these
            // fields, so a render-time read races nothing.
            Provider& pv = agent.provider();
            GenerationParams gp = pv.params();
            if (gp.effort) ctl += "e:" + *gp.effort + " ";
            if (gp.thinking) ctl += *gp.thinking ? "think:on " : "think:off ";
            live_provider = std::string(pv.wire_format());
            live_model = pv.model();
        }

        Element status = hbox({
            text(" moocode ") | bold | bgcolor(Color::Cyan) | color(Color::Black),
            live_provider.empty()
                ? text(" ")
                : text(" " + live_provider + " ") | bold | color(Color::Magenta),
            text(live_model + " "),
            gtext("· " + info.cwd + " ") | dim,
            // --debug: key/mouse chips anchored left (before the filler) so a
            // long raw-byte dump is never clipped off the right edge.
            !info.debug
                ? text("")
                : hbox({text("key[" + std::to_string(key_events) + "] " +
                             last_key + "  ") |
                            color(Color::GreenLight),
                        text("mouse[" + std::to_string(mouse_events) + "] " +
                             last_mouse + " ") |
                            color(Color::Yellow)}),
            filler(),
            ctl.empty() ? text("") : gtext("⚙ " + ctl) | color(Color::Yellow),
            text(tok) | color(Color::GrayLight),
            state.running()
                ? hbox({text(spin_glyph(frame_now)) | color(Color::Yellow),
                        text(" " + busy_word(state.busy_seed())),
                        pending_queue.empty()
                            ? text("")
                            : text(" [" + std::to_string(pending_queue.size()) +
                                   " queued]") | dim,
                        text(" ")})
                : (text(" idle ") | dim),
        });

        Element chat = render_chat(state, frame_now, hits,
                                   zone == Zone::Chat && follow_chat);
        chat_ch = measure_h(chat, chat_view.x_max - chat_view.x_min);
        chat = ((zone == Zone::Chat && follow_chat)
                    ? chat | yframe | vscroll_indicator | flex
                    : chat |
                          focusPositionRelative(
                              0.f, focus_frac(chat_scroll, chat_ch, chat_view)) |
                          yframe | vscroll_indicator | flex) |
               reflect(chat_view);
        Element tree = render_activity_tree(state, frame_now, hits,
                                            zone == Zone::Activity && follow_act);
        act_ch = measure_h(tree, act_view.x_max - act_view.x_min);
        tree = ((zone == Zone::Activity && follow_act)
                    ? tree | yframe | vscroll_indicator | flex
                    : tree |
                          focusPositionRelative(
                              0.f, focus_frac(activity_scroll, act_ch, act_view)) |
                          yframe | vscroll_indicator | flex) |
               reflect(act_view);
        // The lower pane is content-driven: the foldable subagents browser when
        // its zone is focused / a sub-agent (or nothing) is selected, else the
        // selected plain node's I/O.
        const bool show_browser = show_subagents_browser(zone, state);
        std::optional<NodeKey> browser_focus;
        if (const auto& bsel = state.selection();
            bsel && bsel->parent_id.empty() &&
            (bsel->pane == NodeKey::Pane::Subagents ||
             bsel->pane == NodeKey::Pane::Activity) &&
            state.find_group(bsel->id))
            browser_focus = NodeKey{NodeKey::Pane::Subagents, 0, bsel->id, ""};
        Element detail =
            (show_browser
                 ? render_subagent_browser(state, frame_now, hits, browser_focus)
                 : render_detail(state, frame_now));
        detail_ch = measure_h(detail, detail_view.x_max - detail_view.x_min);
        detail = detail |
            focusPositionRelative(
                0.f, focus_frac(detail_scroll, detail_ch, detail_view)) |
            yframe | vscroll_indicator | flex | reflect(detail_view);

        Element complete_box = nullptr;
        if (model_complete.active) {
            Elements rows;
            constexpr int kMaxRows = 8;
            const int n = static_cast<int>(model_complete.items.size());
            int first = std::max(0, model_complete.sel - kMaxRows + 1);
            first = std::min(first, std::max(0, n - kMaxRows));
            for (int i = first; i < n && i < first + kMaxRows; ++i) {
                Element row = text("  " + model_complete.items[i] + "  ");
                if (i == model_complete.sel) row = row | inverted;
                rows.push_back(row);
            }
            std::string title = model_complete.label.empty()
                                    ? std::string("model")
                                    : model_complete.label;
            complete_box =
                window(text(" " + title + " ") | bold,
                       vbox(std::move(rows)) | size(HEIGHT, LESS_THAN, 10)) |
                size(WIDTH, LESS_THAN, 60);
        } else if (complete.active) {
            Elements rows;
            constexpr int kMaxRows = 8;  // visible popup rows (chrome brings window to 10)
            const int n = static_cast<int>(complete.items.size());
            int first = std::max(0, complete.sel - kMaxRows + 1);
            first = std::min(first, std::max(0, n - kMaxRows));
            for (int i = first; i < n && i < first + kMaxRows; ++i) {
                Element row = text("  " + complete.items[i].display + "  ");
                if (i == complete.sel) row = row | inverted;
                rows.push_back(row);
            }
            complete_box =
                window(text(" @-complete ") | bold,
                       vbox(std::move(rows)) | size(HEIGHT, LESS_THAN, 10)) |
                size(WIDTH, LESS_THAN, 60);
        }

        // The input grows with multi-line messages (Shift+Enter / paste) but is
        // capped so a large paste can't swallow the screen; FTXUI's internal
        // `frame` scrolls the cursor line into view past the cap.
        Element input_line =
            hbox({gtext(" ❯ ") | bold | color(Color::Cyan),
                  input->Render() | flex | size(HEIGHT, LESS_THAN, 10)}) |
            border;
        Element input_box = complete_box
                                ? vbox({complete_box, input_line})
                                : input_line;

        Element footer =
            zone == Zone::Input
                ? gtext(" Enter send · Alt/Shift/Ctrl+Enter newline · ↑↓ history · @ attach · "
                        "/effort /thinking /temp /model /provider /settings /theme "
                        "/compact /skill · Tab panes · PgUp/PgDn scroll · Esc stop · "
                        "F2 reasoning · Ctrl+C twice quit") | dim
                : gtext(" ↑↓ select · ←→ fold sub-agent · Enter inspect · "
                        "Ctrl+Y copy · PgUp/PgDn scroll · Tab next pane "
                        "(…/subagents) · Esc back to input") | dim;

        // Deterministic 60/40 split: pin the activity pane to 40% of the screen
        // width so the divider never drifts with content. A dual-`flex` layout
        // instead sizes each pane by its content's min-width (wide code blocks
        // inflate the chat pane), which is what produced the unstable ~3/4+1/4.
        // Use Terminal::Size() rather than screen.dimx(): on the first frame
        // FTXUI renders the document before it assigns the screen's width, so
        // screen.dimx() is still 0 then (split would start at the 12-col floor).
        const int activity_w = std::max(12, Terminal::Size().dimx * 2 / 5);
        // The right column stacks the activity tree over a detail pane pinned
        // to two fifths of the window height; both share the activity width. The
        // tree takes the remaining height via flex.
        const int detail_h = std::max(8, Terminal::Size().dimy * 2 / 5);
        Element right_col = vbox({
            window(text(" activity "), tree) | flex,
            window(text(show_browser ? " subagents " : " detail "), detail) |
                size(HEIGHT, EQUAL, detail_h),
        }) | size(WIDTH, EQUAL, activity_w);
        Element body = hbox({
            vbox({chat, separator(), input_box}) | flex,
            separator(),
            right_col,
        }) | flex;

        Element page = vbox({status, separator(), body, footer});

        // Single read of the optional avoids the pending()/pending_call() TOCTOU
        // where release/answer could race between the two calls.
        if (auto pc = gate.pending_call()) {
            Element dialog =
                window(text(" approve tool call? ") | bold,
                       vbox({
                           hbox({text(sanitize_tui_text(pc->name)) | bold | color(Color::CyanLight),
                                 text("  "),
                                 text(sanitize_tui_text(pc->arguments_json)) | color(Color::Yellow)}),
                           separator(),
                           text("[y] once   [s] session   [a] always   [n] deny") | center,
                       })) |
                size(WIDTH, LESS_THAN, 80) | clear_under | center;
            return dbox({page | dim, dialog});
        }
        // Question modal: rendered when the worker is blocked on ask_user.
        // Reset local UI state on the false→true pending transition.
        {
            bool now_pending = question_gate.pending();
            if (now_pending && !question_was_pending)
                question_modal = QuestionModal{};
            question_was_pending = now_pending;
        }
        if (auto q = question_gate.pending_question()) {
            Elements option_rows;
            for (std::size_t i = 0; i < q->options.size(); ++i) {
                std::string label =
                    std::to_string(i + 1) + ". " + sanitize_tui_text(q->options[i]);
                Element row = text("  " + label + "  ");
                if (!question_modal.custom_mode &&
                    static_cast<int>(i) == question_modal.sel)
                    row = row | inverted;
                if (question_modal.custom_mode) row = row | dim;
                option_rows.push_back(row);
            }
            // Custom input line.
            std::string cursor_display =
                block_cursor(frame_now / 16 % 2 == 0);
            std::string custom_display =
                question_modal.custom_mode
                    ? question_modal.custom_text + cursor_display
                    : (question_modal.custom_text.empty()
                           ? "____________________"
                           : question_modal.custom_text);
            Element custom_line = hbox({
                text("Custom: "),
                text(custom_display) |
                    (question_modal.custom_mode ? bold : dim),
            });
            // Help line changes with mode.
            std::string help =
                question_modal.custom_mode
                    ? "[Enter] submit  [Tab] options  [Esc] halt"
                    : "[↑↓] navigate  [1-9] pick  [Tab] custom  [Enter] pick  [Esc] halt";
            Element help_line = gtext(help) | dim | center;

            // Put question text as first body element (wraps naturally).
            Elements body_elts;
            body_elts.push_back(paragraph(sanitize_tui_text(q->question)) | bold);
            body_elts.push_back(separator());
            body_elts.push_back(vbox(std::move(option_rows)));
            body_elts.push_back(separator());
            body_elts.push_back(custom_line);
            body_elts.push_back(separator());
            body_elts.push_back(help_line);

            Element dialog =
                window(text(" pick one ") | bold,
                       vbox(std::move(body_elts))) |
                size(WIDTH, LESS_THAN, 80) | clear_under | center;
            return dbox({page | dim, dialog});
        }
        if (picker.active) {
            Elements rows;
            for (std::size_t i = 0; i < picker.items.size(); ++i) {
                Element row = text("  " + picker.items[i] + "  ");
                if (static_cast<int>(i) == picker.sel) row = row | inverted;
                rows.push_back(row);
            }
            if (rows.empty()) rows.push_back(text("  (empty)  ") | dim);
            Element dialog =
                window(text(picker.title) | bold,
                       vbox({vbox(std::move(rows)) | vscroll_indicator | yframe |
                                 size(HEIGHT, LESS_THAN, 18),
                             separator(),
                             gtext("↑↓ select · Enter choose · Esc cancel") | dim | center})) |
                size(WIDTH, LESS_THAN, 100) | clear_under | center;
            return dbox({page | dim, dialog});
        }
        // --- Settings editor modal ------------------------------------------
        if (settings_form.active) {
            // Tab bar
            auto make_tab = [&](SettingsForm::Tab t, std::string label) {
                Element el = text(" " + label + " ") | border;
                if (t == settings_form.current_tab) el = el | inverted;
                return el;
            };
            Element tab_bar = hbox({
                make_tab(SettingsForm::Tab::General,   "General"),
                make_tab(SettingsForm::Tab::Profiles,  "Profiles"),
                make_tab(SettingsForm::Tab::Credentials, "Credentials"),
            }) | center;

            // Tab content
            Element tab_content;
            std::string help;

            if (settings_form.current_tab == SettingsForm::Tab::General) {
                Elements rows;
                const int n = static_cast<int>(settings_form.fields.size());
                for (int i = 0; i < n; ++i) {
                    const auto& f = settings_form.fields[i];
                    std::string val = sanitize_tui_text(
                        settings_form.display_value(i, info.model, info.provider));
                    if (settings_form.editing && i == settings_form.sel) {
                        std::string cursor =
                            block_cursor(frame_now / 16 % 2 == 0);
                        val = sanitize_tui_text(settings_form.edit_buf) + cursor;
                    }
                    std::string row_text = "  " + f.label + ": " + val + "  ";
                    Element row = text(row_text);
                    if (i == settings_form.sel) row = row | inverted;
                    rows.push_back(row);
                }
                if (rows.empty())
                    rows.push_back(text("  (no settings)  ") | dim);
                if (settings_form.editing) {
                    const auto& f = settings_form.fields[settings_form.sel];
                    if (f.type == FieldDef::Type::Choice ||
                        f.type == FieldDef::Type::BoolToggle)
                        help = "Enter cycles · Esc cancel";
                    else
                        help = "type value · Enter confirm · Esc cancel";
                } else {
                    help = "← → tabs · ↑↓ select · Enter edit · Esc close";
                }
                tab_content = vbox(std::move(rows));
            } else if (settings_form.current_tab == SettingsForm::Tab::Profiles) {
                Elements rows;
                const auto& pe = settings_form.profile_editor;
                if (pe.editing_profile) {
                    // Profile detail sub-editor
                    const Profile* p = (pe.sel >= 0 && pe.sel < static_cast<int>(pe.profiles.size()))
                        ? &pe.profiles[pe.sel] : nullptr;
                    rows.push_back(text("  profile: " + (p ? p->name : "")) | bold);
                    rows.push_back(gtext("  ─────────────────────────────") | dim);
                    if (p) {
                        for (int i = 0; i < kNumProfileFields; ++i) {
                            std::string val = profile_field_value(*p, i);
                            if (pe.profile_edit_text_mode && pe.edit_field_idx == i)
                                val = sanitize_tui_text(pe.edit_buf) +
                                    (block_cursor(frame_now / 16 % 2 == 0));
                            else
                                val = sanitize_tui_text(val);
                            Element fr = text("  " + profile_field_label(i) + ": " + val);
                            if (pe.profile_field_sel == i) fr = fr | inverted;
                            rows.push_back(fr);
                        }
                        // Models row
                        {
                            std::string ml = "models: " + std::to_string(p->models.size()) + " model(s)";
                            Element mr = text("  " + ml);
                            if (pe.profile_field_sel == kNumProfileFields) mr = mr | inverted;
                            rows.push_back(mr);
                        }
                        if (pe.editing_models) {
                            rows.push_back(gtext("  ─────────────────────────────") | dim);
                            for (int mi = 0; mi < static_cast<int>(p->models.size()); ++mi) {
                                std::string mv = sanitize_tui_text(p->models[mi]);
                                Element mr = text("  " + mv);
                                if (pe.model_sel == mi) mr = mr | inverted;
                                rows.push_back(mr);
                            }
                            rows.push_back(text("  [+ add model]") | dim);
                        }
                    }
                    if (pe.profile_edit_text_mode)
                        help = "type value · Enter confirm · Esc cancel";
                    else if (pe.editing_models)
                        help = "↑↓ select model · Enter edit · a add · d delete · Esc back";
                    else
                        help = "↑↓ select field · Enter edit / open models · Esc back";
                } else {
                    // Profile list
                    for (int i = 0; i < static_cast<int>(pe.profiles.size()); ++i) {
                        const auto& p = pe.profiles[i];
                        std::string marker = (settings_form.draft.profile == p.name) ? "* " : "  ";
                        std::string buf = marker + p.name + "  " +
                            (p.kind.empty() ? "auto" : p.kind) + "  " + p.base_url;
                        Element row = text(sanitize_tui_text(buf));
                        if (i == pe.sel) row = row | inverted;
                        rows.push_back(row);
                    }
                    rows.push_back(text("  [+ Add Profile]") | dim);
                    help = "← → tabs · ↑↓ select · Enter edit · a add · d delete · Esc close";
                }
                tab_content = vbox(std::move(rows));
            } else {
                // Credentials tab (stub for Phase 3)
                const auto& ce = settings_form.credential_editor;
                if (ce.profile_names.empty()) {
                    tab_content = gtext("  (no profiles configured — add profiles first)") | dim | center;
                } else {
                    Elements rows;
                    for (int i = 0; i < static_cast<int>(ce.profile_names.size()); ++i) {
                        const auto& name = ce.profile_names[i];
                        std::string key_disp = ce.display(name);
                        std::string buf = "  " + name + ":  " + key_disp;
                        Element row = text(sanitize_tui_text(buf));
                        if (i == ce.sel) row = row | inverted;
                        rows.push_back(row);
                    }
                    if (ce.editing_key) {
                        // Show masked input: show the raw edit buf length as asterisks
                        std::string masked(ce.key_edit_buf.size(), '*');
                        rows.push_back(text("  key: " + masked +
                            (block_cursor(frame_now / 16 % 2 == 0))));
                    }
                    tab_content = vbox(std::move(rows));
                }
                if (ce.editing_key)
                    help = "type key · Enter save · Esc cancel";
                else
                    help = "← → tabs · ↑↓ select · Enter set key · d delete · Esc close";
            }

            // Warning banner for malformed file
            Element banner = emptyElement();
            if (settings_form.malformed_file) {
                banner = gtext(" ⚠ " + sanitize_tui_text(settings_form.malformed_msg) + " ") |
                         bgcolor(Color::Yellow) | color(Color::Black) | center;
            }

            Element dialog =
                window(text(" settings ") | bold,
                       vbox({banner,
                             tab_bar,
                             separator(),
                             tab_content | vscroll_indicator | yframe |
                                 size(HEIGHT, LESS_THAN, 18),
                             separator(),
                             gtext(help) | dim | center})) |
                size(WIDTH, LESS_THAN, 80) | clear_under | center;
            return dbox({page | dim, dialog});
        }
        if (detail_max) {
            const int tw = Terminal::Size().dimx, th = Terminal::Size().dimy;
            Element dmax_body = render_detail(state, frame_now);
            dmax_ch = measure_h(dmax_body, dmax_view.x_max - dmax_view.x_min);
            Element dlg =
                window(text(" inspect ") | bold,
                       vbox({dmax_body |
                                 focusPositionRelative(
                                     0.f, focus_frac(detail_max_scroll, dmax_ch,
                                                     dmax_view)) |
                                 yframe | vscroll_indicator | flex |
                                 reflect(dmax_view),
                             separator(),
                             gtext("↑↓ / PgUp PgDn scroll · Esc close") | dim |
                                 center})) |
                size(WIDTH, EQUAL, std::max(40, tw - 6)) |
                size(HEIGHT, EQUAL, std::max(12, th - 4)) | clear_under | center;
            return dbox({page | dim, dlg});
        }
        return page;
    });

    auto last_ctrl_c = std::chrono::steady_clock::now();

    // Turn a finished model-detection (posted by the worker) into either the
    // selection picker or a clear info/error line. Runs on the UI thread so the
    // picker callbacks (which switch the connection) are valid. No-op until a
    // result is pending.
    auto handle_detect_result = [&] {
        DetectResult r;
        {
            std::lock_guard lk(state_mtx);
            if (!detect.pending) return;
            r = std::move(detect);
            detect = DetectResult{};
        }
        std::lock_guard lk(state_mtx);
        // Silent startup refresh: update the active profile's model set in place
        // for /model + @-autocomplete without raising a picker or switching the
        // live model. Failures stay quiet (the seed list keeps working offline).
        if (r.silent) {
            // All-profiles startup sweep: apply each profile's refreshed set.
            if (!r.bulk.empty()) {
                std::size_t total = 0;
                for (auto& [name, ids] : r.bulk) {
                    std::vector<std::string> kept =
                        filter_blacklisted(ids, blacklist_for(name));
                    for (Profile& p : profiles)
                        if (p.name == name) { p.models = kept; break; }
                    total += kept.size();
                }
                model_names = rebuild_model_names();
                state.push_info("fetched " + std::to_string(total) +
                                " models across " + std::to_string(r.bulk.size()) +
                                " profile(s)");
                return;
            }
            // Single-profile silent refresh.
            if (!r.ok || r.models.empty()) return;
            r.models = filter_blacklisted(r.models, blacklist_for(r.profile));
            for (Profile& p : profiles)
                if (p.name == r.profile) { p.models = r.models; break; }
            model_names = rebuild_model_names();
            state.push_info("fetched " + std::to_string(r.models.size()) +
                            " models from " + r.base);
            return;
        }
        if (!r.ok) {
            state.push_error("model detection failed (" + r.base + "): " + r.error);
            return;
        }
        r.models = filter_blacklisted(r.models, blacklist_for(r.profile));
        if (r.models.empty()) {
            state.push_info("no models reported by " + r.base + " — keeping " +
                            info.model);
            return;
        }
        // Raise the selection picker — the explicit, foreground prompt.
        picker = Picker{};
        picker.active = true;
        picker.title = " " + std::to_string(r.models.size()) +
                       " model(s) from " + r.base + " — ↑↓ + Enter, Esc cancels ";
        auto shared = std::make_shared<std::vector<std::string>>(r.models);
        for (std::size_t i = 0; i < r.models.size(); ++i) {
            const std::string& id = r.models[i];
            if (id == info.model) picker.sel = static_cast<int>(i);
            picker.items.push_back(id);
            const std::string prof = r.profile;
            picker.on_choose.push_back([&apply_detected_models, prof, id, shared] {
                apply_detected_models(prof, id, *shared);
            });
        }
    };

    // Splice `text` into the prompt at the cursor (UI thread; input_content and
    // cursor_pos are byte offsets, per FTXUI's Input). Snaps focus home and
    // refreshes the completion popup + image chips, mirroring what the Input's
    // own on_change does. Shared by bracketed paste and Shift+Enter newline.
    auto insert_at_cursor = [&](std::string_view text) {
        std::size_t at = std::min(
            static_cast<std::size_t>(std::max(0, cursor_pos)), input_content.size());
        input_content.insert(at, text);
        cursor_pos = static_cast<int>(at + text.size());
        zone = Zone::Input;
        refresh_complete();
        slash.refresh_complete();
        reconcile_images();
    };

    root |= CatchEvent([&](Event e) {
        // Surface any model-detection result the worker just posted (raises the
        // picker / reports) before handling the event itself.
        handle_detect_result();

        // Convert a fixed number of content lines into the scroll fraction the
        // focusPositionRelative panes use, so a wheel notch / key always moves
        // the same visual distance regardless of buffer length. Falls back to a
        // small fraction before the pane has been measured (boxes still empty).
        // ch is the content's natural line count (measured at render via
        // ComputeRequirement); vh is the reflected viewport height.
        auto frac = [](int lines, int ch, const ftxui::Box& view) -> float {
            const int vh = view.y_max - view.y_min + 1;
            const int range = ch - vh;        // scrollable rows
            if (range <= 0) return 0.f;        // content fits — no scroll needed
            float f = static_cast<float>(lines) / static_cast<float>(range);
            // Cap the step so a single notch never jumps more than 10% of the
            // viewport, avoiding the "one tap to bottom" jump on short buffers.
            return std::min(f, 0.1f);
        };
        constexpr int kWheelLines = 3;  // one wheel notch, the terminal standard

        // --debug: record every incoming mouse event for the status chip, then
        // fall through — recorded here, before any handler can swallow it, so
        // the chip reflects exactly what the terminal delivers.
        if (info.debug && e.is_mouse()) {
            last_mouse = describe_mouse(e.mouse());
            ++mouse_events;
        }
        // --debug: record every key event (before any handler swallows it) so
        // the status bar shows the raw bytes the terminal delivered — the way
        // to discover what an unbound key like Shift+Enter actually sends here.
        if (info.debug && !e.is_mouse() && !e.is_cursor_position() &&
            !e.is_cursor_shape() && e != Event::Custom) {
            last_key = describe_key(e);
            ++key_events;
            post();
        }
        // --- Bracketed paste -------------------------------------------------
        // The terminal wraps a paste in ESC[200~ … ESC[201~ (FTXUI surfaces the
        // markers as Special events). Buffer everything between them, then splice
        // it in as a single edit. Handled before the modals so pasted bytes are
        // never mistaken for key commands, and before on_enter ever sees the
        // embedded newlines — which is what used to send one message per line.
        static const Event kPasteStart = Event::Special("\x1B[200~");
        static const Event kPasteEnd = Event::Special("\x1B[201~");
        if (e == kPasteStart) {
            pasting = true;
            paste_buf.clear();
            return true;
        }
        if (pasting) {
            if (e == kPasteEnd) {
                pasting = false;
                if (!paste_buf.empty()) {
                    if (paste_buf.size() > kPasteCollapseBytes) {
                        // Large paste: stash the bytes and splice in an atomic
                        // `[Pasted text #N]` chip instead of the raw blob. The
                        // chip's bytes are recovered on submit (expand_paste_chips)
                        // and the marker survives history recall.
                        const int id = next_paste_id++;
                        const int lines = 1 + static_cast<int>(std::count(
                                                  paste_buf.begin(),
                                                  paste_buf.end(), '\n'));
                        paste_blocks.push_back(PasteBlock{id, paste_buf});
                        const std::string chip = paste_chip(id);
                        insert_at_cursor(chip);
                        std::lock_guard lk(state_mtx);
                        state.push_info(
                            "📋 " + chip + " collapsed (" + std::to_string(lines) +
                            " line" + (lines == 1 ? "" : "s") + ", " +
                            human_tokens(static_cast<int>(paste_buf.size())) +
                            " chars) — Backspace to remove");
                    } else {
                        insert_at_cursor(paste_buf);
                    }
                }
                paste_buf.clear();
                post();
                return true;
            }
            // Collect the paste's bytes. Printable runs arrive as Character
            // events; a newline as Return ("\n"), a tab as Tab ("\t"). Anything
            // else inside a paste (stray control/mouse report) is dropped.
            if (e.is_character()) paste_buf += e.character();
            else if (e == Event::Return) paste_buf += '\n';
            else if (e == Event::Tab) paste_buf += '\t';
            return true;
        }
        // Normalize kitty keyboard-protocol "CSI u" key reports (we enable the
        // protocol around the loop — it is the only scheme some terminals, e.g.
        // kitty/konsole/recent-VTE, honor; they ignore modifyOtherKeys). A
        // modified Enter (codepoint 13 + any modifier) is the multi-line
        // newline. The handful of keys the protocol re-encodes that the
        // handlers below expect in legacy form — Escape and the Ctrl combos —
        // are rewritten back to the canonical FTXUI event, leaving everything
        // downstream untouched. Keys the protocol leaves legacy (plain text,
        // arrows, unmodified Enter/Tab/Backspace) never match parse_kitty_key.
        // Placed after the paste block so pasted bytes are never reinterpreted.
        if (!e.is_mouse()) {
            if (const auto k = parse_kitty_key(e.input())) {
                const auto [cp, mod] = *k;
                const int mask = mod - 1;  // bit 0 Shift, bit 1 Alt, bit 2 Ctrl
                const bool alt = mask & 2;
                const bool ctrl = mask & 4;
                if (cp == 13 && mod >= 2) {  // Shift/Ctrl/Alt+Enter → newline
                    insert_at_cursor("\n");
                    post();
                    return true;
                }
                if (cp == 27 && mask == 0) e = Event::Escape;
                else if (cp == 99 && ctrl && !alt) e = Event::CtrlC;
                else if (cp == 121 && ctrl && !alt) e = Event::CtrlY;
                else if (cp == 118 && ctrl && alt) e = Event::CtrlAltV;
                else if (cp == 118 && ctrl) e = Event::CtrlV;
                // Any other CSI u key falls through unrewritten (just unbound).
            }
        }
        // While the approval modal is up, route the four keys and swallow the rest.
        if (gate.pending()) {
            if (e == Event::Character('y') || e == Event::Character('Y'))
                { gate.answer(Approval::Once); return true; }
            if (e == Event::Character('s') || e == Event::Character('S'))
                { gate.answer(Approval::Session); return true; }
            if (e == Event::Character('a') || e == Event::Character('A'))
                { gate.answer(Approval::Always); return true; }
            if (e == Event::Character('n') || e == Event::Character('N') ||
                e == Event::Escape)
                { gate.answer(Approval::Deny); return true; }
            return true;  // swallow everything else while the modal is up
        }
        // While the question modal is up, route keys for option nav / custom
        // input / dismiss. The question IS the interaction; Esc means halt.
        if (question_gate.pending()) {
            if (question_modal.custom_mode) {
                // -- Custom input mode ---
                if (e == Event::Return) {
                    if (question_modal.custom_text.empty())
                        question_gate.halt();
                    else
                        question_gate.answer(
                            std::move(question_modal.custom_text));
                    question_modal = QuestionModal{};
                    return true;
                }
                if (e == Event::Tab) {
                    question_modal.custom_mode = false;
                    return true;
                }
                if (e == Event::Escape) {
                    question_gate.halt();
                    question_modal = QuestionModal{};
                    return true;
                }
                if (e == Event::Backspace) {
                    if (!question_modal.custom_text.empty())
                        question_modal.custom_text.pop_back();
                    return true;
                }
                if (e.is_character()) {
                    question_modal.custom_text += e.character();
                    return true;
                }
                return true;  // swallow everything else
            } else {
                // --- Option navigation mode ---
                if (e == Event::ArrowUp) {
                    if (question_modal.sel > 0) --question_modal.sel;
                    return true;
                }
                if (e == Event::ArrowDown) {
                    auto q = question_gate.pending_question();
                    if (q && question_modal.sel + 1 <
                                 static_cast<int>(q->options.size()))
                        ++question_modal.sel;
                    return true;
                }
                if (e == Event::Return) {
                    auto q = question_gate.pending_question();
                    if (q && question_modal.sel >= 0 &&
                        question_modal.sel <
                            static_cast<int>(q->options.size())) {
                        std::string chosen =
                            q->options[question_modal.sel];
                        question_gate.answer(std::move(chosen));
                    } else {
                        question_gate.halt();
                    }
                    question_modal = QuestionModal{};
                    return true;
                }
                if (e == Event::Tab) {
                    question_modal.custom_mode = true;
                    question_modal.custom_text.clear();
                    return true;
                }
                if (e == Event::Escape) {
                    question_gate.halt();
                    question_modal = QuestionModal{};
                    return true;
                }
                // Number keys 1-9: quick-pick option by index.
                if (e.is_character() && e.character().size() == 1) {
                    char c = e.character()[0];
                    if (c >= '1' && c <= '9') {
                        int idx = c - '1';
                        auto q = question_gate.pending_question();
                        if (q && idx <
                                     static_cast<int>(q->options.size())) {
                            question_gate.answer(q->options[idx]);
                            question_modal = QuestionModal{};
                            return true;
                        }
                    }
                }
                return true;  // swallow everything else
            }
        }
        // --- Settings editor modal: keyboard navigation ---------------------
        if (settings_form.active) {
            // Tab switching: left/right arrows.
            if (e == Event::ArrowLeft) {
                settings_form.prev_tab();
                if (settings_form.current_tab == SettingsForm::Tab::Profiles &&
                    !settings_form.profile_editor.active)
                    settings_form.open_profile_editor();
                if (settings_form.current_tab == SettingsForm::Tab::Credentials &&
                    !settings_form.credential_editor.active) {
                    settings_form.credential_editor = credential_editor_build(
                        credentials, settings_form.draft.profiles);
                    settings_form.credential_editor.active = true;
                }
                post(); return true;
            }
            if (e == Event::ArrowRight) {
                settings_form.next_tab();
                if (settings_form.current_tab == SettingsForm::Tab::Profiles &&
                    !settings_form.profile_editor.active)
                    settings_form.open_profile_editor();
                if (settings_form.current_tab == SettingsForm::Tab::Credentials &&
                    !settings_form.credential_editor.active) {
                    settings_form.credential_editor = credential_editor_build(
                        credentials, settings_form.draft.profiles);
                    settings_form.credential_editor.active = true;
                }
                post(); return true;
            }
            auto& sf = settings_form;
            auto& pe = sf.profile_editor;
            auto& ce = sf.credential_editor;

            // --- Global Escape: close the whole modal ---
            if (e == Event::Escape) {
                // If in a sub-editor, pop one level first.
                if (sf.current_tab == SettingsForm::Tab::Profiles) {
                    if (pe.editing_models) {
                        pe.editing_models = false; post(); return true;
                    }
                    if (pe.profile_edit_text_mode) {
                        pe.cancel_profile_field_edit(); post(); return true;
                    }
                    if (pe.editing_profile) {
                        pe.cancel_profile_edit(); post(); return true;
                    }
                }
                if (sf.current_tab == SettingsForm::Tab::Credentials) {
                    if (ce.editing_key) {
                        ce.cancel_key(); post(); return true;
                    }
                }
                if (sf.current_tab == SettingsForm::Tab::General) {
                    if (sf.editing) {
                        sf.cancel_edit(); post(); return true;
                    }
                }
                // Close the whole modal.
                FormAction act = settings_form_close(sf);
                if (act == FormAction::Saved && !info.home.empty()) {
                    save_settings(info.home, sf.draft);
                    state.push_info("settings saved to " + info.home + "/settings.toml");
                    profiles = load_settings(info.home).profiles;
                    if (profiles.empty()) profiles = default_profiles();
                    model_names = rebuild_model_names();
                    if (auto t = syntax_theme_from_name(sf.draft.theme))
                        state.set_syntax_theme(*t);
                }
                // Also save credentials if dirty.
                if (ce.dirty && !info.home.empty()) {
                    for (const auto& name : ce.deleted) {
                        save_credential(info.home, name, "");
                        credentials.erase(name);
                    }
                    for (const auto& [name, key] : ce.credentials) {
                        if (!key.empty()) {
                            save_credential(info.home, name, key);
                            credentials[name] = key;
                        }
                    }
                }
                post(); return true;
            }

            // --- General tab editing ---
            if (sf.current_tab == SettingsForm::Tab::General) {
                if (sf.editing) {
                    if (e == Event::Return) { sf.commit_edit(); post(); return true; }
                    if (e == Event::Backspace) {
                        if (!sf.edit_buf.empty()) sf.edit_buf.pop_back();
                        return true;
                    }
                    if (e.is_character()) {
                        if (sf.edit_buf.size() < 4096) sf.edit_buf += e.character();
                        return true;
                    }
                    return true;
                }
                if (e == Event::ArrowUp) { sf.move_up(); return true; }
                if (e == Event::ArrowDown) { sf.move_down(); return true; }
                if (e == Event::Return) { sf.begin_edit(); post(); return true; }
                return true;
            }

            // --- Profiles tab ---
            if (sf.current_tab == SettingsForm::Tab::Profiles) {
                // Profile list mode
                if (!pe.editing_profile) {
                    if (e == Event::ArrowUp) { pe.move_up(); post(); return true; }
                    if (e == Event::ArrowDown) { pe.move_down(); post(); return true; }
                    if (e == Event::Return) {
                        pe.begin_edit_profile(); post(); return true;
                    }
                    if (e == Event::Character('a') || e == Event::Character('A')) {
                        pe.add_profile(); sf.dirty = true; post(); return true;
                    }
                    if (e == Event::Character('d') || e == Event::Character('D')) {
                        if (pe.sel >= 0 && pe.sel < static_cast<int>(pe.profiles.size())) {
                            if (sf.draft.profile == pe.profiles[pe.sel].name)
                                sf.draft.profile.clear();
                            pe.delete_profile(pe.sel);
                            sf.dirty = true;
                        }
                        post(); return true;
                    }
                    return true;
                }

                // Profile detail mode: editing_profile == true
                if (pe.editing_models) {
                    if (e == Event::ArrowUp) {
                        if (pe.model_sel > 0) --pe.model_sel;
                        return true;
                    }
                    if (e == Event::ArrowDown) {
                        const auto& models = pe.profiles[pe.sel].models;
                        if (pe.model_sel + 1 < static_cast<int>(models.size()) + 1)
                            ++pe.model_sel;
                        return true;
                    }
                    if (e == Event::Return) {
                        const auto& models = pe.profiles[pe.sel].models;
                        if (pe.model_sel < static_cast<int>(models.size())) {
                            pe.begin_edit_model(); post(); return true;
                        } else {
                            pe.add_model(); post(); return true;
                        }
                    }
                    if (e == Event::Character('a') || e == Event::Character('A')) {
                        pe.add_model(); post(); return true;
                    }
                    if (e == Event::Character('d') || e == Event::Character('D')) {
                        pe.delete_model(pe.model_sel); post(); return true;
                    }
                    if (e == Event::Backspace) {
                        if (!pe.model_edit_buf.empty()) pe.model_edit_buf.pop_back();
                        return true;
                    }
                    if (e.is_character()) {
                        if (pe.model_edit_buf.size() < 256)
                            pe.model_edit_buf += e.character();
                        return true;
                    }
                    return true;
                }

                if (pe.profile_edit_text_mode) {
                    if (e == Event::Return) {
                        pe.commit_profile_field_edit(); post(); return true;
                    }
                    if (e == Event::Backspace) {
                        if (!pe.edit_buf.empty()) pe.edit_buf.pop_back();
                        return true;
                    }
                    if (e.is_character()) {
                        if (pe.edit_buf.size() < 4096) pe.edit_buf += e.character();
                        return true;
                    }
                    return true;
                }

                // Profile detail navigation mode
                if (e == Event::ArrowUp) {
                    pe.profile_field_up(); return true;
                }
                if (e == Event::ArrowDown) {
                    pe.profile_field_down(); return true;
                }
                if (e == Event::Return) {
                    pe.begin_edit_profile_field(); post(); return true;
                }
                return true;
            }

            // --- Credentials tab ---
            if (sf.current_tab == SettingsForm::Tab::Credentials) {
                if (ce.editing_key) {
                    if (e == Event::Return) {
                        ce.commit_key(); sf.dirty = true; post(); return true;
                    }
                    if (e == Event::Backspace) {
                        if (!ce.key_edit_buf.empty()) ce.key_edit_buf.pop_back();
                        return true;
                    }
                    if (e.is_character()) {
                        ce.key_edit_buf += e.character();
                        return true;
                    }
                    return true;
                }
                if (e == Event::ArrowUp) { ce.move_up(); return true; }
                if (e == Event::ArrowDown) { ce.move_down(); return true; }
                if (e == Event::Return) { ce.begin_edit_key(); post(); return true; }
                if (e == Event::Character('d') || e == Event::Character('D')) {
                    ce.delete_key(); sf.dirty = true; post(); return true;
                }
                return true;
            }

            return true;  // swallow everything else while the modal is up
        }
        // While the picker overlay is up, navigate it and swallow other keys.
        if (picker.active) {
            if (e == Event::ArrowUp) {
                std::lock_guard lk(state_mtx);
                if (picker.sel > 0) --picker.sel;
                return true;
            }
            if (e == Event::ArrowDown) {
                std::lock_guard lk(state_mtx);
                if (picker.sel + 1 < static_cast<int>(picker.items.size()))
                    ++picker.sel;
                return true;
            }
            if (e == Event::Return) {
                std::function<void()> action;
                {
                    std::lock_guard lk(state_mtx);
                    if (picker.sel >= 0 &&
                        picker.sel < static_cast<int>(picker.on_choose.size()))
                        action = picker.on_choose[picker.sel];
                    picker.active = false;
                }
                if (action) action();  // load_into/rewind take state_mtx themselves
                post();
                return true;
            }
            if (e == Event::Escape) {
                std::lock_guard lk(state_mtx);
                picker.active = false;
                post();
                return true;
            }
            return true;  // swallow everything else while the picker is up
        }
        // @-completion / model-completion popup: navigate / accept / dismiss.
        // Other keys fall through to the Input so typing keeps filtering.
        {
            bool popup;
            bool is_model;
            { std::lock_guard lk(state_mtx);
              popup = model_complete.active || complete.active;
              is_model = model_complete.active; }
            if (popup) {
                if (e == Event::ArrowUp) {
                    std::lock_guard lk(state_mtx);
                    if (is_model && model_complete.sel > 0) --model_complete.sel;
                    else if (!is_model && complete.sel > 0) --complete.sel;
                    return true;
                }
                if (e == Event::ArrowDown) {
                    std::lock_guard lk(state_mtx);
                    if (is_model && model_complete.sel + 1 < static_cast<int>(model_complete.items.size()))
                        ++model_complete.sel;
                    else if (!is_model && complete.sel + 1 < static_cast<int>(complete.items.size()))
                        ++complete.sel;
                    return true;
                }
                if (e == Event::Tab || e == Event::Return) {
                    if (is_model) slash.accept_complete();
                    else accept_complete();
                    post();
                    return true;
                }
                if (e == Event::Escape) {
                    std::lock_guard lk(state_mtx);
                    model_complete = ModelComplete{};
                    complete = Complete{};
                    post();
                    return true;
                }
            }
        }
        // --- Detail maximize overlay: modal scroll/close ---------------------
        if (detail_max) {
            auto scroll_max = [&](float d) {
                detail_max_scroll = std::clamp(detail_max_scroll + d, 0.f, 1.f);
            };
            const int page = std::max(1, (dmax_view.y_max - dmax_view.y_min) - 1);
            if (e == Event::Escape) { detail_max = false; return true; }
            if (e == Event::ArrowUp) { scroll_max(-frac(1, dmax_ch, dmax_view)); return true; }
            if (e == Event::ArrowDown) { scroll_max(+frac(1, dmax_ch, dmax_view)); return true; }
            if (e == Event::PageUp) { scroll_max(-frac(page, dmax_ch, dmax_view)); return true; }
            if (e == Event::PageDown) { scroll_max(+frac(page, dmax_ch, dmax_view)); return true; }
            if (e.is_mouse() && e.mouse().button == Mouse::WheelUp)
                { scroll_max(-frac(kWheelLines, dmax_ch, dmax_view)); return true; }
            if (e.is_mouse() && e.mouse().button == Mouse::WheelDown)
                { scroll_max(+frac(kWheelLines, dmax_ch, dmax_view)); return true; }
            return true;  // modal: swallow everything else
        }

        // --- Focus zones: Tab cycles input → activity → chat -----------------
        if (e == Event::Tab) {
            zone = next_zone(zone);
            if (zone != Zone::Input) {
                const auto pane =
                    zone == Zone::Chat        ? NodeKey::Pane::Chat
                    : zone == Zone::Subagents ? NodeKey::Pane::Subagents
                                              : NodeKey::Pane::Activity;
                std::lock_guard lk(state_mtx);
                // Entering a zone without a usable selection starts at the
                // newest node — what investigation almost always targets.
                const auto& sel = state.selection();
                if (!sel || sel->pane != pane) state.select_last(pane);
                if (zone == Zone::Chat) follow_chat = true;
                else if (zone == Zone::Activity) follow_act = true;
            }
            post();
            return true;
        }

        // --- Pane-zone navigation --------------------------------------------
        if (zone != Zone::Input) {
            const NodeKey::Pane zone_pane =
                zone == Zone::Chat        ? NodeKey::Pane::Chat
                : zone == Zone::Subagents ? NodeKey::Pane::Subagents
                                          : NodeKey::Pane::Activity;
            if (e == Event::ArrowUp || e == Event::ArrowDown) {
                std::lock_guard lk(state_mtx);
                if (!state.selection() || state.selection()->pane != zone_pane)
                    state.select_last(zone_pane);
                else if (e == Event::ArrowUp)
                    state.select_prev();
                else
                    state.select_next();
                if (zone == Zone::Chat) follow_chat = true;
                else if (zone == Zone::Activity) follow_act = true;
                post();
                return true;
            }
            if (e == Event::ArrowLeft || e == Event::ArrowRight) {
                std::lock_guard lk(state_mtx);
                const auto& sel = state.selection();
                if (sel && (sel->pane == NodeKey::Pane::Subagents ||
                            sel->pane == NodeKey::Pane::Activity)) {
                    // Fold target: the selected group, or the group owning the
                    // selected nested call. ← on a nested call collapses its
                    // parent (set_expanded snaps the selection up).
                    std::string gid =
                        sel->parent_id.empty() ? sel->id : sel->parent_id;
                    if (state.find_group(gid))
                        state.set_expanded(gid, e == Event::ArrowRight);
                }
                post();
                return true;
            }
            if (e == Event::Return) {
                std::lock_guard lk(state_mtx);
                if (state.detail_node()) {
                    detail_max = true;
                    detail_max_scroll = 0.f;
                }
                post();
                return true;
            }
            if (e == Event::PageUp) {
                const int page = std::max(1, (detail_view.y_max - detail_view.y_min) - 1);
                detail_scroll = std::clamp(
                    detail_scroll - frac(page, detail_ch, detail_view), 0.f, 1.f);
                return true;
            }
            if (e == Event::PageDown) {
                const int page = std::max(1, (detail_view.y_max - detail_view.y_min) - 1);
                detail_scroll = std::clamp(
                    detail_scroll + frac(page, detail_ch, detail_view), 0.f, 1.f);
                return true;
            }
            if (e == Event::Escape) {
                zone = Zone::Input;
                post();
                return true;
            }
            // Typing snaps back to the input so the user can just start a
            // message without explicitly tabbing home.
            if (e.is_character()) {
                zone = Zone::Input;
                return false;  // let the Input component receive the character
            }
        }
        // Escape while the agent is running: interrupt the streaming LLM call.
        if (e == Event::Escape) {
            bool running;
            {
                std::lock_guard lk(state_mtx);
                running = state.running();
                if (running) {
                    // Drop the buffer now, under the lock, and latch the discard
                    // flag so an in-flight inject() / the worker's tail loop
                    // won't flush anything typed before the interrupt. Both the
                    // flag and the buffer are state_mtx-guarded — writing them
                    // here without the lock is a data race with the worker.
                    pending_queue.clear();
                    discard_buffer = true;
                }
            }
            if (running) {
                agent.cancel();
                return true;
            }
        }
        if (e == Event::F2) {
            std::lock_guard lk(state_mtx);
            state.toggle_reasoning();
            return true;
        }
        // Shift/Ctrl+Enter (and Alt+Enter as a no-config fallback) insert a
        // literal newline so the user can compose a multi-line message; plain
        // Enter still submits (handled by the Input's on_enter). Terminals
        // encode a modified Return differently — accept the common forms. The
        // CSI ~ form needs modifyOtherKeys, which we request around the loop;
        // the CSI u form comes from terminals speaking the kitty keyboard
        // protocol; ESC+CR is Alt+Enter, which most terminals send without any
        // mode change. The modifier digit is 2 for Shift and 5 for Ctrl.
        if (e == Event::Special("\x1B[27;2;13~") ||  // Shift+Enter (modifyOtherKeys)
            e == Event::Special("\x1B[27;5;13~") ||  // Ctrl+Enter  (modifyOtherKeys)
            e == Event::Special("\x1B[13;2u") ||     // Shift+Enter (kitty)
            e == Event::Special("\x1B[13;5u") ||     // Ctrl+Enter  (kitty)
            e == Event::Special("\x1B\r") ||         // Alt+Enter
            e == Event::Special("\x1B\n")) {
            insert_at_cursor("\n");
            post();
            return true;
        }
        // Ctrl+Y: yank the focused detail-pane content to the system clipboard
        // via OSC 52 — terminal-independent and SSH-safe, since FTXUI's mouse
        // tracking otherwise swallows native drag-to-copy.
        if (e == Event::CtrlY) {
            std::string content;
            { std::lock_guard lk(state_mtx); content = detail_plain_text(state); }
            std::string note =
                content.empty() ? "nothing focused to copy"
                : osc52_copy(content)
                    ? "copied " + human_tokens(static_cast<int>(content.size())) +
                          " bytes to clipboard"
                    : "clipboard copy failed (terminal rejected OSC 52)";
            { std::lock_guard lk(state_mtx); state.push_info(note); }
            post();
            return true;
        }
        if (e == Event::ArrowUp) {
            if (!history.empty() && hist_pos > 0) {
                --hist_pos;
                input_content = history[hist_pos];
                // Land the cursor at the end of the recalled line so it never
                // sits stale inside a chip (and matches shell history recall).
                cursor_pos = static_cast<int>(input_content.size());
            }
            return true;
        }
        if (e == Event::ArrowDown) {
            if (hist_pos < history.size()) {
                ++hist_pos;
                input_content = hist_pos == history.size() ? "" : history[hist_pos];
                cursor_pos = static_cast<int>(input_content.size());
            }
            return true;
        }
        const bool wheel_up =
            e.is_mouse() && e.mouse().button == Mouse::WheelUp;
        const bool wheel_down =
            e.is_mouse() && e.mouse().button == Mouse::WheelDown;

        // Route a wheel notch (dir = -1 up, +1 down) to the pane under the
        // cursor, moving a fixed kWheelLines lines so the distance is the same
        // on every buffer length and a burst of events can't fly to an edge.
        auto apply_wheel = [&](int dir) {
            int tw = Terminal::Size().dimx;
            int th = Terminal::Size().dimy;
            // Mirror the renderer's split (same floors), so hit regions match.
            int aw = std::max(12, tw * 2 / 5);
            int dh = std::max(8, th * 2 / 5);
            switch (wheel_target(e.mouse().x, e.mouse().y, tw, th, aw, dh)) {
                case Pane::Chat:
                    chat_scroll = std::clamp(
                        chat_scroll + dir * frac(kWheelLines, chat_ch, chat_view),
                        0.f, 1.f);
                    follow_chat = false;
                    break;
                case Pane::Activity:
                    activity_scroll = std::clamp(
                        activity_scroll + dir * frac(kWheelLines, act_ch, act_view),
                        0.f, 1.f);
                    follow_act = false;
                    break;
                case Pane::Detail:
                    detail_scroll = std::clamp(
                        detail_scroll + dir * frac(kWheelLines, detail_ch, detail_view),
                        0.f, 1.f);
                    break;
                case Pane::None:
                    break;
            }
        };

        if (e == Event::PageUp) {
            const int page = std::max(1, (chat_view.y_max - chat_view.y_min) - 1);
            chat_scroll =
                std::clamp(chat_scroll - frac(page, chat_ch, chat_view), 0.f, 1.f);
            follow_chat = false;
            return true;
        }
        if (e == Event::PageDown) {
            const int page = std::max(1, (chat_view.y_max - chat_view.y_min) - 1);
            chat_scroll =
                std::clamp(chat_scroll + frac(page, chat_ch, chat_view), 0.f, 1.f);
            follow_chat = false;
            return true;
        }
        if (wheel_up) {
            apply_wheel(-1);
            return true;
        }
        if (wheel_down) {
            apply_wheel(+1);
            return true;
        }
        // --- Mouse: click a row to select; click it again to inspect ---------
        if (e.is_mouse() && e.mouse().button == Mouse::Left &&
            e.mouse().motion == Mouse::Pressed) {
            std::lock_guard lk(state_mtx);
            for (const Hit& h : hits) {
                if (!h.box.Contain(e.mouse().x, e.mouse().y)) continue;
                switch (classify_row_click(h.key, h.expander, state.selection())) {
                    case RowClick::Fold:
                        if (const auto* g = state.find_group(h.key.id))
                            state.set_expanded(h.key.id, !g->expanded);
                        break;
                    case RowClick::Maximize:
                        detail_max = true;
                        detail_max_scroll = 0.f;
                        break;
                    case RowClick::Select:
                        state.select(h.key);
                        zone = h.key.pane == NodeKey::Pane::Chat ? Zone::Chat
                             : h.key.pane == NodeKey::Pane::Subagents
                                   ? Zone::Subagents
                                   : Zone::Activity;
                        // The clicked row is on screen — don't let the focus
                        // decorator yank the pane's scroll position.
                        if (zone == Zone::Chat) follow_chat = false;
                        else if (zone == Zone::Activity) follow_act = false;
                        break;
                }
                post();
                return true;
            }
            return false;  // not on a row: let FTXUI route it (input click, …)
        }
        // Ctrl+C: double-press within 600 ms exits; single press is ignored so
        // it can be used to copy text from the terminal without closing.
        if (e == Event::CtrlC) {
            auto now = std::chrono::steady_clock::now();
            if (now - last_ctrl_c < std::chrono::milliseconds(600)) {
                screen.Exit();
                return true;
            }
            last_ctrl_c = now;
            {
                std::lock_guard lk(state_mtx);
                state.push_info("press Ctrl+C again within 0.6s to exit");
            }
            post();
            return true;
        }
        // Chips behave as a single character: one keystroke skips or deletes a
        // whole `[Pasted text #N]` / `[img#N]` token rather than nibbling its
        // bytes. cursor_pos is a byte offset (FTXUI convention) and the markers
        // are ASCII, so the byte ranges line up with glyphs. Each handler falls
        // through to the Input when the cursor isn't flush against a chip.
        auto chip_cur = [&] {
            return std::min(static_cast<std::size_t>(std::max(0, cursor_pos)),
                            input_content.size());
        };
        // ArrowLeft / ArrowRight: hop over a paste chip in one step.
        if (e == Event::ArrowLeft) {
            if (auto r = paste_chip_ending_at(input_content, chip_cur())) {
                cursor_pos = static_cast<int>(r->first);
                post();
                return true;
            }
            return false;
        }
        if (e == Event::ArrowRight) {
            if (auto r = paste_chip_starting_at(input_content, chip_cur())) {
                cursor_pos = static_cast<int>(r->second);
                post();
                return true;
            }
            return false;
        }
        // Delete: forward-delete a whole paste chip sitting just right of the
        // cursor. (Its bytes stay stashed in case the same marker is re-entered.)
        if (e == Event::Delete) {
            if (auto r = paste_chip_starting_at(input_content, chip_cur())) {
                input_content.erase(r->first, r->second - r->first);
                cursor_pos = static_cast<int>(r->first);
                post();
                return true;
            }
            return false;
        }
        // Backspace just right of a chip removes the whole token in one keystroke
        // (an image chip also detaches its image). Try the paste chip first, then
        // the image chip; anything else falls through to the Input.
        if (e == Event::Backspace) {
            if (auto r = paste_chip_ending_at(input_content, chip_cur())) {
                input_content.erase(r->first, r->second - r->first);
                cursor_pos = static_cast<int>(r->first);
                post();
                return true;
            }
            auto range = chip_ending_at(input_content, chip_cur());
            if (!range) return false;  // ordinary single-glyph backspace
            input_content.erase(range->first, range->second - range->first);
            cursor_pos = static_cast<int>(range->first);
            reconcile_images();
            post();
            return true;  // handled atomically
        }
        // Ctrl+V / Ctrl+Alt+V: check for an image on the clipboard. When the
        // user presses Ctrl+V (or Ctrl+Shift+V / Ctrl+Alt+V) and the clipboard
        // holds an image (e.g. from a screenshot tool), attach it for the next
        // submit. The event is swallowed so ^V doesn't appear in the input.
        // Note: most terminals intercept Ctrl+V for text-paste and never
        // forward the byte; this handler activates in terminals that pass it
        // through or when the terminal has no text to paste (image-only clip).
        if (e == Event::CtrlV || e == Event::CtrlAltV) {
            scan_clipboard();
            return true;  // swallow
        }
        return false;
    });

    // Set up the screen-independent input modes around the loop; FTXUI manages
    // neither, so we restore each afterwards, and any a terminal doesn't grok it
    // ignores. (1) Bracketed paste (DEC private mode 2004): a multi-line paste
    // arrives as one chunk instead of a burst of Return keys (each of which
    // would otherwise submit a separate message). (2) modifyOtherKeys level 1:
    // lets xterm-family terminals distinguish some modified keys without
    // reformatting Ctrl+C/Ctrl+Y/Tab. The kitty keyboard protocol — the scheme
    // that actually disambiguates Shift/Ctrl+Enter, which modifyOtherKeys leaves
    // alone as a "well-known" key — is enabled separately from the first render
    // (see kitty_pushed): its flags are per-screen and must be pushed only after
    // FTXUI has switched to the alt screen. The trailing pop here is a harmless
    // no-op on the main screen (the alt screen's stack is discarded on exit)
    // that still tidies up on any terminal not keeping separate stacks.
    write_tty_raw("\033[?2004h\033[>4;1m");
    screen.Loop(root);
    write_tty_raw("\033[<u\033[?2004l\033[>4m");

    // Shutdown: release any pending approval, stop the worker (waits for any
    // in-flight turn), then the refresher. The screen outlives both joins so
    // their PostEvent calls stay valid.
    gate.release();
    question_gate.release();
    worker.stop();  // sets the stop flag, wakes the worker, and joins
    ui_alive.store(false);
    refresher.join();

    save_now();  // final save on clean exit (no-op if nothing was started)
    return 0;
}

}  // namespace moocode
