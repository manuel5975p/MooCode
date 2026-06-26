#include "agent/retry.hpp"

#include <atomic>
#include <expected>

#include "test_harness.hpp"

using namespace moocode;

namespace {

// A zero-delay policy so the unit tests never actually sleep.
RetryPolicy fast(int max_retries) {
    return RetryPolicy{.max_retries = max_retries, .base_delay_ms = 0,
                       .max_delay_ms = 0};
}

// Construct an Error with all fields named so -Wmissing-designated-field-init
// (built with -Werror) stays quiet.
Error err(int code, ErrorKind kind = ErrorKind::Unknown) {
    return Error{.msg = "x", .code = code, .kind = kind};
}

std::expected<int, Error> ok(int v) { return v; }
std::expected<int, Error> fail(Error e) { return std::unexpected(std::move(e)); }

}  // namespace

// --- is_transient_error -----------------------------------------------------

TEST("is_transient_error: 5xx and transport are transient") {
    CHECK(is_transient_error(err(500)));
    CHECK(is_transient_error(err(502)));
    CHECK(is_transient_error(err(503)));
    CHECK(is_transient_error(err(599)));
    CHECK(is_transient_error(err(7, ErrorKind::Transport)));
}

TEST("is_transient_error: 4xx, parse, cancel, user are not transient") {
    CHECK(!is_transient_error(err(400)));
    CHECK(!is_transient_error(err(401)));
    CHECK(!is_transient_error(err(404)));
    CHECK(!is_transient_error(err(429)));  // rate-limit: not auto-retried
    CHECK(!is_transient_error(err(0)));     // parse failure of a 2xx
    CHECK(!is_transient_error(err(503, ErrorKind::Cancelled)));
    CHECK(!is_transient_error(err(500, ErrorKind::User)));
}

// --- with_retry -------------------------------------------------------------

TEST("with_retry: succeeds on first try without re-invoking") {
    int calls = 0;
    bool progress = false;
    auto r = with_retry(fast(4), nullptr, progress, [&] { ++calls; return ok(42); });
    CHECK(r.has_value());
    CHECK_EQ(*r, 42);
    CHECK_EQ(calls, 1);
}

TEST("with_retry: retries a transient failure then succeeds") {
    int calls = 0;
    bool progress = false;
    auto r = with_retry(fast(4), nullptr, progress, [&] {
        ++calls;
        if (calls < 3) return fail(err(502));
        return ok(7);
    });
    CHECK(r.has_value());
    CHECK_EQ(*r, 7);
    CHECK_EQ(calls, 3);  // two failures + one success
}

TEST("with_retry: exhausts max_retries on persistent transient failure") {
    int calls = 0;
    bool progress = false;
    auto r = with_retry(fast(4), nullptr, progress,
                        [&] { ++calls; return fail(err(502)); });
    CHECK(!r.has_value());
    CHECK_EQ(r.error().code, 502);
    CHECK_EQ(calls, 5);  // 1 initial + 4 retries
}

TEST("with_retry: a permanent failure is not retried") {
    int calls = 0;
    bool progress = false;
    auto r = with_retry(fast(4), nullptr, progress,
                        [&] { ++calls; return fail(err(401)); });
    CHECK(!r.has_value());
    CHECK_EQ(r.error().code, 401);
    CHECK_EQ(calls, 1);  // 4xx: tried exactly once
}

TEST("with_retry: max_retries=0 disables retrying") {
    int calls = 0;
    bool progress = false;
    auto r = with_retry(fast(0), nullptr, progress,
                        [&] { ++calls; return fail(err(503)); });
    CHECK(!r.has_value());
    CHECK_EQ(calls, 1);
}

TEST("with_retry: made_progress stops a retry even on a transient error") {
    // Mirrors a stream that emitted tokens before the connection dropped: the
    // attempt sets `progress`, so with_retry must not replay it.
    int calls = 0;
    bool progress = false;
    auto r = with_retry(fast(4), nullptr, progress, [&] {
        ++calls;
        progress = true;  // a token was delivered this attempt
        return fail(err(502));
    });
    CHECK(!r.has_value());
    CHECK_EQ(calls, 1);  // not retried despite the 5xx
}

TEST("with_retry: a raised cancel flag stops retrying") {
    int calls = 0;
    bool progress = false;
    std::atomic<bool> cancel{false};
    auto r = with_retry(fast(4), &cancel, progress, [&] {
        ++calls;
        cancel.store(true);  // user hit Esc during the attempt
        return fail(err(502));
    });
    CHECK(!r.has_value());
    CHECK_EQ(calls, 1);  // cancellation short-circuits the retry loop
}
