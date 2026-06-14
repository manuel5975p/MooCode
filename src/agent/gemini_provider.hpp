#ifndef MOOCODE_GEMINI_PROVIDER_HPP
#define MOOCODE_GEMINI_PROVIDER_HPP

// Google Gemini-native Provider: speaks the Generative Language API
// (`/v1beta/models/<model>:generateContent` and `:streamGenerateContent`) used
// by generativelanguage.googleapis.com. It is the third backend behind the
// Provider seam alongside OpenAiProvider and AnthropicProvider; the agent loop
// is unchanged. The request build and response parse are free functions so they
// can be tested without a network round-trip, mirroring the other providers.
//
// Wire-format differences from OpenAI/Anthropic handled here:
//   - auth via the "x-goog-api-key" header (not Bearer / x-api-key);
//   - the model lives in the URL path with a ":method" suffix, not the body;
//   - roles are "user"/"model" only; the system prompt is a top-level
//     "systemInstruction" Content object, not a message;
//   - a message is a list of typed "parts" (text / functionCall /
//     functionResponse / inlineData), one field per part object;
//   - tool calls come back as functionCall parts (args is a JSON object, not a
//     string); results go back as functionResponse parts keyed by function
//     NAME (not id), so the builder threads a call_id->name map;
//   - tools advertise "functionDeclarations"; generation knobs live under
//     "generationConfig" (thinkingConfig for extended thinking);
//   - usage is promptTokenCount / candidatesTokenCount / thoughtsTokenCount.

#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "agent/provider.hpp"
#include "agent/types.hpp"

namespace moocode {

struct GeminiConfig {
    std::string base_url;  // e.g. "https://generativelanguage.googleapis.com/v1beta"
    std::string api_key;   // sent as "x-goog-api-key: <key>"
    std::string model;     // e.g. "gemini-3.5-flash"
    double temperature = 0.0;
    long timeout_secs = 0;  // <= 0: no overall transfer cap (only connect setup)
    // Optional generation controls (opt-in; omitted from the request unless set).
    std::string reasoning_effort;  // "low"/"medium"/"high"; "" => omit thinkingConfig
    std::optional<bool> thinking;  // nullopt => omit; true/false => thinkingConfig
    int max_tokens = 0;            // <= 0 => omit; else maxOutputTokens

    GeminiConfig() = default;
    explicit GeminiConfig(const struct ProviderConnection& c);
};

// Map an effort label to a Gemini thinkingBudget in tokens (minimal 1024,
// low 2048, medium 8192, high 24576; anything else => medium). Total.
int effort_to_thinking_budget(std::string_view effort);

// Compose the generate endpoint URL: "<base>/models/<model>:generateContent",
// or ":streamGenerateContent?alt=sse" when `stream`. A base already ending in
// "/v1beta" or "/v1" is used as-is; a bare host gets "/v1beta" appended so a
// user can pass just "https://generativelanguage.googleapis.com". pre: base has
// no trailing slash (run normalize_base_url first). Total.
std::string gemini_generate_url(std::string_view base, std::string_view model,
                                bool stream);

// Compose the model-listing endpoint ("<base>/models", adding "/v1beta" first
// for a bare host). Mirrors gemini_generate_url. pre: base has no trailing
// slash. Total.
std::string gemini_models_url(std::string_view base);

// Extract model ids from a Gemini "{models:[{name:"models/<id>",
// supportedGenerationMethods:[...]}]}" listing. Strips the "models/" prefix and
// keeps only entries advertising "generateContent". Pure, lenient. Total.
std::vector<std::string> parse_gemini_model_ids(const nlohmann::json& body);

// Build the :generateContent request body. Pure (no I/O). Extracts System
// messages into top-level "systemInstruction", maps assistant tool_calls to
// functionCall parts and Tool results to functionResponse parts (resolving the
// function name via a call_id->name map walked from the assistant turns). Omits
// "tools" when empty and thinkingConfig unless configured. pre: none. post:
// object with "contents".
nlohmann::json build_generate_request(const GeminiConfig& cfg,
                                      const Conversation& conversation,
                                      const std::vector<ToolSpec>& tools);

// Parse a :generateContent response body into a Turn. Pure. Concatenates
// non-thought text parts into Turn.text, maps functionCall parts to ToolCalls
// (args serialised back to a JSON string), reads usageMetadata, and reports an
// Error for API error objects. Thought parts are ignored (display-only).
std::expected<Turn, Error> parse_generate_response(const nlohmann::json& body);

class GeminiProvider : public Provider {
public:
    explicit GeminiProvider(GeminiConfig cfg);

    std::expected<Turn, Error> complete(
        const Conversation& conversation,
        const std::vector<ToolSpec>& tools) override;

    // Stream :streamGenerateContent (SSE), assembling the Turn while forwarding
    // text -> answer and thought -> reasoning fragments to `on_delta`. When
    // `cancel` becomes true the in-flight request is aborted and an Error
    // returned.
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
    std::expected<std::vector<std::string>, Error> list_models() override;
    std::string_view wire_format() const override { return "gemini"; }

private:
    GeminiConfig cfg_;

    // The auth header line(s) shared by both calls.
    std::vector<std::string> headers() const;
};

}  // namespace moocode

#endif  // MOOCODE_GEMINI_PROVIDER_HPP
