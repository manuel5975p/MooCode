#include "agent/openai_provider.hpp"

#include "agent/json_util.hpp"
#include "loopback_server.hpp"
#include "test_harness.hpp"

using namespace moocode;

namespace {
OpenAiConfig cfg() {
    OpenAiConfig c;
    c.base_url = "https://api.example/v1";
    c.api_key = "KEY";
    c.model = "test-model";
    c.temperature = 0.2;
    c.timeout_secs = 30;
    return c;
}
}  // namespace

// --- build_chat_request -----------------------------------------------------

TEST("build_chat_request: includes model and temperature") {
    Conversation c{Message::user("hi")};
    auto req = build_chat_request(cfg(), c, {});
    CHECK_EQ(req["model"], std::string("test-model"));
    CHECK(req.contains("temperature"));
    CHECK(req["messages"].is_array());
}

TEST("build_chat_request: maps system and user roles") {
    Conversation c{Message::system("you are x"), Message::user("go")};
    auto req = build_chat_request(cfg(), c, {});
    CHECK_EQ(req["messages"].size(), size_t{2});
    CHECK_EQ(req["messages"][0]["role"], std::string("system"));
    CHECK_EQ(req["messages"][0]["content"], std::string("you are x"));
    CHECK_EQ(req["messages"][1]["role"], std::string("user"));
}

TEST("build_chat_request: assistant tool_calls serialized in OpenAI shape") {
    std::vector<ToolCall> calls;
    calls.push_back({"call_1", "read_file", R"({"path":"a.txt"})"});
    Message a = Message::assistant("", std::move(calls));
    Conversation c{Message::user("hi"), a};
    auto req = build_chat_request(cfg(), c, {});
    auto& msg = req["messages"][1];
    CHECK_EQ(msg["role"], std::string("assistant"));
    CHECK(msg.contains("tool_calls"));
    auto& tc = msg["tool_calls"][0];
    CHECK_EQ(tc["id"], std::string("call_1"));
    CHECK_EQ(tc["type"], std::string("function"));
    CHECK_EQ(tc["function"]["name"], std::string("read_file"));
    CHECK_EQ(tc["function"]["arguments"], std::string(R"({"path":"a.txt"})"));
}

TEST("build_chat_request: tool-role message carries tool_call_id") {
    Message t = Message::tool("call_1", "file contents");
    Conversation c{Message::user("hi"), t};
    auto req = build_chat_request(cfg(), c, {});
    auto& msg = req["messages"][1];
    CHECK_EQ(msg["role"], std::string("tool"));
    CHECK_EQ(msg["tool_call_id"], std::string("call_1"));
    CHECK_EQ(msg["content"], std::string("file contents"));
}

// A tool-role message keeps its own turn (role "tool"), so a user prompt
// buffered after it is a valid continuation and must NOT be merged into it.
TEST("build_chat_request: user after tool-role message is not merged") {
    Conversation c{Message::user("hi"), Message::tool("call_1", "out"),
                   Message::user("more")};
    auto req = build_chat_request(cfg(), c, {});
    CHECK_EQ(req["messages"].size(), size_t{3});
    CHECK_EQ(req["messages"][1]["role"], std::string("tool"));
    CHECK_EQ(req["messages"][2]["role"], std::string("user"));
    CHECK_EQ(req["messages"][2]["content"], std::string("more"));
}

// Two consecutive user turns (e.g. a buffer flushed before the first request)
// merge into one message whose content is the two text blocks.
TEST("build_chat_request: consecutive user messages merged into one") {
    Conversation c{Message::user("first"), Message::user("second")};
    auto req = build_chat_request(cfg(), c, {});
    CHECK_EQ(req["messages"].size(), size_t{1});
    auto& m = req["messages"][0];
    CHECK_EQ(m["role"], std::string("user"));
    CHECK_EQ(m["content"].size(), size_t{2});
    CHECK_EQ(m["content"][0]["text"], std::string("first"));
    CHECK_EQ(m["content"][1]["text"], std::string("second"));
}

TEST("build_chat_request: tools advertised as function specs") {
    std::vector<ToolSpec> tools;
    tools.push_back({"read_file", "read a file",
                     nlohmann::json::parse(R"({"type":"object","properties":{}})")});
    Conversation c{Message::user("hi")};
    auto req = build_chat_request(cfg(), c, tools);
    CHECK(req.contains("tools"));
    auto& t = req["tools"][0];
    CHECK_EQ(t["type"], std::string("function"));
    CHECK_EQ(t["function"]["name"], std::string("read_file"));
    CHECK_EQ(t["function"]["description"], std::string("read a file"));
    CHECK(t["function"]["parameters"].contains("type"));
}

