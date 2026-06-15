#include "agent/json_util.hpp"

#include "test_harness.hpp"

using namespace moocode;

TEST("parse: valid object round-trips") {
    auto v = json::parse(R"({"a":1,"b":"x"})");
    CHECK(v.has_value());
    if (v) {
        CHECK((*v)["a"] == 1);
        CHECK((*v)["b"] == "x");
    }
}

TEST("parse: malformed input returns Error") {
    auto v = json::parse("{not valid");
    CHECK(!v.has_value());
    if (!v) CHECK(!v.error().msg.empty());
}

TEST("parse: empty input returns Error") {
    auto v = json::parse("");
    CHECK(!v.has_value());
}

TEST("parse: unicode survives round-trip") {
    auto v = json::parse(R"({"s":"héllo é 😀"})");
    CHECK(v.has_value());
    if (v) CHECK((*v)["s"].get<std::string>().find("héllo") != std::string::npos);
}

TEST("dump: compact, no spaces") {
    nlohmann::json j = {{"a", 1}, {"b", 2}};
    auto s = json::dump(j);
    CHECK(s.find(' ') == std::string::npos);
    CHECK_EQ(s, std::string(R"({"a":1,"b":2})"));
}

TEST("dump_pretty: has newlines") {
    nlohmann::json j = {{"a", 1}};
    auto s = json::dump_pretty(j);
    CHECK(s.find('\n') != std::string::npos);
}

TEST("dump then parse is identity for typical values") {
    nlohmann::json j = {{"n", 42}, {"arr", {1, 2, 3}}, {"nested", {{"k", "v"}}}};
    auto back = json::parse(json::dump(j));
    CHECK(back.has_value());
    if (back) CHECK(*back == j);
}

TEST("get_string: present field") {
    auto j = json::parse(R"({"name":"moocode"})").value();
    auto s = json::get_string(j, "name");
    CHECK(s.has_value());
    if (s) CHECK_EQ(*s, std::string("moocode"));
}

TEST("get_string: missing field returns Error naming the key") {
    auto j = json::parse(R"({"other":1})").value();
    auto s = json::get_string(j, "name");
    CHECK(!s.has_value());
    if (!s) CHECK(s.error().msg.find("name") != std::string::npos);
}

TEST("get_string: wrong type returns Error") {
    auto j = json::parse(R"({"name":123})").value();
    auto s = json::get_string(j, "name");
    CHECK(!s.has_value());
}

TEST("get_string: on non-object returns Error") {
    auto j = json::parse("[1,2,3]").value();
    auto s = json::get_string(j, "name");
    CHECK(!s.has_value());
}

TEST("get_int: present integer") {
    auto j = json::parse(R"({"n":7})").value();
    auto n = json::get_int(j, "n");
    CHECK(n.has_value());
    if (n) CHECK_EQ(*n, std::int64_t{7});
}

TEST("get_int: negative and large values") {
    auto j = json::parse(R"({"n":-2147483648, "big": 9000000000})").value();
    CHECK_EQ(json::get_int(j, "n").value(), std::int64_t{-2147483648LL});
    CHECK_EQ(json::get_int(j, "big").value(), std::int64_t{9000000000LL});
}

TEST("get_int: string value returns Error") {
    auto j = json::parse(R"({"n":"7"})").value();
    CHECK(!json::get_int(j, "n").has_value());
}

TEST("get_array: returns pointer to array") {
    auto j = json::parse(R"({"a":[1,2,3]})").value();
    auto a = json::get_array(j, "a");
    CHECK(a.has_value());
    if (a) CHECK_EQ((*a)->size(), size_t{3});
}

TEST("get_string_opt: missing -> nullopt, not error") {
    auto j = json::parse(R"({"x":1})").value();
    auto s = json::get_string_opt(j, "name");
    CHECK(s.has_value());            // no error
    if (s) CHECK(!s->has_value());   // but empty optional
}

