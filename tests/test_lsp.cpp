#include "agent/lsp_client.hpp"

#include "agent/clangd_tools.hpp"
#include "agent/lsp_detail.hpp"  // Framed, try_parse_frame, frame (white-box tests)

#include <cstdlib>
#include <filesystem>

#include "test_harness.hpp"

using namespace moocode;
using namespace moocode::lsp;
namespace fs = std::filesystem;

// --- pure helpers: framing --------------------------------------------------

TEST("frame: round-trips through try_parse_frame") {
    nlohmann::json msg{{"jsonrpc", "2.0"}, {"id", 7}, {"method", "x"}};
    std::string wire = frame(msg);
    auto f = try_parse_frame(wire);
    CHECK(f.has_value());
    CHECK(f->has_value());
    if (f && f->has_value()) {
        CHECK_EQ((*f)->consumed, wire.size());
        CHECK((*f)->msg == msg);
    }
}

TEST("try_parse_frame: incomplete body returns need-more (nullopt)") {
    std::string wire = "Content-Length: 20\r\n\r\n{\"a\":1}";  // body shorter than 20
    auto f = try_parse_frame(wire);
    CHECK(f.has_value());
    CHECK(!f->has_value());  // nullopt: need more bytes
}

TEST("try_parse_frame: header not yet terminated returns need-more") {
    auto f = try_parse_frame("Content-Length: 5\r\n");
    CHECK(f.has_value());
    CHECK(!f->has_value());
}

TEST("try_parse_frame: missing Content-Length is an error") {
    auto f = try_parse_frame("X-Foo: bar\r\n\r\n{}");
    CHECK(!f.has_value());
}

TEST("try_parse_frame: malformed JSON body is an error") {
    auto f = try_parse_frame("Content-Length: 3\r\n\r\n{ x");
    CHECK(!f.has_value());
}

TEST("try_parse_frame: oversized Content-Length is an error, not a crash") {
    auto f = try_parse_frame("Content-Length: 99999999999999999999\r\n\r\n{}");
    CHECK(!f.has_value());  // rejected by the 256 MiB cap, no overflow/hang
}

TEST("try_parse_frame: consumes exactly one frame from a concatenation") {
    std::string two = frame(nlohmann::json{{"id", 1}}) + frame(nlohmann::json{{"id", 2}});
    auto f = try_parse_frame(two);
    CHECK(f.has_value() && f->has_value());
    if (f && f->has_value()) {
        CHECK_EQ((*f)->msg["id"].get<int>(), 1);
        // Remainder still parses as the second frame.
        auto g = try_parse_frame(std::string_view(two).substr((*f)->consumed));
        CHECK(g.has_value() && g->has_value());
        if (g && g->has_value()) CHECK_EQ((*g)->msg["id"].get<int>(), 2);
    }
}

TEST("try_parse_frame: Content-Length header is case-insensitive") {
    std::string wire = "content-length: 2\r\n\r\n{}";
    auto f = try_parse_frame(wire);
    CHECK(f.has_value() && f->has_value());
}

// --- pure helpers: URI <-> path ---------------------------------------------

TEST("path/uri: round-trip a plain absolute path") {
    std::string uri = path_to_uri("/Users/me/proj/src/a.cpp");
    CHECK_EQ(uri, std::string("file:///Users/me/proj/src/a.cpp"));
    auto back = uri_to_path(uri);
    CHECK(back.has_value());
    if (back) CHECK_EQ(back->generic_string(), std::string("/Users/me/proj/src/a.cpp"));
}

TEST("path/uri: spaces and reserved bytes are percent-encoded and decoded") {
    std::string uri = path_to_uri("/tmp/a b+c#d.cpp");
    CHECK(uri.find(' ') == std::string::npos);
    CHECK(uri.find("%20") != std::string::npos);
    auto back = uri_to_path(uri);
    CHECK(back.has_value());
    if (back) CHECK_EQ(back->generic_string(), std::string("/tmp/a b+c#d.cpp"));
}

TEST("uri_to_path: rejects a non-file URI") {
    CHECK(!uri_to_path("http://example.com/x").has_value());
}

// --- pure helpers: column math ----------------------------------------------

TEST("column math: ASCII is identity in both encodings") {
    std::string_view line = "  resolve_in_root(root, rel)";
    CHECK_EQ(byte_to_lsp_char(line, 2, PosEncoding::Utf8), 2);
    CHECK_EQ(byte_to_lsp_char(line, 2, PosEncoding::Utf16), 2);
    CHECK_EQ(lsp_char_to_byte(line, 2, PosEncoding::Utf8), std::size_t{2});
    CHECK_EQ(lsp_char_to_byte(line, 2, PosEncoding::Utf16), std::size_t{2});
}

