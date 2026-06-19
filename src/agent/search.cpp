#include "agent/search.hpp"

#include "agent/search_internal.hpp"

#include <algorithm>
#include <cstdint>
#include <ctime>
#include <fstream>

#include "agent/http_detail.hpp"
#include <iterator>
#include <memory>
#include <utility>

#include "agent/http.hpp"
#include "agent/json_util.hpp"

namespace moocode {

namespace {

// Map a `{"results":[...]}` body's array of {title,url,content} hits. Shared by
// SearXNG and Tavily, which use the same field names.
std::expected<std::vector<SearchResult>, Error>
parse_results_array(const nlohmann::json& body, const char* who) {
    if (!body.is_object() || !body.contains("results") ||
        !body["results"].is_array())
        return std::unexpected(Error{.msg = std::string(who) + ": no results array", .code = 0});

    std::vector<SearchResult> out;
    for (const auto& e : body["results"]) {
        if (!e.is_object()) continue;
        out.push_back(SearchResult{.title = json::get_string_or(e, "title"),
            .url = json::get_string_or(e, "url"),
            .snippet = json::get_string_or(e, "content")});
    }
    return out;
}

}  // namespace

std::expected<std::vector<SearchResult>, Error>
parse_searxng_results(const nlohmann::json& body) {
    return parse_results_array(body, "searxng");
}

std::expected<std::vector<SearchResult>, Error>
parse_tavily_results(const nlohmann::json& body) {
    return parse_results_array(body, "tavily");
}

// z.ai puts results under "search_result" (singular) and uses "link"/"content"
// instead of "url"/"content". Map to the same normalized SearchResult.
std::expected<std::vector<SearchResult>, Error>
parse_zai_results(const nlohmann::json& body) {
    if (!body.is_object() || !body.contains("search_result") ||
        !body["search_result"].is_array())
        return std::unexpected(
            Error{.msg = "zai: no search_result array", .code = 0});

    std::vector<SearchResult> out;
    for (const auto& e : body["search_result"]) {
        if (!e.is_object()) continue;
        out.push_back(SearchResult{.title = json::get_string_or(e, "title"),
            .url = json::get_string_or(e, "link"),
            .snippet = json::get_string_or(e, "content")});
    }
    return out;
}

// --- DuckDuckGo HTML result parsing -----------------------------------------

namespace {

// Extract the actual target URL from a DDG redirect link like
// "//duckduckgo.com/l/?uddg=ENCODED_URL&rut=...". Returns empty on failure.
std::string extract_ddg_url(std::string_view href) {
    auto pos = href.find("uddg=");
    if (pos == std::string_view::npos) return {};
    pos += 5;  // past "uddg="
    auto end = href.find('&', pos);
    std::string encoded(end == std::string_view::npos
                            ? std::string(href.substr(pos))
                            : std::string(href.substr(pos, end - pos)));
    return http::url_decode(encoded);
}

// Read attribute `name`'s value from a single opening tag's text (everything
// between '<' and the matching '>'). Matches `name` only at an attribute
// boundary — preceded by whitespace and followed by '=' (after optional spaces)
// — so a lookup for "href" never matches "data-href", and the result does not
// depend on attribute ORDER within the tag. Handles single/double-quoted and
// bare values; returns empty when the attribute is absent. Total.
std::string tag_attr(std::string_view tag, std::string_view name) {
    auto is_space = [](char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; };
    for (size_t i = 0; (i = tag.find(name, i)) != std::string_view::npos; i += name.size()) {
        bool left_ok = i == 0 || is_space(tag[i - 1]);
        size_t j = i + name.size();
        while (j < tag.size() && is_space(tag[j])) ++j;  // spaces before '='
        if (!left_ok || j >= tag.size() || tag[j] != '=') continue;
        ++j;
        while (j < tag.size() && is_space(tag[j])) ++j;  // spaces after '='
        if (j >= tag.size()) return {};
        if (char q = tag[j]; q == '"' || q == '\'') {
            auto end = tag.find(q, j + 1);
            return end == std::string_view::npos ? std::string{}
                                                 : std::string(tag.substr(j + 1, end - j - 1));
        }
        size_t end = j;  // bare (unquoted) value runs to the next space or '>'
        while (end < tag.size() && !is_space(tag[end]) && tag[end] != '>') ++end;
        return std::string(tag.substr(j, end - j));
    }
    return {};
}

// Strip HTML tags and decode basic entities from a snippet string.
std::string clean_html(std::string_view raw) {
    std::string out;
    out.reserve(raw.size());
    bool in_tag = false;
    for (size_t i = 0; i < raw.size(); ++i) {
        char c = raw[i];
        if (c == '<') {
            in_tag = true;
            continue;
        }
        if (c == '>') {
            in_tag = false;
            continue;
        }
        if (in_tag) continue;
        // Decode common HTML entities inline.
        if (c == '&') {
            if (raw.substr(i, 5) == "&amp;") { out += '&'; i += 4; continue; }
            if (raw.substr(i, 4) == "&lt;")  { out += '<'; i += 3; continue; }
            if (raw.substr(i, 4) == "&gt;")  { out += '>'; i += 3; continue; }
            if (raw.substr(i, 6) == "&quot;"){ out += '"'; i += 5; continue; }
            if (raw.substr(i, 6) == "&#x27;"){ out += '\''; i += 5; continue; }
            if (raw.substr(i, 5) == "&#39;") { out += '\''; i += 4; continue; }
        }
        out += c;
    }
    // Collapse whitespace.
    std::string trimmed;
    trimmed.reserve(out.size());
    bool in_space = false;
    for (char c : out) {
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            if (!in_space && !trimmed.empty()) { trimmed += ' '; in_space = true; }
        } else {
            trimmed += c;
            in_space = false;
        }
    }
    while (!trimmed.empty() && trimmed.back() == ' ') trimmed.pop_back();
    return trimmed;
}

}  // namespace