TEST("build_chat_request: empty tools omits the tools key") {
    Conversation c{Message::user("hi")};
    auto req = build_chat_request(cfg(), c, {});
    CHECK(!req.contains("tools"));
}

TEST("build_chat_request: no stream key by default") {
    Conversation c{Message::user("hi")};
    auto req = build_chat_request(cfg(), c, {});
    CHECK(!req.contains("stream"));
}

TEST("build_chat_request: stream flag set when requested") {
    Conversation c{Message::user("hi")};
    auto req = build_chat_request(cfg(), c, {}, /*stream=*/true);
    CHECK(req.contains("stream"));
    if (req.contains("stream")) CHECK_EQ(req["stream"], true);
}

// --- allowed_openai_params --------------------------------------------------

TEST("build_chat_request: no allowed_openai_params key when unconfigured") {
    Conversation c{Message::user("hi")};
    auto req = build_chat_request(cfg(), c, {});
    CHECK(!req.contains("allowed_openai_params"));
}

TEST("build_chat_request: emits allowed_openai_params on matching url+model") {
    auto cc = cfg();
    cc.reasoning_effort = "high";
    cc.allowed_openai_params = {
        {"https://api.example/v1", "test-model", {"reasoning_effort"}}};
    Conversation c{Message::user("hi")};
    auto req = build_chat_request(cc, c, {});
    CHECK(req.contains("allowed_openai_params"));
    if (req.contains("allowed_openai_params")) {
        CHECK(req["allowed_openai_params"].is_array());
        CHECK_EQ(req["allowed_openai_params"].size(), std::size_t(1));
        CHECK_EQ(req["allowed_openai_params"][0], std::string("reasoning_effort"));
    }
    // The gated param itself is still emitted independently.
    CHECK_EQ(req["reasoning_effort"], std::string("high"));
}

TEST("build_chat_request: allowed_openai_params match tolerates trailing slash") {
    auto cc = cfg();  // base_url ends in "/v1" (no slash)
    cc.allowed_openai_params = {
        {"https://api.example/v1/", "test-model", {"reasoning_effort"}}};
    Conversation c{Message::user("hi")};
    auto req = build_chat_request(cc, c, {});
    CHECK(req.contains("allowed_openai_params"));
}

TEST("build_chat_request: allowed_openai_params model match is case-insensitive") {
    auto cc = cfg();  // model "test-model"
    cc.allowed_openai_params = {
        {"https://api.example/v1", "TEST-MODEL", {"reasoning_effort"}}};
    Conversation c{Message::user("hi")};
    auto req = build_chat_request(cc, c, {});
    CHECK(req.contains("allowed_openai_params"));
}

TEST("build_chat_request: allowed_openai_params skipped on model mismatch") {
    auto cc = cfg();  // model "test-model"
    cc.allowed_openai_params = {
        {"https://api.example/v1", "other-model", {"reasoning_effort"}}};
    Conversation c{Message::user("hi")};
    auto req = build_chat_request(cc, c, {});
    CHECK(!req.contains("allowed_openai_params"));
}

TEST("build_chat_request: allowed_openai_params skipped on url mismatch") {
    auto cc = cfg();  // base_url "https://api.example/v1"
    cc.allowed_openai_params = {
        {"https://other.example/v1", "test-model", {"reasoning_effort"}}};
    Conversation c{Message::user("hi")};
    auto req = build_chat_request(cc, c, {});
    CHECK(!req.contains("allowed_openai_params"));
}

TEST("build_chat_request: empty params list emits nothing even on match") {
    auto cc = cfg();
    cc.allowed_openai_params = {{"https://api.example/v1", "test-model", {}}};
    Conversation c{Message::user("hi")};
    auto req = build_chat_request(cc, c, {});
    CHECK(!req.contains("allowed_openai_params"));
}

// --- drop_reasoning_effort --------------------------------------------------

TEST("build_chat_request: reasoning_effort emitted when not in drop list") {
    auto cc = cfg();
    cc.reasoning_effort = "high";
    Conversation c{Message::user("hi")};
    auto req = build_chat_request(cc, c, {});
    CHECK_EQ(req["reasoning_effort"], std::string("high"));
}

