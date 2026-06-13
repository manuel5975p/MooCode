#ifndef FLAGENT_TYPES_HPP
#define FLAGENT_TYPES_HPP

// Core value types shared across every layer (http, json, provider, tools,
// agent). Header-only: pure data, no behavior, no implementation-only APIs.

#include <cassert>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace flagent {

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

// Legacy heuristic: true when a tool-result string starts with our serialized
// "ERROR: " prefix. Retained ONLY as the persistence loader's back-compat
// fallback for conversations written before `tool_failed` was a typed field;
// the live boundary now carries failure as data (Message::tool_failed), so do
// not introduce new callers.
inline bool is_tool_error(std::string_view s) { return s.rfind("ERROR: ", 0) == 0; }

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

    // Deserialization seam: build a Message field-by-field from persisted data
    // without exposing public setters. Used ONLY by the persist loader.
    static Message from_fields(Role role, std::string content,
                               std::vector<ToolCall> tool_calls,
                               std::string tool_call_id,
                               std::vector<ContentPart> parts, bool tool_failed,
                               std::string reasoning = {});

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
inline Message Message::from_fields(Role role, std::string content,
                                    std::vector<ToolCall> tool_calls,
                                    std::string tool_call_id,
                                    std::vector<ContentPart> parts,
                                    bool tool_failed, std::string reasoning) {
    Message m;
    m.role_ = role;
    m.content_ = std::move(content);
    m.tool_calls_ = std::move(tool_calls);
    m.tool_call_id_ = std::move(tool_call_id);
    m.parts_ = std::move(parts);
    m.tool_failed_ = tool_failed;
    m.reasoning_ = std::move(reasoning);
    return m;
}

using Conversation = std::vector<Message>;

// A tool as advertised to the model (JSON-Schema described parameters).
struct ToolSpec {
    std::string name;
    std::string description;
    nlohmann::json parameters;  // JSON Schema object
};

}  // namespace flagent

#endif  // FLAGENT_TYPES_HPP
