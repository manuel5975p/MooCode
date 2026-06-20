#include "agent/openai_provider.hpp"

#include <utility>

#include "agent/http.hpp"
#include "agent/json_util.hpp"
#include "agent/provider_factory.hpp"  // ProviderConnection (for OpenAiConfig ctor)
#include "agent/stream.hpp"
#include "agent/strutil.hpp"

namespace moocode {

namespace {

// Serialize one conversation message into an OpenAI chat message object.
// When the message carries ContentParts, `content` becomes an array of
// text + image_url blocks so multimodal models see the images.
nlohmann::json message_json(const Message& m) {
    nlohmann::json j;
    j["role"] = role_str(m.role());
    if (m.role() == Role::Tool) j["tool_call_id"] = m.tool_call_id();
    if (m.role() == Role::Assistant && !m.tool_calls().empty()) {
        nlohmann::json calls = nlohmann::json::array();
        for (const auto& tc : m.tool_calls()) {
            calls.push_back({{"id", tc.id},
                             {"type", "function"},
                             {"function",
                              {{"name", tc.name}, {"arguments", tc.arguments_json}}}});
        }
        j["tool_calls"] = std::move(calls);
    }

    if (m.role() == Role::User && !m.parts().empty()) {
        // Multimodal user message: content is an array of parts.
        nlohmann::json content = nlohmann::json::array();
        for (const auto& p : m.parts()) {
            if (!p.text.empty())
                content.push_back({{"type", "text"}, {"text", p.text}});
            if (p.image) {
                std::string data_url = "data:" + p.image->media_type +
                                       ";base64," + p.image->base64_data;
                content.push_back(
                    {{"type", "image_url"},
                     {"image_url", {{"url", std::move(data_url)}}}});
            }
        }
        j["content"] = std::move(content);
    } else {
        j["content"] = m.content();
    }
    return j;
}

}  // namespace

nlohmann::json build_chat_request(const OpenAiConfig& cfg, const Conversation& conv,
                                  const std::vector<ToolSpec>& tools, bool stream) {
    nlohmann::json req;
    req["model"] = cfg.model;
    req["temperature"] = cfg.temperature;
    if (cfg.max_tokens > 0) req["max_tokens"] = cfg.max_tokens;
    // Optional reasoning controls — emitted only when explicitly configured so
    // strict OpenAI-compatible servers that reject unknown fields stay happy.
    if (!cfg.reasoning_effort.empty())
        req["reasoning_effort"] = cfg.reasoning_effort;
    if (cfg.thinking)  // DeepSeek/MiniMax convention: thinking:{type:enabled|disabled|adaptive}
        req["thinking"]["type"] = *cfg.thinking ? cfg.thinking_type : "disabled";
    if (stream) {
        req["stream"] = true;
        req["stream_options"]["include_usage"] = true;
    }

    nlohmann::json messages = nlohmann::json::array();
    for (const auto& m : conv) messages.push_back(message_json(m));
    // Merge consecutive user messages into one. A prompt buffered mid-turn can
    // be injected right after another user turn (e.g. before the first request);
    // collapsing them to a single content array keeps strict servers that reject
    // back-to-back user roles happy. Tool/assistant messages are never merged.
    {
        auto to_array = [](const nlohmann::json& c) {
            if (c.is_array()) return c;
            nlohmann::json arr = nlohmann::json::array();
            if (c.is_string() && !c.get<std::string>().empty())
                arr.push_back({{"type", "text"}, {"text", c}});
            return arr;
        };
        nlohmann::json out = nlohmann::json::array();
        for (auto& m : messages) {
            if (!out.empty() && out.back().value("role", "") == "user" &&
                m.value("role", "") == "user") {
                nlohmann::json arr = to_array(out.back()["content"]);
                for (auto& blk : to_array(m["content"])) arr.push_back(std::move(blk));
                out.back()["content"] = std::move(arr);
            } else {
                out.push_back(std::move(m));
            }
        }
        messages = std::move(out);
    }
    req["messages"] = std::move(messages);

    if (!tools.empty()) {
        nlohmann::json specs = nlohmann::json::array();
        for (const auto& t : tools) {
            specs.push_back({{"type", "function"},
                             {"function",
                              {{"name", t.name},
                               {"description", t.description},
                               {"parameters", t.parameters}}}});
        }
        req["tools"] = std::move(specs);
    }
    return req;
}

std::expected<Turn, Error> parse_chat_response(const nlohmann::json& body) {
    if (!body.is_object())
        return std::unexpected(Error{.msg = "response is not a JSON object", .code = 0});

    // API-level error object short-circuits.
    if (auto it = body.find("error"); it != body.end()) {
        std::string msg = "API error";
        if (it->is_object() && it->contains("message") && (*it)["message"].is_string())
            msg = (*it)["message"].get<std::string>();
        else
            msg = it->dump();
        return std::unexpected(Error{.msg = msg, .code = 0});
    }

    auto choices = json::get_array(body, "choices");
    if (!choices) return std::unexpected(choices.error());
    if ((*choices)->empty())
        return std::unexpected(Error{.msg = "response has no choices", .code = 0});

    const nlohmann::json& choice = (**choices)[0];
    if (!choice.is_object() || !choice.contains("message"))
        return std::unexpected(Error{.msg = "choice missing message", .code = 0});
    const nlohmann::json& msg = choice["message"];

    Turn turn;
    if (auto it = msg.find("content"); it != msg.end() && it->is_string())
        turn.text = it->get<std::string>();
    // reasoning_content: display/audit only (some reasoning models emit it).
    if (auto it = msg.find("reasoning_content");
        it != msg.end() && it->is_string())
        turn.reasoning = it->get<std::string>();

    if (auto it = choice.find("finish_reason");
        it != choice.end() && it->is_string())
        turn.finish_reason = it->get<std::string>();

    if (auto it = msg.find("tool_calls"); it != msg.end() && it->is_array()) {
        for (const auto& tc : *it) {
            if (!tc.is_object() || !tc.contains("function"))
                return std::unexpected(Error{.msg = "malformed tool_call", .code = 0});
            const nlohmann::json& fn = tc["function"];
            ToolCall call;
            if (tc.contains("id") && tc["id"].is_string())
                call.id = tc["id"].get<std::string>();
            if (fn.contains("name") && fn["name"].is_string())
                call.name = fn["name"].get<std::string>();
            if (fn.contains("arguments") && fn["arguments"].is_string())
                call.arguments_json = fn["arguments"].get<std::string>();
            turn.tool_calls.push_back(std::move(call));
        }
    }
    if (auto it = body.find("usage"); it != body.end() && it->is_object())
        turn.usage = parse_usage(*it);

    if (turn.text.empty() && turn.tool_calls.empty()) {
        const std::string fr =
            turn.finish_reason.empty() ? "unknown" : turn.finish_reason;
        return std::unexpected(
            Error{.msg = "model returned no content (finish_reason: " + fr + ")", .code = 0});
    }
    return turn;
}

OpenAiConfig::OpenAiConfig(const ProviderConnection& c)
    : base_url(c.base_url), api_key(c.api_key), model(c.model),
      thinking_type(c.thinking_type.empty() ? "enabled" : c.thinking_type),
      max_tokens(c.max_tokens) {}

OpenAiProvider::OpenAiProvider(OpenAiConfig cfg) : cfg_(std::move(cfg)) {}

void OpenAiProvider::set_params(const GenerationParams& p) {
    if (p.effort) cfg_.reasoning_effort = *p.effort;
    if (p.temperature) cfg_.temperature = *p.temperature;
    if (p.thinking) cfg_.thinking = *p.thinking;
    if (p.max_tokens) cfg_.max_tokens = *p.max_tokens;
}

GenerationParams OpenAiProvider::params() const {
    GenerationParams p;
    if (!cfg_.reasoning_effort.empty()) p.effort = cfg_.reasoning_effort;
    p.temperature = cfg_.temperature;
    p.thinking = cfg_.thinking;
    if (cfg_.max_tokens > 0) p.max_tokens = cfg_.max_tokens;
    return p;
}

void OpenAiProvider::set_model(std::string m) { provider_set_model(cfg_.model, std::move(m)); }

std::string OpenAiProvider::model() const { return provider_get_model(cfg_.model); }

void OpenAiProvider::set_base_url(std::string url) { provider_set_base_url(cfg_.base_url, std::move(url)); }

std::string OpenAiProvider::base_url() const { return provider_get_base_url(cfg_.base_url); }

void OpenAiProvider::set_api_key(std::string key) { provider_set_api_key(cfg_.api_key, std::move(key)); }

void OpenAiProvider::set_thinking_type(std::string type) {
    // Mirror the constructor: an empty type means "use the backend default".
    cfg_.thinking_type = type.empty() ? "enabled" : std::move(type);
}

std::expected<std::vector<std::string>, Error> OpenAiProvider::list_models() {
    std::string url = openai_models_url(cfg_.base_url);
    std::vector<std::string> headers;
    if (!cfg_.api_key.empty())
        headers.push_back("Authorization: Bearer " + cfg_.api_key);

    auto resp = http::get(url, headers, cfg_.timeout_secs);
    if (!resp) return std::unexpected(resp.error());

    auto parsed = json::parse(resp->body);
    if (!parsed) {
        std::string snippet = resp->body.substr(0, 200);
        return std::unexpected(Error{.msg = "HTTP " + std::to_string(resp->status) +
                                         ": non-JSON response: " + snippet,
            .code = static_cast<int>(resp->status)});
    }
    if (resp->status < 200 || resp->status >= 300) {
        // Surface an API error message when present, else just the status.
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
    return json::guard_or([&] { return parse_model_ids(*parsed); },
                          std::vector<std::string>{});
}

std::expected<Turn, Error> OpenAiProvider::complete(
    const Conversation& conversation, const std::vector<ToolSpec>& tools) {
    nlohmann::json req = build_chat_request(cfg_, conversation, tools);
    std::string url = openai_chat_completions_url(cfg_.base_url);

    std::vector<std::string> headers;
    if (!cfg_.api_key.empty())
        headers.push_back("Authorization: Bearer " + cfg_.api_key);

    auto resp = http::post_json(url, headers, json::dump(req), cfg_.timeout_secs);
    if (!resp) return std::unexpected(resp.error());

    auto parsed = json::parse(resp->body);
    if (!parsed) {
        // Non-JSON body (e.g. proxy HTML error page): include status + snippet.
        std::string snippet = resp->body.substr(0, 200);
        return std::unexpected(Error{.msg = "HTTP " + std::to_string(resp->status) +
                                         ": non-JSON response: " + snippet,
            .code = static_cast<int>(resp->status)});
    }

    auto turn = json::guard("openai response",
                            [&] { return parse_chat_response(*parsed); });
    if (!turn) {
        // Attach HTTP status for non-2xx so auth/quota errors are unambiguous.
        if (resp->status < 200 || resp->status >= 300)
            return std::unexpected(Error{.msg = "HTTP " + std::to_string(resp->status) +
                                             ": " + turn.error().msg,
                .code = static_cast<int>(resp->status)});
        return std::unexpected(turn.error());
    }
    return turn;
}

std::expected<Turn, Error> OpenAiProvider::complete_stream(
    const Conversation& conversation, const std::vector<ToolSpec>& tools,
    const StreamFn& on_delta, const std::atomic<bool>* cancel) {
    nlohmann::json req = build_chat_request(cfg_, conversation, tools, /*stream=*/true);
    std::string url = openai_chat_completions_url(cfg_.base_url);

    std::vector<std::string> headers;
    if (!cfg_.api_key.empty())
        headers.push_back("Authorization: Bearer " + cfg_.api_key);

    StreamAccumulator acc;
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
            StreamAccumulator::Added a = json::guard_or(
                [&] { return acc.ingest(*parsed); }, StreamAccumulator::Added{});
            if (on_delta && (!a.answer.empty() || !a.reasoning.empty()))
                on_delta(a.answer, a.reasoning);
        }
    };

