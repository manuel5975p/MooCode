# One test executable per library. Each links test_main (the runner) plus the
# header-only harness, and is registered with CTest.

# moocode_add_test(<name> <source> <libs...>)
function(moocode_add_test name source)
  add_executable(${name} ${source} ${CMAKE_CURRENT_SOURCE_DIR}/test_main.cpp)
  target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  target_link_libraries(${name} PRIVATE ${ARGN})
  add_test(NAME ${name} COMMAND ${name})
endfunction()

moocode_add_test(test_json test_json.cpp agent_json)
moocode_add_test(test_http test_http.cpp agent_http)
find_package(Threads REQUIRED)
target_link_libraries(test_http PRIVATE Threads::Threads)

moocode_add_test(test_stream test_stream.cpp agent_stream agent_json)

moocode_add_test(test_provider test_provider.cpp agent_provider agent_json)
target_link_libraries(test_provider PRIVATE Threads::Threads)

moocode_add_test(test_anthropic_provider test_anthropic_provider.cpp
                 agent_provider agent_json agent_stream)
target_link_libraries(test_anthropic_provider PRIVATE Threads::Threads)

moocode_add_test(test_gemini_provider test_gemini_provider.cpp
                 agent_provider agent_json agent_stream)
target_link_libraries(test_gemini_provider PRIVATE Threads::Threads)

moocode_add_test(test_tools test_tools.cpp agent_tools)
moocode_add_test(test_builtins test_builtins.cpp agent_tools)
moocode_add_test(test_agent test_agent.cpp agent_core)
moocode_add_test(test_permissions test_permissions.cpp agent_permissions)
moocode_add_test(test_search test_search.cpp agent_search agent_json)
moocode_add_test(test_mentions test_mentions.cpp agent_mentions)

# test_gitea: pure formatters + GiteaClient/tools over a canned HTTP transport
moocode_add_test(test_gitea test_gitea.cpp agent_gitea agent_json)
set_tests_properties(test_gitea PROPERTIES TIMEOUT 30)

# test_git: read-only local-git tool factory over a recording GitRunFn seam
moocode_add_test(test_git test_git.cpp agent_git agent_json)
set_tests_properties(test_git PROPERTIES TIMEOUT 30)

# test_lsp: pure LSP helpers (hermetic) + a live clangd smoke test that skips
# itself when no clangd binary is available.
moocode_add_test(test_lsp test_lsp.cpp agent_lsp)
set_tests_properties(test_lsp PROPERTIES TIMEOUT 60)

moocode_add_test(test_rtk test_rtk.cpp agent_rtk)
moocode_add_test(test_types test_types.cpp agent_types)
moocode_add_test(test_diff test_diff.cpp agent_diff)
moocode_add_test(test_syntax test_syntax.cpp agent_syntax)
moocode_add_test(test_persist test_persist.cpp agent_persist agent_types)
set_tests_properties(test_persist PROPERTIES TIMEOUT 30)

# test_tui drives the screen-free TuiState + the threaded ApprovalGate, plus
# the patched FTXUI width/clipping behaviour on an off-screen ftxui::Screen.
moocode_add_test(test_tui test_tui.cpp agent_tui)
target_link_libraries(test_tui PRIVATE Threads::Threads ftxui::screen ftxui::dom)
set_tests_properties(test_tui PROPERTIES TIMEOUT 30)

# test_tui_worker: the background work queue (submit->pop FIFO, per-kind
# dispatch, stop()/dtor join without deadlock) over recording closures.
moocode_add_test(test_tui_worker test_tui_worker.cpp agent_tui)
target_link_libraries(test_tui_worker PRIVATE Threads::Threads)
set_tests_properties(test_tui_worker PROPERTIES TIMEOUT 30)

# test_tui_slash: pure generation-control slash parsers (/effort, /thinking,
# /temp, /theme) — the decision logic extracted from the inline handlers.
moocode_add_test(test_tui_slash test_tui_slash.cpp agent_tui)
set_tests_properties(test_tui_slash PROPERTIES TIMEOUT 30)

# test_settings_editor: screen-free settings form (FieldDef, apply_value,
# display_value, navigation, SettingsForm state machine)
moocode_add_test(test_settings_editor test_settings_editor.cpp agent_tui)
set_tests_properties(test_settings_editor PROPERTIES TIMEOUT 30)

# test_onboarding: first-run wizard (pure helpers + driver over injected streams
# and a fake model-lister, fully offline)
moocode_add_test(test_onboarding test_onboarding.cpp agent_onboarding agent_persist)
target_link_libraries(test_onboarding PRIVATE Threads::Threads)
set_tests_properties(test_onboarding PROPERTIES TIMEOUT 30)

# test_question: QuestionGate sync + ask_user tool parsing
moocode_add_test(test_question test_question.cpp agent_question)
target_link_libraries(test_question PRIVATE Threads::Threads)
set_tests_properties(test_question PROPERTIES TIMEOUT 10)

# test_compact_cancel: verifies cancel flag resets between compact() calls
moocode_add_test(test_compact_cancel test_compact_cancel.cpp agent_core)
target_link_libraries(test_compact_cancel PRIVATE Threads::Threads)
set_tests_properties(test_compact_cancel PROPERTIES TIMEOUT 10)

# test_compact: verifies compact() summarises the bulk and keeps a recent tail
moocode_add_test(test_compact test_compact.cpp agent_core)
target_link_libraries(test_compact PRIVATE Threads::Threads)
set_tests_properties(test_compact PROPERTIES TIMEOUT 10)

# test_subagent: spawn_subagent model/provider override (offline, fake provider)
moocode_add_test(test_subagent test_subagent.cpp agent_subagent)
set_tests_properties(test_subagent PROPERTIES TIMEOUT 30)

# test_subagent_restricted: spawn_subagent_restricted tool/permission filtering
moocode_add_test(test_subagent_restricted test_subagent_restricted.cpp agent_subagent)
target_link_libraries(test_subagent_restricted PRIVATE Threads::Threads)
set_tests_properties(test_subagent_restricted PROPERTIES TIMEOUT 30)

# test_skills: SkillRegistry + load_skill tool (offline)
moocode_add_test(test_skills test_skills.cpp agent_skills)
target_link_libraries(test_skills PRIVATE Threads::Threads)
set_tests_properties(test_skills PROPERTIES TIMEOUT 10)
