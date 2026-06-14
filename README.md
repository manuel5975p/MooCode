# moocode

A minimal but real coding agent in C++23: an LLM loop + tools, talking to any
OpenAI-compatible `/chat/completions` endpoint (OpenAI, OpenRouter, vLLM,
llama.cpp, Ollama, …) **or** an Anthropic-native `/v1/messages` endpoint
(api.anthropic.com and Anthropic-compatible gateways like MiniMax and DeepSeek).

The agent is given filesystem tools, code-intelligence tools (clangd), web
search, sub-agent spawning, and an interactive TUI. It edits files, runs shell
commands, navigates symbols, reads docs, searches the web, and self-corrects from
tool errors — all sandboxed to the current working directory with permission
gating.

## Quick Start

```sh
# Build (C++23 compiler, CMake ≥ 3.20, Ninja recommended)
cmake -S . -B build -G Ninja
cmake --build build

# Run — defaults to MiniMax's OpenAI-compatible endpoint
export LLM_API_KEY=...
./build/src/moocode "summarize the source files in src/"
```

## Build

Requirements: C++23 (`std::expected`), `nlohmann_json`, CMake ≥ 3.20, Ninja,
plus Perl and Make (to build the bundled OpenSSL). **FTXUI v6.1.9** is fetched
automatically by CMake (`FetchContent`, pinned tarball) — no system package
needed.

```sh
cmake -S . -B build -G Ninja          # BUILD_TESTING defaults ON
cmake --build build
ctest --test-dir build --output-on-failure
```

Toggle tests off with `-DBUILD_TESTING=OFF`.

### Bundled static libcurl

By default (`-DMOOCODE_BUNDLED_CURL=ON`), `cmake/BundledCurl.cmake` builds a
**fully static**, feature-stripped curl chain from pinned, hash-verified source
tarballs via CMake `ExternalProject`:

```
zlib 1.3.1 → nghttp2 1.65.0 → OpenSSL 3.5.0 → curl 8.15.0
```

Result: HTTPS + HTTP/2 + gzip only. The binary's sole runtime deps are the
C/C++ runtime (`libc`/`libstdc++`/`libm`/`libgcc_s`). Tarballs are cached in
`.deps-cache/` and reused across clean builds. `-DMOOCODE_BUNDLED_CURL=OFF`
falls back to `find_package(CURL)` against the system libcurl.

## Usage

### CLI (non-interactive)

```sh
# Prompt from argv
./build/src/moocode "explain what this project does"

# Prompt from stdin (pipe/redirect)
echo "fix the failing test" | ./build/src/moocode --yes
cat prompt.txt | ./build/src/moocode

# Override endpoint / model / key
./build/src/moocode -b https://api.openai.com/v1 -m gpt-4o-mini -k sk-... "hi"

# Anthropic-native endpoint (auto-detected from hostname)
LLM_API_KEY=sk-ant-... ./build/src/moocode -m claude-sonnet-4-6 "hi"

# Anthropic-compatible third parties — pass the bare ".../anthropic" base
./build/src/moocode -b https://api.minimax.io/anthropic  -m MiniMax-M3      -k ... "hi"
./build/src/moocode -b https://api.deepseek.com/anthropic -m deepseek-v4-pro -k ... "hi"

# Use a preset (fills base-url + model; OpenAI-native endpoint)
./build/src/moocode -p minimax        -k ... "hi"   # MiniMax-M3
./build/src/moocode -p deepseek-pro   -k ... "hi"   # deepseek-v4-pro
./build/src/moocode -p deepseek-flash -k ... "hi"   # deepseek-v4-flash
```

### Interactive TUI

Run with no argv prompt and a terminal on stdin:

```sh
./build/src/moocode
```

A full-screen two-pane FTXUI interface opens:

- **Left pane** — conversation (user/assistant prose + collapsible reasoning)
- **Right pane** — tool-activity feed (each tool call's name/args + result; long
  results are middle-elided to first 73 + `...` + last 73 chars once over 150)
- **Status bar** — model name, CWD, token counter, thinking indicator

Before each tool call a four-way approval modal appears (unless `--yes`):
`[y]` once, `[s]` session, `[a]` always (persisted), `[n]` deny.

**Keyboard shortcuts:** Enter send, ↑/↓ history, PgUp/PgDn scroll, F2 toggle
reasoning, `/exit` or Ctrl-C quit.

**Slash commands** (while idle):

