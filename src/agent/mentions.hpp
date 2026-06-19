#ifndef MOOCODE_MENTIONS_HPP
#define MOOCODE_MENTIONS_HPP

// @-mentions: when a user prompt contains a token like "@path/to/file.cpp"
// (or "@src/*.hpp"), the agent auto-reads the referenced file(s) and folds
// their contents into the message that the model sees, with a one-line
// "kind + size + key elements" header per file. Globs and directories are
// expanded; resolutions stay under `root` like the other filesystem tools.
// pre: opt.root names an existing directory.

#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "agent/types.hpp"  // ImageBlock

namespace moocode {

struct MentionOptions {
    std::filesystem::path root;             // confinement root (same rules as builtin tools)
    std::size_t max_file_bytes = 64 * 1024; // per-file cap; truncates with a marker
    std::size_t max_total_bytes = 256 * 1024; // total cap across all attachments
    std::size_t max_files = 32;             // hard cap on the number of attachments
    bool include_suggestions = true;        // include the "key elements" line per file
    bool recursive_globs = false;           // expand "**" across subdirectories
};

struct MentionEntry {
    std::string path;        // path as it appeared in the prompt (e.g. "src/*.cpp")
    std::string resolved;    // absolute path actually read (empty on error)
    std::string kind;        // "C++ source", "Markdown", "binary", "directory listing", …
    std::size_t bytes = 0;   // original (pre-truncation) byte size
    std::size_t lines = 0;   // line count (0 for binary)
    std::vector<std::string> suggestions;  // key elements: class/function/heading names
    std::string content;     // the file's contents (or directory listing), possibly truncated
    std::string error;       // non-empty on read/resolve failure
    // When true, this entry is an image file that was encoded rather than
    // read as text. `image_base64` and `image_media_type` are set; `content`
    // holds a one-line "[image: N bytes, type]" placeholder for the prompt.
    bool is_image = false;
    std::string image_base64;
    std::string image_media_type;
};

struct MentionResult {
    std::string prompt;          // the augmented prompt (original + attachments)
    std::vector<MentionEntry> entries;  // one per resolved attachment, in document order
    std::size_t total_bytes = 0; // sum of entry.bytes (pre-truncation)
    // Image attachments extracted from mentions. The caller builds ContentPart
    // blocks from these; they are NOT included in the prompt text.
    std::vector<ImageBlock> images;
};

// Scan `prompt` for word-bounded @-tokens, resolve each under `root`, and
// return a new prompt whose body is the original text followed by an
// "Attached files" section listing every resolved file with its contents.
// Tokens that don't look like paths (e.g. emails like "user@host") are
// ignored. Duplicates are folded. pre: opt.root exists. post: never throws.
MentionResult expand_mentions(std::string prompt, const MentionOptions& opt);

// --- @-mention autocomplete -------------------------------------------------
// Editor-side helpers for completing an @-token as the user types. Pure and
// terminal-free so the TUI stays thin and these stay unit-testable.

// One completion candidate for the popup.
struct MentionCompletion {
    std::string insert;   // full token text after '@' to splice in (e.g. "src/agent/")
    std::string display;  // label shown in the popup (same as insert)
    bool is_dir = false;  // dirs sort first, end with '/', keep the popup open
};

// The @-token the cursor currently sits in. `active` is false when the cursor
// is not inside an @-token (popup hidden). Indices are byte offsets into the
// line. `typed` is the token text from just-after-'@' up to the cursor (what we
// match on); [token_begin, token_end) is the whole token span (what we replace).
struct MentionContext {
    bool active = false;
    std::size_t token_begin = 0;  // index just after '@'
    std::size_t token_end = 0;    // forward end of the token (first terminator/EOL)
    std::string typed;            // line[token_begin, cursor)
};

// Locate the @-token containing `cursor` (a byte index, 0..line.size()).
// An @-token is triggered by '@' at line start or after whitespace, and runs
// until the first @-terminator. post: never throws.
MentionContext mention_context_at(std::string_view line, std::size_t cursor);

// Directory-level prefix completions for `typed` under `root`. Splits `typed`
// at the last '/': the left part is the directory (resolved under `root` via
// the same sandbox rules as expand_mentions), the trailing segment is matched
// case-insensitively as a prefix against that directory's entries. Dirs sort
// before files, each lexicographically; dotfiles are hidden unless the segment
// itself starts with '.'. Returns at most `max` candidates.
//
// Fuzzy fallback: when the prefix phase yields fewer than a small threshold of
// candidates AND the typed token has no '/' (i.e. the user is still at the
// root), the whole tree under `root` is walked and files whose basename
// contains the typed segment as a subsequence (case-insensitive) are appended.
// So typing "test" surfaces "src/test.cpp" even though it is not in the root.
// Build/cache/VCS directories are pruned from the walk. post: never throws;
// empty vector on any resolve/IO error or sandbox escape.
std::vector<MentionCompletion>
complete_mention(std::string_view typed, const std::filesystem::path& root,
                 std::size_t max = 50);

// Splice `c.insert` over [ctx.token_begin, ctx.token_end) in `line`. Returns the
// new line and the new cursor index (end of the inserted token). pre: ctx.active.
std::pair<std::string, std::size_t>
apply_completion(std::string_view line, const MentionContext& ctx,
                 const MentionCompletion& c);

}  // namespace moocode

#endif  // MOOCODE_MENTIONS_HPP
