#include "agent/gemini_provider.hpp"

#include "agent/json_util.hpp"
#include "agent/stream.hpp"
#include "loopback_server.hpp"
#include "test_harness.hpp"

using namespace moocode;

namespace {
GeminiConfig cfg() {
    GeminiConfig c;
    c.base_url = "https://generativelanguage.googleapis.com/v1beta";
    c.api_key = "KEY";
    c.model = "gemini-test";
    c.temperature = 0.2;
    c.timeout_secs = 30;
    return c;
}
}  // namespace

// --- provider choice / detection / presets ----------------------------------

TEST("parse_provider_choice: recognises gemini / google") {
    CHECK(parse_provider_choice("gemini") == ProviderChoice::Gemini);
    CHECK(parse_provider_choice("Google") == ProviderChoice::Gemini);
    CHECK(parse_provider_choice("anthropic") == ProviderChoice::Anthropic);
    CHECK(parse_provider_choice("openai") == ProviderChoice::OpenAI);
}

TEST("detect_provider_kind: Google host => Gemini, openai shim => OpenAI") {
    CHECK(detect_provider_kind(
              "https://generativelanguage.googleapis.com/v1beta", "") ==
          ProviderKind::Gemini);
    CHECK(detect_provider_kind(
              "https://generativelanguage.googleapis.com/v1beta/openai", "") ==
          ProviderKind::OpenAI);
    CHECK(detect_provider_kind("https://api.anthropic.com/v1", "") ==
          ProviderKind::Anthropic);
    CHECK(detect_provider_kind("https://api.minimax.io/v1", "") ==
          ProviderKind::OpenAI);
}

TEST("provider_kind_name: gemini") {
    CHECK_EQ(std::string(provider_kind_name(ProviderKind::Gemini)),
             std::string("gemini"));
}

TEST("lookup_preset: gemini native + openai shim") {
    auto g = lookup_preset("gemini");
    CHECK(g.has_value());
    if (g) {
        CHECK(g->kind == ProviderKind::Gemini);
        CHECK_EQ(g->model, std::string("gemini-3.5-flash"));
        CHECK(g->base_url.find("generativelanguage") != std::string::npos);
    }
    auto shim = lookup_preset("gemini-openai");
    CHECK(shim.has_value());
    if (shim) {
        CHECK(shim->kind == ProviderKind::OpenAI);
        CHECK(shim->base_url.find("/openai") != std::string::npos);
    }
}

// --- URL construction --------------------------------------------------------

TEST("gemini_generate_url: model in path, stream suffix") {
    CHECK_EQ(gemini_generate_url("https://x/v1beta", "gemini-3.5-flash", false),
             std::string("https://x/v1beta/models/gemini-3.5-flash:generateContent"));
    CHECK_EQ(gemini_generate_url("https://x/v1beta", "gemini-3.5-flash", true),
             std::string("https://x/v1beta/models/gemini-3.5-flash:"
                         "streamGenerateContent?alt=sse"));
    // bare host gets /v1beta appended
    CHECK_EQ(gemini_generate_url("https://x", "m", false),
             std::string("https://x/v1beta/models/m:generateContent"));
    // already-versioned /v1 base used as-is
    CHECK_EQ(gemini_generate_url("https://x/v1", "m", false),
             std::string("https://x/v1/models/m:generateContent"));
}

TEST("gemini_models_url: appends /models, healing bare host") {
    CHECK_EQ(gemini_models_url("https://x/v1beta"), std::string("https://x/v1beta/models"));
    CHECK_EQ(gemini_models_url("https://x"), std::string("https://x/v1beta/models"));
}

TEST("effort_to_thinking_budget: labels map to budgets") {
    CHECK_EQ(effort_to_thinking_budget("minimal"), 1024);
    CHECK_EQ(effort_to_thinking_budget("low"), 2048);
    CHECK_EQ(effort_to_thinking_budget("medium"), 8192);
    CHECK_EQ(effort_to_thinking_budget("high"), 24576);
    CHECK_EQ(effort_to_thinking_budget(""), 8192);
}

