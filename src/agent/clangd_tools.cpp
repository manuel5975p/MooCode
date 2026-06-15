#include "agent/clangd_tools.hpp"

#include "agent/builtin_tools.hpp"  // ToolOptions
#include "agent/fsutil.hpp"    // resolve_in_root, slurp
#include "agent/json_util.hpp"  // get_string, get_int
#include "agent/strutil.hpp"   // truncate, to_lower, trim_sv, default_trunc_marker

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace moocode {

namespace {

using Result = std::expected<std::string, Error>;
using lsp::ClangdSession;

constexpr std::size_t kMaxOutput = 30000;  // stay under the agent's 32 KiB tool cap

Result finalize(std::string out) { return truncate(std::move(out), kMaxOutput, default_trunc_marker); }

// LSP SymbolKind (1..26) -> short label. Out-of-range falls back to "symbol".
const char* symbol_kind_name(int k) {
    static const char* kinds[] = {
        "file",    "module",   "namespace", "package", "class",     "method",
        "property", "field",   "constructor", "enum",  "interface", "function",
        "variable", "constant", "string",   "number",  "boolean",   "array",
        "object",  "key",      "null",      "enum-member", "struct", "event",
        "operator", "type-param"};
    if (k >= 1 && k <= 26) return kinds[k - 1];
    return "symbol";
}

std::expected<void, Error> require_cxx_source(const fs::path& p) {
    static const std::vector<std::string> ok = {
        ".c",  ".cc",  ".cpp", ".cxx", ".c++", ".h",   ".hh",
        ".hpp", ".hxx", ".h++", ".ipp", ".tpp", ".inl", ".cu",
        ".cuh", ".m",   ".mm",  ".cppm", ".ccm"};
    std::string e = to_lower(p.extension().string());
    if (std::find(ok.begin(), ok.end(), e) != ok.end()) return {};
    return std::unexpected(Error{
        .msg = "not a C/C++ source file: " + p.filename().string() +
               " (clangd handles .c/.cc/.cpp/.cxx/.h/.hpp/... only)",
        .code = 0});
}

// 1-based line `n` of `content`, without the trailing newline. "" if out of range.
std::string nth_line(std::string_view content, int n) {
    if (n < 1) return {};
    std::size_t start = 0;
    for (int i = 1; i < n; ++i) {
        std::size_t nl = content.find('\n', start);
        if (nl == std::string_view::npos) return {};
        start = nl + 1;
    }
    std::size_t end = content.find('\n', start);
    std::string line(content.substr(start, end == std::string_view::npos ? content.size() - start
                                                                         : end - start));
    if (!line.empty() && line.back() == '\r') line.pop_back();
    return line;
}

std::string trimmed(std::string_view s) { return std::string(trim_sv(s)); }

// `abs` made relative to `root`; falls back to the absolute path when it lies
// outside the root (so the caller can still see where the symbol is).
std::string rel_to_root(const fs::path& abs, const fs::path& root) {
    std::error_code ec;
    fs::path rel = fs::relative(abs, root, ec);
    if (ec || rel.empty()) return abs.generic_string();
    std::string s = rel.generic_string();
    if (s.rfind("..", 0) == 0) return abs.generic_string();
    return s;
}

int read_int(const nlohmann::json& range_pt, const char* key) {
    return range_pt.contains(key) && range_pt[key].is_number_integer()
               ? static_cast<int>(range_pt[key].get<std::int64_t>())
               : 0;
}

// --- result formatters ------------------------------------------------------

// Normalize a Location | Location[] | LocationLink[] into "rel:line:col: <src>".
std::string format_locations(const nlohmann::json& result, const fs::path& root,
                             lsp::PosEncoding enc) {
    struct Loc { fs::path abs; int line; int character; };
    std::vector<Loc> locs;
    auto add = [&](const nlohmann::json& loc) {
        std::string uri;
        const nlohmann::json* range = nullptr;
        if (loc.contains("uri") && loc.contains("range")) {
            uri = json::get_string_or(loc, "uri");
            range = &loc["range"];
        } else if (loc.contains("targetUri")) {
            uri = json::get_string_or(loc, "targetUri");
            range = loc.contains("targetSelectionRange") ? &loc["targetSelectionRange"]
                    : loc.contains("targetRange")        ? &loc["targetRange"]
                                                         : nullptr;
        }
        if (!range || !range->contains("start")) return;
        auto abs = lsp::uri_to_path(uri);
        if (!abs) return;
        locs.push_back({*abs, read_int((*range)["start"], "line"),
                        read_int((*range)["start"], "character")});
    };
    if (result.is_array())
        for (const auto& l : result) add(l);
    else if (result.is_object())
        add(result);

    std::sort(locs.begin(), locs.end(), [](const Loc& a, const Loc& b) {
        if (a.abs != b.abs) return a.abs < b.abs;
        if (a.line != b.line) return a.line < b.line;
        return a.character < b.character;
    });
    locs.erase(std::unique(locs.begin(), locs.end(),
                           [](const Loc& a, const Loc& b) {
                               return a.abs == b.abs && a.line == b.line &&
                                      a.character == b.character;
                           }),
               locs.end());

    std::map<fs::path, std::string> cache;
    std::string out;
    for (const auto& L : locs) {
        auto it = cache.find(L.abs);
        if (it == cache.end()) it = cache.emplace(L.abs, slurp(L.abs)).first;
        std::string line_text = nth_line(it->second, L.line + 1);
        std::size_t col = lsp::lsp_char_to_byte(line_text, L.character, enc);
        out += rel_to_root(L.abs, root) + ":" + std::to_string(L.line + 1) + ":" +
               std::to_string(col + 1) + ": " + trimmed(line_text) + "\n";
    }
    return out;
}

// DocumentSymbol (hierarchical) or SymbolInformation (flat) -> indented tree.
std::string format_symbol_tree(const nlohmann::json& result) {
    std::string out;
    std::function<void(const nlohmann::json&, int)> rec = [&](const nlohmann::json& node,
                                                              int depth) {
        if (!node.contains("name")) return;
        int kind = node.value("kind", 0);
        int line = 0;
        if (node.contains("selectionRange") && node["selectionRange"].contains("start"))
            line = read_int(node["selectionRange"]["start"], "line");
        else if (node.contains("range") && node["range"].contains("start"))
            line = read_int(node["range"]["start"], "line");
        else if (node.contains("location") && node["location"].contains("range") &&
                 node["location"]["range"].contains("start"))
            line = read_int(node["location"]["range"]["start"], "line");
        out += std::string(static_cast<std::size_t>(depth) * 2, ' ') +
               symbol_kind_name(kind) + " " + node.value("name", std::string()) + "  (L" +
               std::to_string(line + 1) + ")\n";
        if (node.contains("children") && node["children"].is_array())
            for (const auto& c : node["children"]) rec(c, depth + 1);
    };
    if (result.is_array())
        for (const auto& n : result) rec(n, 0);
    return out;
}

std::string format_workspace_symbols(const nlohmann::json& result, const fs::path& root) {
    if (!result.is_array()) return {};
    std::string out;
    int count = 0;
    for (const auto& e : result) {
        if (count++ >= 200) {
            out += "[... results truncated at 200 symbols; refine the query]\n";
            break;
        }
        if (!e.contains("location")) continue;
        std::string uri = e["location"].value("uri", std::string());
        auto abs = lsp::uri_to_path(uri);
        int line = e["location"].contains("range") &&
                           e["location"]["range"].contains("start")
                       ? read_int(e["location"]["range"]["start"], "line")
                       : 0;
        std::string name = e.value("name", std::string());
        std::string container = e.value("containerName", std::string());
        std::string full = container.empty() ? name : container + "::" + name;
        out += (abs ? rel_to_root(*abs, root) : uri) + ":" + std::to_string(line + 1) +
               ": " + symbol_kind_name(e.value("kind", 0)) + " " + full + "\n";
    }
    return out;
}

// CallHierarchy{Incoming,Outgoing}Call[] -> "rel:line: name", reading item under `key`.
std::string format_call_items(const nlohmann::json& result, const char* key,
                              const fs::path& root) {
    if (!result.is_array()) return {};
    std::string out;
    for (const auto& call : result) {
        if (!call.contains(key)) continue;
        const nlohmann::json& item = call[key];
        std::string uri = item.value("uri", std::string());
        auto abs = lsp::uri_to_path(uri);
        int line = 0;
        if (item.contains("selectionRange") && item["selectionRange"].contains("start"))
            line = read_int(item["selectionRange"]["start"], "line");
        else if (item.contains("range") && item["range"].contains("start"))
            line = read_int(item["range"]["start"], "line");
        out += (abs ? rel_to_root(*abs, root) : uri) + ":" + std::to_string(line + 1) +
               ": " + item.value("name", std::string()) + "\n";
    }
    return out;
}

std::string format_hover(const nlohmann::json& result) {
    if (!result.is_object() || !result.contains("contents")) return {};
    const auto& c = result["contents"];
    if (c.is_object() && c.contains("value")) return json::get_string_or(c, "value");
    if (c.is_string()) return c.get<std::string>();
    if (c.is_array()) {
        std::string out;
        for (const auto& part : c) {
            if (part.is_string()) out += part.get<std::string>() + "\n";
            else if (part.is_object() && part.contains("value"))
                out += json::get_string_or(part, "value") + "\n";
        }
        return out;
    }
    return {};
}

std::string index_note(const ClangdSession& s) {
    if (s.background_index_done()) return {};
    return "\n\n(note: clangd is still building its background index; cross-file "
           "results may be incomplete — retry shortly for full coverage)";
}

// --- shared argument handling ----------------------------------------------

struct Located { fs::path abs; lsp::Position pos; std::string symbol; };

// path + 1-based line + identifier text -> validated abs path and LSP position.
std::expected<Located, Error> resolve_pos(ClangdSession& s, const nlohmann::json& a) {
    auto path = json::get_string(a, "path");
    if (!path) return std::unexpected(path.error());
    auto line = json::get_int(a, "line");
    if (!line) return std::unexpected(line.error());
    if (*line < 1)
        return std::unexpected(Error{.msg = "line must be >= 1 (1-based)", .code = 0});
    auto symbol = json::get_string(a, "symbol");
    if (!symbol) return std::unexpected(symbol.error());
    int occurrence = 1;
    if (a.contains("occurrence") && a["occurrence"].is_number_integer())
        occurrence = static_cast<int>(a["occurrence"].get<std::int64_t>());

    auto abs = resolve_in_root(s.root(), *path);
    if (!abs) return std::unexpected(abs.error());
    if (auto r = require_cxx_source(*abs); !r) return std::unexpected(r.error());
    // Start clangd first so the negotiated position encoding is known before we
    // turn the identifier into an LSP character offset.
    if (auto st = s.ensure_started(); !st) return std::unexpected(st.error());

    std::string line_text = nth_line(slurp(*abs), static_cast<int>(*line));
    auto pos = lsp::position_of_symbol(line_text, static_cast<int>(*line), *symbol,
                                       occurrence, s.encoding());
    if (!pos) return std::unexpected(pos.error());
    return Located{.abs = *abs, .pos = *pos, .symbol = *symbol};
}

// JSON-schema fragment shared by every position-addressed tool.
const char* kPosProps =
    R"SC("path":{"type":"string","description":"file path, relative to project root"},
       "line":{"type":"integer","description":"1-based line with the symbol"},
       "symbol":{"type":"string","description":"exact identifier text to find on that line (e.g. from grep/read_file output)"},
       "occurrence":{"type":"integer","description":"which match if identifier repeats on line (1-based, default 1)"})SC";

nlohmann::json pos_schema(const std::string& extra_props = {}) {
    std::string props = std::string(kPosProps);
    if (!extra_props.empty()) props += "," + extra_props;
    return json::parse_or("{\"type\":\"object\",\"properties\":{" + props +
                          "},\"required\":[\"path\",\"line\",\"symbol\"]}");
}

// --- rename: apply a WorkspaceEdit to the working tree ----------------------

// Byte offset in `content` of LSP position (line, character) in `enc`.
std::size_t offset_of(std::string_view content, int line, int character, lsp::PosEncoding enc) {
    std::size_t start = 0;
    for (int i = 0; i < line; ++i) {
        std::size_t nl = content.find('\n', start);
        if (nl == std::string_view::npos) return content.size();
        start = nl + 1;
    }
    std::size_t nl = content.find('\n', start);
    std::string_view line_sv =
        content.substr(start, nl == std::string_view::npos ? content.size() - start : nl - start);
    return start + lsp::lsp_char_to_byte(line_sv, character, enc);
}

Result apply_workspace_edit(const nlohmann::json& edit, ClangdSession& s,
                            const ToolOptions& opts, const std::string& new_name) {
    struct FileEdits { fs::path abs; std::vector<nlohmann::json> edits; };
    std::vector<FileEdits> work;
    auto collect = [&](const std::string& uri, const nlohmann::json& edits) {
        auto abs = lsp::uri_to_path(uri);
        if (!abs || !edits.is_array()) return;
        work.push_back({*abs, edits.get<std::vector<nlohmann::json>>()});
    };
    if (edit.contains("changes") && edit["changes"].is_object())
        for (auto& [uri, edits] : edit["changes"].items()) collect(uri, edits);
    if (edit.contains("documentChanges") && edit["documentChanges"].is_array())
        for (const auto& dc : edit["documentChanges"])
            if (dc.contains("textDocument") && dc.contains("edits"))
                collect(dc["textDocument"].value("uri", std::string()), dc["edits"]);

    if (work.empty()) return std::string("rename produced no changes");

    int total = 0;
    std::vector<std::string> applied, skipped;
    for (auto& fe : work) {
        std::string rel = rel_to_root(fe.abs, s.root());
        // Confirm the target is inside the sandbox before writing.
        auto resolved = resolve_in_root(s.root(), rel);
        if (!resolved || rel.rfind("..", 0) == 0 || fs::path(rel).is_absolute()) {
            skipped.push_back(fe.abs.generic_string());
            continue;
        }
        std::string content = slurp(fe.abs), old = content;
        struct Span { std::size_t so, eo; std::string text; };
        std::vector<Span> spans;
        for (const auto& te : fe.edits) {
            if (!te.contains("range")) continue;
            const auto& r = te["range"];
            std::size_t so = offset_of(content, read_int(r["start"], "line"),
                                       read_int(r["start"], "character"), s.encoding());
            std::size_t eo = offset_of(content, read_int(r["end"], "line"),
                                       read_int(r["end"], "character"), s.encoding());
            spans.push_back({so, eo, te.value("newText", std::string())});
        }
        std::sort(spans.begin(), spans.end(),
                  [](const Span& a, const Span& b) { return a.so > b.so; });
        for (const auto& sp : spans) {
            if (sp.so > content.size() || sp.eo > content.size() || sp.so > sp.eo) continue;
            content.replace(sp.so, sp.eo - sp.so, sp.text);
        }
        std::ofstream f(fe.abs, std::ios::binary | std::ios::trunc);
        if (!f) {
            skipped.push_back(rel);
            continue;
        }
        f.write(content.data(), static_cast<std::streamsize>(content.size()));
        f.close();
        total += static_cast<int>(spans.size());
        applied.push_back(rel);
        if (opts.on_file_change)
            opts.on_file_change(FileChange{.path = rel, .old_content = std::move(old),
                                           .new_content = content});
    }

    std::string out = "renamed to '" + new_name + "' across " +
                      std::to_string(applied.size()) + " file(s), " +
                      std::to_string(total) + " edit(s):\n";
    for (const auto& f : applied) out += "  " + f + "\n";
    if (!skipped.empty()) {
        out += "skipped (outside project or unwritable):\n";
        for (const auto& f : skipped) out += "  " + f + "\n";
    }
    return out;
}

}  // namespace

