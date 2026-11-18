#include "engine/RemotePlugin.h"

namespace pn {

RemotePlugin::RemotePlugin(ILineTransport &transport, RemoteTimeouts timeouts)
    : transport_(transport), timeouts_(timeouts), shared_(std::make_shared<Shared>())
{
    // The handler keeps the shared state alive, so a late line cannot hit a destroyed plug-in.
    const std::shared_ptr<Shared> shared = shared_;
    transport_.setLineHandler([shared](const std::string &line) { onLine(shared, line); });
    transport_.setCloseHandler([shared] {
        std::lock_guard<std::mutex> lock(shared->mutex);
        if (!shared->shuttingDown && shared->fault.empty())
            shared->fault = "the connection to the agent was closed";
        shared->changed.notify_all();
    });
}

void RemotePlugin::onLine(const std::shared_ptr<Shared> &shared, const std::string &line)
{
    ProtocolMessage m;
    std::string error;
    if (!decodeMessage(line, m, &error)) {
        std::lock_guard<std::mutex> lock(shared->mutex);
        if (shared->fault.empty())
            shared->fault = "bad message from the agent: " + error;
        shared->changed.notify_all();
        return;
    }

    using T = ProtocolMessage::Type;
    switch (m.type) {
    case T::Event: {
        // Push outside the lock: the queue has its own, and the order of pushes is the order of lines.
        EventQueue *queue = nullptr;
        {
            std::lock_guard<std::mutex> lock(shared->mutex);
            if (shared->events && (m.run == 0 || m.run == shared->run)) {
                queue = shared->events;
                ++shared->pushing;  // stopRun() waits for this push to finish
            }
        }
        if (queue) {
            queue->push(SutEvent{m.kind, m.label, m.timestampMs});
            std::lock_guard<std::mutex> lock(shared->mutex);
            --shared->pushing;
            shared->changed.notify_all();
        }
        return;
    }
    default:
        break;
    }

    std::lock_guard<std::mutex> lock(shared->mutex);
    switch (m.type) {
    case T::Hello:
        shared->helloSeen = true;
        shared->name = m.name;
        shared->caps.snapshot = m.snapshot;
        shared->caps.crash = m.crash;
        shared->caps.restartMs = m.restartMs;
        break;
    case T::RunStarted:
        shared->startAnswered = true;
        shared->startOk = m.ok;
        shared->startError = m.error;
        break;
    case T::PostMortemResult: {
        PostMortem pm;
        pm.ok = m.ok;
        pm.error = m.error;
        pm.files = m.files;
        pm.stackText = m.stack;
        shared->postMortems[m.id] = std::move(pm);
        break;
    }
    case T::Fault:
        if (shared->fault.empty())
            shared->fault = m.error;
        break;
    default:
        // Messages meant for the agent (start_run, stimulate, ...) make no sense here.
        if (shared->fault.empty())
            shared->fault = "the agent sent a message that only the tracker may send";
        break;
    }
    shared->changed.notify_all();
}

bool RemotePlugin::waitForHello(std::chrono::milliseconds timeout)
{
    std::unique_lock<std::mutex> lock(shared_->mutex);
    return shared_->changed.wait_for(lock, timeout, [this] { return shared_->helloSeen; });
}

std::string RemotePlugin::agentName() const
{
    std::lock_guard<std::mutex> lock(shared_->mutex);
    return shared_->name;
}

PluginCapabilities RemotePlugin::capabilities() const
{
    std::lock_guard<std::mutex> lock(shared_->mutex);
    return shared_->caps;
}

void RemotePlugin::shutdown()
{
    {
        std::lock_guard<std::mutex> lock(shared_->mutex);
        shared_->shuttingDown = true;
    }
    ProtocolMessage m;
    m.type = ProtocolMessage::Type::Shutdown;
    transport_.sendLine(encodeMessage(m));
}

bool RemotePlugin::startRun(const std::string &initialMarking, const PluginRunOptions &options,
                            EventQueue &events, std::string *error)
{
    ProtocolMessage m;
    m.type = ProtocolMessage::Type::StartRun;
    m.initial = initialMarking;
    m.quiescenceMs = options.quiescenceTimeoutMs;
    {
        std::lock_guard<std::mutex> lock(shared_->mutex);
        shared_->events = &events;  // events may follow the start at once
        m.run = ++shared_->run;
        shared_->startAnswered = false;
        shared_->startOk = false;
        shared_->startError.clear();
    }

    const auto fail = [&](const std::string &text) {
        detachEvents();
        if (error)
            *error = text;
        return false;
    };

    if (!transport_.sendLine(encodeMessage(m)))
        return fail("the connection to the agent is gone");

    std::unique_lock<std::mutex> lock(shared_->mutex);
    if (!shared_->changed.wait_for(lock, timeouts_.runStart, [this] { return shared_->startAnswered; })) {
        lock.unlock();
        return fail("the agent did not answer start_run in time");
    }
    if (!shared_->startOk) {
        const std::string text = shared_->startError.empty() ? "the agent could not start the SUT"
                                                             : shared_->startError;
        lock.unlock();
        return fail(text);
    }
    return true;
}

bool RemotePlugin::stimulate(const std::string &label, std::string *error)
{
    ProtocolMessage m;
    m.type = ProtocolMessage::Type::Stimulate;
    m.label = label;
    if (!transport_.sendLine(encodeMessage(m))) {
        if (error)
            *error = "the connection to the agent is gone";
        return false;
    }
    return true;
}

PostMortem RemotePlugin::postMortem(DumpMode mode)
{
    ProtocolMessage m;
    m.type = ProtocolMessage::Type::PostMortem;
    m.mode = mode;
    {
        std::lock_guard<std::mutex> lock(shared_->mutex);
        m.id = shared_->nextPostMortemId++;
    }

    PostMortem result;
    if (!transport_.sendLine(encodeMessage(m))) {
        result.error = "the connection to the agent is gone";
        return result;
    }

    std::unique_lock<std::mutex> lock(shared_->mutex);
    const std::int64_t id = m.id;
    if (!shared_->changed.wait_for(lock, timeouts_.postMortem,
                                   [this, id] { return shared_->postMortems.count(id) > 0; })) {
        result.error = "the agent did not deliver the post-mortem in time";
        return result;
    }
    result = std::move(shared_->postMortems[id]);
    shared_->postMortems.erase(id);
    return result;
}

void RemotePlugin::detachEvents()
{
    std::unique_lock<std::mutex> lock(shared_->mutex);
    shared_->events = nullptr;  // no new push starts from here on
    shared_->changed.wait(lock, [this] { return shared_->pushing == 0; });
}

void RemotePlugin::stopRun(StopReason reason)
{
    detachEvents();  // nothing more reaches the Session, and its queue may be destroyed
    ProtocolMessage m;
    m.type = ProtocolMessage::Type::StopRun;
    m.reason = reason == StopReason::Aborted ? "aborted" : "finished";
    transport_.sendLine(encodeMessage(m));
}

std::string RemotePlugin::pollFault()
{
    std::lock_guard<std::mutex> lock(shared_->mutex);
    return shared_->fault;
}

}  // namespace pn
