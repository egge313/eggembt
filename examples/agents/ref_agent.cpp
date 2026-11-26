// GLM-5.3-Flash: the reference agent shim for C++ SUTs (doku section 12,
// item 7). The reusable part is engine/AgentServer: it wraps ANY ISutPlugin
// in the line protocol, so a tracker ('eggembt run MODEL --agent ...') can test a
// SUT that lives in another process. This program shows the whole path for a
// hand-written C++ SUT:
//
//   your SUT  ->  an ISutPlugin that drives it  ->  AgentServer  ->  stdio
//
// and nothing in the SUT knows about the model: the machine below is a plain
// state machine. Test it with:
//
//   eggembt run examples/agents/coffee.json --agent "examples/agents/ref-agent"
//
// WHAT A WRAPPER MUST DO (see engine/SutPlugin.h for the contract):
//   startRun   put the SUT into the state the named initial marking of the
//              model describes (a real SUT maps that name to its own states),
//              remember the EventQueue, and start observing the SUT;
//   stimulate  hand ONE input to the SUT, exactly as suggested; the SUT's
//              acceptance is reported by pushing a Stimulated event;
//   events     everything the SUT does that the model talks about is pushed
//              as an Observed event, in the order it happened, each with a
//              capture timestamp in milliseconds (differences drive the
//              run-time livelock watch);
//   quiescence when the SUT has been silent for options.quiescenceTimeoutMs,
//              push ONE Quiescent event (the tracker has no clock; a tracker
//              that expected an output treats this as a deviation);
//   death      GLM-5.3-Flash: when the SUT dies on its own (it crashed, its
//              process exited, the connection broke), push exactly ONE Died
//              event -- the reason in the label, if any is known -- and push
//              nothing further for the run. The campaign attributes the
//              death to the crash it is testing (a prediction of the model,
//              or the move in flight) and seeks or avoids the way in
//              accordingly; a death that is never reported looks like a hung
//              SUT and only trips the timeouts;
//   postMortem a Snapshot is non-destructive, a Crash ends the SUT; say what
//              you cannot do in capabilities() so the tracker never asks. A
//              SUT that already died cannot be asked for a post-mortem; a
//              wrapper that CAN still read one (a core file, a log) should
//              honour the request even after its own Died event;
//   stopRun    end the run: no more events may be pushed after this returns.
//
// The wrapper below is deliberately small and single-threaded where possible:
// the machine reacts synchronously to inputs, and one worker thread exists
// only because quiescence needs someone with a clock.

#include "engine/AgentServer.h"
#include "engine/SutPlugin.h"
#include "io/Transports.h"

#include <chrono>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>

namespace {

using namespace pn;

// ---------------------------------------------------------------------------
// The system under test: a small coffee machine. Its behaviour is
// hand-written; it knows nothing about Petri nets or the protocol.
// ---------------------------------------------------------------------------

enum class MachineState { Idle, Paid, BrewingCoffee, BrewingTea };

class CoffeeMachine {
public:
    // An input. Returns the output the machine produces right away, or an
    // empty string when the input was ignored (as real machines ignore
    // buttons that make no sense in the current state). This machine brews
    // and serves in one go: after the drink is out it is back in Idle, which
    // is exactly what the model says (serve leads back to the idle place).
    std::string input(const std::string &label)
    {
        if (label == "coin" && state_ == MachineState::Idle) {
            state_ = MachineState::Paid;
            return {};
        }
        if (label == "brew_coffee" && state_ == MachineState::Paid) {
            state_ = MachineState::Idle;  // brewed and served
            return "coffee.served";
        }
        if (label == "brew_tea" && state_ == MachineState::Paid) {
            state_ = MachineState::Idle;  // brewed and served
            return "tea.served";
        }
        if (label == "refund" && state_ == MachineState::Paid) {
            state_ = MachineState::Idle;
            return {};
        }
        return {};  // ignored: the button does nothing here
    }

    void reset()
    {
        state_ = MachineState::Idle;
    }

private:
    MachineState state_ = MachineState::Idle;
};

// ---------------------------------------------------------------------------
// The plug-in wrapper: bridges the machine to the tracker's event queue and
// the protocol's rules (acknowledgements, quiescence, post-mortems).
// ---------------------------------------------------------------------------

class RefSutPlugin : public ISutPlugin {
public:
    PluginCapabilities capabilities() const override
    {
        // DeepSeek V4.1 Flash: no snapshots, no crashes in this example, and
        // exactly one initial state. Advertising the name lets the Session
        // skip other markings instead of failing on the first run (doku
        // section 41).
        PluginCapabilities caps;
        caps.restartMs = 10;
        caps.initialMarkings = {"start"};
        return caps;
    }

