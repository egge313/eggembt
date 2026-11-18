// Strongly connected components of a ReachabilityGraph (iterative Tarjan, so
// deep graphs cannot overflow the call stack).
//
// Only expanded states have known outgoing edges; an unexpanded state is a
// component of its own. Edges into unexpanded states are kept, so analyses
// that ask "can the component be left?" stay conservative on partial graphs.
#pragma once

#include "engine/ReachabilityGraph.h"

#include <cstdint>
#include <functional>
#include <vector>

namespace pn {

struct Sccs {
    // componentOf[s] indexes `members`. Components come out in reverse
    // topological order: a component only has edges into components with a
    // smaller or equal index, so sinks come first.
    std::vector<std::uint32_t> componentOf;
    std::vector<std::vector<StateId>> members;  // each sorted ascending
};

// `keep`, if set, selects the edges that count (e.g. only internal transitions).
Sccs computeScc(const ReachabilityGraph &g, const std::function<bool(const Edge &)> &keep = {});

}  // namespace pn
