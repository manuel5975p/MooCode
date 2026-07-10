#include "agent/builtin_tools.hpp"

#include <string>

#include "temp_dir.hpp"
#include "test_harness.hpp"

using namespace moocode;

namespace {
ToolOptions opts_for(const test::TempDir& d) {
    ToolOptions o;
    o.root = d.path();
    o.max_read_bytes = 64;       // small cap to test truncation
    o.bash_timeout_secs = 2;
    return o;
}
nlohmann::json args(std::initializer_list<std::pair<const char*, std::string>> kv) {
    nlohmann::json j = nlohmann::json::object();
    for (auto& [k, v] : kv) j[k] = v;
    return j;
}
}  // namespace

// --- read_file --------------------------------------------------------------

TEST("read_file: returns contents of an existing file") {
    test::TempDir d;
    d.write("a.txt", "hello world");
    auto t = read_file_tool(opts_for(d));
    auto r = t.run(args({{"path", "a.txt"}}));
    CHECK(r.has_value());
    if (r) CHECK(r->find("hello world") != std::string::npos);
}

TEST("read_file: missing file returns Error") {
    test::TempDir d;
    auto t = read_file_tool(opts_for(d));
    auto r = t.run(args({{"path", "nope.txt"}}));
    CHECK(!r.has_value());
}

TEST("read_file: oversized file is truncated and notes truncation") {
    test::TempDir d;
    d.write("big.txt", std::string(500, 'x'));  // > 64-byte cap
    auto t = read_file_tool(opts_for(d));
    auto r = t.run(args({{"path", "big.txt"}}));
    CHECK(r.has_value());
    if (r) {
        CHECK(r->size() < 500);
        std::string low = *r;
        for (auto& c : low) c = static_cast<char>(::tolower(c));
        CHECK(low.find("truncat") != std::string::npos);
    }
}

TEST("read_file: offset reads from a 1-based line to the end") {
    test::TempDir d;
    d.write("lines.txt", "l1\nl2\nl3\nl4\n");
    auto t = read_file_tool(opts_for(d));
    nlohmann::json a;
    a["path"] = "lines.txt";
    a["offset"] = 2;
    auto r = t.run(a);
    CHECK(r.has_value());
    if (r) CHECK_EQ(*r, std::string("l2\nl3\nl4\n"));
}

TEST("read_file: lines caps the window size") {
    test::TempDir d;
    d.write("lines.txt", "l1\nl2\nl3\nl4\n");
    auto t = read_file_tool(opts_for(d));
    nlohmann::json a;
    a["path"] = "lines.txt";
    a["offset"] = 2;
    a["lines"] = 2;
    auto r = t.run(a);
    CHECK(r.has_value());
    if (r) CHECK_EQ(*r, std::string("l2\nl3\n"));
}

TEST("read_file: lines without offset starts at line 1") {
    test::TempDir d;
    d.write("lines.txt", "l1\nl2\nl3\n");
    auto t = read_file_tool(opts_for(d));
    nlohmann::json a;
    a["path"] = "lines.txt";
    a["lines"] = 2;
    auto r = t.run(a);
    CHECK(r.has_value());
    if (r) CHECK_EQ(*r, std::string("l1\nl2\n"));
}

TEST("read_file: last line without trailing newline is returned") {
    test::TempDir d;
    d.write("lines.txt", "l1\nl2\nl3");  // no final newline
    auto t = read_file_tool(opts_for(d));
    nlohmann::json a;
    a["path"] = "lines.txt";
    a["offset"] = 3;
    auto r = t.run(a);
    CHECK(r.has_value());
    if (r) CHECK_EQ(*r, std::string("l3"));
}

TEST("read_file: offset past the end yields empty output") {
    test::TempDir d;
    d.write("lines.txt", "l1\nl2\n");
    auto t = read_file_tool(opts_for(d));
    nlohmann::json a;
    a["path"] = "lines.txt";
    a["offset"] = 99;
    auto r = t.run(a);
    CHECK(r.has_value());
    if (r) CHECK_EQ(*r, std::string(""));
}

