// Exposes any ISutPlugin as an agent speaking the line protocol: the generic
// agent shim. Wrap your SUT in an ISutPlugin (or use the SimulatedSut), give the
// server a transport (standard streams, a socket, a pipe), and a tracker on the
// other end can test it. See docs/PLUGIN_PROTOCOL.md.
//
// The server answers start_run / stimulate / post_mortem / stop_run by calling
// the plug-in, and forwards the plug-in's events to the tracker as `event`
// messages, stamped with the run number of the current run, in the order the
// plug-in pushed them. It says hello when started.
//
// Synchronized events (DeepSeek V4.1 Flash, doku section 19). The server passes
// start_run's sync_labels to the plug-in's run options, numbers every event of
// a run (0, 1, 2, ...) and marks the ones the plug-in holds the SUT at with
// "sync": true. An "ack" from the tracker is translated back to the plug-in's
// queue sequence and handed to acknowledgeEvent(). The plug-in declares the
// capability in hello.
#pragma once

#include "engine/EventQueue.h"
#include "engine/Protocol.h"
#include "engine/RemotePlugin.h"
#include "engine/SutPlugin.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <thread>

namespace pn {

class AgentServer {
public:
    AgentServer(ISutPlugin &sut, ILineTransport &transport, std::string name = "eggembt-agent");
    ~AgentServer();

    AgentServer(const AgentServer &) = delete;
    AgentServer &operator=(const AgentServer &) = delete;

    // Sends hello and starts forwarding events.
    void start();

    // Waits until the tracker said shutdown or the connection closed.
    bool waitUntilFinished(std::chrono::milliseconds timeout);
    bool finished() const { return finished_; }

private:
    void onLine(const std::string &line);
    void forward();
    void finish();
    void send(const ProtocolMessage &m);

    ISutPlugin &sut_;
    ILineTransport &transport_;
    std::string name_;
    EventQueue local_;
    std::atomic<std::int64_t> run_{0};
    std::atomic<bool> stopForwarder_{false};
    std::atomic<bool> finished_{false};
    std::atomic<bool> runActive_{false};
    std::thread forwarder_;
    std::mutex mutex_;
    std::condition_variable done_;

    // DeepSeek V4.1 Flash: the per-run wire numbers of forwarded events and the
    // held ones' local queue sequences (what acknowledgeEvent needs).
    std::mutex state_;
    std::int64_t nextEventSeq_ = 0;
    std::map<std::int64_t, std::uint64_t> ackSeqs_;
};

}  // namespace pn