// --- tool factories ---------------------------------------------------------

Tool find_references_tool(std::shared_ptr<ClangdSession> session) {
    ToolSpec spec{
        "find_references",
        "Find all references to C/C++ symbol via clangd. Give file `path`, "
        "1-based `line`, `symbol` identifier on that line. Returns one "
        "`path:line:col: <source>` per reference. Most accurate with "
        "compile_commands.json present.",
        pos_schema(
            R"SC("include_declaration":{"type":"boolean","description":"also include declaration (default true)"})SC")};
    return Tool{.spec = std::move(spec), .run = [session](const nlohmann::json& a) -> Result {
        std::lock_guard lk(session->io_mutex());  // serialise the shared clangd pipe
        auto loc = resolve_pos(*session, a);
        if (!loc) return std::unexpected(loc.error());
        bool incl = !a.contains("include_declaration") || !a["include_declaration"].is_boolean() ||
                    a["include_declaration"].get<bool>();
        auto res = session->references(loc->abs, loc->pos, incl);
        if (!res) return std::unexpected(res.error());
        std::string body = format_locations(*res, session->root(), session->encoding());
        if (body.empty()) return std::string("no references found for '" + loc->symbol + "'");
        return finalize("references to '" + loc->symbol + "':\n" + body + index_note(*session));
    }};
}

