#include "agent/gitea.hpp"

#include <map>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "agent/gitea_internal.hpp"
#include "agent/http_detail.hpp"

#include "test_harness.hpp"

using namespace moocode;
using nlohmann::json;

namespace {

// Canned-response HTTP transport keyed by full URL; records every request.
struct FakeHttp {
    std::map<std::string, http::Response> routes;
    std::vector<std::string> urls;
    std::vector<std::vector<std::string>> headers;

    void route(std::string url, std::string body, long status = 200) {
        routes[std::move(url)] = http::Response{status, std::move(body)};
    }

    HttpGetFn fn() {
        return [this](const std::string& url, const std::vector<std::string>& h,
                      long) -> std::expected<http::Response, Error> {
            urls.push_back(url);
            headers.push_back(h);
            auto it = routes.find(url);
            if (it == routes.end())
                return http::Response{404, R"({"message":"not found"})"};
            return it->second;
        };
    }
};

GiteaConfig cfg() {
    GiteaConfig c;
    c.base_url = "https://git.example.com";
    return c;
}

const Tool* find_tool(const std::vector<Tool>& tools, std::string_view name) {
    for (const Tool& t : tools)
        if (t.spec.name == name) return &t;
    return nullptr;
}

constexpr const char* kApi = "https://git.example.com/api/v1";

}  // namespace

// --- pure helpers -------------------------------------------------------------

TEST("gitea_api_base: appends /api/v1 and strips trailing slashes") {
    CHECK_EQ(gitea_api_base("https://g.io"), "https://g.io/api/v1");
    CHECK_EQ(gitea_api_base("https://g.io///"), "https://g.io/api/v1");
    CHECK_EQ(gitea_api_base("https://g.io/api/v1"), "https://g.io/api/v1");
    CHECK_EQ(gitea_api_base("https://g.io/api/v1/"), "https://g.io/api/v1");
}

TEST("gitea_error_message: keeps Gitea's JSON message field") {
    CHECK_EQ(gitea_error_message(404, R"({"message":"user does not exist"})"),
             "gitea: HTTP 404: user does not exist");
    CHECK_EQ(gitea_error_message(500, "<html>oops</html>"), "gitea: HTTP 500");
    CHECK_EQ(gitea_error_message(403, "{}"), "gitea: HTTP 403");
}

// --- formatters ----------------------------------------------------------------

TEST("format_repo_list: one numbered line per repo with flags") {
    json repos = json::array(
        {{{"full_name", "me/tools"},
          {"stars_count", 3},
          {"forks_count", 1},
          {"private", true},
          {"description", "line one\nline two"}},
         {{"full_name", "me/web"}, {"archived", true}, {"fork", true}}});
    const std::string out = format_repo_list(repos);
    CHECK(out.find("1. me/tools  *3 forks:1 [private]") != std::string::npos);
    CHECK(out.find("line one…") != std::string::npos);   // one_line clips at \n
    CHECK(out.find("line two") == std::string::npos);
    CHECK(out.find("2. me/web  *0 forks:0 [archived] [fork]") != std::string::npos);
    CHECK_EQ(format_repo_list(json::array()), "No repositories found.");
    CHECK_EQ(format_repo_list(json::object()), "No repositories found.");
}

TEST("format_repo: metadata plus branch list") {
    json repo = {{"full_name", "me/tools"},     {"description", "desc"},
                 {"default_branch", "main"},    {"private", false},
                 {"stars_count", 7},            {"open_pr_counter", 2},
                 {"created_at", "2024-01-02T10:00:00Z"},
                 {"updated_at", "2026-06-01T10:00:00Z"},
                 {"clone_url", "https://g.io/me/tools.git"}};
    json branches = json::array(
        {{{"name", "main"},
          {"commit", {{"id", "0123456789abcdef"}}},
          {"protected", true}},
         {{"name", "dev"}}});
    const std::string out = format_repo(repo, branches);
    CHECK(out.find("repo: me/tools") != std::string::npos);
    CHECK(out.find("default branch: main") != std::string::npos);
    CHECK(out.find("visibility: public") != std::string::npos);
    CHECK(out.find("open PRs: 2") != std::string::npos);
    CHECK(out.find("created: 2024-01-02  updated: 2026-06-01") != std::string::npos);
    CHECK(out.find("branches (2):") != std::string::npos);
    CHECK(out.find("  main  0123456789  [protected]") != std::string::npos);
    CHECK(out.find("  dev") != std::string::npos);
}

TEST("format_pr_list: number, state, title, author, refs") {
    json prs = json::array(
        {{{"number", 12},
          {"state", "open"},
          {"draft", true},
          {"title", "Add gitea tools"},
          {"user", {{"login", "manuel"}}},
          {"head", {{"ref", "feature"}}},
          {"base", {{"ref", "main"}}},
          {"updated_at", "2026-06-09T08:00:00Z"}}});
    const std::string out = format_pr_list(prs);
    CHECK(out.find("1. #12 [open, draft] Add gitea tools "
                   "(manuel, feature -> main, updated 2026-06-09)") !=
          std::string::npos);
    CHECK_EQ(format_pr_list(json::array()), "No pull requests found.");
}

