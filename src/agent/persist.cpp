#include "agent/persist.hpp"

#include <algorithm>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <string_view>

#include "agent/fsutil.hpp"
#include "agent/platform.hpp"  // user_home
#include "agent/strutil.hpp"

// The project bans exceptions, so toml++ must use its error-returning API. This
// is the ONE translation unit that includes toml.hpp; keep it that way.
#define TOML_EXCEPTIONS 0
#include "toml.hpp"

namespace fs = std::filesystem;

namespace moocode {

namespace {

// Legacy heuristic retained for back-compat: conversations written before
// `tool_failed` was a typed field had no typed bit, so we infer failure from
// the "ERROR: " prefix. Not for new callers.
inline bool is_tool_error(std::string_view s) { return s.rfind("ERROR: ", 0) == 0; }

// Build a Message field-by-field from deserialized TOML data. The public API
// forces construction through role-tagged factories, but the persist loader
// needs to build Messages from arbitrary loaded fields. This free function
// lives here, not in types.hpp, because it is a persistence implementation
// detail — no other code should bypass the role-tagged constructors.
Message message_from_fields(Role role, std::string content,
                            std::vector<ToolCall> tool_calls,
                            std::string tool_call_id,
                            std::vector<ContentPart> parts,
                            bool tool_failed, std::string reasoning) {
    switch (role) {
    case Role::System:
        return Message::system(std::move(content));
    case Role::User:
        return Message::user(std::move(content), std::move(parts));
    case Role::Assistant:
        return Message::assistant(std::move(content), std::move(tool_calls),
                                  std::move(reasoning));
    case Role::Tool:
        return Message::tool(std::move(tool_call_id), std::move(content),
                             tool_failed);
    }
    return Message::user(std::move(content));
}

// Map a role string to the enum; unknown roles default to User (defensive).
Role role_from_string(std::string_view s) {
    if (s == "system") return Role::System;
    if (s == "assistant") return Role::Assistant;
    if (s == "tool") return Role::Tool;
    return Role::User;
}

// Format `t` as ISO-8601 UTC "yyyy-mm-ddThh:mm:ssZ". Empty on clock failure.
std::string iso_utc(std::time_t t) {
    std::tm tm{};
#ifdef _WIN32
    if (::gmtime_s(&tm, &t) != 0) return {};
#else
    if (!::gmtime_r(&t, &tm)) return {};
#endif
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buf;
}

}  // namespace

std::optional<std::string> toml_check_syntax(std::string_view text) {
    toml::parse_result res = toml::parse(text);
    if (!res) return std::string(res.error().description());
    return std::nullopt;
}

std::string moocode_home() {
    std::string fh = env_or("MOOCODE_HOME", "");
    if (!fh.empty()) return fh;
    std::string home = user_home();
    if (!home.empty()) return home + "/.moo";  // forward slash works on Win32 too
    return {};  // no home => persistence disabled
}

const std::vector<Profile>& builtin_profiles() {
    static const std::vector<Profile> p = {
        {"minimax", "openai", "https://api.minimax.io/v1", "MiniMax-M3",
         {"MiniMax-M3", "MiniMax-M1"}, -1, false, "adaptive"},
        {"deepseek", "openai", "https://api.deepseek.com/v1", "deepseek-v4-pro",
         {"deepseek-v4-pro", "deepseek-v4-flash"}},
        {"anthropic", "anthropic", "https://api.anthropic.com/v1",
         "claude-sonnet-4-6",
         {"claude-sonnet-4-6", "claude-sonnet-4-5", "claude-opus-4-5",
          "claude-opus-4", "claude-3-5-sonnet", "claude-3-5-haiku"}},
        {"openai", "openai", "https://api.openai.com/v1", "gpt-5",
         {"gpt-5", "gpt-5-mini", "gpt-5-nano", "gpt-4o", "o4", "o3", "o1"}},
        {"qwen", "openai", "https://dashscope.aliyuncs.com/compatible-mode/v1",
         "qwen-3", {"qwen-3", "qwen-3-coder"}},
        {"glm", "openai", "https://open.bigmodel.cn/api/paas/v4", "glm-4",
         {"glm-4"}},
        {"gemini", "gemini",
         "https://generativelanguage.googleapis.com/v1beta",
         "gemini-3.5-pro", {"gemini-3.5-pro", "gemini-3.5-flash"}},
        {"gemini-openai", "openai",
         "https://generativelanguage.googleapis.com/v1beta/openai",
         "gemini-3.5-pro", {"gemini-3.5-pro", "gemini-3.5-flash"}},
        {"grok", "openai", "https://api.x.ai/v1", "grok-4", {"grok-4"}},
    };
    return p;
}

// --- settings ---------------------------------------------------------------

Settings load_settings(const std::string& home) {
    Settings s;
    if (home.empty()) return s;
    std::string body = slurp(home + "/settings.toml");
    if (body.empty()) return s;
    toml::parse_result res = toml::parse(body);
    if (!res) return s;  // malformed => defaults, no error
    const toml::table& t = res.table();
    if (auto v = t["base_url"].value<std::string>()) s.base_url = *v;
    if (auto v = t["model"].value<std::string>()) s.model = *v;
    if (auto v = t["provider"].value<std::string>()) s.provider = *v;
    if (auto v = t["context_window"].value<int64_t>()) s.context_window = static_cast<int>(*v);
    if (auto v = t["max_iterations"].value<int64_t>()) s.max_iterations = static_cast<int>(*v);
    if (auto v = t["max_tokens"].value<int64_t>()) s.max_tokens = static_cast<int>(*v);
    if (auto v = t["effort"].value<std::string>()) s.effort = *v;
    if (auto v = t["temperature"].value<double>()) s.temperature = *v;
    if (auto v = t["thinking"].value<bool>()) s.thinking = *v ? 1 : 0;
    if (auto v = t["rtk"].value<bool>()) s.rtk = *v ? 1 : 0;
    if (auto v = t["theme"].value<std::string>()) s.theme = *v;
    if (auto v = t["profile"].value<std::string>()) s.profile = *v;

    // [profiles.<name>] sub-tables: one Profile each. Sorted by name on emit so
    // the file order is deterministic regardless of toml++'s internal ordering.
    if (const toml::table* profs = t["profiles"].as_table()) {
        for (const auto& [key, node] : *profs) {
            const toml::table* pt = node.as_table();
            if (!pt) continue;
            Profile p;
            p.name = std::string(key.str());
            if (auto v = (*pt)["kind"].value<std::string>()) p.kind = *v;
            if (auto v = (*pt)["base_url"].value<std::string>()) p.base_url = *v;
            if (auto v = (*pt)["model"].value<std::string>()) p.model = *v;
            if (auto v = (*pt)["thinking"].value<bool>()) p.thinking = *v ? 1 : 0;
            if (auto v = (*pt)["drop_thinking_tag"].value<bool>()) p.drop_thinking_tag = *v;
            if (auto v = (*pt)["thinking_type"].value<std::string>()) p.thinking_type = *v;
            if (const toml::array* arr = (*pt)["models"].as_array())
                for (const toml::node& n : *arr)
                    if (auto v = n.value<std::string>()) p.models.push_back(*v);
            s.profiles.push_back(std::move(p));
        }
        std::ranges::sort(s.profiles, [](const Profile& a, const Profile& b) {
            return a.name < b.name;
        });
    }
    return s;
}

void save_settings(const std::string& home, const Settings& s) {
    if (home.empty()) return;
    std::error_code ec;
    fs::create_directories(home, ec);
    toml::table t;
    if (!s.base_url.empty()) t.insert("base_url", s.base_url);
    if (!s.model.empty()) t.insert("model", s.model);
    if (!s.provider.empty()) t.insert("provider", s.provider);
    if (s.context_window > 0) t.insert("context_window", int64_t(s.context_window));
    if (s.max_iterations > 0) t.insert("max_iterations", int64_t(s.max_iterations));
    if (s.max_tokens > 0) t.insert("max_tokens", int64_t(s.max_tokens));
    if (!s.effort.empty()) t.insert("effort", s.effort);
    if (s.temperature >= 0) t.insert("temperature", s.temperature);
    if (s.thinking >= 0) t.insert("thinking", s.thinking != 0);
    if (s.rtk >= 0) t.insert("rtk", s.rtk != 0);
    if (!s.theme.empty()) t.insert("theme", s.theme);
    if (!s.profile.empty()) t.insert("profile", s.profile);

    // [profiles.*] tables, sorted by name for a deterministic file. api_key is
    // never written here — secrets live only in credentials.toml.
    if (!s.profiles.empty()) {
        std::vector<const Profile*> sorted;
        sorted.reserve(s.profiles.size());
        for (const Profile& p : s.profiles) sorted.push_back(&p);
        std::ranges::sort(sorted, [](const Profile* a, const Profile* b) {
            return a->name < b->name;
        });
        toml::table profs;
        for (const Profile* p : sorted) {
            toml::table pt;
            if (!p->kind.empty()) pt.insert("kind", p->kind);
            if (!p->base_url.empty()) pt.insert("base_url", p->base_url);
            if (!p->model.empty()) pt.insert("model", p->model);
            if (p->thinking >= 0) pt.insert("thinking", p->thinking != 0);
            if (p->drop_thinking_tag) pt.insert("drop_thinking_tag", true);
            if (!p->thinking_type.empty() && p->thinking_type != "enabled")
                pt.insert("thinking_type", p->thinking_type);
            if (!p->models.empty()) {
                toml::array arr;
                for (const std::string& m : p->models) arr.push_back(m);
                pt.insert("models", std::move(arr));
            }
            profs.insert(p->name, std::move(pt));
        }
        t.insert("profiles", std::move(profs));
    }
    std::ofstream out(home + "/settings.toml", std::ios::binary | std::ios::trunc);
    if (out) out << t << '\n';
}

// --- credentials ------------------------------------------------------------

std::map<std::string, std::string> load_credentials(const std::string& home) {
    std::map<std::string, std::string> out;
    if (home.empty()) return out;
    std::string body = slurp(home + "/credentials.toml");
    if (body.empty()) return out;
    toml::parse_result res = toml::parse(body);
    if (!res) return out;  // malformed => empty, no error
    for (const auto& [key, node] : res.table())
        if (auto v = node.value<std::string>()) out.emplace(std::string(key.str()), *v);
    return out;
}

void save_credential(const std::string& home, const std::string& name,
                     const std::string& key) {
    if (home.empty() || name.empty()) return;
    std::error_code ec;
    fs::create_directories(home, ec);

    // Merge into the existing map so other profiles' keys are not clobbered.
    std::map<std::string, std::string> creds = load_credentials(home);
    creds[name] = key;

    toml::table t;
    for (const auto& [n, k] : creds) t.insert(n, k);  // std::map => sorted/deterministic
    std::string path = home + "/credentials.toml";
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        if (!out) return;
        out << t << '\n';
    }
    // Best-effort 0600: secrets must not be world/group readable. Ignore errors
    // on platforms that don't support POSIX permission bits.
    fs::permissions(path, fs::perms::owner_read | fs::perms::owner_write,
                    fs::perm_options::replace, ec);
}