TEST("build_chat_request: reasoning_effort dropped on matching url+model") {
    auto cc = cfg();
    cc.reasoning_effort = "high";
    cc.drop_reasoning_effort = {{"https://api.example/v1", "test-model"}};
    Conversation c{Message::user("hi")};
    auto req = build_chat_request(cc, c, {});
    CHECK(!req.contains("reasoning_effort"));
}

TEST("build_chat_request: drop_reasoning_effort tolerates trailing slash") {
    auto cc = cfg();  // base_url ends in "/v1" (no slash)
    cc.reasoning_effort = "high";
    cc.drop_reasoning_effort = {{"https://api.example/v1/", "test-model"}};
    Conversation c{Message::user("hi")};
    auto req = build_chat_request(cc, c, {});
    CHECK(!req.contains("reasoning_effort"));
}

TEST("build_chat_request: drop_reasoning_effort model match is case-insensitive") {
    auto cc = cfg();  // model "test-model"
    cc.reasoning_effort = "high";
    cc.drop_reasoning_effort = {{"https://api.example/v1", "TEST-MODEL"}};
    Conversation c{Message::user("hi")};
    auto req = build_chat_request(cc, c, {});
    CHECK(!req.contains("reasoning_effort"));
}

TEST("build_chat_request: drop_reasoning_effort skipped on model mismatch") {
    auto cc = cfg();
    cc.reasoning_effort = "high";
    cc.drop_reasoning_effort = {{"https://api.example/v1", "other-model"}};
    Conversation c{Message::user("hi")};
    auto req = build_chat_request(cc, c, {});
    CHECK_EQ(req["reasoning_effort"], std::string("high"));
}

// --- parse_chat_response ----------------------------------------------------

TEST("parse_chat_response: text-only answer") {
    auto body = json::parse(R"({
        "choices":[{"message":{"role":"assistant","content":"hello there"},
                    "finish_reason":"stop"}]})")
                    .value();
    auto t = parse_chat_response(body);
    CHECK(t.has_value());
    if (t) {
        CHECK_EQ(t->text, std::string("hello there"));
        CHECK(t->tool_calls.empty());
        CHECK_EQ(t->finish_reason, std::string("stop"));
    }
}

TEST("parse_chat_response: single tool call") {
    auto body = json::parse(R"({
        "choices":[{"message":{"role":"assistant","content":null,
            "tool_calls":[{"id":"call_1","type":"function",
                "function":{"name":"read_file","arguments":"{\"path\":\"a\"}"}}]},
            "finish_reason":"tool_calls"}]})")
                    .value();
    auto t = parse_chat_response(body);
    CHECK(t.has_value());
    if (t) {
        CHECK_EQ(t->tool_calls.size(), size_t{1});
        CHECK_EQ(t->tool_calls[0].id, std::string("call_1"));
        CHECK_EQ(t->tool_calls[0].name, std::string("read_file"));
        CHECK_EQ(t->tool_calls[0].arguments_json, std::string(R"({"path":"a"})"));
        CHECK(t->text.empty());
    }
}

TEST("parse_chat_response: multiple tool calls preserve order") {
    auto body = json::parse(R"({
        "choices":[{"message":{"role":"assistant","content":null,
            "tool_calls":[
              {"id":"c1","type":"function","function":{"name":"a","arguments":"{}"}},
              {"id":"c2","type":"function","function":{"name":"b","arguments":"{}"}}
            ]},"finish_reason":"tool_calls"}]})")
                    .value();
    auto t = parse_chat_response(body);
    CHECK(t.has_value());
    if (t) {
        CHECK_EQ(t->tool_calls.size(), size_t{2});
        CHECK_EQ(t->tool_calls[0].name, std::string("a"));
        CHECK_EQ(t->tool_calls[1].name, std::string("b"));
    }
}

TEST("parse_chat_response: empty content with no tool calls returns Error") {
    auto body = json::parse(R"({
        "choices":[{"message":{"role":"assistant","content":""},
                    "finish_reason":"stop"}]})")
                    .value();
    auto t = parse_chat_response(body);
    CHECK(!t.has_value());
    if (!t) CHECK(t.error().msg.find("no content") != std::string::npos);
    if (!t) CHECK(t.error().msg.find("stop") != std::string::npos);
}

