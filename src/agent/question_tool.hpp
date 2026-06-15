#ifndef MOOCODE_QUESTION_TOOL_HPP
#define MOOCODE_QUESTION_TOOL_HPP

// The ask_user tool: lets the LLM pause mid-turn and ask the user a "pick one"
// question. A QuestionGate synchronisation primitive mirrors ApprovalGate: the
// tool's run() blocks the worker thread until the UI answers (or the user
// dismisses). Non-interactive mode falls back to stdin/stderr I/O.

#include <condition_variable>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "agent/tools.hpp"

namespace moocode {

// Worker↔UI handshake for a single question. The worker calls request() and
// blocks; the UI polls pending() / pending_question(), then calls answer() or
// halt(); release() unblocks any waiter and rejects future requests.
class QuestionGate {
public:
    struct Question {
        std::string question;
        std::vector<std::string> options;
    };

    // Worker thread: block until the user answers or dismisses.
    // Returns the user's answer (selected option, edited option, or custom
    // text), or std::nullopt when the user halts or the gate is released.
    // post: pending() is false on return.
    std::optional<std::string> request(std::string question,
                                       std::vector<std::string> options);

    // UI thread: a question is awaiting a decision.
    bool pending() const;
    // Snapshot the pending question. Returns nullopt when nothing is awaiting.
    std::optional<Question> pending_question() const;

    // UI thread: resolve the pending question with an answer string.
    void answer(std::string text);

    // UI thread: resolve as halt (user dismissed the modal).
    void halt();

    // Unblock any pending request as halt; future requests also return nullopt.
    void release();

    // Callback fired (once, unlocked) when a request begins waiting.
    void on_request(std::function<void()> fn) { notify_ = std::move(fn); }

private:
    // Held for the whole of request() so concurrent ask_user calls (tool calls
    // now run in parallel — see Agent::run) queue behind one another instead of
    // clobbering the single question slot: one modal is shown at a time.
    std::mutex serialize_;
    mutable std::mutex m_;
    std::condition_variable cv_;
    std::optional<Question> question_;
    std::optional<std::string> answer_;
    bool halted_ = false;
    bool released_ = false;
    std::function<void()> notify_;
};

// Build the ask_user tool. `gate` must outlive the returned Tool.
// When gate is nullptr (non-interactive mode), the tool reads from /dev/tty
// (or stdin) instead of blocking on the gate.
Tool ask_user_tool(QuestionGate* gate);

}  // namespace moocode

#endif  // MOOCODE_QUESTION_TOOL_HPP