// --- permissions ------------------------------------------------------------

std::set<std::string> load_permissions(const std::string& home) {
    std::set<std::string> out;
    if (home.empty()) return out;
    std::string body = slurp(home + "/permissions.toml");
    if (body.empty()) return out;
    toml::parse_result res = toml::parse(body);
    if (!res) return out;
    if (const toml::array* arr = res.table()["always"].as_array()) {
        for (const toml::node& n : *arr)
            if (auto v = n.value<std::string>()) out.insert(*v);
    }
    return out;
}

void save_permissions(const std::string& home, const std::set<std::string>& always) {
    if (home.empty()) return;
    std::error_code ec;
    fs::create_directories(home, ec);
    toml::array arr;
    for (const auto& name : always) arr.push_back(name);  // set => sorted/deterministic
    toml::table t;
    t.insert("always", std::move(arr));
    std::ofstream out(home + "/permissions.toml", std::ios::binary | std::ios::trunc);
    if (out) out << t << '\n';
}

// --- conversations ----------------------------------------------------------

std::string conversations_dir(const std::string& home) {
    if (home.empty()) return {};
    return home + "/conversations";
}

std::string new_conversation_id(const std::string& cwd) {
    std::string ts = iso_utc(std::time(nullptr));
    // "yyyy-mm-ddThh:mm:ssZ" -> "yyyymmdd-hhmmss": drop separators, keep order.
    std::string compact;
    compact.reserve(15);
    for (std::size_t i = 0; i < 10 && i < ts.size(); ++i)  // date portion
        if (ts[i] != '-') compact += ts[i];
    compact += '-';
    for (std::size_t i = 11; i < 19 && i < ts.size(); ++i)  // time portion
        if (ts[i] != ':') compact += ts[i];

    std::size_t h = std::hash<std::string>{}(cwd);
    char hex[9];
    std::snprintf(hex, sizeof hex, "%08x", static_cast<unsigned>(h & 0xffffffffu));
    return compact + "-" + hex;
}