    bool startRun(const std::string &initialMarking, const PluginRunOptions &options,
                  EventQueue &events, std::string *error) override
    {
        halt();
        // The reference machine has one meaningful initial state, "start".
        // A real wrapper maps each named initial marking of the model to the
        // states its SUT can actually start in.
        if (initialMarking != "start") {
            if (error)
                *error = "the reference machine only starts in \"start\"";
            return false;
        }
        machine_.reset();
        events_ = &events;
        quiescenceMs_ = options.quiescenceTimeoutMs;
        start_ = std::chrono::steady_clock::now();
        lastActivity_ = start_;
        quiescentSent_ = false;
        stop_ = false;
        worker_ = std::thread([this] { watchQuiescence(); });
        return true;
    }

    bool stimulate(const std::string &label, std::string *) override
    {
        const std::lock_guard<std::mutex> lock(m_);
        const std::string output = machine_.input(label);  // may be empty: ignored input
        push(EventKind::Stimulated, label);                // the input was accepted as such
        if (!output.empty())
            push(EventKind::Observed, output);             // the machine's own move
        return true;
    }

    PostMortem postMortem(DumpMode) override
    {
        PostMortem pm;
        pm.ok = false;
        pm.error = "the reference machine cannot take post-mortems";
        return pm;
    }

    void stopRun(StopReason) override
    {
        halt();
    }

private:
    // Events are pushed in the order they were captured, each with a
    // millisecond timestamp of the run's own clock (differences matter, not
    // the origin).
    std::int64_t stamp() const
    {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now() - start_).count();
    }

    void push(EventKind kind, const std::string &label)
    {
        if (!events_)
            return;
        events_->push(SutEvent{kind, label, stamp()});
        lastActivity_ = std::chrono::steady_clock::now();
        quiescentSent_ = false;
    }

    // The tracker has no clock: silence for the run's quiescence timeout is
    // the plug-in's report. One Quiescent event per quiet stretch.
    void watchQuiescence()
    {
        std::unique_lock<std::mutex> lock(m_);
        for (;;) {
            if (stop_)
                return;
            const std::int64_t quietFor = stamp()
                - std::chrono::duration_cast<std::chrono::milliseconds>(lastActivity_ - start_).count();
            if (quiescenceMs_ > 0 && quietFor >= quiescenceMs_ && !quiescentSent_) {
                quiescentSent_ = true;
                events_->push(SutEvent{EventKind::Quiescent, "", stamp()});
                continue;
            }
            cv_.wait_for(lock, std::chrono::milliseconds(5));
        }
    }

    void halt()
    {
        {
            const std::lock_guard<std::mutex> lock(m_);
            stop_ = true;
        }
        cv_.notify_all();
        if (worker_.joinable())
            worker_.join();
        events_ = nullptr;
    }

    CoffeeMachine machine_;
    EventQueue *events_ = nullptr;
    std::int64_t quiescenceMs_ = 500;
    std::chrono::steady_clock::time_point start_;
    std::chrono::steady_clock::time_point lastActivity_;
    bool quiescentSent_ = false;
    bool stop_ = false;
    std::mutex m_;
    std::condition_variable cv_;
    std::thread worker_;
};

}  // namespace

int main()
{
    // GLM-5.3-Flash: the protocol owns standard input and output; diagnostics
    // go to standard error. FdTransport, not StreamTransport: its reader can
    // be stopped cleanly, so this process can still exit while the tracker
    // keeps the pipe open (same reasoning as eggembt agent-sim).
    RefSutPlugin sut;
    std::unique_ptr<ILineTransport> link;
#ifdef MBT_POSIX_IO
    link = std::make_unique<FdTransport>(0, 1, false);
#else
    link = std::make_unique<StreamTransport>(std::cin, std::cout);  // the protocol owns standard output here
#endif
    AgentServer server(sut, *link, "eggembt-ref-agent");
    server.start();
    while (!server.waitUntilFinished(std::chrono::milliseconds(200))) {
    }
    return 0;
}
