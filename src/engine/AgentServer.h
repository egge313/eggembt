// Exposes any ISutPlugin as an agent speaking the line protocol: the generic
// agent shim. Wrap your SUT in an ISutPlugin (or use the SimulatedSut), give the
// server a transport (standard streams, a socket, a pipe), and a tracker on the
// other end can test it. See docs/PLUGIN_PROTOCOL.md.
//
// The server answers start_run / stimulate / post_mortem / stop_run by calling
// the plug-in, and forwards the plug-in's events to the tracker as `event`
// messages, stamped with the run number of the current run, in the order the
// plug-in pushed them. It says hello when started.
#pragma once

#include "engine/EventQueue.h"
#include "engine/Protocol.h"
#include "engine/RemotePlugin.h"
#include "engine/SutPlugin.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>

namespace pn {

class AgentServer {
public:
    AgentServer(ISutPlugin &sut, ILineTransport &transport, std::string name = "mbt-agent");
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
};

}  // namespace pn