    auto status = http::post_json_stream(url, headers, json::dump(req), on_chunk,
                                         cancel, cfg_.timeout_secs);
    if (!status) return std::unexpected(status.error());

    // Non-2xx: the server returns a regular JSON error object, not an SSE
    // stream. Mine the API message out of the buffered body for a clear error.
    if (*status < 200 || *status >= 300) {
        std::string detail;
        if (auto parsed = json::parse(raw); parsed) {
            auto err = json::guard("openai response",
                                   [&] { return parse_chat_response(*parsed); });
            if (!err) detail = err.error().msg;
        }
        if (detail.empty()) detail = raw.substr(0, 200);
        return std::unexpected(Error{.msg = "HTTP " + std::to_string(*status) + ": " + detail,
            .code = static_cast<int>(*status)});
    }
    return acc.finish();
}

void normalize_base_url(std::string& base_url) {
    // Strip a trailing slash so base_url + "/chat/completions" is well-formed.
    if (!base_url.empty() && base_url.back() == '/')
        base_url.pop_back();
}

std::string openai_chat_completions_url(std::string_view base) {
    std::string b(base);
    auto ends_with = [&](std::string_view suf) {
        return b.size() >= suf.size() &&
               b.compare(b.size() - suf.size(), suf.size(), suf) == 0;
    };
    if (ends_with("/chat/completions")) return b;            // already the endpoint
    if (ends_with("/v1")) return b + "/chat/completions";    // versioned base
    return b + "/v1/chat/completions";                       // bare base: add both
}

