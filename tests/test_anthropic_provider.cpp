#include "agent/anthropic_provider.hpp"

#include "agent/json_util.hpp"
#include "agent/stream.hpp"
#include "loopback_server.hpp"
#include "test_harness.hpp"

using namespace moocode;

namespace {
AnthropicConfig cfg() {
    AnthropicConfig c;
    c.base_url = "https://api.example/v1";
    c.api_key = "KEY";
    c.model = "claude-test";
    c.max_tokens = 1234;
    c.temperature = 0.2;
    c.timeout_secs = 30;
    return c;
}
}  // namespace

// --- provider choice / detection --------------------------------------------

TEST("parse_provider_choice: recognises anthropic / openai / auto") {
    CHECK(parse_provider_choice("anthropic") == ProviderChoice::Anthropic);
    CHECK(parse_provider_choice("Claude") == ProviderChoice::Anthropic);
    CHECK(parse_provider_choice("openai") == ProviderChoice::OpenAI);
    CHECK(parse_provider_choice("GPT") == ProviderChoice::OpenAI);
    CHECK(parse_provider_choice("auto") == ProviderChoice::Auto);
    CHECK(parse_provider_choice("") == ProviderChoice::Auto);
    CHECK(parse_provider_choice("nonsense") == ProviderChoice::Auto);
}

TEST("detect_provider_kind: anthropic host => Anthropic, else OpenAI") {
    CHECK(detect_provider_kind("https://api.anthropic.com/v1", "claude-x") ==
          ProviderKind::Anthropic);
    CHECK(detect_provider_kind("https://API.ANTHROPIC.com/v1", "x") ==
          ProviderKind::Anthropic);
    // OpenAI-compatible gateway serving Claude must NOT be misclassified.
    CHECK(detect_provider_kind("https://openrouter.ai/api/v1",
                               "anthropic/claude-3.5") == ProviderKind::OpenAI);
    CHECK(detect_provider_kind("https://api.minimax.io/v1", "MiniMax-M3") ==
          ProviderKind::OpenAI);
}

TEST("anthropic_models_url: appends /v1/models, or /models when versioned") {
    CHECK_EQ(anthropic_models_url("https://api.anthropic.com"),
             std::string("https://api.anthropic.com/v1/models"));
    CHECK_EQ(anthropic_models_url("https://api.anthropic.com/v1"),
             std::string("https://api.anthropic.com/v1/models"));
}

TEST("list_models: parses Anthropic /v1/models with version header") {
    test::LoopbackServer srv;
    srv.serve(200, R"({"data":[
        {"type":"model","id":"claude-opus-4-8","display_name":"Opus"},
        {"type":"model","id":"claude-sonnet-4-6"}],"has_more":false})");
    AnthropicConfig cf = cfg();
    cf.base_url = srv.url("");  // bare host => GET /v1/models
    AnthropicProvider p(cf);
    auto models = p.list_models();
    srv.stop();
    CHECK(srv.last_request().find("GET /v1/models") != std::string::npos);
    CHECK(srv.last_request().find("x-api-key: KEY") != std::string::npos);
    CHECK(srv.last_request().find("anthropic-version:") != std::string::npos);
    CHECK(models.has_value());
    if (models) {
        CHECK_EQ(models->size(), size_t{2});
        CHECK_EQ((*models)[0], std::string("claude-opus-4-8"));
    }
}

TEST("anthropic_messages_url: appends /v1/messages, or /messages when versioned") {
    // Bare ANTHROPIC_BASE_URL forms the vendors document.
    CHECK_EQ(anthropic_messages_url("https://api.minimax.io/anthropic"),
             std::string("https://api.minimax.io/anthropic/v1/messages"));
    CHECK_EQ(anthropic_messages_url("https://api.deepseek.com/anthropic"),
             std::string("https://api.deepseek.com/anthropic/v1/messages"));
    // An explicit /v1 base must not double up.
    CHECK_EQ(anthropic_messages_url("https://api.anthropic.com/v1"),
             std::string("https://api.anthropic.com/v1/messages"));
}

