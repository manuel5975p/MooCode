#ifndef MOOCODE_TRACE_HPP
#define MOOCODE_TRACE_HPP

// Opt-in diagnostic log, off unless $MOOCODE_TRACE_STREAM is set. It exists for
// one question: when a response appears to freeze, is the endpoint not sending
// or is the frontend not drawing? The streaming HTTP layer records what arrives
// and when it stops arriving; the GUI records what each render costs. Both write
// here, so one file interleaves the two answers.
//
// Header-only, STL only, no Qt and no agent_* dependency, so every layer from
// http.cpp to the Qt views can include it.
//
// What must never be logged: request headers (they carry the API key), request
// bodies and response content (the user's prose, and base64 image bytes). Sizes,
// counts and timings only — a trace file should be safe to paste into an issue.

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <string_view>

namespace moocode::trace {

enum class SinkKind { Disabled, Stderr, File };

struct SinkChoice {
    SinkKind kind = SinkKind::Disabled;
    std::string path;  // set only for SinkKind::File
};

// How $MOOCODE_TRACE_STREAM's value maps to a destination:
//   unset / "" / "0"  → disabled
//   "1" / "stderr"    → stderr
//   anything else      → that path, appended to
// A path is not a nicety: the TUI *is* stderr's terminal, so tracing to stderr
// would draw log lines over the FTXUI screen, and a GUI started from a launcher
// has no stderr anyone will ever read.
// Pure, so the mapping is unit-testable without touching the process's sink.
inline SinkChoice choose_sink(const char* env) {
    if (!env || !*env) return {};
    const std::string_view v(env);
    if (v == "0") return {};
    if (v == "1" || v == "stderr") return {SinkKind::Stderr, {}};
    return {SinkKind::File, std::string(v)};
}

namespace detail {

// Resolved once, on first use, and never closed: the trace outlives every
// caller and a freeze investigation must not lose lines to teardown order.
inline std::FILE* sink() {
    static std::FILE* f = [] () -> std::FILE* {
        const SinkChoice c = choose_sink(std::getenv("MOOCODE_TRACE_STREAM"));
        switch (c.kind) {
            case SinkKind::Disabled: return nullptr;
            case SinkKind::Stderr:   return stderr;
            case SinkKind::File:     break;
        }
        // Append, never truncate: successive runs of a rare freeze belong in one
        // file. A path that cannot be opened silently disables tracing rather
        // than failing a run that was only ever meant to be observed.
        return std::fopen(c.path.c_str(), "a");
    }();
    return f;
}

}  // namespace detail

// True when anything will be written. Call sites guard their measurement with
// this so an untraced run pays nothing beyond the branch.
inline bool enabled() { return detail::sink() != nullptr; }

// Append one line, stamped with seconds since the first trace call. Flushed
// immediately: the interesting line is the last one before a hang, and a
// buffered tail is exactly what a kill -9 loses.
inline void line(std::string_view msg) {
    std::FILE* f = detail::sink();
    if (!f) return;
    static const std::chrono::steady_clock::time_point t0 =
        std::chrono::steady_clock::now();
    const double secs = std::chrono::duration<double>(
                            std::chrono::steady_clock::now() - t0)
                            .count();
    static std::mutex m;  // the TUI worker, the GUI thread and subagents share f
    const std::lock_guard<std::mutex> lock(m);
    std::fprintf(f, "[moo %9.3f] %.*s\n", secs, static_cast<int>(msg.size()),
                 msg.data());
    std::fflush(f);
}

}  // namespace moocode::trace

#endif  // MOOCODE_TRACE_HPP