TEST("parse_chat_response: API error object returns Error") {
    auto body = json::parse(R"({"error":{"message":"bad key","type":"auth"}})").value();
    auto t = parse_chat_response(body);
    CHECK(!t.has_value());
    if (!t) CHECK(t.error().msg.find("bad key") != std::string::npos);
}

TEST("parse_chat_response: missing choices returns Error") {
    auto body = json::parse(R"({"id":"x"})").value();
    auto t = parse_chat_response(body);
    CHECK(!t.has_value());
}

TEST("parse_chat_response: empty choices array returns Error") {
    auto body = json::parse(R"({"choices":[]})").value();
    auto t = parse_chat_response(body);
    CHECK(!t.has_value());
}

// --- complete() integration over loopback -----------------------------------

TEST("complete: round-trips against a server and parses the Turn") {
    test::LoopbackServer srv;
    srv.serve(200, R"({"choices":[{"message":{"role":"assistant",
        "content":"done"},"finish_reason":"stop"}]})");
    OpenAiConfig cf = cfg();
    cf.base_url = srv.url("");  // bare host; provider appends /v1/chat/completions
    OpenAiProvider p(cf);
    Conversation c{Message::user("hi")};
    auto t = p.complete(c, {});
    srv.stop();
    CHECK(t.has_value());
    if (t) CHECK_EQ(t->text, std::string("done"));
}

TEST("complete: posts to /v1/chat/completions with bearer auth") {
    test::LoopbackServer srv;
    srv.serve(200, R"({"choices":[{"message":{"content":"ok"},"finish_reason":"stop"}]})");
    OpenAiConfig cf = cfg();
    cf.base_url = srv.url("");  // bare host => /v1/chat/completions
    OpenAiProvider p(cf);
    Conversation c{Message::user("hi")};
    auto t = p.complete(c, {});
    srv.stop();
    CHECK(srv.last_request().find("POST /v1/chat/completions") != std::string::npos);
    CHECK(srv.last_request().find("Authorization: Bearer KEY") != std::string::npos);
}

TEST("openai_chat_completions_url: bare host, versioned base, and full endpoint") {
    // Bare host gets the full versioned path (llama.cpp/vLLM/Ollama default).
    CHECK_EQ(openai_chat_completions_url("http://localhost:8080"),
             std::string("http://localhost:8080/v1/chat/completions"));
    // A base already ending in /v1 only needs the endpoint.
    CHECK_EQ(openai_chat_completions_url("https://api.openai.com/v1"),
             std::string("https://api.openai.com/v1/chat/completions"));
    // A fully-qualified endpoint is respected verbatim (escape hatch for servers
    // that serve /chat/completions at the root without a /v1 prefix).
    CHECK_EQ(openai_chat_completions_url("http://host:9000/chat/completions"),
             std::string("http://host:9000/chat/completions"));
}

TEST("OpenAiProvider: wire_format label is stable") {
    CHECK_EQ(std::string(OpenAiProvider(cfg()).wire_format()), std::string("openai"));
}

// --- model listing ----------------------------------------------------------

TEST("openai_models_url: bare host, versioned base, and full endpoint") {
    CHECK_EQ(openai_models_url("http://localhost:11434"),
             std::string("http://localhost:11434/v1/models"));
    CHECK_EQ(openai_models_url("https://api.openai.com/v1"),
             std::string("https://api.openai.com/v1/models"));
    // A chat-completions base resolves to the sibling /models endpoint.
    CHECK_EQ(openai_models_url("https://api.openai.com/v1/chat/completions"),
             std::string("https://api.openai.com/v1/models"));
}

TEST("parse_model_ids: extracts ids, tolerates odd entries") {
    auto body = json::parse(R"({"object":"list","data":[
        {"id":"gpt-4","object":"model"},
        {"object":"model"},
        {"id":42},
        {"id":"qwen2.5-coder"}]})").value();
    auto ids = parse_model_ids(body);
    CHECK_EQ(ids.size(), size_t{2});
    CHECK_EQ(ids[0], std::string("gpt-4"));
    CHECK_EQ(ids[1], std::string("qwen2.5-coder"));
}

TEST("parse_model_ids: empty data and missing data key give empty list") {
    CHECK(parse_model_ids(json::parse(R"({"data":[]})").value()).empty());
    CHECK(parse_model_ids(json::parse(R"({})").value()).empty());
    CHECK(parse_model_ids(json::parse(R"({"data":"nope"})").value()).empty());
}

