// What the command-line tool prints and writes about a model and its coverage.
#pragma once

#include "cli/Console.h"
#include "engine/Json.h"
#include "engine/OverlapAnalysis.h"
#include "engine/Session.h"

#include <string>
#include <vector>

namespace cli {

// "closed:1, open:2" (places without tokens are left out; "(empty)" if none).
std::string formatMarking(const pn::Net &net, const pn::Marking &marking);

// Coverage figures, what is still uncovered, the error markings with their
// hit/dump budgets, and the findings of all recorded runs.
void printCoverage(const Console &con, const pn::Net &net, const pn::CoverageDb &db);
void printRunsAndFindings(const Console &con, const pn::CoverageDb &db);

// GLM-5.3-Flash: the crash predictions of the model and their reproduction
// experiments (engine/CrashCredibility.h) -- one row per entry: the crash
// site, its prediction, where its experiment stands, and its post-mortem
// budget. Prints nothing when there are no entries at all.
void printCrashPredictions(const Console &con, const pn::CoverageDb &db);
// GLM-5.3-Flash: the command hooks the runs executed (doku section 18); no
// block when no run executed one.
void printHookRuns(const Console &con, const pn::CoverageDb &db);

// GLM-5.3-Flash: for every definitely covered transition, the chain of moves
// that led into its first definite visit (doku section 12, item 2 -- the
// GUI half of the item, closed as a textual report).
void printBacktraces(const Console &con, const pn::Net &net, const pn::ReachabilityGraph &rg,
                     const pn::CoverageDb &db);

// GLM-5.3-Flash: wall-clock time of a campaign (total, mean and longest run,
// SUT events per second) -- the second goodness criterion next to coverage,
// and the one that tells seeds apart.
void printTime(const Console &con, const pn::CampaignReport &report);

// GLM-5.3-Flash: how much the runs of a campaign overlap, as mean pairwise
// Jaccard indices (one line) -- "overlap 40%" means two runs share less than
// half of what they cover, i.e. the steering explored more than it repeated.
void printOverlap(const Console &con, const pn::OverlapMatrix &m);

// DeepSeek V4.1 Flash: the recorded trace of a run (doku section 21): the initial
// marking, then every step in firing order. `onlyRun` is 1-based, 0 = every
// run; `full` prints the whole trace, otherwise a long one keeps its last 20
// steps behind a "...". Uses the run log's own `steps` (transition ids) or,
// for event runs, the recorded events.
void printTraces(const Console &con, const pn::Net &net, const std::vector<pn::PersistedRun> &runs,
                 int onlyRun, bool full);

// Machine-readable counterpart for CI.
// `elements` adds every known state and edge with its hit counts (the
// --coverage-elements switch); the default keeps the report compact.
pn::JsonValue coverageJson(const pn::Net &net, const pn::CoverageDb &db, bool elements = false);
pn::JsonValue campaignJson(const pn::Net &net, const pn::Session &session, const pn::CampaignReport &report,
                           bool coverageElements = false);

// DeepSeek V4.1 Flash: the colony's pheromone, keyed readably (doku section 21):
// per edge the source and target markings, the transition, attract, repel and
// the static region; per (marking, T-suffix) pair its list and both channels.
// Edge ids are session-local, so the JSON never mentions them.
pn::JsonValue pheromoneJson(const pn::Net &net, const pn::Session &session);

// GLM-5.3-Flash: the overlap section of a campaign report (pairwise Jaccard
// matrices between the runs, and their means).
pn::JsonValue overlapJson(const pn::OverlapMatrix &m);

// True for the outcomes that mean the SUT (or the protocol) misbehaved.
bool isFailure(pn::RunOutcome outcome);

}  // namespace cli
