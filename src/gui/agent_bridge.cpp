#include "gui/agent_bridge.hpp"

#include <utility>

namespace moocode::gui {

AgentBridge::AgentBridge(ProviderConnection conn, GenerationParams gp,
                         std::string system_prompt, QObject* parent)
    : QObject(parent), provider_(make_provider(conn, gp)) {
    AgentConfig cfg;
    cfg.advertise_tools = false;  // chat-only; see the header note
    cfg.system_prompt = std::move(system_prompt);
    agent_ = std::make_unique<Agent>(*provider_, tools_, cfg);

    // Both callbacks run on the worker thread, so neither emits directly — see
    // post() for why.
    agent_->on_delta([this](std::string_view answer, std::string_view reasoning) {
        if (!answer.empty()) {
            QString s = QString::fromUtf8(answer.data(),
                                          static_cast<qsizetype>(answer.size()));
            post([this, s = std::move(s)] { emit answerDelta(s); });
        }
        if (!reasoning.empty()) {
            QString s = QString::fromUtf8(reasoning.data(),
                                          static_cast<qsizetype>(reasoning.size()));
            post([this, s = std::move(s)] { emit reasoningDelta(s); });
        }
    });
    agent_->on_usage([this](const Usage& u) {
        if (!u.present) return;
        const int in = u.prompt_tokens, out = u.completion_tokens;
        post([this, in, out] { emit usage(in, out); });
    });
}

AgentBridge::~AgentBridge() { stopAndWait(); }

void AgentBridge::stopAndWait() {
    cancel();
    join_worker();
}

void AgentBridge::join_worker() {
    if (worker_.joinable()) worker_.join();
}

QString AgentBridge::model() {
    return QString::fromStdString(agent_->provider().model());
}

void AgentBridge::send(const QString& prompt) {
    if (busy_.load(std::memory_order_acquire)) return;
    const std::string text = prompt.toStdString();
    if (text.empty()) return;

    // The previous turn's thread has finished (busy_ is false) but may not have
    // been joined yet; std::thread::operator= would terminate() on a joinable
    // target.
    join_worker();

    busy_.store(true, std::memory_order_release);
    emit started();
    worker_ = std::thread([this, text] {
        auto result = agent_->run(text);
        busy_.store(false, std::memory_order_release);
        if (result) {
            QString answer = QString::fromStdString(*result);
            post([this, answer = std::move(answer)] { emit finished(answer); });
        } else {
            QString msg = QString::fromStdString(result.error().msg);
            post([this, msg = std::move(msg)] { emit failed(msg); });
        }
    });
}

void AgentBridge::detectModels() {
    if (busy_.load(std::memory_order_acquire)) return;
    join_worker();
    busy_.store(true, std::memory_order_release);
    emit started();
    worker_ = std::thread([this] {
        auto models = agent_->provider().list_models();
        busy_.store(false, std::memory_order_release);
        if (models) {
            QStringList list;
            list.reserve(static_cast<qsizetype>(models->size()));
            for (const std::string& m : *models) list << QString::fromStdString(m);
            post([this, list] { emit modelsDetected(list); });
        } else {
            QString msg = QString::fromStdString(models.error().msg);
            post([this, msg = std::move(msg)] { emit failed(msg); });
        }
    });
}

void AgentBridge::cancel() {
    if (!busy_.load(std::memory_order_acquire)) return;
    agent_->cancel();
}

void AgentBridge::reconnect(const ProviderConnection& conn,
                            const GenerationParams& gp) {
    if (busy_.load(std::memory_order_acquire)) return;
    join_worker();
    agent_->set_provider(make_provider(conn, gp));
    // The Agent owns every provider from here on and no longer references the
    // borrowed startup one, so this frees it. A no-op on later reconnects.
    provider_.reset();
}

}  // namespace moocode::gui
