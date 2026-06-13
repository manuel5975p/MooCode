#ifndef FLAGENT_DIFF_HPP
#define FLAGENT_DIFF_HPP

// Pure line-level diff. No I/O, no dependencies beyond the STL: split both texts
// on '\n', run a classic LCS, and emit the full diff (every context line kept;
// callers elide as they like). Deterministic — given the same inputs it always
// yields the same sequence. Used by both the TUI (colored block) and the
// non-interactive CLI (ANSI to stderr) to show what write_file/edit_file changed.

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace flagent {

// One emitted diff line. `text` is the line content without its trailing newline.
struct DiffLine {
    enum class Op { Context, Add, Del } op;
    std::string text;
};

// Classic LCS line diff of `old_text` vs `new_text` (each split on '\n').
// Returns the complete diff in order. A trailing newline does not synthesise a
// spurious empty final line. Above a hard line cap (2000 lines/side) the LCS
// table is skipped and the result degrades to "whole file replaced" (all of old
// deleted, all of new added), keeping memory bounded. post: deterministic.
std::vector<DiffLine> diff_lines(std::string_view old_text,
                                 std::string_view new_text);

// Default number of unchanged context lines kept around each change when
// rendering a diff for display (stderr and TUI both use this).
inline constexpr std::size_t kDiffContext = 3;

// Collapse long runs of unchanged lines, keeping at most `context` Context lines
// on each side of every change. A collapsed run of two or more lines becomes a
// single dim "⋯ N unchanged ⋯" Context marker; runs of one line are kept as-is.
// Used to keep the rendered diff focused on what actually changed. post: total.
std::vector<DiffLine> elide_context(const std::vector<DiffLine>& diff,
                                    std::size_t context);

// Render a diff to an ANSI-coloured string (green '+', red '-', dim context),
// one line per entry with a trailing newline. Total (never fails).
std::string render_ansi_diff(const std::vector<DiffLine>& lines);

}  // namespace flagent

#endif  // FLAGENT_DIFF_HPP