| Command | Description |
|---|---|
| `/model <name>` | Switch model (profile-aware: carries its endpoint + key) |
| `/provider` | List configured profiles |
| `/provider <name>` | Switch to a named profile |
| `/provider save [name]` | Save the current connection as a profile |
| `/effort low\|medium\|high\|none` | Set reasoning effort |
| `/thinking on\|off` | Toggle extended thinking |
| `/temp <number>` | Set sampling temperature |
| `/continue` | Reload the most recent conversation for this CWD |
| `/resume` | Pick and reload a saved conversation |
| `/rewind` | Rewind the current conversation to a chosen turn |
| `/compact [instructions]` | Summarize the conversation to save context |

### `@`-mentions

Type `@path` in your prompt to auto-attach file contents:

- `@src/main.cpp` — a single file
- `@src/*.cpp` — glob expansion
- `@src/` — directory listing (non-recursive)

Globs and escapes are sandboxed to the current working directory. The TUI
shows a summary and offers autocomplete as you type an `@`-token.

### CLI options

```
moocode [options] [prompt]
  -y, --yes              Auto-approve all tools (skip permission prompts)
  -s, --system <str>     Override the system prompt
  -n, --max-iters <n>    Max agent iterations (0 = unlimited, default)
  -b, --base-url <url>   Override LLM_BASE_URL
  -k, --api-key <key>    Override LLM_API_KEY
  -m, --model <name>     Override LLM_MODEL
  -p, --provider <str>   Backend: openai | anthropic | auto, or a preset
  --profile <name>       Use a named profile from settings.toml
  --max-tokens <n>       Max output tokens (default 8192)
  -e, --effort <lvl>     Reasoning effort: low | medium | high | none
  -t, --temperature <f>  Sampling temperature (default 0.0)
  --thinking             Force extended thinking ON
  --no-thinking          Force extended thinking OFF
  --rtk                  Force rtk output-compaction ON
  --no-rtk               Force rtk output-compaction OFF
  -c, --context-window <n> Context window size for the TUI token gauge
  -h, --help             Show help
```

## Configuration

Config precedence: **flags > `LLM_*` env vars > active profile >
`settings.toml` > built-in defaults**.

### Environment variables

