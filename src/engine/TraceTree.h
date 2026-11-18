// Prefix tree of all run paths. Every run inserts its steps; identical
// prefixes share nodes. A coverage entry only remembers a tree node, and the
// full backtrace is recovered by walking parent pointers, so memory grows with
// the number of *distinct* path prefixes, not with the total number of steps.
#pragma once

#include "engine/ReachabilityGraph.h"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <unordered_map>
#include <vector>

namespace pn {

using NodeId = std::uint32_t;
inline constexpr NodeId kRootNode = 0;
inline constexpr NodeId kNoNode = 0xFFFFFFFFu;

struct TraceStep {
    StateId state;  // state reached by this step
    EdgeId via;     // edge taken; kNoEdge for the initial step of a run
};

class TraceTree {
public:
    struct Node {
        NodeId parent;
        EdgeId via;
        StateId state;
        std::uint32_t depth;   // steps from the root (the initial step has depth 1)
        std::uint32_t visits;  // how many runs passed through this node
    };

    TraceTree();

    // Finds or creates the child of `parent` reached via `via` into `state`,
    // and counts one more visit.
    NodeId child(NodeId parent, EdgeId via, StateId state);

    const Node &node(NodeId n) const { return nodes_[n]; }
    std::size_t size() const { return nodes_.size(); }  // includes the root

    // Steps from the start of the run to `n` (the root is not a step). If
    // `maxSteps` is smaller than the full depth, the last `maxSteps` steps are
    // returned, i.e. the ones closest to `n`.
    std::vector<TraceStep> path(NodeId n,
                                std::size_t maxSteps = std::numeric_limits<std::size_t>::max()) const;

private:
    struct Key {
        NodeId parent;
        EdgeId via;
        StateId state;
        bool operator==(const Key &o) const
        {
            return parent == o.parent && via == o.via && state == o.state;
        }
    };
    struct KeyHash {
        std::size_t operator()(const Key &k) const
        {
            std::uint64_t h = k.parent;
            h = (h * 0x9e3779b97f4a7c15ULL) ^ k.via;
            h = (h * 0x9e3779b97f4a7c15ULL) ^ k.state;
            h ^= h >> 32;
            return static_cast<std::size_t>(h);
        }
    };

    std::vector<Node> nodes_;
    std::unordered_map<Key, NodeId, KeyHash> index_;
};

}  // namespace pn
