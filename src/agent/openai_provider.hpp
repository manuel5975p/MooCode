#ifndef MOOCODE_OPENAI_PROVIDER_HPP
#define MOOCODE_OPENAI_PROVIDER_HPP

// OpenAI-compatible Provider: speaks the /chat/completions wire format, which
// OpenAI, OpenRouter, vLLM, llama.cpp and Ollama all accept. The request build
// and response parse are exposed as free functions so they can be tested
// independently of any network round-trip.

#include <expected>
#include <optional>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

#include "agent/provider.hpp"
#include "agent/types.hpp"

namespace moocode {

struct OpenAiConfig {
    std::string base_url;       // e.g. "https://api.minimax.io/v1" (no trailing /)
    std::string api_key;        // sent as "Authorization: Bearer <key>"
    std::string model;          // e.g. "MiniMax-M3"
    double temperature = 0.0;
    long timeout_secs = 0;  // <= 0: no overall transfer cap (model may reason
                            // indefinitely); only connection setup stays bounded.
    // Optional generation controls (opt-in; omitted from the request unless set).
    std::string reasoning_effort;        // "low"/"medium"/"high"; "" => omit
    std::optional<bool> thinking;        // nullopt => omit; else thinking:{type}
    std::string thinking_type = "enabled";  // thinking.type when ON ("enabled" DeepSeek, "adaptive" MiniMax)
    int max_tokens = 0;                  // <= 0 => omit; else max_tokens
    // Per-(base_url, model) request-param passthrough allowlists. The matching
    // entry (by the live base_url + model) contributes an "allowed_openai_params"
    // array to the request, so proxies that gate fields like reasoning_effort
    // see them. No match => the key is omitted entirely.
    std::vector<AllowedOpenAiParams> allowed_openai_params;

    OpenAiConfig() = default;
    explicit OpenAiConfig(const struct ProviderConnection& c);
};

// Compose the Chat Completions endpoint URL from a base URL. The OpenAI path is
// "<base>/v1/chat/completions"; a base already ending in "/v1" gets only
// "/chat/completions" appended, and a base that already names the full endpoint
// ("…/chat/completions") is used verbatim. This lets a bare host
// ("http://localhost:8080"), a versioned base ("…/v1") and a fully-qualified
// endpoint all resolve correctly, mirroring anthropic_messages_url so both wire
// formats self-heal the user's base URL. pre: base has no trailing slash (run
// normalize_base_url first). Total.
std::string openai_chat_completions_url(std::string_view base);

// Compose the model-listing endpoint ("<base>/v1/models", or "<base>/models"
// when base already ends in "/v1", or "<base>/models" alongside a fully-named
// "…/chat/completions" base). Mirrors openai_chat_completions_url so the same
// base URL resolves both endpoints. pre: base has no trailing slash. Total.
std::string openai_models_url(std::string_view base);

// Extract model ids from an OpenAI-style "{data:[{id:…}]}" listing body. Pure,
// lenient: skips entries without a string "id"; missing/!array "data" => empty.
std::vector<std::string> parse_model_ids(const nlohmann::json& body);

// Heuristic: does `model` look like a reasoning-capable model that honors the
// OpenAI-path reasoning_effort / thinking fields (DeepSeek, MiniMax, Claude,
// GPT-5, o-series, Qwen, GLM, Grok, Gemini)? Used only to warn when those
// controls are set against a model that will likely ignore them; the Anthropic
// path supports thinking natively and never needs this. Case-insensitive. Total.
bool openai_model_likely_reasoning(std::string_view model);

// Build the /chat/completions request body. Pure (no I/O). Omits the "tools" key
// entirely when `tools` is empty; sets "stream": true only when `stream` is set.
// pre: none. post: object with model+messages.
nlohmann::json build_chat_request(const OpenAiConfig& cfg,
                                  const Conversation& conversation,
                                  const std::vector<ToolSpec>& tools,
                                  bool stream = false);

// Parse a /chat/completions response body into a Turn. Pure. Reports an Error
// for API error objects, missing choices, or malformed tool calls.
std::expected<Turn, Error> parse_chat_response(const nlohmann::json& body);

class OpenAiProvider : public Provider {
public:
    explicit OpenAiProvider(OpenAiConfig cfg);

    std::expected<Turn, Error> complete(const Conversation& conversation,
                                        const std::vector<ToolSpec>& tools) override;

    // Stream /chat/completions (SSE), assembling the Turn while forwarding
    // answer/reasoning fragments to `on_delta` as they arrive. When `cancel`
    // becomes true the in-flight HTTP request is aborted and an Error returned.
    std::expected<Turn, Error> complete_stream(
        const Conversation& conversation, const std::vector<ToolSpec>& tools,
        const StreamFn& on_delta,
        const std::atomic<bool>* cancel = nullptr) override;

    void set_params(const GenerationParams& p) override;
    GenerationParams params() const override;
    void set_model(std::string m) override;
    std::string model() const override;
    void set_base_url(std::string url) override;
    std::string base_url() const override;
    void set_api_key(std::string key) override;
    void set_thinking_type(std::string type) override;
    std::expected<std::vector<std::string>, Error> list_models() override;
    std::string_view wire_format() const override { return "openai"; }

private:
    OpenAiConfig cfg_;
};

}  // namespace moocode

#endif  // MOOCODE_OPENAI_PROVIDER_HPP