| Variable | Purpose | Default |
|---|---|---|
| `LLM_PROVIDER` | `openai` / `anthropic` / `auto` | `auto` |
| `LLM_BASE_URL` | API endpoint base URL | OpenAI: `https://api.minimax.io/v1`<br>Anthropic: `https://api.anthropic.com/v1` |
| `LLM_API_KEY` | API key (Bearer / x-api-key) | — |
| `LLM_MODEL` | Model name | OpenAI: `MiniMax-M3`<br>Anthropic: `claude-sonnet-4-6` |
| `MOOCODE_HOME` | State directory | `~/.moo` |
| `MOOCODE_RTK` | rtk output-compaction (`0`/`1`) | on when `rtk` is on `$PATH` |
| `SEARXNG_URL` | SearXNG instance URL | `http://localhost:8080` |
| `TAVILY_API_KEY` | Tavily API key (search fallback) | — |
| `CLANGD_PATH` | clangd binary | `clangd` |
| `CLANGD_COMPILE_COMMANDS_DIR` | compile_commands.json location | `<root>/build` or `<root>` |
| `CLANGD_INDEX_WAIT_MS` | Wait for clangd index on `rename` | `0` (don't block) |
| `MOOCODE_LSP_DEBUG` | Trace LSP messages to stderr | — |

### Persistent state (`~/.moo/`)

```
~/.moo/
  settings.toml       base_url, model, provider, profile, max_iterations, max_tokens,
                      effort, temperature, thinking, rtk, context_window, [profiles.*]
  credentials.toml    per-profile API keys  (chmod 0600)
  permissions.toml    always-allowed tool list
  history             input-line history
  search_quota.json   Tavily monthly-quota counter
  conversations/
    20260606-143012-a1b9.toml   conv ID = <UTC yyyymmdd-hhmmss>-<cwd hash>
```

### Profiles

Profiles bundle endpoint configuration under `[profiles.<name>]` in
`settings.toml`, with API keys stored separately in `credentials.toml`:

```toml
# settings.toml
profile = "my-gpt"
base_url = "https://api.minimax.io/v1"
model = "MiniMax-M3"

[profiles.my-gpt]
kind = "openai"
base_url = "https://api.openai.com/v1"
model = "gpt-4o-mini"
models = ["gpt-4o-mini", "gpt-4o"]

[profiles.my-claude]
kind = "anthropic"
base_url = "https://api.anthropic.com/v1"
model = "claude-sonnet-4-6"
models = ["claude-sonnet-4-6", "claude-opus-4-8"]
```

```toml
# credentials.toml
my-gpt = "sk-..."
my-claude = "sk-ant-..."
```

Activate with `--profile <name>` or set `profile = "<name>"` in `settings.toml`.
API keys are **never** written to `settings.toml`.

### Generation controls

All opt-in (nothing is sent on the wire unless explicitly set):

- **`--effort`** / `settings.effort`: `low|medium|high|none`.  
  OpenAI → `reasoning_effort`; Anthropic → thinking budget (1024/2048/8192/24576 tokens).
- **`--thinking` / `--no-thinking`** / `settings.thinking`: force reasoning on/off.  
  Anthropic: forces `temperature=1` and lifts `max_tokens` above the budget.
- **`--temperature`** / `settings.temperature`: sampling temperature (≥ 0, default 0.0).

A soft warning is emitted when effort/thinking is enabled on an
OpenAI-compatible model that likely ignores it (heuristic detection).

## Tools

### Built-in filesystem tools

| Tool | Description |
|---|---|
| `read_file` | Read a file, optionally a line window (`offset`, `lines`) |
| `write_file` | Create or overwrite a file with given content |
| `edit_file` | Replace a unique `old` string with `new` (fails on 0 or >1 matches) |
| `list_dir` | List directory entries, sorted, trailing `/` on subdirs |
| `run_bash` | Run `/bin/sh -c` in a process group, capture stdout+stderr |

All filesystem paths are sandboxed under the project root; `..` escapes are
rejected. `run_bash` has a wall-clock timeout (default 30s).
`write_file`/`edit_file` render colored diffs in the TUI and on stderr.

When [`rtk`](https://crates.io/crates/rtk) (a token-optimizing CLI proxy) is on
`$PATH`, `run_bash` transparently rewrites simple allowlisted commands
(`ls tree grep find cargo wc env diff`) to `rtk <cmd>` to shrink their output;
pipelines, redirects and non-allowlisted commands run verbatim, and a leading
`\ ` forces raw output. Toggle with `--rtk`/`--no-rtk`, `MOOCODE_RTK=0|1`, or
`rtk = true|false` in `settings.toml` (default on when available).

### Local git tools (read-only)

Dedicated, read-only git inspection — preferred over raw `git` in `run_bash`.
Each runs `rtk git …` when rtk is available, else plain `git …`.

| Tool | Description |
|---|---|
| `git_status` | Working-tree status (compact) |
| `git_diff` | Diff worktree or index (`staged`), optional `path` |
| `git_log` | Recent commits (`count`, default 20; optional `path`) |
| `git_show` | Show a commit/object (`rev` required) |
| `git_branch` | List branches |

No commit/push/mutating tools exist.

### Web search

| Tool | Description |
|---|---|
| `web_search` | Search via SearXNG (primary) → Tavily (fallback, quota-gated) → DuckDuckGo (last resort) |
| `web_fetch` | Fetch an HTTP(S) URL (capped at 256 KiB) |

### Code intelligence (clangd/LSP)

Seven tools backed by a lazily-spawned clangd session, sharing one
`ClangdSession` across all tools:

| Tool | Description |
|---|---|
| `find_references` | Find all references to a symbol via clangd |
| `go_to_definition` | Jump to a symbol's definition |
| `go_to_implementation` | Find implementations/overrides of a virtual method |
| `hover` | Show a symbol's type, signature, and documentation |
| `list_symbols` | List symbols in a file (`path`) or project-wide (`query`) |
| `call_hierarchy` | Show callers or callees of a function |
| `rename` | Rename a symbol project-wide (applies `WorkspaceEdit`) |

**Addressing** uses `{path, line (1-based), symbol}` — the tool reads the line,
finds the identifier, and builds the LSP position. The model never counts
columns.

Requires a `compile_commands.json` (build with
`-DCMAKE_EXPORT_COMPILE_COMMANDS=ON`). Cross-file results carry a
"still indexing" advisory until clangd's background index finishes.

### Other tools

| Tool | Description |
|---|---|
| `spawn_subagent` | Spawn a sub-agent for complex multi-step sub-tasks |
| `ask_user` | Ask the user a question with a pick-one options list |

## Architecture

Layered static libraries in `src/agent/`, bottom-up with no circular
dependencies. Public headers included as `agent/<name>.hpp` (include root is
`src/`).

```
agent_types     (header-only)  Error, Message, ToolCall, ToolSpec, Conversation
  ↑
agent_http       libcurl RAII: post_json + post_json_stream (SSE)
agent_json       safe, error-returning JSON parse/dump helpers
agent_stream     pure SSE framing + delta assembly (StreamAccumulator,
                 AnthropicStreamAccumulator)
  ↑
agent_provider   Provider seam: OpenAI + Anthropic backends behind one interface.
                 GenerationParams (effort/temperature/thinking/max_tokens).
                 provider_factory = single source of truth for building either
                 backend (used by main startup + TUI runtime swaps).
agent_rtk        pure run_bash command rewrite (rtk <cmd>); no I/O
agent_tools      ToolRegistry + builtin tools + shared run_process(argv) primitive
agent_search     web_search tool: SearXNG → Tavily → DuckDuckGo router
agent_gitea      read-only Gitea v1 REST inspection tools
agent_git        read-only local-git tools (status/diff/log/show/branch), rtk-backed
agent_lsp        clangd-backed code-intelligence tools (LSP JSON-RPC over stdio)
agent_permissions  screen-free approval policy: allowlist + decide()
agent_diff       pure LCS line diff (no I/O)
agent_mentions   @-mention expansion (glob, dir listing, sandboxed)
agent_persist    ~/.moo TOML store (toml++ confined to one TU)
  ↑
agent_core       The agent loop: assemble, dispatch tools, manage history
agent_question   ask_user tool + QuestionGate
agent_subagent   spawn_subagent tool
  ↑
agent_tui        Full-screen FTXUI TUI (two-pane, streaming, diff rendering)
  ↑
moocode (main)   CLI entry point: config resolution, tool registration, dispatch
```

### Error handling

`std::expected<T, Error>` for anything user/network-triggered; asserts for
internal invariants. No exceptions. Tool failures are returned as `Error` and
fed back to the model so it can self-correct.

### The agent loop

1. Push system prompt (if set) + user message.
2. `provider.complete_stream(conversation, tools, on_delta)` — stream the turn.
3. Append the assistant turn (text + tool_calls).
4. No tool_calls → return the text (done).
5. Run each tool_call; parse failures and tool errors become an
   `ERROR: …` tool message so the model can recover.
6. Backstop at `max_iterations` (unlimited by default).

### Tool safety

- **Sandboxing:** all paths confined under `ToolOptions::root` via
  `weakly_canonical` + prefix check; `..` escapes rejected.
- **`edit_file`:** requires `old` to match **exactly once** — 0 or >1 matches
  abort loudly and leave the file untouched.
- **`run_bash`:** forks in its own process group, killed on timeout.
- **Permissions:** every tool call is gated; the always-allow set persists to
  `~/.moo/permissions.toml`.

### Streaming & display

- **CLI:** answer → stdout, reasoning → stderr (dimmed inline counter),
  tool calls → stderr, diffs → stderr (ANSI colored).
- **TUI:** answer deltas → chat pane, reasoning → collapsible blocks,
  tool calls → activity feed (results uniformly middle-elided to first 73 +
  `...` + last 73 bytes when over 150), diffs → inline colored + context-elided.

## Testing

One test executable per library in `tests/`, wired into CTest:

| Test | Coverage |
|---|---|
| `test_http` | libcurl over loopback HTTP server: status codes, headers, timeouts, SSE chunk delivery |
| `test_stream` | Pure streaming: `ThinkSplitter`, `parse_sse_chunk`, `StreamAccumulator`, `AnthropicStreamAccumulator` |
| `test_provider` | OpenAI request/response, streaming, tool calls, generation controls |
| `test_anthropic_provider` | Anthropic request/response, streaming, tool_use blocks, effort→budget, presets |
| `test_builtins` | edit ambiguity, path escaping, bash timeout, read truncation, line-window reads, rtk gate off |
| `test_rtk` | Pure `rtk_rewrite`: allowlist, metachar/pipeline passthrough, git carve-out, prefix non-match, `\ ` escape, `rtk_strip_escape` |
| `test_git` | Read-only git tools via `GitRunFn` seam: argv construction, `rtk git` vs `git` selection, `rev` required, read-only set |
| `test_agent` | Scripted fake Provider: loop termination, history shape, max_iterations, tool errors |
| `test_permissions` | Allowlist, session grant, always-grant persistence, `decide()` |
| `test_diff` | LCS diff: add/delete/modify, unicode, context elision, >5000-line fallback |
| `test_persist` | TOML round-trips: conversations, settings, profiles, credentials (0600) |
| `test_tui` | Screen-free units: `TuiState`, `ApprovalGate`, chat/activity reconstruction |
| `test_mentions` | @-token parsing, sandboxing, globs, caps, suggestion extraction |
| `test_lsp` | Pure LSP helpers + live clangd smoke test (skipped if clangd absent) |

Run all tests:

```sh
ctest --test-dir build --output-on-failure
```

## Deliberately deferred

Async/concurrency, conversation compaction beyond `/compact`, multi-provider
routing, MCP, Anthropic `cache_control` prompt caching, and domain tooling
(mutating Git operations such as commit/push — read-only git inspection tools
already exist; debugger; compiler-output parsing). The `Provider` seam and
`ToolRegistry` are the intended extension points.