TEST("list_models: round-trips a /models listing over loopback") {
    test::LoopbackServer srv;
    srv.serve(200, R"({"object":"list","data":[
        {"id":"llama3.1"},{"id":"qwen2.5-coder"}]})");
    OpenAiConfig cf = cfg();
    cf.base_url = srv.url("");  // bare host => GET /v1/models
    OpenAiProvider p(cf);
    auto models = p.list_models();
    srv.stop();
    CHECK(srv.last_request().find("GET /v1/models") != std::string::npos);
    CHECK(srv.last_request().find("Authorization: Bearer KEY") != std::string::npos);
    CHECK(models.has_value());
    if (models) {
        CHECK_EQ(models->size(), size_t{2});
        CHECK_EQ((*models)[0], std::string("llama3.1"));
    }
}

TEST("list_models: keyless endpoint sends no Authorization header") {
    test::LoopbackServer srv;
    srv.serve(200, R"({"data":[{"id":"local-model"}]})");
    OpenAiConfig cf = cfg();
    cf.api_key = "";  // keyless local server
    cf.base_url = srv.url("");
    OpenAiProvider p(cf);
    auto models = p.list_models();
    srv.stop();
    CHECK(srv.last_request().find("Authorization") == std::string::npos);
    CHECK(models.has_value());
}

TEST("list_models: empty listing is a non-error empty vector") {
    test::LoopbackServer srv;
    srv.serve(200, R"({"object":"list","data":[]})");
    OpenAiConfig cf = cfg();
    cf.base_url = srv.url("");
    OpenAiProvider p(cf);
    auto models = p.list_models();
    srv.stop();
    CHECK(models.has_value());
    if (models) CHECK(models->empty());
}

TEST("list_models: HTTP 404 surfaces as Error") {
    test::LoopbackServer srv;
    srv.serve(404, R"({"error":{"message":"no such route"}})");
    OpenAiConfig cf = cfg();
    cf.base_url = srv.url("");
    OpenAiProvider p(cf);
    auto models = p.list_models();
    srv.stop();
    CHECK(!models.has_value());
    if (!models) CHECK(models.error().msg.find("no such route") != std::string::npos);
}

TEST("complete: HTTP 401 surfaces as Error with body") {
    test::LoopbackServer srv;
    srv.serve(401, R"({"error":{"message":"unauthorized"}})");
    OpenAiConfig cf = cfg();
    cf.base_url = srv.url("");
    OpenAiProvider p(cf);
    Conversation c{Message::user("hi")};
    auto t = p.complete(c, {});
    srv.stop();
    CHECK(!t.has_value());
    if (!t) CHECK(t.error().msg.find("unauthorized") != std::string::npos);
}

// --- complete_stream() integration over loopback ----------------------------

TEST("complete_stream: assembles a Turn from SSE deltas and streams the answer") {
    test::LoopbackServer srv;
    srv.serve(200,
              "data: {\"choices\":[{\"delta\":{\"content\":\"Hel\"}}]}\n\n"
              "data: {\"choices\":[{\"delta\":{\"content\":\"lo\"},"
              "\"finish_reason\":\"stop\"}]}\n\n"
              "data: [DONE]\n\n");
    OpenAiConfig cf = cfg();
    cf.base_url = srv.url("");
    OpenAiProvider p(cf);
    Conversation c{Message::user("hi")};
    std::string answer;
    auto t = p.complete_stream(
        c, {}, [&](std::string_view a, std::string_view) { answer.append(a); });
    srv.stop();
    CHECK(t.has_value());
    if (t) {
        CHECK_EQ(t->text, std::string("Hello"));
        CHECK_EQ(t->finish_reason, std::string("stop"));
    }
    CHECK_EQ(answer, std::string("Hello"));
}

TEST("complete_stream: routes reasoning_content to the reasoning sink") {
    test::LoopbackServer srv;
    srv.serve(200,
              "data: {\"choices\":[{\"delta\":{\"reasoning_content\":\"why\"}}]}\n\n"
              "data: {\"choices\":[{\"delta\":{\"content\":\"ok\"},"
              "\"finish_reason\":\"stop\"}]}\n\n"
              "data: [DONE]\n\n");
    OpenAiConfig cf = cfg();
    cf.base_url = srv.url("");
    OpenAiProvider p(cf);
    Conversation c{Message::user("hi")};
    std::string answer, reasoning;
    auto t = p.complete_stream(c, {}, [&](std::string_view a, std::string_view r) {
        answer.append(a);
        reasoning.append(r);
    });
    srv.stop();
    CHECK(t.has_value());
    if (t) CHECK_EQ(t->text, std::string("ok"));
    CHECK_EQ(answer, std::string("ok"));
    CHECK_EQ(reasoning, std::string("why"));
}

