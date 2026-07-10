// Tests for the agent_mentions module: @-token extraction, sandbox, globs,
// suggestion extraction, deduplication, truncation, and directory listings.

#include "agent/mentions.hpp"

#include <algorithm>
#include <filesystem>
#include <string>

#include "temp_dir.hpp"
#include "test_harness.hpp"

using namespace moocode;

namespace {
MentionOptions opts_for(const test::TempDir& d) {
    MentionOptions o;
    o.root = d.path();
    o.max_file_bytes = 1024;          // 1 KiB cap to exercise truncation
    o.max_total_bytes = 8 * 1024;     // 8 KiB aggregate cap
    o.max_files = 8;
    return o;
}
}  // namespace

// --- tokenisation -----------------------------------------------------------

TEST("mentions: a prompt with no @-tokens is returned unchanged") {
    test::TempDir d;
    auto r = expand_mentions("hello world, no mention here", opts_for(d));
    CHECK_EQ(r.entries.size(), std::size_t{0});
    CHECK_EQ(r.prompt, std::string("hello world, no mention here"));
}

TEST("mentions: a single file mention is read and attached") {
    test::TempDir d;
    d.write("a.cpp", "int main() { return 0; }\n");
    auto r = expand_mentions("what does @a.cpp do?", opts_for(d));
    CHECK_EQ(r.entries.size(), std::size_t{1});
    CHECK(r.prompt.find("what does @a.cpp do?") != std::string::npos);
    CHECK(r.prompt.find("### @a.cpp") != std::string::npos);
    CHECK(r.prompt.find("int main()") != std::string::npos);
    CHECK(r.entries[0].error.empty());
    CHECK_EQ(r.entries[0].kind, std::string("C/C++ source"));
    CHECK(r.entries[0].lines > 0);
    CHECK(r.entries[0].bytes > 0);
}

TEST("mentions: email-like tokens are NOT treated as paths") {
    test::TempDir d;
    auto r = expand_mentions("contact me at user@example.com please", opts_for(d));
    CHECK_EQ(r.entries.size(), std::size_t{0});
    CHECK_EQ(r.prompt, std::string("contact me at user@example.com please"));
}

TEST("mentions: stray @ with no path is ignored") {
    test::TempDir d;
    auto r = expand_mentions("a bare @ symbol and @@ doubled and @ with space", opts_for(d));
    CHECK_EQ(r.entries.size(), std::size_t{0});
}

TEST("mentions: @ must be at start-of-input or after whitespace") {
    test::TempDir d;
    d.write("a.cpp", "x");
    // "test@a.cpp" has the @ glued to "test", which is the email case -> rejected.
    auto r = expand_mentions("test@a.cpp is a file", opts_for(d));
    CHECK_EQ(r.entries.size(), std::size_t{0});
    // After whitespace it works.
    auto r2 = expand_mentions("file is @a.cpp", opts_for(d));
    CHECK_EQ(r2.entries.size(), std::size_t{1});
}

TEST("mentions: terminators end the path token (commas, parens, etc.)") {
    test::TempDir d;
    d.write("a.cpp", "x");
    auto r = expand_mentions("look at @a.cpp, also @a.cpp.", opts_for(d));
    CHECK_EQ(r.entries.size(), std::size_t{1});  // dedup'd
    CHECK_EQ(r.entries[0].path, std::string("a.cpp"));
}

// --- outside-root access ----------------------------------------------------
// @-mentions carry the user's own authority (the user typed the path), so they
// are never sandbox-confined — unlike model-issued read_file tool calls.

TEST("mentions: an existing file outside the root resolves (no sandbox)") {
    test::TempDir root, outside;
    outside.write("escape.cpp", "int x;\n");
    auto abs = (outside.path() / "escape.cpp").string();
    auto r = expand_mentions("please read @" + abs, opts_for(root));
    CHECK_EQ(r.entries.size(), std::size_t{1});
    CHECK(r.entries[0].error.empty());
    CHECK(r.prompt.find("int x;") != std::string::npos);
}

TEST("mentions: a missing path outside the root reports a non-sandbox error") {
    test::TempDir root, outside;
    auto abs = (outside.path() / "nope.cpp").string();
    auto r = expand_mentions("please read @" + abs, opts_for(root));
    CHECK_EQ(r.entries.size(), std::size_t{1});
    CHECK(!r.entries[0].error.empty());
    CHECK(r.entries[0].error.find("sandbox") == std::string::npos);
}