// --- request building --------------------------------------------------------

TEST("build_generate_request: system => systemInstruction, user => contents") {
    Conversation c{Message::system("be terse"), Message::user("hi")};
    auto req = build_generate_request(cfg(), c, {});
    CHECK_EQ(req["systemInstruction"]["parts"][0]["text"], std::string("be terse"));
    CHECK_EQ(req["contents"].size(), size_t{1});
    CHECK_EQ(req["contents"][0]["role"], std::string("user"));
    CHECK_EQ(req["contents"][0]["parts"][0]["text"], std::string("hi"));
    CHECK_EQ(req["generationConfig"]["temperature"], 0.2);
    // No tools / thinking unless configured.
    CHECK(!req.contains("tools"));
    CHECK(!req["generationConfig"].contains("thinkingConfig"));
    CHECK(!req["generationConfig"].contains("maxOutputTokens"));
}

TEST("build_generate_request: multiple system messages concatenated") {
    Conversation c{Message::system("a"), Message::system("b"), Message::user("hi")};
    auto req = build_generate_request(cfg(), c, {});
    CHECK_EQ(req["systemInstruction"]["parts"][0]["text"], std::string("a\n\nb"));
}

TEST("build_generate_request: assistant role mapped to model") {
    Conversation c{Message::user("hi"), Message::assistant("hello there")};
    auto req = build_generate_request(cfg(), c, {});
    CHECK_EQ(req["contents"][1]["role"], std::string("model"));
    CHECK_EQ(req["contents"][1]["parts"][0]["text"], std::string("hello there"));
}

TEST("build_generate_request: tools => functionDeclarations") {
    ToolSpec t{.name = "get_weather", .description = "weather",
        .parameters = nlohmann::json{{"type", "object"},
            {"properties", {{"city", {{"type", "string"}}}}}}};
    Conversation c{Message::user("hi")};
    auto req = build_generate_request(cfg(), c, {t});
    auto& decls = req["tools"][0]["functionDeclarations"];
    CHECK_EQ(decls.size(), size_t{1});
    CHECK_EQ(decls[0]["name"], std::string("get_weather"));
    CHECK_EQ(decls[0]["description"], std::string("weather"));
    CHECK_EQ(decls[0]["parameters"]["type"], std::string("object"));
}

TEST("build_generate_request: assistant tool_call => functionCall part, args object") {
    ToolCall tc{.id = "call_1", .name = "get_weather",
        .arguments_json = R"({"city":"Paris"})"};
    Conversation c{Message::user("hi"),
        Message::assistant("", {tc})};
    auto req = build_generate_request(cfg(), c, {});
    auto& fc = req["contents"][1]["parts"][0]["functionCall"];
    CHECK_EQ(fc["name"], std::string("get_weather"));
    CHECK_EQ(fc["id"], std::string("call_1"));
    CHECK(fc["args"].is_object());
    CHECK_EQ(fc["args"]["city"], std::string("Paris"));
}

TEST("build_generate_request: tool result => functionResponse keyed by name") {
    ToolCall tc{.id = "call_1", .name = "get_weather", .arguments_json = "{}"};
    Conversation c{Message::user("hi"), Message::assistant("", {tc}),
        Message::tool("call_1", "18C sunny")};
    auto req = build_generate_request(cfg(), c, {});
    // contents: [user, model(functionCall), user(functionResponse)]
    auto& fr = req["contents"][2]["parts"][0]["functionResponse"];
    CHECK_EQ(req["contents"][2]["role"], std::string("user"));
    CHECK_EQ(fr["name"], std::string("get_weather"));
    CHECK_EQ(fr["id"], std::string("call_1"));
    CHECK_EQ(fr["response"]["result"], std::string("18C sunny"));
}

