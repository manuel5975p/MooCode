#include "agent/rtk.hpp"

#include <optional>
#include <string>

#include "test_harness.hpp"

using namespace moocode;

TEST("rtk_rewrite_wraps_allowlisted") {
    CHECK_EQ(rtk_rewrite("ls").value(), std::string("rtk ls"));
    CHECK_EQ(rtk_rewrite("grep -n foo bar.txt").value(),
             std::string("rtk grep -n foo bar.txt"));
    CHECK_EQ(rtk_rewrite("cargo build").value(), std::string("rtk cargo build"));
    CHECK_EQ(rtk_rewrite("  tree  src ").value(), std::string("rtk tree  src"));
}

TEST("rtk_rewrite_passes_through_unknown") {
    CHECK(!rtk_rewrite("python script.py").has_value());
    CHECK(!rtk_rewrite("./configure").has_value());
    CHECK(!rtk_rewrite("").has_value());
}

TEST("rtk_rewrite_skips_git") {
    CHECK(!rtk_rewrite("git status").has_value());  // git carved out -> git tools
}

TEST("rtk_rewrite_skips_pipelines_and_redirects") {
    CHECK(!rtk_rewrite("ls | head").has_value());
    CHECK(!rtk_rewrite("grep x f && echo done").has_value());
    CHECK(!rtk_rewrite("find . > out.txt").has_value());
    CHECK(!rtk_rewrite("ls; ls").has_value());
    CHECK(!rtk_rewrite("env $(cat f)").has_value());
    CHECK(!rtk_rewrite("ls `pwd`").has_value());
    CHECK(!rtk_rewrite("ls\nls").has_value());
}

TEST("rtk_rewrite_backslash_space_forces_raw") {
    CHECK(!rtk_rewrite("\\ ls").has_value());  // "\ ls" -> nullopt (run raw)
}

TEST("rtk_rewrite_prefix_must_not_match") {
    // Whole-word allowlist match: a command merely starting with an allowlisted
    // name must NOT be wrapped.
    CHECK(!rtk_rewrite("lsblk").has_value());
    CHECK(!rtk_rewrite("findutils --version").has_value());
    CHECK(!rtk_rewrite("LS").has_value());  // case-sensitive
}

TEST("rtk_rewrite_whitespace_only") {
    CHECK(!rtk_rewrite("   ").has_value());
    CHECK(!rtk_rewrite("\t\t").has_value());
    CHECK_EQ(rtk_rewrite("\tls\t").value(), std::string("rtk ls"));  // tabs trimmed
}

TEST("rtk_strip_escape") {
    CHECK_EQ(rtk_strip_escape("\\ ls"), std::string("ls"));        // prefix stripped
    CHECK_EQ(rtk_strip_escape("ls"), std::string("ls"));           // no escape -> unchanged
    CHECK_EQ(rtk_strip_escape("\\ls"), std::string("\\ls"));       // backslash w/o space kept
    CHECK_EQ(rtk_strip_escape(""), std::string(""));               // empty -> empty
}
