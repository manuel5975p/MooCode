#include "agent/mentions.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <regex>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "agent/fsutil.hpp"
#include "agent/image_util.hpp"
#include "agent/strutil.hpp"

namespace fs = std::filesystem;

namespace flagent {

namespace {

// Glob matcher: '*' = run of non-`/` chars, '?' = single non-`/` char, else
// literal. Stops at '/' in both pattern and text. `**` is NOT special: we
// treat it as two `*` runs that must straddle a `/`, which is rarely what
// people want; users that need recursion should enable recursive_globs and
// call expand_glob() instead. pre: both are non-empty.
bool glob_match(std::string_view pat, std::string_view text) {
    std::size_t pi = 0, ti = 0;
    std::size_t star_pi = std::string::npos, star_ti = 0;
    while (ti < text.size()) {
        if (pi < pat.size() && (pat[pi] == '?' || pat[pi] == text[ti])) {
            ++pi;
            ++ti;
        } else if (pi < pat.size() && pat[pi] == '*') {
            star_pi = pi++;
            star_ti = ti;
        } else if (star_pi != std::string::npos) {
            pi = star_pi + 1;
            ti = ++star_ti;
        } else {
            return false;
        }
    }
    while (pi < pat.size() && pat[pi] == '*') ++pi;
    return pi == pat.size();
}

// True when `c` (a single character) is one of the punctuation characters
// that always end a path token. Used both as the main switch's case list
// and as a lookahead helper for the period rule below.
bool is_punct_terminator(char c) {
    switch (c) {
        case ' ': case '\t': case '\n': case '\r':
        case ',': case ';': case ':': case '?': case '!':
        case '(': case ')': case '[': case ']': case '{': case '}':
        case '<': case '>': case '"': case '\'': case '`':
        case '|': case '&':
            return true;
        default:
            return false;
    }
}

// True when `s[i]` ends a path token. Period is the tricky case: a period
// in "a.cpp" (followed by another filename char) is part of the path, but a
// trailing period in "a.cpp." (followed by EOF, whitespace, or punctuation)
// is sentence punctuation. We treat a period as a terminator iff the
// previous char is a word character AND the next char is EOF or a punct
// terminator. A leading period ("./foo", "../foo", ".gitignore") is never
// a terminator.
bool is_terminator_at(std::string_view s, std::size_t i) {
    char c = s[i];
    if (is_punct_terminator(c)) return true;
    if (c != '.') return false;
    if (i == 0) return false;
    char prev = s[i - 1];
    if (!std::isalnum(static_cast<unsigned char>(prev)) && prev != '_')
        return false;
    if (i + 1 >= s.size()) return true;  // trailing period at EOF
    return is_punct_terminator(s[i + 1]);
}

// Heuristic: does the token look like a file path (not an email or stray "@")?
// A path must not contain '@' (mid-token '@' is the email/mention trigger) and
// must contain at least one of: a path separator, a wildcard (glob), or a dot
// (file with an extension or a directory with a dot in its name). This rules
// out bare words like "use sub arrays" but accepts `sub/`, `./sub`, `*.cpp`,
// `user@example.com` is rejected via the @ check, etc.
bool looks_like_path(std::string_view t) {
    if (t.empty() || t.find('@') != std::string::npos) return false;
    if (t.find('/') != std::string::npos) return true;
    if (t.find('\\') != std::string::npos) return true;
    if (t.find('*') != std::string::npos) return true;
    if (t.find('?') != std::string::npos) return true;
    if (t.find('.') != std::string::npos) return true;
    if (t.size() >= 2 && t[0] == '.' && (t[1] == '/' || t[1] == '.')) return true;
    if (!t.empty() && t[0] == '/') return true;
    return false;
}

// Find the next "@" in `s` at position >= `from` that is at the start of the
// string or immediately preceded by whitespace. Returns the position of the
// '@' or std::string::npos.
std::size_t find_trigger(std::string_view s, std::size_t from) {
    for (std::size_t i = from; i < s.size(); ++i) {
        if (s[i] != '@') continue;
        if (i == 0) return i;
        char prev = s[i - 1];
        if (std::isspace(static_cast<unsigned char>(prev))) return i;
    }
    return std::string::npos;
}

// Extract the path token starting at `at` (the '@' position). The token runs
// until the first terminator or end of string. Returns the substring and
// `end` (one past the last char of the token, so callers can continue from
// there). An empty token ("@@" or "@ ") yields {"", at+1} or {at+1, at+1}.
struct Token {
    std::string text;
    std::size_t end;
};

Token extract_token(std::string_view s, std::size_t at) {
    std::size_t i = at + 1;  // skip '@'
    std::size_t start = i;
    while (i < s.size() && !is_terminator_at(s, i)) ++i;
    return Token{std::string(s.substr(start, i - start)), i};
}

// Returns true if `bytes` (which must be at least `n` long) looks like a
// binary file: a NUL byte in the first chunk is a strong signal.
bool looks_binary(const std::string& bytes) {
    std::size_t n = std::min<std::size_t>(bytes.size(), 8192);
    return bytes.find('\0', 0) < n;
}

// Count newlines in `s`. An unterminated final line still counts.
std::size_t count_lines(const std::string& s) {
    if (s.empty()) return 0;
    return static_cast<std::size_t>(std::count(s.begin(), s.end(), '\n')) +
           (s.back() != '\n' ? 1 : 0);
}

// Strip a UTF-8 BOM if present, returning a new string.
std::string strip_bom(std::string s) {
    if (s.size() >= 3 &&
        static_cast<unsigned char>(s[0]) == 0xEF &&
        static_cast<unsigned char>(s[1]) == 0xBB &&
        static_cast<unsigned char>(s[2]) == 0xBF) {
        s.erase(0, 3);
    }
    return s;
}

// Map a file extension to a human-readable kind label. Returns "" if unknown.
std::string kind_for(const std::string& ext_lower) {
    if (ext_lower == ".c" || ext_lower == ".cc" || ext_lower == ".cpp" ||
        ext_lower == ".cxx" || ext_lower == ".h" || ext_lower == ".hpp" ||
        ext_lower == ".hxx")
        return "C/C++ source";
    if (ext_lower == ".py" || ext_lower == ".pyi" || ext_lower == ".pyx")
        return "Python source";
    if (ext_lower == ".rs") return "Rust source";
    if (ext_lower == ".go") return "Go source";
    if (ext_lower == ".js" || ext_lower == ".jsx" || ext_lower == ".mjs" ||
        ext_lower == ".cjs")
        return "JavaScript source";
    if (ext_lower == ".ts" || ext_lower == ".tsx") return "TypeScript source";
    if (ext_lower == ".java") return "Java source";
    if (ext_lower == ".rb") return "Ruby source";
    if (ext_lower == ".sh" || ext_lower == ".bash" || ext_lower == ".zsh")
        return "shell script";
    if (ext_lower == ".html" || ext_lower == ".htm") return "HTML";
    if (ext_lower == ".css" || ext_lower == ".scss" || ext_lower == ".less")
        return "stylesheet";
    if (ext_lower == ".md" || ext_lower == ".markdown") return "Markdown";
    if (ext_lower == ".rst") return "reStructuredText";
    if (ext_lower == ".txt") return "text";
    if (ext_lower == ".json" || ext_lower == ".jsonc") return "JSON";
    if (ext_lower == ".yaml" || ext_lower == ".yml") return "YAML";
    if (ext_lower == ".toml") return "TOML";
    if (ext_lower == ".xml") return "XML";
    if (ext_lower == ".diff" || ext_lower == ".patch") return "diff";
    if (ext_lower == ".sql") return "SQL";
    if (ext_lower == ".csv") return "CSV";
    if (ext_lower == ".env") return "env file";
    if (ext_lower == ".lock") return "lock file";
    return "";
}

// Lowercased extension, with the leading dot.
std::string ext_lower(const std::string& path) {
    auto dot = path.rfind('.');
    if (dot == std::string::npos) return "";
    return to_lower(path.substr(dot));
}

// Suggestion extractors: scan the first N lines of `content` and pull out a
// short list of "key elements" appropriate to the file's language. Returns an
// empty list when nothing relevant was found. The cap on output is a safety
// belt: a 10k-token file shouldn't produce 10k suggestions.
std::vector<std::string> code_suggestions(std::string_view content,
                                          const std::string& kind) {
    static constexpr std::size_t kMaxSuggestions = 16;
    static constexpr std::size_t kMaxScanLines = 4000;
    // Compile each language pattern exactly once (C++23 magic statics make the
    // one-time init thread-safe) and select by pointer, rather than rebuilding
    // a std::regex on every call. Patterns are byte-identical to the originals.
    // We feed lines one at a time, so `^` is naturally the start of the line —
    // no (?m) flag needed (and std::regex/ECMAScript doesn't accept one anyway).
    static const std::regex kCpp(
        R"(^[ \t]*(?:class|struct|enum(?:\s+class)?|union|namespace)\s+([A-Za-z_]\w*))");
    static const std::regex kPy(R"(^[ \t]*(?:def|class)\s+([A-Za-z_]\w*))");
    static const std::regex kRust(
        R"(^[ \t]*(?:fn|struct|enum|trait|impl(?:\s+[A-Za-z_]\w*)?(?:\s+for)?\s+)([A-Za-z_]\w*))");
    static const std::regex kGo(
        R"(^[ \t]*(?:func(?:\s*\(\s*\w+\s+\*?\w+\s*\))?|type)\s+([A-Za-z_]\w*))");
    static const std::regex kJs(
        R"(^[ \t]*(?:export\s+)?(?:async\s+)?(?:class|function)\s+([A-Za-z_$][\w$]*))");
    static const std::regex kJava(
        R"(^[ \t]*(?:public|private|protected|static|abstract|final|\s)*\s*(?:class|interface|enum)\s+([A-Za-z_]\w*))");
    std::vector<std::string> out;
    const std::regex* re = nullptr;
    if (kind == "C/C++ source")              re = &kCpp;
    else if (kind == "Python source")        re = &kPy;
    else if (kind == "Rust source")          re = &kRust;
    else if (kind == "Go source")            re = &kGo;
    else if (kind == "JavaScript source" || kind == "TypeScript source") re = &kJs;
    else if (kind == "Java source")          re = &kJava;
    else return out;
    std::size_t lines_seen = 0;
    std::size_t pos = 0;
    while (pos < content.size() && lines_seen < kMaxScanLines &&
           out.size() < kMaxSuggestions) {
        std::size_t nl = content.find('\n', pos);
        std::string_view line = content.substr(
            pos, nl == std::string::npos ? std::string::npos : nl - pos);
        std::string s(line);
        std::smatch m;
        if (std::regex_search(s, m, *re)) {
            std::string name = m[1].str();
            if (std::find(out.begin(), out.end(), name) == out.end())
                out.push_back(std::move(name));
        }
        if (nl == std::string::npos) break;
        pos = nl + 1;
        ++lines_seen;
    }
    return out;
}

std::vector<std::string> markdown_suggestions(std::string_view content) {
    static constexpr std::size_t kMaxSuggestions = 16;
    static constexpr std::size_t kMaxScanLines = 2000;
    std::vector<std::string> out;
    std::size_t pos = 0;
    std::size_t lines = 0;
    static const std::regex heading(R"(^[ \t]{0,3}(#{1,6})\s+(.+?)\s*#*\s*$)");
    while (pos < content.size() && lines < kMaxScanLines &&
           out.size() < kMaxSuggestions) {
        std::size_t nl = content.find('\n', pos);
        std::string_view line = content.substr(
            pos, nl == std::string::npos ? std::string::npos : nl - pos);
        std::string s(line);
        std::smatch m;
        if (std::regex_search(s, m, heading)) {
            std::string text = m[2].str();
            // Trim trailing # and whitespace, then collapse internal ws.
            while (!text.empty() && (text.back() == '#' || text.back() == ' '))
                text.pop_back();
            if (std::find(out.begin(), out.end(), text) == out.end())
                out.push_back(std::move(text));
        }
        if (nl == std::string::npos) break;
        pos = nl + 1;
        ++lines;
    }
    return out;
}

// Marker appended when an attached file is capped:
// "\n[…truncated, see M/F bytes shown]". Kept verbatim per call site.
std::string file_truncate_marker(std::size_t shown, std::size_t full) {
    return "\n[…truncated, see " + std::to_string(shown) + "/" +
           std::to_string(full) + " bytes shown]";
}

// Format bytes for the header line: <1024 => "NNN B", <1MB => "N.N KB", else
// "N.N MB". pre: n > 0.
std::string human_bytes(std::size_t n) {
    if (n < 1024) return std::to_string(n) + " B";
    if (n < 1024 * 1024) {
        char buf[16];
        std::snprintf(buf, sizeof buf, "%.1f KB", n / 1024.0);
        return buf;
    }
    char buf[16];
    std::snprintf(buf, sizeof buf, "%.1f MB", n / (1024.0 * 1024));
    return buf;
}

// Render one attached file as a block the model can parse: a "### path"
// header with a metadata line, optional suggestions, then a fenced code
// block carrying the file's contents. Image entries get a short placeholder.
std::string render_entry(const MentionEntry& e) {
    std::ostringstream os;
    os << "### @" << e.path;
    if (e.error.empty()) {
        if (e.is_image) {
            os << "\n- kind: image (" << e.image_media_type << ", "
               << human_bytes(e.bytes) << ", base64-encoded)";
            os << "\n- " << e.content;  // "[image: N bytes, type]" placeholder
        } else {
            os << "\n- kind: " << (e.kind.empty() ? "file" : e.kind)
               << "  (" << e.lines << " lines, " << human_bytes(e.bytes) << ")";
            if (!e.suggestions.empty()) {
                os << "\n- key elements: ";
                for (std::size_t i = 0; i < e.suggestions.size(); ++i) {
                    if (i) os << ", ";
                    os << "`" << e.suggestions[i] << "`";
                }
            }
            os << "\n\n```\n" << e.content << "\n```\n";
        }
    } else {
        os << "\n- error: " << e.error << "\n";
    }
    return os.str();
}

// Format the "Attached files" section (the "intro" header + one block per
// entry). The agent is told the source of these attachments so it can ground
// its suggestions in the actual file contents.
std::string render_attached_section(const std::vector<MentionEntry>& entries) {
    if (entries.empty()) return {};
    std::size_t ok = 0;
    for (const auto& e : entries) if (e.error.empty()) ++ok;
    std::ostringstream os;
    os << "\n\n---\n\nThe following file(s) were auto-attached via @-mentions in "
          "your prompt (use them to ground your answer; the user has not "
          "explicitly pasted them):\n\n";
    for (const auto& e : entries) os << render_entry(e) << "\n";
    os << "If a file is binary or unreadable, work from the metadata header. "
          "Do not re-read these files unless the user asks you to — they are "
          "already in your context.\n";
    (void)ok;
    return os.str();
}

// List a non-recursive directory's children. Returns the formatted listing.
MentionEntry list_directory(const fs::path& abs, const std::string& display) {
    MentionEntry e;
    e.path = display;
    e.resolved = abs.string();
    e.kind = "directory listing";
    std::vector<std::string> names;
    std::error_code ec;
    for (const auto& child : fs::directory_iterator(abs, ec)) {
        std::string n = child.path().filename().string();
        std::error_code ec2;
        if (child.is_directory(ec2)) n += "/";
        names.push_back(std::move(n));
    }
    if (ec) {
        e.error = "cannot read directory: " + ec.message();
        return e;
    }
    std::ranges::sort(names);
    std::ostringstream os;
    for (const auto& n : names) os << n << "\n";
    e.content = os.str();
    e.bytes = e.content.size();
    e.lines = count_lines(e.content);
    return e;
}

// Build a MentionEntry for one resolved file. May produce an error entry
// (e.g. binary, missing, too large to read). `display` is the path string
// exactly as it appeared in the prompt.
MentionEntry read_file_entry(const fs::path& abs, const std::string& display,
                             const MentionOptions& opt) {
    MentionEntry e;
    e.path = display;
    e.resolved = abs.string();
    std::error_code ec;
    if (!fs::is_regular_file(abs, ec)) {
        e.error = "not a regular file";
        return e;
    }
    std::string raw = slurp(abs);
    e.bytes = raw.size();
    raw = strip_bom(std::move(raw));
    if (looks_binary(raw)) {
        e.kind = "binary";
        e.content = "[binary file, contents omitted]";
        e.lines = 0;
        e.suggestions = {};
        return e;
    }
    e.kind = kind_for(ext_lower(abs.string()));
    if (e.kind.empty()) e.kind = "file";
    e.lines = count_lines(raw);
    if (opt.include_suggestions) {
        if (e.kind == "Markdown")
            e.suggestions = markdown_suggestions(raw);
        else
            e.suggestions = code_suggestions(raw, e.kind);
    }
    if (raw.size() > opt.max_file_bytes) {
        e.content =
            truncate(std::move(raw), opt.max_file_bytes, file_truncate_marker);
    } else {
        e.content = std::move(raw);
    }
    return e;
}

// Enumerate files matching `pattern` (a single-path glob). When `recursive` is
// true, the ** token descends into subdirectories. Returns paths in sorted
// order, with hidden files (dotfiles) skipped.
std::vector<fs::path> expand_glob(const fs::path& base, const std::string& pattern,
                                  bool recursive) {
    std::vector<fs::path> out;
    fs::path pat(pattern);
    // Walk the path: for each prefix that exists as a directory, iterate it
    // and match the remainder as a glob against the entry name. The first
    // existing prefix is the "anchor"; everything before it must be literal.
    fs::path anchor = base;
    std::size_t idx = 0;
    std::error_code ec;
    while (idx < pat.string().size()) {
        fs::path next_anchor;
        std::size_t i = idx;
        // Consume a literal segment.
        while (i < pat.string().size()) {
            char c = pat.string()[i];
            if (c == '*' || c == '?' || c == '[') break;
            ++i;
        }
        if (i > idx) {
            anchor = anchor / pat.string().substr(idx, i - idx);
            if (!fs::is_directory(anchor, ec)) {
                // Pattern's literal prefix doesn't exist => no matches.
                return out;
            }
            idx = i;
            continue;
        }
        // Hit a glob segment at position idx: this is the anchor's child.
        std::size_t seg_start = idx;
        while (idx < pat.string().size() && pat.string()[idx] != '/') ++idx;
        std::string seg_pat = pat.string().substr(seg_start, idx - seg_start);
        std::vector<std::string> rest_segs;
        bool had_sep = idx < pat.string().size();
        if (had_sep) ++idx;
        while (idx < pat.string().size()) {
            std::size_t s = idx;
            while (idx < pat.string().size() && pat.string()[idx] != '/') ++idx;
            rest_segs.push_back(pat.string().substr(s, idx - s));
            if (idx < pat.string().size()) ++idx;
        }
        auto collect = [&](const fs::path& dir) {
            std::error_code iec;
            for (const auto& entry : fs::directory_iterator(dir, iec)) {
                std::string name = entry.path().filename().string();
                if (!name.empty() && name[0] == '.') continue;  // skip dotfiles
                if (!glob_match(seg_pat, name)) continue;
                fs::path child = dir / name;
                std::error_code rec;
                bool is_dir = entry.is_directory(rec);
                if (rest_segs.empty()) {
                    if (is_dir) continue;  // trailing glob => files only
                    out.push_back(child);
                } else if (is_dir) {
                    // Re-apply remaining segments under child.
                    std::string sub_pat = child.filename().string();
                    for (const auto& s : rest_segs) sub_pat += "/" + s;
                    auto sub = expand_glob(child, sub_pat, recursive);
                    out.insert(out.end(), sub.begin(), sub.end());
                } else if (recursive && rest_segs.size() == 1 && rest_segs[0] == "**") {
                    // `/**` => everything under; recurse via recursive_directory_iterator.
                    std::error_code rec2;
                    for (const auto& sub : fs::recursive_directory_iterator(
                             child, rec2)) {
                        if (sub.is_regular_file(rec2))
                            out.push_back(sub.path());
                    }
                }
            }
        };
        collect(anchor);
        std::ranges::sort(out);
        return out;
    }
    return out;
}

// Per-token entry processor. Resolves the token against `root`, classifies,
// and produces one or more MentionEntry values (a glob may produce several).
// Errors are recorded in a single entry rather than aborting the whole pass.
// Image files are base64-encoded and marked as images so the caller can build
// ContentPart blocks rather than embedding them as text.
void process_token(const std::string& token, const MentionOptions& opt,
                   std::vector<MentionEntry>& out, std::size_t& total_bytes,
                   std::vector<ImageBlock>& images) {
    if (!looks_like_path(token)) return;  // silently drop emails etc.
    auto resolved = resolve_in_root(opt.root, token);
    if (!resolved) {
        MentionEntry e;
        e.path = token;
        e.error = "path escapes the sandbox root: " + token;
        out.push_back(std::move(e));
        return;
    }
    std::error_code ec;
    bool is_dir = fs::is_directory(*resolved, ec);
    bool is_reg = fs::is_regular_file(*resolved, ec);
    if (!is_dir && !is_reg) {
        // Not a regular file or directory. Could be a glob, a non-existent
        // path with wildcards, or a special file. Try the glob path.
        if (token.find('*') == std::string::npos && token.find('?') == std::string::npos) {
            MentionEntry e;
            e.path = token;
            e.resolved = resolved->string();
            e.error = "no such file or directory";
            out.push_back(std::move(e));
            return;
        }
        // Glob: walk the parent.
        fs::path parent = resolved->parent_path();
        std::string base_pat = resolved->filename().string();
        if (parent.empty() || !fs::is_directory(parent, ec)) {
            MentionEntry e;
            e.path = token;
            e.error = "glob has no parent directory";
            out.push_back(std::move(e));
            return;
        }
        std::vector<fs::path> matches;
        std::error_code iec;
        for (const auto& entry : fs::directory_iterator(parent, iec)) {
            std::string name = entry.path().filename().string();
            if (!glob_match(base_pat, name)) continue;
            if (!entry.is_regular_file(iec)) continue;
            matches.push_back(entry.path());
        }
        if (matches.empty()) {
            MentionEntry e;
            e.path = token;
            e.error = "glob matched no files";
            out.push_back(std::move(e));
            return;
        }
        std::ranges::sort(matches);
        for (const auto& m : matches) {
            if (out.size() >= opt.max_files) break;
            if (total_bytes >= opt.max_total_bytes) break;
            MentionEntry e = read_file_entry(m, token + " -> " + m.string(), opt);
            total_bytes += e.bytes;
            out.push_back(std::move(e));
        }
        return;
    }
    if (is_dir) {
        // Directories are listed (non-recursive) so the model sees what is in
        // them; deep traversal can be done with a glob like "src/**".
        if (out.size() >= opt.max_files) {
            MentionEntry e;
            e.path = token;
            e.error = "max_files reached; skipping further attachments";
            out.push_back(std::move(e));
            return;
        }
        MentionEntry e = list_directory(*resolved, token);
        total_bytes += e.bytes;
        out.push_back(std::move(e));
        return;
    }
    if (total_bytes >= opt.max_total_bytes) {
        MentionEntry e;
        e.path = token;
        e.error = "max_total_bytes reached; skipping further attachments";
        out.push_back(std::move(e));
        return;
    }
    if (out.size() >= opt.max_files) {
        MentionEntry e;
        e.path = token;
        e.error = "max_files reached; skipping further attachments";
        out.push_back(std::move(e));
        return;
    }
    // Image files: base64-encode and mark as image, rather than reading as text.
    if (is_image_extension(token)) {
        auto img = read_image(*resolved);
        if (!img) {
            MentionEntry e;
            e.path = token;
            e.resolved = resolved->string();
            e.error = img.error().msg;
            out.push_back(std::move(e));
            return;
        }
        MentionEntry e;
        e.path = token;
        e.resolved = resolved->string();
        e.kind = "image";
        e.bytes = img->base64_data.size();
        e.lines = 0;
        e.is_image = true;
        e.image_base64 = img->base64_data;
        e.image_media_type = img->media_type;
        e.content = "[image: " + human_bytes(e.bytes) + ", " + e.image_media_type + "]";
        total_bytes += e.bytes;
        images.push_back(std::move(*img));
        out.push_back(std::move(e));
        return;
    }
    MentionEntry e = read_file_entry(*resolved, token, opt);
    total_bytes += e.bytes;
    out.push_back(std::move(e));
}

}  // namespace

MentionResult expand_mentions(std::string prompt, const MentionOptions& opt) {
    MentionResult result;
    // Tokenise against the original prompt (string_view of the caller's copy
    // is fine: the loop never mutates it). The augmented prompt is built
    // last, so we don't pay for the appended "Attached files" section during
    // the search itself.
    result.prompt = prompt;

    // Collect (token, original_path) pairs in document order, dedup'd by path.
    std::vector<std::string> tokens;
    std::vector<std::string> seen;
    std::string_view view(prompt);
    std::size_t cursor = 0;
    while (cursor < view.size()) {
        std::size_t at = find_trigger(view, cursor);
        if (at == std::string::npos) break;
        Token tok = extract_token(view, at);
        cursor = tok.end;
        if (tok.text.empty()) continue;
        if (!looks_like_path(tok.text)) continue;
        if (std::find(seen.begin(), seen.end(), tok.text) != seen.end()) continue;
        seen.push_back(tok.text);
        tokens.push_back(std::move(tok.text));
    }

    std::size_t total_bytes = 0;
    for (const auto& t : tokens) {
        process_token(t, opt, result.entries, total_bytes, result.images);
    }
    result.total_bytes = total_bytes;

    if (!result.entries.empty()) {
        result.prompt += render_attached_section(result.entries);
    }
    return result;
}

MentionContext mention_context_at(std::string_view line, std::size_t cursor) {
    MentionContext ctx;
    if (cursor > line.size()) cursor = line.size();
    // Walk backwards from the cursor to find the '@' that opens this token.
    // Stop if we hit a terminator/whitespace first (then there is no token).
    std::size_t i = cursor;
    // Byte-oriented scan; safe for UTF-8 paths because every terminator is ASCII.
    while (i > 0) {
        char c = line[i - 1];
        if (c == '@') {
            // Trigger only if at start or preceded by whitespace.
            if (i - 1 == 0 ||
                std::isspace(static_cast<unsigned char>(line[i - 2]))) {
                ctx.token_begin = i;  // index just after '@'
                break;
            }
            return ctx;  // '@' not a trigger (e.g. email) => inactive
        }
        if (is_terminator_at(line, i - 1)) return ctx;  // space/punct => no token
        --i;
    }
    if (i == 0) return ctx;  // reached start without finding a trigger '@'
    ctx.active = true;
    ctx.typed = std::string(line.substr(ctx.token_begin, cursor - ctx.token_begin));
    // Forward end of the token: first terminator at or after the cursor.
    std::size_t j = cursor;
    while (j < line.size() && !is_terminator_at(line, j)) ++j;
    ctx.token_end = j;
    return ctx;
}

std::vector<MentionCompletion>
complete_mention(std::string_view typed, const fs::path& root, std::size_t max) {
    std::vector<MentionCompletion> out;
    std::string t(typed);
    std::size_t slash = t.find_last_of('/');
    std::string dir_rel = (slash == std::string::npos) ? "" : t.substr(0, slash + 1);
    std::string seg = (slash == std::string::npos) ? t : t.substr(slash + 1);

    auto resolved = resolve_in_root(root, dir_rel);
    if (!resolved) return out;
    std::error_code ec;
    if (!fs::is_directory(*resolved, ec)) return out;

    auto lower = [](std::string s) {
        std::ranges::transform(s, s.begin(), [](unsigned char ch) {
            return static_cast<char>(std::tolower(ch));
        });
        return s;
    };
    const std::string seg_l = lower(seg);
    const bool want_hidden = !seg.empty() && seg[0] == '.';

    std::error_code iter_ec;
    for (const auto& e : fs::directory_iterator(*resolved, iter_ec)) {
        std::string name = e.path().filename().string();
        if (name.empty()) continue;
        if (name[0] == '.' && !want_hidden) continue;
        std::string name_l = lower(name);
        if (name_l.compare(0, seg_l.size(), seg_l) != 0) continue;
        std::error_code dec;
        bool is_dir = e.is_directory(dec);
        std::string insert = dir_rel + name + (is_dir ? "/" : "");
        out.push_back({insert, insert, is_dir});
    }
    if (iter_ec) return {};
    std::ranges::sort(out, [](const MentionCompletion& a, const MentionCompletion& b) {
        if (a.is_dir != b.is_dir) return a.is_dir;
        return a.insert < b.insert;
    });
    if (out.size() > max) out.resize(max);
    return out;
}

std::pair<std::string, std::size_t>
apply_completion(std::string_view line, const MentionContext& ctx,
                 const MentionCompletion& c) {
    if (!ctx.active) return {std::string(line), line.size()};
    std::string out;
    out.reserve(line.size() + c.insert.size());
    out.append(line.substr(0, ctx.token_begin));
    out.append(c.insert);
    std::size_t cursor = out.size();
    out.append(line.substr(ctx.token_end));
    return {std::move(out), cursor};
}

}  // namespace flagent
