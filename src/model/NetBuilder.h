// Small helper for building a pn::Net in code (tests, importers, generators).
// It does not validate: the JSON loader is the validating entry point.
#pragma once

#include "model/Net.h"

#include <algorithm>
#include <cassert>
#include <initializer_list>
#include <optional>
#include <string>
#include <utility>

namespace pn {

// Per-error-marking settings (see TargetPolicy / DumpMode in Net.h).
struct ErrorOptions {
    TargetPolicy policy = TargetPolicy::UntilCovered;
    std::optional<TokenCount> targetHits;
    DumpMode crashDump = DumpMode::None;
    std::uint32_t maxDumps = 1;
};

class NetBuilder {
public:
    explicit NetBuilder(std::string name = {}) { net_.name = std::move(name); }

    // GLM-5.3-Flash: read access while building (importers validate against
    // what is already there, e.g. duplicate ids).
    const Net &net() const { return net_; }

    PlaceIndex place(const std::string &id, std::optional<TokenCount> bound = std::nullopt)
    {
        assert(!net_.placeIndex.count(id) && !net_.transitionIndex.count(id));
        const auto idx = static_cast<PlaceIndex>(net_.places.size());
        net_.places.push_back(Place{id, bound});
        net_.placeIndex.emplace(id, idx);
        return idx;
    }

    TransitionIndex transition(const std::string &id, TransitionKind kind = TransitionKind::Internal,
                               const std::string &label = {})
    {
        assert(!net_.placeIndex.count(id) && !net_.transitionIndex.count(id));
        const auto idx = static_cast<TransitionIndex>(net_.transitions.size());
        Transition t;
        t.id = id;
        t.label = label;
        t.kind = kind;
        net_.transitions.push_back(std::move(t));
        net_.transitionIndex.emplace(id, idx);
        tracedOverride_.push_back(std::nullopt);
        return idx;
    }

    // GLM-5.3-Flash: override whether a step of this transition is reported
    // (see Transition::traced). Without an explicit override, build() derives
    // it: observable steps are traced, internal ones only when they carry a
    // label. This is how a test or a hand-built model makes a named internal
    // step silent, the way the "traced" key does in the JSON.
    NetBuilder &traced(TransitionIndex t, bool on)
    {
        assert(t < tracedOverride_.size());
        tracedOverride_[t] = on;
        return *this;
    }

    // GLM-5.3-Flash: tag a transition as temporally unpredictable (human
    // operator in the loop). A run that steps it keeps the catastrophe
    // pheromones flat; see Session's time pricing.
    NetBuilder &unpredictable(TransitionIndex t, bool on = true)
    {
        net_.transitions.at(t).unpredictable = on;
        return *this;
    }

    NetBuilder &input(PlaceIndex p, TransitionIndex t, TokenCount weight = 1)
    {
        net_.transitions[t].pre.push_back({p, weight});
        return *this;
    }

    NetBuilder &output(TransitionIndex t, PlaceIndex p, TokenCount weight = 1)
    {
        net_.transitions[t].post.push_back({p, weight});
        return *this;
    }

    NetBuilder &inhibitor(PlaceIndex p, TransitionIndex t, TokenCount weight = 1)
    {
        net_.transitions[t].inhibit.push_back({p, weight});
        return *this;
    }

    NetBuilder &initial(const std::string &name, std::initializer_list<TokenEntry> tokens)
    {
        addMarking(net_.initialMarkings, name, tokens, MatchMode::Exact);
        return *this;
    }

    NetBuilder &targetMarking(const std::string &name, std::initializer_list<TokenEntry> tokens,
                             MatchMode match = MatchMode::Exact, bool intentional = false)
    {
        addMarking(net_.targetMarkings, name, tokens, match);
        // GLM-5.3-Flash: intentional targets are "good target states"; reaching
        // one certainly ends the campaign.
        net_.targetMarkings.back().intentional = intentional;
        return *this;
    }

    // GLM-5.3-Flash: tag a transition as an error, optionally with a
    // post-mortem request, mirroring the error-marking fields.
    NetBuilder &error(TransitionIndex t, DumpMode crashDump = DumpMode::None,
                      std::uint32_t maxDumps = 1)
    {
        net_.transitions.at(t).error = true;
        net_.transitions[t].crashDump = crashDump;
        net_.transitions[t].maxDumps = maxDumps;
        return *this;
    }

