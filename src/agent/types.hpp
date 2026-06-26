#ifndef MOOCODE_TYPES_HPP
#define MOOCODE_TYPES_HPP

// Core value types shared across every layer (http, json, provider, tools,
// agent). Header-only: pure data, no behavior, no implementation-only APIs.

#include <cassert>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace moocode {

// Coarse category for an Error, orthogonal to the layer-defined `code`. Lets a
// consumer branch on the kind of failure without string-matching the message.
enum class ErrorKind { Unknown, Transport, Http, Parse, Tool, Cancelled, User };

// Recoverable failure carried by std::expected. `code` is layer-defined
// (e.g. HTTP status, errno, or 0 when not applicable); `kind` is the coarse
// category (defaults to Unknown so existing {.msg,.code} initializers stay
// valid).
struct Error {
    std::string msg;
    int code = 0;
    ErrorKind kind = ErrorKind::Unknown;
};

// A model-requested tool invocation. `id` is provider-assigned and must be
// echoed back on the corresponding Tool-role result message.
struct ToolCall {
    std::string id;
    std::string name;
    std::string arguments_json;  // raw JSON object string, exactly as sent
    // Opaque provider reasoning token that must be replayed verbatim on the
    // assistant turn in later requests. Gemini 3 attaches a `thoughtSignature`
    // to each functionCall part and rejects (HTTP 400) follow-up requests that
    // omit it. Empty for providers/models that don't use one. Never displayed.
    // The default member initializer keeps existing aggregate initializations
    // (which omit this trailing field) warning-clean under -Wmissing-field-initializers.
    std::string signature = {};
};

// A base64-encoded image for multimodal model requests.
struct ImageBlock {
    std::string base64_data;
    std::string media_type;  // "image/png", "image/jpeg", "image/gif", "image/webp"
};

// One multimodal content part: text, an image, or both (captioned image).
struct ContentPart {
    std::string text;                // text (may be empty when image is set)
    std::optional<ImageBlock> image; // image content, when present
};

enum class Role { System, User, Assistant, Tool };

// Lowercase wire name for a role; unreachable default -> "user". Total and
// allocation-free (pure switch over a string literal), hence noexcept.
inline const char* role_str(Role r) noexcept {
    switch (r) {
        case Role::System: return "system";
        case Role::User: return "user";
        case Role::Assistant: return "assistant";
        case Role::Tool: return "tool";
    }
    return "user";
}

// One conversation turn. Field relevance depends on `role`:
//   Assistant: `content` + `tool_calls`. `parts` is unused.
//   Tool:      `content` + `tool_call_id`. `parts` is unused.
//   System:    `content` only. `parts` is unused.
//   User:      `content` when text-only (backward compatible); `parts` when
//              the message includes one or more images (multimodal).
// One conversation turn, constructed only through the role-tagged factories so
// an illegal field combination is unrepresentable. Role-specific accessors
// assert their role (live under Debug/ASan), turning a wrong-role read into a
// fast abort. Field relevance by role:
//   System:    content only.
//   User:      content (text-only) OR parts (multimodal); content ignored when
//              parts is non-empty.
//   Assistant: content + tool_calls.
//   Tool:      content + tool_call_id + tool_failed (failure carried as data).
// `content()` has a mutating overload because compact() rewrites it in place.
class Message {
public:
    static Message system(std::string content);
    static Message user(std::string content, std::vector<ContentPart> parts = {});
    static Message assistant(std::string content, std::vector<ToolCall> calls = {},
                             std::string reasoning = {});
    static Message tool(std::string call_id, std::string content, bool failed = false);

    Role role() const { return role_; }
    const std::string& content() const { return content_; }
    std::string& content() { return content_; }  // compact() mutates in place
    const std::vector<ToolCall>& tool_calls() const {
        assert(role_ == Role::Assistant);
        return tool_calls_;
    }
    // Assistant chain-of-thought; display/audit only, may be empty. No role
    // assert: the persist loader reads it generically and non-assistant
    // messages simply carry an empty string.
    const std::string& reasoning() const { return reasoning_; }
    const std::string& tool_call_id() const {
        assert(role_ == Role::Tool);
        return tool_call_id_;
    }
    bool tool_failed() const {
        assert(role_ == Role::Tool);
        return tool_failed_;
    }
    const std::vector<ContentPart>& parts() const {
        assert(role_ == Role::User);
        return parts_;
    }

private:
    Message() = default;
    Role role_ = Role::User;
    std::string content_;
    std::string reasoning_;
    std::string tool_call_id_;
    std::vector<ToolCall> tool_calls_;
    std::vector<ContentPart> parts_;
    bool tool_failed_ = false;
};

inline Message Message::system(std::string content) {
    Message m;
    m.role_ = Role::System;
    m.content_ = std::move(content);
    return m;
}
inline Message Message::user(std::string content, std::vector<ContentPart> parts) {
    Message m;
    m.role_ = Role::User;
    m.content_ = std::move(content);
    m.parts_ = std::move(parts);
    return m;
}
inline Message Message::assistant(std::string content, std::vector<ToolCall> calls,
                                  std::string reasoning) {
    Message m;
    m.role_ = Role::Assistant;
    m.content_ = std::move(content);
    m.tool_calls_ = std::move(calls);
    m.reasoning_ = std::move(reasoning);
    return m;
}
inline Message Message::tool(std::string call_id, std::string content, bool failed) {
    Message m;
    m.role_ = Role::Tool;
    m.tool_call_id_ = std::move(call_id);
    m.content_ = std::move(content);
    m.tool_failed_ = failed;
    return m;
}
using Conversation = std::vector<Message>;

// A tool as advertised to the model (JSON-Schema described parameters).
struct ToolSpec {
    std::string name;
    std::string description;
    nlohmann::json parameters;  // JSON Schema object
};

// A per-(endpoint, model) allowlist of non-standard OpenAI request params the
// backend should be told to pass through. Some OpenAI-compatible proxies drop
// unknown fields (e.g. reasoning_effort) unless the request itself echoes them
// in an "allowed_openai_params" array; pinning the allowlist to one base_url +
// model lets a single proxy gate it per model. Lives here (shared base layer)
// so both agent_persist (Settings) and the provider layer name one type.
// Matched tolerant of a trailing slash on base_url and case-insensitively on
// model; empty `params` => emit nothing.
struct AllowedOpenAiParams {
    std::string base_url;
    std::string model;
    std::vector<std::string> params;  // e.g. {"reasoning_effort"}
};

// Syntax-highlight colour scheme for fenced code blocks. Lived in tui.hpp but
// moved here so the lightweight tui_slash.hpp can name it without pulling in
// the full tui.hpp + transitive deps. "None" disables highlighting.
enum class SyntaxTheme { None, Default, Mono, Vivid };

// Canonical lowercase name of a theme ("default", "mono", …); round-trips with
// syntax_theme_from_name. Never empty. Pure, unit-testable.
std::string_view syntax_theme_name(SyntaxTheme t);

// Parse a theme name (case-insensitive). nullopt for an unknown name.
std::optional<SyntaxTheme> syntax_theme_from_name(std::string_view name);

// All theme names in display order, for /theme listing and autocomplete.
std::vector<std::string> syntax_theme_names();

}  // namespace moocode

#endif  // MOOCODE_TYPES_HPP
