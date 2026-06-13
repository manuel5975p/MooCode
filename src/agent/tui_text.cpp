#include "agent/tui_text.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace flagent {

namespace {

struct Interval {
    std::uint32_t first;
    std::uint32_t last;
};

// Codepoints dropped wholesale: Unicode 16 Default_Ignorable_Code_Point
// (terminals render them zero-width, FTXUI gives the non-combining ones a
// full cell) plus the emoji modifiers whose effect on the base glyph's width
// varies by terminal (keycap, skin tones; VS16 is inside FE00-FE0F). Sorted.
constexpr std::array<Interval, 19> kDropRanges = {{
    {0x00AD, 0x00AD},    // soft hyphen
    {0x034F, 0x034F},    // combining grapheme joiner
    {0x061C, 0x061C},    // arabic letter mark
    {0x115F, 0x1160},    // hangul jamo fillers
    {0x17B4, 0x17B5},    // khmer inherent vowels
    {0x180B, 0x180F},    // mongolian variation selectors
    {0x200B, 0x200F},    // ZWSP, ZWNJ, ZWJ, LRM, RLM
    {0x202A, 0x202E},    // bidi embeddings/overrides
    {0x2060, 0x206F},    // word joiner, invisibles, bidi isolates
    {0x20E3, 0x20E3},    // combining enclosing keycap (emoji-flips its base)
    {0x3164, 0x3164},    // hangul filler
    {0xFE00, 0xFE0F},    // variation selectors (VS16 emoji-widens its base)
    {0xFEFF, 0xFEFF},    // zero-width no-break space / BOM
    {0xFFA0, 0xFFA0},    // halfwidth hangul filler
    {0xFFF0, 0xFFF8},    // specials block reserved range
    {0x1BCA0, 0x1BCA3},  // shorthand format controls
    {0x1D173, 0x1D17A},  // musical format controls
    {0x1F3FB, 0x1F3FF},  // emoji skin-tone modifiers (join only with shaping)
    {0xE0000, 0xE0FFF},  // tags + variation selectors supplement
}};

bool is_dropped(std::uint32_t cp) {
    if (cp < kDropRanges.front().first || cp > kDropRanges.back().last)
        return false;
    std::size_t lo = 0;
    std::size_t hi = kDropRanges.size();
    while (lo < hi) {
        const std::size_t mid = (lo + hi) / 2;
        if (cp > kDropRanges[mid].last)
            lo = mid + 1;
        else if (cp < kDropRanges[mid].first)
            hi = mid;
        else
            return true;
    }
    return false;
}

// Decode one strict UTF-8 codepoint at s[i], advancing i past it. On
// malformed input (bad lead, truncated/overlong sequence, surrogate,
// > U+10FFFF) advances one byte and returns nullopt so the caller drops it.
std::optional<std::uint32_t> eat_codepoint(std::string_view s, std::size_t& i) {
    const auto b = [&](std::size_t k) {
        return static_cast<std::uint32_t>(static_cast<unsigned char>(s[k]));
    };
    const std::uint32_t lead = b(i);
    std::size_t len = 0;
    std::uint32_t cp = 0;
    if (lead < 0x80) {
        ++i;
        return lead;
    }
    if (lead >= 0xC2 && lead <= 0xDF) {
        len = 2;
        cp = lead & 0x1F;
    } else if (lead >= 0xE0 && lead <= 0xEF) {
        len = 3;
        cp = lead & 0x0F;
    } else if (lead >= 0xF0 && lead <= 0xF4) {
        len = 4;
        cp = lead & 0x07;
    } else {
        ++i;  // stray continuation byte or invalid lead (0xC0/0xC1/0xF5+)
        return std::nullopt;
    }
    if (i + len > s.size()) {
        ++i;
        return std::nullopt;
    }
    for (std::size_t k = 1; k < len; ++k) {
        if ((b(i + k) & 0xC0) != 0x80) {
            ++i;
            return std::nullopt;
        }
        cp = (cp << 6) | (b(i + k) & 0x3F);
    }
    const bool overlong = (len == 2 && cp < 0x80) ||
                          (len == 3 && cp < 0x800) ||
                          (len == 4 && cp < 0x10000);
    if (overlong || (cp >= 0xD800 && cp <= 0xDFFF) || cp > 0x10FFFF) {
        ++i;
        return std::nullopt;
    }
    i += len;
    return cp;
}

// Skip one ANSI escape sequence; `i` points just past the ESC byte. Returns
// the index of the first byte after the sequence (clamped to s.size(); a
// truncated sequence is swallowed). A nested ESC ends a string sequence so
// the outer loop re-parses it.
std::size_t skip_escape(std::string_view s, std::size_t i) {
    if (i >= s.size()) return i;
    const auto uc = [&](std::size_t k) {
        return static_cast<unsigned char>(s[k]);
    };
    const char kind = s[i];
    if (kind == '[') {  // CSI: parameters/intermediates 0x20-0x3F, final 0x40-0x7E
        ++i;
        while (i < s.size() && uc(i) >= 0x20 && uc(i) <= 0x3F) ++i;
        if (i < s.size() && uc(i) >= 0x40 && uc(i) <= 0x7E) ++i;
        return i;
    }
    if (kind == ']' || kind == 'P' || kind == 'X' || kind == '^' ||
        kind == '_') {  // OSC/DCS/SOS/PM/APC: until BEL or ST (ESC \)
        ++i;
        while (i < s.size()) {
            if (s[i] == '\x07') return i + 1;
            if (s[i] == '\x1b')
                return (i + 1 < s.size() && s[i + 1] == '\\') ? i + 2 : i;
            ++i;
        }
        return i;
    }
    // Two-byte and charset sequences: intermediates then one final byte.
    while (i < s.size() && uc(i) >= 0x20 && uc(i) <= 0x2F) ++i;
    if (i < s.size()) ++i;
    return i;
}

}  // namespace

std::string sanitize_tui_text(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    std::size_t i = 0;
    while (i < s.size()) {
        const unsigned char byte = static_cast<unsigned char>(s[i]);
        if (byte == '\n') {
            out += '\n';
            ++i;
            continue;
        }
        if (byte == '\t') {  // FTXUI drops tabs outright; expand instead
            out += "    ";
            ++i;
            continue;
        }
        if (byte == 0x1B) {
            i = skip_escape(s, i + 1);
            continue;
        }
        if (byte < 0x20 || byte == 0x7F) {  // remaining C0 controls + DEL
            ++i;
            continue;
        }
        if (byte < 0x80) {
            out += static_cast<char>(byte);
            ++i;
            continue;
        }
        const std::size_t start = i;
        const auto cp = eat_codepoint(s, i);
        if (!cp) continue;                    // malformed byte dropped
        if (*cp >= 0x80 && *cp <= 0x9F) continue;  // C1 controls
        if (*cp == 0x2028 || *cp == 0x2029) {  // line/paragraph separator:
            out += ' ';                        // width differs per terminal
            continue;
        }
        if (is_dropped(*cp)) continue;
        out.append(s, start, i - start);
    }
    return out;
}

}  // namespace flagent