    // GLM-5.3-Flash: tag a transition as a milestone, with an optional rank
    // (>= 1; a higher rank deposits proportionally more attract pheromone).
    // The transition must be visible to the tester (see Transition::milestone);
    // build() does not check that -- the loaders do, mirroring "error".
    NetBuilder &milestone(TransitionIndex t, std::uint32_t rank = 1)
    {
        assert(rank >= 1);
        net_.transitions.at(t).milestone = true;
        net_.transitions[t].milestoneRank = rank;
        return *this;
    }

    // GLM-5.3-Flash: predict a nondeterministic SUT crash at a transition
    // (engine/CrashCredibility.h): the crash is a target until the
    // reproduction experiment is certain it is deterministic. The transition
    // must be visible and must not be an error transition -- build() does not
    // check that; the loaders do, mirroring "milestone".
    NetBuilder &crash(TransitionIndex t, CrashPrediction prediction)
    {
        net_.transitions.at(t).crashPrediction = std::move(prediction);
        return *this;
    }

    // GLM-5.3-Flash: attach a shell command hook to a transition (the
    // "command" key of the JSON). The transition must be visible to the
    // tester -- build() does not check that; the loaders do, mirroring
    // "error" and "milestone".
    NetBuilder &command(TransitionIndex t, CommandHook hook)
    {
        net_.transitions.at(t).command = std::move(hook);
        return *this;
    }

    // GLM-5.3-Flash: predict a nondeterministic SUT crash while the SUT is
    // certainly in a named marking of the target or error list (the two lists
    // the tracker can be "certainly in"; the loaders reject it on initial
    // markings and on policy "avoid").
    NetBuilder &crash(const std::string &markingName, CrashPrediction prediction)
    {
        for (std::vector<NamedMarking> *list : {&net_.errorMarkings, &net_.targetMarkings})
            for (NamedMarking &m : *list)
                if (m.name == markingName) {
                    m.crash = std::move(prediction);
                    return *this;
                }
        assert(false && "no target or error marking of this name");
        return *this;
    }

    NetBuilder &errorMarking(const std::string &name, std::initializer_list<TokenEntry> tokens,
                             MatchMode match = MatchMode::Exact, ErrorOptions opts = {})
    {
        addMarking(net_.errorMarkings, name, tokens, match);
        NamedMarking &m = net_.errorMarkings.back();
        m.policy = opts.policy;
        m.targetHits = opts.targetHits;
        m.crashDump = opts.crashDump;
        m.maxDumps = opts.maxDumps;
        return *this;
    }

    // Sorts arc lists (the engine and loader rely on that) and returns the net.
    Net build()
    {
        const auto byPlace = [](const WeightedArc &a, const WeightedArc &b) {
            return a.place < b.place;
        };
        for (std::size_t i = 0; i < net_.transitions.size(); ++i) {
            Transition &t = net_.transitions[i];
            std::sort(t.pre.begin(), t.pre.end(), byPlace);
            std::sort(t.post.begin(), t.post.end(), byPlace);
            std::sort(t.inhibit.begin(), t.inhibit.end(), byPlace);
            // GLM-5.3-Flash: the tracing default, unless overridden explicitly.
            t.traced = tracedOverride_[i].value_or(t.kind != TransitionKind::Internal || !t.label.empty());
        }
        return std::move(net_);
    }

private:
    static void addMarking(std::vector<NamedMarking> &out, const std::string &name,
                           std::initializer_list<TokenEntry> tokens, MatchMode match)
    {
        NamedMarking m;
        m.name = name;
        m.match = match;
        for (const TokenEntry &e : tokens)
            if (e.tokens > 0)
                m.tokens.push_back(e);
        std::sort(m.tokens.begin(), m.tokens.end(),
                  [](const TokenEntry &a, const TokenEntry &b) { return a.place < b.place; });
        out.push_back(std::move(m));
    }

    Net net_;
    std::vector<std::optional<bool>> tracedOverride_;  // GLM-5.3-Flash: parallel to transitions
};

}  // namespace pn