TEST("build_generate_request: failed tool result => error field") {
    ToolCall tc{.id = "c1", .name = "f", .arguments_json = "{}"};
    Conversation c{Message::user("hi"), Message::assistant("", {tc}),
        Message::tool("c1", "boom", /*failed=*/true)};
    auto req = build_generate_request(cfg(), c, {});
    auto& fr = req["contents"][2]["parts"][0]["functionResponse"];
    CHECK_EQ(fr["response"]["error"], std::string("boom"));
}

TEST("build_generate_request: JSON-object tool result passed through verbatim") {
    ToolCall tc{.id = "c1", .name = "f", .arguments_json = "{}"};
    Conversation c{Message::user("hi"), Message::assistant("", {tc}),
        Message::tool("c1", R"({"temp":18})")};
    auto req = build_generate_request(cfg(), c, {});
    auto& resp = req["contents"][2]["parts"][0]["functionResponse"]["response"];
    CHECK_EQ(resp["temp"], 18);
}

TEST("build_generate_request: consecutive tool results coalesced into one user content") {
    ToolCall a{.id = "a", .name = "fa", .arguments_json = "{}"};
    ToolCall b{.id = "b", .name = "fb", .arguments_json = "{}"};
    Conversation c{Message::user("hi"), Message::assistant("", {a, b}),
        Message::tool("a", "ra"), Message::tool("b", "rb")};
    auto req = build_generate_request(cfg(), c, {});
    CHECK_EQ(req["contents"].size(), size_t{3});  // user, model, user(2 parts)
    auto& parts = req["contents"][2]["parts"];
    CHECK_EQ(parts.size(), size_t{2});
    CHECK_EQ(parts[0]["functionResponse"]["name"], std::string("fa"));
    CHECK_EQ(parts[1]["functionResponse"]["name"], std::string("fb"));
}

// Gemini demands strictly alternating user/model turns. A prompt buffered
// mid-turn lands as a user turn right after the functionResponse user turn, so
// it must merge into one user content (functionResponse + text parts).
TEST("build_generate_request: buffered user message merged after tool-result turn") {
    ToolCall a{.id = "a", .name = "fa", .arguments_json = "{}"};
    Conversation c{Message::user("hi"), Message::assistant("", {a}),
        Message::tool("a", "ra"), Message::user("more")};
    auto req = build_generate_request(cfg(), c, {});
    CHECK_EQ(req["contents"].size(), size_t{3});  // user, model, user(2 parts)
    auto& parts = req["contents"][2]["parts"];
    CHECK_EQ(req["contents"][2]["role"], std::string("user"));
    CHECK_EQ(parts.size(), size_t{2});
    CHECK(parts[0].contains("functionResponse"));
    CHECK_EQ(parts[1]["text"], std::string("more"));
}

TEST("build_generate_request: consecutive user messages merged into one content") {
    Conversation c{Message::user("a"), Message::user("b")};
    auto req = build_generate_request(cfg(), c, {});
    CHECK_EQ(req["contents"].size(), size_t{1});
    CHECK_EQ(req["contents"][0]["role"], std::string("user"));
    CHECK_EQ(req["contents"][0]["parts"].size(), size_t{2});
    CHECK_EQ(req["contents"][0]["parts"][0]["text"], std::string("a"));
    CHECK_EQ(req["contents"][0]["parts"][1]["text"], std::string("b"));
}

TEST("build_generate_request: thinking on => thinkingConfig with budget") {
    GeminiConfig c = cfg();
    c.thinking = true;
    c.reasoning_effort = "high";
    auto req = build_generate_request(c, {Message::user("hi")}, {});
    auto& tk = req["generationConfig"]["thinkingConfig"];
    CHECK_EQ(tk["thinkingBudget"], 24576);
    CHECK_EQ(tk["includeThoughts"], true);
}

TEST("build_generate_request: thinking off => budget 0") {
    GeminiConfig c = cfg();
    c.thinking = false;
    auto req = build_generate_request(c, {Message::user("hi")}, {});
    CHECK_EQ(req["generationConfig"]["thinkingConfig"]["thinkingBudget"], 0);
}

