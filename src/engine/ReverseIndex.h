// Predecessor index for a lazily growing ReachabilityGraph.
//
// The graph only stores forward edges. This index adds the reverse view that
// the ant colony needs to back-propagate pheromone from an event to the RG
// nodes that led to it. It is updated incrementally: sync() consumes only the
// states expanded since the previous call, so call it after expanding.
//
// Storage is one linked list per state, threaded through a flat array: 12
// bytes per edge plus 4 bytes per state, and appends are O(1).
//
// Not thread-safe (backwardWalk uses a mutable scratch stamp array).
#pragma once

#include "engine/ReachabilityGraph.h"

#include <algorithm>
#include <cstdint>
#include <utility>
#include <vector>

namespace pn {

class ReverseIndex {
public:
    explicit ReverseIndex(const ReachabilityGraph &g) : g_(g) {}

    // Incorporates every state expanded since the last call.
    void sync();

    struct Pred {
        StateId from;
        EdgeId edge;
    };

    // Calls f(Pred) for every known edge entering `s` (newest first).
    template <class F>
    void forEachPredecessor(StateId s, F &&f) const
    {
        if (s >= head_.size())
            return;
        for (std::uint32_t i = head_[s]; i != kEnd; i = rev_[i].next)
            f(Pred{rev_[i].from, rev_[i].edge});
    }

    std::vector<Pred> predecessors(StateId s) const
    {
        std::vector<Pred> out;
        forEachPredecessor(s, [&out](const Pred &p) { out.push_back(p); });
        return out;
    }

    std::size_t numReverseEdges() const { return rev_.size(); }

    // Breadth-first walk backwards from `seed` across at most `maxPrev`
    // previous nodes. For every reverse edge e = (from -> to) it calls
    //     visit(EdgeId e, StateId from, StateId to, unsigned depth)
    // exactly once, where depth is the distance of `to` from the seed: depth 0
    // are the edges entering the seed, depth 1 the edges entering those
    // predecessors, and so on. With maxPrev = 5 the depths run 0..4, i.e. the
    // `from` nodes lie 1..5 steps behind the seed. Nodes are expanded once, at
    // their shortest distance, so a diamond is not double-counted.
    template <class F>
    void backwardWalk(StateId seed, unsigned maxPrev, F &&visit) const
    {
        if (maxPrev == 0 || seed >= g_.numStates())
            return;
        if (stamp_.size() < g_.numStates())
            stamp_.resize(g_.numStates(), 0);
        if (++generation_ == 0) {  // wrapped: clear stale stamps
            std::fill(stamp_.begin(), stamp_.end(), 0u);
            generation_ = 1;
        }

        std::vector<std::pair<StateId, unsigned>> queue;
        queue.emplace_back(seed, 0u);
        stamp_[seed] = generation_;
        for (std::size_t i = 0; i < queue.size(); ++i) {
            const StateId s = queue[i].first;
            const unsigned d = queue[i].second;
            if (d >= maxPrev)
                continue;
            forEachPredecessor(s, [&](const Pred &p) {
                visit(p.edge, p.from, s, d);
                if (stamp_[p.from] != generation_) {
                    stamp_[p.from] = generation_;
                    queue.emplace_back(p.from, d + 1);
                }
            });
        }
    }

private:
    static constexpr std::uint32_t kEnd = 0xFFFFFFFFu;

    struct Entry {
        StateId from;
        EdgeId edge;
        std::uint32_t next;
    };

    const ReachabilityGraph &g_;
    std::vector<std::uint32_t> head_;  // per target state: first Entry, or kEnd
    std::vector<Entry> rev_;
    std::size_t syncedExpansions_ = 0;

    mutable std::vector<std::uint32_t> stamp_;
    mutable std::uint32_t generation_ = 0;
};

}  // namespace pn
