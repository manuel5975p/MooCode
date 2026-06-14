#include "agent/stream.hpp"

#include <string>
#include <vector>

#include "agent/json_util.hpp"
#include "agent/stream_detail.hpp"  // ThinkSplitter (white-box tests)
#include "test_harness.hpp"

using namespace moocode;

// --- ThinkSplitter ----------------------------------------------------------

TEST("ThinkSplitter: plain text is all answer") {
    ThinkSplitter s;
    auto p = s.feed("hello world");
    CHECK_EQ(p.answer, std::string("hello world"));
    CHECK(p.reasoning.empty());
    CHECK(!s.in_think());
}

TEST("ThinkSplitter: a whole think block splits answer from reasoning") {
    ThinkSplitter s;
    auto p = s.feed("before<think>plan</think>after");
    CHECK_EQ(p.answer, std::string("beforeafter"));
    CHECK_EQ(p.reasoning, std::string("plan"));
    CHECK(!s.in_think());
}

TEST("ThinkSplitter: open tag split across feeds") {
    ThinkSplitter s;
    auto a = s.feed("hi<thi");
    CHECK_EQ(a.answer, std::string("hi"));  // partial tag held back
    CHECK(a.reasoning.empty());
    auto b = s.feed("nk>secret");
    CHECK(b.answer.empty());
    CHECK_EQ(b.reasoning, std::string("secret"));
    CHECK(s.in_think());
}

TEST("ThinkSplitter: close tag split across feeds") {
    ThinkSplitter s;
    s.feed("<think>rea");
    auto b = s.feed("son</thi");
    CHECK_EQ(b.reasoning, std::string("son"));  // partial close held back
    auto c = s.feed("nk>done");
    CHECK_EQ(c.answer, std::string("done"));
    CHECK(!s.in_think());
}

TEST("ThinkSplitter: a tag-like prefix that is not <think> is answer") {
    ThinkSplitter s;
    auto p = s.feed("a<thing>b");
    CHECK_EQ(p.answer, std::string("a<thing>b"));
    CHECK(p.reasoning.empty());
}

TEST("ThinkSplitter: reasoning accumulates across feeds while inside") {
    ThinkSplitter s;
    s.feed("<think>one ");
    auto b = s.feed("two ");
    auto c = s.feed("three</think>");
    CHECK_EQ(b.reasoning, std::string("two "));
    CHECK_EQ(c.reasoning, std::string("three"));
    CHECK(!s.in_think());
}

TEST("ThinkSplitter: flush releases a dangling partial tag as text") {
    ThinkSplitter s;
    auto a = s.feed("answer<thi");
    CHECK_EQ(a.answer, std::string("answer"));
    auto f = s.flush();
    CHECK_EQ(f.answer, std::string("<thi"));  // not a real tag after all
}

// --- parse_sse_chunk --------------------------------------------------------

TEST("parse_sse_chunk: one complete data line") {
    std::string buf = "data: {\"a\":1}\n\n";
    std::vector<std::string> out;
    bool done = false;
    parse_sse_chunk(buf, out, done);
    CHECK_EQ(out.size(), size_t{1});
    if (out.size() == 1) CHECK_EQ(out[0], std::string("{\"a\":1}"));
    CHECK(!done);
    CHECK(buf.empty());
}

TEST("parse_sse_chunk: two events in one chunk") {
    std::string buf = "data: {\"n\":1}\n\ndata: {\"n\":2}\n\n";
    std::vector<std::string> out;
    bool done = false;
    parse_sse_chunk(buf, out, done);
    CHECK_EQ(out.size(), size_t{2});
    if (out.size() == 2) {
        CHECK_EQ(out[0], std::string("{\"n\":1}"));
        CHECK_EQ(out[1], std::string("{\"n\":2}"));
    }
}

TEST("parse_sse_chunk: event split across two calls") {
    std::vector<std::string> out;
    bool done = false;
    std::string buf = "data: {\"par";
    parse_sse_chunk(buf, out, done);
    CHECK(out.empty());  // nothing complete yet
    buf += "tial\":true}\n\n";
    parse_sse_chunk(buf, out, done);
    CHECK_EQ(out.size(), size_t{1});
    if (out.size() == 1) CHECK_EQ(out[0], std::string("{\"partial\":true}"));
}

TEST("parse_sse_chunk: [DONE] sets done and is not emitted") {
    std::string buf = "data: {\"x\":1}\n\ndata: [DONE]\n\n";
    std::vector<std::string> out;
    bool done = false;
    parse_sse_chunk(buf, out, done);
    CHECK(done);
    CHECK_EQ(out.size(), size_t{1});
}

TEST("parse_sse_chunk: comments and blank keep-alives are ignored") {
    std::string buf = ": keep-alive\n\ndata: {\"y\":2}\n\n";
    std::vector<std::string> out;
    bool done = false;
    parse_sse_chunk(buf, out, done);
    CHECK_EQ(out.size(), size_t{1});
    if (out.size() == 1) CHECK_EQ(out[0], std::string("{\"y\":2}"));
}