TEST("complete_stream: assembles a streamed tool call") {
    test::LoopbackServer srv;
    srv.serve(200,
              "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,"
              "\"id\":\"c1\",\"function\":{\"name\":\"read_file\","
              "\"arguments\":\"{\\\"p\\\":\"}}]}}]}\n\n"
              "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,"
              "\"function\":{\"arguments\":\"1}\"}}]},"
              "\"finish_reason\":\"tool_calls\"}]}\n\n"
              "data: [DONE]\n\n");
    OpenAiConfig cf = cfg();
    cf.base_url = srv.url("");
    OpenAiProvider p(cf);
    Conversation c{Message::user("hi")};
    auto t = p.complete_stream(c, {}, {});
    srv.stop();
    CHECK(t.has_value());
    if (t) {
        CHECK_EQ(t->tool_calls.size(), size_t{1});
        if (t->tool_calls.size() == 1) {
            CHECK_EQ(t->tool_calls[0].name, std::string("read_file"));
            CHECK_EQ(t->tool_calls[0].arguments_json, std::string(R"({"p":1})"));
        }
    }
}

TEST("complete_stream: sets stream:true in the posted request") {
    test::LoopbackServer srv;
    srv.serve(200, "data: [DONE]\n\n");
    OpenAiConfig cf = cfg();
    cf.base_url = srv.url("");
    OpenAiProvider p(cf);
    Conversation c{Message::user("hi")};
    auto t = p.complete_stream(c, {}, {});
    srv.stop();
    CHECK(srv.last_body().find(R"("stream":true)") != std::string::npos);
}

TEST("complete_stream: HTTP 401 with a JSON error body surfaces as Error") {
    test::LoopbackServer srv;
    srv.serve(401, R"({"error":{"message":"unauthorized"}})");
    OpenAiConfig cf = cfg();
    cf.base_url = srv.url("");
    OpenAiProvider p(cf);
    Conversation c{Message::user("hi")};
    auto t = p.complete_stream(c, {}, {});
    srv.stop();
    CHECK(!t.has_value());
    if (!t) CHECK(t.error().msg.find("unauthorized") != std::string::npos);
}

TEST("complete_stream: retries past a transient 502 and recovers") {
    // A proxy 502 (HTML body, the exact shape from the field report) on the
    // first attempt, a clean SSE stream on the second. The provider must ride
    // out the blip and assemble the Turn instead of failing the whole turn.
    test::LoopbackServer srv;
    srv.serve_each(
        {{502, "<html><head><title>502 Bad Gateway</title></head></html>"},
         {200, "data: {\"choices\":[{\"delta\":{\"content\":\"ok\"},"
               "\"finish_reason\":\"stop\"}]}\n\n"
               "data: [DONE]\n\n"}});
    OpenAiConfig cf = cfg();
    cf.base_url = srv.url("");
    OpenAiProvider p(cf);
    Conversation c{Message::user("hi")};
    std::string answer;
    auto t = p.complete_stream(
        c, {}, [&](std::string_view a, std::string_view) { answer.append(a); });
    srv.stop();
    CHECK(t.has_value());
    if (t) CHECK_EQ(t->text, std::string("ok"));
    CHECK_EQ(answer, std::string("ok"));
}

TEST("complete: retries past a transient 500 and recovers") {
    test::LoopbackServer srv;
    srv.serve_each(
        {{500, R"({"error":{"message":"Connection error"}})"},
         {200, R"({"choices":[{"message":{"content":"hi"},)"
               R"("finish_reason":"stop"}]})"}});
    OpenAiConfig cf = cfg();
    cf.base_url = srv.url("");
    OpenAiProvider p(cf);
    Conversation c{Message::user("hi")};
    auto t = p.complete(c, {});
    srv.stop();
    CHECK(t.has_value());
    if (t) CHECK_EQ(t->text, std::string("hi"));
}

