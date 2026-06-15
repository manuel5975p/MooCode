#ifndef MOOCODE_LSP_CLIENT_HPP
#define MOOCODE_LSP_CLIENT_HPP

// A minimal, server-agnostic LSP JSON-RPC client over stdio, plus a ClangdSession
// that owns a long-lived `clangd` subprocess and the LSP lifecycle. The agent
// drives tools sequentially and blocks on each, so the client is single-threaded:
// it writes a framed request and poll()-reads framed messages until the matching
// response arrives, dispatching server notifications/requests along the way.
//
// The pure helpers (frame parsing, file:// <-> path, symbol->position math) carry
// no I/O and are unit-tested directly.

#include <sys/types.h>  // pid_t

#include <atomic>
#include <chrono>
#include <cstddef>
#include <expected>
#include <filesystem>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>

#include <nlohmann/json.hpp>

#include "agent/types.hpp"  // Error

namespace moocode::lsp {

// How the LSP `character` field counts: bytes (utf-8) or UTF-16 code units. The
// concrete value is negotiated with the server during `initialize`.
enum class PosEncoding { Utf8, Utf16 };

// A 0-based LSP text position; `character` is in the negotiated PosEncoding.
struct Position {
    int line = 0;
    int character = 0;
};

// --- pure helpers (no I/O; unit-tested) ------------------------------------

// "file:///abs/path" -> "/abs/path" (percent-decoded). Error if not a file URI.
std::expected<std::filesystem::path, Error> uri_to_path(std::string_view uri);

// Absolute path -> "file:///abs/path" with reserved bytes percent-encoded.
std::string path_to_uri(const std::filesystem::path& abs);

// Byte offset within a single line -> LSP character offset in `encoding` units.
int byte_to_lsp_char(std::string_view line_text, std::size_t byte_off,
                     PosEncoding encoding);

// LSP character offset (in `encoding` units) within a line -> byte offset,
// clamped to the line length. Inverse of byte_to_lsp_char on valid input.
std::size_t lsp_char_to_byte(std::string_view line_text, int character,
                             PosEncoding encoding);

// Find the `occurrence`-th (1-based) appearance of `symbol` on `line_text` and
// return its LSP Position on 1-based source line `line1`. Error if not present.
std::expected<Position, Error> position_of_symbol(std::string_view line_text,
                                                  int line1,
                                                  std::string_view symbol,
                                                  int occurrence,
                                                  PosEncoding encoding);

// --- the clangd session ----------------------------------------------------

struct LspConfig {
    std::string server_path = "clangd";  // the binary to exec (env CLANGD_PATH)
    std::filesystem::path root;          // workspace root (= tool sandbox root)
    std::optional<std::filesystem::path> compile_commands_dir;  // --compile-commands-dir
    std::chrono::milliseconds request_timeout{20000};
    std::chrono::milliseconds index_wait{0};  // bounded wait for first index pass; 0 = none
};

class ClangdSession {
public:
    explicit ClangdSession(LspConfig cfg) : cfg_(std::move(cfg)) {}
    ~ClangdSession();
    ClangdSession(const ClangdSession&) = delete;
    ClangdSession& operator=(const ClangdSession&) = delete;

    // Spawn + initialize clangd if not already running and handshaken. Idempotent;
    // a dead session is torn down and respawned. Error if the binary cannot be
    // started or the handshake fails.
    std::expected<void, Error> ensure_started();

    // Ensure clangd holds the current on-disk bytes of `abs`: didOpen the first
    // time, didChange (full text) when the file changed since we last synced.
    std::expected<void, Error> sync_file(const std::filesystem::path& abs);

    // Typed requests. Each returns the raw LSP `result` JSON for the caller to
    // format; a JSON null result is a valid "nothing here" answer.
    std::expected<nlohmann::json, Error> definition(const std::filesystem::path&, Position);
    std::expected<nlohmann::json, Error> implementation(const std::filesystem::path&, Position);
    std::expected<nlohmann::json, Error> references(const std::filesystem::path&, Position,
                                                    bool include_declaration);
    std::expected<nlohmann::json, Error> hover(const std::filesystem::path&, Position);
    std::expected<nlohmann::json, Error> document_symbols(const std::filesystem::path&);
    std::expected<nlohmann::json, Error> workspace_symbols(std::string_view query);
    std::expected<nlohmann::json, Error> prepare_call_hierarchy(const std::filesystem::path&, Position);
    std::expected<nlohmann::json, Error> incoming_calls(const nlohmann::json& item);
    std::expected<nlohmann::json, Error> outgoing_calls(const nlohmann::json& item);
    std::expected<nlohmann::json, Error> rename(const std::filesystem::path&, Position,
                                                std::string_view new_name);

    // Pump notifications (bounded by `limit`, else cfg_.index_wait) until the
    // background index reports done. Returns true if done. A 0 budget is an
    // immediate, non-blocking poll of current state.
    bool wait_for_index(std::optional<std::chrono::milliseconds> limit = std::nullopt);

    PosEncoding encoding() const { return encoding_; }
    const std::filesystem::path& root() const { return cfg_.root; }
    bool background_index_done() const { return index_done_; }

    // Cooperative cancel: when set and observed true, blocking reads abort.
    void watch_cancel(const std::atomic<bool>* c) { cancel_ = c; }

    // Serialises tool-level access to the single clangd connection. The session
    // is one stateful JSON-RPC pipe; with tool calls now running concurrently
    // (see Agent::run), each clangd tool locks this for the whole of its
    // sync+query sequence so requests never interleave on the wire. Held only by
    // the clangd tools, never by the session's own methods, so no re-entrancy.
    std::mutex& io_mutex() { return io_mtx_; }

private:
    std::expected<void, Error> spawn();
    std::expected<void, Error> handshake();
    void kill_and_reap();

    std::expected<nlohmann::json, Error> request(std::string_view method,
                                                 const nlohmann::json& params);
    std::expected<nlohmann::json, Error> request_until(
        std::string_view method, const nlohmann::json& params,
        std::chrono::steady_clock::time_point deadline);
    std::expected<void, Error> notify(std::string_view method, const nlohmann::json& params);
    std::expected<void, Error> write_message(const nlohmann::json& msg);
    std::expected<nlohmann::json, Error> pump_until(
        std::optional<int> want_id, std::chrono::steady_clock::time_point deadline);
    void dispatch(const nlohmann::json& msg);

    // Sync `abs`, then issue a textDocument/position request, merging `extra`
    // (e.g. references context or rename newName) into the params object.
    std::expected<nlohmann::json, Error> doc_pos_request(
        std::string_view method, const std::filesystem::path& abs, Position pos,
        const nlohmann::json& extra = nlohmann::json::object());

    std::mutex io_mtx_;  // see io_mutex(): serialises concurrent tool access
    LspConfig cfg_;
    pid_t pid_ = -1;
    int to_child_ = -1;    // we write clangd's stdin
    int from_child_ = -1;  // we read clangd's stdout (O_NONBLOCK)
    bool alive_ = false;
    bool initialized_ = false;
    bool index_done_ = false;
    PosEncoding encoding_ = PosEncoding::Utf16;  // until negotiated
    int next_id_ = 1;
    std::string in_buf_;  // bytes read but not yet framed
    std::set<std::string> index_tokens_;  // in-flight background-index progress tokens
    // path -> {document version, content hash} for didOpen/didChange tracking.
    std::map<std::filesystem::path, std::pair<int, std::size_t>> open_docs_;
    const std::atomic<bool>* cancel_ = nullptr;
};

}  // namespace moocode::lsp

#endif  // MOOCODE_LSP_CLIENT_HPP
