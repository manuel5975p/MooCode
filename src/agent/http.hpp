#ifndef MOOCODE_HTTP_HPP
#define MOOCODE_HTTP_HPP

// Minimal blocking HTTP client over libcurl. Only what the agent needs: a JSON
// POST. A returned Response means the round-trip completed (inspect `status` for
// HTTP-level errors); an Error means the transport itself failed (DNS, connect,
// timeout) and there is no HTTP status to speak of.

#include <atomic>
#include <expected>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "agent/types.hpp"

namespace moocode::http {

struct Response {
    long status = 0;     // HTTP status code, e.g. 200, 404, 500
    std::string body;    // raw response body
};

// POST `body` to `url` with the given header lines ("Key: value"); Content-Type
// is set to application/json unless caller already supplies one.
// pre: url nonempty. timeout_secs <= 0 disables the overall transfer cap (only
// connection setup stays bounded). post: Response on completed round-trip,
// Error on transport failure (Error.code carries the CURLcode).
std::expected<Response, Error> post_json(std::string_view url,
                                         const std::vector<std::string>& headers,
                                         std::string_view body,
                                         long timeout_secs = 60);

// GET `url` with the given header lines ("Key: value"). Same contract as
// post_json: a Response means the round-trip completed (inspect `status`); an
// Error means transport failure (Error.code carries the CURLcode).
// pre: url nonempty. timeout_secs <= 0 disables the overall transfer cap.
std::expected<Response, Error> get(std::string_view url,
                                   const std::vector<std::string>& headers = {},
                                   long timeout_secs = 60);

// Percent-encode/decode helpers are in http_detail.hpp — include that header
// if you need to unit-test them directly. The production call site (gitea.cpp
// constructing URL paths) uses them via http_detail.hpp included from http.cpp,
// so the symbols are still linkable.

// POST `body` and hand the response body to `on_chunk` incrementally as it
// arrives, for streaming (Server-Sent Events) responses. `on_chunk` is invoked
// zero or more times with consecutive byte ranges; the body is not buffered.
// When `cancel` is non-null and becomes true mid-stream the transfer is aborted
// and an Error with code CURLE_ABORTED_BY_CALLBACK is returned.
// pre: url nonempty. timeout_secs <= 0 (the default) disables the overall
// transfer cap so a long-reasoning model is never cut off mid-stream; only
// connection setup stays bounded. post: HTTP status on a completed round-trip,
// Error on transport failure (Error.code carries the CURLcode).
std::expected<long, Error> post_json_stream(
    std::string_view url, const std::vector<std::string>& headers,
    std::string_view body,
    const std::function<void(std::string_view)>& on_chunk,
    const std::atomic<bool>* cancel = nullptr,
    long timeout_secs = 0);

// One-time global libcurl init/cleanup. Call init() once at startup before any
// post_json from multiple threads; safe to skip in single-threaded use.
void global_init();
void global_cleanup();

}  // namespace moocode::http

#endif  // MOOCODE_HTTP_HPP
