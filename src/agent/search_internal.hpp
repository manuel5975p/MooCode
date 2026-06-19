#ifndef MOOCODE_SEARCH_INTERNAL_HPP
#define MOOCODE_SEARCH_INTERNAL_HPP

// Internals of the web-search backends (search.cpp): result type, the
// SearchBackend interface and its concrete SearXNG/Tavily/DuckDuckGo
// implementations, response parsers, the persisted monthly quota, and the
// Router that sequences them. Kept out of the public search.hpp surface; only
// search.cpp and its white-box tests need these. Not part of the stable API.

#include <expected>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "agent/types.hpp"  // Error

namespace moocode {

// One search hit, normalized across backends.
struct SearchResult {
    std::string title;
    std::string url;
    std::string snippet;
};

// A source of search results. Implementations may perform network I/O.
struct SearchBackend {
    virtual ~SearchBackend() = default;
    // pre: max_results > 0. post: results (possibly empty) on success; Error on
    // transport/HTTP/parse failure.
    virtual std::expected<std::vector<SearchResult>, Error>
    search(std::string_view query, int max_results) = 0;
    virtual std::string_view name() const = 0;
};

// --- pure helpers (no I/O; unit-tested directly) ----------------------------

// Parse a SearXNG `format=json` response body. pre: none. post: the "results"
// array mapped to SearchResult; Error if "results" is absent or not an array.
std::expected<std::vector<SearchResult>, Error>
parse_searxng_results(const nlohmann::json& body);

// Parse a Tavily `/search` response body. Same contract as parse_searxng.
std::expected<std::vector<SearchResult>, Error>
parse_tavily_results(const nlohmann::json& body);

// Parse a z.ai (GLM) /paas/v4/web_search response body. Unlike SearXNG/Tavily
// the results live under "search_result" (singular) and each hit uses "link"
// for the URL and "content" for the snippet. pre: none. post: the
// "search_result" array mapped to SearchResult; Error if absent/not an array.
std::expected<std::vector<SearchResult>, Error>
parse_zai_results(const nlohmann::json& body);

// Parse DuckDuckGo HTML search results (from html.duckduckgo.com/html/).
// Extracts title, url, and snippet from each result block; strips HTML tags.
// pre: none. max_results > 0. post: up to max_results SearchResult entries.
std::expected<std::vector<SearchResult>, Error>
parse_duckduckgo_html(std::string_view html, int max_results);

// Build the SearXNG JSON-API GET URL. pre: base nonempty.
std::string searxng_search_url(std::string_view base, std::string_view query,
                               int max_results);

// Build the Tavily `/search` POST body (basic depth, to conserve credits).
std::string tavily_request_body(std::string_view query, int max_results);

// Build the z.ai /paas/v4/web_search POST body. `search-prime` is the only
// engine; `count` is clamped to the API's 1..50 range by the caller. pre: none.
std::string zai_request_body(std::string_view query, int max_results);

// Render results as model-facing tool output (numbered title / url / snippet),
// or a clear "no results" line when empty. Total.
std::string format_results(const std::vector<SearchResult>& results);

// Current UTC month as "YYYY-MM" (the quota period key).
std::string current_month();

// --- concrete backends ------------------------------------------------------

class SearxngBackend : public SearchBackend {
public:
    explicit SearxngBackend(std::string base_url, long timeout_secs = 15);
    std::expected<std::vector<SearchResult>, Error>
    search(std::string_view query, int max_results) override;
    std::string_view name() const override { return "searxng"; }

private:
    std::string base_url_;
    long timeout_secs_;
};

class TavilyBackend : public SearchBackend {
public:
    explicit TavilyBackend(std::string api_key,
                           std::string base_url = "https://api.tavily.com",
                           long timeout_secs = 15);
    std::expected<std::vector<SearchResult>, Error>
    search(std::string_view query, int max_results) override;
    std::string_view name() const override { return "tavily"; }

private:
    std::string api_key_;
    std::string base_url_;
    long timeout_secs_;
};

// z.ai (GLM) Web Search backend. A credential-gated paid search engine
// (POST /paas/v4/web_search, Bearer auth, `search-prime` engine) returning
// LLM-oriented results. Used as a premium fallback tier when SearXNG yields
// nothing; gated by a z.ai API key (env ZAI_API_KEY or credentials.toml "zai").
class ZaiBackend : public SearchBackend {
public:
    explicit ZaiBackend(std::string api_key,
                        std::string base_url = "https://api.z.ai/api",
                        long timeout_secs = 15);
    std::expected<std::vector<SearchResult>, Error>
    search(std::string_view query, int max_results) override;
    std::string_view name() const override { return "zai"; }

private:
    std::string api_key_;
    std::string base_url_;
    long timeout_secs_;
};

// Keyless last-resort backend hitting DuckDuckGo's HTML search endpoint
// (html.duckduckgo.com/html/). Always available with no API key; returns
// real web results by scraping the HTML response. More reliable than the
// Instant-Answer JSON API, which is designed for fact lookups and rarely
// returns web results for arbitrary queries.
class DuckDuckGoBackend : public SearchBackend {
public:
    explicit DuckDuckGoBackend(
        std::string base_url = "https://html.duckduckgo.com",
        long timeout_secs = 15);
    std::expected<std::vector<SearchResult>, Error>
    search(std::string_view query, int max_results) override;
    std::string_view name() const override { return "duckduckgo"; }

private:
    std::string base_url_;
    long timeout_secs_;
};

// --- monthly quota ----------------------------------------------------------

// A persisted per-month call counter (JSON file `{"month":"YYYY-MM","used":N}`).
// The counter resets when a new month is charged, and survives process restarts
// so a long-running agent stays within Tavily's free tier across the month.
// Not thread-safe; the agent invokes tools sequentially.
class MonthlyQuota {
public:
    // pre: limit >= 0. Loads prior state from `file` when it exists.
    MonthlyQuota(std::filesystem::path file, int limit);
    // Charge one unit against `month` (resets first when `month` differs from
    // the stored period). post: true if charged (was under limit), else false.
    bool charge(std::string_view month);
    // True if a charge against `month` would succeed (read-only; no reset/persist).
    // post: month_ != month || used_ < limit_.
    bool has_budget(std::string_view month) const;
    // Units already used in `month` (0 when `month` is a new period).
    int used(std::string_view month) const;
    int limit() const { return limit_; }

private:
    std::filesystem::path file_;
    int limit_;
    std::string month_;
    int used_ = 0;
    void store() const;
};

// --- router -----------------------------------------------------------------

// One fallback tier: a backend plus an optional monthly quota gating it. A tier
// with quota == nullptr is always eligible (e.g. a keyless backend); a tier
// with a quota is skipped when the budget for the current month is exhausted.
struct FallbackTier {
    SearchBackend* backend = nullptr;  // required
    MonthlyQuota* quota = nullptr;     // optional (nullptr = uncapped)
};

// SearXNG first; on Error or empty results fall back through the configured
// tiers in order, each gated by its own optional quota, ending at the keyless
// last-resort tier (DDG). Holds non-owning references.
struct Router {
    SearchBackend* primary = nullptr;  // required
    std::vector<FallbackTier> fallbacks;  // ordered; may be empty
    std::function<std::string()> month_fn = current_month;
    int max_results = 5;

    // pre: primary set. post: results from whichever backend served first;
    // tries primary, then each quota-gated fallback tier in order; if every
    // attempt yielded nothing, the most relevant Error or empty primary result.
    std::expected<std::vector<SearchResult>, Error>
    search(std::string_view query) const;
};

}  // namespace moocode

#endif  // MOOCODE_SEARCH_INTERNAL_HPP
