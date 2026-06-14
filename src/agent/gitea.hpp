#ifndef MOOCODE_GITEA_HPP
#define MOOCODE_GITEA_HPP

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

namespace moocode {

struct GiteaConfig {
    // Default instance root, with or without /api/v1. May be empty: every tool
    // also takes an optional per-call `url` arg; with no default, calls lacking
    // one fail with a clear message.
    std::string base_url;
    // Sent only to base_url's origin; per-call URLs on any other origin go
    // anonymous so the credential never leaks to a model-chosen host.
    std::string token;               // empty => anonymous (public repos only)
    // Basic-auth fallback (from the $MOOCODE_GITEA_AUTH file), used when
    // `token` is empty; origin-pinned exactly like the token.
    std::string auth_user, auth_pass;
    long timeout_secs = 30;
    std::size_t max_bytes = 256 * 1024;  // ceiling for raw diff/file bodies
    // Clock seam (epoch seconds, UTC) for the recent-commits cutoff; empty =>
    // system clock. Injectable for deterministic tests.
    std::function<std::int64_t()> now_epoch;
};

// Pure formatting/parsing helpers are in gitea_internal.hpp (unit-testable
// without the HTTP client). Include that header only if you need them.

// --- client ------------------------------------------------------------------

// HTTP GET seam; defaults to moocode::http::get. Injectable for hermetic tests.
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

}  // namespace moocode

#endif  // MOOCODE_GITEA_HPP