TEST("read_file: non-positive offset is clamped to line 1") {
    test::TempDir d;
    d.write("lines.txt", "l1\nl2\n");
    auto t = read_file_tool(opts_for(d));
    nlohmann::json a;
    a["path"] = "lines.txt";
    a["offset"] = 0;
    auto r = t.run(a);
    CHECK(r.has_value());
    if (r) CHECK_EQ(*r, std::string("l1\nl2\n"));
}

TEST("read_file: path escaping the root is rejected") {
    test::TempDir d;
    auto t = read_file_tool(opts_for(d));
    auto r = t.run(args({{"path", "../../../etc/passwd"}}));
    CHECK(!r.has_value());
}

TEST("read_file: absolute path outside root rejected when read not allowed") {
    test::TempDir root, outside;
    outside.write("secret.txt", "classified");
    auto abs = (outside.path() / "secret.txt").string();
    auto t = read_file_tool(opts_for(root));
    auto r = t.run(args({{"path", abs}}));
    CHECK(!r.has_value());
}

TEST("read_file: outside-root read permitted when allow_read_outside_root set") {
    test::TempDir root, outside;
    outside.write("secret.txt", "classified");
    auto abs = (outside.path() / "secret.txt").string();
    auto o = opts_for(root);
    o.allow_read_outside_root = true;
    auto t = read_file_tool(o);
    auto r = t.run(args({{"path", abs}}));
    CHECK(r.has_value());
    if (r) CHECK(r->find("classified") != std::string::npos);
}

TEST("read_file: read permission does not grant write escape") {
    test::TempDir root, outside;
    auto abs = (outside.path() / "new.txt").string();
    auto o = opts_for(root);
    o.allow_read_outside_root = true;  // read only
    auto t = write_file_tool(o);
    auto r = t.run(args({{"path", abs}, {"content", "x"}}));
    CHECK(!r.has_value());
}

TEST("read_file: missing path argument returns Error") {
    test::TempDir d;
    auto t = read_file_tool(opts_for(d));
    auto r = t.run(nlohmann::json::object());
    CHECK(!r.has_value());
}

// --- outside-root approval tier (approve_escape) ----------------------------

TEST("read_file: escape approver granting access permits an outside read") {
    test::TempDir root, outside;
    outside.write("secret.txt", "classified");
    auto abs = (outside.path() / "secret.txt").string();
    auto o = opts_for(root);
    std::string seen_kind;
    o.approve_escape = [&](std::string_view kind, const std::filesystem::path&,
                           const std::string&) {
        seen_kind = std::string(kind);
        return true;  // grant
    };
    auto t = read_file_tool(o);
    auto r = t.run(args({{"path", abs}}));
    CHECK(r.has_value());
    if (r) CHECK(r->find("classified") != std::string::npos);
    CHECK(seen_kind == "read");  // routed as a read escape
}

TEST("read_file: escape approver denying access rejects an outside read") {
    test::TempDir root, outside;
    outside.write("secret.txt", "classified");
    auto abs = (outside.path() / "secret.txt").string();
    auto o = opts_for(root);
    bool asked = false;
    o.approve_escape = [&](std::string_view, const std::filesystem::path&,
                           const std::string&) {
        asked = true;
        return false;  // deny
    };
    auto t = read_file_tool(o);
    auto r = t.run(args({{"path", abs}}));
    CHECK(!r.has_value());
    CHECK(asked);  // the tier was consulted before failing
}

TEST("read_file: in-root path never consults the escape approver") {
    test::TempDir root;
    root.write("inside.txt", "hello");
    auto o = opts_for(root);
    bool asked = false;
    o.approve_escape = [&](std::string_view, const std::filesystem::path&,
                           const std::string&) {
        asked = true;
        return true;
    };
    auto t = read_file_tool(o);
    auto r = t.run(args({{"path", "inside.txt"}}));
    CHECK(r.has_value());
    CHECK(!asked);  // no prompt for confined paths
}

TEST("read_file: missing outside file still fails after the approver grants") {
    test::TempDir root, outside;
    auto abs = (outside.path() / "nope.txt").string();  // does not exist
    auto o = opts_for(root);
    o.approve_escape = [&](std::string_view, const std::filesystem::path&,
                           const std::string&) { return true; };
    auto t = read_file_tool(o);
    auto r = t.run(args({{"path", abs}}));
    CHECK(!r.has_value());  // approval unlocks the sandbox, not the missing file
}