TEST("mentions: missing files are reported as errors, prompt still augmented") {
    test::TempDir d;
    auto r = expand_mentions("explain @missing.cpp", opts_for(d));
    CHECK_EQ(r.entries.size(), std::size_t{1});
    CHECK(!r.entries[0].error.empty());
    CHECK(r.prompt.find("explain @missing.cpp") != std::string::npos);
}

// --- deduplication ---------------------------------------------------------

TEST("mentions: the same file mentioned twice is attached once") {
    test::TempDir d;
    d.write("a.cpp", "x");
    auto r = expand_mentions("first @a.cpp and again @a.cpp", opts_for(d));
    CHECK_EQ(r.entries.size(), std::size_t{1});
}

// --- globs -----------------------------------------------------------------

TEST("mentions: glob @src/*.cpp matches all cpp files in src") {
    test::TempDir d;
    d.write("src/a.cpp", "// a");
    d.write("src/b.cpp", "// b");
    d.write("src/c.txt", "not a cpp");
    auto r = expand_mentions("scan @src/*.cpp for bugs", opts_for(d));
    CHECK_EQ(r.entries.size(), std::size_t{2});
    // Each entry's path field carries "<glob> -> <actual>".
    std::vector<std::string> got;
    for (const auto& e : r.entries) got.push_back(e.path);
    std::ranges::sort(got);
    CHECK(std::find(got.begin(), got.end(), "src/*.cpp -> " + d.path().string() + "/src/a.cpp") != got.end());
    CHECK(std::find(got.begin(), got.end(), "src/*.cpp -> " + d.path().string() + "/src/b.cpp") != got.end());
}

TEST("mentions: glob with no matches is reported as an error") {
    test::TempDir d;
    auto r = expand_mentions("nothing here @*.zzz", opts_for(d));
    CHECK_EQ(r.entries.size(), std::size_t{1});
    CHECK(!r.entries[0].error.empty());
}

// --- directories -----------------------------------------------------------

TEST("mentions: a directory mention produces a non-recursive listing") {
    test::TempDir d;
    d.write("sub/a.txt", "a");
    d.write("sub/b.txt", "b");
    auto r = expand_mentions("show @sub/", opts_for(d));
    CHECK_EQ(r.entries.size(), std::size_t{1});
    CHECK_EQ(r.entries[0].kind, std::string("directory listing"));
    CHECK(r.entries[0].content.find("a.txt") != std::string::npos);
    CHECK(r.entries[0].content.find("b.txt") != std::string::npos);
}

// --- truncation / caps ----------------------------------------------------

TEST("mentions: a file larger than max_file_bytes is truncated") {
    test::TempDir d;
    d.write("big.cpp", std::string(4096, 'x'));
    auto opts = opts_for(d);
    opts.max_file_bytes = 64;
    auto r = expand_mentions("look at @big.cpp", opts);
    CHECK_EQ(r.entries.size(), std::size_t{1});
    CHECK(!r.entries[0].error.empty() == false);
    // Truncation marker is appended.
    CHECK(r.entries[0].content.find("truncated") != std::string::npos);
    CHECK(r.entries[0].content.size() < 4096);
    // The marker reports shown/original, not original/original (regression).
    CHECK(r.entries[0].content.find("64/4096 bytes shown") != std::string::npos);
    // The reported size is the original (pre-truncation) size.
    CHECK_EQ(r.entries[0].bytes, std::size_t{4096});
}

TEST("mentions: max_total_bytes stops reading further attachments") {
    test::TempDir d;
    d.write("a.cpp", std::string(500, 'a'));
    d.write("b.cpp", std::string(500, 'b'));
    d.write("c.cpp", std::string(500, 'c'));
    auto opts = opts_for(d);
    opts.max_total_bytes = 600;  // fits a.cpp + a slice of b.cpp
    auto r = expand_mentions("see @a.cpp @b.cpp @c.cpp", opts);
    CHECK(r.entries.size() >= 2);
    // The third file (c.cpp) should be either truncated or reported as capped.
    const auto& last = r.entries.back();
    bool capped = !last.error.empty() &&
                  last.error.find("max_total_bytes") != std::string::npos;
    CHECK(capped);
}

// --- suggestions -----------------------------------------------------------

