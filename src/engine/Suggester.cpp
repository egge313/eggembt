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
    case GoalTier::Target:        return cfg_.penaltyTarget;
    case GoalTier::NewTransition: return cfg_.penaltyNewTransition;
    case GoalTier::Edge:          return cfg_.penaltyEdge;
    case GoalTier::Frontier:      return cfg_.penaltyFrontier;
    case GoalTier::None:          break;
    }
    return 0;
}

Suggestion Suggester::suggest(const BeliefTracker &tracker)
{
    Suggestion out;
    if (!tracker.runActive() || tracker.belief().empty()) {
        out.status = SuggestionStatus::Stuck;
        out.reason = "no run in progress";
        return out;
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

    // --- distance to the nearest goal: one backward Dijkstra over the explored graph ----------
    const std::size_t n = rg_.numStates();
    std::vector<std::uint32_t> cost(n, kInf);
    std::vector<GoalTier> tier(n, GoalTier::None);
    using Item = std::pair<std::uint32_t, StateId>;
    std::priority_queue<Item, std::vector<Item>, std::greater<Item>> heap;
    std::size_t goalSeeds = 0;

    const auto seed = [&](StateId u, std::uint32_t c, GoalTier t) {
        ++goalSeeds;
        if (c < cost[u]) {
            cost[u] = c;
            tier[u] = t;
            heap.push({c, u});
        }
    };
    const auto edgeTier = [&](const Edge &ed) {
        return db_.transitionHits(ed.transition) == 0 ? GoalTier::NewTransition : GoalTier::Edge;
    };

    for (StateId u = 0; u < n; ++u) {
        if (board_.forbidden(u))
            continue;
        if (!rg_.isExpanded(u)) {
            seed(u, cfg_.penaltyFrontier, GoalTier::Frontier);
            continue;
        }
        if (board_.classOf(u) == TargetBoard::StateClass::Seeking)
            seed(u, cfg_.penaltyTarget, GoalTier::Target);
        EdgeId e = rg_.firstEdge(u);
        for (const Edge &ed : rg_.successors(u)) {
            if (!board_.forbidden(ed.target) && db_.edge(e).definite == 0) {
                const GoalTier t = edgeTier(ed);
                seed(u, 1 + penalty(t), t);  // the edge itself is the last step
            }
            ++e;
        }
    }

    while (!heap.empty()) {
        const Item top = heap.top();
        heap.pop();
        const std::uint32_t c = top.first;
        const StateId v = top.second;
        if (c > cost[v])
            continue;
        reverse_.forEachPredecessor(v, [&](const ReverseIndex::Pred &p) {
            if (board_.forbidden(p.from))
                return;  // plans never pass through a forbidden state
            if (c + 1 < cost[p.from]) {
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
        double pairAttractSum = 0.0;  // GLM-5.3-Flash: the (marking, T-list) pairs
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
                // GLM-5.3-Flash: the (marking, T-list) pair pheromone of the
                // arrival context this edge would create (doku section 16):
                // the run's trace so far, extended by the edge's own step.
                if (trace_ && ants_->tlistLength() > 0) {
                    const TraceCascade arrival = trace_->extended(stepHashOf(net.transitions[ed.transition]));
                    pairAttractSum += ants_->pairAttract(ed.target, arrival);
                    pairRepelSum += ants_->pairRepel(ed.target, arrival);
                }
            }

            if (board_.forbidden(ed.target)) {
                ++forbiddenTargets;
                continue;
            }
            if (db_.edge(e).definite == 0) {
                const GoalTier t = edgeTier(ed);
                const std::uint32_t sc = 1 + penalty(t);
                if (sc < best) {
                    best = sc;
                    bestTier = t;
                }
            }
            if (cost[ed.target] != kInf && 1 + cost[ed.target] < best) {
                best = 1 + cost[ed.target];
                bestTier = tier[ed.target];
            }
        }

        const double edgeCount = static_cast<double>(c.edges.size());
        const double hazard = static_cast<double>(forbiddenTargets) / edgeCount;
        if (forbiddenTargets == c.edges.size()) {
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
            // T-list) pair pheromone rides in the same two places (doku
            // section 16).
            effective += cfg_.pheromoneRepelWeight * ((repelSum + regionSum + pairRepelSum) / scale)
                         - cfg_.pheromoneAttractWeight * ((attractSum + pairAttractSum) / scale);
        }
        reachable.push_back({&c, robustness, hazard, best, bestTier, effective});
    }

    out.vetoed = vetoed;

    if (!reachable.empty()) {
        const auto better = [](const Eval &a, const Eval &b) {
            if (a.effective != b.effective)
                return a.effective < b.effective;
            if (a.robustness != b.robustness)
                return a.robustness > b.robustness;
            return a.cand->transition < b.cand->transition;
        };
        const Eval *w = &*std::min_element(reachable.begin(), reachable.end(), better);

        // GLM-5.3-Flash: with a seed, break exact ties (same score AND same
        // robustness) with a seeded random pick instead of the lowest
        // transition index. Every pick advances a per-suggester stream, so one
        // campaign is reproducible from its seed while different seeds steer
        // different campaigns. The quality ranking above is untouched; only
        // the arbitrary last resort is randomised.
        if (cfg_.tieBreakSeed != 0) {
            std::vector<const Eval *> tied;
            for (const Eval &e : reachable)
                if (e.effective == w->effective && e.robustness == w->robustness)
                    tied.push_back(&e);
            if (tied.size() > 1) {
                std::mt19937_64 rng(cfg_.tieBreakSeed + tieCall_ * 0x9E3779B97F4A7C15ULL);
                w = tied[rng() % tied.size()];
            }
            ++tieCall_;
        }

        out.status = SuggestionStatus::Suggest;
        out.transition = w->cand->transition;
        out.label = net.transitions[w->cand->transition].label;
        out.robustness = w->robustness;
        out.hazard = w->hazard;
        out.goal = w->goal;
        out.steps = w->score - std::min(w->score - 1, penalty(w->goal));
        out.reason = "leads to " + goalText(w->goal) + " in " + std::to_string(out.steps) + " step(s)";
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
