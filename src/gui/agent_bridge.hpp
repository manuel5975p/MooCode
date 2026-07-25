#ifndef MOOCODE_GUI_AGENT_BRIDGE_HPP
#define MOOCODE_GUI_AGENT_BRIDGE_HPP

// The GUI's seam onto agent_core. Owns the Provider, the (empty) ToolRegistry
// and the Agent, and runs Agent::run() on a worker thread so the event loop
// never blocks on an HTTP round-trip.
//
// Threading contract: every signal below is *emitted* on the thread the bridge
// lives in (the GUI thread), never on the worker. The Agent callbacks fire on
// the worker and only hand the payload to post(), which bounces it back through
// the event loop.
//
// The obvious alternative — emit straight from the worker and rely on Qt making
// cross-thread connections queued — is a trap: Qt only does that when the
// connection names a receiver QObject. A connect() to a bare lambda with no
// context object stays direct and runs the slot on the worker thread, touching
// widgets from the wrong thread. That is a segfault waiting on a caller's
// carelessness at every future call site, so the guarantee is made here instead.
//
// The bridge therefore needs no mutex of its own: the Agent is only mutated
// while idle, which busy() guards.
//
// Chat-only, deliberately: the registry stays empty and AgentConfig disables
// tool advertisement, so the model is told nothing about tools and cannot
// request one. Without an approval UI (the TUI's ApprovalGate has no analogue
// here yet) a tool-capable agent would run shell commands with no gate.

#include <atomic>
#include <memory>
#include <thread>
#include <utility>

#include <QObject>
#include <QString>
#include <QStringList>

#include "agent/agent.hpp"
#include "agent/provider.hpp"
#include "agent/provider_factory.hpp"
#include "agent/tools.hpp"

namespace moocode::gui {

class AgentBridge : public QObject {
    Q_OBJECT

public:
    // `conn` is the resolved startup connection; `gp` the generation controls
    // (effort/thinking/temperature/max_tokens) to apply to it.
    AgentBridge(ProviderConnection conn, GenerationParams gp,
                std::string system_prompt, QObject* parent = nullptr);
    ~AgentBridge() override;

    bool busy() const { return busy_.load(std::memory_order_acquire); }

    // Non-const because Agent::provider() is: after a reconnect the live
    // provider is the Agent's, not the borrowed startup one.
    QString model();
    // Conversation so far, for autosave. Only valid while idle.
    const Conversation& history() const { return agent_->history(); }

public slots:
    // Start a turn. Ignored while busy.
    void send(const QString& prompt);

    // Abort the in-flight turn (and its HTTP request). Returns immediately —
    // this only raises the flag the loop polls.
    void cancel();

    // Cancel and block until the worker has actually stopped. Callers that go
    // on to read history() need this; cancel() alone leaves the worker still
    // appending to the conversation.
    void stopAndWait();

    // Swap in a freshly built provider — how the GUI changes profile or model.
    // Unlike the TUI's switch_connection, which retunes a live provider in
    // place for same-wire-format switches and must thread every per-profile
    // field by hand (see CLAUDE.md), this always constructs a new one, so no
    // field can go stale. Ignored while busy.
    void reconnect(const ProviderConnection& conn, const GenerationParams& gp);

    // Ask the live endpoint which models it serves. Network I/O, so it runs on
    // the worker thread and reports back through modelsDetected / failed, and
    // takes the same busy lock as a turn. Ignored while busy.
    void detectModels();

signals:
    void started();
    void answerDelta(const QString& fragment);
    void reasoningDelta(const QString& fragment);
    void finished(const QString& answer);
    void failed(const QString& message);
    void usage(int prompt_tokens, int completion_tokens);
    void modelsDetected(const QStringList& models);

private:
    // Run `f` on the bridge's own thread. Called from the worker; the queued
    // invocation is what makes the threading contract above hold regardless of
    // how a caller connects. Pending invocations are dropped if the bridge is
    // destroyed first, and the destructor joins the worker anyway.
    template <class F>
    void post(F&& f) {
        QMetaObject::invokeMethod(this, std::forward<F>(f), Qt::QueuedConnection);
    }

    void join_worker();

    // Holds the startup provider only: Agent borrows it at construction, and
    // the first reconnect() hands ownership of every later one to the Agent and
    // frees this. Use agent_->provider() for the live one.
    std::unique_ptr<Provider> provider_;
    ToolRegistry tools_;                 // intentionally empty; see the header note
    std::unique_ptr<Agent> agent_;
    std::thread worker_;
    std::atomic<bool> busy_{false};
};

}  // namespace moocode::gui

#endif  // MOOCODE_GUI_AGENT_BRIDGE_HPP