std::string openai_models_url(std::string_view base) {
    std::string b(base);
    auto ends_with = [&](std::string_view suf) {
        return b.size() >= suf.size() &&
               b.compare(b.size() - suf.size(), suf.size(), suf) == 0;
    };
    constexpr std::string_view kChat = "/chat/completions";
    if (ends_with(kChat))                                    // …/v1/chat/completions
        return b.substr(0, b.size() - kChat.size()) + "/models";
    if (ends_with("/v1")) return b + "/models";              // versioned base
    return b + "/v1/models";                                 // bare base: add both
}

std::vector<std::string> parse_model_ids(const nlohmann::json& body) {
    std::vector<std::string> out;
    auto data = body.find("data");
    if (data == body.end() || !data->is_array()) return out;
    for (const auto& m : *data) {
        if (m.is_object()) {
            auto id = m.find("id");
            if (id != m.end() && id->is_string())
                out.push_back(id->get<std::string>());
        } else if (m.is_string()) {
            out.push_back(m.get<std::string>());
        }
    }
    return out;
}

bool openai_model_likely_reasoning(std::string_view model) {
    const std::string m = to_lower(model);
    static constexpr std::string_view kFamilies[] = {
        "deepseek", "minimax", "claude", "gpt-5", "o1",
        "o3",       "o4",      "qwen",   "glm",   "grok", "gemini",
    };
    for (std::string_view fam : kFamilies)
        if (m.find(fam) != std::string::npos) return true;
    return false;
}

}  // namespace moocode
