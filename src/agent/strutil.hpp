#ifndef MOOCODE_STRUTIL_HPP
#define MOOCODE_STRUTIL_HPP

// Small, dependency-free string helpers shared across layers. Header-only: no
// I/O, no project deps beyond the STL, so any target may include it freely.

#include <cctype>
#include <cstddef>
#include <cstdlib>
#include <string>
#include <string_view>
#include <utility>

namespace moocode {

// Lowercase copy, ASCII-locale, safe for any char value. Total.
inline std::string to_lower(std::string_view s) {
    std::string out(s);
    for (char& c : out)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

// $name when set AND non-empty, else `def`. An empty env var counts as unset.
inline std::string env_or(const char* name, std::string def) {
    const char* v = std::getenv(name);
    return v && *v ? std::string(v) : std::move(def);
}

// Cap `s` to `max` bytes, appending `marker(shown, full)` when truncation
// occurs. The marker is a parameter so each call site keeps its exact wording.
// pre: max may be 0. post: result.size() may exceed `max` (marker is appended).
template <class Marker>
std::string truncate(std::string s, std::size_t max, Marker marker) {
    if (s.size() <= max) return s;
    const std::size_t full = s.size();
    s.resize(max);
    s += marker(max, full);
    return s;
}

// Middle-elide `s`: when it exceeds `max` bytes, keep the first `keep` and the
// last `keep` bytes joined by "...", yielding at most `2*keep + 3` bytes. The
// content (including any newlines) is otherwise preserved verbatim, so the
// result may be multi-line. pre: 2*keep <= max. post: short inputs pass through.
inline std::string elide_middle(std::string_view s, std::size_t max,
                                std::size_t keep) {
    if (s.size() <= max) return std::string(s);
    std::string out;
    out.reserve(2 * keep + 3);
    out.append(s.substr(0, keep));
    out += "...";
    out.append(s.substr(s.size() - keep));
    return out;
}

// First line of `s`, trimmed to `max` bytes, with an ellipsis when shortened.
inline std::string one_line(std::string_view s, std::size_t max) {
    std::string out;
    bool clipped = false;
    for (char c : s) {
        if (c == '\n' || c == '\r') {
            clipped = true;  // there is more beyond the first line
            break;
        }
        out += c;
    }
    if (out.size() > max) {
        out.resize(max);
        clipped = true;
    }
    if (clipped) out += "…";
    return out;
}

// Trim leading/trailing space, tab, CR (not LF). Total; returns a view into `s`.
inline std::string_view trim_sv(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.remove_suffix(1);
    return s;
}

// Hex digit value, or -1 if not [0-9A-Fa-f]. Total.
constexpr int hex_val(unsigned char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// value (0..15) -> uppercase hex digit. pre: 0<=v<16.
constexpr char hex_digit(int v) { return static_cast<char>(v < 10 ? '0' + v : 'A' + (v - 10)); }

// RFC 4648 base64 with '=' padding, no line breaks, standard alphabet
// (A-Za-z0-9+/). pre: data may be empty. post: size == ceil(data.size()/3)*4.
inline std::string base64_encode(std::string_view data) {
    static constexpr char kAlphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((data.size() + 2) / 3) * 4);
    for (std::size_t i = 0; i < data.size(); i += 3) {
        unsigned a = static_cast<unsigned char>(data[i]);
        unsigned b = (i + 1 < data.size()) ? static_cast<unsigned char>(data[i + 1]) : 0;
        unsigned c = (i + 2 < data.size()) ? static_cast<unsigned char>(data[i + 2]) : 0;
        unsigned triple = (a << 16) | (b << 8) | c;
        out += kAlphabet[(triple >> 18) & 0x3F];
        out += kAlphabet[(triple >> 12) & 0x3F];
        out += (i + 1 < data.size()) ? kAlphabet[(triple >> 6) & 0x3F] : '=';
        out += (i + 2 < data.size()) ? kAlphabet[triple & 0x3F] : '=';
    }
    return out;
}

// The standard "\n[truncated: showing M of F bytes]" marker. Total.
inline std::string default_trunc_marker(std::size_t shown, std::size_t full) {
    return "\n[truncated: showing " + std::to_string(shown) + " of " +
           std::to_string(full) + " bytes]";
}

// Replace every occurrence of `key` in `s` with `val`. post: no `key` remains
// unless it reappears inside a substituted `val`.
inline void substitute(std::string& s, std::string_view key, std::string_view val) {
    for (std::size_t pos = 0; (pos = s.find(key, pos)) != std::string::npos;) {
        s.replace(pos, key.size(), val);
        pos += val.size();
    }
}

}  // namespace moocode

#endif  // MOOCODE_STRUTIL_HPP