TEST("get_string_opt: null -> nullopt") {
    auto j = json::parse(R"({"name":null})").value();
    auto s = json::get_string_opt(j, "name");
    CHECK(s.has_value());
    if (s) CHECK(!s->has_value());
}

TEST("get_string_opt: present string -> value") {
    auto j = json::parse(R"({"name":"x"})").value();
    auto s = json::get_string_opt(j, "name");
    CHECK(s.has_value());
    if (s && *s) CHECK_EQ(**s, std::string("x"));
}

TEST("get_string_opt: wrong type -> error") {
    auto j = json::parse(R"({"name":5})").value();
    auto s = json::get_string_opt(j, "name");
    CHECK(!s.has_value());
}

// --- parse_or: total parse with fallback -----------------------------------

TEST("parse_or: valid input parses") {
    auto j = json::parse_or(R"({"a":1})");
    CHECK(j.is_object());
    if (j.is_object()) CHECK_EQ(j["a"], 1);
}

TEST("parse_or: malformed input yields the default empty object") {
    auto j = json::parse_or("{not json");
    CHECK(j.is_object());
    CHECK(j.empty());
}

TEST("parse_or: malformed input yields a custom fallback") {
    auto j = json::parse_or("nope", nlohmann::json::array());
    CHECK(j.is_array());
}

TEST("parse_or: empty input yields fallback") {
    CHECK(json::parse_or("").is_object());
}

// --- find: crash-free optional member navigation ---------------------------

TEST("find: returns pointer to an existing member") {
    auto j = json::parse(R"({"k":"v"})").value();
    const auto* p = json::find(j, "k");
    CHECK(p != nullptr);
    if (p) CHECK_EQ(p->get<std::string>(), std::string("v"));
}

TEST("find: missing key -> nullptr") {
    auto j = json::parse(R"({"k":1})").value();
    CHECK(json::find(j, "absent") == nullptr);
}

TEST("find: non-object values -> nullptr, never throw") {
    CHECK(json::find(json::parse("[1,2,3]").value(), "k") == nullptr);
    CHECK(json::find(json::parse("5").value(), "k") == nullptr);
    CHECK(json::find(json::parse("\"s\"").value(), "k") == nullptr);
    CHECK(json::find(json::parse("null").value(), "k") == nullptr);
}

// --- guard: exception boundary for expected-returning code -----------------

TEST("guard: passes a successful expected through unchanged") {
    auto r = json::guard("ctx", []() -> std::expected<int, Error> { return 42; });
    CHECK(r.has_value());
    if (r) CHECK_EQ(*r, 42);
}

TEST("guard: converts a thrown nlohmann exception into an Error with context") {
    auto r = json::guard("myctx", []() -> std::expected<std::string, Error> {
        nlohmann::json n = 5;          // a number
        return n.get<std::string>();   // throws type_error
    });
    CHECK(!r.has_value());
    if (!r) CHECK(r.error().msg.find("myctx") != std::string::npos);
}

TEST("guard: converts a thrown parse_error into an Error") {
    auto r = json::guard("p", []() -> std::expected<nlohmann::json, Error> {
        return nlohmann::json::parse("{ broken");  // throwing parse
    });
    CHECK(!r.has_value());
}

TEST("guard: preserves an explicit unexpected (no exception thrown)") {
    auto r = json::guard("ctx", []() -> std::expected<int, Error> {
        return std::unexpected(Error{.msg = "boom", .code = 7});
    });
    CHECK(!r.has_value());
    if (!r) {
        CHECK_EQ(r.error().msg, std::string("boom"));
        CHECK_EQ(r.error().code, 7);
    }
}

// --- guard_or: exception boundary for plain-valued code --------------------

TEST("guard_or: returns the value when nothing throws") {
    CHECK_EQ(json::guard_or([] { return 99; }, -1), 99);
}

TEST("guard_or: returns the fallback on a thrown exception") {
    int v = json::guard_or(
        []() -> int {
            nlohmann::json n = 5;
            return n.at("missing").get<int>();  // throws type_error
        },
        -1);
    CHECK_EQ(v, -1);
}
