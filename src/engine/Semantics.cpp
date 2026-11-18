#include "engine/Semantics.h"

#include <algorithm>
#include <cassert>
#include <limits>

namespace pn {

Marking toDense(const Net &net, const SparseMarking &sparse)
{
    Marking m(net.places.size(), 0);
    for (const TokenEntry &e : sparse)
        m[e.place] = e.tokens;
    return m;
}

bool isEnabled(const Transition &t, const TokenCount *marking)
{
    for (const WeightedArc &a : t.pre)
        if (marking[a.place] < a.weight)
            return false;
    for (const WeightedArc &a : t.inhibit)
        if (marking[a.place] >= a.weight)
            return false;
    return true;
}

FireResult fire(const Net &net, const Transition &t, const TokenCount *from, TokenCount *to)
{
    const std::size_t numPlaces = net.places.size();
    std::copy(from, from + numPlaces, to);

    for (const WeightedArc &a : t.pre) {
        assert(to[a.place] >= a.weight && "fire() called on a disabled transition");
        to[a.place] -= a.weight;
    }

    for (const WeightedArc &a : t.post) {
        TokenCount &slot = to[a.place];
        if (slot > std::numeric_limits<TokenCount>::max() - a.weight)
            return {FireStatus::Overflow, a.place};
        slot += a.weight;
        const std::optional<TokenCount> &bound = net.places[a.place].bound;
        if (bound && slot > *bound)
            return {FireStatus::BoundExceeded, a.place};
    }
    return {};
}

bool matches(const NamedMarking &spec, const TokenCount *marking, std::size_t numPlaces)
{
    // Both the dense marking and the sparse spec are sorted by place, so walk
    // them in lockstep: O(places).
    const bool exact = spec.match == MatchMode::Exact;
    std::size_t k = 0;
    for (std::size_t p = 0; p < numPlaces; ++p) {
        TokenCount expected = 0;
        if (k < spec.tokens.size() && spec.tokens[k].place == p)
            expected = spec.tokens[k++].tokens;
        if (exact ? marking[p] != expected : marking[p] < expected)
            return false;
    }
    return true;
}

Classification classify(const Net &net, const TokenCount *marking)
{
    const std::size_t numPlaces = net.places.size();
    for (std::size_t i = 0; i < net.errorMarkings.size(); ++i)
        if (matches(net.errorMarkings[i], marking, numPlaces))
            return {StateKind::Error, static_cast<int>(i)};
    for (std::size_t i = 0; i < net.targetMarkings.size(); ++i)
        if (matches(net.targetMarkings[i], marking, numPlaces))
            return {StateKind::Target, static_cast<int>(i)};
    return {};
}

std::vector<TransitionIndex> enabledTransitions(const Net &net, const TokenCount *marking)
{
    std::vector<TransitionIndex> out;
    for (std::size_t i = 0; i < net.transitions.size(); ++i)
        if (isEnabled(net.transitions[i], marking))
            out.push_back(static_cast<TransitionIndex>(i));
    return out;
}

std::size_t hashTokens(const TokenCount *tokens, std::size_t count)
{
    // FNV-style mixing over 32-bit words, finished with a 64-bit avalanche so
    // the low bits (used by the hash table) depend on every token.
    std::uint64_t h = 0xcbf29ce484222325ULL;
    for (std::size_t i = 0; i < count; ++i) {
        h ^= tokens[i];
        h *= 0x100000001b3ULL;
    }
    h ^= h >> 33;
    h *= 0xff51afd7ed558ccdULL;
    h ^= h >> 33;
    h *= 0xc4ceb9fe1a85ec53ULL;
    h ^= h >> 33;
    return static_cast<std::size_t>(h);
}

}  // namespace pn
