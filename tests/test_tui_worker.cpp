#include "agent/tui_worker.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "test_harness.hpp"

using namespace moocode;
using namespace std::chrono_literals;

namespace {

// A small thread-safe recorder so the test can observe what the worker
// dispatched (kind + payload) and in what order, then wait for N events.
struct Recorder {
    std::mutex m;
    std::condition_variable cv;
    std::vector<std::string> log;  // e.g. "prompt:hello", "compact:x", "detect:p"

    void note(std::string s) {
        {
            std::lock_guard lk(m);
            log.push_back(std::move(s));
        }
        cv.notify_all();
    }

    bool wait_for(std::size_t n, std::chrono::milliseconds timeout = 2000ms) {
        std::unique_lock lk(m);
        return cv.wait_for(lk, timeout, [&] { return log.size() >= n; });
    }

    std::vector<std::string> snapshot() {
        std::lock_guard lk(m);
        return log;
    }
};

TuiWorker::Deps make_deps(Recorder& rec) {
    TuiWorker::Deps d;
    d.run_prompt = [&rec](std::string text, std::vector<ContentPart> parts) {
        rec.note("prompt:" + text + ":" + std::to_string(parts.size()));
    };
    d.run_compact = [&rec](std::string instr) { rec.note("compact:" + instr); };
    d.run_detect = [&rec](std::string profile, bool silent) {
        rec.note("detect:" + profile + (silent ? ":silent" : ""));
    };
    return d;
}

}  // namespace

// Per-kind dispatch: a Prompt item drives run_prompt with text + parts count, a
// Compact item drives run_compact, a DetectModels item drives run_detect.
TEST("TuiWorker dispatches each WorkItem kind to its Deps closure") {
    Recorder rec;
    TuiWorker w(make_deps(rec));
    w.submit({WorkItem::Prompt, "hello", {ContentPart{"a", std::nullopt}}});
    w.submit({WorkItem::Compact, "tighten", {}});
    w.submit({WorkItem::DetectModels, "prof", {}});
    CHECK(rec.wait_for(3));
    auto log = rec.snapshot();
    CHECK_EQ(log.size(), std::size_t{3});
    if (log.size() == 3) {
        // Single worker thread => submit order is preserved (FIFO).
        CHECK_EQ(log[0], std::string{"prompt:hello:1"});
        CHECK_EQ(log[1], std::string{"compact:tighten"});
        CHECK_EQ(log[2], std::string{"detect:prof"});
    }
}

// A DetectModels item with silent=true forwards the flag to run_detect (the
// background startup refresh that skips the picker).
TEST("TuiWorker forwards the silent flag on DetectModels") {
    Recorder rec;
    TuiWorker w(make_deps(rec));
    w.submit({WorkItem::DetectModels, "prof", {}, /*silent=*/true});
    CHECK(rec.wait_for(1));
    auto log = rec.snapshot();
    CHECK_EQ(log.size(), std::size_t{1});
    if (log.size() == 1) CHECK_EQ(log[0], std::string{"detect:prof:silent"});
}

// submit -> pop is strict FIFO on the single worker thread, even under a burst.
TEST("TuiWorker preserves submit->pop FIFO order under a burst") {
    Recorder rec;
    TuiWorker w(make_deps(rec));
    constexpr int kN = 50;
    for (int i = 0; i < kN; ++i)
        w.submit({WorkItem::Compact, std::to_string(i), {}});
    CHECK(rec.wait_for(kN));
    auto log = rec.snapshot();
    CHECK_EQ(log.size(), std::size_t{kN});
    bool ordered = log.size() == static_cast<std::size_t>(kN);
    for (int i = 0; i < kN && ordered; ++i)
        if (log[static_cast<std::size_t>(i)] != "compact:" + std::to_string(i))
            ordered = false;
    CHECK(ordered);
}

// stop() returns promptly with no work and is idempotent; the destructor that
// follows must not deadlock or double-join.
TEST("TuiWorker stop() is idempotent and the dtor does not deadlock") {
    Recorder rec;
    {
        TuiWorker w(make_deps(rec));
        w.stop();
        w.stop();  // second stop is a no-op (already joined)
    }  // dtor calls stop() again — must not hang/throw
    CHECK(rec.snapshot().empty());
}

// The destructor alone joins cleanly even with no explicit stop() and pending
// drained work; queued-but-undequeued items after stop are simply dropped.
TEST("TuiWorker destructor stops and joins cleanly") {
    Recorder rec;
    {
        TuiWorker w(make_deps(rec));
        w.submit({WorkItem::Prompt, "one", {}});
        CHECK(rec.wait_for(1));  // ensure at least one item ran before teardown
    }  // dtor stops + joins
    CHECK(rec.snapshot().size() >= 1);
}

// A blocking job (worker busy) does not stall stop(): stop sets the flag, the
// in-flight job finishes, then the worker exits and joins.
TEST("TuiWorker stop() waits for the in-flight job before joining") {
    Recorder rec;
    std::atomic<bool> entered{false};
    std::atomic<bool> release{false};
    TuiWorker::Deps d = make_deps(rec);
    d.run_prompt = [&](std::string text, std::vector<ContentPart>) {
        entered.store(true);
        while (!release.load()) std::this_thread::sleep_for(1ms);
        rec.note("prompt-done:" + text);
    };
    TuiWorker w(std::move(d));
    w.submit({WorkItem::Prompt, "slow", {}});
    // Wait until the worker is inside the blocking job.
    for (int i = 0; i < 2000 && !entered.load(); ++i)
        std::this_thread::sleep_for(1ms);
    CHECK(entered.load());
    release.store(true);  // let it finish so stop() can join
    w.stop();
    CHECK(rec.wait_for(1));
    CHECK_EQ(rec.snapshot().at(0), std::string{"prompt-done:slow"});
}
