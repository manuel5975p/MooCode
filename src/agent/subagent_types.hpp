#ifndef MOOCODE_SUBAGENT_TYPES_HPP
#define MOOCODE_SUBAGENT_TYPES_HPP

// Lightweight types shared between the sub-agent tool and the TUI callback
// wiring. Extracted from subagent_tool.hpp so tui.hpp can declare its
// shared_ptr<SubagentActivityFn> / shared_ptr<SubagentTextFn> parameters
// without pulling in the full subagent tool infrastructure (SubagentConfig,
// spawn_subagent_tool, etc.).

#include <functional>
#include <string>

namespace moocode {

enum class SubagentActivityStatus { Running, Ok, Failed };

// Called for each subagent tool call start/result. Fires on the worker thread.
// args/preview are passed raw (full, unformatted); the consumer owns any
// collapsing/capping for display.
//   id      - tool_call_id, for matching Running→Ok/Failed
//   name    - tool name (empty for result callbacks)
//   args    - raw arguments JSON (empty for results)
//   status  - Running (tool invoked), Ok/Failed (result arrived)
//   preview - raw result content (empty for Running entries)
using SubagentActivityFn = std::function<void(
    std::string id, std::string name, std::string args,
    SubagentActivityStatus status, std::string preview)>;

// Called for each assistant text response the sub-agent emits (between tool
// calls or the final answer). `text` is raw (full, uncapped at this point);
// the TUI owns formatting, sanitization, and the byte cap.  Fires before the
// tool-call callbacks for the same Message so the turn is created first.
using SubagentTextFn = std::function<void(std::string text)>;

}  // namespace moocode

#endif  // MOOCODE_SUBAGENT_TYPES_HPP