TEST("read_file: static allow short-circuits the approver") {
    test::TempDir root, outside;
    outside.write("s.txt", "data");
    auto abs = (outside.path() / "s.txt").string();
    auto o = opts_for(root);
    o.allow_read_outside_root = true;
    bool asked = false;
    o.approve_escape = [&](std::string_view, const std::filesystem::path&,
                           const std::string&) {
        asked = true;
        return true;
    };
    auto t = read_file_tool(o);
    auto r = t.run(args({{"path", abs}}));
    CHECK(r.has_value());
    CHECK(!asked);  // pre-granted permission needs no prompt
}

TEST("write_file: escape approver granting access permits an outside write") {
    test::TempDir root, outside;
    auto abs = (outside.path() / "made.txt").string();
    auto o = opts_for(root);
    std::string seen_kind;
    o.approve_escape = [&](std::string_view kind, const std::filesystem::path&,
                           const std::string&) {
        seen_kind = std::string(kind);
        return true;
    };
    auto t = write_file_tool(o);
    auto r = t.run(args({{"path", abs}, {"content", "hi"}}));
    CHECK(r.has_value());
    CHECK(seen_kind == "write");  // routed as a write escape
}

TEST("write_file: escape approver denying access rejects an outside write") {
    test::TempDir root, outside;
    auto abs = (outside.path() / "made.txt").string();
    auto o = opts_for(root);
    o.approve_escape = [&](std::string_view, const std::filesystem::path&,
                           const std::string&) { return false; };
    auto t = write_file_tool(o);
    auto r = t.run(args({{"path", abs}, {"content", "hi"}}));
    CHECK(!r.has_value());
    CHECK(!std::filesystem::exists(abs));  // nothing written on denial
}

// --- write_file -------------------------------------------------------------

TEST("write_file: creates a file and reports bytes") {
    test::TempDir d;
    auto t = write_file_tool(opts_for(d));
    auto r = t.run(args({{"path", "out.txt"}, {"content", "data"}}));
    CHECK(r.has_value());
    CHECK_EQ(d.read("out.txt"), std::string("data"));
    if (r) CHECK(r->find("4") != std::string::npos);  // 4 bytes
}

TEST("write_file: overwrites existing content") {
    test::TempDir d;
    d.write("out.txt", "old");
    auto t = write_file_tool(opts_for(d));
    auto r = t.run(args({{"path", "out.txt"}, {"content", "new"}}));
    CHECK(r.has_value());
    CHECK_EQ(d.read("out.txt"), std::string("new"));
}

TEST("write_file: creates parent directories") {
    test::TempDir d;
    auto t = write_file_tool(opts_for(d));
    auto r = t.run(args({{"path", "sub/dir/out.txt"}, {"content", "x"}}));
    CHECK(r.has_value());
    CHECK_EQ(d.read("sub/dir/out.txt"), std::string("x"));
}

TEST("write_file: path escaping the root is rejected") {
    test::TempDir d;
    auto t = write_file_tool(opts_for(d));
    auto r = t.run(args({{"path", "../escape.txt"}, {"content", "x"}}));
    CHECK(!r.has_value());
}

TEST("write_file: outside-root write permitted when allow_write_outside_root set") {
    test::TempDir root, outside;
    auto abs = (outside.path() / "made.txt").string();
    auto o = opts_for(root);
    o.allow_write_outside_root = true;
    auto t = write_file_tool(o);
    auto r = t.run(args({{"path", abs}, {"content", "payload"}}));
    CHECK(r.has_value());
    CHECK_EQ(outside.read("made.txt"), std::string("payload"));
}

TEST("write_file: write permission does not grant read escape") {
    test::TempDir root, outside;
    outside.write("secret.txt", "classified");
    auto abs = (outside.path() / "secret.txt").string();
    auto o = opts_for(root);
    o.allow_write_outside_root = true;  // write only
    auto t = read_file_tool(o);
    auto r = t.run(args({{"path", abs}}));
    CHECK(!r.has_value());
}

