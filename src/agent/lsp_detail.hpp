#ifndef MOOCODE_LSP_DETAIL_HPP
#define MOOCODE_LSP_DETAIL_HPP

// Wire-framing internals of the LSP client (lsp_client.cpp): the
// "Content-Length: N\r\n\r\n<body>" frame parser/serializer. Kept out of the
// public lsp_client.hpp surface; only the client implementation and its
// white-box tests need these. Not part of the stable API.

#include <cstddef>
#include <expected>
#include <optional>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

#include "agent/types.hpp"  // Error

namespace moocode::lsp {

// One framed LSP message peeled off the front of a buffer.
struct Framed {
    nlohmann::json msg;
    std::size_t consumed;  // bytes consumed from the front of the buffer
};

// Try to parse one "Content-Length: N\r\n\r\n<body>" frame from the front of
// `buf`. nullopt => the buffer does not yet hold a complete message (need more
// bytes). Error => malformed header or body JSON.
std::expected<std::optional<Framed>, Error> try_parse_frame(std::string_view buf);

// Serialize `msg` into a Content-Length framed wire message.
std::string frame(const nlohmann::json& msg);

}  // namespace moocode::lsp

#endif  // MOOCODE_LSP_DETAIL_HPP
