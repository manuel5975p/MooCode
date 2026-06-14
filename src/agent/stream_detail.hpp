#ifndef MOOCODE_STREAM_DETAIL_HPP
#define MOOCODE_STREAM_DETAIL_HPP

// Implementation detail of StreamAccumulator: the <think>-tag splitter.
// Included only by stream.cpp and test_stream.cpp. Not part of the public API.

#include <string>
#include <string_view>

namespace moocode {

// Routes a *stream* of text into "answer" (outside <think>) and "reasoning"
// (inside <think>) parts. A tag may be split across feed() calls; partial tag
// bytes are buffered until the next feed()/flush() can disambiguate them.
class ThinkSplitter {
public:
    struct Parts {
        std::string answer;
        std::string reasoning;
    };

    // Route the next fragment. post: bytes that might begin a (possibly partial)
    // <think>/</think> tag at the very end are held back, not yet returned.
    Parts feed(std::string_view fragment);

    // Release any buffered partial-tag bytes as text in the current state. Call
    // once at end-of-stream so a dangling "<thi" is not silently dropped.
    Parts flush();

    bool in_think() const { return in_think_; }

private:
    bool in_think_ = false;
    std::string pending_;  // trailing bytes that could still grow into a tag
};

}  // namespace moocode

#endif  // MOOCODE_STREAM_DETAIL_HPP
