#include "agent/question_tool.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

#include "agent/json_util.hpp"

namespace moocode {

// --- QuestionGate ---------------------------------------------------------

std::optional<std::string> QuestionGate::request(std::string question,
                                                  std::vector<std::string> options) {
    std::unique_lock lk(m_);
    if (released_) return std::nullopt;
    question_ = Question{std::move(question), std::move(options)};
    answer_.reset();
    halted_ = false;
    lk.unlock();
    if (notify_) notify_();
    lk.lock();
    cv_.wait(lk, [&] { return answer_.has_value() || halted_ || released_; });
    if (released_ || halted_) {
        question_.reset();
        answer_.reset();
        halted_ = false;
        return std::nullopt;
    }
    std::optional<std::string> result = std::move(answer_);
    question_.reset();
    answer_.reset();
    return result;
}

bool QuestionGate::pending() const {
    std::lock_guard lk(m_);
    return question_.has_value();
}

std::optional<QuestionGate::Question> QuestionGate::pending_question() const {
    std::lock_guard lk(m_);
    return question_;
}

void QuestionGate::answer(std::string text) {
    {
        std::lock_guard lk(m_);
        answer_ = std::move(text);
    }
    cv_.notify_all();
}

void QuestionGate::halt() {
    {
        std::lock_guard lk(m_);
        halted_ = true;
    }
    cv_.notify_all();
}

void QuestionGate::release() {
    {
        std::lock_guard lk(m_);
        released_ = true;
    }
    cv_.notify_all();
}

// --- ask_user tool factory ------------------------------------------------

namespace {

using Result = std::expected<std::string, Error>;

}  // namespace

Tool ask_user_tool(QuestionGate* gate) {
    ToolSpec spec{
        "ask_user",
        "Ask user a question with list of options to pick from. "
        "User can pick an option, edit one, type custom answer, or "
        "dismiss (Esc). On dismiss, tool returns \"USER_HALTED\" — "
        "continue without an answer. Use when genuinely uncertain and "
        "need user to decide — do not ask things you can determine yourself.",
        nlohmann::json::parse(R"({
            "type":"object",
            "properties":{
              "question":{
                "type":"string",
                "description":"Question to ask user"
              },
              "options":{
                "type":"array",
                "items":{"type":"string"},
                "description":"Options for user to pick from",
                "minItems":1
              }
            },
            "required":["question","options"]
        })")};

    return Tool{
        .spec = std::move(spec),
        .run = [gate](const nlohmann::json& a) -> Result {
            // --- Parse question ---
            auto q = ::moocode::json::arg_string(a, "question");
            if (!q) return std::unexpected(q.error());
            if (q->empty())
                return std::unexpected(
                    Error{.msg = "question must not be empty", .code = 0});

            // --- Parse options ---
            if (!a.contains("options") || !a["options"].is_array())
                return std::unexpected(
                    Error{.msg = "missing or non-array argument: options",
                          .code = 0});
            std::vector<std::string> opts;
            for (const auto& item : a["options"]) {
                if (!item.is_string())
                    return std::unexpected(
                        Error{.msg = "options array must contain only strings",
                              .code = 0});
                opts.push_back(item.get<std::string>());
            }
            if (opts.empty())
                return std::unexpected(
                    Error{.msg = "options array must not be empty", .code = 0});

            // --- Ask the user ---
            if (gate) {
                // TUI (or testing) path: block on the gate.
                auto answer = gate->request(std::move(*q), std::move(opts));
                if (!answer)
                    return std::string("USER_HALTED");
                return *answer;
            }

            // Non-interactive fallback: write to stderr, read from /dev/tty.
            std::fprintf(stderr, "\n❓ %s\n", q->c_str());
            for (std::size_t i = 0; i < opts.size(); ++i)
                std::fprintf(stderr, "  %zu. %s\n", i + 1, opts[i].c_str());
            std::fprintf(stderr,
                         "  Pick an option number, type custom text, "
                         "or press Enter to skip: ");
            std::fflush(stderr);

            // Try /dev/tty first (works even when stdin is piped).
            FILE* tty = std::fopen("/dev/tty", "r");
            if (!tty) tty = stdin;

            std::string line;
            int c;
            while ((c = std::fgetc(tty)) != EOF && c != '\n')
                line += static_cast<char>(c);

            if (tty != stdin) std::fclose(tty);

            // Trim trailing carriage return.
            while (!line.empty() && line.back() == '\r')
                line.pop_back();

            if (line.empty())
                return std::string("USER_HALTED");

            // If line is a number in range, return the corresponding option.
            char* end = nullptr;
            long idx = std::strtol(line.c_str(), &end, 10);
            if (end && *end == '\0' && idx >= 1 &&
                static_cast<std::size_t>(idx) <= opts.size())
                return opts[static_cast<std::size_t>(idx) - 1];

            // Otherwise return the line verbatim as a custom answer.
            return line;
        }};
}

}  // namespace moocode