std::expected<std::vector<SearchResult>, Error>
parse_duckduckgo_html(std::string_view html, int max_results) {
    std::vector<SearchResult> out;
    // Each result block starts with this div class marker.
    static constexpr std::string_view kBlock =
        "class=\"result results_links results_links_deep web-result";
    static constexpr std::string_view kTitleLink =
        "class=\"result__a\"";
    static constexpr std::string_view kSnippet =
        "class=\"result__snippet\"";

    size_t pos = 0;
    while (static_cast<int>(out.size()) < max_results) {
        pos = html.find(kBlock, pos);
        if (pos == std::string_view::npos) break;
        // Find the end of this result block: next kBlock or end of document.
        size_t block_end = html.find(kBlock, pos + kBlock.size());
        if (block_end == std::string_view::npos) block_end = html.size();

        // Locate the title link <a ... class="result__a" ...> in this block.
        auto title_marker = html.find(kTitleLink, pos);
        if (title_marker == std::string_view::npos ||
            title_marker >= block_end) {
            pos = block_end;
            continue;
        }
        // Back up to the opening '<a ' and forward to that tag's closing '>'.
        auto tag_start = html.rfind("<a ", title_marker);
        auto tag_open_end = html.find('>', title_marker);
        if (tag_start == std::string_view::npos || tag_start < pos ||
            tag_open_end == std::string_view::npos || tag_open_end >= block_end) {
            pos = block_end;
            continue;
        }
        // Read href BY NAME from the opening tag — independent of attribute order.
        std::string_view open_tag = html.substr(tag_start, tag_open_end - tag_start);
        std::string url = extract_ddg_url(tag_attr(open_tag, "href"));

        // Extract title text from between the <a...> and </a>.
        auto title_close = html.find("</a>", tag_open_end);
        std::string title;
        if (title_close != std::string_view::npos && title_close < block_end &&
            tag_open_end < title_close)
            title = clean_html(html.substr(tag_open_end + 1, title_close - tag_open_end - 1));

        // Extract snippet from result__snippet link.
        auto snippet_tag = html.find(kSnippet, pos);
        std::string snippet;
        if (snippet_tag != std::string_view::npos && snippet_tag < block_end) {
            auto snip_close = html.find("</a>", snippet_tag);
            if (snip_close != std::string_view::npos && snip_close < block_end) {
                auto snip_text = html.find('>', snippet_tag);
                if (snip_text != std::string_view::npos && snip_text < snip_close)
                    snippet = clean_html(html.substr(snip_text + 1, snip_close - snip_text - 1));
            }
        }

        if (!title.empty() || !url.empty())
            out.push_back(SearchResult{.title = std::move(title),
                                       .url = std::move(url),
                                       .snippet = std::move(snippet)});
        pos = block_end;
    }
    return out;
}

