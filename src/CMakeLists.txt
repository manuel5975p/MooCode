# All public headers are included as "agent/<name>.hpp"; src/ is the include root.
# Each layer is its own target so tests can link it in isolation.
#
# Build types: the standard set plus the project's custom LTO_MINSIZE (defined in
# the root CMakeLists). Its CMAKE_<LANG>_FLAGS_LTO_MINSIZE / CMAKE_EXE_LINKER_FLAGS_
# LTO_MINSIZE are appended to every target below automatically — the static libs
# carry LTO bitcode and the final `moocode` link runs full LTO + strip. No
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

# --- agent_persist: ~/.moo TOML store (confines toml.hpp to one TU) -------
add_library(agent_persist STATIC agent/persist.cpp)
target_include_directories(agent_persist
                           PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
# toml.hpp is third-party: SYSTEM keeps its warnings/analyzer findings out.
target_include_directories(agent_persist SYSTEM PRIVATE ${PROJECT_SOURCE_DIR})
target_link_libraries(agent_persist PUBLIC agent_types)

# --- agent_onboarding: first-run setup wizard (settings.toml + credentials) ---
add_library(agent_onboarding STATIC agent/onboarding.cpp)
target_link_libraries(agent_onboarding PUBLIC agent_types agent_provider agent_persist)

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

# --- agent_skills: dynamically loadable skills + the load_skill tool ----------
add_library(agent_skills STATIC agent/skills.cpp)
target_link_libraries(agent_skills PUBLIC agent_tools PRIVATE agent_json)

# --- agent_syntax: pure code-block syntax highlighter (no I/O, no deps) -------
add_library(agent_syntax STATIC agent/syntax_highlight.cpp)
target_include_directories(agent_syntax PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(agent_syntax PUBLIC agent_types)

# --- agent_tui: full-screen FTXUI interactive UI -----------------------------
# tui_platform_{win,posix}.cpp isolate the OS clipboard/console code so tui.cpp
# never pulls in <windows.h>; compile the one matching the host.
if(WIN32)
  set(_TUI_PLATFORM agent/tui_platform_win.cpp)
else()
  set(_TUI_PLATFORM agent/tui_platform_posix.cpp)
endif()
add_library(agent_tui STATIC agent/tui.cpp agent/tui_text.cpp agent/tui_worker.cpp
                      agent/tui_slash.cpp agent/settings_editor.cpp
                      agent/tui_glyphs.cpp ${_TUI_PLATFORM})
target_link_libraries(agent_tui PUBLIC agent_core agent_permissions agent_diff
                      agent_tools agent_question agent_skills
                      PRIVATE agent_persist agent_mentions agent_syntax agent_json
                          ftxui::screen ftxui::dom ftxui::component)
if(WIN32)
  target_link_libraries(agent_tui PRIVATE user32)  # Win32 clipboard API
endif()

# --- moocode: the CLI executable --------------------------------------------
add_executable(moocode agent/main.cpp)
# ProgramOptions.hxx is a single header living at the project root.
target_include_directories(moocode PRIVATE ${PROJECT_SOURCE_DIR})
# agent_tui transitively pulls in agent_core (-> agent_provider, agent_tools,
# agent_http, agent_mentions), agent_permissions, agent_diff and agent_persist,
# so only the libraries not reachable through it are listed explicitly. Listing
# the transitively-linked archives again makes the linker warn about (and ignore)
# duplicate libraries.
target_link_libraries(moocode PRIVATE agent_tui agent_search agent_gitea agent_git agent_subagent agent_lsp agent_question agent_onboarding)

# --- listmodels: standalone endpoint probe ----------------------------------
# Lists what an endpoint advertises via Provider::list_models(). Needs only the
# provider backends and the ~/.moo store — no agent loop, tools or TUI.
add_executable(listmodels agent/listmodels.cpp)
target_include_directories(listmodels PRIVATE ${PROJECT_SOURCE_DIR})
target_link_libraries(listmodels PRIVATE agent_provider agent_persist)

# --- moogui_theme: the GUI's colour layer (pure; no Qt) ----------------------
# Built unconditionally even though only moogui consumes it: it is Qt-free by
# construction, and keeping it out of the MOOCODE_GUI guard means test_gui_theme
# runs in every preset, including the sanitizer ones, on machines without Qt.
add_library(moogui_theme STATIC gui/theme.cpp)
target_include_directories(moogui_theme PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(moogui_theme PUBLIC agent_types agent_syntax)

# --- moogui_convimport: folding a saved conversation into a chat-only one ----
# Qt-free for the same reason moogui_theme is: it holds rules worth testing
# (what survives the fold, and whether the source file may be written back to)
# and a test for them should not need a display or a Qt install.
add_library(moogui_convimport STATIC gui/conversation_import.cpp)
target_include_directories(moogui_convimport PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(moogui_convimport PUBLIC agent_types)

# --- moogui: the Qt6 chat window --------------------------------------------
# A second frontend beside the FTXUI TUI, sharing everything below agent_tui
# (the agent loop, the providers, ~/.moo and the syntax highlighter) and none of
# its rendering. Qt is confined to src/gui/, the way toml.hpp is confined to
# persist.cpp — no agent_* target gains a Qt dependency.
#
# Off by default: the dev/asan/tsan presets and every existing build must keep
# working on a machine with no Qt installed. Enable with -DMOOCODE_GUI=ON.
#
# Deliberately no Qt6::Network — all HTTP goes through agent_http/libcurl, so
# the bundled static OpenSSL never meets a second TLS stack.
option(MOOCODE_GUI "Build the Qt6 chat GUI (moogui)" OFF)
if(MOOCODE_GUI)
  find_package(Qt6 REQUIRED COMPONENTS Widgets Gui)
  # --- moogui_attach: clipboard/drop payload -> ImageBlock -------------------
  # A library rather than another moogui source, so a test can exercise the
  # encode rules (passthrough vs re-encode, the downscale) against a hand-built
  # QMimeData. Needs QtGui for QImage, hence the guard — but no widgets and no
  # display: everything it does is in-memory.
  add_library(moogui_attach STATIC gui/image_attach.cpp)
  set_target_properties(moogui_attach PROPERTIES AUTOMOC OFF)
  target_include_directories(moogui_attach PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
  target_link_libraries(moogui_attach PUBLIC agent_types Qt6::Gui)

  add_executable(moogui gui/main.cpp gui/main_window.cpp gui/chat_panel.cpp
                        gui/markdown_view.cpp gui/agent_bridge.cpp
                        gui/settings_menu.cpp gui/moogui.qrc)
  # conversation_import.cpp is linked in as a library, not compiled here, so the
  # test and the GUI share one build of it.
  # Per-target rather than the global CMAKE_AUTOMOC, so no agent_* target pays
  # the moc scan.
  set_target_properties(moogui PROPERTIES AUTOMOC ON AUTORCC ON)
  target_include_directories(moogui PRIVATE ${PROJECT_SOURCE_DIR})
  target_link_libraries(moogui PRIVATE agent_core agent_provider agent_persist
                                       agent_syntax moogui_theme
                                       moogui_convimport moogui_attach
                                       Qt6::Widgets)
endif()
