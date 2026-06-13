#include "agent/http.hpp"

#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include "loopback_server.hpp"
#include "test_harness.hpp"

using namespace flagent;

TEST("post_json: 200 returns body and status") {
    test::LoopbackServer srv;
    srv.serve(200, R"({"ok":true})");
    auto r = http::post_json(srv.url(), {}, R"({"q":1})");
    srv.stop();
    CHECK(r.has_value());
    if (r) {
        CHECK_EQ(r->status, 200L);
        CHECK_EQ(r->body, std::string(R"({"ok":true})"));
    }
}

TEST("post_json: sends the body to the server") {
    test::LoopbackServer srv;
    srv.serve(200, "ok");
    auto r = http::post_json(srv.url(), {}, R"({"hello":"world"})");
    srv.stop();
    CHECK(r.has_value());
    CHECK(srv.last_body().find(R"("hello":"world")") != std::string::npos);
}

TEST("post_json: sets application/json content-type by default") {
    test::LoopbackServer srv;
    srv.serve(200, "ok");
    auto r = http::post_json(srv.url(), {}, "{}");
    srv.stop();
    std::string req = srv.last_request();
    for (auto& c : req) c = static_cast<char>(::tolower(c));
    CHECK(req.find("content-type: application/json") != std::string::npos);
}

TEST("post_json: forwards custom headers") {
    test::LoopbackServer srv;
    srv.serve(200, "ok");
    auto r = http::post_json(srv.url(), {"Authorization: Bearer SEKRET"}, "{}");
    srv.stop();
    CHECK(srv.last_request().find("Authorization: Bearer SEKRET") != std::string::npos);
}

TEST("post_json: HTTP 500 is a value with status 500, not an Error") {
    test::LoopbackServer srv;
    srv.serve(500, R"({"error":"boom"})");
    auto r = http::post_json(srv.url(), {}, "{}");
    srv.stop();
    CHECK(r.has_value());  // transport succeeded
    if (r) {
        CHECK_EQ(r->status, 500L);
        CHECK(r->body.find("boom") != std::string::npos);
    }
}

TEST("post_json: HTTP 404 body is captured") {
    test::LoopbackServer srv;
    srv.serve(404, "not found");
    auto r = http::post_json(srv.url(), {}, "{}");
    srv.stop();
    CHECK(r.has_value());
    if (r) CHECK_EQ(r->status, 404L);
}

TEST("post_json: connection refused returns Error") {
    // Nothing listening on this port (server destructed immediately).
    std::string dead_url;
    {
        test::LoopbackServer srv;
        dead_url = srv.url();
    }
    auto r = http::post_json(dead_url, {}, "{}");
    CHECK(!r.has_value());
    if (!r) CHECK(!r.error().msg.empty());
}

TEST("post_json: timeout returns Error") {
    test::LoopbackServer srv;
    srv.serve(200, "late", 1, std::chrono::milliseconds{1500});
    auto r = http::post_json(srv.url(), {}, "{}", /*timeout_secs=*/1);
    srv.stop();
    CHECK(!r.has_value());
}

TEST("post_json: empty url returns Error") {
    auto r = http::post_json("", {}, "{}");
    CHECK(!r.has_value());
}

// --- P3: CURLSH shared handle (connection/DNS/TLS reuse) --------------------
// The handshake-reuse win is not unit-testable without real TLS; these tests
// instead pin behavioural parity: independent bodies, no option-bleed between a
// post and a get, the no-global_init fallback, and concurrent correctness (the
// lock-callback gate, meaningful under TSan).

TEST("http: two sequential post_json yield independent correct bodies") {
    test::LoopbackServer srv;
    srv.serve(200, R"({"r":1})");
    auto a = http::post_json(srv.url(), {}, R"({"q":"first"})");
    srv.stop();
    CHECK(a.has_value());
    if (a) CHECK_EQ(a->body, std::string(R"({"r":1})"));
    CHECK(srv.last_body().find("first") != std::string::npos);

    test::LoopbackServer srv2;
    srv2.serve(200, R"({"r":2})");
    auto b = http::post_json(srv2.url(), {}, R"({"q":"second"})");
    srv2.stop();
    CHECK(b.has_value());
    if (b) CHECK_EQ(b->body, std::string(R"({"r":2})"));
    CHECK(srv2.last_body().find("second") != std::string::npos);
}

TEST("http: a get after a post_json on the same thread does not bleed options") {
    test::LoopbackServer srv;
    srv.serve(200, "ok", 2);  // serve a POST then a GET
    auto p = http::post_json(srv.url(), {}, R"({"x":1})");
    CHECK(p.has_value());
    auto g = http::get(srv.url());
    srv.stop();
    CHECK(g.has_value());
    if (g) CHECK_EQ(g->status, 200L);
    // The second request must be a clean GET, not a leftover POST.
    CHECK(srv.last_request().find("GET ") != std::string::npos);
}

