// Coverage annotation of a ReachabilityGraph, merged over any number of runs.
//
// The database lives *beside* the graph (it never modifies it) and is keyed by
// StateId / EdgeId, so it keeps working while the graph grows lazily. Every
// run is recorded as a path; hit counts, first/last visit (run + trace-tree
// node, hence a full backtrace) and per-run "new coverage" figures are derived
// from those steps as they arrive.
//
// "Definite" coverage means the visit lies on the one known path. The
// "ambiguous" counters are reserved for the belief tracker, which will credit
// elements that were only possibly visited when internal events were silent.
//
// Error-marking bookkeeping (visits, satisfied?, dumps taken) follows the
// policy fields of the net's error markings, so the suggester and the ant
// colony can ask "is this error still a target?" and "may we still take a dump?".
//
// Not thread-safe.
#pragma once

#include "engine/CrashCredibility.h"  // GLM-5.3-Flash
#include "engine/Events.h"
#include "engine/ReachabilityGraph.h"
#include "engine/TraceTree.h"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace pn {

using RunId = std::uint32_t;
inline constexpr RunId kNoRun = 0xFFFFFFFFu;

enum class RunOutcome {
    Open,          // run in progress
    Completed,     // ended normally (target marking reached or tester stopped)
    Deadlock,
    Livelock,
    ErrorReached,  // ended in an error marking
    Deviation,     // SUT behaved in a way the specification does not allow
    SutCrashed,    // SUT died (controlled crash or not)
    Aborted        // tooling problem, run discarded for analysis purposes
};

struct ElementCoverage {
    std::uint64_t definite = 0;
    std::uint64_t ambiguous = 0;  // possible-but-not-certain visits (belief tracker)
    NodeId firstNode = kNoNode;   // trace-tree node of the first definite visit
    NodeId lastNode = kNoNode;
    RunId firstRun = kNoRun;
    RunId lastRun = kNoRun;
};

struct RunStep {
    EdgeId edge;
    StateId target;
};

// A post-mortem collected during a run.
struct DumpRecord {
    std::uint32_t stepIndex = 0;  // number of steps taken when it was collected (0 = at the initial state)
    std::string marking;          // error marking it belongs to, if any
    DumpMode mode = DumpMode::Snapshot;
    std::vector<std::string> files;
    std::string stackText;

    // GLM-5.3-Flash: error transition it belongs to, if any (kNoTransition
    // otherwise). A dump belongs to a transition when the error came from an
    // error-tagged transition and no error marking was certain.
    TransitionIndex transition = kNoTransition;
};

// GLM-5.3-Flash: "no exit status known" sentinel for HookRecord::exit (a
// child's exit status is always 0..255).
inline constexpr int kHookNoExit = -1;

// GLM-5.3-Flash: one execution of a transition's command hook during a run
// (the "command" key of the JSON, engine-side execution in Session). Status
// words: "ok" (synchronous, exit status 0), "failed" (synchronous, non-zero
// exit status), "timeout" (synchronous, killed at --hook-timeout), "launched"
// (asynchronous: eggembt does not wait for it). `exit` is the child's exit status
// when known, kHookNoExit (-1, which no child can produce) otherwise; output
// carries the captured stdout+stderr of a synchronous command, truncated.
struct HookRecord {
    std::string transition;  // the transition id from the net
    std::string label;       // the transition label (may be empty)
    bool async = false;
    std::string status;
    int exit = kHookNoExit;
    std::string output;
};

struct RunRecord {
    RunId id = kNoRun;
    StateId initial = kNoState;
    std::string note;
    std::vector<RunStep> steps;      // definite steps credited so far (may skip ambiguous gaps)
    std::vector<SutEvent> events;    // live events, for runs driven by the BeliefTracker
    RunOutcome outcome = RunOutcome::Open;
    std::string detail;  // e.g. the error marking name or a deviation message
    std::vector<DumpRecord> dumps;
    std::uint32_t newStates = 0;  // states/edges this run covered for the first time
    std::uint32_t newEdges = 0;

    // GLM-5.3-Flash: what this run definitely touched, in visit order (with
    // repetitions). Lets the overlap analysis attribute every covered element
    // to a run exactly, including the states credited without a known edge
    // (ambiguous stretches) and the transitions credited as certainly fired
    // although their edge stayed ambiguous.
    std::vector<StateId> states;
    std::vector<TransitionIndex> transitions;

