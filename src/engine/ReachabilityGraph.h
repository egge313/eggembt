// Lazily expanded reachability graph (RG) of a pn::Net.
//
// States are interned markings with dense ids assigned in discovery order.
// Nothing is expanded until you ask: expand(s) computes all successors of one
// state, exploreAll() does a breadth-first expansion of everything reachable.
//
// Storage is compact: all markings live in one contiguous arena, and because a
// state is expanded at most once, its outgoing edges are one contiguous run in
// a single edge array.
//
// Two safety nets keep exploration finite and honest:
//  * bound violations (a firing that would exceed a place's `bound`, or
//    overflow 32 bits) are not turned into edges; they are recorded;
//  * a state limit stops the creation of new states. Dropped edges mark the
//    source state "incomplete" and the graph "truncated", so analyses never
//    mistake a cut-off frontier for a deadlock.
#pragma once

#include "engine/Semantics.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <unordered_set>
#include <vector>

namespace pn {

using StateId = std::uint32_t;
inline constexpr StateId kNoState = 0xFFFFFFFFu;

// Global edge id: the index into the graph's append-only edge array. Stable
// for the life of the graph, so coverage and the reverse index can key on it.
using EdgeId = std::uint32_t;
inline constexpr EdgeId kNoEdge = 0xFFFFFFFFu;

struct Edge {
    TransitionIndex transition;
    StateId target;
};

struct EdgeRange {
    const Edge *first = nullptr;
    const Edge *last = nullptr;
    const Edge *begin() const { return first; }
    const Edge *end() const { return last; }
    std::size_t size() const { return static_cast<std::size_t>(last - first); }
    bool empty() const { return first == last; }
};

struct GenerationLimits {
    std::size_t maxStates = 1000000;
    // If true, exploreAll() does not expand states that match an error marking.
    bool stopAtErrorStates = false;
};

struct BoundViolation {
    StateId state;
    TransitionIndex transition;
    PlaceIndex place;
    bool overflow;  // true: 32-bit overflow on an unbounded place; false: declared bound exceeded
};

class ReachabilityGraph {
public:
    explicit ReachabilityGraph(const Net &net, GenerationLimits limits = {});

    // The hash table holds a pointer back to this object.
    ReachabilityGraph(const ReachabilityGraph &) = delete;
    ReachabilityGraph &operator=(const ReachabilityGraph &) = delete;

    const Net &net() const { return net_; }

    // --- building -------------------------------------------------------

    // Adds an initial state (idempotent) and returns its id. Initial states
    // are always added, even beyond the state limit.
    StateId addInitial(const Marking &marking);
    StateId addInitial(std::size_t initialMarkingIndex);  // net.initialMarkings[i]

    // Expands one state. Returns the number of NEW states it created; 0 if it
    // was already expanded.
    std::size_t expand(StateId s);

    // Breadth-first expansion of all unexpanded states (honoring the limits).
    // Returns the number of states expanded by this call.
    std::size_t exploreAll();

    // --- queries --------------------------------------------------------

    std::size_t numStates() const { return states_.size(); }
    std::size_t numEdges() const { return edges_.size(); }
    const std::vector<StateId> &initialStates() const { return initial_; }

    const TokenCount *tokens(StateId s) const { return arena_.data() + std::size_t(s) * numPlaces_; }
    Marking marking(StateId s) const { return Marking(tokens(s), tokens(s) + numPlaces_); }
    StateKind kind(StateId s) const { return states_[s].kind; }

    bool isExpanded(StateId s) const { return states_[s].expanded; }

    // The following are meaningful only for expanded states.
    std::size_t enabledCount(StateId s) const { return states_[s].enabled; }
    // True if a successor was dropped because of the state limit.
    bool isIncomplete(StateId s) const { return states_[s].incomplete; }
    EdgeRange successors(StateId s) const;

    const Edge &edge(EdgeId e) const { return edges_[e]; }
    EdgeId firstEdge(StateId s) const { return static_cast<EdgeId>(states_[s].edgeBegin); }  // expanded only
    // The outgoing edge of expanded state `s` that fires `t`, if any.
    std::optional<EdgeId> findEdge(StateId s, TransitionIndex t) const;

    // Index into net.errorMarkings / net.targetMarkings of the marking the
    // state matches (see kind()); -1 for Normal states.
    int markingIndex(StateId s) const { return states_[s].markingIndex; }

    // Log of states in the order they were expanded; lets incremental
    // consumers (ReverseIndex) catch up without rescanning the graph.
    std::size_t numExpanded() const { return expansionLog_.size(); }
    StateId expandedState(std::size_t i) const { return expansionLog_[i]; }

    // Expanded, nothing enabled, and not a target or error marking.
    bool isDeadlock(StateId s) const;
    std::vector<StateId> deadlocks() const;
    std::vector<StateId> statesOfKind(StateKind kind) const;

    bool truncated() const { return truncated_; }
    const std::vector<BoundViolation> &boundViolations() const { return boundViolations_; }

private:
    struct StateInfo {
        std::size_t edgeBegin = 0;
        std::uint32_t edgeCount = 0;
        std::uint32_t enabled = 0;
        std::int32_t markingIndex = -1;
        StateKind kind = StateKind::Normal;
        bool expanded = false;
        bool incomplete = false;
    };

    struct IdHash {
        const ReachabilityGraph *g;
        std::size_t operator()(StateId id) const { return hashTokens(g->tokens(id), g->numPlaces_); }
    };
    struct IdEq {
        const ReachabilityGraph *g;
        bool operator()(StateId a, StateId b) const;
    };

    // Returns the id of the marking, creating the state if it is new and
    // allowNew is set (otherwise kNoState). `tokens` must not point into this
    // graph's own storage.
    StateId intern(const TokenCount *tokens, bool allowNew, bool &created);

    const Net &net_;
    GenerationLimits limits_;
    std::size_t numPlaces_;

    std::vector<TokenCount> arena_;  // numStates * numPlaces tokens
    std::vector<StateInfo> states_;
    std::vector<Edge> edges_;
    std::vector<StateId> initial_;
    std::vector<StateId> expansionLog_;
    std::unordered_set<StateId, IdHash, IdEq> index_;

    std::vector<TokenCount> scratchFrom_, scratchTo_;
    std::vector<BoundViolation> boundViolations_;
    bool truncated_ = false;
};

}  // namespace pn
