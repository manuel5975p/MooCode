#ifndef FLAGENT_PROVIDER_HPP
#define FLAGENT_PROVIDER_HPP

// The provider seam: one abstract operation, "produce the next assistant turn".
// Keeping this wire-format-agnostic lets OpenAI-compatible and (future)
// Anthropic-native backends coexist without touching the agent loop.

#include <atomic>
#include <expected>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "agent/types.hpp"

namespace flagent {

// Token accounting for one turn, as reported by the server's `usage` object.
// `present` is false when the endpoint did not report usage.
struct Usage {
    int prompt_tokens = 0;
    int completion_tokens = 0;
    int total_tokens = 0;
    bool present = false;
};

// One assistant turn. Empty `tool_calls` => the model is done and `text` is the
// final answer. Non-empty => the agent must run them and feed back results.
struct Turn {
    std::string text;
    std::vector<ToolCall> tool_calls;
    std::string finish_reason;
    Usage usage;
    std::string reasoning;  // assistant chain-of-thought; display/audit only,
                            // never sent back to the model. May be empty.
};

// Live-output callback for complete_stream: invoked with (answer fragment,
// reasoning fragment) as bytes arrive. Either fragment may be empty.
using StreamFn =
    std::function<void(std::string_view answer, std::string_view reasoning)>;

// Optional per-request generation controls, applied by whichever backend
// supports them (see each provider's request builder). Every field is opt-in: a
// std::nullopt leaves the provider's existing value untouched, and nothing is
// sent on the wire unless explicitly set, so strict endpoints aren't broken.
// Used both for initial config (from flags/settings) and live TUI adjustment.
struct GenerationParams {
    std::optional<std::string> effort;       // "low"/"medium"/"high" (passthrough)
    std::optional<double> temperature;       // sampling temperature
    std::optional<bool> thinking;            // force reasoning on/off
    std::optional<int> max_tokens;           // output-token cap (>0)
};

struct Provider {
    // Produce one assistant turn from the full conversation and advertised tools.
    // pre: conversation nonempty. post: on success, text and/or tool_calls set.
    virtual std::expected<Turn, Error> complete(
        const Conversation& conversation, const std::vector<ToolSpec>& tools) = 0;

    // Streaming variant: same assembled result as complete(), but `on_delta` is
    // called with answer/reasoning fragments as they arrive. When `cancel` is
    // non-null and becomes true mid-stream the transfer is aborted and an Error
    // is returned. The base default ignores `cancel` and falls back to the
    // blocking complete().
    virtual std::expected<Turn, Error> complete_stream(
        const Conversation& conversation, const std::vector<ToolSpec>& tools,
        const StreamFn& on_delta,
        const std::atomic<bool>* cancel = nullptr) {
        (void)cancel;
        auto turn = complete(conversation, tools);
        if (turn && on_delta) on_delta(turn->text, std::string_view{});
        return turn;
    }

    // Merge the set fields of `p` into this provider's generation config; unset
    // fields are left as-is. Applied on the next request. The base default
    // ignores them (a provider that supports none of these need not override).
    virtual void set_params(const GenerationParams& p) { (void)p; }

    // The provider's current generation config, for display (e.g. the TUI).
    // The base default reports nothing set.
    virtual GenerationParams params() const { return {}; }

    // Change the model at runtime (TUI /model command). The base default is a
    // no-op for providers that don't support live model switching.
    virtual void set_model(std::string m) { (void)m; }

    // Return the current model name, for display (e.g. the TUI status bar).
    virtual std::string model() const { return {}; }

    // Change the base URL at runtime (TUI /model command may also switch
    // endpoints when moving between vendors). The base default is a no-op.
    virtual void set_base_url(std::string url) { (void)url; }

    // Return the current base URL, for display.
    virtual std::string base_url() const { return {}; }

    // The backend wire format this provider speaks ("openai" / "anthropic"), for
    // display (e.g. the TUI status bar) and the same_kind check on a runtime
    // swap. Reading it is a plain const access, safe from the render thread. The
    // base default reports the OpenAI-compatible format (the generic case).
    virtual std::string_view wire_format() const { return "openai"; }

    // Change the API key at runtime (TUI /provider, or /model when the key
    // changes between profiles). The base default is a no-op.
    virtual void set_api_key(std::string key) { (void)key; }

    // List the model ids the endpoint advertises (for populating a profile's
    // model set / autocomplete). An Error means the listing call failed
    // (transport, non-2xx, unparseable); an empty vector means the endpoint
    // replied with no models. The base default reports "unsupported" so a
    // backend opts in by overriding.
    virtual std::expected<std::vector<std::string>, Error> list_models() {
        return std::unexpected(
            Error{.msg = "model listing not supported by this provider",
                  .code = 0});
    }

    virtual ~Provider() = default;
};

}  // namespace flagent

#endif  // FLAGENT_PROVIDER_HPP
