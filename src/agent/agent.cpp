#include "agent/agent.hpp"

#include <climits>
#include <sstream>
#include <string>
#include <utility>

#include "agent/json_util.hpp"
#include "agent/strutil.hpp"

namespace moocode {

namespace {

// Strip the interior of fenced code blocks (```\n...\n```) from `s`, replacing
// the content with a marker so file paths / tool names are still visible but
// bulk file contents don't bloat the summarise prompt.  Three-backtick fences
// only; indented and tilde fences are left untouched (they're rare in agent
// output).  Keeps the opening fence line (with its optional language tag) and
// closing fence; the payload becomes "[contents omitted]".
std::string strip_fenced_blocks(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    std::size_t pos = 0;
    while (pos < s.size()) {
        // Find a line that starts with ```
        auto fence = s.find("\n```", pos);
        if (fence == std::string_view::npos) {
            out.append(s.substr(pos));
            break;
        }
        // `fence` points to the \n before ```; include through the end of the
        // opening-fence line.
        std::size_t open_end = s.find('\n', fence + 4);
        if (open_end == std::string_view::npos) open_end = s.size();
        // Copy everything up to and including the opening fence line.
        out.append(s.substr(pos, open_end - pos));
        // Now find the closing ``` (must be on its own line: \n```\n or \n``` at EOF).
        std::size_t close = s.find("\n```", open_end);
        std::size_t after = close;
        if (close != std::string_view::npos) {
            after = s.find('\n', close + 4);
            if (after == std::string_view::npos) after = s.size();
        }
        if (close != std::string_view::npos) {
            out.append("\n[contents omitted]");
            // Append the closing fence line.
            out.append(s.substr(close, after - close));
            pos = after;
        } else {
            // No closing fence: copy the rest verbatim (malformed input).
            out.append(s.substr(open_end));
            break;
        }
    }
    return out;
}

// Render one Message in a compact, model-readable form. Tool calls are shown
// inline with the assistant turn that issued them; tool results are shown on
// their own line tagged with the tool call id so the summary preserves
// causality. Fenced code blocks are stripped from message content so bulk file
// contents don't bloat the summarise prompt — file paths and metadata headers
// are preserved. Reasoning is never in history, so we don't render it.
std::string render_for_summary(const Message& m) {
    std::ostringstream os;
    switch (m.role()) {
        case Role::System:
            os << "[system]\n" << strip_fenced_blocks(m.content());
            break;
        case Role::User:
            os << "[user]\n" << strip_fenced_blocks(m.content());
            break;
        case Role::Assistant:
            os << "[assistant]";
            if (!m.content().empty())
                os << "\n" << strip_fenced_blocks(m.content());
            for (const auto& tc : m.tool_calls()) {
                os << "\n  tool_call " << tc.name << "(" << tc.arguments_json << ")";
            }
            break;
        case Role::Tool:
            os << "[tool:" << m.tool_call_id() << "]\n"
               << strip_fenced_blocks(m.content());
            break;
    }
    return os.str();
}

// One-shot summary request. The system prompt is preserved (so the model
// summarises in role), tools are omitted (summaries never invoke tools), and
// the cancel flag is honored. Returns the summary text only (no tool calls).
// When `instructions` is non-empty it is appended as an extra sentence,
// letting the user guide what the summary should focus on.
std::expected<std::string, Error> summarise(Provider& provider,
                                             const std::string& system,
                                             const std::string& convo,
                                             const StreamFn& on_delta,
                                             const std::atomic<bool>* cancel,
                                             std::string_view instructions) {
    Conversation req;
    if (!system.empty()) {
        req.push_back(Message::system(system));
    }
    std::string prompt =
        "Summarise the following conversation between a user and a coding "
        "agent. Preserve: the user's original task, key files read or edited "
        "and their paths, important decisions or constraints, the final state "
        "or last action, and any open follow-ups. Drop raw file contents and "
        "routine tool outputs — keep only one-line mentions of what they were. "
        "Write in the same language as the conversation. Be concise: aim for "
        "under 400 words.";
    if (!instructions.empty()) {
        prompt += " Additional instructions: ";
        prompt += instructions;
        prompt += ".";
    }
    prompt += " Output the summary only, no preamble.\n\n"
              "--- conversation ---\n" + convo + "\n--- end ---";
    req.push_back(Message::user(prompt));
    auto turn = provider.complete_stream(req, /*tools=*/{}, on_delta, cancel);
    if (!turn) return std::unexpected(turn.error());
    return turn->text;
}

}  // namespace

Agent::Agent(Provider& provider, ToolRegistry& tools, AgentConfig config)
    : provider_(&provider), tools_(tools), config_(std::move(config)) {}

void Agent::on_message(ProgressFn fn) { on_message_ = std::move(fn); }

void Agent::on_delta(StreamFn fn) { on_delta_ = std::move(fn); }

void Agent::on_approve(ApprovalFn fn) { approve_ = std::move(fn); }

void Agent::cancel() { cancel_.store(true, std::memory_order_release); }

void Agent::on_usage(UsageFn fn) { on_usage_ = std::move(fn); }

void Agent::append(Message m) {
    conv_.push_back(std::move(m));
    if (on_message_) on_message_(conv_.back());
}

std::expected<std::string, Error> Agent::run(std::string user_prompt,
                                               std::vector<ContentPart> user_parts) {
    // Reset the cancellation flag so a cancellation from an earlier run does
    // not abort this one. cancel() can be called from another thread while
    // run() is in progress.
    cancel_.store(false, std::memory_order_release);

    // System prompt is prepended once, on the first turn only; subsequent
    // run() calls continue the same conversation (REPL multi-turn).
    if (conv_.empty() && !config_.system_prompt.empty())
        conv_.push_back(Message::system(config_.system_prompt));
    conv_.push_back(Message::user(std::move(user_prompt), std::move(user_parts)));

    const auto cap = config_.max_iterations;  // nullopt => run until the model stops
    for (std::uint32_t iter = 0; !cap || iter < *cap; ++iter) {
        // Check cancellation flag — aborted before a turn starts after an Esc.
        // Also honour an external (parent) cancel source when installed.
        if (cancel_.load(std::memory_order_acquire) ||
            (watch_cancel_ && watch_cancel_->load(std::memory_order_acquire)))
            return std::unexpected(Error{.msg = "interrupted by user", .code = 0});
        // When a watched source is set it supersedes the local flag — the
        // sub-agent never calls its own cancel(), only the parent flag is set
        // via Esc. This ensures Esc also aborts in-flight HTTP requests.
        const std::atomic<bool>* stream_cancel =
            watch_cancel_ ? watch_cancel_ : &cancel_;
        // --no-tools (advertise_tools == false) sends no tool schema, so the
        // model gets a chat-only request and never emits tool_calls.
        auto specs =
            config_.advertise_tools ? tools_.specs() : std::vector<ToolSpec>{};
        auto turn = provider_->complete_stream(conv_, specs, on_delta_,
                                               stream_cancel);
        if (!turn) return std::unexpected(turn.error());

        if (turn->usage.present && on_usage_) on_usage_(turn->usage);

        append(Message::assistant(turn->text, turn->tool_calls, turn->reasoning));

        if (turn->tool_calls.empty()) return turn->text;  // model is done

        for (const auto& tc : turn->tool_calls) {
            std::string result;
            bool failed = false;
            if (approve_ && !approve_(tc)) {
                result = "ERROR: user declined to run " + tc.name;
                failed = true;
            } else {
                // Empty args string is common shorthand for "{}".
                std::string raw = tc.arguments_json.empty() ? "{}" : tc.arguments_json;
                auto args = json::parse(raw);
                if (!args) {
                    result =
                        "ERROR: could not parse arguments as JSON: " + args.error().msg;
                    failed = true;
                } else {
                    auto out = tools_.invoke(tc.name, *args);
                    if (out) {
                        result = *out;
                    } else {
                        result = "ERROR: " + out.error().msg;
                        failed = true;
                    }
                }
            }
            // Failure is recorded as data here (the truth is known now); the
            // "ERROR: " prefix is kept only as context for the model/human.
            append(Message::tool(
                tc.id,
                truncate(std::move(result), config_.max_tool_output,
                         default_trunc_marker),
                failed));
        }
    }
    // Only reached when a cap is set (an uncapped loop never exits here).
    return std::unexpected(
        Error{.msg = "max_iterations (" + std::to_string(*cap) +
                  ") exceeded without a final answer",
            .code = 0});
}

std::expected<Conversation, Error> Agent::compact(
    std::string_view instructions) {
    // Locate the system prompt (always at index 0 when present, mirroring
    // run()'s prepend rule). Everything after is the "compressible" body.
    std::size_t sys_idx = 0;
    std::string system;
    if (!conv_.empty() && conv_[0].role() == Role::System) {
        system = conv_[0].content();
        sys_idx = 1;
    }
    // Keep the most recent messages that fit a token budget verbatim and fold
    // everything older into a single summary. A token budget (rather than a
    // fixed message count) keeps the retained context size predictable
    // regardless of how large individual tool results are. Anchoring on a
    // recent tail — rather than on the last user turn — is deliberate: in a
    // coding session the bulk of the tokens is the assistant/tool loop that
    // *follows* the latest user prompt, so a last-user-turn anchor either
    // summarises nothing (a single long turn) or discards the recent work. The
    // summarise prompt preserves the original task from the folded prefix.
    const std::size_t n = conv_.size();
    const std::size_t budget_chars = config_.compact_keep_tail_tokens * 4;
    auto msg_chars = [](const Message& m) {
        std::size_t c = m.content().size();
        if (m.role() == Role::Assistant)
            for (const auto& tc : m.tool_calls())
                c += tc.name.size() + tc.arguments_json.size();
        return c;
    };
    // Select the tail [cut, n): walk back from the end accumulating estimated
    // size, always keeping the single most recent message, then stopping before
    // the budget would be exceeded.
    std::size_t cut = n;
    std::size_t acc = 0;
    for (std::size_t i = n; i > sys_idx; --i) {
        const std::size_t c = msg_chars(conv_[i - 1]);
        if (cut != n && acc + c > budget_chars) break;  // keep ≥1 message
        acc += c;
        cut = i - 1;
    }
    // The verbatim tail must not begin with a tool result whose initiating
    // tool_call would land in the summarised prefix — that orphaned
    // tool_result breaks tool_use/tool_result pairing at the provider. Walk the
    // boundary back onto the assistant turn that issued the pending calls so
    // the pair is kept together.
    while (cut > sys_idx && conv_[cut].role() == Role::Tool) --cut;
    // Nothing worth folding: the body already fits in the kept tail, or only a
    // single message would be summarised.
    if (cut <= sys_idx + 1) return conv_;

    // Build a textual rendering of the body that needs summarising (everything
    // between the system prompt and the kept tail).
    std::string body;
    {
        std::ostringstream os;
        for (std::size_t i = sys_idx; i < cut; ++i)
            os << render_for_summary(conv_[i]) << "\n\n";
        body = os.str();
    }

    // Reset the cancellation flag right before the LLM call so a cancellation
    // from an earlier operation is consumed and does not abort this compaction.
    // The flag can still be raised during the summarise call itself (Esc during
    // streaming), which is the intended cancel path. Placing the reset here
    // eliminates the window where a spurious cancel() between an early reset
    // and the summarise call would poison every subsequent compact.
    cancel_.store(false, std::memory_order_release);

    // Honour an external (parent) cancel source alongside the local flag,
    // mirroring run(). When a watched source is set it supersedes the local
    // flag so the in-flight HTTP request is also aborted on parent Esc.
    const std::atomic<bool>* stream_cancel =
        watch_cancel_ ? watch_cancel_ : &cancel_;

    // The summary is delivered via on_delta so the TUI can show progress.
    auto summary = summarise(*provider_, system, body, on_delta_, stream_cancel,
                             instructions);
    if (!summary) return std::unexpected(summary.error());

    // Assemble the new history: [system?, summary, recent tail verbatim]. The
    // summary goes in as a User message so the model treats it as prior
    // context, not as new instructions. The tail [cut, n) is preserved
    // verbatim so the live request and latest tool exchange survive intact.
    Conversation next;
    if (!system.empty()) {
        next.push_back(Message::system(system));
    }
    next.push_back(
        Message::user("Summary of earlier conversation:\n\n" + *summary));
    for (std::size_t i = cut; i < n; ++i) next.push_back(conv_[i]);
    return next;
}

int estimated_tokens(const Conversation& conv) {
    std::size_t chars = 0;
    for (const auto& m : conv) {
        chars += m.content().size();
        if (m.role() == Role::Assistant)
            for (const auto& tc : m.tool_calls())
                chars += tc.name.size() + tc.arguments_json.size();
    }
    // Saturate at INT_MAX to avoid UB in the /4 division on overflow
    // (conversations this large can't fit any real context window anyway).
    if (chars > static_cast<std::size_t>(INT_MAX))
        chars = static_cast<std::size_t>(INT_MAX);
    return std::max(1, static_cast<int>(chars / 4));
}

}  // namespace moocode
