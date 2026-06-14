#include "agent/search.hpp"

#include "agent/search_internal.hpp"

#include <filesystem>

#include "test_harness.hpp"

using namespace moocode;

namespace {

std::vector<SearchResult> one(const std::string& title, const std::string& url) {
    return {SearchResult{.title = title, .url = url, .snippet = "snippet"}};
}

// A SearchBackend returning a canned outcome, counting its invocations.
struct FakeBackend : SearchBackend {
    std::expected<std::vector<SearchResult>, Error> out;
    std::string nm;
    int calls = 0;
    FakeBackend(std::string n, std::expected<std::vector<SearchResult>, Error> o)
        : out(std::move(o)), nm(std::move(n)) {}
    std::expected<std::vector<SearchResult>, Error> search(std::string_view,
                                                           int) override {
        ++calls;
        return out;
    }
    std::string_view name() const override { return nm; }
};

std::filesystem::path tmp_quota() {
    static int n = 0;
    auto p = std::filesystem::temp_directory_path() /
             ("moocode_quota_test_" + std::to_string(++n) + ".json");
    std::filesystem::remove(p);
    return p;
}

}  // namespace

// --- SearXNG parsing --------------------------------------------------------

TEST("parse_searxng: maps results array to title/url/snippet") {
    auto body = nlohmann::json::parse(R"({"results":[
        {"title":"RAII","url":"https://x/raii","content":"resource mgmt"},
        {"title":"Move","url":"https://x/move","content":"semantics"}]})");
    auto r = parse_searxng_results(body);
    CHECK(r.has_value());
    if (r) {
        CHECK_EQ(r->size(), size_t{2});
        CHECK_EQ((*r)[0].title, std::string("RAII"));
        CHECK_EQ((*r)[0].url, std::string("https://x/raii"));
        CHECK_EQ((*r)[0].snippet, std::string("resource mgmt"));
    }
}

TEST("parse_searxng: absent results array is an Error") {
    auto r = parse_searxng_results(nlohmann::json::parse(R"({"query":"x"})"));
    CHECK(!r.has_value());
}

TEST("parse_searxng: missing fields tolerated as empty strings") {
    auto body = nlohmann::json::parse(R"({"results":[{"url":"https://y"}]})");
    auto r = parse_searxng_results(body);
    CHECK(r.has_value());
    if (r) {
        CHECK_EQ(r->size(), size_t{1});
        CHECK_EQ((*r)[0].title, std::string(""));
        CHECK_EQ((*r)[0].url, std::string("https://y"));
    }
}

TEST("parse_searxng: empty results array is an empty (ok) vector") {
    auto r = parse_searxng_results(nlohmann::json::parse(R"({"results":[]})"));
    CHECK(r.has_value());
    if (r) CHECK(r->empty());
}

// --- Tavily parsing ---------------------------------------------------------

TEST("parse_tavily: maps results array to title/url/snippet") {
    auto body = nlohmann::json::parse(R"({"results":[
        {"title":"T","url":"https://t","content":"body"}]})");
    auto r = parse_tavily_results(body);
    CHECK(r.has_value());
    if (r) {
        CHECK_EQ(r->size(), size_t{1});
        CHECK_EQ((*r)[0].title, std::string("T"));
        CHECK_EQ((*r)[0].snippet, std::string("body"));
    }
}

TEST("parse_tavily: absent results array is an Error") {
    auto r = parse_tavily_results(nlohmann::json::parse(R"({"answer":"hi"})"));
    CHECK(!r.has_value());
}

// --- URL / body builders ----------------------------------------------------

TEST("searxng_search_url: encodes the query and asks for JSON") {
    auto u = searxng_search_url("http://localhost:8080", "c++ raii", 5);
    CHECK(u.find("http://localhost:8080/search?") != std::string::npos);
    CHECK(u.find("q=c%2B%2B%20raii") != std::string::npos);
    CHECK(u.find("format=json") != std::string::npos);
}