// --- edit_file --------------------------------------------------------------

TEST("edit_file: replaces a unique occurrence") {
    test::TempDir d;
    d.write("c.txt", "alpha beta gamma");
    auto t = edit_file_tool(opts_for(d));
    auto r = t.run(args({{"path", "c.txt"}, {"old", "beta"}, {"new", "BETA"}}));
    CHECK(r.has_value());
    CHECK_EQ(d.read("c.txt"), std::string("alpha BETA gamma"));
}

TEST("edit_file: outside-root edit follows allow_write_outside_root") {
    test::TempDir root, outside;
    outside.write("c.txt", "alpha beta gamma");
    auto abs = (outside.path() / "c.txt").string();
    auto o = opts_for(root);
    auto denied = edit_file_tool(o).run(
        args({{"path", abs}, {"old", "beta"}, {"new", "BETA"}}));
    CHECK(!denied.has_value());
    o.allow_write_outside_root = true;
    auto allowed = edit_file_tool(o).run(
        args({{"path", abs}, {"old", "beta"}, {"new", "BETA"}}));
    CHECK(allowed.has_value());
    CHECK_EQ(outside.read("c.txt"), std::string("alpha BETA gamma"));
}

TEST("edit_file: absent old string fails loudly") {
    test::TempDir d;
    d.write("c.txt", "alpha beta");
    auto t = edit_file_tool(opts_for(d));
    auto r = t.run(args({{"path", "c.txt"}, {"old", "zeta"}, {"new", "x"}}));
    CHECK(!r.has_value());
    if (!r) CHECK(r.error().msg.find("not found") != std::string::npos ||
                  r.error().msg.find("no match") != std::string::npos);
    CHECK_EQ(d.read("c.txt"), std::string("alpha beta"));  // unchanged
}

TEST("edit_file: non-unique old string fails loudly") {
    test::TempDir d;
    d.write("c.txt", "x x x");
    auto t = edit_file_tool(opts_for(d));
    auto r = t.run(args({{"path", "c.txt"}, {"old", "x"}, {"new", "y"}}));
    CHECK(!r.has_value());
    if (!r) CHECK(r.error().msg.find("3") != std::string::npos ||
                  r.error().msg.find("uniqu") != std::string::npos ||
                  r.error().msg.find("multiple") != std::string::npos);
    CHECK_EQ(d.read("c.txt"), std::string("x x x"));  // unchanged
}

TEST("edit_file: missing file returns Error") {
    test::TempDir d;
    auto t = edit_file_tool(opts_for(d));
    auto r = t.run(args({{"path", "nope.txt"}, {"old", "a"}, {"new", "b"}}));
    CHECK(!r.has_value());
}

TEST("edit_file: multiline replacement") {
    test::TempDir d;
    d.write("c.txt", "line1\nline2\nline3\n");
    auto t = edit_file_tool(opts_for(d));
    auto r = t.run(args({{"path", "c.txt"}, {"old", "line2\n"}, {"new", "LINE2\n"}}));
    CHECK(r.has_value());
    CHECK_EQ(d.read("c.txt"), std::string("line1\nLINE2\nline3\n"));
}

// --- list_dir ---------------------------------------------------------------

TEST("list_dir: returns sorted entries") {
    test::TempDir d;
    d.write("b.txt", "");
    d.write("a.txt", "");
    d.write("c.txt", "");
    auto t = list_dir_tool(opts_for(d));
    auto r = t.run(args({{"path", "."}}));
    CHECK(r.has_value());
    if (r) {
        auto pa = r->find("a.txt");
        auto pb = r->find("b.txt");
        auto pc = r->find("c.txt");
        CHECK(pa != std::string::npos && pb != std::string::npos &&
              pc != std::string::npos);
        CHECK(pa < pb);
        CHECK(pb < pc);
    }
}

TEST("list_dir: marks directories distinctly") {
    test::TempDir d;
    std::filesystem::create_directory(d.path() / "subdir");
    d.write("file.txt", "");
    auto t = list_dir_tool(opts_for(d));
    auto r = t.run(args({{"path", "."}}));
    CHECK(r.has_value());
    if (r) CHECK(r->find("subdir/") != std::string::npos);
}