Tool go_to_definition_tool(std::shared_ptr<ClangdSession> session) {
    ToolSpec spec{"go_to_definition",
                  "Jump to where C/C++ symbol is defined via clangd. Args: `path`, "
                  "1-based `line`, `symbol`. Returns defining `path:line:col`.",
                  pos_schema()};
    return Tool{.spec = std::move(spec), .run = [session](const nlohmann::json& a) -> Result {
        std::lock_guard lk(session->io_mutex());  // serialise the shared clangd pipe
        auto loc = resolve_pos(*session, a);
        if (!loc) return std::unexpected(loc.error());
        auto res = session->definition(loc->abs, loc->pos);
        if (!res) return std::unexpected(res.error());
        std::string body = format_locations(*res, session->root(), session->encoding());
        if (body.empty()) return std::string("no definition found for '" + loc->symbol + "'");
        return finalize(body + index_note(*session));
    }};
}

Tool go_to_implementation_tool(std::shared_ptr<ClangdSession> session) {
    ToolSpec spec{"go_to_implementation",
                  "Find implementations/overrides of C/C++ symbol (e.g. virtual "
                  "method or abstract interface) via clangd. Args: `path`, 1-based "
                  "`line`, `symbol`. Returns implementing `path:line:col` locations.",
                  pos_schema()};
    return Tool{.spec = std::move(spec), .run = [session](const nlohmann::json& a) -> Result {
        std::lock_guard lk(session->io_mutex());  // serialise the shared clangd pipe
        auto loc = resolve_pos(*session, a);
        if (!loc) return std::unexpected(loc.error());
        auto res = session->implementation(loc->abs, loc->pos);
        if (!res) return std::unexpected(res.error());
        std::string body = format_locations(*res, session->root(), session->encoding());
        if (body.empty()) return std::string("no implementations found for '" + loc->symbol + "'");
        return finalize("implementations of '" + loc->symbol + "':\n" + body + index_note(*session));
    }};
}

