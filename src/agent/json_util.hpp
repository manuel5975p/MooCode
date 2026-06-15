#ifndef MOOCODE_JSON_UTIL_HPP
#define MOOCODE_JSON_UTIL_HPP

// Thin, error-returning helpers over nlohmann::json. Every accessor reports a
// descriptive Error instead of throwing, so callers stay exception-free and can
// surface precise messages back to the model.

#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include <nlohmann/json.hpp>

#include "agent/types.hpp"

namespace moocode::json {

// Parse text into a JSON value. pre: none. post: on success a valid value.
// Error.msg names the byte offset / reason on malformed input.
std::expected<nlohmann::json, Error> parse(std::string_view text);

// Parse text, substituting `fallback` for malformed input instead of erroring.
// For embedding trusted/constant JSON (e.g. tool schema literals) where a parse
// failure is a build-time bug, never a runtime condition. Total (never throws).
nlohmann::json parse_or(std::string_view text,
                        nlohmann::json fallback = nlohmann::json::object());

// Pointer to member `key` when `value` is an object that contains it, else
// nullptr. Crash-free replacement for operator[] on optional/foreign JSON
// (const operator[] on a missing key is undefined behaviour). Total.
const nlohmann::json* find(const nlohmann::json& value, std::string_view key);

// Run `fn` (returning std::expected<T, Error>) and convert any thrown nlohmann
// or std exception into an Error tagged with `context`, so JSON handling at a
// boundary can never escape as an exception. pre: fn returns an expected<T,
// Error>. post: never throws.
template <class Fn>
std::invoke_result_t<Fn> guard(std::string_view context, Fn&& fn) {
    try {
        return std::forward<Fn>(fn)();
    } catch (const std::exception& e) {
        return std::unexpected(
            Error{.msg = std::string(context) + ": " + e.what(), .code = 0});
    } catch (...) {
        return std::unexpected(
            Error{.msg = std::string(context) + ": unknown JSON error", .code = 0});
    }
}

// Like guard, but for a plain-valued computation: returns fn(), or `fallback`
// on any thrown exception. For best-effort consumption of untrusted JSON on hot
// paths (e.g. streaming deltas) where the result degrades gracefully. Total.
template <class Fn>
std::invoke_result_t<Fn> guard_or(Fn&& fn, std::invoke_result_t<Fn> fallback) {
    try {
        return std::forward<Fn>(fn)();
    } catch (...) {
        return fallback;
    }
}

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

// Require an object field `key` to be a present, non-null string. Error names
// the key on absence/wrong-type. Common tool-argument extraction primitive.
std::expected<std::string, Error> arg_string(const nlohmann::json& value,
                                             std::string_view key);

// Optional string with a fallback default: absent/null -> def; wrong type ->
// Error. Convenience wrapper over get_string_opt that replaces std::nullopt
// with a caller-supplied default.
std::expected<std::string, Error> get_string_or_default(
    const nlohmann::json& value, std::string_view key, std::string def);

}  // namespace moocode::json

#endif  // MOOCODE_JSON_UTIL_HPP
