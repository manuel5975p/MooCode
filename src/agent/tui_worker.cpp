#include "agent/tui_worker.hpp"

#include <utility>

namespace flagent {

TuiWorker::TuiWorker(Deps d) : d_(std::move(d)) {
    t_ = std::thread([this] { loop(); });
}

TuiWorker::~TuiWorker() { stop(); }

void TuiWorker::submit(WorkItem item) {
    {
        std::lock_guard lk(m_);
        q_.push(std::move(item));
    }
    cv_.notify_one();
}

void TuiWorker::stop() {
    {
        std::lock_guard lk(m_);
        stop_ = true;
    }
    cv_.notify_all();
    if (t_.joinable()) t_.join();
}

void TuiWorker::loop() {
    for (;;) {
        WorkItem item;
        {
            std::unique_lock lk(m_);
            cv_.wait(lk, [this] { return stop_ || !q_.empty(); });
            if (q_.empty()) return;  // woken to stop
            item = std::move(q_.front());
            q_.pop();
        }
        if (item.kind == WorkItem::Prompt) {
            d_.run_prompt(std::move(item.text), std::move(item.user_parts));
        } else if (item.kind == WorkItem::DetectModels) {
            d_.run_detect(std::move(item.text), item.silent);
        } else if (item.kind == WorkItem::DetectAllModels) {
            if (d_.run_detect_all) d_.run_detect_all();
        } else {
            d_.run_compact(std::move(item.text));
        }
    }
}

}  // namespace flagent
