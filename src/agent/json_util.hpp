#ifndef FLAGENT_JSON_UTIL_HPP
#define FLAGENT_JSON_UTIL_HPP

// Thin, error-returning helpers over nlohmann::json. Every accessor reports a
// descriptive Error instead of throwing, so callers stay exception-free and can
// surface precise messages back to the model.

#include <expected>
#include <optional>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

#include "agent/types.hpp"

namespace flagent::json {

// Parse text into a JSON value. pre: none. post: on success a valid value.
// Error.msg names the byte offset / reason on malformed input.
std::expected<nlohmann::json, Error> parse(std::string_view text);

// Compact serialization of any value. Total (never fails).
std::string dump(const nlohmann::json& value);

// Pretty (2-space) serialization, for human-facing output. Total.
std::string dump_pretty(const nlohmann::json& value);

// Require `value` be an object containing `key` of the requested type.
// On type/shape mismatch returns Error naming the key and expected type.
std::expected<std::string, Error> get_string(const nlohmann::json& value,
                                             std::string_view key);
std::expected<std::int64_t, Error> get_int(const nlohmann::json& value,
                                           std::string_view key);
std::expected<const nlohmann::json*, Error> get_array(const nlohmann::json& value,
                                                      std::string_view key);

// Optional string field: absent/null -> std::nullopt; present-but-wrong-type ->
// Error. Distinguishes "missing" from "malformed", which get_string conflates.
std::expected<std::optional<std::string>, Error> get_string_opt(
    const nlohmann::json& value, std::string_view key);

// String field `key` if present and string-typed, else `def`. Tolerant of a
// non-object `value`. Total (never errors).
std::string get_string_or(const nlohmann::json& value, std::string_view key,
                          std::string_view def = "");

}  // namespace flagent::json

#endif  // FLAGENT_JSON_UTIL_HPP
