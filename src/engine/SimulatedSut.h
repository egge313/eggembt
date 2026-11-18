// A SUT that behaves exactly as its model says, simulated on its own thread, as
// a plug-in. It is useful to try a model before a real SUT exists (`mbt run
// --simulate`, `mbt agent-sim`), as a reference for plug-in authors, and in the
// tests. Switches make it misbehave in the ways a Session has to cope with.
//
// How it behaves: inputs are applied when they arrive (or after `ackDelayMs`),
// and between inputs the simulated SUT takes its own moves (outputs and
// internal steps) at random, weighted by `weight`, one per `stepDelayMs`.
// Visible moves (observable, traced internal) become events; silent internal
// moves do not. After the quiescence timeout without any move it reports
// Quiescent once.
//
// GLM-5.3-Flash: it can also DIE on its own, which a real SUT can and the
// protocol could not even express before EventKind::Died: handling a
// stimulus from `crashAt` kills it with the mapped probability (the seeded
// reproduction of a predicted crash), and `crashAfterEvents` kills it after
// that many events wherever it is. Either way the acknowledgement of the
// input goes out first, then one Died event with the reason, and the SUT
// stays down for the rest of the run.
#pragma once

#include "engine/Semantics.h"
#include "engine/SutPlugin.h"
#include "model/Net.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <map>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

namespace pn {

struct SimulatedSutOptions {
    std::uint64_t seed = 1;
    bool snapshot = true;                   // capabilities reported to the Session
    bool crash = true;
    int stepDelayMs = 1;                    // pause between the SUT's own steps
    int ackDelayMs = 0;                     // an input is received (and reported) this long after it was sent
    std::map<std::string, double> weight;   // transition id -> relative weight for the SUT's own choices (default 1, 0 = never)
    int deviateAfterEvents = -1;            // after this many events push `deviationLabel` once
    std::string deviationLabel;
    bool failStart = false;
    bool silent = false;                    // hung SUT: no events, no quiescence report, inputs ignored
    int burstAtStart = 0;                   // extra events pushed synchronously inside startRun
    std::int64_t clockStepMs = 0;           // > 0: logical timestamps advancing by this per event

    // GLM-5.3-Flash: spontaneous deaths -- the surface the plug-in protocol
    // was missing until EventKind::Died. crashAt maps a stimulus label to the
    // probability that handling it kills the SUT (the seeded reproduction of
    // a predicted crash: the CLI fills it from the net's predictions).
    // crashAfterEvents ends the SUT once that many events were pushed,
    // wherever it is -- a death nobody predicted, for the implicit entry.
    // Either way the SUT pushes the Stimulated acknowledgement first (the
    // tester did send the input), then exactly one Died event with the
    // reason in the label, and stays down for the rest of the run.
    std::map<std::string, double> crashAt;
    int crashAfterEvents = -1;
};

class SimulatedSut : public ISutPlugin {
public:
    explicit SimulatedSut(const Net &net, SimulatedSutOptions options = {});
    ~SimulatedSut() override;

    PluginCapabilities capabilities() const override;
    bool startRun(const std::string &initial, const PluginRunOptions &options, EventQueue &events,
                  std::string *error) override;
    bool stimulate(const std::string &label, std::string *error) override;
    PostMortem postMortem(DumpMode mode) override;
    void stopRun(StopReason reason) override;
    std::string pollFault() override;

    // Introspection and fault injection, mainly for tests.
    void setFault(const std::string &text);
    std::vector<std::string> stimuliReceived() const;
    std::vector<DumpMode> dumpModes() const;
    bool crashed() const;
    int stopCalls() const { return stopCalls_; }

private:
    struct PendingStim {
        std::chrono::steady_clock::time_point due;
        std::string label;
    };

    void halt();
    std::int64_t stamp();
    void push(EventKind kind, const std::string &label);  // callers hold m_
    void applyStimulus(const std::string &label);         // callers hold m_
    double weightOf(const Transition &t) const;
    void loop();

    const Net &net_;
    SimulatedSutOptions opt_;

    mutable std::mutex m_;
    std::thread worker_;
    bool stop_ = false;
    bool crashed_ = false;
    bool quiescentSent_ = false;
    bool deviated_ = false;
    int eventsPushed_ = 0;
    int dumps_ = 0;
    std::atomic<int> stopCalls_{0};
    std::int64_t quiescenceMs_ = 1000;
    std::int64_t clock_ = 0;
    std::uint64_t generation_ = 0;
    std::mt19937_64 rng_;
    std::string fault_;
    Marking marking_;
    EventQueue *events_ = nullptr;
    std::chrono::steady_clock::time_point start_;
    std::chrono::steady_clock::time_point lastActivity_;
    std::vector<PendingStim> pendingStim_;
    std::vector<std::string> stimuli_;
    std::vector<DumpMode> modes_;
};

}  // namespace pn
