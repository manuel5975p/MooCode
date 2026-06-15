#include "agent/gemini_provider.hpp"

#include <unordered_map>
#include <utility>

#include "agent/http.hpp"
#include "agent/json_util.hpp"
#include "agent/provider_factory.hpp"  // ProviderConnection (for GeminiConfig ctor)
#include "agent/stream.hpp"

namespace moocode {

namespace {

// Build the functionResponse "response" object for a Tool-role result. Gemini
// requires an object; pass a JSON object body through verbatim, otherwise wrap
// the raw string under "result" (or "error" for a failed call).
nlohmann::json tool_response_object(const Message& m) {
    auto parsed = json::parse(m.content());
    if (parsed && parsed->is_object()) return std::move(*parsed);
    return nlohmann::json{{m.tool_failed() ? "error" : "result", m.content()}};
}

// One assistant/user/model "parts" array for a non-tool message.
nlohmann::json content_parts(const Message& m) {
    nlohmann::json parts = nlohmann::json::array();
    if (m.role() == Role::User && !m.parts().empty()) {
        for (const auto& p : m.parts()) {
            if (!p.text.empty()) parts.push_back({{"text", p.text}});
            if (p.image)
                parts.push_back({{"inlineData",
                    {{"mimeType", p.image->media_type},
                     {"data", p.image->base64_data}}}});
        }
        return parts;
    }
    if (m.role() == Role::Assistant) {
        if (!m.content().empty()) parts.push_back({{"text", m.content()}});
        for (const auto& tc : m.tool_calls()) {
            nlohmann::json fc{{"name", tc.name}};
            if (!tc.id.empty()) fc["id"] = tc.id;
            auto args = json::parse(tc.arguments_json);
            fc["args"] = (args && args->is_object()) ? std::move(*args)
                                                     : nlohmann::json::object();
            nlohmann::json part{{"functionCall", std::move(fc)}};
            // Replay the Gemini 3 thoughtSignature on the same part, else the API
            // rejects the follow-up request with HTTP 400 "missing thought_signature".
            if (!tc.signature.empty()) part["thoughtSignature"] = tc.signature;
            parts.push_back(std::move(part));
        }
        return parts;
    }
    parts.push_back({{"text", m.content()}});
    return parts;
}

}  // namespace

int effort_to_thinking_budget(std::string_view effort) {
    if (effort == "minimal") return 1024;
    if (effort == "low") return 2048;
    if (effort == "high") return 24576;
    return 8192;  // "medium" and anything else
}

std::string gemini_generate_url(std::string_view base, std::string_view model,
                                bool stream) {
    std::string b(base);
    auto ends_with = [&](std::string_view suf) {
        return b.size() >= suf.size() &&
               b.compare(b.size() - suf.size(), suf.size(), suf) == 0;
    };
    if (!ends_with("/v1beta") && !ends_with("/v1")) b += "/v1beta";
    b += "/models/";
    b += model;
    b += stream ? ":streamGenerateContent?alt=sse" : ":generateContent";
    return b;
}

std::string gemini_models_url(std::string_view base) {
    std::string b(base);
    auto ends_with = [&](std::string_view suf) {
        return b.size() >= suf.size() &&
               b.compare(b.size() - suf.size(), suf.size(), suf) == 0;
    };
    if (!ends_with("/v1beta") && !ends_with("/v1")) b += "/v1beta";
    return b + "/models";
}

std::vector<std::string> parse_gemini_model_ids(const nlohmann::json& body) {
    std::vector<std::string> out;
    auto models = body.find("models");
    if (models == body.end() || !models->is_array()) return out;
    for (const auto& m : *models) {
        if (!m.is_object()) continue;
        auto name = m.find("name");
        if (name == m.end() || !name->is_string()) continue;
        // Keep only models that can actually generate content.
        bool can_generate = true;  // default true if the field is absent
        if (auto sm = m.find("supportedGenerationMethods");
            sm != m.end() && sm->is_array()) {
            can_generate = false;
            for (const auto& method : *sm)
                if (method.is_string() &&
                    method.get<std::string>() == "generateContent") {
                    can_generate = true;
                    break;
                }
        }
        if (!can_generate) continue;
        std::string id = name->get<std::string>();
        if (id.rfind("models/", 0) == 0) id.erase(0, 7);  // strip prefix
        out.push_back(std::move(id));
    }
    return out;
}

nlohmann::json build_generate_request(const GeminiConfig& cfg,
                                      const Conversation& conv,
                                      const std::vector<ToolSpec>& tools) {
    nlohmann::json req;

    // Map each assistant-issued tool-call id to its function name so Tool-role
    // results (which carry only the id) can be sent back keyed by name.
    std::unordered_map<std::string, std::string> id_to_name;
    for (const auto& m : conv)
        if (m.role() == Role::Assistant)
            for (const auto& tc : m.tool_calls())
                if (!tc.id.empty()) id_to_name.emplace(tc.id, tc.name);

    std::string system_text;
    nlohmann::json contents = nlohmann::json::array();

    for (std::size_t i = 0; i < conv.size();) {
        const Message& m = conv[i];
        if (m.role() == Role::System) {
            if (!system_text.empty()) system_text += "\n\n";
            system_text += m.content();
            ++i;
            continue;
        }
        if (m.role() == Role::Tool) {
            // Coalesce a run of tool results into one "user" content.
            nlohmann::json parts = nlohmann::json::array();
            for (; i < conv.size() && conv[i].role() == Role::Tool; ++i) {
                const Message& t = conv[i];
                nlohmann::json fr{{"response", tool_response_object(t)}};
                if (auto it = id_to_name.find(t.tool_call_id());
                    it != id_to_name.end())
                    fr["name"] = it->second;
                if (!t.tool_call_id().empty()) fr["id"] = t.tool_call_id();
                parts.push_back({{"functionResponse", std::move(fr)}});
            }
            contents.push_back(
                {{"role", "user"}, {"parts", std::move(parts)}});
            continue;
        }
        const char* role = m.role() == Role::Assistant ? "model" : "user";
        contents.push_back(
            {{"role", role}, {"parts", content_parts(m)}});
        ++i;
    }

    // Gemini requires strictly alternating user/model turns. A prompt buffered
    // mid-turn lands as a user turn right after the functionResponse user turn,
    // so merge consecutive same-role contents by concatenating their parts —
    // otherwise the API rejects the back-to-back user roles.
    nlohmann::json merged = nlohmann::json::array();
    for (auto& c : contents) {
        if (!merged.empty() &&
            merged.back().value("role", "") == c.value("role", "")) {
            for (auto& p : c["parts"]) merged.back()["parts"].push_back(std::move(p));
        } else {
            merged.push_back(std::move(c));
        }
    }
    contents = std::move(merged);

    req["contents"] = std::move(contents);
    if (!system_text.empty())
        req["systemInstruction"]["parts"] =
            nlohmann::json::array({{{"text", system_text}}});

    nlohmann::json gen;
    gen["temperature"] = cfg.temperature;
    if (cfg.max_tokens > 0) gen["maxOutputTokens"] = cfg.max_tokens;
    if (cfg.thinking) {
        if (*cfg.thinking) {
            gen["thinkingConfig"] = {
                {"thinkingBudget", effort_to_thinking_budget(cfg.reasoning_effort)},
                {"includeThoughts", true}};
        } else {
            gen["thinkingConfig"] = {{"thinkingBudget", 0}};
        }
    }
    req["generationConfig"] = std::move(gen);

    if (!tools.empty()) {
        nlohmann::json decls = nlohmann::json::array();
        for (const auto& t : tools) {
            nlohmann::json d{{"name", t.name}, {"description", t.description}};
            if (!t.parameters.is_null()) d["parameters"] = t.parameters;
            decls.push_back(std::move(d));
        }
        req["tools"] =
            nlohmann::json::array({{{"functionDeclarations", std::move(decls)}}});
    }
    return req;
}

std::expected<Turn, Error> parse_generate_response(const nlohmann::json& body) {
    if (!body.is_object())
        return std::unexpected(Error{.msg = "response is not a JSON object", .code = 0});

    if (auto it = body.find("error"); it != body.end()) {
        std::string msg = "API error";
        if (it->is_object() && it->contains("message") && (*it)["message"].is_string())
            msg = (*it)["message"].get<std::string>();
        else
            msg = it->dump();
        return std::unexpected(Error{.msg = msg, .code = 0});
    }

    Turn turn;
    auto cands = body.find("candidates");
    if (cands != body.end() && cands->is_array() && !cands->empty()) {
        const nlohmann::json& cand = (*cands)[0];
        if (cand.is_object()) {
            if (auto fr = cand.find("finishReason");
                fr != cand.end() && fr->is_string())
                turn.finish_reason = fr->get<std::string>();
            if (auto content = cand.find("content");
                content != cand.end() && content->is_object())
                if (auto parts = content->find("parts");
                    parts != content->end() && parts->is_array())
                    for (const auto& part : *parts) {
                        if (!part.is_object()) continue;
                        if (auto fc = part.find("functionCall");
                            fc != part.end() && fc->is_object()) {
                            ToolCall call;
                            call.id = json::get_string_or(*fc, "id");
                            call.name = json::get_string_or(*fc, "name");
                            if (auto a = fc->find("args"); a != fc->end())
                                call.arguments_json = a->dump();
                            else
                                call.arguments_json = "{}";
                            // thoughtSignature sits on the part, beside functionCall.
                            call.signature = json::get_string_or(part, "thoughtSignature");
                            turn.tool_calls.push_back(std::move(call));
                            continue;
                        }
                        // Thought parts are display/audit only — captured on
                        // Turn.reasoning, never folded into Turn.text.
                        const bool is_thought =
                            [&] { auto th = part.find("thought");
                                  return th != part.end() && th->is_boolean() &&
                                         th->get<bool>(); }();
                        if (auto t = part.find("text");
                            t != part.end() && t->is_string()) {
                            if (is_thought)
                                turn.reasoning += t->get<std::string>();
                            else
                                turn.text += t->get<std::string>();
                        }
                    }
        }
    }

    if (auto u = body.find("usageMetadata"); u != body.end() && u->is_object())
        turn.usage = parse_gemini_usage(*u);

    if (turn.text.empty() && turn.tool_calls.empty()) {
        const std::string fr =
            turn.finish_reason.empty() ? "unknown" : turn.finish_reason;
        return std::unexpected(Error{
            .msg = "model returned no content (finishReason: " + fr + ")", .code = 0});
    }
    return turn;
}

GeminiConfig::GeminiConfig(const ProviderConnection& c)
    : base_url(c.base_url), api_key(c.api_key), model(c.model),
      max_tokens(c.max_tokens) {}

GeminiProvider::GeminiProvider(GeminiConfig cfg) : cfg_(std::move(cfg)) {}

std::vector<std::string> GeminiProvider::headers() const {
    std::vector<std::string> h;
    if (!cfg_.api_key.empty()) h.push_back("x-goog-api-key: " + cfg_.api_key);
    return h;
}

void GeminiProvider::set_params(const GenerationParams& p) {
    if (p.effort) cfg_.reasoning_effort = *p.effort;
    if (p.temperature) cfg_.temperature = *p.temperature;
    if (p.thinking) cfg_.thinking = *p.thinking;
    if (p.max_tokens) cfg_.max_tokens = *p.max_tokens;
}

GenerationParams GeminiProvider::params() const {
    GenerationParams p;
    if (!cfg_.reasoning_effort.empty()) p.effort = cfg_.reasoning_effort;
    p.temperature = cfg_.temperature;
    p.thinking = cfg_.thinking;
    if (cfg_.max_tokens > 0) p.max_tokens = cfg_.max_tokens;
    return p;
}

void GeminiProvider::set_model(std::string m) { provider_set_model(cfg_.model, std::move(m)); }
std::string GeminiProvider::model() const { return provider_get_model(cfg_.model); }
void GeminiProvider::set_base_url(std::string url) { provider_set_base_url(cfg_.base_url, std::move(url)); }
std::string GeminiProvider::base_url() const { return provider_get_base_url(cfg_.base_url); }
void GeminiProvider::set_api_key(std::string key) { provider_set_api_key(cfg_.api_key, std::move(key)); }

std::expected<std::vector<std::string>, Error> GeminiProvider::list_models() {
    std::string url = gemini_models_url(cfg_.base_url);
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
        if (auto e = parsed->find("error"); e != parsed->end() && e->is_object())
            detail = ": " + json::get_string_or(*e, "message");
        return std::unexpected(Error{
            .msg = "HTTP " + std::to_string(resp->status) + detail,
            .code = static_cast<int>(resp->status)});
    }
    return json::guard_or([&] { return parse_gemini_model_ids(*parsed); },
                          std::vector<std::string>{});
}

