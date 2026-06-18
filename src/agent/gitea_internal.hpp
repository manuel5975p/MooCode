#ifndef MOOCODE_GITEA_INTERNAL_HPP
#define MOOCODE_GITEA_INTERNAL_HPP

// Pure formatting/parsing helpers for the Gitea REST API — no I/O, no HTTP
// client. Included only by gitea.cpp and test_gitea.cpp. Follows the same
// internal-header convention as search_internal.hpp and lsp_detail.hpp.

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "agent/types.hpp"

namespace moocode {

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

// Credentials (and optional instance URL) read from a $MOOCODE_GITEA_AUTH
// file. Fields are empty when the corresponding key is absent.
struct GiteaBasicAuth {
    std::string user, pass, url;
};

// Parse .env-style content for GITEA_USER / GITEA_PASS / GITEA_URL: KEY=VALUE
// lines, '#' comments and blank lines ignored, whitespace and one layer of
// matching quotes around the value stripped. Total.
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

}  // namespace moocode

#endif  // MOOCODE_GITEA_INTERNAL_HPP