std::expected<void, Error> save_conversation(const std::string& path,
                                             const Conversation& conv,
                                             const ConvMeta& meta) {
    if (path.empty())
        return std::unexpected(Error{.msg = "no conversation path (persistence disabled)", .code = 0});
    std::error_code ec;
    fs::create_directories(fs::path(path).parent_path(), ec);

    toml::table meta_tbl;
    if (!meta.cwd.empty()) meta_tbl.insert("cwd", meta.cwd);
    if (!meta.model.empty()) meta_tbl.insert("model", meta.model);
    if (!meta.created.empty()) meta_tbl.insert("created", meta.created);
    if (!meta.updated.empty()) meta_tbl.insert("updated", meta.updated);
    if (!meta.title.empty()) meta_tbl.insert("title", meta.title);

    toml::array messages;
    for (const Message& m : conv) {
        toml::table msg;
        msg.insert("role", role_str(m.role()));
        msg.insert("content", m.content());
        if (m.role() == Role::Assistant && !m.tool_calls().empty()) {
            toml::array calls;
            for (const ToolCall& tc : m.tool_calls()) {
                toml::table c;
                c.insert("id", tc.id);
                c.insert("name", tc.name);
                c.insert("arguments", tc.arguments_json);
                if (!tc.signature.empty()) c.insert("signature", tc.signature);
                calls.push_back(std::move(c));
            }
            msg.insert("tool_call", std::move(calls));
        }
        if (m.role() == Role::Assistant && !m.reasoning().empty())
            msg.insert("reasoning", m.reasoning());
        if (m.role() == Role::Tool && !m.tool_call_id().empty())
            msg.insert("tool_call_id", m.tool_call_id());
        // Always persist the typed bit for Tool messages (true AND false): its
        // presence disables the legacy "ERROR: "-prefix inference on load, so a
        // successful tool whose output happens to start with "ERROR: " is not
        // misclassified on /resume. Files written before this carry no key and
        // fall back to the heuristic (see messages_from_table).
        if (m.role() == Role::Tool)
            msg.insert("tool_failed", m.tool_failed());
        messages.push_back(std::move(msg));
    }

    toml::table doc;
    doc.insert("meta", std::move(meta_tbl));
    doc.insert("message", std::move(messages));

    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return std::unexpected(Error{.msg = "cannot open for write: " + path, .code = 0});
    out << doc << '\n';
    if (!out) return std::unexpected(Error{.msg = "write failed: " + path, .code = 0});
    return {};
}

