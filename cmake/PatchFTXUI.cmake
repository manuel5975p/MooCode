# Applied to the fetched FTXUI v6.1.9 sources as the FetchContent
# PATCH_COMMAND (working directory = the FTXUI source tree). Three rendering
# fixes; each is idempotent (guarded by a marker string) and fails loudly if
# the upstream source drifts, so a version bump can't silently drop a fix.
#
#  1. screen/string.cpp — supplement the fullwidth table (Unicode 13 era) with
#     the Unicode 14-16 EastAsianWidth W/F ranges. Modern terminals draw these
#     two cells wide; FTXUI counting one shifts every cell right of the glyph
#     on that row (drifted separators) and leaves stale cells on repaint.
#  2. dom/text.cpp — never draw a fullwidth glyph whose reserved second cell
#     is clipped off by the element box: the orphaned half-glyph makes
#     Screen::ToString skip the neighbouring element's pixel (e.g. the pane
#     separator) since it skips the cell following any fullwidth glyph.
#  3. component/screen_interactive.cpp — wrap the frame write in DEC private
#     mode 2026 (synchronized update) so supporting terminals present each
#     frame atomically (no mid-frame artifacts); others ignore the sequence.

cmake_minimum_required(VERSION 3.20)

# Replace `old` with `new` in `path` unless `marker` is already present.
# pre: run with CWD = FTXUI source root (FetchContent guarantees this).
function(ftxui_patch path marker old new)
  set(file "${CMAKE_CURRENT_SOURCE_DIR}/${path}")
  if(NOT EXISTS "${file}")
    message(FATAL_ERROR "PatchFTXUI: ${file} not found — not the FTXUI source dir?")
  endif()
  file(READ "${file}" content)
  string(FIND "${content}" "${marker}" already)
  if(NOT already EQUAL -1)
    return()
  endif()
  string(FIND "${content}" "${old}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "PatchFTXUI: pattern not found in ${path} — upstream changed, re-derive this patch")
  endif()
  string(REPLACE "${old}" "${new}" content "${content}")
  file(WRITE "${file}" "${content}")
  message(STATUS "PatchFTXUI: patched ${path}")
endfunction()

# --- 1. Unicode 14-16 fullwidth supplement -----------------------------------
# Ranges = Unicode 16.0 EastAsianWidth W/F minus g_full_width_characters
# (mechanically diffed; mostly emoji like U+1FAE0 melting face, U+1FAF0 block).
ftxui_patch(src/ftxui/screen/string.cpp
  "g_full_width_supplement"
  [=[bool IsFullWidth(uint32_t ucs) {
  if (ucs < 0x0300)  // Quick path: // NOLINT
    return false;

  return Bisearch(ucs, g_full_width_characters);
}]=]
  [=[// moocode patch (cmake/PatchFTXUI.cmake): Unicode 14-16 additions missing
// from the Unicode 13 table above, diffed against Unicode 16 EastAsianWidth
// W/F. Modern terminals render these two cells wide; under-counting them
// desyncs FTXUI's in-place frame overwrites.
constexpr std::array<Interval, 30> g_full_width_supplement = {{
    {0x02630, 0x02637}, {0x0268a, 0x0268f}, {0x02ffc, 0x02fff},
    {0x031e4, 0x031e5}, {0x031ef, 0x031ef}, {0x04dc0, 0x04dff},
    {0x18cff, 0x18cff}, {0x1aff0, 0x1aff3}, {0x1aff5, 0x1affb},
    {0x1affd, 0x1affe}, {0x1b11f, 0x1b122}, {0x1b132, 0x1b132},
    {0x1b155, 0x1b155}, {0x1d300, 0x1d356}, {0x1d360, 0x1d376},
    {0x1f6dc, 0x1f6df}, {0x1f7f0, 0x1f7f0}, {0x1f979, 0x1f979},
    {0x1f9cc, 0x1f9cc}, {0x1fa75, 0x1fa77}, {0x1fa7b, 0x1fa7c},
    {0x1fa87, 0x1fa89}, {0x1fa8f, 0x1fa8f}, {0x1faa9, 0x1faaf},
    {0x1fab7, 0x1fabf}, {0x1fac3, 0x1fac6}, {0x1face, 0x1facf},
    {0x1fad7, 0x1fadc}, {0x1fadf, 0x1fae9}, {0x1faf0, 0x1faf8},
}};

bool IsFullWidth(uint32_t ucs) {
  if (ucs < 0x0300)  // Quick path: // NOLINT
    return false;

  return Bisearch(ucs, g_full_width_characters) ||
         Bisearch(ucs, g_full_width_supplement);
}]=]
)

# --- 2. half-glyph clip fix ---------------------------------------------------
ftxui_patch(src/ftxui/dom/text.cpp
  "moocode patch"
  [=[    for (const auto& cell : Utf8ToGlyphs(text_)) {
      if (x > box_.x_max) {
        break;
      }
      if (cell == "\n") {
        continue;
      }
      screen.PixelAt(x, y).character = cell;]=]
  [=[    const auto cells = Utf8ToGlyphs(text_);
    for (std::size_t i = 0; i < cells.size(); ++i) {
      const auto& cell = cells[i];
      if (x > box_.x_max) {
        break;
      }
      if (cell == "\n") {
        continue;
      }
      // moocode patch (cmake/PatchFTXUI.cmake): a fullwidth glyph reserves
      // the next cell (the empty string Utf8ToGlyphs appends). If that
      // reserved cell falls outside the box, drawing the glyph would bleed
      // into the neighbouring element's pixel and make Screen::ToString skip
      // it (ToString skips the cell after any fullwidth glyph). Drop the
      // unrepresentable half-glyph instead.
      if (x == box_.x_max && i + 1 < cells.size() && cells[i + 1].empty()) {
        break;
      }
      screen.PixelAt(x, y).character = cell;]=]
)

# --- 3. synchronized updates (DEC 2026) ---------------------------------------
ftxui_patch(src/ftxui/component/screen_interactive.cpp
  "?2026h"
  [=[  const bool resized = (dimx != dimx_) || (dimy != dimy_);
  ResetCursorPosition();
  std::cout << ResetPosition(/*clear=*/resized);]=]
  [=[  const bool resized = (dimx != dimx_) || (dimy != dimy_);
  // moocode patch (cmake/PatchFTXUI.cmake): begin synchronized update (DEC
  // private mode 2026); supporting terminals present the frame atomically.
  std::cout << "\033[?2026h";
  ResetCursorPosition();
  std::cout << ResetPosition(/*clear=*/resized);]=]
)
ftxui_patch(src/ftxui/component/screen_interactive.cpp
  "?2026l"
  [=[  std::cout << ToString() << set_cursor_position;
  Flush();]=]
  [=[  std::cout << ToString() << set_cursor_position;
  std::cout << "\033[?2026l";  // moocode patch: end synchronized update
  Flush();]=]
)