TEST("build_generate_request: max_tokens => maxOutputTokens") {
    GeminiConfig c = cfg();
    c.max_tokens = 555;
    auto req = build_generate_request(c, {Message::user("hi")}, {});
    CHECK_EQ(req["generationConfig"]["maxOutputTokens"], 555);
}

TEST("build_generate_request: user image part => inlineData") {
    std::vector<ContentPart> u_parts;
    u_parts.push_back(ContentPart{"look", std::nullopt});
    u_parts.push_back(ContentPart{"", ImageBlock{"QUJD", "image/png"}});
    Message u = Message::user("look", std::move(u_parts));
    auto req = build_generate_request(cfg(), {u}, {});
    auto& parts = req["contents"][0]["parts"];
    CHECK_EQ(parts.size(), size_t{2});
    CHECK_EQ(parts[0]["text"], std::string("look"));
    CHECK_EQ(parts[1]["inlineData"]["mimeType"], std::string("image/png"));
    CHECK_EQ(parts[1]["inlineData"]["data"], std::string("QUJD"));
}

// --- response parsing --------------------------------------------------------

TEST("parse_generate_response: text + usage") {
    auto body = json::parse(R"({
        "candidates":[{"content":{"role":"model","parts":[{"text":"hello"}]},
                       "finishReason":"STOP"}],
        "usageMetadata":{"promptTokenCount":3,"candidatesTokenCount":2,
                         "thoughtsTokenCount":5,"totalTokenCount":10}})");
    CHECK(body.has_value());
    auto t = parse_generate_response(*body);
    CHECK(t.has_value());
    if (t) {
        CHECK_EQ(t->text, std::string("hello"));
        CHECK_EQ(t->finish_reason, std::string("STOP"));
        CHECK(t->usage.present);
        CHECK_EQ(t->usage.prompt_tokens, 3);
        CHECK_EQ(t->usage.completion_tokens, 7);  // candidates + thoughts
        CHECK_EQ(t->usage.total_tokens, 10);
    }
}

TEST("parse_generate_response: functionCall => ToolCall with serialised args") {
    auto body = json::parse(R"({
        "candidates":[{"content":{"parts":[
            {"functionCall":{"id":"c1","name":"get_weather","args":{"city":"Paris"}}}]}}]})");
    auto t = parse_generate_response(*body);
    CHECK(t.has_value());
    if (t) {
        CHECK_EQ(t->tool_calls.size(), size_t{1});
        CHECK_EQ(t->tool_calls[0].id, std::string("c1"));
        CHECK_EQ(t->tool_calls[0].name, std::string("get_weather"));
        auto args = json::parse(t->tool_calls[0].arguments_json);
        CHECK(args.has_value());
        if (args) CHECK_EQ((*args)["city"], std::string("Paris"));
    }
}

TEST("parse_generate_response: captures part-level thoughtSignature") {
    auto body = json::parse(R"({
        "candidates":[{"content":{"parts":[
            {"functionCall":{"name":"f","args":{}},"thoughtSignature":"SIG123"}]}}]})");
    auto t = parse_generate_response(*body);
    CHECK(t.has_value());
    if (t) {
        CHECK_EQ(t->tool_calls.size(), size_t{1});
        CHECK_EQ(t->tool_calls[0].signature, std::string("SIG123"));
    }
}

TEST("build_generate_request: replays thoughtSignature on the functionCall part") {
    ToolCall tc{.id = "c1", .name = "f", .arguments_json = "{}", .signature = "SIG123"};
    Conversation c{Message::user("hi"), Message::assistant("", {tc})};
    auto req = build_generate_request(cfg(), c, {});
    auto& part = req["contents"][1]["parts"][0];
    CHECK(part.contains("functionCall"));
    CHECK_EQ(part["thoughtSignature"], std::string("SIG123"));
}