TEST("parse_chat_response: captures usage when present") {
    auto body = json::parse(R"({
        "choices":[{"message":{"content":"hi"},"finish_reason":"stop"}],
        "usage":{"prompt_tokens":12,"completion_tokens":3,"total_tokens":15}})")
                    .value();
    auto t = parse_chat_response(body);
    CHECK(t.has_value());
    if (t) {
        CHECK(t->usage.present);
        CHECK_EQ(t->usage.prompt_tokens, 12);
        CHECK_EQ(t->usage.total_tokens, 15);
    }
}

TEST("parse_chat_response: usage absent leaves present false") {
    auto body = json::parse(
        R"({"choices":[{"message":{"content":"hi"},"finish_reason":"stop"}]})")
                    .value();
    auto t = parse_chat_response(body);
    CHECK(t.has_value());
    if (t) CHECK(!t->usage.present);
}

TEST("build_chat_request: streaming asks for usage via stream_options") {
    Conversation c{Message::user("hi")};
    auto req = build_chat_request(cfg(), c, {}, /*stream=*/true);
    CHECK(req.contains("stream_options"));
    if (req.contains("stream_options"))
        CHECK_EQ(req["stream_options"]["include_usage"], true);
}

TEST("build_chat_request: no stream_options when not streaming") {
    Conversation c{Message::user("hi")};
    auto req = build_chat_request(cfg(), c, {});
    CHECK(!req.contains("stream_options"));
}

// --- generation controls (effort / thinking / max_tokens) -------------------

TEST("build_chat_request: reasoning controls omitted by default") {
    Conversation c{Message::user("hi")};
    auto req = build_chat_request(cfg(), c, {});
    CHECK(!req.contains("reasoning_effort"));
    CHECK(!req.contains("thinking"));
    CHECK(!req.contains("max_tokens"));
}

TEST("build_chat_request: reasoning_effort emitted when set") {
    OpenAiConfig c = cfg();
    c.reasoning_effort = "high";
    auto req = build_chat_request(c, Conversation{Message::user("hi")}, {});
    CHECK_EQ(req["reasoning_effort"], std::string("high"));
}

TEST("build_chat_request: thinking toggle emits enabled/disabled") {
    OpenAiConfig c = cfg();
    c.thinking = true;
    auto on = build_chat_request(c, Conversation{Message::user("hi")}, {});
    CHECK_EQ(on["thinking"]["type"], std::string("enabled"));
    c.thinking = false;
    auto off = build_chat_request(c, Conversation{Message::user("hi")}, {});
    CHECK_EQ(off["thinking"]["type"], std::string("disabled"));
}

TEST("build_chat_request: max_tokens emitted only when positive") {
    OpenAiConfig c = cfg();
    c.max_tokens = 2048;
    auto req = build_chat_request(c, Conversation{Message::user("hi")}, {});
    CHECK_EQ(req["max_tokens"], 2048);
}

TEST("openai_model_likely_reasoning: known families vs plain models") {
    CHECK(openai_model_likely_reasoning("deepseek-v4-pro"));
    CHECK(openai_model_likely_reasoning("MiniMax-M3"));
    CHECK(openai_model_likely_reasoning("gpt-5-mini"));
    CHECK(openai_model_likely_reasoning("o3"));
    CHECK(openai_model_likely_reasoning("anthropic/claude-sonnet-4-6"));
    CHECK(!openai_model_likely_reasoning("gpt-4o-mini"));
    CHECK(!openai_model_likely_reasoning("llama-3.3-70b"));
    CHECK(!openai_model_likely_reasoning("mistral-large"));
}

TEST("OpenAiProvider: set_params/params round-trip") {
    OpenAiProvider p(cfg());
    GenerationParams in;
    in.effort = "medium";
    in.temperature = 0.7;
    in.thinking = true;
    in.max_tokens = 4096;
    p.set_params(in);
    GenerationParams out = p.params();
    CHECK(out.effort && *out.effort == "medium");
    CHECK(out.temperature && *out.temperature == 0.7);
    CHECK(out.thinking && *out.thinking == true);
    CHECK(out.max_tokens && *out.max_tokens == 4096);
    // Unset fields in a later call leave existing values intact.
    GenerationParams partial;
    partial.effort = "high";
    p.set_params(partial);
    CHECK(p.params().effort.value_or("") == "high");
    CHECK(p.params().max_tokens.value_or(0) == 4096);  // untouched
}
