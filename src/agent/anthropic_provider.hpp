#ifndef FLAGENT_ANTHROPIC_PROVIDER_HPP
#define FLAGENT_ANTHROPIC_PROVIDER_HPP

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

#include <concepts>
#include <expected>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "agent/provider.hpp"
#include "agent/strutil.hpp"
#include "agent/types.hpp"

namespace flagent {

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
};

// Map a flagent effort label to an Anthropic output_config.effort value
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

// Which backend wire format a base_url/model pair speaks.
enum class ProviderKind { OpenAI, Anthropic, Gemini };

// An explicit user choice; Auto means "decide via detect_provider_kind".
enum class ProviderChoice { Auto, OpenAI, Anthropic, Gemini };

// Human-readable label for the status bar / logs ("openai" / "anthropic").
const char* provider_kind_name(ProviderKind k);

// A named convenience bundle (wire format + endpoint + model) so a user can
// write e.g. `--provider deepseek-flash` instead of spelling out a base URL and
// model. Presets target each vendor's OpenAI-native endpoint (the richer,
// standard path; the Anthropic-compatible endpoint stays reachable by passing
// its base URL explicitly).
struct ProviderPreset {
    ProviderKind kind;
    std::string base_url;
    std::string model;
};

// Resolve a preset name (case-insensitive): "minimax", "deepseek"/"deepseek-pro",
// "deepseek-flash", "gemini"/"google" (native), "gemini-openai" (compat shim).
// Returns std::nullopt for non-preset values (openai/anthropic/auto and anything
// unrecognised), so callers fall back to parse_provider_choice.
std::optional<ProviderPreset> lookup_preset(std::string_view name);

// Parse a --provider / $LLM_PROVIDER / settings value. "anthropic"/"claude" =>
// Anthropic; "openai"/"oai"/"chat"/"gpt" => OpenAI; "gemini"/"google" => Gemini;
// anything else (incl. "", "auto") => Auto. Case-insensitive. Total.
ProviderChoice parse_provider_choice(std::string_view s);

// Autodetect the wire format from the endpoint: a base_url whose host contains
// "anthropic" => Anthropic; a Google "generativelanguage"/"googleapis" host =>
// Gemini (unless the path names the "/openai" compat shim, which is OpenAI);
// otherwise OpenAI. `model` is reserved for future heuristics and currently
// unused so OpenAI-compatible gateways are not misclassified. Total.
ProviderKind detect_provider_kind(std::string_view base_url,
                                  std::string_view model);

// The duck-typed contract the templated profile helpers below require: string
// .kind/.base_url/.model members and a .models range. Stated as a concept so
// this layer documents the contract and yields readable diagnostics without a
// dependency on agent_persist's Profile (the layering keeps persist
// provider-agnostic). The real Profile satisfies it (verified at build time).
template <class P>
concept ProfileLike = requires(const P& p) {
    { p.kind }     -> std::convertible_to<std::string_view>;
    { p.base_url } -> std::convertible_to<std::string_view>;
    { p.model }    -> std::convertible_to<std::string_view>;
    { p.models }   -> std::ranges::range;
};

// Translate a profile's "kind" string into a ProviderKind, falling back to
// detection from its base_url/model when kind is empty/"auto". Templated on the
// profile type so this layer needs no dependency on agent_persist's Profile
// (the layering keeps persist provider-agnostic). Total.
template <ProfileLike ProfileT>
ProviderKind profile_kind(const ProfileT& p) {
    ProviderChoice c = parse_provider_choice(p.kind);
    if (c == ProviderChoice::Anthropic) return ProviderKind::Anthropic;
    if (c == ProviderChoice::OpenAI) return ProviderKind::OpenAI;
    if (c == ProviderChoice::Gemini) return ProviderKind::Gemini;
    return detect_provider_kind(p.base_url, p.model);
}

// Find the first profile in `profiles` whose `models[]` contains `name`
// (case-insensitive). Returns nullptr when none serves it. Templated to avoid a
// dependency on agent_persist's Profile. pre: each profile has a .models range
// of strings.
template <ProfileLike ProfileT>
const ProfileT* find_model_profile(std::string_view name,
                                   const std::vector<ProfileT>& profiles) {
    const std::string want = to_lower(name);
    for (const ProfileT& p : profiles)
        for (const auto& m : p.models)
            if (to_lower(m) == want) return &p;
    return nullptr;
}

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

}  // namespace flagent

#endif  // FLAGENT_ANTHROPIC_PROVIDER_HPP