TEST("tavily_request_body: is valid JSON carrying the query") {
    auto b = tavily_request_body("hello world", 5);
    auto j = nlohmann::json::parse(b);
    CHECK_EQ(j.value("query", std::string()), std::string("hello world"));
    CHECK_EQ(j.value("max_results", 0), 5);
}

// --- DuckDuckGo HTML scraping (offline fixture; never hits the network) ------

namespace {
// Two result blocks in the real html.duckduckgo.com/html/ shape: href appears
// AFTER class="result__a" inside the <a> tag, and the URL is a /l/?uddg= redirect.
constexpr std::string_view kDdgHtml =
    "<div class=\"result results_links results_links_deep web-result \">"
    "<h2 class=\"result__title\">"
    "<a rel=\"nofollow\" class=\"result__a\" "
    "href=\"//duckduckgo.com/l/?uddg=https%3A%2F%2Fen.cppreference.com%2Fcpp%2Flanguage%2Fraii&amp;rut=aa\">"
    "RAII - cppreference.com</a></h2>"
    "<a class=\"result__snippet\" href=\"x\">RAII binds a resource to an object's lifetime.</a>"
    "</div>"
    "<div class=\"result results_links results_links_deep web-result \">"
    "<h2 class=\"result__title\">"
    "<a rel=\"nofollow\" class=\"result__a\" "
    "href=\"//duckduckgo.com/l/?uddg=https%3A%2F%2Fen.wikipedia.org%2Fwiki%2FRAII&amp;rut=bb\">"
    "RAII - Wikipedia</a></h2>"
    "<a class=\"result__snippet\" href=\"y\">Resource acquisition is initialization.</a>"
    "</div>";
}  // namespace

TEST("ddg html: parses real-format blocks with href after the class attr") {
    auto r = parse_duckduckgo_html(kDdgHtml, 5);
    CHECK(r.has_value());
    if (r) {
        CHECK_EQ(r->size(), size_t{2});
        if (r->size() == 2) {
            CHECK_EQ((*r)[0].title, std::string("RAII - cppreference.com"));
            CHECK_EQ((*r)[0].url, std::string("https://en.cppreference.com/cpp/language/raii"));
            CHECK_EQ((*r)[0].snippet,
                     std::string("RAII binds a resource to an object's lifetime."));
            CHECK_EQ((*r)[1].url, std::string("https://en.wikipedia.org/wiki/RAII"));
        }
    }
}

TEST("ddg html: honours max_results") {
    auto r = parse_duckduckgo_html(kDdgHtml, 1);
    CHECK(r.has_value());
    if (r) CHECK_EQ(r->size(), size_t{1});
}

TEST("ddg html: no result blocks yields an empty (ok) vector") {
    auto r = parse_duckduckgo_html("<html><body>nothing here</body></html>", 5);
    CHECK(r.has_value());
    if (r) CHECK(r->empty());
}

// --- format_results ---------------------------------------------------------

TEST("format_results: empty list says so") {
    CHECK(format_results({}).find("o results") != std::string::npos);
}

TEST("format_results: includes title and url") {
    auto s = format_results(one("MyTitle", "https://example/x"));
    CHECK(s.find("MyTitle") != std::string::npos);
    CHECK(s.find("https://example/x") != std::string::npos);
}

// --- current_month ----------------------------------------------------------

TEST("current_month: formatted as YYYY-MM") {
    auto m = current_month();
    CHECK_EQ(m.size(), size_t{7});
    CHECK_EQ(m[4], '-');
}

// --- MonthlyQuota -----------------------------------------------------------

TEST("quota: charges up to the limit then refuses") {
    auto f = tmp_quota();
    MonthlyQuota q(f, 2);
    CHECK(q.charge("2026-06"));
    CHECK(q.charge("2026-06"));
    CHECK(!q.charge("2026-06"));  // limit reached
    CHECK_EQ(q.used("2026-06"), 2);
    std::filesystem::remove(f);
}