TEST("column math: astral char counts as 2 UTF-16 units, 4 bytes") {
    std::string line = "x\xF0\x9F\x98\x80y";  // "x" + U+1F600 + "y"
    // byte offset of 'y' is 5 (1 + 4).
    CHECK_EQ(byte_to_lsp_char(line, 5, PosEncoding::Utf8), 5);
    CHECK_EQ(byte_to_lsp_char(line, 5, PosEncoding::Utf16), 3);  // 1 + 2
    CHECK_EQ(lsp_char_to_byte(line, 3, PosEncoding::Utf16), std::size_t{5});
    CHECK_EQ(lsp_char_to_byte(line, 5, PosEncoding::Utf8), std::size_t{5});
}

// --- pure helpers: position_of_symbol ---------------------------------------

TEST("position_of_symbol: finds the identifier and reports 0-based line") {
    std::string_view line = "    return resolve_in_root(opts.root, path);";
    auto p = position_of_symbol(line, 10, "resolve_in_root", 1, PosEncoding::Utf8);
    CHECK(p.has_value());
    if (p) {
        CHECK_EQ(p->line, 9);                       // 1-based 10 -> 0-based 9
        CHECK_EQ(p->character, 11);                  // byte offset of the match
    }
}

TEST("position_of_symbol: occurrence selects the Nth match") {
    std::string_view line = "x = foo + foo;";
    auto p1 = position_of_symbol(line, 1, "foo", 1, PosEncoding::Utf8);
    auto p2 = position_of_symbol(line, 1, "foo", 2, PosEncoding::Utf8);
    CHECK(p1.has_value() && p2.has_value());
    if (p1 && p2) {
        CHECK_EQ(p1->character, 4);
        CHECK_EQ(p2->character, 10);
    }
}

TEST("position_of_symbol: missing identifier is an error") {
    CHECK(!position_of_symbol("int x;", 1, "nope", 1, PosEncoding::Utf8).has_value());
    CHECK(!position_of_symbol("foo", 1, "", 1, PosEncoding::Utf8).has_value());
}

// --- live clangd smoke test (skipped when clangd is unavailable) -------------

namespace {
std::string find_clangd() {
    if (const char* e = std::getenv("CLANGD_PATH"); e && *e) return e;
    for (const char* c : {"/opt/homebrew/opt/llvm/bin/clangd", "/usr/bin/clangd",
                          "/usr/local/bin/clangd"}) {
        std::error_code ec;
        if (fs::exists(c, ec)) return c;
    }
    return {};
}
// Repo root derived from this file's path: <root>/tests/test_lsp.cpp.
fs::path repo_root() { return fs::path(__FILE__).parent_path().parent_path(); }
}  // namespace

TEST("clangd: find_references resolves a real symbol (live)") {
    std::string clangd = find_clangd();
    if (clangd.empty()) {
        std::fprintf(stderr, "    (skipped: clangd not found)\n");
        return;
    }
    fs::path root = repo_root();
    if (!fs::exists(root / "src/agent/fsutil.hpp")) {
        std::fprintf(stderr, "    (skipped: repo layout not found at %s)\n", root.c_str());
        return;
    }
    ClangdToolsConfig cfg;
    cfg.root = root;
    cfg.clangd_path = clangd;
    std::error_code ec;
    if (fs::exists(root / "build/compile_commands.json", ec))
        cfg.compile_commands_dir = root / "build";
    cfg.index_wait_ms = 3000;  // brief; same-file refs return at once, full index is slow

    auto session = make_clangd_session(cfg);
    Tool refs = find_references_tool(session);
    // resolve_in_root is declared at fsutil.hpp:26 (inline header function).
    auto out = refs.run(nlohmann::json{{"path", "src/agent/fsutil.hpp"},
                                       {"line", 26},
                                       {"symbol", "resolve_in_root"}});
    CHECK(out.has_value());
    if (out) {
        std::fprintf(stderr, "    find_references ->\n%s\n", out->c_str());
        // Either real references, or a clean "still indexing"/"no references" string;
        // a transport failure would have produced an Error instead.
        CHECK(!out->empty());
    } else {
        std::fprintf(stderr, "    find_references error: %s\n", out.error().msg.c_str());
    }
}
