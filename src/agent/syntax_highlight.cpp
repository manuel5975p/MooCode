#include "agent/syntax_highlight.hpp"

#include <cctype>
#include <unordered_set>

namespace flagent {
namespace {

// Lower-case a single byte without locale/sign surprises.
char lower(char c) {
    return static_cast<char>(
        std::tolower(static_cast<unsigned char>(c)));
}

bool is_ident_start(char c) {
    unsigned char u = static_cast<unsigned char>(c);
    return std::isalpha(u) || c == '_';
}
bool is_ident(char c) {
    unsigned char u = static_cast<unsigned char>(c);
    return std::isalnum(u) || c == '_';
}
bool is_digit(char c) {
    return std::isdigit(static_cast<unsigned char>(c)) != 0;
}

using WordSet = std::unordered_set<std::string_view>;

// Per-language lexing rules. Word sets reference static string literals, so the
// string_view keys are safe to keep for the program's lifetime.
struct LangSpec {
    const WordSet* keywords = nullptr;
    const WordSet* types = nullptr;
    const WordSet* builtins = nullptr;
    bool hash_comment = false;   // '#' to end of line (bash, python)
    bool slash_comment = false;  // '//' to end of line (cpp)
    bool block_comment = false;  // '/* ... */' (cpp)
    bool triple_string = false;  // '''...''' / """...""" (python)
    bool dollar_var = false;     // $name / ${...} (bash)
    bool preproc = false;        // '#directive' at line start (cpp)
};

const WordSet& bash_keywords() {
    static const WordSet s = {
        "if", "then", "else", "elif", "fi", "for", "while", "until", "do",
        "done", "case", "esac", "in", "function", "select", "time", "return",
        "break", "continue", "local", "export", "readonly", "declare", "set",
        "unset", "shift", "exit", "trap", "source"};
    return s;
}
const WordSet& bash_builtins() {
    static const WordSet s = {
        "echo", "printf", "cd", "pwd", "ls", "cat", "grep", "sed", "awk",
        "read", "test", "eval", "exec", "true", "false", "mkdir", "rm", "cp",
        "mv", "touch", "chmod", "kill", "wait", "sleep"};
    return s;
}

const WordSet& py_keywords() {
    static const WordSet s = {
        "def",    "class",  "if",     "elif",   "else",   "for",   "while",
        "return", "import", "from",   "as",     "with",   "try",   "except",
        "finally","raise",  "lambda", "yield",  "global", "nonlocal", "pass",
        "break",  "continue","and",   "or",     "not",    "in",    "is",
        "async",  "await",  "del",    "assert", "with",   "True",  "False",
        "None"};
    return s;
}
const WordSet& py_builtins() {
    static const WordSet s = {
        "print", "len",   "range", "int",   "str",   "float", "list",
        "dict",  "set",   "tuple", "bool",  "open",  "type",  "isinstance",
        "enumerate", "zip", "map", "filter", "sorted", "sum", "min", "max",
        "abs",   "input", "super", "object", "self"};
    return s;
}

const WordSet& cpp_keywords() {
    static const WordSet s = {
        "alignas","alignof","and","asm","auto","break","case","catch","class",
        "concept","const","consteval","constexpr","constinit","const_cast",
        "continue","co_await","co_return","co_yield","decltype","default",
        "delete","do","dynamic_cast","else","enum","explicit","export","extern",
        "for","friend","goto","if","inline","mutable","namespace","new",
        "noexcept","operator","or","private","protected","public","register",
        "reinterpret_cast","requires","return","sizeof","static","static_assert",
        "static_cast","struct","switch","template","this","thread_local","throw",
        "try","typedef","typeid","typename","union","using","virtual","volatile",
        "while","nullptr","true","false","not","xor"};
    return s;
}
const WordSet& cpp_types() {
    static const WordSet s = {
        "int","char","bool","float","double","void","short","long","signed",
        "unsigned","wchar_t","char8_t","char16_t","char32_t","size_t",
        "int8_t","int16_t","int32_t","int64_t","uint8_t","uint16_t","uint32_t",
        "uint64_t","string","string_view","vector","map","set","array","pair",
        "optional","expected","unique_ptr","shared_ptr"};
    return s;
}

const LangSpec& spec_for(Language lang) {
    static const LangSpec none{};
    static const LangSpec bash{&bash_keywords(), nullptr, &bash_builtins(),
                               /*hash*/ true, false, false, false,
                               /*dollar*/ true, false};
    static const LangSpec python{&py_keywords(), nullptr, &py_builtins(),
                                 /*hash*/ true, false, false,
                                 /*triple*/ true, false, false};
    static const LangSpec cpp{&cpp_keywords(), &cpp_types(), nullptr,
                              false, /*slash*/ true, /*block*/ true, false,
                              false, /*preproc*/ true};
    switch (lang) {
        case Language::Bash: return bash;
        case Language::Python: return python;
        case Language::Cpp: return cpp;
        case Language::None: break;
    }
    return none;
}

TokenCategory classify_word(std::string_view w, const LangSpec& spec) {
    if (spec.types && spec.types->contains(w)) return TokenCategory::Type;
    if (spec.keywords && spec.keywords->contains(w)) return TokenCategory::Keyword;
    if (spec.builtins && spec.builtins->contains(w)) return TokenCategory::Builtin;
    return TokenCategory::Plain;
}

// Lexer state that can straddle a line boundary.
enum class Carry { None, BlockComment, TripleSingle, TripleDouble };

// Accumulates spans for the block, flushing a line whenever it sees '\n'.
struct Emitter {
    std::vector<std::vector<HlSpan>> lines;
    std::vector<HlSpan> cur;

