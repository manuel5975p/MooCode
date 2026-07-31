#include "gui/conversation_import.hpp"

#include <algorithm>
#include <string_view>
#include <utility>

namespace moocode::gui {
namespace {

// `s` with runs of whitespace collapsed to single spaces and clipped to `cap`
// characters (an ellipsis marks a clip). For folding a tool's JSON arguments
// onto the one line that names the call.
std::string one_line(std::string_view s, std::size_t cap) {
    std::string out;
    out.reserve(std::min(s.size(), cap) + 1);
    bool space = false;
    for (const char c : s) {
        if (c == '\n' || c == '\r' || c == '\t' || c == ' ') {
            space = !out.empty();
            continue;
        }
        if (space) {
            out.push_back(' ');
            space = false;
        }
        if (out.size() >= cap) {
            out += "…";
            break;
        }
        out.push_back(c);
    }
    return out;
}

std::string clip(const std::string& s, std::size_t cap) {
    if (s.size() <= cap) return s;
    return s.substr(0, cap) + "\n… (" + std::to_string(s.size() - cap) +
           " more characters)";
}

// How a folded call and its result read in the transcript.
std::string call_line(const ToolCall& c) {
    return "`▸ " + c.name + "(" + one_line(c.arguments_json, 160) + ")`";
}

std::string result_block(const std::string& content) {
    return "```\n" + clip(content, kToolResultClip) + "\n```";
}

}  // namespace

Conversation to_chat_only(const Conversation& in, bool& folded) {
    Conversation out;
    out.reserve(in.size());
    folded = false;

    // Append `text` to the trailing assistant turn, opening one when the last
    // message is not an assistant turn — which happens for a tool result whose
    // call was rewound away, and for a file that begins mid-exchange.
    auto append_assistant = [&out](const std::string& text) {
        if (!out.empty() && out.back().role() == Role::Assistant) {
            std::string merged = out.back().content();
            if (!merged.empty()) merged += "\n\n";
            merged += text;
            out.back() =
                Message::assistant(std::move(merged), {}, out.back().reasoning());
            return;
        }
        out.push_back(Message::assistant(text));
    };

    for (const Message& m : in) {
        if (m.role() == Role::System || m.role() == Role::User) {
            out.push_back(m);
            continue;
        }
        if (m.role() == Role::Tool) {
            folded = true;
            append_assistant(result_block(m.content()));
            continue;
        }
        // Assistant: keep what it said, and render what it asked for.
        std::string text = m.content();
        for (const ToolCall& c : m.tool_calls()) {
            folded = true;
            if (!text.empty()) text += "\n\n";
            text += call_line(c);
        }
        if (text.empty()) continue;  // said nothing and called nothing
        out.push_back(Message::assistant(std::move(text), {}, m.reasoning()));
    }
    return out;
}

}  // namespace moocode::gui
