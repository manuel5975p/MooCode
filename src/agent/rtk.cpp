#include "agent/rtk.hpp"

#include <array>
#include <string>

namespace moocode {

namespace {

// Programs rtk proxies losslessly. git intentionally absent (git tools own it);
// no summarizers (lossy); no cat/read (read_file owns file reads).
constexpr std::array<std::string_view, 8> kAllowlist{
    "ls", "tree", "grep", "find", "cargo", "wc", "env", "diff"};

// Any of these means we cannot safely prepend `rtk` to a single subcommand.
bool has_shell_metachar(std::string_view s) {
    for (char c : s) {
        switch (c) {
            case '|': case '&': case ';': case '<': case '>':
            case '$': case '`': case '(': case ')': case '{': case '}':
            case '\n': case '\\':
                return true;
            default: break;
        }
    }
    return false;
}

std::string_view first_word(std::string_view s) {
    std::size_t b = s.find_first_not_of(" \t");
    if (b == std::string_view::npos) return {};
    std::size_t e = s.find_first_of(" \t", b);
    return s.substr(b, e == std::string_view::npos ? e : e - b);
}

std::string_view trim(std::string_view s) {
    std::size_t b = s.find_first_not_of(" \t");
    if (b == std::string_view::npos) return {};
    std::size_t e = s.find_last_not_of(" \t");
    return s.substr(b, e - b + 1);
}

}  // namespace

std::string rtk_strip_escape(std::string_view cmd) {
    if (cmd.size() >= 2 && cmd[0] == '\\' && cmd[1] == ' ')
        return std::string(cmd.substr(2));
    return std::string(cmd);
}

std::optional<std::string> rtk_rewrite(std::string_view cmd) {
    std::string_view t = trim(cmd);
    if (t.empty()) return std::nullopt;
    if (has_shell_metachar(t)) return std::nullopt;  // also catches the "\ " escape
    std::string_view w = first_word(t);
    for (std::string_view a : kAllowlist)
        if (w == a) return "rtk " + std::string(t);
    return std::nullopt;
}

}  // namespace moocode
