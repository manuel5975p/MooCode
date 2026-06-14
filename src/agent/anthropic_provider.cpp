#include "agent/anthropic_provider.hpp"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <utility>

#include "agent/http.hpp"
#include "agent/json_util.hpp"
#include "agent/openai_provider.hpp"  // parse_model_ids
#include "agent/provider_factory.hpp"  // ProviderConnection (for AnthropicConfig ctor)
#include "agent/stream.hpp"
#include "agent/strutil.hpp"

namespace moocode {

namespace {

// Parse a tool-call argument string into a JSON object for the "input" field.
// Anthropic wants a structured object; an empty or malformed string degrades to
// an empty object rather than failing the whole request.
nlohmann::json input_object(const std::string& arguments_json) {
    if (arguments_json.empty()) return nlohmann::json::object();
    auto v = json::parse(arguments_json);
    if (!v || !v->is_object()) return nlohmann::json::object();
    return *v;
}

// Serialise one assistant turn's content as Anthropic content blocks: a leading
// text block (only when non-empty) followed by one tool_use block per call.
nlohmann::json assistant_content(const Message& m) {
    nlohmann::json blocks = nlohmann::json::array();
    if (!m.content().empty())
        blocks.push_back({{"type", "text"}, {"text", m.content()}});
    for (const auto& tc : m.tool_calls())
        blocks.push_back({{"type", "tool_use"},
                          {"id", tc.id},
                          {"name", tc.name},
                          {"input", input_object(tc.arguments_json)}});
    return blocks;
}

}  // namespace

std::string effort_to_output_effort(std::string_view effort) {
    const std::string e = to_lower(effort);
    if (e == "minimal") return "low";            // no API "minimal" tier
    if (e == "low" || e == "medium" || e == "high" || e == "xhigh" ||
        e == "max")
        return e;                                // valid output_config.effort
    return {};                                   // unrecognised => model default
}

std::string anthropic_messages_url(std::string_view base) {
    std::string b(base);
    constexpr std::string_view kV1 = "/v1";
    if (b.size() >= kV1.size() &&
        b.compare(b.size() - kV1.size(), kV1.size(), kV1) == 0)
        return b + "/messages";       // base already versioned: ".../v1/messages"
    return b + "/v1/messages";        // bare base: append the full path
}

std::string anthropic_models_url(std::string_view base) {
    std::string b(base);
    constexpr std::string_view kV1 = "/v1";
    if (b.size() >= kV1.size() &&
        b.compare(b.size() - kV1.size(), kV1.size(), kV1) == 0)
        return b + "/models";         // base already versioned: ".../v1/models"
    return b + "/v1/models";          // bare base: append the full path
}

const char* provider_kind_name(ProviderKind k) {
    switch (k) {
        case ProviderKind::Anthropic: return "anthropic";
        case ProviderKind::Gemini: return "gemini";
        case ProviderKind::OpenAI: break;
    }
    return "openai";
}

std::optional<ProviderPreset> lookup_preset(std::string_view name) {
    const std::string n = to_lower(name);
    if (n == "minimax")
        return ProviderPreset{.kind = ProviderKind::OpenAI,
            .base_url = "https://api.minimax.io/v1",
            .model = "MiniMax-M3"};
    if (n == "deepseek" || n == "deepseek-pro")
        return ProviderPreset{.kind = ProviderKind::OpenAI,
            .base_url = "https://api.deepseek.com/v1",
            .model = "deepseek-v4-pro"};
    if (n == "deepseek-flash")
        return ProviderPreset{.kind = ProviderKind::OpenAI,
            .base_url = "https://api.deepseek.com/v1",
            .model = "deepseek-v4-flash"};
    // Google Gemini, native Generative Language API.
    if (n == "gemini" || n == "google")
        return ProviderPreset{.kind = ProviderKind::Gemini,
            .base_url = "https://generativelanguage.googleapis.com/v1beta",
            .model = "gemini-3.5-flash"};
    // Gemini via its OpenAI-compatibility shim, for callers who prefer the
    // OpenAI wire format (fewer native features, but reuses that code path).
    if (n == "gemini-openai")
        return ProviderPreset{.kind = ProviderKind::OpenAI,
            .base_url = "https://generativelanguage.googleapis.com/v1beta/openai",
            .model = "gemini-3.5-flash"};
    return std::nullopt;
}

ProviderChoice parse_provider_choice(std::string_view s) {
    const std::string v = to_lower(s);
    if (v == "anthropic" || v == "claude") return ProviderChoice::Anthropic;
    if (v == "openai" || v == "oai" || v == "chat" || v == "gpt")
        return ProviderChoice::OpenAI;
    if (v == "gemini" || v == "google") return ProviderChoice::Gemini;
    return ProviderChoice::Auto;  // "", "auto", or anything unrecognised
}

ProviderKind detect_provider_kind(std::string_view base_url,
                                  std::string_view /*model*/) {
    const std::string b = to_lower(base_url);
    if (b.find("anthropic") != std::string::npos)
        return ProviderKind::Anthropic;
    // Google's Generative Language API host. The OpenAI-compat shim lives under
    // ".../v1beta/openai" on the same host; route that to OpenAI instead.
    if (b.find("generativelanguage") != std::string::npos ||
        b.find("googleapis") != std::string::npos)
        return b.find("/openai") != std::string::npos ? ProviderKind::OpenAI
                                                       : ProviderKind::Gemini;
    return ProviderKind::OpenAI;
}

nlohmann::json build_messages_request(const AnthropicConfig& cfg,
                                      const Conversation& conv,
                                      const std::vector<ToolSpec>& tools,
                                      bool stream) {
    nlohmann::json req;
    req["model"] = cfg.model;
    const int max_tok = std::max(1, cfg.max_tokens);  // floor: 0/negative misconfig
    req["max_tokens"] = max_tok;
    // temperature is opt-in: Opus 4.7+/Fable 5 reject any sampling param with a
    // 400, so emit it only when explicitly set (and never alongside thinking,
    // which forces its own value below).
    if (cfg.temperature) req["temperature"] = *cfg.temperature;
    if (stream) req["stream"] = true;

    // Extended thinking: enabled when explicitly requested, or implied by a set
    // effort. Current Claude models (Opus 4.6+/4.7/4.8, Fable 5) take adaptive
    // thinking — the fixed {type:"enabled", budget_tokens} shape and any
    // sampling param are hard 400s. Depth is steered via output_config.effort;
    // request a summarized reasoning stream so the TUI has thinking text to show.
    const bool thinking_on =
        cfg.thinking.value_or(!cfg.reasoning_effort.empty());
    if (thinking_on) {
        req["thinking"] = {{"type", "adaptive"}, {"display", "summarized"}};
        req.erase("temperature");  // rejected alongside thinking
        if (const std::string e = effort_to_output_effort(cfg.reasoning_effort);
            !e.empty())
            req["output_config"] = {{"effort", e}};
    }

    // System messages are hoisted into the top-level "system" field. Multiple
    // (rare) are joined with a blank line so none is dropped.
    std::string system;
    for (const auto& m : conv) {
        if (m.role() != Role::System || m.content().empty()) continue;
        if (!system.empty()) system += "\n\n";
        system += m.content();
    }
    if (!system.empty()) req["system"] = system;

    nlohmann::json messages = nlohmann::json::array();
    for (std::size_t i = 0; i < conv.size();) {
        const Message& m = conv[i];
        if (m.role() == Role::System) {  // already hoisted
            ++i;
            continue;
        }
        if (m.role() == Role::Tool) {
            // Coalesce this and all immediately following tool results into one
            // user message of tool_result blocks (Anthropic's required shape).
            nlohmann::json blocks = nlohmann::json::array();
            for (; i < conv.size() && conv[i].role() == Role::Tool; ++i) {
                nlohmann::json block{{"type", "tool_result"},
                                     {"tool_use_id", conv[i].tool_call_id()},
                                     {"content", conv[i].content()}};
                if (conv[i].tool_failed()) block["is_error"] = true;
                blocks.push_back(std::move(block));
            }
            messages.push_back({{"role", "user"}, {"content", std::move(blocks)}});
            continue;
        }
        if (m.role() == Role::Assistant) {
            messages.push_back(
                {{"role", "assistant"}, {"content", assistant_content(m)}});
            ++i;
            continue;
        }
        // Role::User — string content or multimodal content blocks.
        if (!m.parts().empty()) {
            nlohmann::json blocks = nlohmann::json::array();
            for (const auto& p : m.parts()) {
                if (!p.text.empty())
                    blocks.push_back({{"type", "text"}, {"text", p.text}});
                if (p.image) {
                    blocks.push_back({{"type", "image"},
                                      {"source",
                                       {{"type", "base64"},
                                        {"media_type", p.image->media_type},
                                        {"data", p.image->base64_data}}}});
                }
            }
            messages.push_back({{"role", "user"}, {"content", std::move(blocks)}});
        } else {
            messages.push_back({{"role", "user"}, {"content", m.content()}});
        }
        ++i;
    }
    req["messages"] = std::move(messages);

    if (!tools.empty()) {
        nlohmann::json specs = nlohmann::json::array();
        for (const auto& t : tools)
            specs.push_back({{"name", t.name},
                             {"description", t.description},
                             {"input_schema", t.parameters}});
        req["tools"] = std::move(specs);
    }
    // Debug: when MOOCODE_DEBUG_REQUEST is set, append a sanitised copy of the
    // outgoing request (image base64 replaced by its length) so we can confirm
    // whether image blocks actually reach the wire without dumping megabytes.
    if (const char* dbg = std::getenv("MOOCODE_DEBUG_REQUEST")) {
        nlohmann::json san = req;
        if (san.contains("messages"))
            for (auto& msg : san["messages"])
                if (msg.contains("content") && msg["content"].is_array())
                    for (auto& blk : msg["content"])
                        if (blk.value("type", std::string{}) == "image" &&
                            blk.contains("source") &&
                            blk["source"].contains("data"))
                            blk["source"]["data"] =
                                "<base64 " +
                                std::to_string(
                                    blk["source"]["data"].get<std::string>().size()) +
                                " chars>";
        const std::string path = (dbg[0] != '\0') ? std::string(dbg)
                                                   : std::string("/tmp/moocode_request.json");
        std::ofstream(path, std::ios::app) << san.dump(2) << "\n=====\n";
    }
    return req;
}

std::expected<Turn, Error> parse_messages_response(const nlohmann::json& body) {
    if (!body.is_object())
        return std::unexpected(Error{.msg = "response is not a JSON object", .code = 0});

    // API-level error object short-circuits (also the shape of an "error" type).
    if (auto it = body.find("error"); it != body.end()) {
        std::string msg = "API error";
        if (it->is_object() && it->contains("message") &&
            (*it)["message"].is_string())
            msg = (*it)["message"].get<std::string>();
        else
            msg = it->dump();
        return std::unexpected(Error{.msg = msg, .code = 0});
    }

    auto content = body.find("content");
    if (content == body.end() || !content->is_array())
        return std::unexpected(Error{.msg = "response has no content array", .code = 0});

    Turn turn;
    for (const auto& block : *content) {
        if (!block.is_object()) continue;
        const auto t = block.find("type");
        const std::string bt = (t != block.end() && t->is_string())
                                   ? t->get<std::string>()
                                   : std::string();
        if (bt == "text") {
            if (auto x = block.find("text"); x != block.end() && x->is_string())
                turn.text += x->get<std::string>();
        } else if (bt == "tool_use") {
            ToolCall call;
            if (auto x = block.find("id"); x != block.end() && x->is_string())
                call.id = x->get<std::string>();
            if (auto x = block.find("name"); x != block.end() && x->is_string())
                call.name = x->get<std::string>();
            if (auto x = block.find("input"); x != block.end() && x->is_object())
                call.arguments_json = x->dump();
            else
                call.arguments_json = "{}";
            turn.tool_calls.push_back(std::move(call));
        } else if (bt == "thinking") {
            // Display/audit only — captured on Turn.reasoning, never in text.
            if (auto x = block.find("thinking"); x != block.end() && x->is_string())
                turn.reasoning += x->get<std::string>();
        }
    }

    if (auto it = body.find("stop_reason");
        it != body.end() && it->is_string())
        turn.finish_reason = it->get<std::string>();
    if (auto it = body.find("usage"); it != body.end() && it->is_object())
        turn.usage = parse_anthropic_usage(*it);

    if (turn.text.empty() && turn.tool_calls.empty()) {
        const std::string fr =
            turn.finish_reason.empty() ? "unknown" : turn.finish_reason;
        return std::unexpected(
            Error{.msg = "model returned no content (finish_reason: " + fr + ")", .code = 0});
    }
    return turn;
}

AnthropicConfig::AnthropicConfig(const ProviderConnection& c)
    : base_url(c.base_url), api_key(c.api_key), model(c.model),
      max_tokens(c.max_tokens) {}

AnthropicProvider::AnthropicProvider(AnthropicConfig cfg) : cfg_(std::move(cfg)) {}

void AnthropicProvider::set_params(const GenerationParams& p) {
    if (p.effort) cfg_.reasoning_effort = *p.effort;
    if (p.temperature) cfg_.temperature = *p.temperature;
    if (p.thinking) cfg_.thinking = *p.thinking;
    if (p.max_tokens && *p.max_tokens > 0) cfg_.max_tokens = *p.max_tokens;
}

GenerationParams AnthropicProvider::params() const {
    GenerationParams p;
    if (!cfg_.reasoning_effort.empty()) p.effort = cfg_.reasoning_effort;
    p.temperature = cfg_.temperature;
    p.thinking = cfg_.thinking;
    p.max_tokens = cfg_.max_tokens;
    return p;
}

void AnthropicProvider::set_model(std::string m) { provider_set_model(cfg_.model, std::move(m)); }

std::string AnthropicProvider::model() const { return provider_get_model(cfg_.model); }

void AnthropicProvider::set_base_url(std::string url) { provider_set_base_url(cfg_.base_url, std::move(url)); }

std::string AnthropicProvider::base_url() const { return provider_get_base_url(cfg_.base_url); }

void AnthropicProvider::set_api_key(std::string key) { provider_set_api_key(cfg_.api_key, std::move(key)); }

std::vector<std::string> AnthropicProvider::headers() const {
    std::vector<std::string> h;
    if (!cfg_.api_key.empty()) h.push_back("x-api-key: " + cfg_.api_key);
    h.push_back("anthropic-version: " + cfg_.anthropic_version);
    return h;
}

std::expected<std::vector<std::string>, Error> AnthropicProvider::list_models() {
    std::string url = anthropic_models_url(cfg_.base_url);
    auto resp = http::get(url, headers(), cfg_.timeout_secs);
    if (!resp) return std::unexpected(resp.error());

    auto parsed = json::parse(resp->body);
    if (!parsed) {
        std::string snippet = resp->body.substr(0, 200);
        return std::unexpected(Error{.msg = "HTTP " + std::to_string(resp->status) +
                                         ": non-JSON response: " + snippet,
            .code = static_cast<int>(resp->status)});
    }
    if (resp->status < 200 || resp->status >= 300) {
        std::string detail;
        if (auto e = parsed->find("error");
            e != parsed->end() && e->is_object()) {
            if (auto m = e->find("message"); m != e->end() && m->is_string())
                detail = ": " + m->get<std::string>();
        }
        return std::unexpected(Error{.msg = "HTTP " + std::to_string(resp->status) +
                                         detail,
            .code = static_cast<int>(resp->status)});
    }
    // Anthropic's /v1/models uses the same {data:[{id:…}]} shape as OpenAI.
    return parse_model_ids(*parsed);
}

std::expected<Turn, Error> AnthropicProvider::complete(
    const Conversation& conversation, const std::vector<ToolSpec>& tools) {
    nlohmann::json req = build_messages_request(cfg_, conversation, tools);
    std::string url = anthropic_messages_url(cfg_.base_url);

    auto resp = http::post_json(url, headers(), json::dump(req), cfg_.timeout_secs);
    if (!resp) return std::unexpected(resp.error());

    auto parsed = json::parse(resp->body);
    if (!parsed) {
        std::string snippet = resp->body.substr(0, 200);
        return std::unexpected(Error{.msg = "HTTP " + std::to_string(resp->status) +
                                         ": non-JSON response: " + snippet,
            .code = static_cast<int>(resp->status)});
    }

    auto turn = parse_messages_response(*parsed);
    if (!turn) {
        if (resp->status < 200 || resp->status >= 300)
            return std::unexpected(Error{.msg = "HTTP " + std::to_string(resp->status) +
                                             ": " + turn.error().msg,
                .code = static_cast<int>(resp->status)});
        return std::unexpected(turn.error());
    }
    return turn;
}

std::expected<Turn, Error> AnthropicProvider::complete_stream(
    const Conversation& conversation, const std::vector<ToolSpec>& tools,
    const StreamFn& on_delta, const std::atomic<bool>* cancel) {
    nlohmann::json req =
        build_messages_request(cfg_, conversation, tools, /*stream=*/true);
    std::string url = anthropic_messages_url(cfg_.base_url);

    AnthropicStreamAccumulator acc;
    std::string buffer;  // SSE line reassembly across network chunks
    std::string raw;     // whole body, retained only for the error path
    bool done = false;

    auto on_chunk = [&](std::string_view bytes) {
        raw.append(bytes);
        buffer.append(bytes);
        std::vector<std::string> events;
        parse_sse_chunk(buffer, events, done);
        for (const auto& ev : events) {
            auto parsed = json::parse(ev);
            if (!parsed) continue;  // skip a malformed event, keep streaming
            AnthropicStreamAccumulator::Added a = acc.ingest(*parsed);
            if (on_delta && (!a.answer.empty() || !a.reasoning.empty()))
                on_delta(a.answer, a.reasoning);
        }
    };

    auto status = http::post_json_stream(url, headers(), json::dump(req),
                                         on_chunk, cancel, cfg_.timeout_secs);
    if (!status) return std::unexpected(status.error());

    // Non-2xx: the server returns a JSON error object, not an SSE stream.
    if (*status < 200 || *status >= 300) {
        std::string detail;
        if (auto parsed = json::parse(raw); parsed) {
            if (auto err = parse_messages_response(*parsed); !err)
                detail = err.error().msg;
        }
        if (detail.empty()) detail = raw.substr(0, 200);
        return std::unexpected(Error{.msg = "HTTP " + std::to_string(*status) + ": " +
                                         detail,
            .code = static_cast<int>(*status)});
    }
    // An "error" event can arrive under a 200 status mid-stream.
    if (acc.error())
        return std::unexpected(Error{.msg = "anthropic stream error: " + *acc.error(), .code = 0});
    return acc.finish();
}

}  // namespace moocode
