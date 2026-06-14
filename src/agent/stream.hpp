#ifndef MOOCODE_STREAM_HPP
#define MOOCODE_STREAM_HPP

// Pure, network-free building blocks for streaming chat completions. The
// transport (http) feeds raw bytes in; these turn them into displayable
// answer/reasoning fragments and a final assembled Turn. Kept free of I/O so
// the fiddly bits (SSE framing, tags split across packets, fragmented
// tool-call arguments) are exhaustively unit-testable.

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "agent/provider.hpp"  // Turn

namespace moocode {

class ThinkSplitter;  // defined in stream_detail.hpp (included by stream.cpp only)

// Pull complete Server-Sent-Events "data:" payloads out of a growing buffer.
// Each payload's JSON text is appended to `out`; `done` is set if the OpenAI
// "[DONE]" sentinel was seen. Any incomplete trailing line is left in `buffer`
// for the next call. Non-data lines (comments, blank keep-alives, "event:")
// are ignored. pre: none. post: buffer holds only an unterminated remainder.
void parse_sse_chunk(std::string& buffer, std::vector<std::string>& out,
                     bool& done);

// Read an OpenAI `usage` object into a Usage. A non-object input returns a
// not-present Usage. pre: none. post: result.present iff `u` was an object.
Usage parse_usage(const nlohmann::json& u);

// Read an Anthropic usage object ({"input_tokens":N,"output_tokens":M}) into a
// Usage, mapping prompt_tokens<-input_tokens, completion_tokens<-output_tokens,
// and total_tokens to their sum. A non-object input returns a not-present Usage.
// pre: none. post: result.present iff `u` was an object.
Usage parse_anthropic_usage(const nlohmann::json& u);

// Read a Gemini usageMetadata object into a Usage, mapping
// prompt_tokens<-promptTokenCount, completion_tokens<-candidatesTokenCount +
// thoughtsTokenCount (thinking is billed as output), total_tokens<-
// totalTokenCount (falling back to their sum when absent). A non-object input
// returns a not-present Usage. pre: none. post: result.present iff `u` object.
Usage parse_gemini_usage(const nlohmann::json& u);

// Assembles streamed chat-completion delta chunks into a final Turn, exposing
// the answer/reasoning text contributed by each chunk for live display.
class StreamAccumulator {
public:
    StreamAccumulator();
    ~StreamAccumulator();

    struct Added {
        std::string answer;     // delta.content outside <think>
        std::string reasoning;  // delta.reasoning_content + content inside <think>
    };

    // Ingest one chunk object, i.e. {"choices":[{"delta":{...},
    // "finish_reason":...}]}. Tolerates empty/usage-only chunks. post: returns
    // the answer/reasoning text this chunk contributed (either may be empty).
    Added ingest(const nlohmann::json& chunk);

    // The fully assembled turn. Call once the stream is complete. text is the
    // raw concatenated delta.content (tags included, matching the non-streaming
    // parse); tool_calls are ordered by their streamed index.
    Turn finish();

private:
    struct PartialCall {
        std::string id;
        std::string name;
        std::string args;
        bool seen = false;
    };

    std::unique_ptr<ThinkSplitter> splitter_;
    std::string content_;        // raw delta.content concatenation => Turn.text
    std::string reasoning_;      // reasoning_content + <think> bytes => Turn.reasoning
    std::string finish_reason_;
    std::vector<PartialCall> calls_;  // indexed by tool_call delta "index"
    Usage usage_;  // captured from a trailing include_usage chunk, if any
};

// Assembles Anthropic Messages-API streaming events into a final Turn. Unlike
// the OpenAI stream (one delta object per chunk), Anthropic emits a typed event
// sequence — message_start, content_block_start/delta/stop (one per content
// block: text, tool_use, or thinking), message_delta, message_stop — so this
// accumulator is keyed off each event's "type". Text deltas feed Turn.text and
// the answer sink; thinking deltas feed the reasoning sink (display-only, never
// folded into Turn.text); tool_use blocks accrete their arguments from
// input_json_delta fragments. Pure: ingest one already-parsed event object.
class AnthropicStreamAccumulator {
public:
    struct Added {
        std::string answer;     // text_delta bytes
        std::string reasoning;  // thinking_delta bytes
    };

    // Ingest one event object (e.g. {"type":"content_block_delta",...}). Unknown
    // or housekeeping events (ping, content_block_stop, message_stop) are no-ops.
    // An "error" event records its message (see error()). post: returns the
    // answer/reasoning text this event contributed (either may be empty).
    Added ingest(const nlohmann::json& event);

    // The fully assembled turn. Call once the stream is complete. text is the
    // concatenated text-block content (no thinking); tool_calls are ordered by
    // their content-block index, each carrying the assembled input JSON.
    Turn finish();

    // The message from an Anthropic "error" event, if one was seen (the stream
    // may carry an error object under a 200 status). nullopt otherwise.
    const std::optional<std::string>& error() const { return error_; }

private:
    // One streamed content block. Text blocks accumulate into content_; only
    // tool_use blocks are retained here, accreting their argument JSON.
    struct Block {
        bool is_tool = false;
        std::string id;
        std::string name;
        std::string args;  // assembled from input_json_delta fragments
    };

    // Resize blocks_ so index `i` is addressable.
    Block& block_at(std::size_t i);

    std::vector<Block> blocks_;  // indexed by content-block "index"
    std::string content_;        // concatenated text_delta => Turn.text
    std::string reasoning_;      // concatenated thinking_delta => Turn.reasoning
    std::string finish_reason_;  // last stop_reason seen (from message_delta)
    int input_tokens_ = 0;       // from message_start
    int output_tokens_ = 0;      // from message_delta (cumulative final)
    bool usage_seen_ = false;
    std::optional<std::string> error_;
};

// Assembles Gemini :streamGenerateContent SSE chunks into a final Turn. Each
// `data:` payload is a full GenerateContentResponse; this folds the incremental
// text/thought parts and any functionCall parts together across chunks. Text
// parts (no truthy "thought" flag) feed Turn.text + the answer sink; thought
// parts feed the reasoning sink only (display-only, never in Turn.text);
// functionCall parts arrive whole and accrete as tool calls. The last non-empty
// finishReason and the last usageMetadata win. Pure: ingest one parsed chunk.
class GeminiStreamAccumulator {
public:
    struct Added {
        std::string answer;     // non-thought text bytes
        std::string reasoning;  // thought text bytes
    };

    // Ingest one GenerateContentResponse object. Tolerates candidate-less /
    // empty-parts / usage-only chunks. post: returns the answer/reasoning text
    // this chunk contributed (either may be empty).
    Added ingest(const nlohmann::json& chunk);

    // The fully assembled turn. Call once the stream is complete. text is the
    // concatenated non-thought text; tool_calls are in arrival order, each
    // carrying its serialised argument JSON.
    Turn finish();

private:
    struct PartialCall {
        std::string id;
        std::string name;
        std::string args;       // serialised functionCall.args object
        std::string signature;  // part-level thoughtSignature (Gemini 3)
    };

    std::string content_;        // concatenated non-thought text => Turn.text
    std::string reasoning_;      // concatenated thought text => Turn.reasoning
    std::string finish_reason_;  // last non-empty finishReason
    std::vector<PartialCall> calls_;
    Usage usage_;                // last usageMetadata seen
};

}  // namespace moocode

#endif  // MOOCODE_STREAM_HPP
