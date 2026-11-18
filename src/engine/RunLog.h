// Persistent form of a run, and replay of persisted runs into a CoverageDb.
//
// Runs are stored from a *named initial marking*, never by StateId/EdgeId:
// those depend on the order in which the lazy graph happened to be expanded
// and mean nothing in the next session. Two forms exist:
//   * path runs: the exact transition ids fired (model simulations, fully
//     determined runs);
//   * event runs: the live events of the SUT as the plug-in reported them.
//     Replay feeds them through a BeliefTracker again, so the ambiguity
//     handling (definite vs. ambiguous coverage) is reproduced exactly.
// Replaying against a (possibly fresh) graph rebuilds all coverage,
// backtraces and counters, and tells you when a model change has made a
// trace incompatible.
//
// This header is Qt-free; the JSON-lines file format lives in io/RunLogIO.
#pragma once

#include "engine/BeliefTracker.h"
#include "engine/CoverageDb.h"
#include "engine/Events.h"

#include <string>
#include <vector>

namespace pn {

struct PersistedRun {
    std::string initialMarking;             // name in net.initialMarkings
    std::vector<std::string> transitions;   // path run: transition ids, in firing order
    std::vector<SutEvent> events;           // event run: live events (then `transitions` is empty)
    RunOutcome outcome = RunOutcome::Completed;
    std::string detail;
    std::string note;
    std::vector<DumpRecord> dumps;          // ascending stepIndex (steps of a path run, events of an event run)

    // GLM-5.3-Flash: the crash entry ("t:<id>" / "m:<name>") this run's
    // spontaneous death was attributed to, when the outcome is sut_crashed
    // (empty otherwise). The recording alone cannot re-derive it -- the
    // belief at the death is not recorded -- so it travels with the run for
    // the rebuild's parity.
    std::string crashAttribution;

    // GLM-5.3-Flash: the crash attempts this run counted, as their entry keys
    // in counting order. The settle timing of the recorded steps cannot be
    // replayed (which credits the final settle adds after a death is not in
    // the recording), so the trail itself travels with the run.
    std::vector<std::string> crashAttempts;

    // GLM-5.3-Flash: the command hooks this run executed, in firing order.
    // They travel with the run so the report shows the evidence of every
    // data check after a restore; the commands are NOT re-executed when the
    // recording is restored or replayed.
    std::vector<HookRecord> hooks;
};

const char *toString(RunOutcome o);
bool parseOutcome(const std::string &s, RunOutcome &out);  // "open" is not accepted
const char *toString(DumpMode m);
bool parseDumpMode(const std::string &s, DumpMode &out);

// Converts a finished run. The initial state must equal one of the net's named
// initial markings; otherwise initialMarking is left empty (and replay fails).
PersistedRun toPersisted(const ReachabilityGraph &rg, const RunRecord &run);

struct ReplayResult {
    bool ok = false;
    std::string error;    // set when !ok
    RunId run = kNoRun;   // id of the replayed run in the db when ok
};

// Replays `pr` into `db`, expanding the lazy graph `rg` as needed (`db` must
// have been built on the same `rg`). The whole run is validated first; if any
// step is unknown or not enabled, or an event run hits a deviation or deadlock
// before its last event, nothing is recorded.
//
// For event runs, `config` should be the TrackerConfig the run was recorded
// with. A different livelock timeout changes where a Livelock verdict would
// appear, but never the coverage: a run recorded as ending in a livelock
// replays fine with detection switched off.
ReplayResult replay(CoverageDb &db, ReachabilityGraph &rg, const PersistedRun &pr,
                    const TrackerConfig &config = {});

}  // namespace pn
