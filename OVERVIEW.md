# moocode — Project Overview

**A minimal but real coding agent in C++23** — an LLM loop + tools, talking to
any OpenAI-compatible `/chat/completions` endpoint, any Anthropic-compatible
`/v1/messages` endpoint, or Gemini `/v1beta/models/*:generateContent`, with an
interactive FTXUI TUI and an optional Qt desktop window.

---

## Key Facts

| Aspect | Detail |
|---|---|
| **Language** | C++23 (`std::expected`) |
| **Build** | CMake ≥ 3.20, Ninja |
| **Binary deps** | nlohmann_json, FTXUI v6.1.9 (fetched), libcurl (bundled static by default), Qt 6 Widgets (optional, `moogui` only) |
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
agent_provider      Provider interface → OpenAI + Anthropic + Gemini backends
agent_rtk           run_bash command rewrite (rtk <cmd>); no I/O
agent_tools         ToolRegistry: name → (spec, fn)
agent_search        web_search tool: SearXNG → z.ai → Tavily → DuckDuckGo
agent_gitea         read-only Gitea REST inspection tools
agent_git           read-only local-git tools (status/diff/log/show/branch)
agent_lsp           clangd-backed code intelligence (LSP JSON-RPC over stdio)
agent_permissions   screen-free approval policy (allowlist + decide)
agent_diff          pure LCS line diff (no I/O)
agent_persist       ~/.moo TOML store (toml++ confined to one TU)
agent_onboarding    first-run setup flow (profile + credential capture)
agent_mentions      @-mention expansion (glob, dir, sandboxed)
  ↑
agent_core          Agent loop: assemble, dispatch tools, manage history
agent_question      ask_user tool + QuestionGate
agent_subagent      spawn_subagent + spawn_subagent_restricted tools
agent_skills        SkillRegistry + load_skill tool (on-demand capabilities)
agent_syntax        pure code-block syntax highlighter + the theme-name table
moogui_theme        the Qt GUI's colour layer — Qt-free, so it is unit-testable
                    without a display and builds in every preset
moogui_convimport   folds a saved tool-using conversation into a chat-only one
                    for the GUI to resume — Qt-free for the same reason
moogui_attach       clipboard/drop payload → ImageBlock for the GUI composer;
                    needs QtGui, so MOOCODE_GUI=ON only, but no display
  ↑
agent_tui           Full-screen FTXUI TUI (two-pane, streaming, diff rendering)
  ↑
moocode (main)      CLI entry point: config, tool registration, dispatch

