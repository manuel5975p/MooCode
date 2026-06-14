# All public headers are included as "agent/<name>.hpp"; src/ is the include root.
# Each layer is its own target so tests can link it in isolation.
#
# Build types: the standard set plus the project's custom LTO_MINSIZE (defined in
# the root CMakeLists). Its CMAKE_<LANG>_FLAGS_LTO_MINSIZE / CMAKE_EXE_LINKER_FLAGS_
# LTO_MINSIZE are appended to every target below automatically — the static libs
# carry LTO bitcode and the final `flagent` link runs full LTO + strip. No
# per-target flags are needed here. Build it with:
#   cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=LTO_MINSIZE && cmake --build build

# --- agent_types: header-only common value types ----------------------------
add_library(agent_types INTERFACE)
target_include_directories(agent_types INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(agent_types INTERFACE nlohmann_json::nlohmann_json)

# --- agent_json: error-returning helpers over nlohmann::json ----------------
add_library(agent_json STATIC agent/json_util.cpp)
target_link_libraries(agent_json PUBLIC agent_types nlohmann_json::nlohmann_json)

# --- agent_http: libcurl JSON POST client -----------------------------------
add_library(agent_http STATIC agent/http.cpp)
target_link_libraries(agent_http PUBLIC agent_types PRIVATE CURL::libcurl)

# --- agent_stream: pure SSE framing + delta assembly (no I/O) ----------------
add_library(agent_stream STATIC agent/stream.cpp)
target_link_libraries(agent_stream PUBLIC agent_types nlohmann_json::nlohmann_json)

# --- agent_provider: OpenAI-, Anthropic- + Gemini-compatible backends --------
add_library(agent_provider STATIC agent/openai_provider.cpp
                                  agent/anthropic_provider.cpp
                                  agent/gemini_provider.cpp
                                  agent/provider_factory.cpp)
target_link_libraries(agent_provider PUBLIC agent_types
                      PRIVATE agent_http agent_json agent_stream)

# --- agent_rtk: pure run_bash command rewrite (no I/O) ------------------------
add_library(agent_rtk STATIC agent/rtk.cpp)
target_link_libraries(agent_rtk PUBLIC agent_types)

# --- agent_tools: registry + builtin filesystem/shell tools ------------------
add_library(agent_tools STATIC agent/tools.cpp agent/builtin_tools.cpp agent/proc.cpp)
target_link_libraries(agent_tools PUBLIC agent_types agent_rtk PRIVATE agent_json)

# --- agent_search: web-search backends + the web_search tool -----------------
add_library(agent_search STATIC agent/search.cpp)
target_link_libraries(agent_search PUBLIC agent_types agent_tools
                      PRIVATE agent_http agent_json)

# --- agent_gitea: read-only Gitea REST inspection tools -----------------------
add_library(agent_gitea STATIC agent/gitea.cpp)
target_link_libraries(agent_gitea PUBLIC agent_types agent_tools agent_http
                      PRIVATE agent_json)

# --- agent_git: read-only local-git inspection tools (rtk-backed) -------------
add_library(agent_git STATIC agent/git_tools.cpp)
target_link_libraries(agent_git PUBLIC agent_types agent_tools PRIVATE agent_json)

# --- agent_lsp: clangd-backed code-intelligence tools (LSP over stdio) --------
add_library(agent_lsp STATIC agent/lsp_client.cpp agent/clangd_tools.cpp)
target_link_libraries(agent_lsp PUBLIC agent_types agent_tools PRIVATE agent_json)

# --- agent_permissions: tool-approval policy (allowlist + decision glue) ------
add_library(agent_permissions STATIC agent/permissions.cpp)
target_link_libraries(agent_permissions PUBLIC agent_types)

# --- agent_diff: pure LCS line diff (no I/O, no deps but STL) -----------------
add_library(agent_diff STATIC agent/diff.cpp)
target_include_directories(agent_diff PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(agent_diff PUBLIC agent_types)

# --- agent_persist: ~/.flagent TOML store (confines toml.hpp to one TU) -------
add_library(agent_persist STATIC agent/persist.cpp)
target_include_directories(agent_persist
                           PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}
                           PRIVATE ${PROJECT_SOURCE_DIR})  # for "toml.hpp"
target_link_libraries(agent_persist PUBLIC agent_types)

# --- agent_mentions: @-mention expansion (preprocesses user prompts) -------
add_library(agent_mentions STATIC agent/mentions.cpp)
target_link_libraries(agent_mentions PUBLIC agent_types)

# --- agent_core: the agent loop ---------------------------------------------
add_library(agent_core STATIC agent/agent.cpp)
target_link_libraries(agent_core PUBLIC agent_types agent_provider agent_tools
                      PRIVATE agent_json agent_mentions)

# --- agent_question: the ask_user tool + QuestionGate ----------------------
add_library(agent_question STATIC agent/question_tool.cpp)
target_link_libraries(agent_question PUBLIC agent_tools)

# --- agent_subagent: the spawn_subagent tool ---------------------------------
add_library(agent_subagent STATIC agent/subagent_tool.cpp)
target_link_libraries(agent_subagent PUBLIC agent_core agent_tools)

# --- agent_syntax: pure code-block syntax highlighter (no I/O, no deps) -------
add_library(agent_syntax STATIC agent/syntax_highlight.cpp)
target_include_directories(agent_syntax PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(agent_syntax PUBLIC agent_types)

# --- agent_tui: full-screen FTXUI interactive UI -----------------------------
add_library(agent_tui STATIC agent/tui.cpp agent/tui_text.cpp agent/tui_worker.cpp
                      agent/tui_slash.cpp agent/settings_editor.cpp)
target_link_libraries(agent_tui PUBLIC agent_core agent_permissions agent_diff
                      agent_tools agent_question
                      PRIVATE agent_persist agent_mentions agent_syntax agent_json
                          ftxui::screen ftxui::dom ftxui::component)

# --- flagent: the CLI executable --------------------------------------------
add_executable(flagent agent/main.cpp)
# ProgramOptions.hxx is a single header living at the project root.
target_include_directories(flagent PRIVATE ${PROJECT_SOURCE_DIR})
# agent_tui transitively pulls in agent_core (-> agent_provider, agent_tools,
# agent_http, agent_mentions), agent_permissions, agent_diff and agent_persist,
# so only the libraries not reachable through it are listed explicitly. Listing
# the transitively-linked archives again makes the linker warn about (and ignore)
# duplicate libraries.
target_link_libraries(flagent PRIVATE agent_tui agent_search agent_gitea agent_git agent_subagent agent_lsp agent_question)
