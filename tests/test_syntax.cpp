// Tests for the pure syntax highlighter: language-tag mapping plus per-line
// tokenisation for bash, python and cpp, including multi-line lexer state.

#include "agent/syntax_highlight.hpp"

#include <string>

#include "test_harness.hpp"

using namespace flagent;

namespace {

// Concatenate every span's text on a line — must reproduce the input exactly so
// the highlighter never drops or duplicates characters.
std::string join(const std::vector<HlSpan>& line) {
    std::string s;
    for (const auto& sp : line) s += sp.text;
    return s;
}

// True if some span exactly equal to `text` carries category `cat`.
bool has(const std::vector<HlSpan>& line, const std::string& text,
         TokenCategory cat) {
    for (const auto& sp : line)
        if (sp.text == text && sp.category == cat) return true;
    return false;
}

}  // namespace

TEST("language_from_tag maps aliases case-insensitively") {
    CHECK(language_from_tag("bash") == Language::Bash);
    CHECK(language_from_tag("SH") == Language::Bash);
    CHECK(language_from_tag("Python") == Language::Python);
    CHECK(language_from_tag("py") == Language::Python);
    CHECK(language_from_tag("cpp") == Language::Cpp);
    CHECK(language_from_tag("C++") == Language::Cpp);
    CHECK(language_from_tag("rust") == Language::None);
    CHECK(language_from_tag("") == Language::None);
}

TEST("None language yields one plain span per line, content preserved") {
    auto out = highlight_block("a = 1\nb = 2", Language::None);
    CHECK_EQ(out.size(), std::size_t{2});
    CHECK_EQ(join(out[0]), std::string("a = 1"));
    CHECK_EQ(join(out[1]), std::string("b = 2"));
    CHECK(out[0][0].category == TokenCategory::Plain);
}

TEST("highlight preserves every character on each line") {
    const std::string src = "int main() { return 0; } // ok\n#include <x>";
    auto out = highlight_block(src, Language::Cpp);
    CHECK_EQ(out.size(), std::size_t{2});
    CHECK_EQ(join(out[0]), std::string("int main() { return 0; } // ok"));
    CHECK_EQ(join(out[1]), std::string("#include <x>"));
}

TEST("cpp keywords, types, numbers, comments and preproc classified") {
    auto out = highlight_block("#include <vector>\nint x = 42; // c", Language::Cpp);
    CHECK(has(out[0], "#include", TokenCategory::Preproc));
    CHECK(has(out[1], "int", TokenCategory::Type));
    CHECK(has(out[1], "return", TokenCategory::Keyword) == false);  // not present
    CHECK(has(out[1], "42", TokenCategory::Number));
    CHECK(has(out[1], "// c", TokenCategory::Comment));
}

TEST("cpp block comment carries across lines") {
    auto out = highlight_block("a /* start\nstill comment\nend */ b", Language::Cpp);
    CHECK_EQ(out.size(), std::size_t{3});
    CHECK(has(out[1], "still comment", TokenCategory::Comment));
    CHECK(has(out[2], "/* never", TokenCategory::Comment) == false);
    // After the close, the trailing identifier is plain again.
    CHECK(join(out[2]) == std::string("end */ b"));
}

TEST("python keywords, builtins, strings and hash comment") {
    auto out = highlight_block("def f():\n    print(\"hi\")  # note", Language::Python);
    CHECK(has(out[0], "def", TokenCategory::Keyword));
    CHECK(has(out[1], "print", TokenCategory::Builtin));
    CHECK(has(out[1], "\"hi\"", TokenCategory::String));
    CHECK(has(out[1], "# note", TokenCategory::Comment));
}

TEST("python triple-quoted string spans lines") {
    auto out = highlight_block("x = '''line one\nline two'''\ny = 1", Language::Python);
    CHECK_EQ(out.size(), std::size_t{3});
    CHECK(has(out[1], "line two'''", TokenCategory::String));
    CHECK(has(out[2], "1", TokenCategory::Number));
}

TEST("bash keywords, variables and comments") {
    auto out = highlight_block("if true; then\n  echo $HOME # hi\nfi", Language::Bash);
    CHECK(has(out[0], "if", TokenCategory::Keyword));
    CHECK(has(out[0], "true", TokenCategory::Builtin));
    CHECK(has(out[1], "echo", TokenCategory::Builtin));
    CHECK(has(out[1], "$HOME", TokenCategory::Variable));
    CHECK(has(out[1], "# hi", TokenCategory::Comment));
    CHECK(has(out[2], "fi", TokenCategory::Keyword));
}

TEST("bash brace variable form") {
    auto out = highlight_block("echo ${PATH}", Language::Bash);
    CHECK(has(out[0], "${PATH}", TokenCategory::Variable));
}

TEST("single trailing newline does not add an empty line") {
    auto out = highlight_block("a\n", Language::None);
    CHECK_EQ(out.size(), std::size_t{1});
    CHECK_EQ(join(out[0]), std::string("a"));
}

TEST("empty input yields a single empty line") {
    auto out = highlight_block("", Language::Cpp);
    CHECK_EQ(out.size(), std::size_t{1});
    CHECK(out[0].empty());
}