TEST("mentions: C++ suggestions list class/struct/function names") {
    test::TempDir d;
    d.write("c.cpp",
            "class Alpha {\n"
            "  void run();\n"
            "};\n"
            "struct Beta { int x; };\n"
            "namespace gamma { void f(); }\n"
            "int free_function(int n) { return n; }\n");
    auto r = expand_mentions("summarise @c.cpp", opts_for(d));
    CHECK_EQ(r.entries.size(), std::size_t{1});
    const auto& s = r.entries[0].suggestions;
    CHECK(std::find(s.begin(), s.end(), "Alpha") != s.end());
    CHECK(std::find(s.begin(), s.end(), "Beta") != s.end());
    CHECK(std::find(s.begin(), s.end(), "gamma") != s.end());
}

TEST("mentions: Python suggestions list def/class names") {
    test::TempDir d;
    d.write("p.py",
            "import os\n"
            "def alpha(x):\n"
            "    return x\n"
            "class Beta:\n"
            "    def method(self):\n"
            "        pass\n");
    auto r = expand_mentions("@p.py", opts_for(d));
    CHECK_EQ(r.entries.size(), std::size_t{1});
    const auto& s = r.entries[0].suggestions;
    CHECK(std::find(s.begin(), s.end(), "alpha") != s.end());
    CHECK(std::find(s.begin(), s.end(), "Beta") != s.end());
    CHECK(std::find(s.begin(), s.end(), "method") != s.end());
}

// Characterization of the verbatim Rust pattern: only the `impl … for X` form
// has whitespace before its capture group, so X is the one reliably extracted
// identifier. (struct/fn lines do not capture under the original pattern; this
// test pins that behaviour so the regex-hoist provably did not change it.)
TEST("mentions: Rust suggestions extract impl-for target") {
    test::TempDir d;
    d.write("r.rs",
            "struct Widget;\n"
            "fn build() {}\n"
            "impl Render for Widget {\n"
            "    fn draw(&self) {}\n"
            "}\n");
    auto r = expand_mentions("@r.rs", opts_for(d));
    CHECK_EQ(r.entries.size(), std::size_t{1});
    const auto& s = r.entries[0].suggestions;
    CHECK(std::find(s.begin(), s.end(), "Widget") != s.end());
}

TEST("mentions: Java suggestions handle modifier lines") {
    test::TempDir d;
    d.write("J.java",
            "package x;\n"
            "public final class Foo {\n"
            "    interface Bar {}\n"
            "}\n");
    auto r = expand_mentions("@J.java", opts_for(d));
    CHECK_EQ(r.entries.size(), std::size_t{1});
    const auto& s = r.entries[0].suggestions;
    CHECK(std::find(s.begin(), s.end(), "Foo") != s.end());
    CHECK(std::find(s.begin(), s.end(), "Bar") != s.end());
}

// Calling twice on the same content must yield identical suggestion vectors.
// Guards the hoisted-static regex path against any match drift.
TEST("mentions: code_suggestions is deterministic across repeated calls") {
    test::TempDir d;
    d.write("c.cpp",
            "class Alpha {\n"
            "struct Beta {};\n"
            "namespace gamma {}\n");
    auto r1 = expand_mentions("@c.cpp", opts_for(d));
    auto r2 = expand_mentions("@c.cpp", opts_for(d));
    CHECK_EQ(r1.entries.size(), std::size_t{1});
    CHECK_EQ(r2.entries.size(), std::size_t{1});
    CHECK(r1.entries[0].suggestions == r2.entries[0].suggestions);
}

TEST("mentions: markdown suggestions list headings, indented by level") {
    test::TempDir d;
    d.write("doc.md",
            "# Title\n"
            "Intro paragraph.\n"
            "## Section A\n"
            "### Subsection\n"
            "## Section B\n");
    auto r = expand_mentions("what's in @doc.md?", opts_for(d));
    CHECK_EQ(r.entries.size(), std::size_t{1});
    const auto& s = r.entries[0].suggestions;
    CHECK(std::find(s.begin(), s.end(), "Title") != s.end());
    CHECK(std::find(s.begin(), s.end(), "Section A") != s.end());
    CHECK(std::find(s.begin(), s.end(), "Subsection") != s.end());
    CHECK(std::find(s.begin(), s.end(), "Section B") != s.end());
}

