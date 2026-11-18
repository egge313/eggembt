#include "engine/ReachabilityGraph.h"

#include <algorithm>
#include <cassert>

namespace pn {

bool ReachabilityGraph::IdEq::operator()(StateId a, StateId b) const
{
    if (a == b)
        return true;
    const TokenCount *ta = g->tokens(a);
    const TokenCount *tb = g->tokens(b);
    return std::equal(ta, ta + g->numPlaces_, tb);
}

ReachabilityGraph::ReachabilityGraph(const Net &net, GenerationLimits limits)
    : net_(net),
      limits_(limits),
      numPlaces_(net.places.size()),
      index_(64, IdHash{this}, IdEq{this})
{
    // Ids are 32-bit and kNoState is reserved.
    limits_.maxStates = std::min<std::size_t>(limits_.maxStates, kNoState - 1);
    scratchFrom_.resize(numPlaces_);
    scratchTo_.resize(numPlaces_);
}

StateId ReachabilityGraph::intern(const TokenCount *tokens, bool allowNew, bool &created)
{
    created = false;

    // Append the candidate to the arena so the hash set can look it up by id,
    // then undo the append if it already exists (or may not be added).
    const StateId candidate = static_cast<StateId>(states_.size());
    arena_.insert(arena_.end(), tokens, tokens + numPlaces_);

    const auto existing = index_.find(candidate);
    if (existing != index_.end()) {
        arena_.resize(arena_.size() - numPlaces_);
        return *existing;
    }
    if (!allowNew) {
        arena_.resize(arena_.size() - numPlaces_);
        return kNoState;
    }

    index_.insert(candidate);
    StateInfo info;
    const Classification c = classify(net_, tokens);
    info.kind = c.kind;
    info.markingIndex = c.markingIndex;
    states_.push_back(info);
    created = true;
    return candidate;
}

StateId ReachabilityGraph::addInitial(const Marking &marking)
{
    assert(marking.size() == numPlaces_);
    bool created = false;
    const StateId id = intern(marking.data(), true, created);
    if (std::find(initial_.begin(), initial_.end(), id) == initial_.end())
        initial_.push_back(id);
    return id;
}

StateId ReachabilityGraph::addInitial(std::size_t initialMarkingIndex)
{
    assert(initialMarkingIndex < net_.initialMarkings.size());
    return addInitial(toDense(net_, net_.initialMarkings[initialMarkingIndex].tokens));
}

std::size_t ReachabilityGraph::expand(StateId s)
{
    assert(s < states_.size());
    if (states_[s].expanded)
        return 0;

    // Copy the source marking: interning may reallocate the arena.
    const TokenCount *src = tokens(s);
    scratchFrom_.assign(src, src + numPlaces_);

    const std::size_t edgeBegin = edges_.size();
    std::uint32_t enabled = 0;
    std::size_t created = 0;
    bool incomplete = false;

    for (std::size_t i = 0; i < net_.transitions.size(); ++i) {
        const Transition &t = net_.transitions[i];
        if (!isEnabled(t, scratchFrom_.data()))
            continue;
        ++enabled;

        const auto ti = static_cast<TransitionIndex>(i);
        const FireResult fr = fire(net_, t, scratchFrom_.data(), scratchTo_.data());
        if (fr.status != FireStatus::Ok) {
            boundViolations_.push_back({s, ti, fr.place, fr.status == FireStatus::Overflow});
            continue;
        }

        bool createdNow = false;
        const bool allowNew = states_.size() < limits_.maxStates;
        const StateId target = intern(scratchTo_.data(), allowNew, createdNow);
        if (target == kNoState) {
            truncated_ = true;
            incomplete = true;
            continue;
        }
        if (createdNow)
            ++created;
        edges_.push_back({ti, target});
    }

    StateInfo &info = states_[s];  // taken after the loop: states_ may have grown
    info.expanded = true;
    info.edgeBegin = edgeBegin;
    info.edgeCount = static_cast<std::uint32_t>(edges_.size() - edgeBegin);
    info.enabled = enabled;
    info.incomplete = incomplete;
    assert(edges_.size() < kNoEdge && "edge ids are 32-bit");
    expansionLog_.push_back(s);
    return created;
}

std::size_t ReachabilityGraph::exploreAll()
{
    // Ids are handed out in discovery order and new states are appended, so
    // walking ids in increasing order is a breadth-first traversal.
    std::size_t expanded = 0;
    for (StateId s = 0; s < states_.size(); ++s) {
        if (states_[s].expanded)
            continue;
        if (limits_.stopAtErrorStates && states_[s].kind == StateKind::Error)
            continue;
        expand(s);
        ++expanded;
    }
    return expanded;
}

EdgeRange ReachabilityGraph::successors(StateId s) const
{
    assert(states_[s].expanded);
    const StateInfo &info = states_[s];
    const Edge *base = edges_.data() + info.edgeBegin;
    return {base, base + info.edgeCount};
}

std::optional<EdgeId> ReachabilityGraph::findEdge(StateId s, TransitionIndex t) const
{
    assert(states_[s].expanded);
    const StateInfo &info = states_[s];
    for (std::uint32_t k = 0; k < info.edgeCount; ++k) {
        const auto e = static_cast<EdgeId>(info.edgeBegin + k);
        if (edges_[e].transition == t)
            return e;
    }
    return std::nullopt;
}

bool ReachabilityGraph::isDeadlock(StateId s) const
{
    const StateInfo &info = states_[s];
    return info.expanded && info.enabled == 0 && info.kind == StateKind::Normal;
}

std::vector<StateId> ReachabilityGraph::deadlocks() const
{
    std::vector<StateId> out;
    for (StateId s = 0; s < states_.size(); ++s)
        if (isDeadlock(s))
            out.push_back(s);
    return out;
}

std::vector<StateId> ReachabilityGraph::statesOfKind(StateKind k) const
{
    std::vector<StateId> out;
    for (StateId s = 0; s < states_.size(); ++s)
        if (states_[s].kind == k)
            out.push_back(s);
    return out;
}

}  // namespace pn