Tool hover_tool(std::shared_ptr<ClangdSession> session) {
    ToolSpec spec{"hover",
                  "Show type, signature, docs of C/C++ symbol via "
                  "clangd (textDocument/hover). Args: `path`, 1-based `line`, `symbol`.",
                  pos_schema()};
    return Tool{.spec = std::move(spec), .run = [session](const nlohmann::json& a) -> Result {
        std::lock_guard lk(session->io_mutex());  // serialise the shared clangd pipe
        auto loc = resolve_pos(*session, a);
        if (!loc) return std::unexpected(loc.error());
        auto res = session->hover(loc->abs, loc->pos);
        if (!res) return std::unexpected(res.error());
        std::string body = format_hover(*res);
        if (body.empty()) return std::string("no hover information for '" + loc->symbol + "'");
        return finalize(body);
    }};
}

Tool list_symbols_tool(std::shared_ptr<ClangdSession> session) {
    ToolSpec spec{
        "list_symbols",
        "List C/C++ symbols via clangd. Pass `path` for symbols defined in one "
        "file (indented kind/name/line tree), OR `query` to search symbols across "
        "whole project. Provide exactly one.",
        json::parse_or(R"SC({
            "type":"object",
            "properties":{
              "path":{"type":"string","description":"file to list symbols of"},
              "query":{"type":"string","description":"project-wide symbol-name search (empty string lists top-level symbols)"}}})SC")};
    return Tool{.spec = std::move(spec), .run = [session](const nlohmann::json& a) -> Result {
        std::lock_guard lk(session->io_mutex());  // serialise the shared clangd pipe
        bool has_path = a.contains("path") && a["path"].is_string() &&
                        !a["path"].get<std::string>().empty();
        bool has_query = a.contains("query") && a["query"].is_string();
        if (has_path == has_query)
            return std::unexpected(Error{
                .msg = "provide exactly one of `path` (symbols in a file) or `query` "
                       "(project-wide search)",
                .code = 0});
        if (has_path) {
            std::string p = a["path"].get<std::string>();
            auto abs = resolve_in_root(session->root(), p);
            if (!abs) return std::unexpected(abs.error());
            if (auto r = require_cxx_source(*abs); !r) return std::unexpected(r.error());
            auto res = session->document_symbols(*abs);
            if (!res) return std::unexpected(res.error());
            std::string body = format_symbol_tree(*res);
            if (body.empty()) return std::string("no symbols found in " + p);
            return finalize(body);
        }
        std::string q = a["query"].get<std::string>();

        // workspace/symbol requires the background index; give it time to build.
        session->wait_for_index(std::chrono::seconds(10));

        auto res = session->workspace_symbols(q);
        if (!res) return std::unexpected(res.error());
        std::string body = format_workspace_symbols(*res, session->root());
        if (body.empty())
            return std::string("no symbols matching '" + q + "'" + index_note(*session));
        return finalize(body + index_note(*session));
    }};
}

