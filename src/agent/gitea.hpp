#ifndef FLAGENT_GITEA_HPP
#define FLAGENT_GITEA_HPP

// Read-only Gitea inspection tools over the v1 REST API: list/inspect
// repositories and pull requests, query commits, diffs and files of any repo
// on a configured Gitea instance. One GiteaClient (token auth, pagination,
// error decoding) behind a set of small tools; all JSON->text rendering is in
// pure helpers so the formatting is unit-testable without a server.

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "agent/http.hpp"
#include "agent/tools.hpp"
#include "agent/types.hpp"

namespace flagent {

struct GiteaConfig {
    // Default instance root, with or without /api/v1. May be empty: every tool
    // also takes an optional per-call `url` arg; with no default, calls lacking
    // one fail with a clear message.
    std::string base_url;
    // Sent only to base_url's origin; per-call URLs on any other origin go
    // anonymous so the credential never leaks to a model-chosen host.
    std::string token;               // empty => anonymous (public repos only)
    // Basic-auth fallback (from the $FLAGENT_GITEA_AUTH file), used when
    // `token` is empty; origin-pinned exactly like the token.
    std::string auth_user, auth_pass;
    long timeout_secs = 30;
    std::size_t max_bytes = 256 * 1024;  // ceiling for raw diff/file bodies
    // Clock seam (epoch seconds, UTC) for the recent-commits cutoff; empty =>
    // system clock. Injectable for deterministic tests.
    std::function<std::int64_t()> now_epoch;
};

// --- pure helpers (no I/O; unit-tested directly) ----------------------------

// Resolve the API root: base verbatim when it already ends in "/api/v1",
// else base + "/api/v1"; trailing '/' stripped first. pre: base nonempty.
std::string gitea_api_base(std::string_view base_url);

// Human-readable error for a non-2xx response, keeping Gitea's JSON `message`
// field when the body carries one ("HTTP 404 ...: <message>"). Total.
std::string gitea_error_message(long status, std::string_view body);

// "scheme://host[:port]" prefix of a URL, lowercased — the token-pinning
// identity. "" when `url` has no "://". Total.
std::string gitea_origin(std::string_view url);

// Credentials read from a $FLAGENT_GITEA_AUTH file. Fields are empty when the
// corresponding key is absent.
struct GiteaBasicAuth {
    std::string user, pass;
};

// Parse .env-style content for GITEA_USER / GITEA_PASS: KEY=VALUE lines, '#'
// comments and blank lines ignored, whitespace and one layer of matching
// quotes around the value stripped. Total.
GiteaBasicAuth parse_gitea_auth(std::string_view content);

// "Authorization: Basic <base64(user:pass)>" header line. Total.
std::string gitea_basic_auth_header(std::string_view user, std::string_view pass);

// Parse a relative timeframe "<N><unit>", unit m(inutes)/h(ours)/d(ays)/
// w(eeks), e.g. "90m", "36h", "7d", "2w". pre: none. Error unless N > 0.
std::expected<std::chrono::seconds, Error> parse_timeframe(std::string_view tf);

// Epoch seconds of an ISO 8601 timestamp ("2026-06-11T12:30:00Z", offset
// "+02:00"/"+0200", date-only, fractional seconds ignored). nullopt on junk.
std::optional<std::int64_t> parse_iso8601(std::string_view ts);

// "YYYY-MM-DDTHH:MM:SSZ" for epoch seconds (UTC). Total; inverse of
// parse_iso8601 on its own output.
std::string iso8601_utc(std::int64_t epoch);

// Array elements whose string `field` matches `pattern` (ECMAScript regex,
// matched case-insensitively, regex_search semantics). Error on an invalid
// pattern; non-array `items` yields an empty array.
std::expected<nlohmann::json, Error> regex_filter(const nlohmann::json& items,
                                                  const char* field,
                                                  const std::string& pattern);

// Commits whose committer (fallback: author) date >= since_epoch; undated
// commits are kept. Total.
nlohmann::json filter_commits_since(const nlohmann::json& commits,
                                    std::int64_t since_epoch);

// Render parsed API JSON as model-facing text. Total: missing or oddly typed
// fields degrade to blanks, never throw.
std::string format_repo_list(const nlohmann::json& repos);
std::string format_repo(const nlohmann::json& repo, const nlohmann::json& branches);
std::string format_pr_list(const nlohmann::json& prs);
std::string format_pr(const nlohmann::json& pr, const nlohmann::json& files,
                      const nlohmann::json& comments);
std::string format_commit_list(const nlohmann::json& commits);
std::string format_commit(const nlohmann::json& commit);
// `groups` is an array of {"repo": "owner/name", "commits": [...]}.
std::string format_recent_commits(const nlohmann::json& groups);
std::string format_dir_listing(const nlohmann::json& entries);

// --- client ------------------------------------------------------------------

// HTTP GET seam; defaults to flagent::http::get. Injectable for hermetic tests.
using HttpGetFn = std::function<std::expected<http::Response, Error>(
    const std::string& url, const std::vector<std::string>& headers,
    long timeout_secs)>;

class GiteaClient {
public:
    // pre: cfg.base_url nonempty. `get` empty => real libcurl transport.
    explicit GiteaClient(GiteaConfig cfg, HttpGetFn get = {});

    // GET api_base + path_query, parse the body as JSON. Error on transport
    // failure, non-2xx status (message decoded), or malformed JSON.
    std::expected<nlohmann::json, Error> get_json(const std::string& path_query) const;

    // GET api_base + path_query, return the raw body (diff/raw-file endpoints).
    std::expected<std::string, Error> get_text(const std::string& path_query) const;

    // Walk a paginated array endpoint (50/page) until exhausted or `limit`
    // items collected. `query` is the extra query string without page params
    // ("state=open", may be empty). `unwrap`: object key holding the array when
    // the endpoint wraps it (e.g. /repos/search -> "data"), nullptr otherwise.
    // pre: limit > 0. post: a JSON array of at most `limit` items.
    std::expected<nlohmann::json, Error> get_paged(const std::string& path,
                                                   const std::string& query,
                                                   int limit,
                                                   const char* unwrap = nullptr) const;

private:
    GiteaConfig cfg_;
    std::string api_base_;
    HttpGetFn get_;
    std::vector<std::string> headers_;
};

// --- tool factory -------------------------------------------------------------

// Build the read-only Gitea tools (gitea_repos, gitea_repo, gitea_prs,
// gitea_pr, gitea_pr_diff, gitea_commits, gitea_commit, gitea_file,
// gitea_recent_commits). Each call
// targets the per-call `url` arg when given, else cfg.base_url; cfg.token is
// attached only on cfg.base_url's origin. `get` empty => real transport.
std::vector<Tool> gitea_tools(GiteaConfig cfg, HttpGetFn get = {});

}  // namespace flagent

#endif  // FLAGENT_GITEA_HPP