TEST("parse_sse_chunk: tolerates data: with no space") {
    std::string buf = "data:{\"z\":3}\n\n";
    std::vector<std::string> out;
    bool done = false;
    parse_sse_chunk(buf, out, done);
    CHECK_EQ(out.size(), size_t{1});
    if (out.size() == 1) CHECK_EQ(out[0], std::string("{\"z\":3}"));
}

// --- StreamAccumulator ------------------------------------------------------

namespace {
nlohmann::json chunk(const std::string& s) { return json::parse(s).value(); }
}  // namespace

TEST("StreamAccumulator: content deltas concatenate into the final text") {
    StreamAccumulator acc;
    auto a = acc.ingest(chunk(R"({"choices":[{"delta":{"content":"Hel"}}]})"));
    auto b = acc.ingest(chunk(R"({"choices":[{"delta":{"content":"lo"}}]})"));
    CHECK_EQ(a.answer, std::string("Hel"));
    CHECK_EQ(b.answer, std::string("lo"));
    Turn t = acc.finish();
    CHECK_EQ(t.text, std::string("Hello"));
    CHECK(t.tool_calls.empty());
}

TEST("StreamAccumulator: reasoning_content is reasoning, not in the text") {
    StreamAccumulator acc;
    auto a = acc.ingest(chunk(R"({"choices":[{"delta":{"reasoning_content":"why"}}]})"));
    acc.ingest(chunk(R"({"choices":[{"delta":{"content":"answer"}}]})"));
    CHECK_EQ(a.reasoning, std::string("why"));
    CHECK(a.answer.empty());
    Turn t = acc.finish();
    CHECK_EQ(t.text, std::string("answer"));
    CHECK_EQ(t.reasoning, std::string("why"));  // persisted on the Turn
}

TEST("StreamAccumulator: inline think tags route to reasoning") {
    StreamAccumulator acc;
    auto a = acc.ingest(chunk(R"({"choices":[{"delta":{"content":"<think>plan</think>hi"}}]})"));
    CHECK_EQ(a.reasoning, std::string("plan"));
    CHECK_EQ(a.answer, std::string("hi"));
    // Raw content (tags included) is preserved for history parity.
    Turn t = acc.finish();
    CHECK_EQ(t.text, std::string("<think>plan</think>hi"));
    CHECK_EQ(t.reasoning, std::string("plan"));
}

TEST("AnthropicStreamAccumulator: thinking_delta accrues to reasoning, not text") {
    AnthropicStreamAccumulator acc;
    auto a = acc.ingest(chunk(R"({"type":"content_block_delta","index":0,
        "delta":{"type":"thinking_delta","thinking":"step "}})"));
    acc.ingest(chunk(R"({"type":"content_block_delta","index":0,
        "delta":{"type":"thinking_delta","thinking":"two"}})"));
    acc.ingest(chunk(R"({"type":"content_block_delta","index":1,
        "delta":{"type":"text_delta","text":"answer"}})"));
    CHECK_EQ(a.reasoning, std::string("step "));
    CHECK(a.answer.empty());
    Turn t = acc.finish();
    CHECK_EQ(t.text, std::string("answer"));
    CHECK_EQ(t.reasoning, std::string("step two"));
}

TEST("GeminiStreamAccumulator: thought parts accrue to reasoning, not text") {
    GeminiStreamAccumulator acc;
    auto a = acc.ingest(chunk(R"({"candidates":[{"content":{"parts":[
        {"text":"because ","thought":true}]}}]})"));
    acc.ingest(chunk(R"({"candidates":[{"content":{"parts":[
        {"text":"so","thought":true}]}}]})"));
    acc.ingest(chunk(R"({"candidates":[{"content":{"parts":[
        {"text":"final"}]}}]})"));
    CHECK_EQ(a.reasoning, std::string("because "));
    CHECK(a.answer.empty());
    Turn t = acc.finish();
    CHECK_EQ(t.text, std::string("final"));
    CHECK_EQ(t.reasoning, std::string("because so"));
}

TEST("StreamAccumulator: finish_reason is captured") {
    StreamAccumulator acc;
    acc.ingest(chunk(R"({"choices":[{"delta":{"content":"x"},"finish_reason":null}]})"));
    acc.ingest(chunk(R"({"choices":[{"delta":{},"finish_reason":"stop"}]})"));
    Turn t = acc.finish();
    CHECK_EQ(t.finish_reason, std::string("stop"));
}

