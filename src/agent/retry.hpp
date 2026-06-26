#ifndef MOOCODE_RETRY_HPP
#define MOOCODE_RETRY_HPP

// Transient-failure retry policy shared by every provider backend. An upstream
// blip — a 502 Bad Gateway from a proxy, a litellm "Connection error" 500, or a
// connection reset before any token arrived — should not abort the whole turn
// and force the user to re-issue the prompt. Wrapping each request in a short
// exponential backoff turns those self-healing failures into a brief pause
// instead of a dead turn. Header-only: tiny, pure logic plus a generic wrapper.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <thread>

#include "agent/types.hpp"

namespace moocode {

// Retry knobs. The defaults give up after ~7.5s of cumulative backoff (4 retries
// at 0.5s, 1s, 2s, 4s), short enough not to feel like a hang yet long enough to
// ride out a proxy hiccup.
struct RetryPolicy {
    int max_retries = 4;       // request attempts after the first
    long base_delay_ms = 500;  // first backoff; doubles each retry
    long max_delay_ms = 8000;  // cap on any single backoff
};

// Process-wide default policy. MOOCODE_MAX_RETRIES overrides max_retries (0
// disables retries entirely) and is read exactly once.
inline const RetryPolicy& default_retry_policy() {
    static const RetryPolicy p = [] {
        RetryPolicy d;
        if (const char* e = std::getenv("MOOCODE_MAX_RETRIES"); e && *e) {
            int n = std::atoi(e);
            if (n >= 0) d.max_retries = n;
        }
        return d;
    }();
    return p;
}

// True when `e` is a transient, server-side failure worth re-sending: a
// transport error (DNS/connect/recv reset) or an HTTP 5xx. Client errors (4xx),
// parse failures of an otherwise-complete 2xx response, cancellation, and
// user-declined actions are never retried — re-sending changes nothing. The 5xx
// test is on `code` so it fires even on the Unknown-kind error paths that carry
// the HTTP status in `code` without setting `kind`.
inline bool is_transient_error(const Error& e) {
    if (e.kind == ErrorKind::Cancelled || e.kind == ErrorKind::User) return false;
    if (e.kind == ErrorKind::Transport) return true;
    return e.code >= 500 && e.code <= 599;
}

// Backoff for 0-based retry `n`: base*2^n capped at max_delay_ms, slept in short
// slices so an Esc (via `cancel`) aborts promptly. Returns false if cancellation
// was observed (the caller should stop retrying).
inline bool retry_backoff_sleep(const RetryPolicy& p, int n,
                                const std::atomic<bool>* cancel) {
    long delay = p.base_delay_ms;
    for (int i = 0; i < n && delay < p.max_delay_ms; ++i) delay *= 2;
    delay = std::min(delay, p.max_delay_ms);
    constexpr long kSlice = 50;
    for (long waited = 0; waited < delay; waited += kSlice) {
        if (cancel && cancel->load(std::memory_order_acquire)) return false;
        std::this_thread::sleep_for(
            std::chrono::milliseconds(std::min(kSlice, delay - waited)));
    }
    return !(cancel && cancel->load(std::memory_order_acquire));
}

// Run `attempt` and re-run it while it fails transiently, up to
// policy.max_retries extra times. `made_progress` is re-read after each attempt:
// once it is true the result is returned as-is (a stream that already emitted
// tokens must never be replayed — that would duplicate output). `attempt` owns
// any per-try accumulator and must reset it on entry. Honors `cancel` both
// during the backoff wait and as a stop condition. Generic over the result type
// so it serves both complete() and complete_stream().
template <typename Fn>
auto with_retry(const RetryPolicy& policy, const std::atomic<bool>* cancel,
                const bool& made_progress, Fn&& attempt) -> decltype(attempt()) {
    auto r = attempt();
    for (int n = 0; !r && n < policy.max_retries; ++n) {
        if (made_progress || !is_transient_error(r.error())) break;
        if (cancel && cancel->load(std::memory_order_acquire)) break;
        if (!retry_backoff_sleep(policy, n, cancel)) break;
        r = attempt();
    }
    return r;
}

}  // namespace moocode

#endif  // MOOCODE_RETRY_HPP
