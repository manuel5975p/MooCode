# moocode — Project Overview

**A minimal but real coding agent in C++23** — an LLM loop + tools, talking to
any OpenAI-compatible `/chat/completions` endpoint or any
Anthropic-compatible `/v1/messages` endpoint, with an interactive FTXUI TUI.

---

## Key Facts

| Aspect | Detail |
|---|---|
| **Language** | C++23 (`std::expected`) |
| **Build** | CMake ≥ 3.20, Ninja |
| **Binary deps** | nlohmann_json, FTXUI v6.1.9 (fetched), libcurl (bundled static by default) |
| **Test framework** | Catch2-style (`test_harness.hpp`) via CTest |
| **Error model** | `std::expected<T, Error>` — no exceptions |
| **Config** | flags > `LLM_*` env vars > active profile > `settings.toml` > defaults |
| **State dir** | `~/.moo/` (configurable via `$MOOCODE_HOME`) |

---

## Architecture: Layered Static Libraries

Layered bottom-up, no circular dependencies. Public headers in `src/agent/`,
included as `agent/<name>.hpp`.

```
agent_types         header-only: Error, Message, ToolCall, ToolSpec, Conversation
  ↑
agent_json          safe JSON parse/dump helpers over nlohmann::json
agent_http          libcurl RAII HTTP client (POST JSON, streaming SSE)
agent_stream        pure SSE framing: StreamAccumulator, AnthropicStreamAccumulator
  ↑
agent_provider      Provider interface → OpenAI + Anthropic backends
agent_tools         ToolRegistry: name → (spec, fn)
agent_search        web_search tool: SearXNG → Tavily → DuckDuckGo
agent_gitea         read-only Gitea REST inspection tools
agent_lsp           clangd-backed code intelligence (LSP JSON-RPC over stdio)
agent_permissions   screen-free approval policy (allowlist + decide)
agent_diff          pure LCS line diff (no I/O)
agent_persist       ~/.moo TOML store (toml++ confined to one TU)
agent_mentions      @-mention expansion (glob, dir, sandboxed)
  ↑
agent_core          Agent loop: assemble, dispatch tools, manage history
agent_question      ask_user tool + QuestionGate
agent_subagent      spawn_subagent tool
  ↑
agent_tui           Full-screen FTXUI TUI (two-pane, streaming, diff rendering)
  ↑
moocode (main)      CLI entry point: config, tool registration, dispatch
```

Every library is a separate CMake target so tests can link each in isolation.

---

## Source Files

`src/agent/` (60 files):

| Category | Files |
|---|---|
| **Core types** | `types.hpp` |
| **JSON** | `json_util.{hpp,cpp}` |
| **HTTP** | `http.{hpp,cpp}` |
| **Streaming** | `stream.{hpp,cpp}`, `stream_detail.hpp` |
| **Providers** | `provider.hpp`, `provider_factory.{hpp,cpp}`, `openai_provider.{hpp,cpp}`, `anthropic_provider.{hpp,cpp}` |
| **Tools** | `tools.{hpp,cpp}`, `builtin_tools.{hpp,cpp}` |
| **Search** | `search.{hpp,cpp}`, `search_internal.hpp` |
| **Gitea** | `gitea.{hpp,cpp}` |
| **LSP/Clangd** | `lsp_client.{hpp,cpp}`, `lsp_detail.hpp`, `clangd_tools.{hpp,cpp}` |
| **Permissions** | `permissions.{hpp,cpp}` |
| **Diff** | `diff.{hpp,cpp}` |
| **Persistence** | `persist.{hpp,cpp}` |
| **Mentions** | `mentions.{hpp,cpp}` |
| **Agent loop** | `agent.{hpp,cpp}` |
| **ask_user** | `question_tool.{hpp,cpp}` |
| **Subagent** | `subagent_tool.{hpp,cpp}` |
| **Syntax** | `syntax_highlight.{hpp,cpp}` |
| **TUI** | `tui.{hpp,cpp}`, `tui_text.{hpp,cpp}`, `tui_worker.{hpp,cpp}`, `tui_slash.{hpp,cpp}` |
| **Entry point** | `main.cpp` |
| **Shared helpers** | `strutil.hpp`, `fsutil.hpp` |