TEST("StreamAccumulator: a tool call assembled from fragments") {
    StreamAccumulator acc;
    acc.ingest(chunk(R"({"choices":[{"delta":{"tool_calls":[
        {"index":0,"id":"call_1","type":"function",
         "function":{"name":"read_file","arguments":""}}]}}]})"));
    acc.ingest(chunk(R"({"choices":[{"delta":{"tool_calls":[
        {"index":0,"function":{"arguments":"{\"pa"}}]}}]})"));
    acc.ingest(chunk(R"({"choices":[{"delta":{"tool_calls":[
        {"index":0,"function":{"arguments":"th\":\"a\"}"}}]}}]})"));
    Turn t = acc.finish();
    CHECK_EQ(t.tool_calls.size(), size_t{1});
    if (t.tool_calls.size() == 1) {
        CHECK_EQ(t.tool_calls[0].id, std::string("call_1"));
        CHECK_EQ(t.tool_calls[0].name, std::string("read_file"));
        CHECK_EQ(t.tool_calls[0].arguments_json, std::string(R"({"path":"a"})"));
    }
}

TEST("StreamAccumulator: two tool calls kept in index order") {
    StreamAccumulator acc;
    acc.ingest(chunk(R"({"choices":[{"delta":{"tool_calls":[
        {"index":0,"id":"c0","function":{"name":"a","arguments":"{}"}}]}}]})"));
    acc.ingest(chunk(R"({"choices":[{"delta":{"tool_calls":[
        {"index":1,"id":"c1","function":{"name":"b","arguments":"{}"}}]}}]})"));
    Turn t = acc.finish();
    CHECK_EQ(t.tool_calls.size(), size_t{2});
    if (t.tool_calls.size() == 2) {
        CHECK_EQ(t.tool_calls[0].name, std::string("a"));
        CHECK_EQ(t.tool_calls[1].name, std::string("b"));
    }
}

TEST("StreamAccumulator: empty/usage-only chunk is a no-op") {
    StreamAccumulator acc;
    auto a = acc.ingest(chunk(R"({"choices":[]})"));
    CHECK(a.answer.empty());
    CHECK(a.reasoning.empty());
    Turn t = acc.finish();
    CHECK(t.text.empty());
    CHECK(t.tool_calls.empty());
}

TEST("StreamAccumulator: a bogus huge tool-call index is ignored, no OOM") {
    StreamAccumulator acc;
    acc.ingest(chunk(R"({"choices":[{"delta":{"tool_calls":[
        {"index":100000000,"id":"x","function":{"name":"f","arguments":"{}"}}]}}]})"));
    Turn t = acc.finish();
    CHECK(t.tool_calls.empty());  // slot rejected by the cap
}

TEST("AnthropicStreamAccumulator: a huge content_block index does not OOM") {
    AnthropicStreamAccumulator acc;
    acc.ingest(chunk(R"({"type":"content_block_start","index":100000000,
        "content_block":{"type":"text","text":""}})"));
    acc.ingest(chunk(R"({"type":"content_block_delta","index":100000000,
        "delta":{"type":"text_delta","text":"hi"}})"));
    Turn t = acc.finish();  // clamped slot; must return without OOM
    CHECK(t.tool_calls.empty());
}

// --- parse_usage ------------------------------------------------------------

TEST("parse_usage: reads token fields and marks present") {
    auto u = nlohmann::json::parse(
        R"({"prompt_tokens":10,"completion_tokens":5,"total_tokens":15})");
    Usage out = parse_usage(u);
    CHECK(out.present);
    CHECK_EQ(out.prompt_tokens, 10);
    CHECK_EQ(out.completion_tokens, 5);
    CHECK_EQ(out.total_tokens, 15);
}

TEST("parse_usage: a non-object yields not-present") {
    Usage out = parse_usage(nlohmann::json("nope"));
    CHECK(!out.present);
}

TEST("parse_usage: an out-of-int-range field is clamped, not UB/negative") {
    auto u = nlohmann::json::parse(R"({"total_tokens":9999999999})");
    Usage out = parse_usage(u);
    CHECK(out.present);
    CHECK(out.total_tokens >= 0);  // clamped to INT_MAX, never negative
}

// --- StreamAccumulator usage ------------------------------------------------

TEST("StreamAccumulator: captures a usage-only final chunk") {
    StreamAccumulator acc;
    acc.ingest(nlohmann::json::parse(
        R"({"choices":[{"delta":{"content":"hi"}}]})"));
    // include_usage emits a trailing chunk whose choices array is empty.
    acc.ingest(nlohmann::json::parse(
        R"({"choices":[],"usage":{"prompt_tokens":7,"completion_tokens":2,"total_tokens":9}})"));
    Turn t = acc.finish();
    CHECK_EQ(t.text, std::string("hi"));
    CHECK(t.usage.present);
    CHECK_EQ(t.usage.total_tokens, 9);
}

TEST("StreamAccumulator: no usage chunk leaves usage not-present") {
    StreamAccumulator acc;
    acc.ingest(nlohmann::json::parse(R"({"choices":[{"delta":{"content":"hi"}}]})"));
    CHECK(!acc.finish().usage.present);
}