TEST("lookup_preset: minimax + deepseek pro/flash, nullopt otherwise") {
    auto mm = lookup_preset("minimax");
    CHECK(mm.has_value());
    if (mm) {
        CHECK(mm->kind == ProviderKind::OpenAI);
        CHECK_EQ(mm->base_url, std::string("https://api.minimax.io/v1"));
        CHECK_EQ(mm->model, std::string("MiniMax-M3"));
    }
    auto pro = lookup_preset("DeepSeek-Pro");  // case-insensitive
    CHECK(pro.has_value());
    if (pro) {
        CHECK_EQ(pro->base_url, std::string("https://api.deepseek.com/v1"));
        CHECK_EQ(pro->model, std::string("deepseek-v4-pro"));
    }
    auto flash = lookup_preset("deepseek-flash");
    CHECK(flash.has_value());
    if (flash) CHECK_EQ(flash->model, std::string("deepseek-v4-flash"));
    CHECK(lookup_preset("deepseek").has_value());  // alias for pro
    if (auto d = lookup_preset("deepseek"))
        CHECK_EQ(d->model, std::string("deepseek-v4-pro"));
    // Non-presets fall through to parse_provider_choice.
    CHECK(!lookup_preset("openai").has_value());
    CHECK(!lookup_preset("anthropic").has_value());
    CHECK(!lookup_preset("auto").has_value());
    CHECK(!lookup_preset("").has_value());
}

TEST("provider_kind_name: stable labels") {
    CHECK_EQ(std::string(provider_kind_name(ProviderKind::Anthropic)),
             std::string("anthropic"));
    CHECK_EQ(std::string(provider_kind_name(ProviderKind::OpenAI)),
             std::string("openai"));
}

TEST("AnthropicProvider: wire_format label is stable") {
    CHECK_EQ(std::string(AnthropicProvider(cfg()).wire_format()),
             std::string("anthropic"));
}

// --- build_messages_request -------------------------------------------------

TEST("build_messages_request: model, max_tokens, temperature present") {
    Conversation c{Message::user("hi")};
    auto req = build_messages_request(cfg(), c, {});
    CHECK_EQ(req["model"], std::string("claude-test"));
    CHECK_EQ(req["max_tokens"], 1234);
    CHECK(req.contains("temperature"));
    CHECK(req["messages"].is_array());
}

TEST("build_messages_request: system hoisted to top-level field") {
    Conversation c{Message::system("you are x"), Message::user("go")};
    auto req = build_messages_request(cfg(), c, {});
    CHECK_EQ(req["system"], std::string("you are x"));
    // The system message must NOT appear in messages[].
    CHECK_EQ(req["messages"].size(), size_t{1});
    CHECK_EQ(req["messages"][0]["role"], std::string("user"));
    CHECK_EQ(req["messages"][0]["content"], std::string("go"));
}

TEST("build_messages_request: assistant tool_calls => tool_use blocks") {
    std::vector<ToolCall> a_calls;
    a_calls.push_back({"tu_1", "read_file", R"({"path":"a.txt"})"});
    Message a = Message::assistant("let me look", std::move(a_calls));
    Conversation c{Message::user("hi"), a};
    auto req = build_messages_request(cfg(), c, {});
    auto& content = req["messages"][1]["content"];
    CHECK(content.is_array());
    CHECK_EQ(content[0]["type"], std::string("text"));
    CHECK_EQ(content[0]["text"], std::string("let me look"));
    CHECK_EQ(content[1]["type"], std::string("tool_use"));
    CHECK_EQ(content[1]["id"], std::string("tu_1"));
    CHECK_EQ(content[1]["name"], std::string("read_file"));
    CHECK_EQ(content[1]["input"]["path"], std::string("a.txt"));
}

TEST("build_messages_request: tool results coalesced into one user message") {
    std::vector<ToolCall> a_calls;
    a_calls.push_back({"tu_1", "a", "{}"});
    a_calls.push_back({"tu_2", "b", "{}"});
    Message a = Message::assistant("", std::move(a_calls));
    // tu_1 succeeded; tu_2 failed (carried as data, not derived from content).
    Message ok = Message::tool("tu_1", "result one");
    Message bad = Message::tool("tu_2", "ERROR: boom", true);
    Conversation c{Message::user("hi"), a, ok, bad};
    auto req = build_messages_request(cfg(), c, {});
    // user, assistant, user(tool_results) => 3 messages.
    CHECK_EQ(req["messages"].size(), size_t{3});
    auto& tr = req["messages"][2];
    CHECK_EQ(tr["role"], std::string("user"));
    CHECK_EQ(tr["content"].size(), size_t{2});
    CHECK_EQ(tr["content"][0]["type"], std::string("tool_result"));
    CHECK_EQ(tr["content"][0]["tool_use_id"], std::string("tu_1"));
    CHECK_EQ(tr["content"][0]["content"], std::string("result one"));
    CHECK(!tr["content"][0].contains("is_error"));
    // The failed result is flagged is_error.
    CHECK_EQ(tr["content"][1]["tool_use_id"], std::string("tu_2"));
    CHECK_EQ(tr["content"][1]["is_error"], true);
}

