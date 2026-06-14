#include "agent/gitea.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>

#include "agent/gitea_internal.hpp"
#include "agent/http_detail.hpp"
#include <cstdint>
#include <cstring>
#include <format>
#include <limits>
#include <regex>
#include <string>
#include <utility>
#include <vector>

#include "agent/json_util.hpp"
#include "agent/strutil.hpp"

namespace moocode {

namespace {

using njson = nlohmann::json;

// --- tolerant JSON field access (formatters must never throw) ----------------

std::int64_t nfield(const njson& j, const char* key) {
    auto it = j.find(key);
    return it != j.end() && it->is_number() ? it->get<std::int64_t>() : 0;
}

bool bfield(const njson& j, const char* key) {
    auto it = j.find(key);
    return it != j.end() && it->is_boolean() && it->get<bool>();
}

const njson* ofield(const njson& j, const char* key) {
    auto it = j.find(key);
    return it != j.end() && it->is_object() ? &*it : nullptr;
}

// "YYYY-MM-DDTHH:MM:SSZ" -> "YYYY-MM-DD"; shorter strings pass through.
std::string date_only(std::string ts) {
    if (ts.size() > 10) ts.resize(10);
    return ts;
}

std::string short_sha(std::string sha) {
    if (sha.size() > 10) sha.resize(10);
    return sha;
}

// user object -> login, falling back to a plain name field.
std::string login_of(const njson& j, const char* key) {
    if (const nlohmann::json* u = ofield(j, key)) {
        std::string login = json::get_string_or(*u, "login");
        if (!login.empty()) return login;
        return json::get_string_or(*u, "name");
    }
    return {};
}
// Encode a repo-relative file path, keeping '/' separators.
std::string enc_path(std::string_view path) {
    std::string out;
    std::size_t start = 0;
    while (start <= path.size()) {
        const std::size_t slash = path.find('/', start);
        const auto seg = path.substr(start, slash == std::string_view::npos
                                                ? std::string_view::npos
                                                : slash - start);
        out += http::url_encode(seg);
        if (slash == std::string_view::npos) break;
        out += '/';
        start = slash + 1;
    }
    return out;
}

std::string repo_path(std::string_view owner, std::string_view repo) {
    return "/repos/" + http::url_encode(owner) + "/" + http::url_encode(repo);
}

// Strip one layer of matching single or double quotes.
std::string_view unquote(std::string_view s) {
    if (s.size() >= 2 && (s.front() == '"' || s.front() == '\'') &&
        s.back() == s.front())
        return s.substr(1, s.size() - 2);
    return s;
}

}  // namespace

// --- pure helpers -------------------------------------------------------------

std::string gitea_api_base(std::string_view base_url) {
    std::string base(base_url);
    while (base.size() > 1 && base.back() == '/') base.pop_back();
    if (base.ends_with("/api/v1")) return base;
    return base + "/api/v1";
}

std::string gitea_error_message(long status, std::string_view body) {
    std::string out = "gitea: HTTP " + std::to_string(status);
    if (auto parsed = nlohmann::json::parse(body, /*cb=*/nullptr, /*allow_exceptions=*/false);
        parsed.is_object()) {
        const std::string msg = json::get_string_or(parsed, "message");
        if (!msg.empty()) out += ": " + one_line(msg, 300);
    }
    return out;
}

std::string gitea_origin(std::string_view url) {
    const std::size_t scheme_end = url.find("://");
    if (scheme_end == std::string_view::npos) return {};
    std::size_t host_end = scheme_end + 3;
    while (host_end < url.size() && url[host_end] != '/' && url[host_end] != '?' &&
           url[host_end] != '#')
        ++host_end;
    return to_lower(url.substr(0, host_end));
}

GiteaBasicAuth parse_gitea_auth(std::string_view content) {
    GiteaBasicAuth auth;
    std::size_t start = 0;
    while (start <= content.size()) {
        const std::size_t nl = content.find('\n', start);
        std::string_view line = content.substr(
            start, nl == std::string_view::npos ? std::string_view::npos : nl - start);
        line = trim_sv(line);
        if (!line.empty() && line.front() != '#') {
            if (const std::size_t eq = line.find('='); eq != std::string_view::npos) {
                const std::string_view key = trim_sv(line.substr(0, eq));
                const std::string_view val = unquote(trim_sv(line.substr(eq + 1)));
                if (key == "GITEA_USER") auth.user = std::string(val);
                else if (key == "GITEA_PASS") auth.pass = std::string(val);
            }
        }
        if (nl == std::string_view::npos) break;
        start = nl + 1;
    }
    return auth;
}

std::string gitea_basic_auth_header(std::string_view user, std::string_view pass) {
    std::string creds;
    creds.reserve(user.size() + 1 + pass.size());
    creds.append(user);
    creds += ':';
    creds.append(pass);
    return "Authorization: Basic " + base64_encode(creds);
}

std::expected<std::chrono::seconds, Error> parse_timeframe(std::string_view tf) {
    tf = trim_sv(tf);
    const auto bad = [] {
        return std::unexpected(Error{
            .msg = "timeframe must be <number><unit> with unit m/h/d/w, "
                   "e.g. \"90m\", \"36h\", \"7d\", \"2w\"",
            .code = 0});
    };
    if (tf.size() < 2) return bad();
    std::int64_t n = 0;
    const auto [ptr, ec] = std::from_chars(tf.data(), tf.data() + tf.size() - 1, n);
    if (ec != std::errc{} || ptr != tf.data() + tf.size() - 1) return bad();
    if (n <= 0 || n > 1'000'000) return bad();  // bound keeps the multiply safe
    switch (tf.back()) {
        case 'm': return std::chrono::seconds{n * 60};
        case 'h': return std::chrono::seconds{n * 3600};
        case 'd': return std::chrono::seconds{n * 86400};
        case 'w': return std::chrono::seconds{n * 7 * 86400};
        default: return bad();
    }
}

std::optional<std::int64_t> parse_iso8601(std::string_view ts) {
    // YYYY-MM-DD[[T ]HH:MM:SS[.frac]][Z|+HH[:]MM|-HH[:]MM]
    const auto num = [ts](std::size_t pos, std::size_t len, int& out) {
        if (pos + len > ts.size()) return false;
        out = 0;
        for (std::size_t i = pos; i < pos + len; ++i) {
            if (ts[i] < '0' || ts[i] > '9') return false;
            out = out * 10 + (ts[i] - '0');
        }
        return true;
    };
    int y = 0, mo = 0, d = 0, h = 0, mi = 0, s = 0;
    if (!num(0, 4, y) || ts.size() < 10 || ts[4] != '-' || !num(5, 2, mo) ||
        ts[7] != '-' || !num(8, 2, d))
        return std::nullopt;
    const std::chrono::year_month_day ymd{std::chrono::year{y},
                                          std::chrono::month{static_cast<unsigned>(mo)},
                                          std::chrono::day{static_cast<unsigned>(d)}};
    if (!ymd.ok()) return std::nullopt;
    std::size_t pos = 10;
    if (pos < ts.size() && (ts[pos] == 'T' || ts[pos] == ' ')) {
        if (!num(pos + 1, 2, h) || pos + 3 >= ts.size() || ts[pos + 3] != ':' ||
            !num(pos + 4, 2, mi) || pos + 6 >= ts.size() || ts[pos + 6] != ':' ||
            !num(pos + 7, 2, s))
            return std::nullopt;
        pos += 9;
        if (pos < ts.size() && ts[pos] == '.') {
            ++pos;
            while (pos < ts.size() && ts[pos] >= '0' && ts[pos] <= '9') ++pos;
        }
    }
    if (h > 23 || mi > 59 || s > 60) return std::nullopt;  // 60: leap second
    std::int64_t offset = 0;
    if (pos < ts.size()) {
        if (ts[pos] == 'Z') {
            if (pos + 1 != ts.size()) return std::nullopt;
        } else if (ts[pos] == '+' || ts[pos] == '-') {
            int oh = 0, om = 0;
            if (!num(pos + 1, 2, oh)) return std::nullopt;
            std::size_t mpos = pos + 3;
            if (mpos < ts.size() && ts[mpos] == ':') ++mpos;
            if (!num(mpos, 2, om) || mpos + 2 != ts.size() || oh > 23 || om > 59)
                return std::nullopt;
            offset = (oh * 3600LL + om * 60) * (ts[pos] == '+' ? 1 : -1);
        } else {
            return std::nullopt;
        }
    }
    const std::chrono::sys_days days{ymd};
    return days.time_since_epoch().count() * 86400LL + h * 3600LL + mi * 60 + s -
           offset;
}

std::string iso8601_utc(std::int64_t epoch) {
    const std::chrono::sys_seconds tp{std::chrono::seconds{epoch}};
    return std::format("{:%FT%TZ}", tp);
}

std::expected<nlohmann::json, Error> regex_filter(const nlohmann::json& items,
                                                  const char* field,
                                                  const std::string& pattern) {
    // std::regex reports bad patterns (and pathological inputs) only by
    // throwing; confine that here and surface a plain Error.
    try {
        const std::regex re(pattern, std::regex::ECMAScript | std::regex::icase);
        nlohmann::json out = nlohmann::json::array();
        if (!items.is_array()) return out;
        for (const nlohmann::json& it : items)
            if (std::regex_search(json::get_string_or(it, field), re))
                out.push_back(it);
        return out;
    } catch (const std::regex_error& e) {
        return std::unexpected(Error{
            .msg = std::string("invalid regex: ") + e.what(), .code = 0});
    }
}

namespace {

// Committer (fallback: author) date of a commit as epoch seconds.
std::optional<std::int64_t> commit_epoch(const njson& c) {
    if (const njson* meta = ofield(c, "commit")) {
        for (const char* who : {"committer", "author"}) {
            if (const njson* u = ofield(*meta, who)) {
                const std::string date = json::get_string_or(*u, "date");
                if (!date.empty()) return parse_iso8601(date);
            }
        }
    }
    return std::nullopt;
}

}  // namespace

nlohmann::json filter_commits_since(const nlohmann::json& commits,
                                    std::int64_t since_epoch) {
    nlohmann::json out = nlohmann::json::array();
    if (!commits.is_array()) return out;
    for (const nlohmann::json& c : commits) {
        const std::optional<std::int64_t> epoch = commit_epoch(c);
        if (!epoch || *epoch >= since_epoch) out.push_back(c);
    }
    return out;
}

std::string format_repo_list(const nlohmann::json& repos) {
    if (!repos.is_array() || repos.empty()) return "No repositories found.";
    std::string out;
    int n = 0;
    for (const nlohmann::json& r : repos) {
        out += std::to_string(++n) + ". " + json::get_string_or(r, "full_name");
        out += "  *" + std::to_string(nfield(r, "stars_count"));
        out += " forks:" + std::to_string(nfield(r, "forks_count"));
        if (bfield(r, "private")) out += " [private]";
        if (bfield(r, "archived")) out += " [archived]";
        if (bfield(r, "fork")) out += " [fork]";
        if (bfield(r, "mirror")) out += " [mirror]";
        const std::string desc = json::get_string_or(r, "description");
        if (!desc.empty()) out += " — " + one_line(desc, 120);
        out += '\n';
    }
    return out;
}

std::string format_repo(const nlohmann::json& repo, const nlohmann::json& branches) {
    std::string out = "repo: " + json::get_string_or(repo, "full_name") + "\n";
    const std::string desc = json::get_string_or(repo, "description");
    if (!desc.empty()) out += "description: " + desc + "\n";
    out += "default branch: " + json::get_string_or(repo, "default_branch") + "\n";
    out += std::string("visibility: ") + (bfield(repo, "private") ? "private" : "public");
    if (bfield(repo, "fork")) out += ", fork";
    if (bfield(repo, "mirror")) out += ", mirror";
    if (bfield(repo, "archived")) out += ", archived";
    if (bfield(repo, "template")) out += ", template";
    out += '\n';
    out += "stars: " + std::to_string(nfield(repo, "stars_count"));
    out += "  forks: " + std::to_string(nfield(repo, "forks_count"));
    out += "  watchers: " + std::to_string(nfield(repo, "watchers_count"));
    out += "  open issues: " + std::to_string(nfield(repo, "open_issues_count"));
    out += "  open PRs: " + std::to_string(nfield(repo, "open_pr_counter"));
    out += '\n';
    out += "size: " + std::to_string(nfield(repo, "size")) + " KiB\n";
    out += "created: " + date_only(json::get_string_or(repo, "created_at"));
    out += "  updated: " + date_only(json::get_string_or(repo, "updated_at")) + "\n";
    const std::string clone = json::get_string_or(repo, "clone_url");
    if (!clone.empty()) out += "clone: " + clone + "\n";
    const std::string html = json::get_string_or(repo, "html_url");
    if (!html.empty()) out += "url: " + html + "\n";
    if (branches.is_array() && !branches.empty()) {
        out += "branches (" + std::to_string(branches.size()) + "):\n";
        for (const nlohmann::json& b : branches) {
            out += "  " + json::get_string_or(b, "name");
            if (const nlohmann::json* c = ofield(b, "commit")) {
                const std::string id = short_sha(json::get_string_or(*c, "id"));
                if (!id.empty()) out += "  " + id;
            }
            if (bfield(b, "protected")) out += "  [protected]";
            out += '\n';
        }
    }
    return out;
}

std::string format_pr_list(const nlohmann::json& prs) {
    if (!prs.is_array() || prs.empty()) return "No pull requests found.";
    std::string out;
    int n = 0;
    for (const nlohmann::json& pr : prs) {
        out += std::to_string(++n) + ". #" + std::to_string(nfield(pr, "number"));
        out += " [" + json::get_string_or(pr, "state");
        if (bfield(pr, "merged")) out += ", merged";
        if (bfield(pr, "draft")) out += ", draft";
        out += "] " + one_line(json::get_string_or(pr, "title"), 100);
        out += " (" + login_of(pr, "user");
        const nlohmann::json* base = ofield(pr, "base");
        const nlohmann::json* head = ofield(pr, "head");
        if (base && head)
            out += ", " + json::get_string_or(*head, "ref") + " -> " + json::get_string_or(*base, "ref");
        const std::string upd = date_only(json::get_string_or(pr, "updated_at"));
        if (!upd.empty()) out += ", updated " + upd;
        out += ")\n";
    }
    return out;
}

std::string format_pr(const nlohmann::json& pr, const nlohmann::json& files, const nlohmann::json& comments) {
    std::string out = "PR #" + std::to_string(nfield(pr, "number")) + ": " +
                      json::get_string_or(pr, "title") + "\n";
    out += "state: " + json::get_string_or(pr, "state");
    if (bfield(pr, "draft")) out += ", draft";
    if (bfield(pr, "merged")) {
        out += ", merged " + date_only(json::get_string_or(pr, "merged_at"));
        const std::string by = login_of(pr, "merged_by");
        if (!by.empty()) out += " by " + by;
    } else {
        out += bfield(pr, "mergeable") ? ", mergeable" : ", not mergeable";
    }
    out += '\n';
    out += "author: " + login_of(pr, "user") + "\n";
    const nlohmann::json* base = ofield(pr, "base");
    const nlohmann::json* head = ofield(pr, "head");
    if (base && head) {
        out += "base: " + json::get_string_or(*base, "ref") + " (" + short_sha(json::get_string_or(*base, "sha")) + ")";
        out += "  head: " + json::get_string_or(*head, "ref") + " (" + short_sha(json::get_string_or(*head, "sha")) + ")\n";
    }
    if (auto it = pr.find("labels"); it != pr.end() && it->is_array() && !it->empty()) {
        out += "labels: ";
        for (std::size_t i = 0; i < it->size(); ++i)
            out += (i ? ", " : "") + json::get_string_or((*it)[i], "name");
        out += '\n';
    }
    if (const nlohmann::json* ms = ofield(pr, "milestone"))
        out += "milestone: " + json::get_string_or(*ms, "title") + "\n";
    out += "created: " + date_only(json::get_string_or(pr, "created_at"));
    out += "  updated: " + date_only(json::get_string_or(pr, "updated_at")) + "\n";
    const std::string body = json::get_string_or(pr, "body");
    if (!body.empty()) out += "\n" + truncate(body, 4096, default_trunc_marker) + "\n";
    if (files.is_array() && !files.empty()) {
        out += "\nchanged files (" + std::to_string(files.size()) + "):\n";
        for (const nlohmann::json& f : files) {
            out += "  " + json::get_string_or(f, "status") + " " + json::get_string_or(f, "filename");
            out += " (+" + std::to_string(nfield(f, "additions")) + "/-" +
                   std::to_string(nfield(f, "deletions")) + ")\n";
        }
    }
    if (comments.is_array() && !comments.empty()) {
        out += "\ncomments (" + std::to_string(comments.size()) + "):\n";
        for (const nlohmann::json& c : comments) {
            out += "  [" + login_of(c, "user") + " @ " +
                   date_only(json::get_string_or(c, "created_at")) + "] ";
            out += truncate(json::get_string_or(c, "body"), 2048, default_trunc_marker) + "\n";
        }
    }
    return out;
}

namespace {

// One "sha  date  author  title" line shared by the commit-list formatters.
std::string commit_line(const njson& c) {
    std::string out = short_sha(json::get_string_or(c, "sha"));
    std::string author = login_of(c, "author");
    std::string date, title;
    if (const njson* meta = ofield(c, "commit")) {
        title = one_line(json::get_string_or(*meta, "message"), 100);
        if (const njson* a = ofield(*meta, "author")) {
            if (author.empty()) author = json::get_string_or(*a, "name");
            date = date_only(json::get_string_or(*a, "date"));
        }
    }
    return out + "  " + date + "  " + author + "  " + title;
}

}  // namespace

std::string format_commit_list(const nlohmann::json& commits) {
    if (!commits.is_array() || commits.empty()) return "No commits found.";
    std::string out;
    for (const nlohmann::json& c : commits) out += commit_line(c) + "\n";
    return out;
}

std::string format_recent_commits(const nlohmann::json& groups) {
    if (!groups.is_array() || groups.empty()) return "No matching commits found.";
    std::string out;
    for (const nlohmann::json& g : groups) {
        const nlohmann::json* commits = nullptr;
        if (auto it = g.find("commits"); it != g.end() && it->is_array())
            commits = &*it;
        const std::size_t n = commits ? commits->size() : 0;
        if (!out.empty()) out += '\n';
        out += json::get_string_or(g, "repo") + " (" + std::to_string(n) +
               (n == 1 ? " commit" : " commits");
        if (auto it = g.find("branches"); it != g.end() && it->is_array() &&
                                          !it->empty()) {
            out += "; branches: ";
            for (std::size_t i = 0; i < it->size(); ++i) {
                if (i) out += ", ";
                out += (*it)[i].is_string() ? (*it)[i].get<std::string>() : "";
            }
        }
        out += "):\n";
        if (commits)
            for (const nlohmann::json& c : *commits) out += "  " + commit_line(c) + "\n";
    }
    return out;
}

std::string format_commit(const nlohmann::json& commit) {
    std::string out = "commit " + json::get_string_or(commit, "sha") + "\n";
    if (const nlohmann::json* meta = ofield(commit, "commit")) {
        if (const nlohmann::json* a = ofield(*meta, "author")) {
            out += "author: " + json::get_string_or(*a, "name");
            const std::string email = json::get_string_or(*a, "email");
            if (!email.empty()) out += " <" + email + ">";
            out += "  " + date_only(json::get_string_or(*a, "date")) + "\n";
        }
        if (auto it = commit.find("parents"); it != commit.end() && it->is_array() &&
                                              !it->empty()) {
            out += "parents:";
            for (const nlohmann::json& p : *it) out += " " + short_sha(json::get_string_or(p, "sha"));
            out += '\n';
        }
        if (const nlohmann::json* st = ofield(commit, "stats"))
            out += "stats: +" + std::to_string(nfield(*st, "additions")) + " -" +
                   std::to_string(nfield(*st, "deletions")) + "\n";
        out += "\n" + truncate(json::get_string_or(*meta, "message"), 8192, default_trunc_marker) + "\n";
    }
    if (auto it = commit.find("files"); it != commit.end() && it->is_array() &&
                                        !it->empty()) {
        out += "\nfiles (" + std::to_string(it->size()) + "):\n";
        for (const nlohmann::json& f : *it)
            out += "  " + json::get_string_or(f, "status") + " " + json::get_string_or(f, "filename") + "\n";
    }
    return out;
}

std::string format_dir_listing(const nlohmann::json& entries) {
    if (!entries.is_array()) return "Not a directory listing.";
    if (entries.empty()) return "Empty directory.";
    // Deterministic: directories first, then by path.
    std::vector<const nlohmann::json*> sorted;
    sorted.reserve(entries.size());
    for (const nlohmann::json& e : entries) sorted.push_back(&e);
    std::ranges::stable_sort(sorted, [](const nlohmann::json* a, const nlohmann::json* b) {
        const bool da = json::get_string_or(*a, "type") == "dir", db = json::get_string_or(*b, "type") == "dir";
        if (da != db) return da;
        return json::get_string_or(*a, "path") < json::get_string_or(*b, "path");
    });
    std::string out;
    for (const nlohmann::json* e : sorted) {
        const std::string type = json::get_string_or(*e, "type");
        out += json::get_string_or(*e, "name");
        if (type == "dir") out += "/";
        else if (type == "file") out += "  (" + std::to_string(nfield(*e, "size")) + " B)";
        else if (!type.empty()) out += "  [" + type + "]";
        out += '\n';
    }
    return out;
}

// --- client --------------------------------------------------------------------

GiteaClient::GiteaClient(GiteaConfig cfg, HttpGetFn get)
    : cfg_(std::move(cfg)), api_base_(gitea_api_base(cfg_.base_url)),
      get_(std::move(get)) {
    if (!get_)
        get_ = [](const std::string& url, const std::vector<std::string>& headers,
                  long timeout_secs) { return http::get(url, headers, timeout_secs); };
    if (!cfg_.token.empty())
        headers_.push_back("Authorization: token " + cfg_.token);
    else if (!cfg_.auth_user.empty() || !cfg_.auth_pass.empty())
        headers_.push_back(gitea_basic_auth_header(cfg_.auth_user, cfg_.auth_pass));
}

std::expected<std::string, Error> GiteaClient::get_text(
    const std::string& path_query) const {
    auto resp = get_(api_base_ + path_query, headers_, cfg_.timeout_secs);
    if (!resp) return std::unexpected(resp.error());
    if (resp->status < 200 || resp->status >= 300)
        return std::unexpected(Error{
            .msg = gitea_error_message(resp->status, resp->body),
            .code = static_cast<int>(resp->status)});
    return std::move(resp->body);
}

std::expected<nlohmann::json, Error> GiteaClient::get_json(
    const std::string& path_query) const {
    auto body = get_text(path_query);
    if (!body) return std::unexpected(body.error());
    return json::parse(*body);
}

std::expected<nlohmann::json, Error> GiteaClient::get_paged(
    const std::string& path, const std::string& query, int limit,
    const char* unwrap) const {
    constexpr int kPageSize = 50;
    nlohmann::json out = nlohmann::json::array();
    for (int page = 1; static_cast<int>(out.size()) < limit; ++page) {
        std::string pq = path + "?";
        if (!query.empty()) pq += query + "&";
        pq += "page=" + std::to_string(page) + "&limit=" + std::to_string(kPageSize);
        auto j = get_json(pq);
        if (!j) return j;
        const nlohmann::json* arr = &*j;
        if (unwrap) {
            auto it = j->find(unwrap);
            if (it == j->end() || !it->is_array())
                return std::unexpected(Error{
                    .msg = std::string("gitea: response missing '") + unwrap + "' array",
                    .code = 0});
            arr = &*it;
        } else if (!j->is_array()) {
            return std::unexpected(Error{.msg = "gitea: expected a JSON array", .code = 0});
        }
        if (arr->empty()) break;
        for (const nlohmann::json& e : *arr) {
            out.push_back(e);
            if (static_cast<int>(out.size()) >= limit) break;
        }
        if (static_cast<int>(arr->size()) < kPageSize) break;
    }
    return out;
}

// --- tool factory ----------------------------------------------------------------

namespace {

// Optional integer arg clamped to [lo, hi]; absent/null -> def.
std::expected<int, Error> opt_int(const nlohmann::json& args, const char* key,
                                  int def, int lo, int hi) {
    auto it = args.find(key);
    if (it == args.end() || it->is_null()) return def;
    if (!it->is_number_integer())
        return std::unexpected(Error{
            .msg = std::string(key) + " must be an integer", .code = 0});
    return std::clamp(static_cast<int>(it->get<std::int64_t>()), lo, hi);
}

// Required owner+repo pair shared by most tools.
struct OwnerRepo {
    std::string owner, repo;
};
std::expected<OwnerRepo, Error> owner_repo(const nlohmann::json& args) {
    auto owner = json::get_string(args, "owner");
    if (!owner) return std::unexpected(owner.error());
    auto repo = json::get_string(args, "repo");
    if (!repo) return std::unexpected(repo.error());
    if (owner->empty() || repo->empty())
        return std::unexpected(Error{.msg = "owner and repo must not be empty", .code = 0});
    return OwnerRepo{std::move(*owner), std::move(*repo)};
}

nlohmann::json string_prop(const char* desc) {
    return {{"type", "string"}, {"description", desc}};
}
nlohmann::json int_prop(const char* desc) {
    return {{"type", "integer"}, {"description", desc}};
}
nlohmann::json bool_prop(const char* desc) {
    return {{"type", "boolean"}, {"description", desc}};
}

nlohmann::json owner_prop() { return string_prop("repo owner (user or org)"); }
nlohmann::json repo_prop() { return string_prop("repo name"); }

nlohmann::json url_prop() {
    return string_prop("Gitea instance base URL; defaults to configured "
                       "instance ($MOOCODE_GITEA_URL)");
}

nlohmann::json pattern_prop() {
    return string_prop("ECMAScript regex, case-insensitive, matched on "
                       "owner/name, e.g. \"^infra/\" or \"agent\"");
}

// How many repos a client-side filter may pull from the search endpoint.
constexpr int kRepoScanCap = 500;

// Shared tool context: resolves a client per call. A per-call `url` arg
// overrides the configured instance; the configured token is attached only
// when the target shares the configured origin, so the credential never goes
// to a model-chosen host.
struct GiteaCtx {
    GiteaConfig cfg;
    HttpGetFn get;