std::string searxng_search_url(std::string_view base, std::string_view query, int) {
    // SearXNG has no result-count parameter; the backend trims to max_results.
    return std::string(base) + "/search?q=" + http::url_encode(query) +
           "&format=json&safesearch=0";
}

std::string tavily_request_body(std::string_view query, int max_results) {
    nlohmann::json j{{"query", std::string(query)},
                     {"search_depth", "basic"},  // 1 credit, not 2 — conserve tier
                     {"max_results", max_results},
                     {"include_answer", false}};
    return j.dump();
}

std::string zai_request_body(std::string_view query, int max_results) {
    // z.ai's `count` is 1..50; clamp defensively (the API rejects out-of-range).
    int count = max_results < 1 ? 1 : (max_results > 50 ? 50 : max_results);
    nlohmann::json j{{"search_engine", "search-prime"},
                     {"search_query", std::string(query)},
                     {"count", count}};
    return j.dump();
}

std::string format_results(const std::vector<SearchResult>& results) {
    if (results.empty()) return "No results found.";
    std::string out;
    int i = 1;
    for (const auto& r : results) {
        out += std::to_string(i++) + ". " + (r.title.empty() ? r.url : r.title) + "\n";
        out += "   " + r.url + "\n";
        if (!r.snippet.empty()) out += "   " + r.snippet + "\n";
        out += "\n";
    }
    while (!out.empty() && out.back() == '\n') out.pop_back();
    return out;
}

std::string current_month() {
    std::time_t t = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    if (::gmtime_s(&tm, &t) != 0) return {};
#else
    if (!::gmtime_r(&t, &tm)) return {};
#endif
    char buf[8];
    std::strftime(buf, sizeof buf, "%Y-%m", &tm);
    return buf;
}

// --- SearxngBackend ---------------------------------------------------------

SearxngBackend::SearxngBackend(std::string base_url, long timeout_secs)
    : base_url_(std::move(base_url)), timeout_secs_(timeout_secs) {}

std::expected<std::vector<SearchResult>, Error>
SearxngBackend::search(std::string_view query, int max_results) {
    auto resp = http::get(searxng_search_url(base_url_, query, max_results),
                          {"Accept: application/json"}, timeout_secs_);
    if (!resp) return std::unexpected(resp.error());
    if (resp->status != 200)
        return std::unexpected(
            Error{.msg = "searxng HTTP " + std::to_string(resp->status),
                .code = static_cast<int>(resp->status)});

    auto j = json::parse(resp->body);
    if (!j) return std::unexpected(j.error());
    auto results = parse_searxng_results(*j);
    if (!results) return results;
    if (static_cast<int>(results->size()) > max_results) results->resize(max_results);
    return results;
}

// --- TavilyBackend ----------------------------------------------------------

TavilyBackend::TavilyBackend(std::string api_key, std::string base_url, long timeout_secs)
    : api_key_(std::move(api_key)),
      base_url_(std::move(base_url)),
      timeout_secs_(timeout_secs) {}

std::expected<std::vector<SearchResult>, Error>
TavilyBackend::search(std::string_view query, int max_results) {
    if (api_key_.empty()) return std::unexpected(Error{.msg = "tavily: no API key", .code = 0});
    auto resp = http::post_json(base_url_ + "/search",
                                {"Authorization: Bearer " + api_key_},
                                tavily_request_body(query, max_results), timeout_secs_);
    if (!resp) return std::unexpected(resp.error());
    if (resp->status != 200)
        return std::unexpected(
            Error{.msg = "tavily HTTP " + std::to_string(resp->status) + ": " + resp->body,
                .code = static_cast<int>(resp->status)});

    auto j = json::parse(resp->body);
    if (!j) return std::unexpected(j.error());
    return parse_tavily_results(*j);
}

// --- ZaiBackend -------------------------------------------------------------

ZaiBackend::ZaiBackend(std::string api_key, std::string base_url, long timeout_secs)
    : api_key_(std::move(api_key)),
      base_url_(std::move(base_url)),
      timeout_secs_(timeout_secs) {}

