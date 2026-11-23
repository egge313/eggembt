#include "engine/AntColony.h"

#include <algorithm>
#include <cmath>

namespace pn {

AntColony::AntColony(ReachabilityGraph &rg, const CoverageDb &db, AntConfig config)
    : rg_(rg), db_(db), cfg_(std::move(config)), board_(rg, db, cfg_.targets), reverse_(rg), rng_(cfg_.seed)
{
    cfg_.backpropDepth = std::max(cfg_.backpropDepth, kMinBackpropDepth);
}

std::vector<AntColony::PairEntry> AntColony::pairEntries() const
{
    // DeepSeek V4.1 Flash: a read-only view of the (marking, T-suffix) store for the
    // pheromone dump. Sorted, because the hash map's order is not a contract.
    std::vector<PairEntry> rows;
    rows.reserve(pairs_.size());
    for (const auto &kv : pairs_) {
        PairEntry e;
        e.state = kv.first.state;
        e.tsuffix = kv.first.tsuffix;
        e.attract = kv.second.attract;
        e.repel = kv.second.repel;
        rows.push_back(e);
    }
    std::sort(rows.begin(), rows.end(), [](const PairEntry &a, const PairEntry &b) {
        if (a.state != b.state)
            return a.state < b.state;
        return a.tsuffix < b.tsuffix;
    });
    return rows;
}

void AntColony::setLivelockFindings(const std::vector<LivelockFinding> &findings)
{
    board_.setLivelockFindings(findings);
}

double AntColony::uniform01()
{
    // 53 random bits, so the result is the same on every standard library.
    return static_cast<double>(rng_() >> 11) * (1.0 / 9007199254740992.0);
}

void AntColony::ensure()
{
    const std::size_t n = rg_.numEdges();
    if (attract_.size() < n) {
        attract_.resize(n, 0.0f);
        repel_.resize(n, 0.0f);
    }
    // GLM-5.3-Flash: the trace-step hash of every transition (doku section
    // 16), computed once per transition (label, or "I:" + id when silent).
    const std::size_t t = rg_.net().transitions.size();
    if (stepHashes_.size() < t)
        for (std::size_t i = stepHashes_.size(); i < t; ++i)
            stepHashes_.push_back(stepHashOf(rg_.net().transitions[i]));
}

// DeepSeek V4.1 Flash: renamed to `off-target` in journal §36; the computation
// is unchanged.
// GLM-5.3-Flash: the off-target region (doku section 12 item 5). Recomputed
// only when the graph grew. The seeds are the states behind which a
// legitimate end is KNOWN to exist or the future is unknown: states of kind
// Target, states the board is still SEEKING (an error marking, a deadlock, a
// livelock -- the campaign goes there on purpose, so the region must not
// repel the way in), and expanded states that can still step into unknown
// territory (an edge into an unexpanded state, or an incomplete expansion
// cut off by the limits). Everything expanded that reaches none of the seeds
// can never reach a target marking: the region. Every edge into it is SET to
// offTargetRepel -- never accumulated, never evaporated; edges into unknown
// territory stay clean.
void AntColony::refreshOffTargetRegionIfGrown()
{
    const std::size_t states = rg_.numStates();
    const std::size_t edges = rg_.numEdges();
    if (states == regionStatesSeen_ && edges == regionEdgesSeen_)
        return;
    reverse_.sync();
    regionStatesSeen_ = states;
    regionEdgesSeen_ = edges;
    region_.assign(edges, 0.0f);
    if (cfg_.offTargetRepel <= 0.0 || rg_.net().targetMarkings.empty())
        return;

    // The target phases decide which states are legitimate places to go;
    // refresh the board here so the classification is current even on an
    // iteration where no ant walks.
    board_.refresh();

    std::vector<char> reached(states, 0);
    std::vector<StateId> work;
    const auto seed = [&](StateId s) {
        if (!reached[s]) {
            reached[s] = 1;
            work.push_back(s);
        }
    };
    for (const StateId s : rg_.statesOfKind(StateKind::Target))
        seed(s);
    for (StateId s = 0; s < states; ++s) {
        if (!rg_.isExpanded(s) || reached[s])
            continue;
        bool excused = rg_.isIncomplete(s) || board_.classOf(s) == TargetBoard::StateClass::Seeking;
        for (const Edge &x : rg_.successors(s)) {
            if (!rg_.isExpanded(x.target)) {
                excused = true;
                break;
            }
        }
        if (excused)
            seed(s);
    }
    while (!work.empty()) {
        const StateId s = work.back();
        work.pop_back();
        reverse_.forEachPredecessor(s, [&](const ReverseIndex::Pred &p) {
            if (!reached[p.from]) {
                reached[p.from] = 1;
                work.push_back(p.from);
            }
        });
    }
    for (EdgeId e = 0; e < edges; ++e) {
        const StateId target = rg_.edge(e).target;
        if (rg_.isExpanded(target) && !reached[target])
            region_[e] = static_cast<float>(cfg_.offTargetRepel);
    }
}

void AntColony::deposit(StateId at, double amount, std::vector<float> &channel)
{
    reverse_.sync();
    ensure();
    reverse_.backwardWalk(at, cfg_.backpropDepth, [&](EdgeId e, StateId, StateId, unsigned depth) {
        const double value = amount * std::pow(cfg_.depthDecay, static_cast<double>(depth));
        channel[e] = static_cast<float>(std::min(cfg_.tauMax, static_cast<double>(channel[e]) + value));
    });
}

void AntColony::depositAttract(StateId at, double amount)
{
    deposit(at, amount, attract_);
}

void AntColony::depositRepel(StateId at, double amount)
{
    deposit(at, amount, repel_);
}

// GLM-5.3-Flash: the pair store (doku section 16). A deposit lands flat under
// the one (marking, T-suffix) pair -- the marking `at`, arrived at with the
// T-suffix `arrival` -- capped at tauMax like every deposit. No back-propagation
// here: the edge channels still do the propagating, the pairs only refine
// what they already say.
void AntColony::depositPairAttract(StateId at, const TraceCascade &arrival, double amount)
{
    if (cfg_.tsuffixLength == 0)
        return;
    PairPheromone &p = pairs_[PairKey{at, arrival.elements()}];
    p.attract = static_cast<float>(std::min(cfg_.tauMax, static_cast<double>(p.attract) + amount));
}

void AntColony::depositPairRepel(StateId at, const TraceCascade &arrival, double amount)
{
    if (cfg_.tsuffixLength == 0)
        return;
    PairPheromone &p = pairs_[PairKey{at, arrival.elements()}];
    p.repel = static_cast<float>(std::min(cfg_.tauMax, static_cast<double>(p.repel) + amount));
}

double AntColony::pairAttract(StateId at, const TraceCascade &arrival) const
{
    if (cfg_.tsuffixLength == 0)
        return 0.0;
    const auto it = pairs_.find(PairKey{at, arrival.elements()});
    return it == pairs_.end() ? 0.0 : static_cast<double>(it->second.attract);
}

double AntColony::pairRepel(StateId at, const TraceCascade &arrival) const
{
    if (cfg_.tsuffixLength == 0)
        return 0.0;
    const auto it = pairs_.find(PairKey{at, arrival.elements()});
    return it == pairs_.end() ? 0.0 : static_cast<double>(it->second.repel);
}

std::uint32_t AntColony::edgeStepHash(EdgeId e) const
{
    const TransitionIndex t = rg_.edge(e).transition;
    return t < stepHashes_.size() ? stepHashes_[t] : stepHashOf(rg_.net().transitions[t]);
}

// GLM-5.3-Flash: the ant's weight with the pair layer (doku section 16). The
// pair pheromone of the arrival context this edge would create -- its target,
// arrived at with `trace` extended by the edge's own step -- rides on top of
// the edge channels, in the same two places: the attraction joins the base of
// the power, the repulsion joins the exponent of the decay.
double AntColony::inputWeight(EdgeId e, const TraceCascade &trace) const
{
    if (cfg_.tsuffixLength == 0)
        return inputWeight(e);
    const StateId target = rg_.edge(e).target;
    const TraceCascade arrival = trace.extended(edgeStepHash(e));
    return weight(e, pairAttract(target, arrival), pairRepel(target, arrival));
}

double AntColony::weight(EdgeId e, double extraAttract, double extraRepel) const
{
    const double novelty = db_.isSaturatedEdge(e) ? 0.01 : 1.0 / (1.0 + static_cast<double>(db_.edge(e).definite));
    // GLM-5.3-Flash: the static off-target region rides with the learned repel
    // (doku section 12 item 5); `extra` carries the (marking, T-suffix) pair
    // layer (doku section 16), both channels in the same places.
    return std::pow(attract(e) + extraAttract + 0.1, cfg_.alpha) * std::pow(novelty, cfg_.beta)
           * std::exp(-cfg_.lambda * (repel(e) + region(e) + extraRepel));
}

double AntColony::inputWeight(EdgeId e) const
{
    return weight(e, 0.0, 0.0);
}

EdgeId AntColony::choose(StateId at, const TraceCascade &trace)
{
    const Net &net = rg_.net();
    const EdgeRange edges = rg_.successors(at);
    const EdgeId first = rg_.firstEdge(at);

    std::vector<EdgeId> inputs;
    std::vector<EdgeId> environment;
    for (std::size_t i = 0; i < edges.size(); ++i) {
        const EdgeId e = first + static_cast<EdgeId>(i);
        const bool isInput = net.transitions[edges.first[i].transition].kind == TransitionKind::Controllable;
        (isInput ? inputs : environment).push_back(e);
    }
    if (inputs.empty() && environment.empty())
        return kNoEdge;

    bool takeEnvironment;
    if (inputs.empty())
        takeEnvironment = true;
    else if (environment.empty())
        takeEnvironment = false;
    else
        takeEnvironment = uniform01() < cfg_.pEnvironment;

    const std::vector<EdgeId> &pool = takeEnvironment ? environment : inputs;
    std::vector<double> weights;
    weights.reserve(pool.size());
    double total = 0.0;
    for (EdgeId e : pool) {
        double w;
        if (takeEnvironment) {
            // GLM-5.3-Flash: environment moves are untouched by the pair
            // layer (doku section 16) -- the SUT's own moves have no arrival
            // context an input trace could describe.
            w = cfg_.environmentWeight ? cfg_.environmentWeight(e) : 1.0;
        } else if (cfg_.tsuffixLength == 0) {
            w = inputWeight(e);
        } else {
            // GLM-5.3-Flash: the (marking, T-suffix) pair consult (doku
            // section 16): the edge is weighed under the pair of the marking
            // it leads to, arrived at with the ant's trace extended by the
            // edge's own step.
            const StateId target = rg_.edge(e).target;
            const TraceCascade arrival = trace.extended(edgeStepHash(e));
            w = weight(e, pairAttract(target, arrival), pairRepel(target, arrival));
        }
        if (!(w > 0.0))
            w = 0.0;
        weights.push_back(w);
        total += w;
    }
    if (total <= 0.0)
        return pool[static_cast<std::size_t>(uniform01() * static_cast<double>(pool.size())) % pool.size()];

    double pick = uniform01() * total;
    for (std::size_t i = 0; i < pool.size(); ++i) {
        pick -= weights[i];
        if (pick < 0.0)
            return pool[i];
    }
    return pool.back();
}

// Returns true if something happened that ends the ant.
bool AntColony::arrive(StateId s)
{
    switch (board_.classOf(s)) {
    case TargetBoard::StateClass::Forbidden:
        deposit(s, cfg_.depositCatastrophe, repel_);
        ++stats_.died;
        return true;
    case TargetBoard::StateClass::Seeking:
        deposit(s, cfg_.depositTarget, attract_);
        ++stats_.foundTarget;
        return true;
    case TargetBoard::StateClass::Normal:
        break;
    }

    if (!rg_.isExpanded(s)) {
        if (expansionsLeft_ > 0) {
            rg_.expand(s);
            --expansionsLeft_;
        } else {
            deposit(s, cfg_.depositUnexplored, attract_);  // unexplored and out of budget to look
            ++stats_.foundUnexplored;
            return true;
        }
    }

    EdgeId e = rg_.firstEdge(s);
    for (const Edge &edge : rg_.successors(s)) {
        if (!board_.forbidden(edge.target) && db_.edge(e).definite == 0) {
            deposit(s, cfg_.depositUnexplored, attract_);
            ++stats_.foundUnexplored;
            return true;
        }
        ++e;
    }
    return false;
}

void AntColony::runAnt(StateId start)
{
    ++stats_.ants;
    // GLM-5.3-Flash: the ant carries its own T-suffix cascade (doku section
    // 16): all kTraceSeed at its start, one step pushed per edge it walks --
    // the ant's own arrival history, exactly the trace from which it arrived
    // wherever it stands.
    TraceCascade trace;
    trace.setLength(cfg_.tsuffixLength);
    StateId cur = start;
    for (std::size_t step = 0; step < cfg_.maxSteps; ++step) {
        if (!rg_.isExpanded(cur)) {
            if (expansionsLeft_ == 0)
                break;
            rg_.expand(cur);
            --expansionsLeft_;
        }
        ensure();
        const EdgeId chosen = choose(cur, trace);
        if (chosen == kNoEdge)
            break;  // a dead end: nothing to learn here
        cur = rg_.edge(chosen).target;
        trace.push(edgeStepHash(chosen));  // GLM-5.3-Flash: the arrival step
        if (arrive(cur))
            return;
    }
    ++stats_.exhausted;
}

void AntColony::iterate(const std::vector<StateId> &starts, bool evaporate)
{
    ++stats_.iterations;

    // GLM-5.3-Flash: keep the static off-target region current before anything
    // walks or chooses (a no-op unless the graph grew).
    refreshOffTargetRegionIfGrown();

    if (!starts.empty()) {
        board_.refresh();
        reverse_.sync();
        ensure();
        expansionsLeft_ = cfg_.expansionBudget;
        for (std::size_t i = 0; i < cfg_.antsPerIteration; ++i) {
            const auto pick = static_cast<std::size_t>(uniform01() * static_cast<double>(starts.size()));
            runAnt(starts[std::min(pick, starts.size() - 1)]);
        }
    }

    // DeepSeek V4.1 Flash: a caller that evaporates between its own runs
    // asks the ants to walk without aging; age() runs once at its boundary.
    if (evaporate)
        age();
}

void AntColony::age(bool spareMatureTargets)
{
    // Evaporation, then saturated edges lose their attraction. A matured
    // target's way in is exempt from the evaporation (DeepSeek V4.1 Flash):
    // its phase is Satisfied, so the finding has reached its visited-N
    // criterion and the lesson should outlive the merge. The board is the
    // same source the ants and the suggester read; refreshing it here keeps
    // the exception aligned with the live target phases.
    ensure();
    std::vector<char> spared;
    if (spareMatureTargets && rg_.numStates() > 0) {
        board_.refresh();
        spared.assign(rg_.numStates(), 0);
        for (StateId s = 0; s < rg_.numStates(); ++s)
            spared[s] = board_.classOf(s) == TargetBoard::StateClass::Forbidden ? 1 : 0;
    }
    const auto isSparedEdge = [&](EdgeId e) {
        if (spared.empty())
            return false;
        const StateId target = rg_.edge(e).target;
        return target < spared.size() && spared[target] != 0;
    };

    const auto keepAttract = static_cast<float>(1.0 - cfg_.evaporationAttract);
    const auto keepRepel = static_cast<float>(1.0 - cfg_.evaporationRepel);
    for (std::size_t e = 0; e < attract_.size(); ++e) {
        // Saturation is a coverage rule, not evaporation: even a spared way in
        // loses its attraction once the edge is covered to the threshold.
        if (db_.isSaturatedEdge(static_cast<EdgeId>(e)))
            attract_[e] = 0.0f;
        if (isSparedEdge(static_cast<EdgeId>(e)))
            continue;
        attract_[e] *= keepAttract;
        repel_[e] *= keepRepel;
    }
    // GLM-5.3-Flash: the pair store evaporates with the same per-iteration
    // rates as the edge channels (doku section 16); an entry whose both
    // channels fell below the floor is pruned. A pair at a matured target is
    // spared for the same reason its incoming edges are.
    if (!pairs_.empty()) {
        constexpr double kPairFloor = 1e-4;
        for (auto it = pairs_.begin(); it != pairs_.end();) {
            if (spared.empty() || it->first.state >= spared.size() || spared[it->first.state] == 0) {
                it->second.attract = static_cast<float>(it->second.attract * keepAttract);
                it->second.repel = static_cast<float>(it->second.repel * keepRepel);
            }
            if (it->second.attract < kPairFloor && it->second.repel < kPairFloor)
                it = pairs_.erase(it);
            else
                ++it;
        }
    }
}

}  // namespace pn