Tool call_hierarchy_tool(std::shared_ptr<ClangdSession> session) {
    ToolSpec spec{
        "call_hierarchy",
        "Show callers or callees of C/C++ function via clangd. Args: `path`, 1-based "
        "`line`, `symbol`, `direction` ('callers' for who calls it, 'callees' for "
        "what it calls; default callers).",
        pos_schema(
            R"SC("direction":{"type":"string","enum":["callers","callees"],"description":"'callers' (incoming) or 'callees' (outgoing); default callers"})SC")};
    return Tool{.spec = std::move(spec), .run = [session](const nlohmann::json& a) -> Result {
        std::lock_guard lk(session->io_mutex());  // serialise the shared clangd pipe
        auto loc = resolve_pos(*session, a);
        if (!loc) return std::unexpected(loc.error());
        auto prep = session->prepare_call_hierarchy(loc->abs, loc->pos);
        if (!prep) return std::unexpected(prep.error());
        if (!prep->is_array() || prep->empty())
            return std::string("no call-hierarchy item at '" + loc->symbol +
                               "' (point at a function name)");
        const nlohmann::json& item = (*prep)[0];
        std::string name = item.value("name", loc->symbol);
        std::string dir = a.contains("direction") && a["direction"].is_string()
                              ? a["direction"].get<std::string>()
                              : "callers";
        if (dir == "callees" || dir == "outgoing") {
            auto res = session->outgoing_calls(item);
            if (!res) return std::unexpected(res.error());
            std::string body = format_call_items(*res, "to", session->root());
            if (body.empty()) return std::string("'" + name + "' has no outgoing calls");
            return finalize("callees of '" + name + "':\n" + body + index_note(*session));
        }
        auto res = session->incoming_calls(item);
        if (!res) return std::unexpected(res.error());
        std::string body = format_call_items(*res, "from", session->root());
        if (body.empty()) return std::string("no callers found for '" + name + "'");
        return finalize("callers of '" + name + "':\n" + body + index_note(*session));
    }};
}