    // GLM-5.3-Flash: the crash entry ("t:<id>" / "m:<name>") this run's
    // spontaneous death was attributed to, when the outcome is sut_crashed.
    // Empty otherwise (tester-induced crashes, unattributed deaths). The
    // recording alone cannot re-derive it -- the belief at the death is not
    // recorded -- so it travels with the run for the rebuild's parity.
    std::string crashAttribution;

    // GLM-5.3-Flash: the crash attempts this run counted, as their entry keys
    // in counting order (transition entries may repeat: one key per definite
    // step; marking entries at most once). The trail travels with the run:
    // which credits the final settle adds after a death can be replayed only
    // from this trail, not from the recording alone. The rebuild applies it
    // in order, then the attribution, and the experiment continues exactly.
    std::vector<std::string> crashAttempts;

    // GLM-5.3-Flash: the command hooks this run executed, in firing order
    // (see HookRecord). The records travel with the run in the run log; the
    // commands are NOT re-executed when a recording is restored or replayed.
    std::vector<HookRecord> hookRuns;
};

struct CoverageConfig {
    std::uint32_t threshold = 1;  // definite visits after which an element counts as saturated

    // GLM-5.3-Flash: the crash-prediction experiment (engine/CrashCredibility.h).
    // crashLevel is the credibility level gamma of "certain deterministic"
    // (--crash-level); crashAttempts is the give-up budget of the reproduction
    // experiment for crashes nobody predicted, and the default for predictions
    // that do not carry their own "attempts" (--crash-attempts).
    double crashLevel = 0.95;
    std::uint32_t crashAttempts = 6;
};

// GLM-5.3-Flash: one crash under observation. A PREDICTED crash is declared in
// the net (on a transition or a named marking); an IMPLICIT crash is a
// spontaneous death nobody predicted, keyed by the transition of the run's
// last credited step. The entry carries the reproduction experiment
// (engine/CrashCredibility.h): attempts and crashes accumulate while the
// status is Seeking, and the status re-evaluates after every count.
struct CrashEntry {
    std::string key;              // "t:<transition id>" or "m:<marking name>"
    TransitionIndex transition = kNoTransition;  // for transition entries; kNoTransition otherwise
    bool predicted = false;       // declared in the net (vs implicit)
    std::optional<CrashPrediction> prediction;   // unset for implicit entries
    CrashStats stats;
    CrashStatus status = CrashStatus::Seeking;
    std::uint32_t dumps = 0;      // post-mortems taken for this entry (its max_dumps budget)
};

struct CoverageSummary {
    std::size_t statesKnown = 0, statesCovered = 0, statesExpanded = 0;
    std::size_t edgesKnown = 0, edgesCovered = 0;
    std::size_t transitionsTotal = 0, transitionsCovered = 0;
    std::size_t placesTotal = 0, placesCovered = 0;  // a place is covered once a visited marking held a token in it
};

class CoverageDb {
public:
    explicit CoverageDb(const ReachabilityGraph &rg, CoverageConfig cfg = {});

    const ReachabilityGraph &graph() const { return rg_; }
    const CoverageConfig &config() const { return cfg_; }

    // --- recording a run ------------------------------------------------

    // Starts a run at `initial` (which must already exist in the graph) and
    // counts the visit, unless `creditInitial` is false: the BeliefTracker
    // credits the initial state itself, once it knows it is definite.
    // An unfinished previous run is closed as Aborted.
    RunId beginRun(StateId initial, std::string note = {}, bool creditInitial = true);

    // Follows `edge` from the run's current state. Returns false, changing
    // nothing, if no run is open or the edge does not leave the current
    // state (or the current state is not expanded yet).
    bool step(EdgeId edge);

    void endRun(RunOutcome outcome, std::string detail = {});

    // Attaches a post-mortem to the open run at the current step. If the
    // current state is an error marking it also counts toward that marking's
    // max_dumps.
    void recordDump(DumpMode mode, std::vector<std::string> files, std::string stackText = {});
    // Same, with the error marking given explicitly (-1: none) instead of
    // derived from the current state, for runs where the exact state is not
    // always known.
    void recordDump(DumpMode mode, std::vector<std::string> files, std::string stackText,
                    int errorMarking);
    // GLM-5.3-Flash: with an error transition given (kNoTransition: none),
    // the dump counts toward that transition's max_dumps instead, as long as
    // no error marking is definite.
    void recordDump(DumpMode mode, std::vector<std::string> files, std::string stackText,
                    int errorMarking, TransitionIndex errorTransition);