TEST("build_generate_request: no thoughtSignature key when signature empty") {
    ToolCall tc{.id = "c1", .name = "f", .arguments_json = "{}"};
    Conversation c{Message::user("hi"), Message::assistant("", {tc})};
    auto req = build_generate_request(cfg(), c, {});
    CHECK(!req["contents"][1]["parts"][0].contains("thoughtSignature"));
}

TEST("GeminiStreamAccumulator: captures part-level thoughtSignature") {
    GeminiStreamAccumulator acc;
    auto j = json::parse(R"({"candidates":[{"content":{"parts":[
        {"functionCall":{"name":"f","args":{}},"thoughtSignature":"SIGX"}]}}]})");
    acc.ingest(*j);
    Turn t = acc.finish();
    CHECK_EQ(t.tool_calls.size(), size_t{1});
    if (!t.tool_calls.empty()) CHECK_EQ(t.tool_calls[0].signature, std::string("SIGX"));
}

TEST("parse_generate_response: thought parts excluded from text") {
    auto body = json::parse(R"({
        "candidates":[{"content":{"parts":[
            {"text":"reasoning...","thought":true},
            {"text":"answer"}]}}]})");
    auto t = parse_generate_response(*body);
    CHECK(t.has_value());
    if (t) CHECK_EQ(t->text, std::string("answer"));
}

TEST("parse_generate_response: API error object => Error") {
    auto body = json::parse(R"({"error":{"code":400,"message":"bad key"}})");
    auto t = parse_generate_response(*body);
    CHECK(!t.has_value());
    if (!t) CHECK(t.error().msg.find("bad key") != std::string::npos);
}

TEST("parse_generate_response: empty candidates => Error not crash") {
    auto body = json::parse(R"({"candidates":[]})");
    auto t = parse_generate_response(*body);
    CHECK(!t.has_value());
}

// --- usage parser ------------------------------------------------------------

TEST("parse_gemini_usage: total falls back to sum when absent") {
    auto u = json::parse(R"({"promptTokenCount":3,"candidatesTokenCount":4})");
    Usage out = parse_gemini_usage(*u);
    CHECK(out.present);
    CHECK_EQ(out.prompt_tokens, 3);
    CHECK_EQ(out.completion_tokens, 4);
    CHECK_EQ(out.total_tokens, 7);
}

TEST("parse_gemini_usage: non-object => not present") {
    CHECK(!parse_gemini_usage(nlohmann::json(5)).present);
}

// --- model listing -----------------------------------------------------------

TEST("parse_gemini_model_ids: strips prefix, filters by generateContent") {
    auto body = json::parse(R"({"models":[
        {"name":"models/gemini-3.5-flash","supportedGenerationMethods":["generateContent"]},
        {"name":"models/embedding-001","supportedGenerationMethods":["embedContent"]},
        {"name":"models/gemini-3-flash"}]})");
    auto ids = parse_gemini_model_ids(*body);
    CHECK_EQ(ids.size(), size_t{2});  // flash + the field-absent one (default keep)
    CHECK_EQ(ids[0], std::string("gemini-3.5-flash"));
    CHECK_EQ(ids[1], std::string("gemini-3-flash"));
}

// --- streaming accumulator ---------------------------------------------------

TEST("GeminiStreamAccumulator: text deltas concatenate, thought => reasoning") {
    GeminiStreamAccumulator acc;
    std::string answer, reasoning;
    auto feed = [&](const char* json_text) {
        auto j = json::parse(json_text);
        auto a = acc.ingest(*j);
        answer += a.answer;
        reasoning += a.reasoning;
    };
    feed(R"({"candidates":[{"content":{"parts":[{"text":"think","thought":true}]}}]})");
    feed(R"({"candidates":[{"content":{"parts":[{"text":"Hel"}]}}]})");
    feed(R"({"candidates":[{"content":{"parts":[{"text":"lo"}],"role":"model"},
             "finishReason":"STOP"}],
             "usageMetadata":{"promptTokenCount":1,"candidatesTokenCount":2}})");
    Turn t = acc.finish();
    CHECK_EQ(answer, std::string("Hello"));
    CHECK_EQ(reasoning, std::string("think"));
    CHECK_EQ(t.text, std::string("Hello"));
    CHECK_EQ(t.finish_reason, std::string("STOP"));
    CHECK(t.usage.present);
}