TEST("format_pr: body, changed files, comments") {
    json pr = {{"number", 7},
               {"title", "Fix crash"},
               {"state", "closed"},
               {"merged", true},
               {"merged_at", "2026-05-30T12:00:00Z"},
               {"merged_by", {{"login", "boss"}}},
               {"user", {{"login", "manuel"}}},
               {"base", {{"ref", "main"}, {"sha", "aaaabbbbccccdddd"}}},
               {"head", {{"ref", "fix"}, {"sha", "1111222233334444"}}},
               {"labels", json::array({{{"name", "bug"}}, {{"name", "P1"}}})},
               {"body", "It crashed."},
               {"created_at", "2026-05-29T12:00:00Z"},
               {"updated_at", "2026-05-30T12:00:00Z"}};
    json files = json::array({{{"filename", "src/a.cpp"},
                               {"status", "changed"},
                               {"additions", 5},
                               {"deletions", 2}}});
    json comments = json::array({{{"user", {{"login", "rev"}}},
                                  {"created_at", "2026-05-29T13:00:00Z"},
                                  {"body", "LGTM"}}});
    const std::string out = format_pr(pr, files, comments);
    CHECK(out.find("PR #7: Fix crash") != std::string::npos);
    CHECK(out.find("state: closed, merged 2026-05-30 by boss") != std::string::npos);
    CHECK(out.find("base: main (aaaabbbbcc)  head: fix (1111222233)") !=
          std::string::npos);
    CHECK(out.find("labels: bug, P1") != std::string::npos);
    CHECK(out.find("It crashed.") != std::string::npos);
    CHECK(out.find("changed files (1):") != std::string::npos);
    CHECK(out.find("  changed src/a.cpp (+5/-2)") != std::string::npos);
    CHECK(out.find("[rev @ 2026-05-29] LGTM") != std::string::npos);
}

TEST("format_commit_list: sha, date, author, first line") {
    json commits = json::array(
        {{{"sha", "0123456789abcdef"},
          {"author", {{"login", "manuel"}}},
          {"commit",
           {{"message", "fix: thing\n\nlong body"},
            {"author", {{"name", "Manuel"}, {"date", "2026-06-08T09:00:00Z"}}}}}}});
    const std::string out = format_commit_list(commits);
    CHECK(out.find("0123456789  2026-06-08  manuel  fix: thing…") !=
          std::string::npos);
    CHECK(out.find("long body") == std::string::npos);
    CHECK_EQ(format_commit_list(json::array()), "No commits found.");
}

TEST("format_commit: full message, parents, stats, files") {
    json commit = {
        {"sha", "0123456789abcdef"},
        {"parents", json::array({{{"sha", "fedcba9876543210"}}})},
        {"stats", {{"additions", 10}, {"deletions", 3}}},
        {"commit",
         {{"message", "fix: thing\n\nbody text"},
          {"author",
           {{"name", "Manuel"}, {"email", "m@x.de"}, {"date", "2026-06-08T09:00:00Z"}}}}},
        {"files", json::array({{{"filename", "a.cpp"}, {"status", "modified"}}})}};
    const std::string out = format_commit(commit);
    CHECK(out.find("commit 0123456789abcdef") != std::string::npos);
    CHECK(out.find("author: Manuel <m@x.de>  2026-06-08") != std::string::npos);
    CHECK(out.find("parents: fedcba9876") != std::string::npos);
    CHECK(out.find("stats: +10 -3") != std::string::npos);
    CHECK(out.find("body text") != std::string::npos);
    CHECK(out.find("  modified a.cpp") != std::string::npos);
}

TEST("format_dir_listing: directories first, deterministic order") {
    json entries = json::array({{{"name", "z.txt"}, {"path", "z.txt"}, {"type", "file"}, {"size", 12}},
                                {{"name", "src"}, {"path", "src"}, {"type", "dir"}},
                                {{"name", "a.txt"}, {"path", "a.txt"}, {"type", "file"}, {"size", 1}},
                                {{"name", "ln"}, {"path", "ln"}, {"type", "symlink"}}});
    const std::string out = format_dir_listing(entries);
    CHECK_EQ(out, "src/\na.txt  (1 B)\nln  [symlink]\nz.txt  (12 B)\n");
    CHECK_EQ(format_dir_listing(json::array()), "Empty directory.");
}

// --- client ----------------------------------------------------------------------

TEST("GiteaClient: sends token header, decodes error message") {
    FakeHttp fake;
    fake.route(std::string(kApi) + "/repos/me/tools", R"({"full_name":"me/tools"})");
    GiteaConfig c = cfg();
    c.token = "secret";
    GiteaClient client(c, fake.fn());

    auto ok = client.get_json("/repos/me/tools");
    CHECK(ok.has_value());
    CHECK_EQ((*ok)["full_name"], "me/tools");
    CHECK_EQ(fake.headers.size(), std::size_t{1});
    CHECK_EQ(fake.headers[0].size(), std::size_t{1});
    CHECK_EQ(fake.headers[0][0], "Authorization: token secret");

    auto missing = client.get_json("/repos/me/nope");
    CHECK(!missing.has_value());
    CHECK_EQ(missing.error().msg, "gitea: HTTP 404: not found");
    CHECK_EQ(missing.error().code, 404);
}