listmodels          standalone endpoint probe (provider + persist only)
moogui              standalone Qt6 chat window (src/gui/, -DMOOCODE_GUI=ON)
```

Every library is a separate CMake target so tests can link each in isolation.
Qt is confined to `src/gui/`, the way toml++ is confined to `persist.cpp`: no
`agent_*` target gains a Qt dependency, and the normal build never needs Qt.

---

## Source Files

`src/agent/`, plus `src/gui/` for the Qt frontend:

| Category | Files |
|---|---|
| **Core types** | `types.hpp` |
| **JSON** | `json_util.{hpp,cpp}` |
| **HTTP** | `http.{hpp,cpp}` |
| **Streaming** | `stream.{hpp,cpp}`, `stream_detail.hpp` |
| **Providers** | `provider.hpp`, `provider_factory.{hpp,cpp}`, `openai_provider.{hpp,cpp}`, `anthropic_provider.{hpp,cpp}`, `gemini_provider.{hpp,cpp}`, `retry.hpp` |
| **Tools** | `tools.{hpp,cpp}`, `builtin_tools.{hpp,cpp}` |
| **Search** | `search.{hpp,cpp}`, `search_internal.hpp` |
| **Gitea** | `gitea.{hpp,cpp}` |
| **Local git** | `git_tools.{hpp,cpp}` |
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
| **Settings editor** | `settings_editor.{hpp,cpp}` |
| **Entry points** | `main.cpp` (moocode), `listmodels.cpp` |
| **Shared helpers** | `strutil.hpp`, `fsutil.hpp`, `trace.hpp` (opt-in stall/render log, `$MOOCODE_TRACE_STREAM`) |

---

## Tools

### Filesystem (sandboxed under project root)
- `read_file` — line-window reads
- `write_file` — create/overwrite
- `edit_file` — exact unique match replace (fails on 0 or >1 matches)
- `list_dir` — sorted listing
- `run_bash` — `/bin/sh -c` in process group with timeout; optionally rewrites through `rtk` for token compaction

### Local Git (read-only)
- `git_status`, `git_diff`, `git_log`, `git_show`, `git_branch`

### Web
- `web_search` — SearXNG → z.ai (GLM, credential-gated) → Tavily (quota-gated) → DuckDuckGo
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
- `load_skill` — pull in an optional, on-demand skill (registers extra tools)

Skills are named capability bundles kept **out** of the default tool list and
system prompt, loaded on demand via `load_skill` (model) or `/skill` (user).
`spawn_subagent_restricted` ships as one: it spawns a sub-agent confined to a
chosen tool subset with its own auto-approve allowlist.

---

## Provider Support

Three backends behind a single `Provider` interface:

| Backend | Endpoint | Auth |
|---|---|---|
| **OpenAI** | `/chat/completions` | `Authorization: Bearer` |
| **Anthropic** | `/v1/messages` | `x-api-key` + `anthropic-version` |
| **Gemini** | `/v1beta/models/*:generateContent` | `x-goog-api-key` |

Auto-detected from hostname. Built-in profiles: `minimax`, `deepseek`,
`anthropic`, `openai`, `qwen`, `glm`, `gemini`, `gemini-openai`, `grok`,
`kimi`.

Generation controls, all opt-in — unset means the key is omitted from the
request and the server's own default applies: `effort` (low/medium/high/none),
`thinking` (on/off/backend default), `temperature` (≥ 0). A profile can pin a
temperature its endpoint demands (`kimi` pins `1.0`, the only value its coding
endpoint accepts) and its own `thinking.type` dialect (`enabled` for
z.ai/DeepSeek, `adaptive` for MiniMax, which rejects `enabled`).

5xx and transport failures are retried with backoff (`retry.hpp`), and a
per-profile model blacklist drops advertised-but-dead ids from detection.

---

## Interactive TUI

Two-pane FTXUI interface:
- **Left pane**: chat (prose, collapsible reasoning, inline diffs)
- **Right pane**: activity tree (clickable tool-call rows, subagent nesting, detail pane)
- **Status bar**: model, CWD, token counter, thinking indicator

**Slash commands**: `/model`, `/provider`, `/settings`, `/effort`, `/thinking`,
`/temp`, `/theme`, `/system`, `/continue`, `/resume`, `/rewind`, `/fork`,
`/compact`, `/skill`, `/skills`, `/paste`, `/history`, `/clear`.

**Clipboard**: tool-approval modal (once/session/always/deny), `@`-autocomplete.

---

## Qt GUI (`moogui`)

A second frontend, off by default (`-DMOOCODE_GUI=ON`, needs Qt 6 Widgets): a
desktop chat window rendering GitHub-flavoured Markdown, with fenced code
coloured by `agent_syntax` so a code block looks the same as in the TUI. It
shares `~/.moo/settings.toml` and `conversations/` with the TUI.

A settings dropdown covers profile, model (declared list + endpoint detection +
free-text entry), effort, thinking, temperature and appearance — theme plus
three font slots (interface, chat, code), stored in a GUI-only `[gui]` table
the other frontends preserve but ignore. The prose default is EB Garamond,
embedded in the binary under the OFL.

Images can be pasted (Ctrl+V) or dropped onto the composer: each is staged as a
chip with a thumbnail and detach button, and the turn is sent as multimodal
content parts. A pasted bitmap is capped at 1568px on its long edge; a dropped
file is sent as it is on disk.

Chat only for now: the agent runs with `advertise_tools = false` and an empty
registry, since the GUI has no approval modal.

---

## Testing

36 test executables in `tests/`, one per library, registered with CTest. Run:

```sh
ctest --test-dir build --output-on-failure
```

`test_gui_theme` and `test_conversation_import` cover Qt frontend logic while
needing neither Qt nor a display, so they run in every preset — including the
sanitizer ones, and on machines with no Qt installed. `test_image_attach` is the
one exception: it needs QtGui for `QImage`, so it is registered only with
`-DMOOCODE_GUI=ON`, though it still runs headless.

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
├── settings.toml       base_url, model, provider, profiles, generation params,
│                       theme, and [gui] (moogui fonts only)
├── credentials.toml    per-profile API keys + optional `zai` search key (0600)
├── permissions.toml    always-allowed tool list
├── history             input-line history
├── search_quota.json   Tavily monthly-quota counter
└── conversations/
    └── <yyyymmdd-hhmmss>-<cwd-hash>.toml
```

---

## Deliberate Scope

What's **in**: single-provider agent loop, file + shell + web + LSP + Gitea +
read-only git tools, interactive TUI, an optional Qt chat window, sub-agent
spawning, on-demand skills, conversation persistence, permission gating,
streaming display.

What's **deferred**: async/concurrency, multi-provider routing, MCP, Anthropic
`cache_control`, conversation compaction beyond `/compact`, mutating Git tools
(commit/push), debugger/compiler tooling, and tool use in the Qt GUI (it has no
approval modal yet).
