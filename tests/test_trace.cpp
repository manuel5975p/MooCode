// Tests for the opt-in diagnostic log. The sink mapping is a pure function
// precisely so it can be checked here without the process committing to a sink —
// detail::sink() caches its choice on first use, so a test that went through
// line() could only ever exercise one configuration.

#include "agent/trace.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#include "test_harness.hpp"

using namespace moocode::trace;

namespace {

bool contains(const std::string& hay, const std::string& needle) {
    return hay.find(needle) != std::string::npos;
}

std::string slurp(const std::filesystem::path& p) {
    std::ifstream f(p);
    return std::string(std::istreambuf_iterator<char>(f), {});
}

}  // namespace

TEST("an unset or explicitly-off variable disables tracing") {
    CHECK(choose_sink(nullptr).kind == SinkKind::Disabled);
    CHECK(choose_sink("").kind == SinkKind::Disabled);
    CHECK(choose_sink("0").kind == SinkKind::Disabled);
}

TEST("1 and stderr mean stderr") {
    CHECK(choose_sink("1").kind == SinkKind::Stderr);
    CHECK(choose_sink("stderr").kind == SinkKind::Stderr);
    CHECK(choose_sink("1").path.empty());
}

TEST("anything else is a path, kept verbatim") {
    const SinkChoice c = choose_sink("/tmp/moo trace.log");
    CHECK(c.kind == SinkKind::File);
    CHECK_EQ(c.path, std::string("/tmp/moo trace.log"));
    // Not special-cased, deliberately: "2" is a filename, not a file
    // descriptor — the mapping has exactly two keywords.
    CHECK(choose_sink("2").kind == SinkKind::File);
}

TEST("lines are appended to the file, stamped, and flushed at once") {
    const std::filesystem::path path =
        std::filesystem::temp_directory_path() / "moo_trace_test.log";
    std::error_code ec;
    std::filesystem::remove(path, ec);

    // Set before the first line() call in this process, which is what fixes the
    // sink; every later test in this file shares it.
#ifdef _WIN32
    _putenv_s("MOOCODE_TRACE_STREAM", path.string().c_str());
#else
    ::setenv("MOOCODE_TRACE_STREAM", path.string().c_str(), 1);
#endif

    CHECK(enabled());
    line("first line");
    line("second line");

    // Readable immediately: line() flushes, because the last line before a hang
    // is the one that matters and a buffered tail dies with the process.
    const std::string body = slurp(path);
    CHECK(contains(body, "first line"));
    CHECK(contains(body, "second line"));
    CHECK(contains(body, "[moo "));  // the elapsed-seconds stamp
    // One line each, in order.
    CHECK(body.find("first line") < body.find("second line"));
    CHECK_EQ(std::count(body.begin(), body.end(), '\n'), 2);

    std::filesystem::remove(path, ec);
}