TEST("GiteaClient::get_paged: walks pages, honors limit and unwrap") {
    FakeHttp fake;
    json page1 = json::array();
    for (int i = 0; i < 50; ++i) page1.push_back({{"id", i}});
    json page2 = json::array({{{"id", 50}}, {{"id", 51}}});
    fake.route(std::string(kApi) + "/repos/me/t/pulls?state=open&page=1&limit=50",
               page1.dump());
    fake.route(std::string(kApi) + "/repos/me/t/pulls?state=open&page=2&limit=50",
               page2.dump());
    GiteaClient client(cfg(), fake.fn());

    auto all = client.get_paged("/repos/me/t/pulls", "state=open", 200);
    CHECK(all.has_value());
    CHECK_EQ(all->size(), std::size_t{52});
    CHECK_EQ((*all)[51]["id"], 51);

    auto capped = client.get_paged("/repos/me/t/pulls", "state=open", 10);
    CHECK(capped.has_value());
    CHECK_EQ(capped->size(), std::size_t{10});

    // unwrap: /repos/search style {"ok":true,"data":[...]}
    fake.route(std::string(kApi) + "/repos/search?page=1&limit=50",
               R"({"ok":true,"data":[{"full_name":"me/t"}]})");
    auto search = client.get_paged("/repos/search", "", 30, "data");
    CHECK(search.has_value());
    CHECK_EQ(search->size(), std::size_t{1});

    // shape mismatch: array expected but object served
    fake.route(std::string(kApi) + "/weird?page=1&limit=50", R"({"x":1})");
    auto weird = client.get_paged("/weird", "", 30);
    CHECK(!weird.has_value());
}

// --- tools -------------------------------------------------------------------------

TEST("gitea_tools: registers the read-only tool set") {
    FakeHttp fake;
    auto tools = gitea_tools(cfg(), fake.fn());
    for (const char* name :
         {"gitea_repos", "gitea_repo", "gitea_prs", "gitea_pr", "gitea_pr_diff",
          "gitea_commits", "gitea_commit", "gitea_file", "gitea_recent_commits"})
        CHECK(find_tool(tools, name) != nullptr);
    CHECK_EQ(tools.size(), std::size_t{9});
}

TEST("gitea_repos: owner routes to /users/<owner>/repos, else /repos/search") {
    FakeHttp fake;
    fake.route(std::string(kApi) + "/users/me/repos?page=1&limit=50",
               R"([{"full_name":"me/tools","stars_count":1}])");
    fake.route(std::string(kApi) + "/repos/search?q=web&page=1&limit=50",
               R"({"ok":true,"data":[{"full_name":"you/web"}]})");
    auto tools = gitea_tools(cfg(), fake.fn());
    const Tool* t = find_tool(tools, "gitea_repos");

    auto mine = t->run({{"owner", "me"}});
    CHECK(mine.has_value());
    CHECK(mine->find("me/tools") != std::string::npos);

    auto searched = t->run({{"query", "web"}});
    CHECK(searched.has_value());
    CHECK(searched->find("you/web") != std::string::npos);
}

TEST("gitea_prs: validates state, missing owner is an Error") {
    FakeHttp fake;
    auto tools = gitea_tools(cfg(), fake.fn());
    const Tool* t = find_tool(tools, "gitea_prs");
    auto bad_state = t->run({{"owner", "me"}, {"repo", "t"}, {"state", "merged"}});
    CHECK(!bad_state.has_value());
    auto no_owner = t->run({{"repo", "t"}});
    CHECK(!no_owner.has_value());
    CHECK_EQ(fake.urls.size(), std::size_t{0});  // rejected before any request
}

TEST("gitea_pr: combines metadata, files, and comments") {
    FakeHttp fake;
    fake.route(std::string(kApi) + "/repos/me/t/pulls/3",
               R"({"number":3,"title":"T","state":"open","user":{"login":"u"}})");
    fake.route(std::string(kApi) + "/repos/me/t/pulls/3/files?page=1&limit=50",
               R"([{"filename":"f.cpp","status":"added","additions":1,"deletions":0}])");
    fake.route(std::string(kApi) + "/repos/me/t/issues/3/comments?page=1&limit=50",
               R"([{"user":{"login":"rev"},"body":"nice","created_at":"2026-06-01T00:00:00Z"}])");
    auto tools = gitea_tools(cfg(), fake.fn());
    auto out = find_tool(tools, "gitea_pr")->run({{"owner", "me"}, {"repo", "t"}, {"number", 3}});
    CHECK(out.has_value());
    CHECK(out->find("PR #3: T") != std::string::npos);
    CHECK(out->find("  added f.cpp (+1/-0)") != std::string::npos);
    CHECK(out->find("[rev @ 2026-06-01] nice") != std::string::npos);
}

