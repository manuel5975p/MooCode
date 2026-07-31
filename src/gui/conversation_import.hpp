#ifndef MOOCODE_GUI_CONVERSATION_IMPORT_HPP
#define MOOCODE_GUI_CONVERSATION_IMPORT_HPP

// Turning a saved conversation into one the GUI can send.
//
// Both frontends write the same conversations/*.toml, so the GUI's Chats menu
// can open a conversation the TUI saved — but only the TUI's agent has tools.
// A conversation from it carries assistant turns with tool calls and the tool
// results that answered them, and this frontend advertises no tools at all (see
// agent_bridge.hpp): a request whose history contains tool traffic the request
// never declared is rejected outright by some backends.
//
// So the traffic is folded into the assistant turn that caused it, as plain
// Markdown. Kept rather than dropped, so a resumed conversation still knows
// what was done; clipped, because a resumed chat needs the gist and not the
// forty kilobytes some read_file returned.
//
// Qt-free on purpose, like moogui_theme: this is where the interesting rules
// live, so it should be testable without a display.

#include <cstddef>
#include <string>

#include "agent/types.hpp"  // Conversation

namespace moocode::gui {

// How much of one tool result survives the fold, in characters.
inline constexpr std::size_t kToolResultClip = 1200;

// `in` with every tool call and tool result folded into assistant prose.
// System and user messages pass through untouched; an assistant turn that said
// nothing but a tool call keeps only the rendered call. `folded` is set to true
// when anything was rewritten, and false when `in` was already chat-shaped —
// the caller uses that to decide whether continuing the conversation may
// overwrite the file it came from.
Conversation to_chat_only(const Conversation& in, bool& folded);

}  // namespace moocode::gui

#endif  // MOOCODE_GUI_CONVERSATION_IMPORT_HPP
