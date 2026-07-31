# MOO.md — moocode

A minimal but real coding agent in C++23: an LLM loop + tools, with an FTXUI interactive TUI, talking to any OpenAI-compatible `/chat/completions` endpoint, any Anthropic-compatible `/v1/messages` endpoint, or Gemini `/v1beta/models/*:generateContent`.

## Key Facts

| Aspect | Detail |
|---|---|
| **Language** | C++23 (`std::expected`) |
| **Build** | CMake ≥ 3.20, Ninja |
| **Binary deps** | nlohmann_json, FTXUI v6.1.9 (fetched), libcurl (bundled static by default), Qt 6 Widgets (optional, `moogui` only) |
| **Test framework** | Catch2-style (`test_harness.hpp`) via CTest |
| **Error model** | `std::expected<T, Error>` — no exceptions |
| **Config precedence** | flags > `LLM_*` env vars > active profile > `settings.toml` > defaults |
| **State dir** | `~/.moo/` (configurable via `$MOOCODE_HOME`) |

## Architecture

Layered static libraries in `src/agent/`, bottom-up, no circular deps. Public headers included as `agent/<name>.hpp`.

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
agent_search        web_search tool: SearXNG → Tavily → DuckDuckGo
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
moogui_theme        the Qt GUI's colour layer — deliberately Qt-free, so it is
                    unit-testable without a display and builds in every preset
moogui_convimport   folds a saved tool-using conversation into a chat-only one
                    for the GUI to resume — Qt-free for the same reason
moogui_attach       clipboard/drop payload → ImageBlock for the GUI composer.
                    Needs QtGui (QImage), so it exists only with MOOCODE_GUI=ON,
                    but no display: its test runs headless
  ↑
agent_tui           Full-screen FTXUI TUI (two-pane, streaming, diff rendering)
  ↑
moocode (main)      CLI entry point: config, tool registration, dispatch