TEST("mentions: include_suggestions=false yields no suggestions") {
    test::TempDir d;
    d.write("c.cpp", "class Alpha { void f(); };\n");
    auto opts = opts_for(d);
    opts.include_suggestions = false;
    auto r = expand_mentions("@c.cpp", opts);
    CHECK_EQ(r.entries.size(), std::size_t{1});
    CHECK(r.entries[0].suggestions.empty());
}

// --- binary / non-text ----------------------------------------------------

TEST("mentions: a binary file is detected and not pasted verbatim") {
    test::TempDir d;
    // Magic bytes with a NUL — looks_binary() tests for a NUL in the first
    // 8 KiB. Using char literals avoids hex-escape terminator confusion.
    std::string bin;
    bin.push_back(static_cast<char>(0x7F));
    bin += "ELF";
    bin.push_back('\0');
    bin.push_back(static_cast<char>(0x01));
    bin.push_back(static_cast<char>(0x02));
    d.write("img.bin", bin);
    auto r = expand_mentions("what is @img.bin?", opts_for(d));
    CHECK_EQ(r.entries.size(), std::size_t{1});
    CHECK_EQ(r.entries[0].kind, std::string("binary"));
    CHECK_EQ(r.entries[0].lines, std::size_t{0});
    CHECK(r.entries[0].content.find("binary file") != std::string::npos);
}

// --- kind detection -------------------------------------------------------

TEST("mentions: kind is detected from extension") {
    test::TempDir d;
    d.write("x.py", "def f(): return 1\n");
    d.write("y.md", "# h\n");
    d.write("z.json", "{}\n");
    auto r = expand_mentions("first @x.py then @y.md then @z.json", opts_for(d));
    CHECK_EQ(r.entries.size(), std::size_t{3});
    // Order matches prompt order.
    CHECK_EQ(r.entries[0].kind, std::string("Python source"));
    CHECK_EQ(r.entries[1].kind, std::string("Markdown"));
    CHECK_EQ(r.entries[2].kind, std::string("JSON"));
}

// --- prompt structure -----------------------------------------------------

TEST("mentions: augmented prompt has original then a single Attached section") {
    test::TempDir d;
    d.write("a.cpp", "x");
    d.write("b.md", "y");
    auto r = expand_mentions("first @a.cpp then @b.md", opts_for(d));
    CHECK(r.prompt.find("first @a.cpp then @b.md") != std::string::npos);
    const auto pos = r.prompt.find("auto-attached via @-mentions");
    CHECK(pos != std::string::npos);
    // The attached section must come AFTER the original prompt.
    CHECK(pos > r.prompt.find("first @a.cpp then @b.md"));
    // The two attachments both appear.
    CHECK(r.prompt.find("### @a.cpp") != std::string::npos);
    CHECK(r.prompt.find("### @b.md") != std::string::npos);
}

TEST("mentions: the original @-token stays verbatim in the prompt") {
    test::TempDir d;
    d.write("a.cpp", "x");
    auto r = expand_mentions("what is @a.cpp?", opts_for(d));
    CHECK(r.prompt.find("@a.cpp?") != std::string::npos);
}

// --- autocomplete: mention_context_at ---------------------------------------

TEST("context: cursor outside any @-token is inactive") {
    auto c = mention_context_at("hello world", 5);
    CHECK(!c.active);
}
TEST("context: cursor inside a leading @-token is active") {
    auto c = mention_context_at("@src/ag", 7);
    CHECK(c.active);
    CHECK_EQ(c.token_begin, std::size_t{1});
    CHECK_EQ(c.token_end, std::size_t{7});
    CHECK_EQ(c.typed, std::string("src/ag"));
}
TEST("context: @-token must follow whitespace or start") {
    auto c = mention_context_at("user@host", 9);
    CHECK(!c.active);
}
TEST("context: token after a space is found") {
    auto c = mention_context_at("explain @a.cpp", 14);
    CHECK(c.active);
    CHECK_EQ(c.typed, std::string("a.cpp"));
}
TEST("context: matching uses text up to cursor, span is whole token") {
    auto c = mention_context_at("@src/agent", 4);
    CHECK(c.active);
    CHECK_EQ(c.typed, std::string("src"));
    CHECK_EQ(c.token_end, std::size_t{10});
}
TEST("context: a space inside breaks the token") {
    auto c = mention_context_at("@src ag", 7);
    CHECK(!c.active);
}
TEST("context: bare @ at cursor is active with empty typed") {
    auto c = mention_context_at("see @", 5);
    CHECK(c.active);
    CHECK_EQ(c.typed, std::string(""));
}