std::expected<std::vector<SearchResult>, Error>
ZaiBackend::search(std::string_view query, int max_results) {
    if (api_key_.empty()) return std::unexpected(Error{.msg = "zai: no API key", .code = 0});
    auto resp = http::post_json(base_url_ + "/paas/v4/web_search",
                                {"Authorization: Bearer " + api_key_,
                                 "Accept-Language: en-US,en"},
                                zai_request_body(query, max_results), timeout_secs_);
    if (!resp) return std::unexpected(resp.error());
    if (resp->status != 200)
        return std::unexpected(
            Error{.msg = "zai HTTP " + std::to_string(resp->status) + ": " + resp->body,
                .code = static_cast<int>(resp->status)});

    auto j = json::parse(resp->body);
    if (!j) return std::unexpected(j.error());
    return parse_zai_results(*j);
}

// --- DuckDuckGoBackend ------------------------------------------------------

DuckDuckGoBackend::DuckDuckGoBackend(std::string base_url, long timeout_secs)
    : base_url_(std::move(base_url)), timeout_secs_(timeout_secs) {}

std::expected<std::vector<SearchResult>, Error>
DuckDuckGoBackend::search(std::string_view query, int max_results) {
    std::string url = base_url_ + "/html/?q=" + http::url_encode(query);
    auto resp = http::get(url, {"Accept: text/html"}, timeout_secs_);
    if (!resp) return std::unexpected(resp.error());
    if (resp->status != 200)
        return std::unexpected(
            Error{.msg = "duckduckgo HTTP " + std::to_string(resp->status),
                .code = static_cast<int>(resp->status)});

    return parse_duckduckgo_html(resp->body, max_results);
}

// --- MonthlyQuota -----------------------------------------------------------

MonthlyQuota::MonthlyQuota(std::filesystem::path file, int limit)
    : file_(std::move(file)), limit_(limit) {
    std::ifstream in(file_);
    if (!in) return;
    std::string txt((std::istreambuf_iterator<char>(in)), {});
    auto j = json::parse(txt);
    if (!j || !j->is_object()) return;  // missing/corrupt → fresh counter
    month_ = j->value("month", std::string{});
    used_ = static_cast<int>(j->value("used", 0));
}

bool MonthlyQuota::charge(std::string_view month) {
    if (month_ != month) {  // new period → reset
        month_ = std::string(month);
        used_ = 0;
    }
    if (used_ >= limit_) return false;
    ++used_;
    store();
    return true;
}

bool MonthlyQuota::has_budget(std::string_view month) const {
    return month_ != month || used_ < limit_;
}

int MonthlyQuota::used(std::string_view month) const {
    return month_ == month ? used_ : 0;
}

void MonthlyQuota::store() const {
    if (file_.empty()) return;
    std::ofstream out(file_, std::ios::trunc);
    if (!out) return;
    out << nlohmann::json{{"month", month_}, {"used", used_}}.dump();
}

// --- Router -----------------------------------------------------------------

std::expected<std::vector<SearchResult>, Error>
Router::search(std::string_view query) const {
    auto p = primary->search(query, max_results);
    if (p && !p->empty()) return p;  // SearXNG served — done

    // SearXNG errored or came back empty: walk the fallback tiers in order,
    // each gated by its own optional quota. Charge only a productive call.
    // Track every tier's outcome so an all-empty result surfaces a composite
    // error naming each backend the query actually reached.
    std::string composite;
    auto note = [&](std::string_view name, const auto& r) {
        if (!composite.empty()) composite += "; ";
        composite += std::string(name);
        composite += ": ";
        composite += (r ? "no results" : r.error().msg);
    };
    note(primary->name(), p);

    for (const auto& tier : fallbacks) {
        if (!tier.backend) continue;
        if (tier.quota && !tier.quota->has_budget(month_fn())) continue;  // spent
        auto f = tier.backend->search(query, max_results);
        if (f && !f->empty()) {
            if (tier.quota) tier.quota->charge(month_fn());
            return f;
        }
        note(tier.backend->name(), f);
    }
    // Every tier yielded nothing: surface a composite error naming each backend
    // the query reached, so the model sees which failed and how.
    return std::unexpected(Error{.msg = composite, .code = 0});
}

// --- tool factory -----------------------------------------------------------