listmodels          standalone endpoint probe (provider + persist only)
moogui              standalone Qt6 chat window (src/gui/, -DMOOCODE_GUI=ON)
```

## Provider Support

Three backends behind a single `Provider` interface, auto-detected from hostname:

| Backend | Endpoint | Auth |
|---|---|---|
| **OpenAI** | `/chat/completions` | `Authorization: Bearer` |
| **Anthropic** | `/v1/messages` | `x-api-key` + `anthropic-version` |
| **Gemini** | `/v1beta/models/*:generateContent` | `x-goog-api-key` |

Built-in profiles (used when no `[profiles.*]` are configured): `minimax`,
`deepseek`, `anthropic`, `openai`, `qwen`, `glm`, `gemini`, `gemini-openai`,
`grok`, `kimi`.

Generation controls, all opt-in — unset means the key is omitted from the
request and the server's own default applies:

| Control | Values | Notes |
|---|---|---|
| `effort` | low / medium / high / none | OpenAI `reasoning_effort`; Anthropic thinking budget |
| `thinking` | on / off / backend default | `thinking.type` dialect is per profile (see below) |
| `temperature` | ≥ 0, or unset | flag > settings > profile pin |

A profile can pin a `temperature` its endpoint demands — the built-in `kimi`
entry pins `1.0`, the only value its coding endpoint accepts. OpenAI-compatible
endpoints also disagree on `thinking.type` (`enabled` for z.ai/DeepSeek,
`adaptive` for MiniMax, which rejects `enabled` outright), so that too is a
per-profile field.

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
- `web_search` — SearXNG → Tavily → DuckDuckGo fallback chain
- `web_fetch` — HTTP GET (capped 256 KiB)

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

### Skills (optional, on-demand)
Named capability bundles kept **out** of the default tool list and system
prompt; loaded when needed via the `load_skill` tool (model) or the `/skill`
command (user). Loading registers the skill's tools into the live registry for
the next turn.
- `spawn_subagent_restricted` — adds the `spawn_subagent_restricted` tool: spawn
  a sub-agent confined to a chosen tool subset (`tools` arg) with an explicit
  auto-approve allowlist (`permissions` arg); unlisted tools defer to the normal
  approval gate.

## Interactive TUI

FTXUI two-pane interface:
- **Left pane**: chat (prose, collapsible reasoning, inline diffs)
- **Right pane**: activity tree (tool-call rows, subagent nesting)
- **Status bar**: model, CWD, token counter, thinking indicator

Slash commands: `/model`, `/provider`, `/settings`, `/effort`, `/thinking`,
`/temp`, `/theme`, `/system`, `/continue`, `/resume`, `/rewind`, `/fork`,
`/compact`, `/skill`, `/skills`, `/paste`, `/history`, `/clear`.

Permission modal before each tool call: `[y]` once / `[s]` session / `[a]` always / `[n]` deny.

## Qt GUI (`moogui`)

A second frontend, off by default (`-DMOOCODE_GUI=ON`, needs Qt 6 Widgets):
a desktop chat window rendering full GitHub-flavoured Markdown via
`QTextDocument::setMarkdown`, with fenced code coloured by `agent_syntax`.

Qt is confined to `src/gui/` exactly as toml++ is confined to `persist.cpp` —
no `agent_*` target gains a Qt dependency. Markdown is imported and then walked
by a pass that writes concrete formats into the document, because
`setDefaultStyleSheet` applies to `setHtml()` only and a markdown-imported
document arrives with no character formats at all.

Settings dropdown: profile, model (declared list + endpoint detection + free
text entry), effort, thinking, temperature, system prompt, and appearance
(theme, three font slots, text size). Shares `~/.moo/settings.toml` and
`conversations/` with the TUI; the fonts and the system prompt live in a
GUI-only `[gui]` table the other frontends preserve but ignore — the TUI's own
prompt is a `{TOOLS}`/`{DIR}` template and would be wrong for a chat window,
and vice versa.

Chats dropdown: new conversation (Ctrl+N), the eight most recent conversations
in the working directory, and a browser over all of them. `moogui --continue`
opens the latest in the working directory at startup. Because the two frontends
write the same files, this resumes TUI sessions too — tool calls and their
results are folded into the assistant turn that made them (`to_chat_only`), so
the chat-only agent can send the history and the transcript still shows what
happened. That fold is lossy, so a conversation resumed from a tool-using
session continues into a *new* file rather than overwriting the original.

Three font slots, each falling back to the one before it: **interface**
(the whole window), **chat** (the transcript alone, so the messages can differ
from the chrome) and **code** (fenced and inline code, restricted to
monospaced families). The prose default is EB Garamond, embedded in the binary
from `src/gui/fonts/` under the OFL — a default face that depends on the
machine having a font installed is not a default.

Images: Ctrl+V in the composer attaches a clipboard image instead of pasting it,
and dropping image files on the composer does the same. Each staged image gets a
chip above the input (thumbnail, name, ✕ to detach) — the GUI's answer to the
TUI's `[img#N]` markers, which have to live inside the prompt text because a
terminal has nowhere else to put them. On send the turn goes out as multimodal
content parts (text first, then one part per image) and the thumbnails stay in
the transcript. Files are sent as they are on disk; a pasted bitmap is downscaled
to 1568px on its long edge and re-encoded, unless the clipboard also offered the
original compressed bytes and no rescale is needed, in which case those go out
untouched (`gui/image_attach.cpp`, tested in `test_image_attach`). The bytes are
not saved with the conversation, so an image-only turn is sent — and recorded —
as `[image #N]` prose.

Chat only for now: the agent runs with `advertise_tools = false` and an empty
registry, since the GUI has no approval modal.

## Bundled Dependencies

Default (`-DMOOCODE_BUNDLED_CURL=ON`): fully static, feature-stripped curl chain from source:
```
zlib 1.3.1 → nghttp2 1.65.0 → OpenSSL 3.5.0 → curl 8.15.0
```
Result: HTTPS + HTTP/2 + gzip only. Tarballs cached in `.deps-cache/`, hash-verified.

## Testing

36 test executables in `tests/`, one per library, via CTest:

```sh
cmake -S . -B build -G Ninja -DBUILD_TESTING=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

CI presets (via `CMakePresets.json`):
- `dev` — Release, `MOOCODE_WERROR=ON`
- `asan` — Debug, `MOOCODE_SANITIZE=address;undefined`
- `tsan` — Debug, `MOOCODE_SANITIZE=thread`

## Config Files

```
~/.moo/
├── settings.toml       base_url, model, provider, profiles, generation params,
│                       theme, and [gui] (moogui only: font, chat_font,
│                       mono_font and their sizes, system_prompt)
├── credentials.toml    per-profile API keys (0600)
├── permissions.toml    always-allowed tool list
├── history             input-line history
├── search_quota.json   Tavily monthly-quota counter
└── conversations/
    └── <yyyymmdd-hhmmss>-<cwd-hash>.toml
```

## Deliberate Scope

**In**: single-provider agent loop, file + shell + web + LSP + Gitea + git tools, interactive TUI, sub-agent spawning, conversation persistence, permission gating, streaming display.

**Deferred**: async/concurrency, multi-provider routing, MCP, Anthropic `cache_control`, conversation compaction beyond `/compact`, mutating Git tools (commit/push), debugger/compiler tooling.