TEST("list_dir: missing directory returns Error") {
    test::TempDir d;
    auto t = list_dir_tool(opts_for(d));
    auto r = t.run(args({{"path", "nope"}}));
    CHECK(!r.has_value());
}

TEST("list_dir: path escaping the root is rejected") {
    test::TempDir d;
    auto t = list_dir_tool(opts_for(d));
    auto r = t.run(args({{"path", "../.."}}));
    CHECK(!r.has_value());
}

TEST("list_dir: outside-root listing follows allow_read_outside_root") {
    test::TempDir root, outside;
    outside.write("file.txt", "x");
    auto abs = outside.path().string();
    auto o = opts_for(root);
    auto denied = list_dir_tool(o).run(args({{"path", abs}}));
    CHECK(!denied.has_value());
    o.allow_read_outside_root = true;
    auto allowed = list_dir_tool(o).run(args({{"path", abs}}));
    CHECK(allowed.has_value());
    if (allowed) CHECK(allowed->find("file.txt") != std::string::npos);
}

// --- run_bash ---------------------------------------------------------------

TEST("run_bash: captures stdout and exit 0") {
    test::TempDir d;
    auto t = run_bash_tool(opts_for(d));
    auto r = t.run(args({{"cmd", "echo hello"}}));
    CHECK(r.has_value());
    if (r) {
        CHECK(r->find("hello") != std::string::npos);
        CHECK(r->find("0") != std::string::npos);  // exit code 0 noted
    }
}

TEST("run_bash_rtk_disabled_runs_verbatim") {
    test::TempDir d;
    auto o = opts_for(d);
    o.rtk = false;  // gate off (also the default)
    auto t = run_bash_tool(o);
    auto r = t.run(args({{"cmd", "echo hi"}}));
    CHECK(r.has_value());
    if (r) CHECK(r->find("hi") != std::string::npos);  // normal output
}

TEST("run_bash: captures stderr") {
    test::TempDir d;
    auto t = run_bash_tool(opts_for(d));
    auto r = t.run(args({{"cmd", "echo oops 1>&2"}}));
    CHECK(r.has_value());
    if (r) CHECK(r->find("oops") != std::string::npos);
}

TEST("run_bash: nonzero exit code is reported in output") {
    test::TempDir d;
    auto t = run_bash_tool(opts_for(d));
    auto r = t.run(args({{"cmd", "exit 7"}}));
    CHECK(r.has_value());  // command ran; result carries the code
    if (r) CHECK(r->find("7") != std::string::npos);
}

TEST("run_bash: runs with root as working directory") {
    test::TempDir d;
    d.write("marker.txt", "");
    auto t = run_bash_tool(opts_for(d));
    auto r = t.run(args({{"cmd", "ls"}}));
    CHECK(r.has_value());
    if (r) CHECK(r->find("marker.txt") != std::string::npos);
}

TEST("run_bash: times out and returns Error") {
    test::TempDir d;
    auto t = run_bash_tool(opts_for(d));  // 2s timeout
    auto r = t.run(args({{"cmd", "sleep 10"}}));
    CHECK(!r.has_value());
    if (!r) {
        std::string low = r.error().msg;
        for (auto& c : low) c = static_cast<char>(::tolower(c));
        CHECK(low.find("time") != std::string::npos);
    }
}

TEST("run_bash: missing cmd argument returns Error") {
    test::TempDir d;
    auto t = run_bash_tool(opts_for(d));
    auto r = t.run(nlohmann::json::object());
    CHECK(!r.has_value());
}

// --- registration -----------------------------------------------------------

TEST("register_builtin_tools: registers the full set") {
    test::TempDir d;
    ToolRegistry reg;
    register_builtin_tools(reg, opts_for(d));
    CHECK(reg.has("read_file"));
    CHECK(reg.has("write_file"));
    CHECK(reg.has("edit_file"));
    CHECK(reg.has("list_dir"));
    CHECK(reg.has("run_bash"));
}