namespace {

// Parse the [[message]] array from an already-loaded document table.
Conversation messages_from_table(const toml::table& doc) {
    Conversation conv;
    const toml::array* msgs = doc["message"].as_array();
    if (!msgs) return conv;
    for (const toml::node& node : *msgs) {
        const toml::table* mt = node.as_table();
        if (!mt) continue;
        Role role = Role::User;
        std::string content;
        std::string tool_call_id;
        std::vector<ToolCall> tool_calls;
        bool tool_failed = false;
        std::string reasoning;
        if (auto r = (*mt)["role"].value<std::string>()) role = role_from_string(*r);
        if (auto c = (*mt)["content"].value<std::string>()) content = *c;
        if (auto r = (*mt)["reasoning"].value<std::string>()) reasoning = *r;
        if (auto id = (*mt)["tool_call_id"].value<std::string>()) tool_call_id = *id;
        // Failure is a typed bit in current files. For pre-migration files that
        // lack the key, infer it from the legacy "ERROR: " content prefix so old
        // conversations keep their Failed display on /resume.
        if (auto f = (*mt)["tool_failed"].value<bool>())
            tool_failed = *f;
        else if (role == Role::Tool)
            tool_failed = is_tool_error(content);
        if (const toml::array* calls = (*mt)["tool_call"].as_array()) {
            for (const toml::node& cnode : *calls) {
                const toml::table* ct = cnode.as_table();
                if (!ct) continue;
                ToolCall tc;
                if (auto v = (*ct)["id"].value<std::string>()) tc.id = *v;
                if (auto v = (*ct)["name"].value<std::string>()) tc.name = *v;
                if (auto v = (*ct)["arguments"].value<std::string>()) tc.arguments_json = *v;
                if (auto v = (*ct)["signature"].value<std::string>()) tc.signature = *v;
                tool_calls.push_back(std::move(tc));
            }
        }
        conv.push_back(message_from_fields(role, std::move(content),
                                           std::move(tool_calls),
                                           std::move(tool_call_id), {},
                                           tool_failed, std::move(reasoning)));
    }
    return conv;
}

ConvMeta meta_from_table(const toml::table& doc) {
    ConvMeta meta;
    const toml::table* mt = doc["meta"].as_table();
    if (!mt) return meta;
    if (auto v = (*mt)["cwd"].value<std::string>()) meta.cwd = *v;
    if (auto v = (*mt)["model"].value<std::string>()) meta.model = *v;
    if (auto v = (*mt)["created"].value<std::string>()) meta.created = *v;
    if (auto v = (*mt)["updated"].value<std::string>()) meta.updated = *v;
    if (auto v = (*mt)["title"].value<std::string>()) meta.title = *v;
    return meta;
}

}  // namespace