TEST("gitea_pr_diff: returns raw diff, max_bytes truncates") {
    FakeHttp fake;
    fake.route(std::string(kApi) + "/repos/me/t/pulls/3.diff",
               "diff --git a/f b/f\n+12345678901234567890\n");
    auto tools = gitea_tools(cfg(), fake.fn());
    const Tool* t = find_tool(tools, "gitea_pr_diff");

    auto full = t->run({{"owner", "me"}, {"repo", "t"}, {"number", 3}});
    CHECK(full.has_value());
    CHECK(full->find("+12345678901234567890") != std::string::npos);

    auto cut = t->run({{"owner", "me"}, {"repo", "t"}, {"number", 3}, {"max_bytes", 10}});
    CHECK(cut.has_value());
    CHECK(cut->starts_with("diff --git"));
    CHECK(cut->find("[truncated: showing 10 of") != std::string::npos);
}

TEST("gitea_commits: ref and path become query params") {
    FakeHttp fake;
    fake.route(std::string(kApi) +
                   "/repos/me/t/commits?stat=false&verification=false&files=false"
                   "&sha=dev&path=src%2Fa.cpp&page=1&limit=50",
               R"([{"sha":"0123456789ab","commit":{"message":"m",
                    "author":{"name":"A","date":"2026-06-01T00:00:00Z"}}}])");
    auto tools = gitea_tools(cfg(), fake.fn());
    auto out = find_tool(tools, "gitea_commits")
                   ->run({{"owner", "me"}, {"repo", "t"}, {"ref", "dev"},
                          {"path", "src/a.cpp"}});
    CHECK(out.has_value());
    CHECK(out->find("0123456789  2026-06-01  A  m") != std::string::npos);
}

TEST("gitea_commit: optional diff is appended") {
    FakeHttp fake;
    fake.route(std::string(kApi) + "/repos/me/t/git/commits/abc?stat=true",
               R"({"sha":"abc","commit":{"message":"msg",
                    "author":{"name":"A","date":"2026-06-01T00:00:00Z"}}})");
    fake.route(std::string(kApi) + "/repos/me/t/git/commits/abc.diff",
               "diff --git a/x b/x\n");
    auto tools = gitea_tools(cfg(), fake.fn());
    const Tool* t = find_tool(tools, "gitea_commit");

    auto plain = t->run({{"owner", "me"}, {"repo", "t"}, {"sha", "abc"}});
    CHECK(plain.has_value());
    CHECK(plain->find("diff --git") == std::string::npos);

    auto with = t->run({{"owner", "me"}, {"repo", "t"}, {"sha", "abc"}, {"diff", true}});
    CHECK(with.has_value());
    CHECK(with->find("commit abc") != std::string::npos);
    CHECK(with->find("diff --git a/x b/x") != std::string::npos);
}

TEST("gitea_file: directory listing, raw file, binary guard") {
    FakeHttp fake;
    fake.route(std::string(kApi) + "/repos/me/t/contents/",
               R"([{"name":"src","path":"src","type":"dir"}])");
    fake.route(std::string(kApi) + "/repos/me/t/contents/src/a.cpp?ref=dev",
               R"({"type":"file","path":"src/a.cpp"})");
    fake.route(std::string(kApi) + "/repos/me/t/raw/src/a.cpp?ref=dev",
               "int main() {}\n");
    fake.route(std::string(kApi) + "/repos/me/t/contents/blob.bin",
               R"({"type":"file","path":"blob.bin"})");
    fake.route(std::string(kApi) + "/repos/me/t/raw/blob.bin",
               std::string("\x7f""ELF\0\0junk", 9));
    auto tools = gitea_tools(cfg(), fake.fn());
    const Tool* t = find_tool(tools, "gitea_file");

    auto root = t->run({{"owner", "me"}, {"repo", "t"}});
    CHECK(root.has_value());
    CHECK_EQ(*root, "src/\n");

    auto file = t->run({{"owner", "me"}, {"repo", "t"}, {"path", "src/a.cpp"}, {"ref", "dev"}});
    CHECK(file.has_value());
    CHECK_EQ(*file, "int main() {}\n");

    auto bin = t->run({{"owner", "me"}, {"repo", "t"}, {"path", "blob.bin"}});
    CHECK(bin.has_value());
    CHECK_EQ(*bin, "binary file (9 bytes)");
}

// --- per-call url + token pinning ---------------------------------------------

TEST("gitea_origin: scheme://host[:port], lowercased") {
    CHECK_EQ(gitea_origin("https://Git.Example.com/api/v1"), "https://git.example.com");
    CHECK_EQ(gitea_origin("http://g.io:3000/x?y#z"), "http://g.io:3000");
    CHECK_EQ(gitea_origin("https://g.io"), "https://g.io");
    CHECK_EQ(gitea_origin("not a url"), "");
}

TEST("url arg: overrides the configured instance") {
    FakeHttp fake;
    fake.route("https://other.example.com/api/v1/repos/me/tools",
               R"({"full_name":"me/tools"})");
    fake.route("https://other.example.com/api/v1/repos/me/tools/branches?page=1&limit=50",
               "[]");
    auto tools = gitea_tools(cfg(), fake.fn());
    auto out = find_tool(tools, "gitea_repo")
                   ->run({{"url", "https://other.example.com"},
                          {"owner", "me"}, {"repo", "tools"}});
    CHECK(out.has_value());
    CHECK(out->find("repo: me/tools") != std::string::npos);
}