std::expected<Turn, Error> GeminiProvider::complete(
    const Conversation& conversation, const std::vector<ToolSpec>& tools) {
    nlohmann::json req = build_generate_request(cfg_, conversation, tools);
    std::string url = gemini_generate_url(cfg_.base_url, cfg_.model, /*stream=*/false);

    auto resp = http::post_json(url, headers(), json::dump(req), cfg_.timeout_secs);
    if (!resp) return std::unexpected(resp.error());

    auto parsed = json::parse(resp->body);
    if (!parsed) {
        std::string snippet = resp->body.substr(0, 200);
        return std::unexpected(Error{.msg = "HTTP " + std::to_string(resp->status) +
                                         ": non-JSON response: " + snippet,
            .code = static_cast<int>(resp->status)});
    }

    auto turn = json::guard("gemini response",
                            [&] { return parse_generate_response(*parsed); });
    if (!turn) {
        if (resp->status < 200 || resp->status >= 300)
            return std::unexpected(Error{
                .msg = "HTTP " + std::to_string(resp->status) + ": " + turn.error().msg,
                .code = static_cast<int>(resp->status)});
        return std::unexpected(turn.error());
    }
    return turn;
}

std::expected<Turn, Error> GeminiProvider::complete_stream(
    const Conversation& conversation, const std::vector<ToolSpec>& tools,
    const StreamFn& on_delta, const std::atomic<bool>* cancel) {
    nlohmann::json req = build_generate_request(cfg_, conversation, tools);
    std::string url = gemini_generate_url(cfg_.base_url, cfg_.model, /*stream=*/true);

    GeminiStreamAccumulator acc;
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
            GeminiStreamAccumulator::Added a = json::guard_or(
                [&] { return acc.ingest(*parsed); }, GeminiStreamAccumulator::Added{});
            if (on_delta && (!a.answer.empty() || !a.reasoning.empty()))
                on_delta(a.answer, a.reasoning);
        }
    };

    auto status = http::post_json_stream(url, headers(), json::dump(req), on_chunk,
                                         cancel, cfg_.timeout_secs);
    if (!status) return std::unexpected(status.error());

    // Non-2xx: the server returns a regular JSON error object, not SSE. Mine the
    // API message out of the buffered body for a clear error.
    if (*status < 200 || *status >= 300) {
        std::string detail;
        if (auto parsed = json::parse(raw); parsed) {
            auto err = json::guard("gemini response",
                                   [&] { return parse_generate_response(*parsed); });
            if (!err) detail = err.error().msg;
        }
        if (detail.empty()) detail = raw.substr(0, 200);
        return std::unexpected(Error{.msg = "HTTP " + std::to_string(*status) + ": " + detail,
            .code = static_cast<int>(*status)});
    }
    return acc.finish();
}

}  // namespace moocode