std::expected<std::pair<Conversation, ConvMeta>, Error> load_conversation_with_meta(
    const std::string& path) {
    std::error_code ec;
    if (!fs::is_regular_file(path, ec))
        return std::unexpected(Error{.msg = "no such conversation: " + path, .code = 0});
    std::string body = slurp(path);
    toml::parse_result res = toml::parse(body);
    if (!res)
        return std::unexpected(
            Error{.msg = "malformed conversation " + path + ": " +
                      std::string(res.error().description()),
                .code = 0});
    const toml::table& doc = res.table();
    return std::pair{messages_from_table(doc), meta_from_table(doc)};
}

std::expected<Conversation, Error> load_conversation(const std::string& path) {
    auto r = load_conversation_with_meta(path);
    if (!r) return std::unexpected(r.error());
    return std::move(r->first);
}

std::vector<ConvSummary> list_conversations(const std::string& dir,
                                            const std::string& cwd) {
    std::vector<ConvSummary> out;
    if (dir.empty()) return out;
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return out;

    for (const auto& e : fs::directory_iterator(dir, ec)) {
        if (ec) break;
        if (!e.is_regular_file(ec)) continue;
        if (e.path().extension() != ".toml") continue;
        std::string path = e.path().string();
        std::string body = slurp(path);
        toml::parse_result res = toml::parse(body);
        if (!res) continue;  // skip garbage rather than fail the whole listing
        const toml::table& doc = res.table();
        ConvMeta meta = meta_from_table(doc);
        if (!cwd.empty() && meta.cwd != cwd) continue;
        ConvSummary s;
        s.path = path;
        s.title = meta.title;
        s.updated = meta.updated;
        if (const toml::array* msgs = doc["message"].as_array()) s.count = msgs->size();
        out.push_back(std::move(s));
    }

    // Most-recent-first; ISO-8601 strings sort lexically == chronologically.
    std::ranges::sort(out, [](const ConvSummary& a, const ConvSummary& b) {
        if (a.updated != b.updated) return a.updated > b.updated;
        return a.path > b.path;
    });
    return out;
}

}  // namespace moocode