TEST("quota: a new month resets the counter") {
    auto f = tmp_quota();
    MonthlyQuota q(f, 2);
    CHECK(q.charge("2026-06"));
    CHECK(q.charge("2026-06"));
    CHECK(!q.charge("2026-06"));
    CHECK(q.charge("2026-07"));      // rolled over → fresh budget
    CHECK_EQ(q.used("2026-07"), 1);
    CHECK_EQ(q.used("2026-06"), 0);  // not the current period
    std::filesystem::remove(f);
}

TEST("quota: persists across reconstruction within the same month") {
    auto f = tmp_quota();
    {
        MonthlyQuota q(f, 5);
        CHECK(q.charge("2026-06"));
        CHECK(q.charge("2026-06"));
    }
    MonthlyQuota q2(f, 5);  // reload from disk
    CHECK_EQ(q2.used("2026-06"), 2);
    CHECK(q2.charge("2026-06"));
    CHECK_EQ(q2.used("2026-06"), 3);
    std::filesystem::remove(f);
}

// --- Router -----------------------------------------------------------------

TEST("router: primary success returns it without touching fallback") {
    FakeBackend primary("searxng", one("p", "https://p"));
    FakeBackend fallback("tavily", one("f", "https://f"));
    Router r{&primary, &fallback, nullptr, nullptr, [] { return "2026-06"; }, 5};
    auto out = r.search("q");
    CHECK(out.has_value());
    if (out) CHECK_EQ((*out)[0].url, std::string("https://p"));
    CHECK_EQ(fallback.calls, 0);
}

TEST("router: primary error falls back to tavily") {
    FakeBackend primary("searxng", std::unexpected(Error{.msg = "down", .code = 0}));
    FakeBackend fallback("tavily", one("f", "https://f"));
    Router r{&primary, &fallback, nullptr, nullptr, [] { return "2026-06"; }, 5};
    auto out = r.search("q");
    CHECK(out.has_value());
    if (out) CHECK_EQ((*out)[0].url, std::string("https://f"));
    CHECK_EQ(fallback.calls, 1);
}

TEST("router: primary empty falls back to tavily") {
    FakeBackend primary("searxng", std::vector<SearchResult>{});
    FakeBackend fallback("tavily", one("f", "https://f"));
    Router r{&primary, &fallback, nullptr, nullptr, [] { return "2026-06"; }, 5};
    auto out = r.search("q");
    CHECK(out.has_value());
    if (out) CHECK_EQ((*out)[0].url, std::string("https://f"));
    CHECK_EQ(fallback.calls, 1);
}

TEST("router: no fallback configured returns the primary outcome") {
    FakeBackend primary("searxng", std::unexpected(Error{.msg = "down", .code = 0}));
    Router r{&primary, nullptr, nullptr, nullptr, [] { return "2026-06"; }, 5};
    auto out = r.search("q");
    CHECK(!out.has_value());
}

TEST("router: exhausted quota skips the fallback") {
    auto f = tmp_quota();
    MonthlyQuota q(f, 1);
    CHECK(q.charge("2026-06"));  // budget now spent
    FakeBackend primary("searxng", std::unexpected(Error{.msg = "down", .code = 0}));
    FakeBackend fallback("tavily", one("f", "https://f"));
    Router r{&primary, &fallback, nullptr, &q, [] { return "2026-06"; }, 5};
    auto out = r.search("q");
    CHECK(!out.has_value());       // fell through to primary's error
    CHECK_EQ(fallback.calls, 0);   // never charged/called
    std::filesystem::remove(f);
}

TEST("router: using the fallback consumes one quota unit") {
    auto f = tmp_quota();
    MonthlyQuota q(f, 10);
    FakeBackend primary("searxng", std::vector<SearchResult>{});
    FakeBackend fallback("tavily", one("f", "https://f"));
    Router r{&primary, &fallback, nullptr, &q, [] { return "2026-06"; }, 5};
    auto out = r.search("q");
    CHECK(out.has_value());
    CHECK_EQ(fallback.calls, 1);
    CHECK_EQ(q.used("2026-06"), 1);
    std::filesystem::remove(f);
}