    std::expected<GiteaClient, Error> client_for(const nlohmann::json& args) const {
        auto url = ::moocode::json::get_string_or_default(args, "url", "");
        if (!url) return std::unexpected(url.error());
        GiteaConfig c = cfg;
        if (!url->empty()) {
            if (!url->starts_with("http://") && !url->starts_with("https://"))
                return std::unexpected(Error{
                    .msg = "url must be http:// or https://", .code = 0});
            c.base_url = *url;
            if (gitea_origin(*url) != gitea_origin(cfg.base_url)) {
                c.token.clear();
                c.auth_user.clear();
                c.auth_pass.clear();
            }
        }
        if (c.base_url.empty())
            return std::unexpected(Error{
                .msg = "no Gitea instance configured: pass `url` or set "
                       "MOOCODE_GITEA_URL",
                .code = 0});
        return GiteaClient(std::move(c), get);
    }
};

// Cap a raw body (diff/file) to min(tool ceiling, per-call max_bytes arg).
std::expected<std::size_t, Error> body_cap(const nlohmann::json& args,
                                           std::size_t ceiling) {
    auto n = opt_int(args, "max_bytes", 0, 1, 1 << 30);
    if (!n) return std::unexpected(n.error());
    if (*n > 0) return std::min(ceiling, static_cast<std::size_t>(*n));
    return ceiling;
}

}  // namespace

std::vector<Tool> gitea_tools(GiteaConfig cfg, HttpGetFn get) {
    auto ctx = std::make_shared<GiteaCtx>(GiteaCtx{std::move(cfg), std::move(get)});
    const std::size_t max_bytes = ctx->cfg.max_bytes;
    std::vector<Tool> tools;

    tools.push_back(Tool{
        ToolSpec{"gitea_repos",
                 "List or search repos on the Gitea instance. With `owner`, "
                 "lists that user's/org's repos; else searches all visible repos "
                 "(filter with `query`). `pattern` filters by regex on owner/name.",
                 nlohmann::json{
                     {"type", "object"},
                     {"properties",
                      {{"url", url_prop()},
                       {"query", string_prop("keyword filter for repo search")},
                       {"pattern", pattern_prop()},
                       {"owner", string_prop("list repos of this user or org only")},
                       {"limit", int_prop("max repos to return (default 30)")}}}}},
        [ctx](const nlohmann::json& args) -> std::expected<std::string, Error> {
            auto client = ctx->client_for(args);
            if (!client) return std::unexpected(client.error());
            auto query = ::moocode::json::get_string_or_default(args, "query", "");
            if (!query) return std::unexpected(query.error());
            auto pattern = ::moocode::json::get_string_or_default(args, "pattern", "");
            if (!pattern) return std::unexpected(pattern.error());
            auto owner = ::moocode::json::get_string_or_default(args, "owner", "");
            if (!owner) return std::unexpected(owner.error());
            auto limit = opt_int(args, "limit", 30, 1, 200);
            if (!limit) return std::unexpected(limit.error());
            // Regex filtering happens client-side, so scan past `limit` to
            // still fill the list when matches are sparse.
            const int scan = pattern->empty() ? *limit : kRepoScanCap;
            auto repos =
                owner->empty()
                    ? client->get_paged("/repos/search",
                                        query->empty() ? "" : "q=" + http::url_encode(*query),
                                        scan, "data")
                    : client->get_paged("/users/" + http::url_encode(*owner) + "/repos", "", scan);
            if (!repos) return std::unexpected(repos.error());
            if (!pattern->empty()) {
                auto matched = regex_filter(*repos, "full_name", *pattern);
                if (!matched) return std::unexpected(matched.error());
                while (static_cast<int>(matched->size()) > *limit)
                    matched->erase(matched->size() - 1);
                return format_repo_list(*matched);
            }
            return format_repo_list(*repos);
        }});

    tools.push_back(Tool{
        ToolSpec{"gitea_repo",
                 "Inspect one Gitea repo: metadata (default branch, visibility, "
                 "counters, clone URL) plus branch list.",
                 nlohmann::json{
                     {"type", "object"},
                     {"properties",
                      {{"url", url_prop()},
                       {"owner", owner_prop()},
                       {"repo", repo_prop()}}},
                     {"required", nlohmann::json::array({"owner", "repo"})}}},
        [ctx](const nlohmann::json& args) -> std::expected<std::string, Error> {
            auto client = ctx->client_for(args);
            if (!client) return std::unexpected(client.error());
            auto or_ = owner_repo(args);
            if (!or_) return std::unexpected(or_.error());
            const std::string base = repo_path(or_->owner, or_->repo);
            auto repo = client->get_json(base);
            if (!repo) return std::unexpected(repo.error());
            auto branches = client->get_paged(base + "/branches", "", 100);
            if (!branches) return std::unexpected(branches.error());
            return format_repo(*repo, *branches);
        }});

    tools.push_back(Tool{
        ToolSpec{"gitea_prs",
                 "List pull requests of a Gitea repo, newest first.",
                 nlohmann::json{
                     {"type", "object"},
                     {"properties",
                      {{"url", url_prop()},
                       {"owner", owner_prop()},
                       {"repo", repo_prop()},
                       {"state", string_prop("open (default), closed, or all")},
                       {"limit", int_prop("max PRs to return (default 30)")}}},
                     {"required", nlohmann::json::array({"owner", "repo"})}}},
        [ctx](const nlohmann::json& args) -> std::expected<std::string, Error> {
            auto client = ctx->client_for(args);
            if (!client) return std::unexpected(client.error());
            auto or_ = owner_repo(args);
            if (!or_) return std::unexpected(or_.error());
            auto state = ::moocode::json::get_string_or_default(args, "state", "open");
            if (!state) return std::unexpected(state.error());
            if (*state != "open" && *state != "closed" && *state != "all")
                return std::unexpected(Error{
                    .msg = "state must be open, closed, or all", .code = 0});
            auto limit = opt_int(args, "limit", 30, 1, 200);
            if (!limit) return std::unexpected(limit.error());
            auto prs = client->get_paged(repo_path(or_->owner, or_->repo) + "/pulls",
                                         "state=" + *state + "&sort=recentupdate",
                                         *limit);
            if (!prs) return std::unexpected(prs.error());
            return format_pr_list(*prs);
        }});

    tools.push_back(Tool{
        ToolSpec{"gitea_pr",
                 "Inspect one pull request: metadata, description, changed files "
                 "with add/delete counts, and comments.",
                 nlohmann::json{
                     {"type", "object"},
                     {"properties",
                      {{"url", url_prop()},
                       {"owner", owner_prop()},
                       {"repo", repo_prop()},
                       {"number", int_prop("pull request number")}}},
                     {"required", nlohmann::json::array({"owner", "repo", "number"})}}},
        [ctx](const nlohmann::json& args) -> std::expected<std::string, Error> {
            auto client = ctx->client_for(args);
            if (!client) return std::unexpected(client.error());
            auto or_ = owner_repo(args);
            if (!or_) return std::unexpected(or_.error());
            auto number = json::get_int(args, "number");
            if (!number) return std::unexpected(number.error());
            const std::string base = repo_path(or_->owner, or_->repo);
            const std::string n = std::to_string(*number);
            auto pr = client->get_json(base + "/pulls/" + n);
            if (!pr) return std::unexpected(pr.error());
            auto files = client->get_paged(base + "/pulls/" + n + "/files", "", 300);
            if (!files) return std::unexpected(files.error());
            auto comments =
                client->get_paged(base + "/issues/" + n + "/comments", "", 100);
            if (!comments) return std::unexpected(comments.error());
            return format_pr(*pr, *files, *comments);
        }});

    tools.push_back(Tool{
        ToolSpec{"gitea_pr_diff",
                 "Fetch raw unified diff of a pull request (truncated when large).",
                 nlohmann::json{
                     {"type", "object"},
                     {"properties",
                      {{"url", url_prop()},
                       {"owner", owner_prop()},
                       {"repo", repo_prop()},
                       {"number", int_prop("pull request number")},
                       {"max_bytes",
                        int_prop("cap on returned diff bytes")}}},
                     {"required", nlohmann::json::array({"owner", "repo", "number"})}}},
        [ctx, max_bytes](const nlohmann::json& args)
            -> std::expected<std::string, Error> {
            auto client = ctx->client_for(args);
            if (!client) return std::unexpected(client.error());
            auto or_ = owner_repo(args);
            if (!or_) return std::unexpected(or_.error());
            auto number = json::get_int(args, "number");
            if (!number) return std::unexpected(number.error());
            auto cap = body_cap(args, max_bytes);
            if (!cap) return std::unexpected(cap.error());
            auto diff = client->get_text(repo_path(or_->owner, or_->repo) + "/pulls/" +
                                         std::to_string(*number) + ".diff");
            if (!diff) return std::unexpected(diff.error());
            return truncate(std::move(*diff), *cap, default_trunc_marker);
        }});

    tools.push_back(Tool{
        ToolSpec{"gitea_commits",
                 "List commits of a Gitea repo, newest first. Can start from a "
                 "branch/tag/sha or filter by file path.",
                 nlohmann::json{
                     {"type", "object"},
                     {"properties",
                      {{"url", url_prop()},
                       {"owner", owner_prop()},
                       {"repo", repo_prop()},
                       {"ref", string_prop("branch, tag, or commit sha to start from "
                                           "(default: default branch)")},
                       {"path", string_prop("only commits touching this file path")},
                       {"limit", int_prop("max commits to return (default 30)")}}},
                     {"required", nlohmann::json::array({"owner", "repo"})}}},
        [ctx](const nlohmann::json& args) -> std::expected<std::string, Error> {
            auto client = ctx->client_for(args);
            if (!client) return std::unexpected(client.error());
            auto or_ = owner_repo(args);
            if (!or_) return std::unexpected(or_.error());
            auto ref = ::moocode::json::get_string_or_default(args, "ref", "");
            if (!ref) return std::unexpected(ref.error());
            auto path = ::moocode::json::get_string_or_default(args, "path", "");
            if (!path) return std::unexpected(path.error());
            auto limit = opt_int(args, "limit", 30, 1, 200);
            if (!limit) return std::unexpected(limit.error());
            std::string query = "stat=false&verification=false&files=false";
            if (!ref->empty()) query += "&sha=" + http::url_encode(*ref);
            if (!path->empty()) query += "&path=" + http::url_encode(*path);
            auto commits = client->get_paged(
                repo_path(or_->owner, or_->repo) + "/commits", query, *limit);
            if (!commits) return std::unexpected(commits.error());
            return format_commit_list(*commits);
        }});

    tools.push_back(Tool{
        ToolSpec{"gitea_commit",
                 "Inspect one commit: author, message, per-file stats, and "
                 "its raw diff when asked.",
                 nlohmann::json{
                     {"type", "object"},
                     {"properties",
                      {{"url", url_prop()},
                       {"owner", owner_prop()},
                       {"repo", repo_prop()},
                       {"sha", string_prop("commit sha (or branch/tag name)")},
                       {"diff", bool_prop("also include raw unified diff")},
                       {"max_bytes",
                        int_prop("cap on returned diff bytes")}}},
                     {"required", nlohmann::json::array({"owner", "repo", "sha"})}}},
        [ctx, max_bytes](const nlohmann::json& args)
            -> std::expected<std::string, Error> {
            auto client = ctx->client_for(args);
            if (!client) return std::unexpected(client.error());
            auto or_ = owner_repo(args);
            if (!or_) return std::unexpected(or_.error());
            auto sha = json::get_string(args, "sha");
            if (!sha) return std::unexpected(sha.error());
            const std::string base = repo_path(or_->owner, or_->repo);
            auto commit = client->get_json(base + "/git/commits/" + http::url_encode(*sha) +
                                           "?stat=true");
            if (!commit) return std::unexpected(commit.error());
            std::string out = format_commit(*commit);
            auto it = args.find("diff");
            if (it != args.end() && it->is_boolean() && it->get<bool>()) {
                auto cap = body_cap(args, max_bytes);
                if (!cap) return std::unexpected(cap.error());
                auto diff = client->get_text(base + "/git/commits/" + http::url_encode(*sha) + ".diff");
                if (!diff) return std::unexpected(diff.error());
                out += "\ndiff:\n" + truncate(std::move(*diff), *cap, default_trunc_marker);
            }
            return out;
        }});

    tools.push_back(Tool{
        ToolSpec{"gitea_file",
                 "Read a file or list a directory of a Gitea repo at a ref. "
                 "Directories return entries; files return raw content "
                 "(truncated when large).",
                 nlohmann::json{
                     {"type", "object"},
                     {"properties",
                      {{"url", url_prop()},
                       {"owner", owner_prop()},
                       {"repo", repo_prop()},
                       {"path",
                        string_prop("repo-relative file or directory path "
                                    "(default: repo root)")},
                       {"ref", string_prop("branch, tag, or commit sha "
                                           "(default: default branch)")},
                       {"max_bytes",
                        int_prop("cap on returned file bytes")}}},
                     {"required", nlohmann::json::array({"owner", "repo"})}}},
        [ctx, max_bytes](const nlohmann::json& args)
            -> std::expected<std::string, Error> {
            auto client = ctx->client_for(args);
            if (!client) return std::unexpected(client.error());
            auto or_ = owner_repo(args);
            if (!or_) return std::unexpected(or_.error());
            auto path = ::moocode::json::get_string_or_default(args, "path", "");
            if (!path) return std::unexpected(path.error());
            auto ref = ::moocode::json::get_string_or_default(args, "ref", "");
            if (!ref) return std::unexpected(ref.error());
            const std::string base = repo_path(or_->owner, or_->repo);
            const std::string ref_q =
                ref->empty() ? "" : "?ref=" + http::url_encode(*ref);
            auto contents = client->get_json(base + "/contents/" + enc_path(*path) + ref_q);
            if (!contents) return std::unexpected(contents.error());
            if (contents->is_array()) return format_dir_listing(*contents);
            const std::string type = json::get_string_or(*contents, "type");
            if (type != "file")
                return "path is a " + (type.empty() ? "non-file entry" : type) + ": " +
                       json::get_string_or(*contents, "path");
            auto cap = body_cap(args, max_bytes);
            if (!cap) return std::unexpected(cap.error());
            auto raw = client->get_text(base + "/raw/" + enc_path(*path) + ref_q);
            if (!raw) return std::unexpected(raw.error());
            const std::size_t probe = std::min<std::size_t>(raw->size(), 8192);
            if (std::memchr(raw->data(), '\0', probe) != nullptr)
                return "binary file (" + std::to_string(raw->size()) + " bytes)";
            return truncate(std::move(*raw), *cap, default_trunc_marker);
        }});

    tools.push_back(Tool{
        ToolSpec{"gitea_recent_commits",
                 "List commits within a recent timeframe across all visible repos, "
                 "grouped per repo, newest first. `pattern` limits which repos are "
                 "scanned, `branch_pattern` which branches (default: all); commits "
                 "on several branches reported once. Repos with no commits in the "
                 "window are dropped.",
                 nlohmann::json{
                     {"type", "object"},
                     {"properties",
                      {{"url", url_prop()},
                       {"timeframe",
                        string_prop("how far back to look: <number><unit> with "
                                    "unit m(inutes)/h(ours)/d(ays)/w(eeks), "
                                    "e.g. \"36h\" or \"7d\"")},
                       {"pattern", pattern_prop()},
                       {"branch_pattern",
                        string_prop("ECMAScript regex, case-insensitive, matched on "
                                    "branch names, e.g. \"^(main|release-)\"; "
                                    "default: all branches")},
                       {"max_repos",
                        int_prop("max repos to report (default 20)")},
                       {"limit",
                        int_prop("max commits per repo (default 20)")}}},
                     {"required", nlohmann::json::array({"timeframe"})}}},
        [ctx](const nlohmann::json& args) -> std::expected<std::string, Error> {
            auto client = ctx->client_for(args);
            if (!client) return std::unexpected(client.error());
            auto timeframe = json::get_string(args, "timeframe");
            if (!timeframe) return std::unexpected(timeframe.error());
            auto span = parse_timeframe(*timeframe);
            if (!span) return std::unexpected(span.error());
            auto pattern = ::moocode::json::get_string_or_default(args, "pattern", "");
            if (!pattern) return std::unexpected(pattern.error());
            auto branch_pat = ::moocode::json::get_string_or_default(args, "branch_pattern", "");
            if (!branch_pat) return std::unexpected(branch_pat.error());
            // Validate the branch regex up front; per-repo filtering below
            // skips repos on error and would otherwise mask a bad pattern.
            if (!branch_pat->empty()) {
                auto check = regex_filter(nlohmann::json::array(), "name", *branch_pat);
                if (!check) return std::unexpected(check.error());
            }
            auto max_repos = opt_int(args, "max_repos", 20, 1, 100);
            if (!max_repos) return std::unexpected(max_repos.error());
            auto limit = opt_int(args, "limit", 20, 1, 200);
            if (!limit) return std::unexpected(limit.error());
            const std::int64_t now =
                ctx->cfg.now_epoch
                    ? ctx->cfg.now_epoch()
                    : std::chrono::duration_cast<std::chrono::seconds>(
                          std::chrono::system_clock::now().time_since_epoch())
                          .count();
            const std::int64_t cutoff = now - span->count();
            // Newest-updated first: every repo pushed to within the window is a
            // prefix of the scan, so stop at the first stale one.
            auto repos = client->get_paged("/repos/search", "sort=updated&order=desc",
                                           kRepoScanCap, "data");
            if (!repos) return std::unexpected(repos.error());
            nlohmann::json active = nlohmann::json::array();
            for (const nlohmann::json& r : *repos) {
                const auto updated = parse_iso8601(json::get_string_or(r, "updated_at"));
                if (updated && *updated < cutoff) break;
                active.push_back(r);
            }
            auto matched = pattern->empty()
                               ? std::expected<nlohmann::json, Error>(std::move(active))
                               : regex_filter(active, "full_name", *pattern);
            if (!matched) return std::unexpected(matched.error());
            const std::string since_q =
                "stat=false&verification=false&files=false&since=" +
                http::url_encode(iso8601_utc(cutoff));
            nlohmann::json groups = nlohmann::json::array();
            for (const nlohmann::json& r : *matched) {
                if (static_cast<int>(groups.size()) >= *max_repos) break;
                const std::string full = json::get_string_or(r, "full_name");
                const std::size_t slash = full.find('/');
                if (slash == std::string::npos) continue;
                const std::string base =
                    repo_path(full.substr(0, slash), full.substr(slash + 1));
                // Skip per-repo failures (e.g. HTTP 409 for an empty repo)
                // instead of failing the whole sweep.
                auto branches = client->get_paged(base + "/branches", "", 100);
                if (!branches) continue;
                auto picked = branch_pat->empty()
                                  ? std::expected<nlohmann::json, Error>(
                                        std::move(*branches))
                                  : regex_filter(*branches, "name", *branch_pat);
                if (!picked) continue;  // pattern pre-validated; defensive
                nlohmann::json recent = nlohmann::json::array();
                nlohmann::json hit_branches = nlohmann::json::array();
                std::vector<std::string> seen;  // shas already reported
                for (const nlohmann::json& b : *picked) {
                    if (static_cast<int>(recent.size()) >= *limit) break;
                    const std::string name = json::get_string_or(b, "name");
                    if (name.empty()) continue;
                    auto commits = client->get_paged(
                        base + "/commits", since_q + "&sha=" + http::url_encode(name), *limit);
                    if (!commits) continue;
                    // `since` needs a recent Gitea; filter client-side regardless.
                    bool contributed = false;
                    for (nlohmann::json& c : filter_commits_since(*commits, cutoff)) {
                        if (static_cast<int>(recent.size()) >= *limit) break;
                        const std::string sha = json::get_string_or(c, "sha");
                        if (!sha.empty() && std::ranges::contains(seen, sha)) continue;
                        if (!sha.empty()) seen.push_back(sha);
                        recent.push_back(std::move(c));
                        contributed = true;
                    }
                    if (contributed) hit_branches.push_back(name);
                }
                if (recent.empty()) continue;
                // Branches are fetched one after another; restore a global
                // newest-first order across them (undated commits sink).
                std::stable_sort(recent.begin(), recent.end(),
                                 [](const nlohmann::json& a, const nlohmann::json& b) {
                    constexpr auto kMin = std::numeric_limits<std::int64_t>::min();
                    return commit_epoch(a).value_or(kMin) >
                           commit_epoch(b).value_or(kMin);
                });
                groups.push_back({{"repo", full},
                                  {"branches", std::move(hit_branches)},
                                  {"commits", std::move(recent)}});
            }
            if (groups.empty())
                return "No commits in the last " + *timeframe +
                       (pattern->empty()
                            ? std::string(".")
                            : " for repositories matching '" + *pattern + "'.");
            return format_recent_commits(groups);
        }});

    return tools;
}

}  // namespace moocode