TEST("url arg: token pinned to the configured origin") {
    FakeHttp fake;
    fake.route(std::string(kApi) + "/repos/me/t", R"({"full_name":"me/t"})");
    fake.route(std::string(kApi) + "/repos/me/t/branches?page=1&limit=50", "[]");
    // requests go out with the per-call url verbatim; only origin matching is
    // case-insensitive
    fake.route("https://GIT.example.com/api/v1/repos/me/t", R"({"full_name":"me/t"})");
    fake.route("https://GIT.example.com/api/v1/repos/me/t/branches?page=1&limit=50", "[]");
    fake.route("https://evil.example.com/api/v1/repos/me/t", R"({"full_name":"me/t"})");
    fake.route("https://evil.example.com/api/v1/repos/me/t/branches?page=1&limit=50", "[]");
    GiteaConfig c = cfg();
    c.token = "secret";
    auto tools = gitea_tools(c, fake.fn());
    const Tool* t = find_tool(tools, "gitea_repo");

    // same origin (also via explicit url, case-insensitive host): token sent
    CHECK(t->run({{"owner", "me"}, {"repo", "t"}}).has_value());
    CHECK(t->run({{"url", "https://GIT.example.com"}, {"owner", "me"}, {"repo", "t"}})
              .has_value());
    // foreign origin: anonymous
    CHECK(t->run({{"url", "https://evil.example.com"}, {"owner", "me"}, {"repo", "t"}})
              .has_value());
    CHECK_EQ(fake.headers.size(), std::size_t{6});  // 3 calls x (repo + branches)
    for (std::size_t i = 0; i < 4; ++i) {
        CHECK_EQ(fake.headers[i].size(), std::size_t{1});
        CHECK_EQ(fake.headers[i][0], "Authorization: token secret");
    }
    for (std::size_t i = 4; i < 6; ++i) CHECK(fake.headers[i].empty());
}

TEST("url arg: required when no default instance is configured") {
    FakeHttp fake;
    fake.route("https://adhoc.example.com/api/v1/repos/me/t/pulls/1.diff", "diff x\n");
    auto tools = gitea_tools(GiteaConfig{}, fake.fn());
    const Tool* t = find_tool(tools, "gitea_pr_diff");

    auto missing = t->run({{"owner", "me"}, {"repo", "t"}, {"number", 1}});
    CHECK(!missing.has_value());
    CHECK(missing.error().msg.find("MOOCODE_GITEA_URL") != std::string::npos);

    auto bad = t->run({{"url", "ftp://x"}, {"owner", "me"}, {"repo", "t"}, {"number", 1}});
    CHECK(!bad.has_value());

    auto ok = t->run({{"url", "https://adhoc.example.com"},
                      {"owner", "me"}, {"repo", "t"}, {"number", 1}});
    CHECK(ok.has_value());
    CHECK_EQ(*ok, "diff x\n");
}

// --- basic auth (MOOCODE_GITEA_AUTH) --------------------------------------------

TEST("parse_gitea_auth: .env keys, quotes, comments, CRLF") {
    auto a = parse_gitea_auth("GITEA_USER=manuel\nGITEA_PASS=s3cret\n");
    CHECK_EQ(a.user, "manuel");
    CHECK_EQ(a.pass, "s3cret");

    a = parse_gitea_auth("# comment\r\n  GITEA_USER = \"man=uel\" \r\n\r\n"
                         "GITEA_PASS='p w'\nOTHER=x\n");
    CHECK_EQ(a.user, "man=uel");   // quotes stripped, '=' kept in value
    CHECK_EQ(a.pass, "p w");

    a = parse_gitea_auth("no key value pairs here");
    CHECK_EQ(a.user, "");
    CHECK_EQ(a.pass, "");
    CHECK_EQ(a.url, "");

    a = parse_gitea_auth("GITEA_URL=https://git.skysec.dev\n"
                         "GITEA_USER=manuel\nGITEA_PASS=s3cret\n");
    CHECK_EQ(a.url, "https://git.skysec.dev");
    CHECK_EQ(a.user, "manuel");
    CHECK_EQ(a.pass, "s3cret");
}

TEST("gitea_basic_auth_header: RFC 2617 known vector") {
    CHECK_EQ(gitea_basic_auth_header("Aladdin", "open sesame"),
             "Authorization: Basic QWxhZGRpbjpvcGVuIHNlc2FtZQ==");
    CHECK_EQ(gitea_basic_auth_header("a", ""), "Authorization: Basic YTo=");
}

