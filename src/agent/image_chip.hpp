#ifndef FLAGENT_IMAGE_CHIP_HPP
#define FLAGENT_IMAGE_CHIP_HPP

// Pasted-image "chip" helpers for the TUI prompt line. A pasted clipboard image
// is represented in the input buffer by a short, pure-ASCII placeholder token
// `[img#N]` (N = a stable per-paste id). The token *is* the image's handle:
// while it is present in the text the image is attached; once it is erased the
// image is detached. Keeping the marker ASCII means a byte offset equals a glyph
// offset, so it composes cleanly with FTXUI Input's byte-indexed cursor.
//
// Header-only and dependency-free (STL only) so the byte arithmetic is unit-
// testable without a terminal.

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace flagent {

// The chip text for image id `id`. pre: id >= 0. post: pure ASCII `[img#<id>]`.
inline std::string image_chip(int id) {
    return "[img#" + std::to_string(id) + "]";
}

// Ids of every well-formed `[img#N]` chip in `text`, in first-appearance order,
// deduplicated. A chip needs `[img#`, one or more digits, then `]`; malformed or
// partially-edited markers are ignored (so a half-deleted chip detaches).
inline std::vector<int> extract_chip_ids(std::string_view text) {
    static constexpr std::string_view kPrefix = "[img#";
    std::vector<int> ids;
    std::size_t pos = 0;
    while (true) {
        std::size_t at = text.find(kPrefix, pos);
        if (at == std::string_view::npos) break;
        std::size_t d = at + kPrefix.size();
        std::size_t digits_start = d;
        while (d < text.size() && text[d] >= '0' && text[d] <= '9') ++d;
        if (d > digits_start && d < text.size() && text[d] == ']') {
            int id = 0;
            auto [ptr, ec] =
                std::from_chars(text.data() + digits_start, text.data() + d, id);
            if (ec == std::errc{} &&
                std::find(ids.begin(), ids.end(), id) == ids.end())
                ids.push_back(id);
            pos = d + 1;  // resume past the closing bracket
        } else {
            pos = at + 1;  // not a chip; advance one byte to keep scanning
        }
    }
    return ids;
}

// Byte range [start, cursor) of a complete chip whose closing `]` sits exactly
// at `cursor` (i.e. the cursor is immediately to its right), or nullopt. Drives
// atomic single-keystroke chip deletion. pre: cursor <= text.size().
inline std::optional<std::pair<std::size_t, std::size_t>>
chip_ending_at(std::string_view text, std::size_t cursor) {
    static constexpr std::string_view kPrefix = "[img#";
    if (cursor == 0 || cursor > text.size()) return std::nullopt;
    if (text[cursor - 1] != ']') return std::nullopt;
    std::size_t digits_end = cursor - 1;  // index of ']'
    std::size_t j = digits_end;
    while (j > 0 && text[j - 1] >= '0' && text[j - 1] <= '9') --j;
    if (j == digits_end) return std::nullopt;  // need at least one digit
    if (j < kPrefix.size()) return std::nullopt;
    std::size_t prefix_start = j - kPrefix.size();
    if (text.substr(prefix_start, kPrefix.size()) != kPrefix)
        return std::nullopt;
    return std::make_pair(prefix_start, cursor);
}

// `text` with every well-formed `[img#N]` chip rewritten to the human-readable
// `[image #N]`, leaving all other bytes untouched. Used for the prose actually
// sent to the model, so the positional reference survives without the raw UI
// marker.
inline std::string strip_chips(std::string_view text) {
    static constexpr std::string_view kPrefix = "[img#";
    std::string out;
    out.reserve(text.size());
    std::size_t pos = 0;
    while (pos < text.size()) {
        if (text.compare(pos, kPrefix.size(), kPrefix) == 0) {
            std::size_t d = pos + kPrefix.size();
            std::size_t digits_start = d;
            while (d < text.size() && text[d] >= '0' && text[d] <= '9') ++d;
            if (d > digits_start && d < text.size() && text[d] == ']') {
                out += "[image #";
                out.append(text.substr(digits_start, d - digits_start));
                out += ']';
                pos = d + 1;
                continue;
            }
        }
        out += text[pos];
        ++pos;
    }
    return out;
}

}  // namespace flagent

#endif  // FLAGENT_IMAGE_CHIP_HPP