// is_error follows the typed tool_failed bit, NOT the content prefix: a tool
// that succeeded but whose text begins with "ERROR:" must carry no is_error.
TEST("build_messages_request: is_error follows tool_failed, not content") {
    std::vector<ToolCall> a_calls;
    a_calls.push_back({"tu_1", "a", "{}"});
    Message a = Message::assistant("", std::move(a_calls));
    Message ok = Message::tool("tu_1", "ERROR: not really");  // succeeded (tool_failed=false)
    Conversation c{Message::user("hi"), a, ok};
    auto req = build_messages_request(cfg(), c, {});
    auto& tr = req["messages"][2];
    CHECK_EQ(tr["content"][0]["tool_use_id"], std::string("tu_1"));
    CHECK(!tr["content"][0].contains("is_error"));
}

TEST("build_messages_request: tools advertised with input_schema") {
    std::vector<ToolSpec> tools;
    tools.push_back({"read_file", "read a file",
                     nlohmann::json::parse(R"({"type":"object","properties":{}})")});
    Conversation c{Message::user("hi")};
    auto req = build_messages_request(cfg(), c, tools);
    CHECK(req.contains("tools"));
    auto& t = req["tools"][0];
    CHECK_EQ(t["name"], std::string("read_file"));
    CHECK_EQ(t["description"], std::string("read a file"));
    CHECK(t["input_schema"].contains("type"));
    CHECK(!t.contains("function"));
}

TEST("build_messages_request: empty tools omits the tools key") {
    Conversation c{Message::user("hi")};
    auto req = build_messages_request(cfg(), c, {});
    CHECK(!req.contains("tools"));
}

TEST("build_messages_request: stream flag only when requested") {
    Conversation c{Message::user("hi")};
    CHECK(!build_messages_request(cfg(), c, {}).contains("stream"));
    auto s = build_messages_request(cfg(), c, {}, /*stream=*/true);
    CHECK(s.contains("stream"));
    if (s.contains("stream")) CHECK_EQ(s["stream"], true);
}

// --- generation controls (effort -> thinking budget) ------------------------

TEST("effort_to_output_effort: maps labels to API effort values") {
    CHECK_EQ(effort_to_output_effort("minimal"), "low");  // no API minimal tier
    CHECK_EQ(effort_to_output_effort("low"), "low");
    CHECK_EQ(effort_to_output_effort("medium"), "medium");
    CHECK_EQ(effort_to_output_effort("high"), "high");
    CHECK_EQ(effort_to_output_effort("xhigh"), "xhigh");
    CHECK_EQ(effort_to_output_effort("max"), "max");
    CHECK_EQ(effort_to_output_effort("garbage"), "");  // omit => model default
}

TEST("build_messages_request: no thinking block by default") {
    auto req = build_messages_request(cfg(), Conversation{Message::user("hi")}, {});
    CHECK(!req.contains("thinking"));
    CHECK_EQ(req["temperature"], 0.2);  // unchanged
}

TEST("build_messages_request: effort enables adaptive thinking + output effort") {
    AnthropicConfig c = cfg();
    c.max_tokens = 1234;
    c.reasoning_effort = "high";
    auto req = build_messages_request(c, Conversation{Message::user("hi")}, {});
    CHECK(req.contains("thinking"));
    CHECK_EQ(req["thinking"]["type"], std::string("adaptive"));
    CHECK_EQ(req["thinking"]["display"], std::string("summarized"));
    CHECK_EQ(req["output_config"]["effort"], std::string("high"));
    CHECK(!req.contains("temperature"));  // dropped: rejected alongside thinking
    CHECK_EQ(req["max_tokens"], 1234);    // no budget bump under adaptive thinking
}

TEST("build_messages_request: explicit thinking off omits the block") {
    AnthropicConfig c = cfg();
    c.reasoning_effort = "high";  // would imply on...
    c.thinking = false;           // ...but explicit off wins
    auto req = build_messages_request(c, Conversation{Message::user("hi")}, {});
    CHECK(!req.contains("thinking"));
}

