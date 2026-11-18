// The test session: ties the model, the live SUT and all the guidance together.
//
// A Session owns the reachability graph, the merged coverage, the belief
// tracker, the suggester, the ant colony and the event queue, and drives a
// plug-in through test runs on ONE thread (the caller's). The plug-in's events
// reach it asynchronously through the queue.
//
// ONE RUN, repeated until nothing is left to cover:
//   1. start the SUT in an initial marking
//   2. wait briefly for events, then feed every queued event to the tracker
//   3. react to what the tracker says: a deviation, deadlock or run-time
//      livelock ends the run (optionally with a post-mortem); entering an error
//      marking or certainly stepping an error transition takes a post-mortem
//      if its settings ask for one and its budget (max_dumps) allows, and a
//      Crash ends the run (GLM-5.3-Flash); certainly entering an intentional
//      ("good") target marking ends the run -- and the campaign
//   4. if something changed: let the ants walk, ask the suggester, and send the
//      suggested input; while an input is in flight (sent, not yet reported
//      back as a Stimulated event) no second input is sent
//   5. stop when the suggester says Done or Stuck and the SUT has settled, when
//      the run is too long, or when the SUT or plug-in misbehaves (Aborted)
//
// "SETTLED" matters: Done and Stuck end a run only if the model expects no
// further output. If outputs are still possible the Session keeps listening,
// because the SUT may be busy: that is how a run-time livelock gets noticed
// (events keep coming without progress) and how a SUT that fell silent when it
// owed an output gets noticed (the plug-in's Quiescent event is a deviation).
//
// WHAT IS PERSISTED. After every run the `runSink` gets the run as a
// PersistedRun (events with capture timestamps, dumps, outcome); append it to a
// run log. restore() replays such runs into a fresh session, so coverage,
// target phases and dump budgets continue where the last session stopped.
//
// Not thread-safe, except requestStop().
#pragma once

#include <sys/types.h>  // GLM-5.3-Flash: pid_t of the asynchronous hook children

#include <map>

#include "engine/AntColony.h"
#include "engine/BeliefTracker.h"
#include "engine/CoverageDb.h"
#include "engine/EventQueue.h"
#include "engine/LivelockAnalysis.h"
#include "engine/ReachabilityGraph.h"
#include "engine/RunLog.h"
#include "engine/Suggester.h"
#include "engine/SutPlugin.h"
#include "engine/TList.h"
#include "engine/TargetBoard.h"

#include <atomic>
#include <chrono>
#include <functional>
#include <string>
#include <vector>

namespace pn {

// What to do about faults that are not error markings of the model.
struct DumpPolicy {
    DumpMode onDeviation = DumpMode::None;
    DumpMode onDeadlock = DumpMode::None;
    DumpMode onLivelock = DumpMode::None;
};

struct RunReport;

struct SessionConfig {
    GenerationLimits graph;
    CoverageConfig coverage;
    TrackerConfig tracker;       // run-time livelock timeout lives here
    SuggesterConfig suggester;
    AntConfig ants;
    TargetPolicies targets;      // deadlock / livelock policies; copied into suggester and ants

    bool useAnts = true;
    std::size_t antIterationsPerSuggestion = 1;

    // Explore the whole model first (up to the graph limits) and run the static
    // livelock analysis; its certain findings become livelock targets.
    bool analyzeModel = true;
    LivelockOptions livelock;

    DumpPolicy dumps;
    PluginRunOptions plugin;     // quiescence timeout, passed to the plug-in

    // GLM-5.3-Flash: command hooks (the "command" key of the JSON, doku
    // section 18). hookTimeoutSeconds bounds a SYNCHRONOUS hook: a child
    // still running after it is killed and its execution counts as failed.
    // logPath, when set, is handed to every hook as MBT_LOG (the path of the
    // open run log), so a hook can append to what the session records.
    std::uint32_t hookTimeoutSeconds = 60;
    std::string logPath;

