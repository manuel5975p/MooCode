#ifndef FLAGENT_CLANGD_TOOLS_HPP
#define FLAGENT_CLANGD_TOOLS_HPP

// clangd-backed code-intelligence tools: precise, semantic navigation that the
// text tools (read_file/grep) cannot give. All share one long-lived ClangdSession
// (spawned lazily on first use). Symbols are addressed "on a line": the model
// supplies a path, a 1-based line, and the identifier text it can already see
// from grep/read_file; the tool computes the LSP cursor position itself.

#include <filesystem>
#include <memory>
#include <optional>
#include <string>

#include "agent/builtin_tools.hpp"  // ToolOptions (for rename's diff callback)
#include "agent/lsp_client.hpp"
#include "agent/tools.hpp"

namespace flagent {

struct ClangdToolsConfig {
    std::filesystem::path root;
    std::string clangd_path = "clangd";  // env CLANGD_PATH
    std::optional<std::filesystem::path> compile_commands_dir;  // env CLANGD_COMPILE_COMMANDS_DIR
    int request_timeout_secs = 20;
    int index_wait_ms = 0;  // env CLANGD_INDEX_WAIT_MS; bounded wait before queries
};

// One shared session for the whole tool set (lazy spawn on first tool call).
std::shared_ptr<lsp::ClangdSession> make_clangd_session(const ClangdToolsConfig& cfg);

// Individual factories (each independently testable).
Tool find_references_tool(std::shared_ptr<lsp::ClangdSession> session);
Tool go_to_definition_tool(std::shared_ptr<lsp::ClangdSession> session);
Tool go_to_implementation_tool(std::shared_ptr<lsp::ClangdSession> session);
Tool hover_tool(std::shared_ptr<lsp::ClangdSession> session);
Tool list_symbols_tool(std::shared_ptr<lsp::ClangdSession> session);
Tool call_hierarchy_tool(std::shared_ptr<lsp::ClangdSession> session);
// rename mutates files; it reports each change through opts.on_file_change so the
// CLI/TUI render diffs, and is gated by the permission allowlist as tool "rename".
Tool rename_tool(std::shared_ptr<lsp::ClangdSession> session, ToolOptions opts);

// Register the full clangd tool set into `reg`.
void register_clangd_tools(ToolRegistry& reg, std::shared_ptr<lsp::ClangdSession> session,
                           ToolOptions opts);

}  // namespace flagent

#endif  // FLAGENT_CLANGD_TOOLS_HPP