// --- autocomplete: complete_mention / apply_completion ----------------------

TEST("complete: prefix matches dirs and files, dirs first") {
    test::TempDir d;
    d.write("src/agent/agent.cpp", "x");
    d.write("src/agent/agent.hpp", "x");
    d.write("src/aux.txt", "x");
    auto v = complete_mention("src/a", d.path());
    CHECK_EQ(v.size(), std::size_t{2});
    CHECK_EQ(v[0].insert, std::string("src/agent/"));
    CHECK(v[0].is_dir);
    CHECK_EQ(v[1].insert, std::string("src/aux.txt"));
    CHECK(!v[1].is_dir);
}
TEST("complete: case-insensitive prefix on the trailing segment") {
    test::TempDir d;
    d.write("README.md", "x");
    auto v = complete_mention("re", d.path());
    CHECK_EQ(v.size(), std::size_t{1});
    CHECK_EQ(v[0].insert, std::string("README.md"));
}
TEST("complete: bare token lists root directory entries") {
    test::TempDir d;
    d.write("a.txt", "x");
    d.write("b/c.txt", "x");
    auto v = complete_mention("", d.path());
    CHECK_EQ(v.size(), std::size_t{2});
    CHECK_EQ(v[0].insert, std::string("b/"));
    CHECK_EQ(v[1].insert, std::string("a.txt"));
}
TEST("complete: dotfiles hidden unless segment starts with '.'") {
    test::TempDir d;
    d.write(".hidden", "x");
    d.write("visible.txt", "x");
    CHECK_EQ(complete_mention("", d.path()).size(), std::size_t{1});
    auto dotted = complete_mention(".", d.path());
    CHECK_EQ(dotted.size(), std::size_t{1});
    CHECK_EQ(dotted[0].insert, std::string(".hidden"));
}
TEST("complete: sandbox escape yields no candidates") {
    test::TempDir d;
    auto v = complete_mention("../", d.path());
    CHECK_EQ(v.size(), std::size_t{0});
}
TEST("complete: missing directory yields no candidates") {
    test::TempDir d;
    auto v = complete_mention("nope/x", d.path());
    CHECK_EQ(v.size(), std::size_t{0});
}
TEST("apply: splices the whole token and moves the cursor to its end") {
    std::string line = "explain @src/ag and stuff";
    auto ctx = mention_context_at(line, 15);  // cursor at end of "@src/ag"
    CHECK(ctx.active);
    MentionCompletion c{"src/agent/", "src/agent/", true};
    auto [out, cur] = apply_completion(line, ctx, c);
    CHECK_EQ(out, std::string("explain @src/agent/ and stuff"));
    CHECK_EQ(cur, std::size_t{19});
}

TEST("apply: an inactive context returns the line verbatim, cursor at end") {
    std::string line = "no mention here";
    MentionContext ctx{};  // default: active=false
    MentionCompletion c{"x", "x", false};
    auto [out, cur] = apply_completion(line, ctx, c);
    CHECK_EQ(out, line);
    CHECK_EQ(cur, line.size());
}

TEST("context: cursor at 0 is inactive") {
    auto c = mention_context_at("@abc", 0);
    CHECK(!c.active);
}
TEST("context: deep multi-slash token") {
    auto c = mention_context_at("@src/agent/ag", 13);
    CHECK(c.active);
    CHECK_EQ(c.typed, std::string("src/agent/ag"));
    CHECK_EQ(c.token_begin, std::size_t{1});
    CHECK_EQ(c.token_end, std::size_t{13});
}
TEST("complete: multi-slash directory prefix") {
    test::TempDir d;
    d.write("src/agent/agent.cpp", "x");
    d.write("src/agent/agent.hpp", "x");
    d.write("src/agent/tui.cpp", "x");
    auto v = complete_mention("src/agent/ag", d.path());
    CHECK_EQ(v.size(), std::size_t{2});
    CHECK_EQ(v[0].insert, std::string("src/agent/agent.cpp"));
    CHECK_EQ(v[1].insert, std::string("src/agent/agent.hpp"));
}
TEST("complete: trailing slash lists the subdirectory") {
    test::TempDir d;
    d.write("src/a.txt", "x");
    d.write("src/b.txt", "x");
    auto v = complete_mention("src/", d.path());
    CHECK_EQ(v.size(), std::size_t{2});
    CHECK_EQ(v[0].insert, std::string("src/a.txt"));
    CHECK_EQ(v[1].insert, std::string("src/b.txt"));
}
TEST("complete: max caps the candidate count") {
    test::TempDir d;
    d.write("a.txt", "x");
    d.write("b.txt", "x");
    d.write("c.txt", "x");
    auto v = complete_mention("", d.path(), 2);
    CHECK_EQ(v.size(), std::size_t{2});
}