TEST("basic auth: used when no token, token wins, origin-pinned") {
    FakeHttp fake;
    fake.route(std::string(kApi) + "/repos/me/t", R"({"full_name":"me/t"})");
    fake.route(std::string(kApi) + "/repos/me/t/branches?page=1&limit=50", "[]");
    fake.route("https://evil.example.com/api/v1/repos/me/t", R"({"full_name":"me/t"})");
    fake.route("https://evil.example.com/api/v1/repos/me/t/branches?page=1&limit=50", "[]");
    GiteaConfig c = cfg();
    c.auth_user = "manuel";
    c.auth_pass = "pw";
    {
        auto tools = gitea_tools(c, fake.fn());
        const Tool* t = find_tool(tools, "gitea_repo");
        CHECK(t->run({{"owner", "me"}, {"repo", "t"}}).has_value());
        CHECK(t->run({{"url", "https://evil.example.com"}, {"owner", "me"}, {"repo", "t"}})
                  .has_value());
    }
    CHECK_EQ(fake.headers.size(), std::size_t{4});
    CHECK_EQ(fake.headers[0].size(), std::size_t{1});
    CHECK_EQ(fake.headers[0][0], gitea_basic_auth_header("manuel", "pw"));
    CHECK(fake.headers[2].empty());  // foreign origin: anonymous
    CHECK(fake.headers[3].empty());

    fake.headers.clear();
    c.token = "tok";  // token takes precedence over basic auth
    auto tools = gitea_tools(c, fake.fn());
    CHECK(find_tool(tools, "gitea_repo")->run({{"owner", "me"}, {"repo", "t"}}).has_value());
    CHECK_EQ(fake.headers[0].size(), std::size_t{1});
    CHECK_EQ(fake.headers[0][0], "Authorization: token tok");
}

// --- recent commits -----------------------------------------------------------

TEST("parse_timeframe: m/h/d/w units, rejects junk") {
    CHECK_EQ(parse_timeframe("90m").value().count(), 5400);
    CHECK_EQ(parse_timeframe("36h").value().count(), 36 * 3600);
    CHECK_EQ(parse_timeframe("7d").value().count(), 7 * 86400);
    CHECK_EQ(parse_timeframe("2w").value().count(), 14 * 86400);
    CHECK_EQ(parse_timeframe(" 1d ").value().count(), 86400);  // trimmed

    for (const char* bad : {"", "7", "d", "d7", "7x", "0d", "-3d", "7 d",
                            "1.5d", "9999999999d"})
        CHECK(!parse_timeframe(bad).has_value());
}

TEST("parse_iso8601: UTC, offsets, date-only, fraction; junk is nullopt") {
    CHECK_EQ(parse_iso8601("1970-01-01T00:00:00Z").value(), 0);
    CHECK_EQ(parse_iso8601("1970-01-02T03:04:05Z").value(),
             86400 + 3 * 3600 + 4 * 60 + 5);
    CHECK_EQ(parse_iso8601("1970-01-01T01:00:00+01:00").value(), 0);
    CHECK_EQ(parse_iso8601("1970-01-01T01:00:00+0100").value(), 0);
    CHECK_EQ(parse_iso8601("1969-12-31T23:00:00-01:00").value(), 0);
    CHECK_EQ(parse_iso8601("1970-01-02").value(), 86400);
    CHECK_EQ(parse_iso8601("1970-01-01T00:00:00.123456Z").value(), 0);
    CHECK_EQ(parse_iso8601("1970-01-01 00:00:01Z").value(), 1);
    CHECK_EQ(parse_iso8601("1969-12-31T00:00:00Z").value(), -86400);

    for (const char* bad : {"", "garbage", "1970-13-01T00:00:00Z",
                            "1970-02-30T00:00:00Z", "1970-01-01T24:00:00Z",
                            "1970-01-01T00:00:00Zx", "1970-01-01T00:00",
                            "1970-01-01T00:00:00+25:00"})
        CHECK(!parse_iso8601(bad).has_value());
}

