// An ISutPlugin that talks to an agent over the line protocol (Protocol.h).
// The transport is abstract: a socket, a pipe to a child process, standard
// streams, an in-memory loopback in tests (see io/Transports.h).
//
// Threads. The Session calls the ISutPlugin methods from its thread. The
// transport delivers incoming lines from whatever thread it likes (typically
// its own); the plug-in turns `event` messages straight into pushes on the
// Session's EventQueue, so the Session thread never sits in the middle of the
// agent's event stream. Everything shared is guarded by one mutex; the plug-in
// never calls the transport while holding it, so a transport that delivers the
// answer synchronously inside sendLine() works too.
//
// Lifetime. The queue belongs to the Session. stopRun() does not return before
// every event push that was already under way has finished, and afterwards the
// plug-in never touches the queue again, so the Session may destroy it at once.
//
// Runs are numbered. start_run carries a run number the agent echoes in every
// event of that run; events of any other run (late arrivals of the previous
// run) are dropped, and so is everything after stopRun().
//
// Synchronized events (DeepSeek V4.1 Flash, doku section 19). An agent that can hold
// its SUT says "sync": true in its hello. start_run then carries the labels to
// hold at, and an event the agent is holding carries "seq" plus "sync": true.
// The plug-in remembers which EventQueue sequence belongs to that wire
// sequence; when the Session calls acknowledgeEvent(queueSeq) -- after it has
// handled the event and any post-mortem -- the ack goes back. If the run ends
// instead, stopRun() releases the SUT: no ack is sent.
#pragma once

#include "engine/Protocol.h"
#include "engine/SutPlugin.h"

#include <chrono>
#include <condition_variable>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>

namespace pn {

class ILineTransport {
public:
    virtual ~ILineTransport() = default;

    // Sends one line (no newline in it). False if the connection is gone.
    virtual bool sendLine(const std::string &line) = 0;

    // The function to call for every complete line received, in the order the
    // lines arrived. May be called from any thread. Set before traffic flows.
    virtual void setLineHandler(std::function<void(const std::string &)> handler) = 0;

    // Called once when the peer closes the connection (or it breaks). Optional:
    // transports that cannot tell may ignore it.
    virtual void setCloseHandler(std::function<void()> handler) { (void)handler; }
};

struct RemoteTimeouts {
    std::chrono::milliseconds hello{5000};
    std::chrono::milliseconds runStart{10000};
    std::chrono::milliseconds postMortem{60000};
};

class RemotePlugin : public ISutPlugin {
public:
    explicit RemotePlugin(ILineTransport &transport, RemoteTimeouts timeouts = {});

    // Blocks until the agent's hello has arrived (true) or `timeout` passed.
    bool waitForHello(std::chrono::milliseconds timeout);
    std::string agentName() const;

    // Tells the agent the session is over.
    void shutdown();

    // --- ISutPlugin ---------------------------------------------------------------
    PluginCapabilities capabilities() const override;
    bool startRun(const std::string &initialMarking, const PluginRunOptions &options, EventQueue &events,
                  std::string *error) override;
    bool stimulate(const std::string &label, std::string *error) override;
    PostMortem postMortem(DumpMode mode) override;
    void acknowledgeEvent(std::uint64_t queueSeq) override;  // DeepSeek V4.1 Flash
    void stopRun(StopReason reason) override;
    std::string pollFault() override;

private:
    struct Shared {
        std::mutex mutex;
        std::condition_variable changed;

        bool helloSeen = false;
        std::string name;
        PluginCapabilities caps;

        EventQueue *events = nullptr;  // non-null while a run is active
        std::size_t pushing = 0;       // event pushes in progress (outside the lock)

        // DeepSeek V4.1 Flash: synchronized events the agent is holding the SUT at,
        // keyed by the EventQueue sequence (what the Session acks), holding
        // the run number and wire sequence the ack must name.
        std::map<std::uint64_t, std::pair<std::int64_t, std::int64_t>> pendingAcks;
        std::int64_t run = 0;          // number of the active (or last) run
        bool startAnswered = false;
        bool startOk = false;
        std::string startError;

        std::int64_t nextPostMortemId = 1;
        std::map<std::int64_t, PostMortem> postMortems;

        std::string fault;
        bool shuttingDown = false;  // we told the agent to shut down: its closing is no fault
    };

    static void onLine(const std::shared_ptr<Shared> &shared, const std::string &line);
    // Stops accepting events and waits for the pushes still in progress.
    void detachEvents();

    ILineTransport &transport_;
    RemoteTimeouts timeouts_;
    std::shared_ptr<Shared> shared_;
};

}  // namespace pn
