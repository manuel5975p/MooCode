#ifndef MOOCODE_HTTP_DETAIL_HPP
#define MOOCODE_HTTP_DETAIL_HPP

// Internal URL-encoding helpers, moved out of http.hpp so the production
// surface stays lean. Included by http.cpp (which defines them) and
// test_http.cpp (which white-box tests them).

#include <string>
#include <string_view>

namespace moocode::http {

// Percent-encode `s` for use as a URL query-component value (RFC 3986
// unreserved bytes pass through; everything else becomes %XX). Total.
std::string url_encode(std::string_view s);

// Percent-decode a URL-encoded string (RFC 3986). %XX hex sequences are
// decoded; malformed sequences (truncated, non-hex) are left as-is. Total.
std::string url_decode(std::string_view s);

}  // namespace moocode::http

#endif  // MOOCODE_HTTP_DETAIL_HPP