TEST("build_messages_request: thinking on without effort omits output_config") {
    AnthropicConfig c = cfg();
    c.thinking = true;
    auto req = build_messages_request(c, Conversation{Message::user("hi")}, {});
    CHECK_EQ(req["thinking"]["type"], std::string("adaptive"));
    CHECK(!req.contains("output_config"));  // no effort => model default
}

TEST("AnthropicProvider: set_params/params round-trip") {
    AnthropicProvider p(cfg());
    GenerationParams in;
    in.effort = "low";
    in.thinking = true;
    in.temperature = 0.5;
    p.set_params(in);
    GenerationParams out = p.params();
    CHECK(out.effort && *out.effort == "low");
    CHECK(out.thinking && *out.thinking == true);
    CHECK(out.temperature && *out.temperature == 0.5);
}

// --- parse_messages_response ------------------------------------------------

TEST("parse_messages_response: text-only answer") {
    auto body = json::parse(R"({
        "type":"message","role":"assistant",
        "content":[{"type":"text","text":"hello there"}],
        "stop_reason":"end_turn"})")
                    .value();
    auto t = parse_messages_response(body);
    CHECK(t.has_value());
    if (t) {
        CHECK_EQ(t->text, std::string("hello there"));
        CHECK(t->tool_calls.empty());
        CHECK_EQ(t->finish_reason, std::string("end_turn"));
    }
}

TEST("parse_messages_response: text + tool_use block") {
    auto body = json::parse(R"({
        "content":[
          {"type":"text","text":"sure"},
          {"type":"tool_use","id":"tu_9","name":"read_file","input":{"path":"a"}}],
        "stop_reason":"tool_use",
        "usage":{"input_tokens":10,"output_tokens":4}})")
                    .value();
    auto t = parse_messages_response(body);
    CHECK(t.has_value());
    if (t) {
        CHECK_EQ(t->text, std::string("sure"));
        CHECK_EQ(t->tool_calls.size(), size_t{1});
        if (!t->tool_calls.empty()) {
            CHECK_EQ(t->tool_calls[0].id, std::string("tu_9"));
            CHECK_EQ(t->tool_calls[0].name, std::string("read_file"));
            CHECK_EQ(t->tool_calls[0].arguments_json, std::string(R"({"path":"a"})"));
        }
        CHECK(t->usage.present);
        CHECK_EQ(t->usage.prompt_tokens, 10);
        CHECK_EQ(t->usage.completion_tokens, 4);
        CHECK_EQ(t->usage.total_tokens, 14);
    }
}

TEST("parse_messages_response: thinking block ignored in text") {
    auto body = json::parse(R"({
        "content":[{"type":"thinking","thinking":"hmm"},
                   {"type":"text","text":"answer"}]})")
                    .value();
    auto t = parse_messages_response(body);
    CHECK(t.has_value());
    if (t) CHECK_EQ(t->text, std::string("answer"));
}

TEST("parse_messages_response: API error object returns Error") {
    auto body =
        json::parse(R"({"type":"error","error":{"type":"x","message":"bad key"}})")
            .value();
    auto t = parse_messages_response(body);
    CHECK(!t.has_value());
    if (!t) CHECK(t.error().msg.find("bad key") != std::string::npos);
}

TEST("parse_messages_response: missing content array returns Error") {
    auto body = json::parse(R"({"type":"message","role":"assistant"})").value();
    auto t = parse_messages_response(body);
    CHECK(!t.has_value());
}

// --- AnthropicStreamAccumulator ---------------------------------------------

TEST("AnthropicStreamAccumulator: assembles text + usage + finish_reason") {
    AnthropicStreamAccumulator acc;
    std::string answer;
    auto feed = [&](const char* s) {
        auto a = acc.ingest(json::parse(s).value());
        answer += a.answer;
    };
    feed(R"({"type":"message_start","message":{"usage":{"input_tokens":7,"output_tokens":1}}})");
    feed(R"({"type":"content_block_start","index":0,"content_block":{"type":"text","text":""}})");
    feed(R"({"type":"content_block_delta","index":0,"delta":{"type":"text_delta","text":"Hel"}})");
    feed(R"({"type":"content_block_delta","index":0,"delta":{"type":"text_delta","text":"lo"}})");
    feed(R"({"type":"content_block_stop","index":0})");
    feed(R"({"type":"message_delta","delta":{"stop_reason":"end_turn"},"usage":{"output_tokens":5}})");
    feed(R"({"type":"message_stop"})");
    CHECK_EQ(answer, std::string("Hello"));
    Turn t = acc.finish();
    CHECK_EQ(t.text, std::string("Hello"));
    CHECK_EQ(t.finish_reason, std::string("end_turn"));
    CHECK(t.usage.present);
    CHECK_EQ(t.usage.prompt_tokens, 7);
    CHECK_EQ(t.usage.completion_tokens, 5);
    CHECK_EQ(t.usage.total_tokens, 12);
    CHECK(t.tool_calls.empty());
}

