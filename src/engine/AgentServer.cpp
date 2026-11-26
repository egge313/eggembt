#include "engine/AgentServer.h"

namespace pn {

AgentServer::AgentServer(ISutPlugin &sut, ILineTransport &transport, std::string name)
    : sut_(sut), transport_(transport), name_(std::move(name))
{
    transport_.setLineHandler([this](const std::string &line) { onLine(line); });
    transport_.setCloseHandler([this] {
        if (runActive_.exchange(false))
            sut_.stopRun(StopReason::Aborted);
        finish();
    });
}

AgentServer::~AgentServer()
{
    stopForwarder_ = true;
    if (forwarder_.joinable())
        forwarder_.join();
}

void AgentServer::start()
{
    const PluginCapabilities caps = sut_.capabilities();
    ProtocolMessage hello;
    hello.type = ProtocolMessage::Type::Hello;
    hello.name = name_;
    hello.snapshot = caps.snapshot;
    hello.crash = caps.crash;
    hello.restartMs = caps.restartMs;
    hello.syncCapable = caps.eventAcks;  // DeepSeek V4.1 Flash
    hello.initials = caps.initialMarkings;  // DeepSeek V4.1 Flash: what the wrapper can start
    send(hello);
    forwarder_ = std::thread([this] { forward(); });
}

bool AgentServer::waitUntilFinished(std::chrono::milliseconds timeout)
{
    std::unique_lock<std::mutex> lock(mutex_);
    return done_.wait_for(lock, timeout, [this] { return finished_.load(); });
}

void AgentServer::finish()
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        finished_ = true;
    }
    done_.notify_all();
}

void AgentServer::send(const ProtocolMessage &m)
{
    transport_.sendLine(encodeMessage(m));
}

void AgentServer::onLine(const std::string &line)
{
    using T = ProtocolMessage::Type;
    ProtocolMessage m;
    std::string error;
    if (!decodeMessage(line, m, &error)) {
        ProtocolMessage fault;
        fault.type = T::Fault;
        fault.error = "bad message from the tracker: " + error;
        send(fault);
        return;
    }

    switch (m.type) {
    case T::StartRun: {
        run_ = m.run;
        {
            std::lock_guard<std::mutex> lock(state_);  // DeepSeek V4.1 Flash: new run, new event numbers
            nextEventSeq_ = 0;
            ackSeqs_.clear();
        }
        local_.reset();
        PluginRunOptions options;
        options.quiescenceTimeoutMs = m.quiescenceMs > 0 ? m.quiescenceMs : options.quiescenceTimeoutMs;
        options.syncLabels = m.syncLabels;  // DeepSeek V4.1 Flash
        ProtocolMessage reply;
        reply.type = T::RunStarted;
        reply.ok = sut_.startRun(m.initial, options, local_, &reply.error);
        runActive_ = reply.ok;
        send(reply);
        break;
    }
    case T::Stimulate: {
        std::string err;
        if (!sut_.stimulate(m.label, &err)) {
            ProtocolMessage fault;
            fault.type = T::Fault;
            fault.error = "could not send input \"" + m.label + "\" to the SUT: " + err;
            send(fault);
        }
        break;
    }
    case T::PostMortem: {
        const PostMortem pm = sut_.postMortem(m.mode);
        ProtocolMessage reply;
        reply.type = T::PostMortemResult;
        reply.id = m.id;
        reply.ok = pm.ok;
        reply.error = pm.error;
        reply.files = pm.files;
        reply.stack = pm.stackText;
        send(reply);
        break;
    }
    case T::Ack: {  // DeepSeek V4.1 Flash: the tracker handled a held event
        if (m.run != run_)
            break;  // a late ack of an earlier run
        std::uint64_t queueSeq = 0;
        bool found = false;
        {
            std::lock_guard<std::mutex> lock(state_);
            const auto it = ackSeqs_.find(m.seq);
            if (it != ackSeqs_.end()) {
                queueSeq = it->second;
                ackSeqs_.erase(it);
                found = true;
            }
        }
        if (found)
            sut_.acknowledgeEvent(queueSeq);
        break;
    }
    case T::StopRun:
        if (runActive_.exchange(false))
            sut_.stopRun(m.reason == "aborted" ? StopReason::Aborted : StopReason::Finished);
        break;
    case T::Shutdown:
        if (runActive_.exchange(false))
            sut_.stopRun(StopReason::Finished);
        finish();
        break;
    default:
        break;  // messages meant for the tracker: ignored
    }
}

void AgentServer::forward()
{
    SequencedEvent e;
    std::string reportedFault;
    while (!stopForwarder_) {
        // A plug-in fault is reported once (again only if its text changes).
        const std::string fault = sut_.pollFault();
        if (!fault.empty() && fault != reportedFault) {
            reportedFault = fault;
            ProtocolMessage m;
            m.type = ProtocolMessage::Type::Fault;
            m.error = fault;
            send(m);
        }
        if (!local_.waitForEvents(std::chrono::milliseconds(5)))
            continue;
        while (local_.tryPop(e)) {
            ProtocolMessage m;
            m.type = ProtocolMessage::Type::Event;
            m.run = run_;
            m.kind = e.event.kind;
            m.label = e.event.label;
            m.timestampMs = e.event.timestampMs;
            {
                // DeepSeek V4.1 Flash: number the events of the run and remember
                // which local sequence a held event's ack must wake.
                std::lock_guard<std::mutex> lock(state_);
                m.seq = nextEventSeq_++;
                m.syncEvent = e.needsAck;
                if (e.needsAck)
                    ackSeqs_[m.seq] = e.seq;
            }
            send(m);
        }
    }
}

}  // namespace pn