    std::vector<std::size_t> initialMarkings;  // indices to cycle through; empty: all of them

    std::size_t maxRuns = 100;
    std::size_t maxEventsPerRun = 10000;
    std::size_t maxConsecutiveAborts = 3;
    std::size_t eventQueueCapacity = 65536;

    // GLM-5.3-Flash: seed for the suggester's tie-breaks. 0 (the default)
    // keeps the fully deterministic choice: among equally scored inputs the
    // more robust one wins, then the lower transition index. A non-zero seed
    // picks randomly -- but reproducibly -- among exactly equal candidates, so
    // campaigns with different seeds test in a different order while each
    // single campaign stays reproducible. (The ant colony is seeded
    // separately, with ants.seed / --ant-seed.)
    std::uint64_t selectionSeed = 0;

    // GLM-5.3-Flash: whether the catastrophe pheromones are priced by the way
    // in (see AntConfig::catastropheDeposit). The wall clock is the reasoning:
    // reaching a deeper error state costs more campaign time, so its way in
    // deserves harder repel. With false the deposit stays flat
    // (depositCatastrophe), e.g. when reaching speed is not the point or the
    // SUT's timing is dominated by something a run cannot measure. This is
    // also what --flat-catastrophe sets on the CLI. Independent of it, a run
    // whose way in crossed an "unpredictable" (human-operator) transition
    // deposits flat as well: the wall clock says nothing about a human's
    // reaction time.
    bool catastropheTimePricing = true;

    // GLM-5.3-Flash: how much of an earlier round's pheromone teaching a
    // continued campaign starts with, in percent (doku section 16). The run
    // log saves every round's runs; restoring them rebuilds their milestone
    // and catastrophe deposits -- and the (marking, T-list) pair deposits --
    // scaled by this factor (the merge of several rounds is unchanged: the
    // deposits accumulate as before). 50, the default, halves the inherited
    // knowledge: it is second-hand and may be stale, so a fresh round should
    // weigh what it learns itself higher and keep exploring. 100 keeps the
    // old full strength, 0 ignores the inherited teaching entirely. The
    // factor does not compound across rounds: the log stores recordings, not
    // pheromone state, so every continuation re-derives the knowledge at
    // log strength times this factor. Also what --restored-pheromones sets
    // on the CLI.
    unsigned restoredPheromonePercent = 50;

    std::chrono::milliseconds poll{20};                  // how long to wait for events per loop
    std::chrono::milliseconds idleTimeout{10000};        // no event while waiting: abort (0: never)
    std::chrono::milliseconds stimulusAckTimeout{5000};  // an input never reported back: abort (0: never)

    // Called with every finished run, e.g. to append it to a run log.
    std::function<void(const PersistedRun &)> runSink;
    // Called with the report of every finished run, e.g. to print progress.
    std::function<void(const RunReport &)> progress;
};

struct RunReport {
    RunId id = kNoRun;
    RunOutcome outcome = RunOutcome::Aborted;
    std::string detail;
    std::size_t events = 0;
    std::size_t stimuli = 0;                 // inputs sent
    std::size_t dumps = 0;                   // post-mortems collected
    std::vector<std::string> errorsReached;  // names of error markings the SUT was certainly in

    // GLM-5.3-Flash: labels of milestone transitions the SUT certainly stepped
    // in this run (mirror of errorsReached; a milestone is a success, so the
    // reaction is positive pheromone instead of ending the run).
    std::vector<std::string> milestonesReached;

    std::vector<std::string> notes;          // e.g. "plug-in cannot snapshot: no dump taken"
    bool coverageDone = false;               // the suggester had nothing left to cover
    bool stuck = false;                      // goals remain but none reachable from here