    void push(std::string_view t, TokenCategory cat) {
        if (t.empty()) return;
        if (!cur.empty() && cur.back().category == cat)
            cur.back().text.append(t);
        else
            cur.push_back({std::string(t), cat});
    }
    void newline() {
        lines.push_back(std::move(cur));
        cur.clear();
    }
};

}  // namespace

Language language_from_tag(std::string_view tag) {
    std::string t;
    t.reserve(tag.size());
    for (char c : tag) t.push_back(lower(c));
    if (t == "bash" || t == "sh" || t == "shell" || t == "zsh") return Language::Bash;
    if (t == "python" || t == "py") return Language::Python;
    if (t == "cpp" || t == "c++" || t == "cc" || t == "cxx" || t == "hpp" ||
        t == "c" || t == "h")
        return Language::Cpp;
    return Language::None;
}

std::vector<std::vector<HlSpan>> highlight_block(std::string_view code,
                                                 Language lang) {
    const LangSpec& spec = spec_for(lang);
    Emitter em;
    Carry carry = Carry::None;
    const std::size_t n = code.size();
    std::size_t i = 0;
    bool at_line_start = true;  // only whitespace seen so far on this line

    auto eat_string = [&](char quote) {
        // Single-line quoted string with backslash escapes; stops at the close
        // quote or end of line (whichever comes first).
        std::size_t start = i;
        ++i;  // opening quote
        while (i < n && code[i] != '\n') {
            if (code[i] == '\\' && i + 1 < n && code[i + 1] != '\n') {
                i += 2;
                continue;
            }
            if (code[i] == quote) {
                ++i;
                break;
            }
            ++i;
        }
        em.push(code.substr(start, i - start), TokenCategory::String);
    };

    while (i < n) {
        char c = code[i];

        // --- carried multi-line state -------------------------------------
        // Each branch consumes the rest of the current line; if it stops at a
        // newline still inside the construct it must emit that line and advance
        // past the '\n' itself (the normal newline branch below only runs when
        // carry == None), otherwise the loop would spin on the same '\n'.
        if (carry == Carry::BlockComment) {
            std::size_t start = i;
            while (i < n && code[i] != '\n') {
                if (code[i] == '*' && i + 1 < n && code[i + 1] == '/') {
                    i += 2;
                    carry = Carry::None;
                    break;
                }
                ++i;
            }
            em.push(code.substr(start, i - start), TokenCategory::Comment);
            if (carry != Carry::None && i < n && code[i] == '\n') {
                em.newline();
                ++i;
                at_line_start = true;
            }
            continue;
        }
        if (carry == Carry::TripleSingle || carry == Carry::TripleDouble) {
            const std::string_view close =
                carry == Carry::TripleSingle ? "'''" : "\"\"\"";
            std::size_t start = i;
            while (i < n && code[i] != '\n') {
                if (code.substr(i, 3) == close) {
                    i += 3;
                    carry = Carry::None;
                    break;
                }
                ++i;
            }
            em.push(code.substr(start, i - start), TokenCategory::String);
            if (carry != Carry::None && i < n && code[i] == '\n') {
                em.newline();
                ++i;
                at_line_start = true;
            }
            continue;
        }

        // --- newline -------------------------------------------------------
        if (c == '\n') {
            em.newline();
            ++i;
            at_line_start = true;
            continue;
        }

        // --- whitespace ----------------------------------------------------
        if (c == ' ' || c == '\t' || c == '\r') {
            em.push(code.substr(i, 1), TokenCategory::Plain);
            ++i;
            continue;
        }

        // --- C preprocessor directive (line-start '#') ---------------------
        if (spec.preproc && at_line_start && c == '#') {
            std::size_t start = i;
            ++i;
            while (i < n && (code[i] == ' ' || code[i] == '\t')) ++i;
            while (i < n && is_ident(code[i])) ++i;
            em.push(code.substr(start, i - start), TokenCategory::Preproc);
            at_line_start = false;
            continue;
        }

        at_line_start = false;

        // --- line comments -------------------------------------------------
        if (spec.hash_comment && c == '#') {
            std::size_t start = i;
            while (i < n && code[i] != '\n') ++i;
            em.push(code.substr(start, i - start), TokenCategory::Comment);
            continue;
        }
        if (spec.slash_comment && c == '/' && i + 1 < n && code[i + 1] == '/') {
            std::size_t start = i;
            while (i < n && code[i] != '\n') ++i;
            em.push(code.substr(start, i - start), TokenCategory::Comment);
            continue;
        }

        // --- block comment open --------------------------------------------
        // Emit "/*" and advance past it so the carried branch does not mistake
        // the opener for a closer; the remainder is handled there.
        if (spec.block_comment && c == '/' && i + 1 < n && code[i + 1] == '*') {
            carry = Carry::BlockComment;
            em.push(code.substr(i, 2), TokenCategory::Comment);
            i += 2;
            continue;
        }

        // --- triple-quoted string open (python) ----------------------------
        // Likewise step past the opening triple quote so it is not re-read as
        // the closing one.
        if (spec.triple_string && (c == '\'' || c == '"') && i + 2 < n &&
            code[i + 1] == c && code[i + 2] == c) {
            carry = c == '\'' ? Carry::TripleSingle : Carry::TripleDouble;
            em.push(code.substr(i, 3), TokenCategory::String);
            i += 3;
            continue;
        }

        // --- single-line string --------------------------------------------
        if (c == '"' || c == '\'') {
            eat_string(c);
            continue;
        }

        // --- shell variable ------------------------------------------------
        if (spec.dollar_var && c == '$') {
            std::size_t start = i;
            ++i;
            if (i < n && code[i] == '{') {
                while (i < n && code[i] != '}' && code[i] != '\n') ++i;
                if (i < n && code[i] == '}') ++i;
            } else {
                while (i < n && is_ident(code[i])) ++i;
            }
            em.push(code.substr(start, i - start), TokenCategory::Variable);
            continue;
        }

        // --- number --------------------------------------------------------
        if (is_digit(c)) {
            std::size_t start = i;
            while (i < n && (is_ident(code[i]) || code[i] == '.')) ++i;
            em.push(code.substr(start, i - start), TokenCategory::Number);
            continue;
        }

        // --- identifier / keyword ------------------------------------------
        if (is_ident_start(c)) {
            std::size_t start = i;
            while (i < n && is_ident(code[i])) ++i;
            std::string_view w = code.substr(start, i - start);
            em.push(w, classify_word(w, spec));
            continue;
        }

        // --- anything else (punctuation/operators) -------------------------
        em.push(code.substr(i, 1), TokenCategory::Plain);
        ++i;
    }

    // Flush the final line unless the block ended exactly on a newline (which
    // already pushed and reset cur to empty).
    if (!em.cur.empty() || em.lines.empty()) em.newline();
    return std::move(em.lines);
}

}  // namespace flagent