Tool web_search_tool(SearchConfig cfg) {
    auto searxng = std::make_shared<SearxngBackend>(cfg.searxng_url, cfg.timeout_secs);
    auto ddg = std::make_shared<DuckDuckGoBackend>("https://html.duckduckgo.com",
                                                   cfg.timeout_secs);
    // Premium credential-gated tiers, built only when their key is present.
    std::shared_ptr<ZaiBackend> zai;
    if (!cfg.zai_api_key.empty())
        zai = std::make_shared<ZaiBackend>(cfg.zai_api_key, "https://api.z.ai/api",
                                           cfg.timeout_secs);
    std::shared_ptr<TavilyBackend> tavily;
    std::shared_ptr<MonthlyQuota> quota;
    if (!cfg.tavily_api_key.empty()) {
        tavily = std::make_shared<TavilyBackend>(cfg.tavily_api_key,
                                                 "https://api.tavily.com", cfg.timeout_secs);
        quota = std::make_shared<MonthlyQuota>(cfg.quota_file, cfg.tavily_monthly_limit);
    }
    const int max_results = cfg.max_results;

    ToolSpec spec{
        "web_search",
        "Search web via self-hosted SearXNG, with z.ai (GLM) and Tavily fallbacks "
        "when empty, then a keyless DuckDuckGo last resort. Returns numbered list "
        "of title / URL / snippet. Use for current or external info not in "
        "working tree.",
        nlohmann::json{
            {"type", "object"},
            {"properties",
             {{"query", {{"type", "string"}, {"description", "web search query"}}}}},
            {"required", nlohmann::json::array({"query"})}}};

    auto run = [searxng, zai, tavily, ddg, quota, max_results](
                   const nlohmann::json& args) -> std::expected<std::string, Error> {
        auto q = json::get_string(args, "query");
        if (!q) return std::unexpected(q.error());
        if (q->empty()) return std::unexpected(Error{.msg = "query must not be empty", .code = 0});

        Router router;
        router.primary = searxng.get();
        // Order: premium credential-gated tiers first (z.ai, then Tavily under
        // its monthly quota), then the keyless DuckDuckGo last resort.
        if (zai) router.fallbacks.push_back({zai.get(), nullptr});
        if (tavily) router.fallbacks.push_back({tavily.get(), quota.get()});
        router.fallbacks.push_back({ddg.get(), nullptr});  // keyless final fallback
        router.max_results = max_results;

        auto res = router.search(*q);
        if (!res) return std::unexpected(res.error());
        return format_results(*res);
    };
    return Tool{.spec = std::move(spec), .run = std::move(run)};
}

// --- web_fetch --------------------------------------------------------------

Tool web_fetch_tool(FetchConfig cfg) {
    ToolSpec spec{
        "web_fetch",
        "Fetch one http(s) URL, return response body. Body truncated when large. "
        "Use to read specific page found via web_search.",
        nlohmann::json{
            {"type", "object"},
            {"properties",
             {{"url", {{"type", "string"}, {"description", "http(s) URL to fetch"}}},
              {"max_bytes",
               {{"type", "integer"},
                {"description",
                 "cap on returned body bytes; clamped to tool ceiling"}}}}},
            {"required", nlohmann::json::array({"url"})}}};

    auto run = [cfg](const nlohmann::json& args) -> std::expected<std::string, Error> {
        auto url = json::get_string(args, "url");
        if (!url) return std::unexpected(url.error());
        if (!url->starts_with("http://") && !url->starts_with("https://"))
            return std::unexpected(Error{.msg = "url must be http:// or https://", .code = 0});

        std::size_t cap = cfg.max_bytes;  // ceiling; per-call arg may only lower it
        if (auto it = args.find("max_bytes"); it != args.end() && !it->is_null()) {
            if (!it->is_number_integer())
                return std::unexpected(Error{.msg = "max_bytes must be an integer", .code = 0});
            auto n = it->get<std::int64_t>();
            if (n <= 0) return std::unexpected(Error{.msg = "max_bytes must be positive", .code = 0});
            cap = std::min(cap, static_cast<std::size_t>(n));
        }

        auto resp = http::get(*url, {}, cfg.timeout_secs);
        if (!resp) return std::unexpected(resp.error());
        if (resp->status != 200)
            return std::unexpected(
                Error{.msg = "web_fetch HTTP " + std::to_string(resp->status),
                    .code = static_cast<int>(resp->status)});

        std::string body = std::move(resp->body);
        if (body.size() > cap) {
            body.resize(cap);
            body += "\n\n[truncated]";
        }
        return body;
    };
    return Tool{.spec = std::move(spec), .run = std::move(run)};
}

}  // namespace moocode
