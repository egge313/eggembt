// Static livelock analysis of the reachability graph generated from the
// specification (livelock kind 1). Run-time livelocks, which need a timeout,
// are detected by the BeliefTracker (kind 2).
//
// A TERMINAL CYCLE is a strongly connected component that
//   * contains a cycle (more than one state, or a self-loop),
//   * cannot be left (no edge, controllable or not, goes out of it), and
//   * contains no target marking,
// i.e. the system can run forever in it but can never finish. A component
// with a target marking is a legitimate steady state; a single state without
// any edge is a deadlock, not a livelock (see ReachabilityGraph::deadlocks).
//
// Optionally, an INTERNAL CYCLE is a cycle made only of internal transitions
// (divergence: the system can spin without any visible progress). It is off by
// default because environment-like internal behaviour that toggles back and
// forth (an obstruction that appears and clears) is legitimate in many models.
//
// The analysis works on whatever has been explored so far. A terminal cycle is
// only CERTAIN if nothing could have hidden an exit: no state of the component
// is incomplete (state limit) and none has a firing dropped for exceeding a
// place bound. Edges into unexpanded states count as exits, so a component
// next to unexplored territory is never reported as terminal.
#pragma once

#include "engine/ReachabilityGraph.h"

#include <cstddef>
#include <vector>

namespace pn {

enum class LivelockKind { TerminalCycle, InternalCycle };

struct LivelockFinding {
    LivelockKind kind;
    std::vector<StateId> states;  // the component, sorted
    bool certain;                 // false: exploration may have hidden an exit
};

struct LivelockOptions {
    bool terminalCycles = true;
    bool internalCycles = false;
};

struct LivelockReport {
    std::vector<LivelockFinding> findings;  // ordered by smallest state id
    std::size_t components = 0;             // SCCs of the explored graph
    std::size_t cyclicComponents = 0;       // of those, with a cycle
    // Every state expanded, nothing cut by the state limit, no dropped firings:
    // the analysis saw the whole graph.
    bool graphComplete = false;
};

LivelockReport analyzeLivelocks(const ReachabilityGraph &g, const LivelockOptions &options = {});

}  // namespace pn
