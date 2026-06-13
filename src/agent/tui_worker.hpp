#ifndef FLAGENT_TUI_WORKER_HPP
#define FLAGENT_TUI_WORKER_HPP

// Background work queue for the TUI: a single worker thread that drains
// submitted WorkItems (prompts, compactions, model detections) one at a time so
// the UI render loop never blocks on the provider. The class owns the queue +
// thread; all interaction with the shared render state happens through the
// std::function closures in Deps (the thread never touches TuiState/state_mtx
// directly), which makes the worker unit-testable with recording closures.

#include <condition_variable>
#include <functional>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>

#include "agent/types.hpp"  // ContentPart

namespace flagent {

// One unit of background work for the TUI worker thread. `text` is the prompt
// text for Prompt, the additional instructions for Compact, and the profile
// name ("" => live model only) for DetectModels. `user_parts` is multimodal
// content for Prompt only. `silent` (DetectModels only) requests a background
// refresh that updates the profile's model set in place without raising the
// foreground selection picker — used for the automatic fetch on startup.
struct WorkItem {
    enum Kind { Prompt, Compact, DetectModels, DetectAllModels } kind;
    std::string text;
    std::vector<ContentPart> user_parts;
    bool silent = false;
};

// Background worker: serialises WorkItems on a dedicated thread. Each kind is
// dispatched to the matching Deps closure, which owns all shared-state access
// (locking, redraw posting, persistence). submit() enqueues + notifies; stop()
// (and the destructor) sets the stop flag, notifies, and joins.
class TuiWorker {
public:
    // The work the run loop performs per kind, plus side-effect hooks. Each
    // closure is invoked on the worker thread. run_prompt/run_compact/run_detect
    // carry the kind-specific body; the queue mechanics stay inside TuiWorker.
    struct Deps {
        std::function<void(std::string text, std::vector<ContentPart> parts)> run_prompt;
        std::function<void(std::string instructions)> run_compact;
        std::function<void(std::string profile, bool silent)> run_detect;
        // Background sweep that refreshes every profile's model set on startup
        // (each profile gets its own provider built from its endpoint + key).
        std::function<void()> run_detect_all;
    };

    explicit TuiWorker(Deps d);
    ~TuiWorker();

    TuiWorker(const TuiWorker&) = delete;
    TuiWorker& operator=(const TuiWorker&) = delete;

    // Enqueue an item and wake the worker. Safe to call from any thread.
    void submit(WorkItem item);

    // Request shutdown: set the stop flag, wake the worker, and join. Idempotent;
    // any items already dequeued finish, queued-but-undequeued items are dropped
    // (matches the original worker.join-after-stop behaviour). post: not running.
    void stop();

private:
    void loop();

    Deps d_;
    std::mutex m_;
    std::condition_variable cv_;
    std::queue<WorkItem> q_;
    bool stop_ = false;
    std::thread t_;
};

}  // namespace flagent

#endif  // FLAGENT_TUI_WORKER_HPP