TEST("AnthropicStreamAccumulator: thinking delta routes to reasoning") {
    AnthropicStreamAccumulator acc;
    auto a = acc.ingest(json::parse(R"({"type":"content_block_delta","index":0,
        "delta":{"type":"thinking_delta","thinking":"why"}})")
                            .value());
    CHECK_EQ(a.reasoning, std::string("why"));
    CHECK(a.answer.empty());
}

TEST("AnthropicStreamAccumulator: assembles a tool call across input deltas") {
    AnthropicStreamAccumulator acc;
    acc.ingest(json::parse(R"({"type":"content_block_start","index":0,
        "content_block":{"type":"tool_use","id":"tu_1","name":"read_file","input":{}}})")
                   .value());
    acc.ingest(json::parse(R"({"type":"content_block_delta","index":0,
        "delta":{"type":"input_json_delta","partial_json":"{\"p\":"}})")
                   .value());
    acc.ingest(json::parse(R"({"type":"content_block_delta","index":0,
        "delta":{"type":"input_json_delta","partial_json":"1}"}})")
                   .value());
    Turn t = acc.finish();
    CHECK_EQ(t.tool_calls.size(), size_t{1});
    if (!t.tool_calls.empty()) {
        CHECK_EQ(t.tool_calls[0].id, std::string("tu_1"));
        CHECK_EQ(t.tool_calls[0].name, std::string("read_file"));
        CHECK_EQ(t.tool_calls[0].arguments_json, std::string(R"({"p":1})"));
    }
}

TEST("AnthropicStreamAccumulator: text then tool_use keep block order") {
    AnthropicStreamAccumulator acc;
    acc.ingest(json::parse(R"({"type":"content_block_delta","index":0,
        "delta":{"type":"text_delta","text":"ok"}})")
                   .value());
    acc.ingest(json::parse(R"({"type":"content_block_start","index":1,
        "content_block":{"type":"tool_use","id":"tu_2","name":"b","input":{}}})")
                   .value());
    Turn t = acc.finish();
    CHECK_EQ(t.text, std::string("ok"));
    CHECK_EQ(t.tool_calls.size(), size_t{1});
    if (!t.tool_calls.empty()) {
        CHECK_EQ(t.tool_calls[0].name, std::string("b"));
        CHECK_EQ(t.tool_calls[0].arguments_json, std::string("{}"));
    }
}

TEST("AnthropicStreamAccumulator: error event recorded") {
    AnthropicStreamAccumulator acc;
    acc.ingest(json::parse(
                   R"({"type":"error","error":{"type":"overloaded","message":"busy"}})")
                   .value());
    CHECK(acc.error().has_value());
    if (acc.error()) CHECK_EQ(*acc.error(), std::string("busy"));
}

// --- complete() integration over loopback -----------------------------------

TEST("complete: round-trips and parses a Turn from /messages") {
    test::LoopbackServer srv;
    srv.serve(200, R"({"type":"message","role":"assistant",
        "content":[{"type":"text","text":"done"}],"stop_reason":"end_turn"})");
    AnthropicConfig cf = cfg();
    cf.base_url = srv.url("");  // provider appends /messages
    AnthropicProvider p(cf);
    Conversation c{Message::user("hi")};
    auto t = p.complete(c, {});
    srv.stop();
    CHECK(t.has_value());
    if (t) CHECK_EQ(t->text, std::string("done"));
}