TEST("iso8601_utc: known vector, round-trips through parse_iso8601") {
    CHECK_EQ(iso8601_utc(0), "1970-01-01T00:00:00Z");
    CHECK_EQ(iso8601_utc(1'000'000'000), "2001-09-09T01:46:40Z");
    for (std::int64_t e : {std::int64_t{0}, std::int64_t{86399},
                           std::int64_t{1'781'179'200}})
        CHECK_EQ(parse_iso8601(iso8601_utc(e)).value(), e);
}

TEST("regex_filter: case-insensitive search on a field; bad pattern is Error") {
    const json repos = json::parse(R"([
        {"full_name":"infra/build-agent"},
        {"full_name":"Me/Tools"},
        {"full_name":"you/web"}])");
    auto hit = regex_filter(repos, "full_name", "^infra/.*agent");
    CHECK_EQ(hit.value().size(), std::size_t{1});
    CHECK_EQ(regex_filter(repos, "full_name", "me/").value().size(), std::size_t{1});
    CHECK_EQ(regex_filter(repos, "full_name", ".").value().size(), std::size_t{3});
    CHECK_EQ(regex_filter(json::object(), "full_name", ".").value().size(),
             std::size_t{0});
    CHECK(!regex_filter(repos, "full_name", "[unclosed").has_value());

    const json branches = json::parse(R"([{"name":"main"},{"name":"release-1"}])");
    CHECK_EQ(regex_filter(branches, "name", "^release-").value().size(),
             std::size_t{1});
}

TEST("filter_commits_since: committer date, author fallback, undated kept") {
    const json commits = json::parse(R"([
        {"sha":"new1","commit":{"committer":{"date":"1970-01-03T00:00:00Z"}}},
        {"sha":"old1","commit":{"committer":{"date":"1970-01-01T00:00:00Z"}}},
        {"sha":"auth","commit":{"author":{"date":"1970-01-04T00:00:00Z"}}},
        {"sha":"none"}])");
    const json kept = filter_commits_since(commits, 86400);
    CHECK_EQ(kept.size(), std::size_t{3});
    std::string names;
    for (const json& c : kept) names += c["sha"].get<std::string>() + " ";
    CHECK_EQ(names, "new1 auth none ");
    CHECK_EQ(filter_commits_since(json::object(), 0).size(), std::size_t{0});
}

TEST("format_recent_commits: groups per repo with commit lines") {
    const json groups = json::parse(R"([
        {"repo":"me/hot","commits":[
            {"sha":"abcdef012345","commit":{"message":"Fix the frobnicator",
             "author":{"name":"alice","date":"2001-09-09T00:00:00Z"}}}]},
        {"repo":"you/warm","commits":[]}])");
    const std::string out = format_recent_commits(groups);
    CHECK(out.find("me/hot (1 commit):") != std::string::npos);
    CHECK(out.find("  abcdef0123  2001-09-09  alice  Fix the frobnicator\n") !=
          std::string::npos);
    CHECK(out.find("you/warm (0 commits):") != std::string::npos);
    CHECK_EQ(format_recent_commits(json::array()), "No matching commits found.");

    const json with_branches = json::parse(R"([
        {"repo":"me/hot","branches":["main","dev"],"commits":[{"sha":"a"},{"sha":"b"}]}])");
    CHECK(format_recent_commits(with_branches)
              .find("me/hot (2 commits; branches: main, dev):") != std::string::npos);
}

TEST("gitea_repos: pattern filters client-side, limit applies after") {
    FakeHttp fake;
    fake.route(std::string(kApi) + "/repos/search?page=1&limit=50",
               R"({"ok":true,"data":[{"full_name":"a/tool-one"},
                                     {"full_name":"b/other"},
                                     {"full_name":"c/Tool-Two"}]})");
    auto tools = gitea_tools(cfg(), fake.fn());
    const Tool* t = find_tool(tools, "gitea_repos");

    auto matched = t->run({{"pattern", "tool"}});
    CHECK(matched.has_value());
    CHECK(matched->find("a/tool-one") != std::string::npos);
    CHECK(matched->find("c/Tool-Two") != std::string::npos);  // icase
    CHECK(matched->find("b/other") == std::string::npos);

    auto capped = t->run({{"pattern", "tool"}, {"limit", 1}});
    CHECK(capped.has_value());
    CHECK(capped->find("a/tool-one") != std::string::npos);
    CHECK(capped->find("Tool-Two") == std::string::npos);

    auto bad = t->run({{"pattern", "[unclosed"}});
    CHECK(!bad.has_value());
}

namespace {

// Fixed-clock config for gitea_recent_commits: "now" = 2001-09-09T01:46:40Z.
constexpr std::int64_t kNow = 1'000'000'000;
GiteaConfig recent_cfg() {
    GiteaConfig c = cfg();
    c.now_epoch = [] { return kNow; };
    return c;
}

std::string commits_url(const char* repo, std::int64_t cutoff,
                        const char* branch = "main") {
    return std::string(kApi) + "/repos/" + repo +
           "/commits?stat=false&verification=false&files=false&since=" +
           http::url_encode(iso8601_utc(cutoff)) + "&sha=" + branch +
           "&page=1&limit=50";
}

std::string branches_url(const char* repo) {
    return std::string(kApi) + "/repos/" + repo + "/branches?page=1&limit=50";
}

constexpr const char* kSearchUrl =
    "https://git.example.com/api/v1/repos/search?sort=updated&order=desc&page=1&limit=50";

}  // namespace

TEST("gitea_recent_commits: window + pattern, stale repos pruned, 409 skipped") {
    const std::int64_t cutoff = kNow - 86400;  // timeframe 1d
    FakeHttp fake;
    fake.route(kSearchUrl,
               R"({"ok":true,"data":[
                   {"full_name":"me/hot","updated_at":"2001-09-09T00:00:00Z"},
                   {"full_name":"you/warm","updated_at":"2001-09-08T12:00:00Z"},
                   {"full_name":"me/empty","updated_at":"2001-09-08T11:00:00Z"},
                   {"full_name":"me/stale","updated_at":"2000-01-01T00:00:00Z"}]})");
    fake.route(branches_url("me/hot"), R"([{"name":"main"}])");
    fake.route(branches_url("you/warm"), R"([{"name":"main"}])");
    fake.route(branches_url("me/empty"),
               R"({"message":"Git Repository is empty."})", 409);
    // One commit inside the window, one outside (server ignored `since`).
    fake.route(commits_url("me/hot", cutoff),
               R"([{"sha":"fresh001","commit":{"message":"recent work",
                    "committer":{"date":"2001-09-08T12:00:00Z"},
                    "author":{"name":"alice","date":"2001-09-08T12:00:00Z"}}},
                   {"sha":"ancient0","commit":{"message":"old work",
                    "committer":{"date":"2001-09-01T00:00:00Z"},
                    "author":{"name":"bob","date":"2001-09-01T00:00:00Z"}}}])");
    fake.route(commits_url("you/warm", cutoff),
               R"([{"sha":"warm0001","commit":{"message":"warm work",
                    "committer":{"date":"2001-09-08T10:00:00Z"},
                    "author":{"name":"carol","date":"2001-09-08T10:00:00Z"}}}])");
    auto tools = gitea_tools(recent_cfg(), fake.fn());
    const Tool* t = find_tool(tools, "gitea_recent_commits");

    auto all = t->run({{"timeframe", "1d"}});
    CHECK(all.has_value());
    CHECK(all->find("me/hot (1 commit; branches: main):") != std::string::npos);
    CHECK(all->find("fresh001") != std::string::npos);
    CHECK(all->find("ancient0") == std::string::npos);  // client-side cutoff
    CHECK(all->find("you/warm (1 commit; branches: main):") != std::string::npos);
    CHECK(all->find("me/empty") == std::string::npos);  // 409 skipped
    CHECK(all->find("me/stale") == std::string::npos);  // pruned, never fetched
    for (const std::string& url : fake.urls)
        CHECK(url.find("me/stale") == std::string::npos);

    fake.urls.clear();
    auto mine = t->run({{"timeframe", "1d"}, {"pattern", "^me/"}});
    CHECK(mine.has_value());
    CHECK(mine->find("me/hot") != std::string::npos);
    CHECK(mine->find("you/warm") == std::string::npos);
    for (const std::string& url : fake.urls)
        CHECK(url.find("you/warm") == std::string::npos);
}