TEST("GeminiStreamAccumulator: functionCall assembled as a tool call") {
    GeminiStreamAccumulator acc;
    auto j = json::parse(R"({"candidates":[{"content":{"parts":[
        {"functionCall":{"name":"f","args":{"x":1}}}]}}]})");
    acc.ingest(*j);
    Turn t = acc.finish();
    CHECK_EQ(t.tool_calls.size(), size_t{1});
    CHECK_EQ(t.tool_calls[0].name, std::string("f"));
    auto args = json::parse(t.tool_calls[0].arguments_json);
    if (args) CHECK_EQ((*args)["x"], 1);
}

TEST("GeminiStreamAccumulator: usage-only / candidate-less chunk tolerated") {
    GeminiStreamAccumulator acc;
    auto j = json::parse(R"({"usageMetadata":{"promptTokenCount":9,"candidatesTokenCount":1}})");
    auto a = acc.ingest(*j);
    CHECK(a.answer.empty());
    Turn t = acc.finish();
    CHECK(t.usage.present);
    CHECK_EQ(t.usage.prompt_tokens, 9);
}

// --- integration over loopback ----------------------------------------------

TEST("complete: round-trips and parses a Turn from :generateContent") {
    test::LoopbackServer srv;
    srv.serve(200, R"({"candidates":[{"content":{"parts":[{"text":"done"}]},
        "finishReason":"STOP"}]})");
    GeminiConfig cf = cfg();
    cf.base_url = srv.url("");  // provider appends /v1beta/models/...:generateContent
    GeminiProvider p(cf);
    auto t = p.complete({Message::user("hi")}, {});
    srv.stop();
    CHECK(t.has_value());
    if (t) CHECK_EQ(t->text, std::string("done"));
    CHECK(srv.last_request().find(":generateContent") != std::string::npos);
    CHECK(srv.last_request().find("x-goog-api-key: KEY") != std::string::npos);
}

TEST("complete: HTTP 400 surfaces as Error with body message") {
    test::LoopbackServer srv;
    srv.serve(400, R"({"error":{"message":"invalid key"}})");
    GeminiConfig cf = cfg();
    cf.base_url = srv.url("");
    GeminiProvider p(cf);
    auto t = p.complete({Message::user("hi")}, {});
    srv.stop();
    CHECK(!t.has_value());
    if (!t) CHECK(t.error().msg.find("invalid key") != std::string::npos);
}

TEST("complete_stream: assembles a Turn from SSE chunks, posts stream URL") {
    test::LoopbackServer srv;
    srv.serve(200,
        "data: {\"candidates\":[{\"content\":{\"parts\":[{\"text\":\"Hel\"}]}}]}\n\n"
        "data: {\"candidates\":[{\"content\":{\"parts\":[{\"text\":\"lo\"}]},"
        "\"finishReason\":\"STOP\"}],\"usageMetadata\":{\"promptTokenCount\":1,"
        "\"candidatesTokenCount\":2}}\n\n");
    GeminiConfig cf = cfg();
    cf.base_url = srv.url("");
    GeminiProvider p(cf);
    std::string answer;
    auto t = p.complete_stream({Message::user("hi")}, {},
        [&](std::string_view a, std::string_view) { answer.append(a); });
    srv.stop();
    CHECK(t.has_value());
    if (t) {
        CHECK_EQ(t->text, std::string("Hello"));
        CHECK_EQ(t->finish_reason, std::string("STOP"));
    }
    CHECK_EQ(answer, std::string("Hello"));
    CHECK(srv.last_request().find(":streamGenerateContent") != std::string::npos);
}