TEST("complete: posts to /messages with x-api-key + anthropic-version") {
    test::LoopbackServer srv;
    srv.serve(200, R"({"content":[{"type":"text","text":"ok"}]})");
    AnthropicConfig cf = cfg();
    cf.base_url = srv.url("");
    AnthropicProvider p(cf);
    Conversation c{Message::user("hi")};
    auto t = p.complete(c, {});
    srv.stop();
    CHECK(srv.last_request().find("POST /v1/messages") != std::string::npos);
    CHECK(srv.last_request().find("x-api-key: KEY") != std::string::npos);
    CHECK(srv.last_request().find("anthropic-version: 2023-06-01") !=
          std::string::npos);
}

TEST("complete: HTTP 401 surfaces as Error with body message") {
    test::LoopbackServer srv;
    srv.serve(401, R"({"type":"error","error":{"message":"unauthorized"}})");
    AnthropicConfig cf = cfg();
    cf.base_url = srv.url("");
    AnthropicProvider p(cf);
    Conversation c{Message::user("hi")};
    auto t = p.complete(c, {});
    srv.stop();
    CHECK(!t.has_value());
    if (!t) CHECK(t.error().msg.find("unauthorized") != std::string::npos);
}

// --- complete_stream() integration over loopback ----------------------------

TEST("complete_stream: assembles a Turn from typed SSE events") {
    test::LoopbackServer srv;
    srv.serve(200,
              "event: message_start\n"
              "data: {\"type\":\"message_start\",\"message\":{\"usage\":"
              "{\"input_tokens\":3,\"output_tokens\":1}}}\n\n"
              "event: content_block_delta\n"
              "data: {\"type\":\"content_block_delta\",\"index\":0,\"delta\":"
              "{\"type\":\"text_delta\",\"text\":\"Hel\"}}\n\n"
              "event: content_block_delta\n"
              "data: {\"type\":\"content_block_delta\",\"index\":0,\"delta\":"
              "{\"type\":\"text_delta\",\"text\":\"lo\"}}\n\n"
              "event: message_delta\n"
              "data: {\"type\":\"message_delta\",\"delta\":{\"stop_reason\":"
              "\"end_turn\"},\"usage\":{\"output_tokens\":2}}\n\n"
              "event: message_stop\n"
              "data: {\"type\":\"message_stop\"}\n\n");
    AnthropicConfig cf = cfg();
    cf.base_url = srv.url("");
    AnthropicProvider p(cf);
    Conversation c{Message::user("hi")};
    std::string answer;
    auto t = p.complete_stream(
        c, {}, [&](std::string_view a, std::string_view) { answer.append(a); });
    srv.stop();
    CHECK(t.has_value());
    if (t) {
        CHECK_EQ(t->text, std::string("Hello"));
        CHECK_EQ(t->finish_reason, std::string("end_turn"));
        CHECK(t->usage.present);
    }
    CHECK_EQ(answer, std::string("Hello"));
}

TEST("complete_stream: sets stream:true in the posted request") {
    test::LoopbackServer srv;
    srv.serve(200, "event: message_stop\ndata: {\"type\":\"message_stop\"}\n\n");
    AnthropicConfig cf = cfg();
    cf.base_url = srv.url("");
    AnthropicProvider p(cf);
    Conversation c{Message::user("hi")};
    auto t = p.complete_stream(c, {}, {});
    srv.stop();
    CHECK(srv.last_body().find(R"("stream":true)") != std::string::npos);
}

// Reproduces the exact parts shape the TUI submit builds for a pasted image:
// parts[0] = text (no image), parts[1] = image (empty text). The serialized
// user message must contain both a text block and a base64 image block.
TEST("build_messages_request: user image part => base64 image block") {
    std::vector<ContentPart> u_parts;
    u_parts.push_back(ContentPart{"[image #1] what do you think about those two?", std::nullopt});
    u_parts.push_back(ContentPart{"", ImageBlock{"QUJD", "image/png"}});
    Message u = Message::user("[image #1] what do you think about those two?", std::move(u_parts));
    Conversation c{u};
    auto req = build_messages_request(cfg(), c, {});
    auto& content = req["messages"][0]["content"];
    CHECK(content.is_array());
    CHECK_EQ(content.size(), size_t{2});
    CHECK_EQ(content[0]["type"], std::string("text"));
    CHECK_EQ(content[1]["type"], std::string("image"));
    CHECK_EQ(content[1]["source"]["type"], std::string("base64"));
    CHECK_EQ(content[1]["source"]["media_type"], std::string("image/png"));
    CHECK_EQ(content[1]["source"]["data"], std::string("QUJD"));
}