TEST("gitea_recent_commits: branch_pattern selects branches, dedup across them") {
    const std::int64_t cutoff = kNow - 86400;
    FakeHttp fake;
    fake.route(kSearchUrl,
               R"({"ok":true,"data":[
                   {"full_name":"me/hot","updated_at":"2001-09-09T00:00:00Z"}]})");
    fake.route(branches_url("me/hot"),
               R"([{"name":"main"},{"name":"feature-x"},{"name":"wip"}])");
    // `shared01` is reachable from both branches; `feat0001` is newer than it.
    fake.route(commits_url("me/hot", cutoff, "main"),
               R"([{"sha":"shared01","commit":{"message":"shared base",
                    "committer":{"date":"2001-09-08T12:00:00Z"},
                    "author":{"name":"alice","date":"2001-09-08T12:00:00Z"}}}])");
    fake.route(commits_url("me/hot", cutoff, "feature-x"),
               R"([{"sha":"feat0001","commit":{"message":"feature tip",
                    "committer":{"date":"2001-09-08T18:00:00Z"},
                    "author":{"name":"bob","date":"2001-09-08T18:00:00Z"}}},
                   {"sha":"shared01","commit":{"message":"shared base",
                    "committer":{"date":"2001-09-08T12:00:00Z"},
                    "author":{"name":"alice","date":"2001-09-08T12:00:00Z"}}}])");
    fake.route(commits_url("me/hot", cutoff, "wip"), "[]");
    auto tools = gitea_tools(recent_cfg(), fake.fn());
    const Tool* t = find_tool(tools, "gitea_recent_commits");

    auto all = t->run({{"timeframe", "1d"}});
    CHECK(all.has_value());
    CHECK(all->find("me/hot (2 commits; branches: main, feature-x):") !=
          std::string::npos);
    CHECK_EQ(all->find("shared01"), all->rfind("shared01"));  // reported once
    // Newest first across branches despite main being fetched first.
    CHECK(all->find("feat0001") < all->find("shared01"));

    fake.urls.clear();
    auto feat = t->run({{"timeframe", "1d"}, {"branch_pattern", "^feature-"}});
    CHECK(feat.has_value());
    CHECK(feat->find("me/hot (2 commits; branches: feature-x):") != std::string::npos);
    for (const std::string& url : fake.urls)
        CHECK(url.find("sha=main") == std::string::npos);  // only feature-x fetched
}

TEST("gitea_recent_commits: empty window message, bad args rejected early") {
    FakeHttp fake;
    fake.route(kSearchUrl,
               R"({"ok":true,"data":[
                   {"full_name":"me/hot","updated_at":"2001-09-09T00:00:00Z"}]})");
    fake.route(branches_url("me/hot"), R"([{"name":"main"}])");
    fake.route(commits_url("me/hot", kNow - 30 * 86400), "[]");
    auto tools = gitea_tools(recent_cfg(), fake.fn());
    const Tool* t = find_tool(tools, "gitea_recent_commits");

    auto none = t->run({{"timeframe", "30d"}, {"pattern", "nothing-matches"}});
    CHECK(none.has_value());
    CHECK_EQ(*none, "No commits in the last 30d for repositories matching "
                    "'nothing-matches'.");

    CHECK(!t->run(json::object()).has_value());          // timeframe required
    CHECK(!t->run({{"timeframe", "soon"}}).has_value()); // unparsable
    CHECK(!t->run({{"timeframe", "1d"}, {"pattern", "[unclosed"}}).has_value());
    const std::size_t before = fake.urls.size();
    CHECK(!t->run({{"timeframe", "bad"}}).has_value());
    CHECK(!t->run({{"timeframe", "1d"}, {"branch_pattern", "[unclosed"}})
               .has_value());
    CHECK_EQ(fake.urls.size(), before);  // rejected before any request
}
