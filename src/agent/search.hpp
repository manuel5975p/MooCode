#ifndef MOOCODE_SEARCH_HPP
#define MOOCODE_SEARCH_HPP

// Web-search backends for the `web_search` tool: a self-hosted SearXNG primary
// with an optional Tavily fallback, behind one SearchBackend interface. A Router
// sends each query to SearXNG and falls back to Tavily (under a persisted
// monthly quota) only when SearXNG errors or returns nothing — keeping the free
// SearXNG path primary and Tavily as a budget-capped safety net. The backends,
// parsers, quota, and router live in search_internal.hpp; this header exposes
// only the tool factories and their configs.

#include <cstddef>
#include <filesystem>
#include <string>

#include "agent/tools.hpp"

namespace moocode {

// --- tool factory -----------------------------------------------------------

struct SearchConfig {
    std::string searxng_url = "http://localhost:8080";
    std::string tavily_api_key;  // empty => SearXNG-only (no fallback)
    std::filesystem::path quota_file;
    int tavily_monthly_limit = 1000;
    int max_results = 5;
    long timeout_secs = 15;
};

// Build the `web_search` tool, owning its backends + quota for its lifetime.
Tool web_search_tool(SearchConfig cfg);

// --- web_fetch --------------------------------------------------------------

struct FetchConfig {
    long timeout_secs = 30;
    std::size_t max_bytes = 256 * 1024;  // default/ceiling for returned body size
};

// Build the `web_fetch` tool: GET a single URL and return its body, truncated
// (with a marker) to the per-call `max_bytes` arg when given, else to
// FetchConfig::max_bytes, whichever is smaller. Rejects non-http(s) URLs.
Tool web_fetch_tool(FetchConfig cfg = {});

}  // namespace moocode

#endif  // MOOCODE_SEARCH_HPP