---

## Tools

### Filesystem (sandboxed under project root)
- `read_file` — line-window reads
- `write_file` — create/overwrite
- `edit_file` — exact unique match replace (fails on 0 or >1 matches)
- `list_dir` — sorted listing
- `run_bash` — `/bin/sh -c` in process group with timeout

### Web
- `web_search` — SearXNG → Tavily (quota-gated) → DuckDuckGo
- `web_fetch` — HTTP GET (256 KiB cap)

### Code Intelligence (clangd/LSP)
- `find_references`, `go_to_definition`, `go_to_implementation`
- `hover`, `list_symbols`, `call_hierarchy`, `rename`

### Gitea (read-only REST)
- `gitea_repos`, `gitea_repo`, `gitea_prs`, `gitea_pr`, `gitea_pr_diff`
- `gitea_commits`, `gitea_commit`, `gitea_file`

### Interaction
- `spawn_subagent` — recursive sub-agent for complex sub-tasks
- `ask_user` — blocking pick-one modal (TUI) or stderr/stdin (CLI)

---

## Provider Support

Two backends behind a single `Provider` interface:

| Backend | Endpoint | Auth |
|---|---|---|
| **OpenAI** | `/chat/completions` | `Authorization: Bearer` |
| **Anthropic** | `/v1/messages` | `x-api-key` + `anthropic-version` |

Auto-detection from hostname. Presets: `minimax`, `deepseek-pro`, `deepseek-flash`.

Generation controls (all opt-in): `effort` (low/medium/high/none), `thinking` (on/off), `temperature` (0.0–∞).

---

## Interactive TUI

Two-pane FTXUI interface:
- **Left pane**: chat (prose, collapsible reasoning, inline diffs)
- **Right pane**: activity tree (clickable tool-call rows, subagent nesting, detail pane)
- **Status bar**: model, CWD, token counter, thinking indicator

**Slash commands**: `/model`, `/provider`, `/effort`, `/thinking`, `/temp`, `/continue`, `/resume`, `/rewind`, `/compact`.

**Clipboard**: tool-approval modal (once/session/always/deny), `@`-autocomplete.

---

## Testing

26 test files in `tests/`, one executable per library:

```
test_types, test_json, test_http, test_stream,
test_provider, test_anthropic_provider, test_builtins,
test_agent, test_permissions, test_diff, test_persist,
test_tui, test_tui_slash, test_tui_worker, test_mentions,
test_lsp, test_search, test_tools, test_question,
test_subagent, test_gitea, test_syntax, test_compact,
test_compact_cancel
```

Run: `ctest --test-dir build --output-on-failure`

CI presets (via `CMakePresets.json`):
- `dev` — Release, `MOOCODE_WERROR=ON`
- `asan` — Debug, `MOOCODE_SANITIZE=address;undefined`
- `tsan` — Debug, `MOOCODE_SANITIZE=thread`

---

## Bundled Dependencies

Default (`-DMOOCODE_BUNDLED_CURL=ON`): fully static, feature-stripped curl chain built from source via `ExternalProject`:

```
zlib 1.3.1 → nghttp2 1.65.0 → OpenSSL 3.5.0 → curl 8.15.0
```

Result: HTTPS + HTTP/2 + gzip only. Tarballs cached in `.deps-cache/`, hash-verified. `-DMOOCODE_BUNDLED_CURL=OFF` falls back to system libcurl.

---

## Config Files

```
~/.moo/
├── settings.toml       base_url, model, provider, profiles, generation params
├── credentials.toml    per-profile API keys (0600)
├── permissions.toml    always-allowed tool list
├── history             input-line history
├── search_quota.json   Tavily monthly-quota counter
└── conversations/
    └── <yyyymmdd-hhmmss>-<cwd-hash>.toml
```

---

## Deliberate Scope

What's **in**: single-provider agent loop, file + shell + web + LSP + Gitea tools, interactive TUI, sub-agent spawning, conversation persistence, permission gating, streaming display.

What's **deferred**: async/concurrency, multi-provider routing, MCP, Anthropic `cache_control`, Git/debugger/compiler tooling, conversation compaction beyond `/compact`.