    bool runOpen() const { return open_; }
    StateId currentState() const { return current_; }  // kNoState if no run is open
    NodeId currentNode() const { return node_; }

    std::size_t numRuns() const { return runs_.size(); }
    const RunRecord &run(RunId id) const { return runs_[id]; }

    // --- belief-tracker hooks ---------------------------------------------

    // Credits one definite visit that is NOT necessarily adjacent to the
    // previous one: when observations left an ambiguous stretch in between,
    // the trace-tree node simply hangs below the previous definite node (its
    // `via` is kNoEdge although its depth is > 1, which marks the gap).
    // `via` is the edge that led into `state`, if known. `countTransition`
    // false means the transition's hit is credited separately with
    // creditTransition (used for observed events whose edge is ambiguous
    // but whose transition is certain).
    void creditDefinite(StateId state, EdgeId via, bool countTransition = true);
    void creditTransition(TransitionIndex t);

    // Visits that lie on some but not all paths consistent with the observations.
    void addAmbiguousState(StateId s);
    void addAmbiguousEdge(EdgeId e);

    // Appends a live event to the open run's log.
    void recordEvent(const SutEvent &ev);

    // --- offered versus taken (GLM-5.3-Flash, doku section 12 item 3) ------
    //
    // Per SUT-move LABEL (the label string, not a transition: a label may map
    // to several transitions): how often the move was OFFERED to the SUT
    // (the model expected it while the session was waiting for the SUT) and
    // how often the SUT actually TOOK it (a model-known Observed event).
    // Feeds the ant colony's environment weights and the suggester's
    // robustness estimate; see Session and Suggester.
    struct MoveStat {
        std::uint64_t offered = 0;
        std::uint64_t taken = 0;
    };

    void recordOffered(const std::string &label);
    void recordTaken(const std::string &label);

    // taken/offered, clamped to [0, 1]. A move that was never offered has no
    // evidence either way and rates 1.0 -- in particular a silent internal
    // step (empty label), which the session never offers because it is never
    // visible.
    double takeRate(const std::string &label) const;
    const std::unordered_map<std::string, MoveStat> &moveStats() const { return moveStats_; }

    // --- queries ----------------------------------------------------------

    // Out-of-range ids (not yet covered, graph grew since) yield an empty entry.
    const ElementCoverage &state(StateId s) const;
    const ElementCoverage &edge(EdgeId e) const;
    std::uint64_t transitionHits(TransitionIndex t) const { return transitionHits_[t]; }
    std::uint64_t placeVisits(PlaceIndex p) const { return placeVisits_[p]; }

    bool isSaturated(StateId s) const { return state(s).definite >= cfg_.threshold; }
    bool isSaturatedEdge(EdgeId e) const { return edge(e).definite >= cfg_.threshold; }

    // Error markings: indices refer to net.errorMarkings.
    std::uint64_t errorMarkingHits(std::size_t i) const { return errorHits_[i]; }
    std::uint32_t errorTarget(std::size_t i) const;
    bool isErrorSatisfied(std::size_t i) const { return errorHits_[i] >= errorTarget(i); }
    std::uint64_t errorMarkingDumps(std::size_t i) const { return errorDumps_[i]; }
    // crash_dump is set and fewer than max_dumps dumps were taken so far.
    bool dumpAllowed(std::size_t i) const;
    std::uint64_t targetMarkingHits(std::size_t i) const { return targetHits_[i]; }

    // GLM-5.3-Flash: error TRANSITIONS. The visit counter is the ordinary
    // transitionHits(t); these are the dump queries.
    std::uint64_t errorTransitionDumps(TransitionIndex t) const
    {
        return t < errorTransDumps_.size() ? errorTransDumps_[t] : 0;
    }
    // error tag with a crash_dump request and fewer than max_dumps dumps
    // taken so far (mirror of dumpAllowed for error markings).
    bool errorTransitionDumpAllowed(TransitionIndex t) const;

