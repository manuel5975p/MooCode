#include "agent/stream.hpp"

#include <algorithm>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <utility>

#include "agent/json_util.hpp"  // get_string_or
#include "agent/stream_detail.hpp"  // ThinkSplitter
#include "agent/types.hpp"  // ToolCall

namespace moocode {

namespace {

constexpr std::string_view kOpen = "<think>";
constexpr std::string_view kClose = "</think>";

// Upper bound on streamed tool-call / content-block slot indices. A malformed
// SSE event with a huge "index" would otherwise force a multi-GB resize; no
// real provider emits more than a handful of concurrent slots.
constexpr std::size_t kMaxStreamSlots = 256;

// Largest k in [0, tag.size()) such that the last k bytes of `s` equal the
// first k bytes of `tag` — i.e. how much of a trailing partial tag to hold back
// until more bytes arrive. 0 when no suffix of `s` could begin `tag`.
std::size_t trailing_partial_tag(std::string_view s, std::string_view tag) {
    for (std::size_t k = std::min(s.size(), tag.size() - 1); k > 0; --k)
        if (s.substr(s.size() - k) == tag.substr(0, k)) return k;
    return 0;
}

// Read an integer usage field into dst when present and integer-typed, clamped
// into [0, INT_MAX] (get<int>() on an out-of-range value is UB in nlohmann).
void usage_get(const nlohmann::json& u, const char* key, int& dst) {
    if (auto it = u.find(key); it != u.end() && it->is_number_integer()) {
        std::int64_t v = it->get<std::int64_t>();
        dst = static_cast<int>(std::clamp<std::int64_t>(v, 0, INT_MAX));
    }
}

}  // namespace

Usage parse_usage(const nlohmann::json& u) {
    Usage out;
    if (!u.is_object()) return out;
    usage_get(u, "prompt_tokens", out.prompt_tokens);
    usage_get(u, "completion_tokens", out.completion_tokens);
    usage_get(u, "total_tokens", out.total_tokens);
    out.present = true;
    return out;
}

Usage parse_anthropic_usage(const nlohmann::json& u) {
    Usage out;
    if (!u.is_object()) return out;
    usage_get(u, "input_tokens", out.prompt_tokens);
    usage_get(u, "output_tokens", out.completion_tokens);
    out.total_tokens = std::max(0, out.prompt_tokens + out.completion_tokens);
    out.present = true;
    return out;
}

Usage parse_gemini_usage(const nlohmann::json& u) {
    Usage out;
    if (!u.is_object()) return out;
    usage_get(u, "promptTokenCount", out.prompt_tokens);
    int candidates = 0, thoughts = 0;
    usage_get(u, "candidatesTokenCount", candidates);
    usage_get(u, "thoughtsTokenCount", thoughts);
    out.completion_tokens = candidates + thoughts;  // thinking billed as output
    if (u.contains("totalTokenCount"))
        usage_get(u, "totalTokenCount", out.total_tokens);
    else
        out.total_tokens = out.prompt_tokens + out.completion_tokens;
    out.present = true;
    return out;
}

ThinkSplitter::Parts ThinkSplitter::feed(std::string_view fragment) {
    Parts out;
    std::string buf = pending_;
    buf.append(fragment);
    pending_.clear();

    std::size_t pos = 0;
    for (;;) {
        std::string_view tag = in_think_ ? kClose : kOpen;
        std::string& sink = in_think_ ? out.reasoning : out.answer;
        std::size_t idx = buf.find(tag, pos);
        if (idx != std::string::npos) {
            sink.append(buf, pos, idx - pos);
            pos = idx + tag.size();
            in_think_ = !in_think_;
            continue;
        }
        // No complete tag remains; emit everything except a trailing run that
        // could still grow into `tag` on the next feed().
        std::string_view rest(buf.data() + pos, buf.size() - pos);
        std::size_t hold = trailing_partial_tag(rest, tag);
        sink.append(buf, pos, rest.size() - hold);
        pending_.assign(buf, buf.size() - hold, hold);
        break;
    }
    return out;
}

ThinkSplitter::Parts ThinkSplitter::flush() {
    Parts out;
    if (!pending_.empty()) {
        (in_think_ ? out.reasoning : out.answer) = std::move(pending_);
        pending_.clear();
    }
    return out;
}

void parse_sse_chunk(std::string& buffer, std::vector<std::string>& out,
                     bool& done) {
    std::size_t start = 0;
    for (std::size_t nl; (nl = buffer.find('\n', start)) != std::string::npos;) {
        std::string_view line(buffer.data() + start, nl - start);
        start = nl + 1;
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        if (line.empty() || line.front() == ':') continue;  // separator/comment
        if (line.substr(0, 5) != "data:") continue;          // ignore event:/id:
        std::string_view payload = line.substr(5);
        if (!payload.empty() && payload.front() == ' ') payload.remove_prefix(1);
        if (payload == "[DONE]") {
            done = true;
            continue;
        }
        out.emplace_back(payload);
    }
    buffer.erase(0, start);
}

StreamAccumulator::StreamAccumulator()
    : splitter_(std::make_unique<ThinkSplitter>()) {}

StreamAccumulator::~StreamAccumulator() = default;

StreamAccumulator::Added StreamAccumulator::ingest(const nlohmann::json& chunk) {
    Added added;
    if (!chunk.is_object()) return added;
    if (auto u = chunk.find("usage"); u != chunk.end() && u->is_object())
        usage_ = parse_usage(*u);
    auto choices = chunk.find("choices");
    if (choices == chunk.end() || !choices->is_array() || choices->empty())
        return added;
    const nlohmann::json& choice = (*choices)[0];
    if (!choice.is_object()) return added;

    if (auto fr = choice.find("finish_reason");
        fr != choice.end() && fr->is_string())
        finish_reason_ = fr->get<std::string>();

    auto d = choice.find("delta");
    if (d == choice.end() || !d->is_object()) return added;
    const nlohmann::json& delta = *d;

    // reasoning_content: a dedicated field some reasoning models use. Display
    // only — never folded into Turn.text (matching the non-streaming parse).
    if (auto it = delta.find("reasoning_content");
        it != delta.end() && it->is_string()) {
        std::string r = it->get<std::string>();
        reasoning_ += r;
        added.reasoning += std::move(r);
    }

    // content: kept verbatim for Turn.text, and split into answer/reasoning for
    // live display (in case the model emits inline <think> tags).
    if (auto it = delta.find("content"); it != delta.end() && it->is_string()) {
        std::string c = it->get<std::string>();
        content_ += c;
        ThinkSplitter::Parts p = splitter_->feed(c);
        added.answer += p.answer;
        reasoning_ += p.reasoning;
        added.reasoning += p.reasoning;
    }

    // tool_calls: fragments arrive across chunks, keyed by "index"; name comes
    // once, arguments accrete a piece at a time.
    if (auto it = delta.find("tool_calls"); it != delta.end() && it->is_array()) {
        for (const auto& tc : *it) {
            if (!tc.is_object()) continue;
            std::size_t index = 0;
            if (auto ix = tc.find("index");
                ix != tc.end() && ix->is_number_integer()) {
                std::int64_t v = ix->get<std::int64_t>();
                if (v > 0) index = static_cast<std::size_t>(v);
            }
            if (index >= kMaxStreamSlots) continue;  // reject bogus slot
            if (index >= calls_.size()) calls_.resize(index + 1);
            PartialCall& call = calls_[index];
            call.seen = true;
            if (auto id = tc.find("id"); id != tc.end() && id->is_string())
                call.id = id->get<std::string>();
            if (auto fn = tc.find("function");
                fn != tc.end() && fn->is_object()) {
                if (auto nm = fn->find("name"); nm != fn->end() && nm->is_string())
                    call.name = nm->get<std::string>();
                if (auto ar = fn->find("arguments");
                    ar != fn->end() && ar->is_string())
                    call.args += ar->get<std::string>();
            }
        }
    }
    return added;
}

Turn StreamAccumulator::finish() {
    Turn turn;
    turn.text = content_;
    turn.reasoning = reasoning_;
    turn.finish_reason = finish_reason_;
    for (auto& c : calls_) {
        if (!c.seen) continue;
        turn.tool_calls.push_back(
            ToolCall{.id = std::move(c.id), .name = std::move(c.name), .arguments_json = std::move(c.args)});
    }
    turn.usage = usage_;
    return turn;
}

// --- AnthropicStreamAccumulator ---------------------------------------------

namespace {

// Read a non-negative content-block "index" from an event, defaulting to 0.
std::size_t event_index(const nlohmann::json& event) {
    if (auto it = event.find("index");
        it != event.end() && it->is_number_integer()) {
        std::int64_t v = it->get<std::int64_t>();
        if (v > 0) return static_cast<std::size_t>(v);
    }
    return 0;
}

}  // namespace

AnthropicStreamAccumulator::Block& AnthropicStreamAccumulator::block_at(
    std::size_t i) {
    i = std::min(i, kMaxStreamSlots - 1);  // clamp a bogus huge index
    if (i >= blocks_.size()) blocks_.resize(i + 1);
    return blocks_[i];
}

AnthropicStreamAccumulator::Added AnthropicStreamAccumulator::ingest(
    const nlohmann::json& event) {
    Added added;
    if (!event.is_object()) return added;
    const std::string type = json::get_string_or(event, "type");

    if (type == "message_start") {
        if (auto m = event.find("message"); m != event.end() && m->is_object())
            if (auto u = m->find("usage"); u != m->end() && u->is_object()) {
                Usage parsed = parse_anthropic_usage(*u);
                input_tokens_ = parsed.prompt_tokens;
                output_tokens_ = parsed.completion_tokens;
                usage_seen_ = true;
            }
        return added;
    }
    if (type == "content_block_start") {
        Block& b = block_at(event_index(event));
        if (auto cb = event.find("content_block");
            cb != event.end() && cb->is_object()) {
            if (json::get_string_or(*cb, "type") == "tool_use") {
                b.is_tool = true;
                b.id = json::get_string_or(*cb, "id");
                b.name = json::get_string_or(*cb, "name");
                // `input` here is the empty placeholder {}; the real arguments
                // stream in as input_json_delta fragments, so ignore it.
            }
        }
        return added;
    }
    if (type == "content_block_delta") {
        auto d = event.find("delta");
        if (d == event.end() || !d->is_object()) return added;
        const std::string dt = json::get_string_or(*d, "type");
        if (dt == "text_delta") {
            std::string t = json::get_string_or(*d, "text");
            content_ += t;
            added.answer += t;
        } else if (dt == "input_json_delta") {
            block_at(event_index(event)).args += json::get_string_or(*d, "partial_json");
        } else if (dt == "thinking_delta") {
            std::string r = json::get_string_or(*d, "thinking");
            reasoning_ += r;
            added.reasoning += std::move(r);
        }
        return added;
    }
    if (type == "message_delta") {
        if (auto d = event.find("delta"); d != event.end() && d->is_object()) {
            std::string sr = json::get_string_or(*d, "stop_reason");
            if (!sr.empty()) finish_reason_ = std::move(sr);
        }
        if (auto u = event.find("usage"); u != event.end() && u->is_object()) {
            // message_delta reports the final cumulative output token count.
            if (auto ot = u->find("output_tokens");
                ot != u->end() && ot->is_number_integer()) {
                output_tokens_ = ot->get<int>();
                usage_seen_ = true;
            }
        }
        return added;
    }
    if (type == "error") {
        if (auto e = event.find("error"); e != event.end() && e->is_object())
            error_ = json::get_string_or(*e, "message");
        if (!error_) error_ = std::string("anthropic stream error");
        return added;
    }
    // ping / content_block_stop / message_stop / unknown: nothing to do.
    return added;
}

Turn AnthropicStreamAccumulator::finish() {
    Turn turn;
    turn.text = content_;
    turn.reasoning = reasoning_;
    turn.finish_reason = finish_reason_;
    for (auto& b : blocks_) {
        if (!b.is_tool) continue;
        turn.tool_calls.push_back(ToolCall{.id = std::move(b.id),
            .name = std::move(b.name),
            .arguments_json = b.args.empty() ? "{}"
                                                          : std::move(b.args)});
    }
    if (usage_seen_) {
        turn.usage.prompt_tokens = input_tokens_;
        turn.usage.completion_tokens = output_tokens_;
        turn.usage.total_tokens = input_tokens_ + output_tokens_;
        turn.usage.present = true;
    }
    return turn;
}

// --- GeminiStreamAccumulator ------------------------------------------------

namespace {

// True when a Gemini part carries a truthy "thought" flag (reasoning summary).
bool part_is_thought(const nlohmann::json& part) {
    auto it = part.find("thought");
    return it != part.end() && it->is_boolean() && it->get<bool>();
}

}  // namespace

GeminiStreamAccumulator::Added GeminiStreamAccumulator::ingest(
    const nlohmann::json& chunk) {
    Added added;
    if (!chunk.is_object()) return added;

    if (auto u = chunk.find("usageMetadata");
        u != chunk.end() && u->is_object())
        usage_ = parse_gemini_usage(*u);

    auto cands = chunk.find("candidates");
    if (cands == chunk.end() || !cands->is_array() || cands->empty())
        return added;
    const nlohmann::json& cand = (*cands)[0];
    if (!cand.is_object()) return added;

    if (auto fr = cand.find("finishReason");
        fr != cand.end() && fr->is_string() && !fr->get<std::string>().empty())
        finish_reason_ = fr->get<std::string>();

    auto content = cand.find("content");
    if (content == cand.end() || !content->is_object()) return added;
    auto parts = content->find("parts");
    if (parts == content->end() || !parts->is_array()) return added;

    for (const auto& part : *parts) {
        if (!part.is_object()) continue;
        if (auto fc = part.find("functionCall");
            fc != part.end() && fc->is_object()) {
            PartialCall call;
            call.id = json::get_string_or(*fc, "id");
            call.name = json::get_string_or(*fc, "name");
            if (auto a = fc->find("args"); a != fc->end())
                call.args = a->dump();
            else
                call.args = "{}";
            // thoughtSignature sits on the part, beside functionCall.
            call.signature = json::get_string_or(part, "thoughtSignature");
            calls_.push_back(std::move(call));
            continue;
        }
        if (auto t = part.find("text"); t != part.end() && t->is_string()) {
            std::string text = t->get<std::string>();
            if (part_is_thought(part)) {
                reasoning_ += text;
                added.reasoning += text;
            } else {
                content_ += text;
                added.answer += text;
            }
        }
    }
    return added;
}

Turn GeminiStreamAccumulator::finish() {
    Turn turn;
    turn.text = content_;
    turn.reasoning = reasoning_;
    turn.finish_reason = finish_reason_;
    for (auto& c : calls_)
        turn.tool_calls.push_back(ToolCall{.id = std::move(c.id),
            .name = std::move(c.name),
            .arguments_json = c.args.empty() ? "{}" : std::move(c.args),
            .signature = std::move(c.signature)});
    turn.usage = usage_;
    return turn;
}

}  // namespace moocode
