#include "agent/json_util.hpp"

#include <string>

namespace flagent::json {

namespace {

// Format a "field <key>: <what>" error message uniformly.
Error field_error(std::string_view key, std::string_view what) {
    return Error{.msg = "field '" + std::string(key) + "' " + std::string(what), .code = 0};
}

// Resolve an object member, or report why it is unusable. Returns nullptr with
// a populated `err` when value is not an object or the key is absent.
const nlohmann::json* member(const nlohmann::json& value, std::string_view key,
                             Error& err) {
    if (!value.is_object()) {
        err = Error{.msg = "expected JSON object", .code = 0};
        return nullptr;
    }
    auto it = value.find(std::string(key));
    if (it == value.end()) {
        err = field_error(key, "missing");
        return nullptr;
    }
    return &*it;
}

}  // namespace

std::expected<nlohmann::json, Error> parse(std::string_view text) {
    // allow_exceptions=false => discarded sentinel instead of throw.
    auto v = nlohmann::json::parse(text, /*cb=*/nullptr, /*allow_exceptions=*/false);
    if (v.is_discarded())
        return std::unexpected(Error{.msg = "malformed JSON", .code = 0});
    return v;
}

std::string dump(const nlohmann::json& value) { return value.dump(); }

std::string dump_pretty(const nlohmann::json& value) { return value.dump(2); }

std::expected<std::string, Error> get_string(const nlohmann::json& value,
                                             std::string_view key) {
    Error err;
    const nlohmann::json* m = member(value, key, err);
    if (!m) return std::unexpected(err);
    if (!m->is_string()) return std::unexpected(field_error(key, "is not a string"));
    return m->get<std::string>();
}

std::expected<std::int64_t, Error> get_int(const nlohmann::json& value,
                                           std::string_view key) {
    Error err;
    const nlohmann::json* m = member(value, key, err);
    if (!m) return std::unexpected(err);
    if (!m->is_number_integer())
        return std::unexpected(field_error(key, "is not an integer"));
    return m->get<std::int64_t>();
}

std::expected<const nlohmann::json*, Error> get_array(const nlohmann::json& value,
                                                      std::string_view key) {
    Error err;
    const nlohmann::json* m = member(value, key, err);
    if (!m) return std::unexpected(err);
    if (!m->is_array()) return std::unexpected(field_error(key, "is not an array"));
    return m;
}

std::expected<std::optional<std::string>, Error> get_string_opt(
    const nlohmann::json& value, std::string_view key) {
    if (!value.is_object()) return std::unexpected(Error{.msg = "expected JSON object", .code = 0});
    auto it = value.find(std::string(key));
    if (it == value.end() || it->is_null()) return std::optional<std::string>{};
    if (!it->is_string()) return std::unexpected(field_error(key, "is not a string"));
    return std::optional<std::string>{it->get<std::string>()};
}

std::string get_string_or(const nlohmann::json& value, std::string_view key,
                          std::string_view def) {
    if (!value.is_object()) return std::string(def);
    auto it = value.find(std::string(key));
    if (it == value.end() || !it->is_string()) return std::string(def);
    return it->get<std::string>();
}

}  // namespace flagent::json