    // GLM-5.3-Flash: name of the intentional ("good") target marking the run
    // ended in, if any. Reaching one certainly ends the campaign.
    std::string goodTarget;
    std::uint32_t newStates = 0;
    std::uint32_t newEdges = 0;
    std::int64_t durationMs = 0;  // GLM-5.3-Flash: wall-clock time of the run
};

struct CampaignReport {
    std::vector<RunReport> runs;
    std::vector<std::string> warnings;  // problems with the model found by the static analysis
    bool complete = false;  // everything explored is covered and all targets are satisfied
    std::string stopReason;
    CoverageSummary summary;
    std::int64_t wallMs = 0;  // GLM-5.3-Flash: wall-clock time of the whole campaign
};

class Session {
public:
    explicit Session(const Net &net, SessionConfig config = {});
    ~Session();  // GLM-5.3-Flash: reaps the asynchronous hook children

    Session(const Session &) = delete;
    Session &operator=(const Session &) = delete;

    // Explores the model and runs the static livelock analysis (done once;
    // runCampaign does it automatically if config.analyzeModel is set). A model
    // whose initial marking lies inside a terminal cycle without a target marking
    // (a cyclic protocol that never declares an idle state as a target) is a livelock
    // by definition and leaves the suggester nowhere to go: this is reported in
    // warnings().
    const LivelockReport &analyzeModel();

    // Replays a recorded run so that coverage continues from it.
    ReplayResult restore(const PersistedRun &run);

    RunReport runOnce(ISutPlugin &plugin);
    CampaignReport runCampaign(ISutPlugin &plugin);

    // Thread-safe: ends the current run (Aborted) and the campaign.
    void requestStop() { stop_ = true; }

    const Net &net() const { return net_; }
    const ReachabilityGraph &graph() const { return rg_; }
    const CoverageDb &coverage() const { return db_; }
    const LivelockReport &staticReport() const { return static_; }
    const std::vector<std::string> &warnings() const { return warnings_; }
    const AntColony &ants() const { return ants_; }
    // GLM-5.3-Flash: whether ant-colony guidance takes part in this session
    // (switched off with --no-ants or SessionConfig::useAnts = false).
    bool antsEnabled() const { return cfg_.useAnts; }

    // GLM-5.3-Flash: the configuration this session was built with, so the
    // report can say which switches steered the campaign (target policies,
    // post-mortem switches, coverage threshold, ...). Read-only; the Session
    // never changes its config after construction.
    const SessionConfig &config() const { return cfg_; }

private:
    std::size_t pickInitialMarking();

    // GLM-5.3-Flash: after a successful replay, rebuild from the recording the
    // pheromones the live run would have left behind (see Session::restore).
    void rebuildPheromones(const PersistedRun &run, const RunRecord &rec);

    // GLM-5.3-Flash: after a successful replay, continue the crash experiments
    // with what the run's carried trail and attribution say (see
    // Session::restore). Runs before the pheromone rebuild, whose seek-vs-
    // catastrophe choice reads the entry statuses this settles.
    void rebuildCrashExperiment(const PersistedRun &run);

    Net net_;
    SessionConfig cfg_;
    ReachabilityGraph rg_;
    CoverageDb db_;
    BeliefTracker tracker_;
    Suggester suggester_;
    AntColony ants_;
    EventQueue queue_;

    // GLM-5.3-Flash: the live run's T-list cascade (doku section 16). Its
    // length comes from the ant configuration at construction; it is reset at
    // every run's begin and advanced with every definite step the tracker
    // credits. The pair deposits and the suggester's pair consults read it;
    // the restore's rebuild walks its own copy over the recorded steps.
    TraceCascade run_;

    LivelockReport static_;
    std::vector<std::string> warnings_;
    bool analyzed_ = false;
    std::atomic<bool> stop_{false};
    std::size_t runCounter_ = 0;

    // GLM-5.3-Flash: command hooks (doku section 18). hooksFired_ counts the
    // executions per transition over the WHOLE campaign (a hook's "times"
    // budget); hookChildren_ collects the asynchronous children so they are
    // reaped -- never killed -- at destruction.
    std::map<TransitionIndex, std::uint32_t> hooksFired_;
    std::vector<pid_t> hookChildren_;
};

}  // namespace pn