Tool rename_tool(std::shared_ptr<ClangdSession> session, ToolOptions opts) {
    ToolSpec spec{
        "rename",
        "Rename C/C++ symbol project-wide via clangd, apply edits to "
        "working tree (shown as diffs). Args: `path`, 1-based `line`, `symbol`, "
        "`new_name`. Most reliable once clangd finished indexing; set "
        "CLANGD_INDEX_WAIT_MS to wait for index first.",
        pos_schema(
            R"SC("new_name":{"type":"string","description":"new identifier name"})SC")};
    // required becomes [path,line,symbol]; new_name is enforced below.
    return Tool{.spec = std::move(spec),
                .run = [session, opts](const nlohmann::json& a) -> Result {
        std::lock_guard lk(session->io_mutex());  // serialise the shared clangd pipe
        auto loc = resolve_pos(*session, a);
        if (!loc) return std::unexpected(loc.error());
        auto new_name = json::get_string(a, "new_name");
        if (!new_name) return std::unexpected(new_name.error());
        if (new_name->empty())
            return std::unexpected(Error{.msg = "new_name must not be empty", .code = 0});
        session->wait_for_index();  // bounded by CLANGD_INDEX_WAIT_MS (default: no wait)
        auto res = session->rename(loc->abs, loc->pos, *new_name);
        if (!res) return std::unexpected(res.error());
        if (res->is_null()) return std::string("rename produced no changes");
        auto applied = apply_workspace_edit(*res, *session, opts, *new_name);
        if (!applied) return std::unexpected(applied.error());
        return finalize(*applied + index_note(*session));
    }};
}

std::shared_ptr<lsp::ClangdSession> make_clangd_session(const ClangdToolsConfig& cfg) {
    lsp::LspConfig lc;
    lc.server_path = cfg.clangd_path;
    lc.root = cfg.root;
    lc.compile_commands_dir = cfg.compile_commands_dir;
    lc.request_timeout = std::chrono::seconds(cfg.request_timeout_secs);
    lc.index_wait = std::chrono::milliseconds(cfg.index_wait_ms);
    return std::make_shared<lsp::ClangdSession>(std::move(lc));
}

void register_clangd_tools(ToolRegistry& reg, std::shared_ptr<lsp::ClangdSession> session,
                           ToolOptions opts) {
    reg.add(find_references_tool(session));
    reg.add(go_to_definition_tool(session));
    reg.add(go_to_implementation_tool(session));
    reg.add(hover_tool(session));
    reg.add(list_symbols_tool(session));
    reg.add(call_hierarchy_tool(session));
    reg.add(rename_tool(session, std::move(opts)));
}

}  // namespace moocode
