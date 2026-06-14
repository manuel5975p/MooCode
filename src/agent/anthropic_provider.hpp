#ifndef MOOCODE_ANTHROPIC_PROVIDER_HPP
#define MOOCODE_ANTHROPIC_PROVIDER_HPP

// Anthropic-native Provider: speaks the Messages API (`/v1/messages`) wire
// format used by api.anthropic.com and Anthropic-compatible gateways. It is the
// second backend behind the Provider seam alongside OpenAiProvider; the agent
// loop is unchanged. The request build and response parse are free functions so
// they can be tested without a network round-trip, mirroring openai_provider.
//
// Wire-format differences from OpenAI handled here:
//   - auth via "x-api-key" + "anthropic-version" headers (not Bearer);
//   - the system prompt is a top-level "system" field, not a message;
//   - "max_tokens" is required;
//   - assistant tool calls are "tool_use" content blocks; tool results are
//     "tool_result" blocks coalesced into a single following user message;
//   - tools advertise "input_schema" rather than "function"/"parameters";
//   - usage is input_tokens/output_tokens (see parse_anthropic_usage).

#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "agent/provider.hpp"
#include "agent/types.hpp"

namespace moocode {

struct AnthropicConfig {
    std::string base_url;  // e.g. "https://api.anthropic.com/v1" (no trailing /)
    std::string api_key;   // sent as "x-api-key: <key>"
    std::string model;     // e.g. "claude-sonnet-4-6"
    std::optional<double> temperature;  // unset => omit (Opus 4.7+/Fable reject it)
    int max_tokens = 8192;  // required by the Messages API; output token cap
    long timeout_secs = 0;  // <= 0: no overall transfer cap (only connect setup)
    std::string anthropic_version = "2023-06-01";  // "anthropic-version" header
    // Optional reasoning controls (opt-in). effort selects an extended-thinking
    // budget (low/medium/high); thinking forces it on/off; when thinking is
    // enabled the request forces temperature=1 and lifts max_tokens above the
    // budget, both Messages-API requirements.
    std::string reasoning_effort;    // "low"/"medium"/"high"; "" => use default budget
    std::optional<bool> thinking;    // nullopt => model default (off)

    AnthropicConfig() = default;
    explicit AnthropicConfig(const struct ProviderConnection& c);
};

// Map a moocode effort label to an Anthropic output_config.effort value
// (low/medium/high/xhigh/max; "minimal" => "low"). Returns "" for unrecognised
// labels so the caller omits the field and lets the model default apply.
std::string effort_to_output_effort(std::string_view effort);

// Compose the Messages endpoint URL from a base URL. The Anthropic Messages
// path is "<base>/v1/messages"; a base that already ends in "/v1" gets only
// "/messages" appended. This lets both the bare "ANTHROPIC_BASE_URL" form that
// vendors document (e.g. "https://api.minimax.io/anthropic",
// "https://api.deepseek.com/anthropic") and an explicit "…/v1" base resolve to
// the same correct endpoint. pre: base has no trailing slash (run
// normalize_base_url first). Total.
std::string anthropic_messages_url(std::string_view base);

// Compose the model-listing endpoint ("<base>/v1/models", or "<base>/models"
// when base already ends in "/v1"). Mirrors anthropic_messages_url. pre: base
// has no trailing slash. Total.
std::string anthropic_models_url(std::string_view base);

// Build the /messages request body. Pure (no I/O). Extracts System messages
// into the top-level "system" field, coalesces consecutive Tool-role messages
// into one user message of tool_result blocks, and serialises assistant
// tool_calls as tool_use blocks. Omits "tools" when empty; sets "stream":true
// only when `stream` is set. pre: none. post: object with model+max_tokens+
// messages.
nlohmann::json build_messages_request(const AnthropicConfig& cfg,
                                      const Conversation& conversation,
                                      const std::vector<ToolSpec>& tools,
                                      bool stream = false);

// Parse a /messages response body into a Turn. Pure. Concatenates text blocks
// into Turn.text, maps tool_use blocks to ToolCalls (input serialised back to a
// JSON string), and reports an Error for API error objects or a missing content
// array. thinking blocks are ignored (display-only, never persisted).
std::expected<Turn, Error> parse_messages_response(const nlohmann::json& body);

class AnthropicProvider : public Provider {
public:
    explicit AnthropicProvider(AnthropicConfig cfg);

    std::expected<Turn, Error> complete(
        const Conversation& conversation,
        const std::vector<ToolSpec>& tools) override;

    // Stream /messages (typed SSE events), assembling the Turn while forwarding
    // text -> answer and thinking -> reasoning fragments to `on_delta`. When
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
    std::string_view wire_format() const override { return "anthropic"; }

private:
    AnthropicConfig cfg_;

    // The auth + version header lines shared by both calls.
    std::vector<std::string> headers() const;
};

}  // namespace moocode

#endif  // MOOCODE_ANTHROPIC_PROVIDER_HPP