    // --- crash predictions (GLM-5.3-Flash, engine/CrashCredibility.h) ------
    //
    // The database seeds itself from the net at construction: every predicted
    // transition and every predicted marking gets an entry with zero counts.
    // Implicit entries appear at the first attributed spontaneous crash. The
    // session drives the experiment:
    //   noteCrashAttempt  one visit of the predicted place (a definite step of
    //                     the transition, or a run certainly in the marking),
    //                     counted only while the entry is unresolved
    //   noteCrash         the run died and was attributed to this entry
    // The status re-evaluates after every count; a resolved entry never
    // counts again. addCrashEntry creates an entry only when the key is new,
    // so the implicit-entry creation is idempotent.
    void addCrashEntry(std::string key, TransitionIndex transition,
                       std::optional<CrashPrediction> prediction);
    const CrashEntry *findCrashEntry(const std::string &key) const;
    // All entries in insertion order (predicted first, then implicit).
    const std::vector<CrashEntry> &crashEntries() const { return crashEntries_; }

    void noteCrashAttempt(const std::string &key);
    void noteCrash(const std::string &key);
    // GLM-5.3-Flash: adopts recorded hook executions into a run record
    // (restore/replay: the records travel with the run, the commands are
    // NOT re-executed). Replaces whatever hookRuns the record carried.
    void adoptHookRuns(RunId id, std::vector<HookRecord> hooks);
    // GLM-5.3-Flash: records one executed command hook on the open run
    // (the live session's own execution; no-op when no run is open).
    void recordHookRun(HookRecord rec);
    // Remembers which crash entry the open run's spontaneous death was
    // attributed to (written once, before endRun; goes into the run log).
    void recordCrashAttribution(std::string key);
    // The prediction's crash_dump request with its own max_dumps budget
    // (mirror of dumpAllowed). recordCrashDump counts the taken post-mortem.
    bool crashDumpAllowed(const std::string &key) const;
    void recordCrashDump(const std::string &key, DumpMode mode, std::vector<std::string> files,
                         std::string stackText = {});

    CoverageSummary summary() const;

    // Backtraces: the steps of the first/last definite visit, oldest first.
    // With maxSteps set, only the last maxSteps steps are returned.
    enum class Which { First, Last };
    std::vector<TraceStep> stateBacktrace(StateId s, Which which = Which::First,
                                          std::size_t maxSteps = static_cast<std::size_t>(-1)) const;
    std::vector<TraceStep> edgeBacktrace(EdgeId e, Which which = Which::First,
                                         std::size_t maxSteps = static_cast<std::size_t>(-1)) const;

    const TraceTree &traceTree() const { return tree_; }

private:
    void ensureSized();
    void touch(ElementCoverage &c, std::uint32_t &newCounter);
    void onStateVisited(StateId s);
    // GLM-5.3-Flash: the calibration an entry evaluates itself with (the
    // class table, the explicit p/attempts overrides, or the neutral prior
    // and --crash-attempts budget of an implicit entry).
    CrashStatus evaluateCrashStatus(const CrashEntry &e) const;

    const ReachabilityGraph &rg_;
    CoverageConfig cfg_;

    std::vector<std::uint64_t> transitionHits_;

    // GLM-5.3-Flash: offered/taken counts per SUT-move label (see above).
    std::unordered_map<std::string, MoveStat> moveStats_;
    std::vector<std::uint64_t> placeVisits_;
    std::vector<std::uint64_t> errorHits_;
    std::vector<std::uint64_t> targetHits_;
    std::vector<std::uint64_t> errorDumps_;
    std::vector<std::uint64_t> errorTransDumps_;  // GLM-5.3-Flash: per transition

    std::vector<ElementCoverage> stateCov_;
    std::vector<ElementCoverage> edgeCov_;
    std::vector<RunRecord> runs_;
    TraceTree tree_;

    // GLM-5.3-Flash: the crash experiments, keyed by "t:<id>" / "m:<name>",
    // kept in insertion order for the report and the board.
    std::vector<CrashEntry> crashEntries_;
    std::unordered_map<std::string, std::size_t> crashIndex_;

    bool open_ = false;
    StateId current_ = kNoState;
    NodeId node_ = kRootNode;
};

}  // namespace pn
