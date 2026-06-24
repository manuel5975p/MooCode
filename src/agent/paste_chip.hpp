#ifndef MOOCODE_PASTE_CHIP_HPP
#define MOOCODE_PASTE_CHIP_HPP

// Pasted-text "chip" helpers for the TUI prompt line. A large bracketed paste is
// collapsed in the input buffer to a short, pure-ASCII placeholder token
// `[Pasted text #N]` (N = a stable per-paste id). The token *is* the paste's
// handle: it shows compactly in the prompt and in the chat log, survives recall
// from the input history, and is expanded back to the full bytes only in the
// text actually sent to the model. Keeping the marker ASCII means a byte offset
// equals a glyph offset, so it composes cleanly with FTXUI Input's byte-indexed
// cursor — and the token behaves as a single character: one keystroke skips or
// deletes the whole thing.
//
// Header-only and dependency-free (STL only) so the byte arithmetic is unit-
// testable without a terminal. Mirrors image_chip.hpp.

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace moocode {

inline constexpr std::string_view kPasteChipPrefix = "[Pasted text #";

// The chip text for paste id `id`. pre: id >= 0. post: `[Pasted text #<id>]`.
inline std::string paste_chip(int id) {
    return std::string(kPasteChipPrefix) + std::to_string(id) + "]";
}

// Parse a `[Pasted text #N]` chip whose opening `[` sits exactly at `start`.
// Returns {index just past the closing `]`, id} when one begins there, else
// nullopt. pre: start <= text.size().
inline std::optional<std::pair<std::size_t, int>>
parse_paste_chip_at(std::string_view text, std::size_t start) {
    if (start > text.size() ||
        text.substr(start, kPasteChipPrefix.size()) != kPasteChipPrefix)
        return std::nullopt;
    std::size_t d = start + kPasteChipPrefix.size();
    std::size_t digits_start = d;
    while (d < text.size() && text[d] >= '0' && text[d] <= '9') ++d;
    if (d == digits_start || d >= text.size() || text[d] != ']')
        return std::nullopt;  // need >=1 digit then a closing bracket
    int id = 0;
    auto [ptr, ec] =
        std::from_chars(text.data() + digits_start, text.data() + d, id);
    if (ec != std::errc{}) return std::nullopt;
    return std::make_pair(d + 1, id);  // d+1 = index just past ']'
}

// Ids of every well-formed `[Pasted text #N]` chip in `text`, in first-
// appearance order, deduplicated. Malformed or partially-edited markers are
// ignored (so a half-deleted chip stops resolving).
inline std::vector<int> extract_paste_ids(std::string_view text) {
    std::vector<int> ids;
    std::size_t pos = 0;
    while (pos < text.size()) {
        if (auto p = parse_paste_chip_at(text, pos)) {
            const auto [end, id] = *p;
            if (std::find(ids.begin(), ids.end(), id) == ids.end())
                ids.push_back(id);
            pos = end;
        } else {
            ++pos;
        }
    }
    return ids;
}

// Byte range [start, cursor) of a complete chip whose closing `]` sits exactly
// at `cursor` (i.e. the cursor is immediately to its right), or nullopt. Drives
// atomic single-keystroke chip deletion (Backspace) and atomic left-skip
// (ArrowLeft). pre: cursor <= text.size().
inline std::optional<std::pair<std::size_t, std::size_t>>
paste_chip_ending_at(std::string_view text, std::size_t cursor) {
    if (cursor == 0 || cursor > text.size()) return std::nullopt;
    if (text[cursor - 1] != ']') return std::nullopt;
    std::size_t digits_end = cursor - 1;  // index of ']'
    std::size_t j = digits_end;
    while (j > 0 && text[j - 1] >= '0' && text[j - 1] <= '9') --j;
    if (j == digits_end) return std::nullopt;  // need at least one digit
    if (j < kPasteChipPrefix.size()) return std::nullopt;
    std::size_t prefix_start = j - kPasteChipPrefix.size();
    if (text.substr(prefix_start, kPasteChipPrefix.size()) != kPasteChipPrefix)
        return std::nullopt;
    return std::make_pair(prefix_start, cursor);
}

// Byte range [cursor, end) of a complete chip whose opening `[` sits exactly at
// `cursor` (i.e. the cursor is immediately to its left), or nullopt. Drives
// atomic forward deletion (Delete) and atomic right-skip (ArrowRight).
// pre: cursor <= text.size().
inline std::optional<std::pair<std::size_t, std::size_t>>
paste_chip_starting_at(std::string_view text, std::size_t cursor) {
    auto p = parse_paste_chip_at(text, cursor);
    if (!p) return std::nullopt;
    return std::make_pair(cursor, p->first);
}

// `text` with every well-formed `[Pasted text #N]` chip replaced by the full
// bytes `lookup(N)` returns, leaving every other byte — and any chip `lookup`
// can't resolve — untouched. Used to rebuild the prose actually sent to the
// model. `lookup` is `int -> std::optional<std::string_view>`.
template <class Lookup>
std::string expand_paste_chips(std::string_view text, Lookup&& lookup) {
    std::string out;
    out.reserve(text.size());
    std::size_t pos = 0;
    while (pos < text.size()) {
        if (auto p = parse_paste_chip_at(text, pos)) {
            const auto [end, id] = *p;
            if (auto content = lookup(id)) {
                out.append(*content);
                pos = end;
                continue;
            }
        }
        out += text[pos];
        ++pos;
    }
    return out;
}

}  // namespace moocode

#endif  // MOOCODE_PASTE_CHIP_HPP
