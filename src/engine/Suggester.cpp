#include "engine/Suggester.h"

#include "engine/AntColony.h"

#include <algorithm>
#include <functional>
#include <limits>
#include <queue>
#include <random>
#include <unordered_set>
#include <utility>

namespace pn {

namespace {

constexpr std::uint32_t kInf = std::numeric_limits<std::uint32_t>::max();

std::string goalText(GoalTier t)
{
    switch (t) {
    case GoalTier::Rare:
        return "a rare transition that has not been seen yet";
    case GoalTier::Target:
        return "a target (error marking, deadlock or livelock) that is still being sought";
    case GoalTier::NewTransition:
        return "a transition that has not been covered yet";
    case GoalTier::Edge:
        return "an RG edge that has not been covered yet";
    case GoalTier::Frontier:
        return "unexplored states";
    case GoalTier::None:
        break;
    }
    return "nothing";
}

}  // namespace

Suggester::Suggester(ReachabilityGraph &rg, const CoverageDb &db, SuggesterConfig config)
    : rg_(rg), db_(db), cfg_(config), board_(rg, db, config.targets), reverse_(rg)
{
}

void Suggester::setLivelockFindings(const std::vector<LivelockFinding> &findings)
{
    board_.setLivelockFindings(findings);
}

std::uint32_t Suggester::penalty(GoalTier tier) const
{
    switch (tier) {
    case GoalTier::Rare:          return cfg_.penaltyRare;
    case GoalTier::Target:        return cfg_.penaltyTarget;
    case GoalTier::NewTransition: return cfg_.penaltyNewTransition;
    case GoalTier::Edge:          return cfg_.penaltyEdge;
    case GoalTier::Frontier:      return cfg_.penaltyFrontier;
    case GoalTier::None:          break;
    }
    return 0;
}

// DeepSeek V4.1 Flash: the hunt's goal (doku section 30). "Seen" is the
// ordinary definite transition coverage; a still-possible but ambiguous step
// does not count. The attempt budget belongs to the campaign, not the run.
bool Suggester::rareGoal(TransitionIndex t) const
{
    if (t >= rg_.net().transitions.size())
        return false;
    const Transition &tr = rg_.net().transitions[t];
    return tr.rare && db_.transitionHits(t) == 0 && db_.rareAttempts(t) < tr.rare->attempts;
}

Suggestion Suggester::suggest(const BeliefTracker &tracker)
{
    Suggestion out;
    if (!tracker.runActive() || tracker.belief().empty()) {
        out.status = SuggestionStatus::Stuck;
        out.reason = "no run in progress";
        return out;
    }

    // DeepSeek V4.1 Flash: while the SUT is paused or down (doku section 26), only
    // the lifecycle inputs that bring it back are offered; an ordinary
    // goal-directed input would reach a SUT that cannot answer. The gate is
    // not a coverage state: no goal search is involved, the way back is all
    // that matters.
    if (tracker.lifecycleGate() != LifecycleGate::Running) {
        Suggestion s;
        const TransitionIndex t = tracker.suggestedLifecycle();
        if (t == kNoTransition) {
            s.status = SuggestionStatus::Stuck;
            s.reason = tracker.lifecycleGate() == LifecycleGate::Down
                           ? "the SUT is down and the model offers no start or restart input to bring it back"
                           : "the SUT is paused and the model offers no resume, stop or restart input";
            return s;
        }
        s.status = SuggestionStatus::Suggest;
        s.transition = t;
        s.label = rg_.net().transitions[t].label;
        s.reason = tracker.lifecycleGate() == LifecycleGate::Down
                       ? "the SUT is down: the model's own start/restart input brings it back"
                       : "the SUT is paused: the model's own lifecycle input brings it back";
        return s;
    }

    const Net &net = rg_.net();
    const std::vector<StateId> belief = tracker.belief();

    // --- candidates and the edges they would take --------------------------------------
    struct Cand {
        TransitionIndex transition;
        std::size_t enabledIn;
        std::vector<std::pair<StateId, EdgeId>> edges;  // (belief state, edge of the input)
    };
    std::vector<Cand> cands;
    for (const ControllableCandidate &c : tracker.enabledControllable()) {
        Cand cand{c.transition, c.enabledIn, {}};
        for (StateId s : belief) {
            if (!isEnabled(net.transitions[c.transition], rg_.tokens(s)))
                continue;
            rg_.expand(s);
            if (const auto e = rg_.findEdge(s, c.transition))
                cand.edges.push_back({s, *e});
        }
        cands.push_back(std::move(cand));
    }

    // --- expand ahead, so that goals beyond the known graph can be found -----------------
    {
        std::vector<StateId> queue;
        std::unordered_set<StateId> seen;
        for (const Cand &c : cands) {
            for (const auto &se : c.edges) {
                const StateId v = rg_.edge(se.second).target;
                if (seen.insert(v).second)
                    queue.push_back(v);
            }
        }
        std::size_t expanded = 0;
        for (std::size_t i = 0; i < queue.size(); ++i) {
            const StateId u = queue[i];
            if (!rg_.isExpanded(u)) {
                if (expanded >= cfg_.expansionBudget)
                    continue;
                rg_.expand(u);
                ++expanded;
            }
            for (const Edge &e : rg_.successors(u))
                if (seen.insert(e.target).second)
                    queue.push_back(e.target);
        }
    }

    reverse_.sync();
    board_.refresh();

    // DeepSeek V4.1 Flash: the phase policy (--satisfied-target-policy).
    // Veto keeps the historical hard refusal; Penalise lets plans enter a
    // satisfied target but keeps its hazard; Ignore does not even count it.
    const bool vetoSatisfied = cfg_.satisfiedTargets == SatisfiedTargetPolicy::Veto;
    const bool hazardSatisfied = cfg_.satisfiedTargets != SatisfiedTargetPolicy::Ignore;

    // DeepSeek V4.1 Flash: a rare transition that is enabled right here is the
    // strongest goal there is (doku section 30). The SUT's own move cannot be
    // sent, so dwell at the enabling state for its wait-for and give it a real
    // chance to fire; any real event ends the dwell (the session's rule). With
    // wait-for 0 the campaign still stays put and listens instead of moving
    // away from the rare chance. A controllable rare input is handled by the
    // ordinary candidate path below: it is simply sent after the dwell.
    if (cfg_.huntRare) {
        for (TransitionIndex t = 0; t < net.transitions.size(); ++t) {
            const Transition &tr = net.transitions[t];
            if (!rareGoal(t) || tr.kind == TransitionKind::Controllable)
                continue;
            bool enabledSomewhere = false;
            for (StateId s : belief) {
                if (isEnabled(tr, rg_.tokens(s))
                    && (!vetoSatisfied || !board_.forbidden(s))) {
                    enabledSomewhere = true;
                    break;
                }
            }
            if (!enabledSomewhere)
                continue;
            out.transition = t;
            out.label = tr.label;
            out.goal = GoalTier::Rare;
            const RareSpec &rs = *tr.rare;
            if (rs.waitForMs > 0) {
                out.status = SuggestionStatus::Dwell;
                out.dwell = std::chrono::milliseconds(rs.waitForMs);
                out.reason = "dwelling at the enabling state of rare transition '" + tr.id + "' for "
                             + std::to_string(rs.waitForMs) + " ms";
            } else {
                out.status = SuggestionStatus::Wait;
                out.reason = "rare transition '" + tr.id + "' is enabled: waiting for the SUT to fire it";
            }
            return out;
        }
    }

    // --- distance to the nearest goal: one backward Dijkstra over the explored graph ----------
    const std::size_t n = rg_.numStates();
    std::vector<std::uint32_t> cost(n, kInf);
    std::vector<GoalTier> tier(n, GoalTier::None);
    using Item = std::pair<std::uint32_t, StateId>;
    std::priority_queue<Item, std::vector<Item>, std::greater<Item>> heap;
    std::size_t goalSeeds = 0;

    // DeepSeek V4.1 Flash: a rare goal wins an equal-cost tie against any
    // other tier (the historical tiers keep their mutual order).
    const auto tierRank = [](GoalTier t) { return t == GoalTier::Rare ? 0 : 1; };
    const auto seed = [&](StateId u, std::uint32_t c, GoalTier t) {
        ++goalSeeds;
        if (c < cost[u] || (c == cost[u] && tierRank(t) < tierRank(tier[u]))) {
            cost[u] = c;
            tier[u] = t;
            heap.push({c, u});
        }
    };
    const auto edgeTier = [&](const Edge &ed) {
        if (cfg_.huntRare && rareGoal(ed.transition))
            return GoalTier::Rare;
        return db_.transitionHits(ed.transition) == 0 ? GoalTier::NewTransition : GoalTier::Edge;
    };

    for (StateId u = 0; u < n; ++u) {
        // DeepSeek V4.1 Flash: with the veto off, a satisfied target is
        // traversable. It is still not a goal of its own, but its frontier
        // and its uncovered outgoing edges are goals like any other state's.
        if (board_.forbidden(u) && vetoSatisfied)
            continue;
        if (!rg_.isExpanded(u)) {
            seed(u, cfg_.penaltyFrontier, GoalTier::Frontier);
            continue;
        }
        if (board_.classOf(u) == TargetBoard::StateClass::Seeking)
            seed(u, cfg_.penaltyTarget, GoalTier::Target);
        // DeepSeek V4.1 Flash: an enabling state of an unseen rare transition
        // is itself the goal -- being there is what gives the transition its
        // chance, not taking its edge (which may be the SUT's own move).
        bool rareSource = false;
        EdgeId e = rg_.firstEdge(u);
        for (const Edge &ed : rg_.successors(u)) {
            if (cfg_.huntRare && rareGoal(ed.transition))
                rareSource = true;
            if ((!board_.forbidden(ed.target) || !vetoSatisfied) && db_.edge(e).definite == 0) {
                const GoalTier t = edgeTier(ed);
                seed(u, 1 + penalty(t), t);  // the edge itself is the last step
            }
            ++e;
        }
        if (rareSource)
            seed(u, cfg_.penaltyRare, GoalTier::Rare);
    }

    while (!heap.empty()) {
        const Item top = heap.top();
        heap.pop();
        const std::uint32_t c = top.first;
        const StateId v = top.second;
        if (c > cost[v])
            continue;
        reverse_.forEachPredecessor(v, [&](const ReverseIndex::Pred &p) {
            // DeepSeek V4.1 Flash: only the veto keeps plans out of satisfied
            // targets; Penalise and Ignore let the distance search pass.
            if (vetoSatisfied && board_.forbidden(p.from))
                return;  // plans never pass through a forbidden state
            // DeepSeek V4.1 Flash: a rare goal also wins an equal-cost tie
            // here. Without this, a fresh uncovered edge out of a state on the
            // way to a rare source would keep its own tier at the same cost,
            // and the rare priority would be lost on the last step.
            if (c + 1 < cost[p.from]
                || (c + 1 == cost[p.from] && tierRank(tier[v]) < tierRank(tier[p.from]))) {
                cost[p.from] = c + 1;
                tier[p.from] = tier[v];
                heap.push({c + 1, p.from});
            }
        });
    }

    // --- evaluate the candidates ------------------------------------------------------------
    struct Eval {
        const Cand *cand;
        double robustness;
        double hazard;
        std::uint32_t score;  // steps + goal penalty
        GoalTier goal;
        double effective;
    };
    std::vector<Eval> reachable;
    std::vector<std::string> vetoed;

    for (const Cand &c : cands) {
        if (c.edges.empty())
            continue;  // every enabling edge was dropped (bound or state limit): cannot judge it
        const Transition &tr = net.transitions[c.transition];

        std::size_t forbiddenTargets = 0;
        double stableSum = 0.0;
        double attractSum = 0.0;
        double repelSum = 0.0;
        double regionSum = 0.0;   // GLM-5.3-Flash: the static nontarget region
        double pairAttractSum = 0.0;  // GLM-5.3-Flash: the (marking, T-suffix) pairs
        double pairRepelSum = 0.0;
        std::uint32_t best = kInf;
        GoalTier bestTier = GoalTier::None;

        for (const auto &se : c.edges) {
            const StateId s = se.first;
            const EdgeId e = se.second;
            const Edge &ed = rg_.edge(e);

            // Does the input survive the SUT's own moves (outputs, internal steps)?
            // GLM-5.3-Flash: weighted by the offered/taken statistics (doku
            // section 12, item 3). Every environment move counts with its take
            // rate instead of 1: a move the SUT keeps refusing contributes
            // little to the estimate either way, so the survival figure is
            // dominated by the moves the SUT really makes. Moves that were
            // never offered (so far) rate 1.0 and weigh as much as before --
            // in particular silent internal steps, which are never offered.
            double envTotal = 0.0;
            double envKeep = 0.0;
            for (const Edge &x : rg_.successors(s)) {
                if (net.transitions[x.transition].kind == TransitionKind::Controllable)
                    continue;
                const double w = db_.takeRate(net.transitions[x.transition].label);
                envTotal += w;
                if (isEnabled(tr, rg_.tokens(x.target)))
                    envKeep += w;
            }
            stableSum += envTotal <= 0.0 ? 1.0 : envKeep / envTotal;
            if (ants_) {
                attractSum += ants_->attract(e);
                repelSum += ants_->repel(e);
                regionSum += ants_->region(e);  // GLM-5.3-Flash: doku section 12 item 5
                // GLM-5.3-Flash: the (marking, T-suffix) pair pheromone of the
                // arrival context this edge would create (doku section 16):
                // the run's trace so far, extended by the edge's own step.
                if (trace_ && ants_->tsuffixLength() > 0) {
                    const TraceCascade arrival = trace_->extended(stepHashOf(net.transitions[ed.transition]));
                    pairAttractSum += ants_->pairAttract(ed.target, arrival);
                    pairRepelSum += ants_->pairRepel(ed.target, arrival);
                }
            }

            // DeepSeek V4.1 Flash: the phase policy. Veto refuses the move;
            // Penalise counts the entry as hazard but lets the goal search
            // continue through the state; Ignore treats it as ordinary.
            if (board_.forbidden(ed.target)) {
                if (hazardSatisfied)
                    ++forbiddenTargets;
                if (vetoSatisfied)
                    continue;
            }
            if (db_.edge(e).definite == 0) {
                const GoalTier t = edgeTier(ed);
                const std::uint32_t sc = 1 + penalty(t);
                if (sc < best || (sc == best && tierRank(t) < tierRank(bestTier))) {
                    best = sc;
                    bestTier = t;
                }
            }
            if (cost[ed.target] != kInf) {
                const std::uint32_t sc = 1 + cost[ed.target];
                const GoalTier t = tier[ed.target];
                // DeepSeek V4.1 Flash: an equal-cost goal reached through the
                // target keeps its tier -- and a rare one wins that tie.
                if (sc < best || (sc == best && tierRank(t) < tierRank(bestTier))) {
                    best = sc;
                    bestTier = t;
                }
            }
        }

        const double edgeCount = static_cast<double>(c.edges.size());
        const double hazard = static_cast<double>(forbiddenTargets) / edgeCount;
        if (vetoSatisfied && forbiddenTargets == c.edges.size()) {
            vetoed.push_back(tr.label);
            continue;
        }
        const double robustness =
            (static_cast<double>(c.enabledIn) / static_cast<double>(belief.size())) * (stableSum / edgeCount);

        if (best == kInf)
            continue;  // no goal reachable through this input
        double effective = static_cast<double>(best) + cfg_.riskWeight * (1.0 - robustness)
                           + cfg_.hazardWeight * hazard;
        if (ants_ && ants_->tauMax() > 0.0) {
            const double scale = edgeCount * ants_->tauMax();
            // GLM-5.3-Flash: the static nontarget region rides with the
            // repellent pheromone (doku section 12 item 5), and the (marking,
            // T-suffix) pair pheromone rides in the same two places (doku
            // section 16).
            effective += cfg_.pheromoneRepelWeight * ((repelSum + regionSum + pairRepelSum) / scale)
                         - cfg_.pheromoneAttractWeight * ((attractSum + pairAttractSum) / scale);
        }
        reachable.push_back({&c, robustness, hazard, best, bestTier, effective});
    }

    out.vetoed = vetoed;

    if (!reachable.empty()) {
        // DeepSeek V4.1 Flash: "rare outranks the rest" (doku section 30).
        // Whenever a candidate can lead to a rare goal, only those candidates
        // compete; the ordinary tiers are the fallback for when no rare goal
        // is reachable through any input.
        const bool anyRare = cfg_.huntRare
                             && std::any_of(reachable.begin(), reachable.end(),
                                            [](const Eval &e) { return e.goal == GoalTier::Rare; });
        std::vector<const Eval *> pool;
        pool.reserve(reachable.size());
        for (const Eval &e : reachable)
            if (!anyRare || e.goal == GoalTier::Rare)
                pool.push_back(&e);

        const auto better = [&](const Eval &a, const Eval &b) {
            if (a.effective != b.effective)
                return a.effective < b.effective;
            if (a.robustness != b.robustness)
                return a.robustness > b.robustness;
            if (a.goal != b.goal)
                return tierRank(a.goal) < tierRank(b.goal);
            return a.cand->transition < b.cand->transition;
        };
        const Eval *w = *std::min_element(pool.begin(), pool.end(),
                                          [&](const Eval *a, const Eval *b) { return better(*a, *b); });

        // GLM-5.3-Flash: with a seed, break exact ties (same score AND same
        // robustness) with a seeded random pick instead of the lowest
        // transition index. Every pick advances a per-suggester stream, so one
        // campaign is reproducible from its seed while different seeds steer
        // different campaigns. The quality ranking above is untouched; only
        // the arbitrary last resort is randomised.
        if (cfg_.tieBreakSeed != 0) {
            std::vector<const Eval *> tied;
            for (const Eval *e : pool)
                if (e->effective == w->effective && e->robustness == w->robustness)
                    tied.push_back(e);
            if (tied.size() > 1) {
                std::mt19937_64 rng(cfg_.tieBreakSeed + tieCall_ * 0x9E3779B97F4A7C15ULL);
                w = tied[rng() % tied.size()];
            }
            ++tieCall_;
        }

        const Transition &chosen = net.transitions[w->cand->transition];
        out.status = SuggestionStatus::Suggest;
        out.transition = w->cand->transition;
        out.label = chosen.label;
        out.robustness = w->robustness;
        out.hazard = w->hazard;
        out.goal = w->goal;
        out.steps = w->score - std::min(w->score - 1, penalty(w->goal));
        out.reason = "leads to " + goalText(w->goal) + " in " + std::to_string(out.steps) + " step(s)";
        // DeepSeek V4.1 Flash: a controllable rare input is sent only after
        // its wait-for has elapsed at the enabling state (doku section 30).
        if (cfg_.huntRare && chosen.rare && rareGoal(w->cand->transition)
            && chosen.rare->waitForMs > 0) {
            out.dwell = std::chrono::milliseconds(chosen.rare->waitForMs);
            out.reason += "; dwelling for " + std::to_string(chosen.rare->waitForMs)
                          + " ms before sending the input";
        }
        return out;
    }

    // Nothing to send. Is waiting for the SUT useful, are we done, or are we stuck?
    const bool goalsReachableFromHere = std::any_of(belief.begin(), belief.end(), [&](StateId s) {
        return s < n && cost[s] != kInf;
    });
    const std::vector<std::string> expected = tracker.expectedLabels();

    if (goalSeeds == 0) {
        out.status = SuggestionStatus::Done;
        out.reason = vetoed.empty() ? "every explored edge is covered and no target is left to seek"
                                    : "only moves into satisfied targets remain";
    } else if (!expected.empty() && goalsReachableFromHere) {
        out.status = SuggestionStatus::Wait;
        out.reason = "no useful input; waiting for the SUT's next output";
    } else {
        out.status = SuggestionStatus::Stuck;
        out.reason = "goals remain, but none is reachable from the current state: end the run and restart";
    }
    return out;
}

}  // namespace pn