// --- fuzzy / recursive subsequence fallback ---------------------------------

TEST("complete: subsequence fallback surfaces a file in a subdir") {
    test::TempDir d;
    d.write("src/test.cpp", "x");
    // Root has no entry starting with "test", so the prefix phase is empty
    // and the recursive subsequence fallback must find src/test.cpp.
    auto v = complete_mention("test", d.path());
    CHECK_EQ(v.size(), std::size_t{1});
    CHECK_EQ(v[0].insert, std::string("src/test.cpp"));
    CHECK(!v[0].is_dir);
}

TEST("complete: subsequence matches non-contiguous chars (tst -> test.cpp)") {
    test::TempDir d;
    d.write("src/test.cpp", "x");
    auto v = complete_mention("tst", d.path());
    CHECK_EQ(v.size(), std::size_t{1});
    CHECK_EQ(v[0].insert, std::string("src/test.cpp"));
}

TEST("complete: subsequence is case-insensitive") {
    test::TempDir d;
    d.write("src/TestCase.cpp", "x");
    auto v = complete_mention("test", d.path());
    CHECK_EQ(v.size(), std::size_t{1});
    CHECK_EQ(v[0].insert, std::string("src/TestCase.cpp"));
}

TEST("complete: prefix matches sort before fuzzy hits") {
    test::TempDir d;
    // "testing.txt" is a prefix hit in the root; "src/test.cpp" is a fuzzy hit.
    d.write("testing.txt", "x");
    d.write("src/test.cpp", "x");
    auto v = complete_mention("test", d.path());
    CHECK_EQ(v.size(), std::size_t{2});
    CHECK_EQ(v[0].insert, std::string("testing.txt"));
    CHECK_EQ(v[1].insert, std::string("src/test.cpp"));
}

TEST("complete: fuzzy fallback does not run once a subdir is drilled into") {
    test::TempDir d;
    d.write("src/test.cpp", "x");
    d.write("src/other.cpp", "x");
    // "src/test" points into src/; prefix match there finds test.cpp. The
    // fallback is suppressed (dir_rel non-empty), so other.cpp is NOT added.
    auto v = complete_mention("src/test", d.path());
    CHECK_EQ(v.size(), std::size_t{1});
    CHECK_EQ(v[0].insert, std::string("src/test.cpp"));
}

TEST("complete: fuzzy fallback skips build/cache/VCS directories") {
    test::TempDir d;
    // A file that would match "test" but lives under a pruned build tree.
    d.write("build/test.cpp", "x");
    d.write(".git/test.cpp", "x");
    d.write("src/test.cpp", "x");
    auto v = complete_mention("test", d.path());
    // Only the src/ hit survives; build/ and .git/ are pruned.
    CHECK_EQ(v.size(), std::size_t{1});
    CHECK_EQ(v[0].insert, std::string("src/test.cpp"));
}

TEST("complete: fuzzy fallback is deduped against prefix hits") {
    test::TempDir d;
    // "test.cpp" is both a root prefix hit and a fuzzy hit; must appear once.
    d.write("test.cpp", "x");
    d.write("src/test.cpp", "x");
    auto v = complete_mention("test", d.path());
    std::vector<std::string> inserts;
    for (const auto& c : v) inserts.push_back(c.insert);
    std::ranges::sort(inserts);
    CHECK_EQ(inserts.size(), std::size_t{2});
    CHECK_EQ(inserts[0], std::string("src/test.cpp"));
    CHECK_EQ(inserts[1], std::string("test.cpp"));
}

TEST("complete: empty segment lists root only (no recursive walk)") {
    test::TempDir d;
    d.write("src/test.cpp", "x");
    // Bare "@" with nothing typed: prefix phase lists root entries; the
    // fallback is skipped (seg empty), so src/test.cpp is not surfaced.
    auto v = complete_mention("", d.path());
    CHECK_EQ(v.size(), std::size_t{1});
    CHECK_EQ(v[0].insert, std::string("src/"));
}