TEST("http: requests succeed without global_init (g_share null fallback)") {
    // The default test binary never calls global_init; this is the same path
    // the rest of the suite uses, asserted explicitly.
    test::LoopbackServer srv;
    srv.serve(200, "no-share-ok");
    auto r = http::post_json(srv.url(), {}, "{}");
    srv.stop();
    CHECK(r.has_value());
    if (r) CHECK_EQ(r->body, std::string("no-share-ok"));
}

TEST("http: concurrent requests under an active share stay correct") {
    // Activate the share, then hammer it from several threads (the lock-callback
    // correctness gate — run under TSan). Each thread has its own server so the
    // single-connection loopback never serializes the clients.
    http::global_init();
    constexpr int kThreads = 4;
    std::vector<test::LoopbackServer> servers(kThreads);
    std::vector<std::string> bodies(kThreads);
    std::vector<std::thread> ts;
    for (int i = 0; i < kThreads; ++i) {
        servers[i].serve(200, "body-" + std::to_string(i));
        ts.emplace_back([&, i] {
            auto r = http::post_json(servers[i].url(), {}, "{}");
            if (r) bodies[i] = r->body;
        });
    }
    for (auto& t : ts) t.join();
    for (int i = 0; i < kThreads; ++i) servers[i].stop();
    for (int i = 0; i < kThreads; ++i)
        CHECK_EQ(bodies[i], std::string("body-" + std::to_string(i)));
    http::global_cleanup();  // restore hermetic state for the rest of the suite
}

// --- url_encode -------------------------------------------------------------

TEST("url_encode: unreserved bytes pass through unchanged") {
    CHECK_EQ(http::url_encode("AZaz09-_.~"), std::string("AZaz09-_.~"));
}

TEST("url_encode: space and reserved chars become percent escapes") {
    CHECK_EQ(http::url_encode("a b&c=d/e?f"),
             std::string("a%20b%26c%3Dd%2Fe%3Ff"));
}

TEST("url_encode: empty string yields empty string") {
    CHECK_EQ(http::url_encode(""), std::string(""));
}

// --- get --------------------------------------------------------------------

TEST("get: 200 returns body and status") {
    test::LoopbackServer srv;
    srv.serve(200, R"({"results":[]})");
    auto r = http::get(srv.url());
    srv.stop();
    CHECK(r.has_value());
    if (r) {
        CHECK_EQ(r->status, 200L);
        CHECK_EQ(r->body, std::string(R"({"results":[]})"));
    }
}

TEST("get: issues an HTTP GET request") {
    test::LoopbackServer srv;
    srv.serve(200, "ok");
    auto r = http::get(srv.url());
    srv.stop();
    CHECK(srv.last_request().find("GET ") != std::string::npos);
}

TEST("get: forwards custom headers") {
    test::LoopbackServer srv;
    srv.serve(200, "ok");
    auto r = http::get(srv.url(), {"X-Probe: yes"});
    srv.stop();
    CHECK(srv.last_request().find("X-Probe: yes") != std::string::npos);
}

TEST("get: empty url returns Error") {
    auto r = http::get("");
    CHECK(!r.has_value());
}

// --- post_json_stream -------------------------------------------------------

TEST("post_json_stream: delivers the body to on_chunk and returns status") {
    test::LoopbackServer srv;
    srv.serve(200, "data: {\"a\":1}\n\ndata: [DONE]\n\n");
    std::string got;
    auto st = http::post_json_stream(srv.url(), {}, "{}",
                                     [&](std::string_view sv) { got.append(sv); });
    srv.stop();
    CHECK(st.has_value());
    if (st) CHECK_EQ(*st, 200L);
    CHECK(got.find("[DONE]") != std::string::npos);
}

TEST("post_json_stream: sends the request body to the server") {
    test::LoopbackServer srv;
    srv.serve(200, "data: [DONE]\n\n");
    auto st = http::post_json_stream(srv.url(), {}, R"({"stream":true})",
                                     [](std::string_view) {});
    srv.stop();
    CHECK(st.has_value());
    CHECK(srv.last_body().find(R"("stream":true)") != std::string::npos);
}

TEST("post_json_stream: connection refused returns Error") {
    std::string dead;
    {
        test::LoopbackServer srv;
        dead = srv.url();
    }
    auto st = http::post_json_stream(dead, {}, "{}", [](std::string_view) {});
    CHECK(!st.has_value());
}

TEST("post_json_stream: empty url returns Error") {
    auto st = http::post_json_stream("", {}, "{}", [](std::string_view) {});
    CHECK(!st.has_value());
}
