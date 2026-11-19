// Ant colony guidance over the reachability graph.
//
// Ants walk the MODEL (never the SUT) and leave pheromone on RG edges. Two
// channels, with separate evaporation:
//
//   attract   "there is something worth reaching down this way"
//   repel     "there is a catastrophe down this way"; evaporates more slowly,
//             so the memory of a costly mistake outlives the attraction to a goal
//
// WHAT AN ANT MEETS, on arriving in a state:
//   a Forbidden state     a satisfied target (error marking, deadlock, terminal
//                         cycle: see TargetBoard) is a CATASTROPHE: the ant dies
//                         and marks the way in as repellent
//   a Seeking state       a target still being sought: the ant marks the way in
//                         as attractive and stops
//   unexplored territory  an unexpanded state, or a state with an edge that has
//                         not been definitely covered: attractive, stop
//   a nontarget region     GLM-5.3-Flash (doku section 12 item 5): a state from
//                         which no target marking is reachable is a REPELLENT
//                         REGION -- whatever goes in there can never come out
//                         to a legitimate end, so a run that wanders in has to
//                         be restarted. The way in carries STATIC repel: the
//                         colony sets it whenever the graph grows, it never
//                         accumulates and never evaporates, and it stays
//                         modest so uncovered behaviour inside the region can
//                         still win when the campaign needs it. States the
//                         board is still seeking (error markings, deadlocks,
//                         livelocks) are excused -- the campaign goes there on
//                         purpose -- and so is everything that can still
//                         reach unknown territory. An ant that walks into the
//                         region is not killed: it may still cover what lies
//                         there.
// Anything else, the ant walks on. Edges whose coverage reached the threshold
// are SATURATED: ants no longer find them interesting and their attractive
// pheromone is wiped, so the colony moves on to other regions.
//
// BACK-PROPAGATION. A deposit is not laid only along the ant's own path. From
// the state where the event happened, the colony walks the reverse edges of the
// graph (ReverseIndex) to a depth of at least 5 previous RG nodes and marks
// every edge it meets with amount * depthDecay^d, d being how far behind the
// event the edge's target lies. So predecessors the ant never visited learn
// too, and an event shapes the colony's view of everything that leads to it.
// In a cycle the signal also reaches edges that merely lead back to the place
// where the event can happen, weaker the further they are; the direct way in
// always carries the most.
//
// HOW AN ANT CHOOSES. At a state, the SUT's own moves (outputs, internal steps)
// and the tester's inputs compete. With probability pEnvironment the ant takes
// an environment move (drawn by `environmentWeight`, uniform by default),
// otherwise one of the inputs, drawn with weight
//     (attract + 0.1)^alpha * novelty^beta * exp(-lambda * (repel + region))
// where novelty = 1/(1 + hits), or 0.01 for a saturated edge, and `region` is
// the static nontarget region above (GLM-5.3-Flash). The tester can
// only choose inputs, hence the two-step draw. With the T-list layer on
// (GLM-5.3-Flash, doku section 16), the input weight gains the (marking,
// T-list) pair pheromone of the arrival context the edge would create: the
// pair attraction joins the base of the power, the pair repulsion joins the
// exponent of the decay, and the ant's own cascade -- fresh at its start, one
// step pushed per edge it walks -- is the trace. Environment moves are
// untouched by the layer.
//
// Runs are deterministic for a given seed (own uniform generator on top of
// std::mt19937_64, so results do not depend on the standard library).
//
// Pheromone is NOT persisted: edge ids belong to one session's graph. The
// colony re-learns quickly from the coverage that the run log restores.
//
// Not thread-safe. iterate() refreshes its TargetBoard, which is O(states).
#pragma once

#include "engine/CoverageDb.h"
#include "engine/LivelockAnalysis.h"
#include "engine/ReachabilityGraph.h"
#include "engine/ReverseIndex.h"
#include "engine/TList.h"
#include "engine/TargetBoard.h"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <random>
#include <unordered_map>
#include <vector>

namespace pn {

struct AntConfig {
    std::uint64_t seed = 1;
    std::size_t antsPerIteration = 20;
    std::size_t maxSteps = 40;           // steps an ant may take
    std::size_t expansionBudget = 200;   // states the ants may expand per iteration

    unsigned backpropDepth = 5;          // previous RG nodes marked per event; never below 5
    double depthDecay = 0.7;             // pheromone factor per step of distance

    double alpha = 1.0;                  // weight of attractive pheromone
    double beta = 1.0;                   // weight of novelty
    double lambda = 1.0;                 // weight of repellent pheromone

    double evaporationAttract = 0.10;    // fraction lost per iteration
    double evaporationRepel = 0.02;
    double depositUnexplored = 1.0;
    double depositTarget = 2.0;
    double depositCatastrophe = 3.0;
    double tauMax = 10.0;                // pheromone is capped here

    // GLM-5.3-Flash: a catastrophe deserves worse pheromones the more time it
    // takes to reach the error state. A repeat visit pays the way in again,
    // and for a given SUT the wall-clock time of that way grows with the
    // number of inputs on it -- the quantity a run can measure reproducibly.
    // The Session therefore prices its real-run deposits (a crashed SUT, a run
    // stranded in a deadlock) with catastropheDeposit() below: the base
    // deposit grows with the inputs the run spent getting there, so a crash
    // one input away is marked lightly while a crash deep in the model is
    // marked hard (tauMax remains the ceiling). The colony's own deposits --
    // ants that die in a Forbidden state -- stay flat: an ant's walk through
    // the model costs the campaign nothing.
    double catastropheTimeWeight = 0.25;  // extra base deposits per input on the way in
    std::size_t catastropheTimeCap = 8;   // inputs counted at most, however long the run was

    // GLM-5.3-Flash: the deposit for a catastrophe reached after `stimuli`
    // inputs. With catastropheTimeWeight = 0 this is the flat
    // depositCatastrophe again.
    double catastropheDeposit(std::size_t stimuli) const
    {
        const std::size_t way = std::min(stimuli, catastropheTimeCap);
        return depositCatastrophe * (1.0 + catastropheTimeWeight * static_cast<double>(way));
    }

    // GLM-5.3-Flash: the deposit for certainly reaching a MILESTONE
    // transition. Rank 1 sits just under the ant's deposit for a target
    // (depositTarget = 2.0): a milestone is not the campaign's purpose -- that
    // is the target marking -- but reaching one is a success worth advertising
    // almost as loudly. The rank multiplies: a rank-3 milestone counts three
    // times as much. The way-in is NOT priced here (unlike catastrophes): a
    // success is a success, however long the way to it was, and a deposit
    // that grew with the way in would make deep milestones paradoxically more
    // attractive than shallow ones of the same rank. tauMax remains the
    // ceiling.
    double depositMilestone = 1.5;

    double milestoneDeposit(std::uint32_t rank) const
    {
        return depositMilestone * static_cast<double>(rank);
    }

    // GLM-5.3-Flash: the deposit for the nontarget region (doku section 12
    // item 5). States from which no target marking is reachable form a further
    // repellent region: whatever goes in there can never come out to a
    // legitimate end, so a run that wanders in has to be restarted. Unlike
    // the catastrophe repel (learned from events, subject to evaporation) the
    // region is STATIC knowledge about the explored model: the colony SETS
    // this amount on every edge into the region each time the graph grows,
    // it never accumulates and never evaporates, and it is deliberately
    // modest (half a milestone deposit) so uncovered behaviour inside the
    // region can still win when the campaign needs it. Three kinds of states
    // are excused: states the board is still SEEKING (an error marking, a
    // deadlock, a livelock -- the campaign goes there on purpose), and states
    // that can still reach an unexpanded or incomplete state (a target marking
    // might lie beyond what is not known yet). 0 switches the region off.
    double nontargetRepel = 1.0;

    double pEnvironment = 0.5;           // chance of an environment move when both kinds are possible
    std::function<double(EdgeId)> environmentWeight;  // e.g. observed frequencies; empty: uniform

    // GLM-5.3-Flash: the T-list layer (doku section 16). With N > 0, every
    // marking carries a T-list -- the mini-hashes of the run trace's last N
    // steps (a suffix cascade; engine/TList.h) -- and the (marking, T-list)
    // pairs become a second, finer basis for pheromone generation beside the
    // edge channels: the session's milestone and catastrophe deposits also
    // land in a per-pair store, and ants and suggester weigh an edge under
    // the pair of the arrival context it would create. A catastrophe suffered
    // via one route no longer condemns the same marking reached via another.
    // 0 switches the layer off (the default, so the pinned pheromone amounts
    // keep their exact values).
    std::size_t tlistLength = 0;

    TargetPolicies targets;              // deadlock / livelock policies (error markings come from the model)
};

struct AntStats {
    std::uint64_t iterations = 0;
    std::uint64_t ants = 0;
    std::uint64_t died = 0;              // met a catastrophe
    std::uint64_t foundTarget = 0;
    std::uint64_t foundUnexplored = 0;
    std::uint64_t exhausted = 0;         // ran out of steps, expansion budget or edges
};

class AntColony {
public:
    static constexpr unsigned kMinBackpropDepth = 5;

    AntColony(ReachabilityGraph &rg, const CoverageDb &db, AntConfig config = {});

    // Certain terminal cycles of a static analysis become livelock targets.
    void setLivelockFindings(const std::vector<LivelockFinding> &findings);

    // One iteration: antsPerIteration ants start from `starts` (e.g. the
    // tracker's belief), then both channels evaporate and saturated edges lose
    // their attraction. With no starting states it only ages the pheromone.
    void iterate(const std::vector<StateId> &starts);

    // Back-propagating deposits, for the ants and for callers with their own
    // knowledge (e.g. a catastrophe seen in a real run).
    void depositAttract(StateId at, double amount);
    void depositRepel(StateId at, double amount);

    double attract(EdgeId e) const { return e < attract_.size() ? attract_[e] : 0.0; }
    double repel(EdgeId e) const { return e < repel_.size() ? repel_[e] : 0.0; }
    // GLM-5.3-Flash: the static nontarget region (doku section 12 item 5). Set,
    // not accumulated; recomputed when the graph grows, by iterate().
    double region(EdgeId e) const { return e < region_.size() ? region_[e] : 0.0; }
    double tauMax() const { return cfg_.tauMax; }
    unsigned backpropDepth() const { return cfg_.backpropDepth; }

    // The weight an ant gives to taking input edge `e`.
    double inputWeight(EdgeId e) const;

    // GLM-5.3-Flash: the same weight under the (marking, T-list) pair layer
    // (doku section 16): the pair pheromone of the arrival context `trace`
    // extended by this edge's step rides on top of the edge channels. With
    // the layer off this is inputWeight(e).
    double inputWeight(EdgeId e, const TraceCascade &trace) const;

    // GLM-5.3-Flash: the pair store (doku section 16). Deposits are flat at
    // the arrival pair -- no back-propagation, the edge channels still do the
    // propagating -- and capped at tauMax like every deposit. With the layer
    // off (tlistLength 0) they do nothing.
    void depositPairAttract(StateId at, const TraceCascade &arrival, double amount);
    void depositPairRepel(StateId at, const TraceCascade &arrival, double amount);
    double pairAttract(StateId at, const TraceCascade &arrival) const;
    double pairRepel(StateId at, const TraceCascade &arrival) const;
    std::size_t tlistLength() const { return cfg_.tlistLength; }
    std::size_t pairCount() const { return pairs_.size(); }

    // DeepSeek V4.1 Flash: the pair store as readable rows, for the pheromone dump
    // (--dump-pheromone, doku section 21). Sorted by (state, list), so the
    // file does not depend on the hash order. `tlist` is the key in full; only
    // the first tlistLength() elements are meaningful.
    struct PairEntry {
        StateId state = kNoState;
        std::array<std::uint32_t, kMaxTList> tlist{};
        double attract = 0.0;
        double repel = 0.0;
    };
    std::vector<PairEntry> pairEntries() const;

    const AntStats &stats() const { return stats_; }
    const TargetBoard &targets() const { return board_; }

    // GLM-5.3-Flash: the configuration this colony was built with (read-only).
    // Tests use it to verify what the session wired into it (e.g. the
    // environment feed, doku section 12 item 3).
    const AntConfig &config() const { return cfg_; }

private:
    // GLM-5.3-Flash: one (marking, T-list) pair (doku section 16). The key
    // carries the whole raw element array; entries at or beyond the configured
    // length stay kTraceSeed, so the key is determined by the length alone.
    struct PairKey {
        StateId state;
        std::array<std::uint32_t, kMaxTList> tlist;
        bool operator==(const PairKey &o) const { return state == o.state && tlist == o.tlist; }
    };
    struct PairKeyHash {
        std::size_t operator()(const PairKey &k) const
        {
            // Order-independent fold over the key's words; no std::hash.
            std::uint64_t h = k.state;
            for (const std::uint32_t w : k.tlist) {
                h ^= w;
                h *= 0x9E3779B97F4A7C15ull;  // golden ratio, 64-bit
                h ^= h >> 29;
            }
            return static_cast<std::size_t>(h);
        }
    };
    struct PairPheromone {
        float attract = 0.0f;
        float repel = 0.0f;
    };

    void deposit(StateId at, double amount, std::vector<float> &channel);
    void ensure();
    void refreshNontargetRegionIfGrown();  // GLM-5.3-Flash: doku section 12 item 5
    void runAnt(StateId start);
    bool arrive(StateId s);
    EdgeId choose(StateId at, const TraceCascade &trace);
    double weight(EdgeId e, double extraAttract, double extraRepel) const;
    std::uint32_t edgeStepHash(EdgeId e) const;
    double uniform01();

    ReachabilityGraph &rg_;
    const CoverageDb &db_;
    AntConfig cfg_;
    TargetBoard board_;
    ReverseIndex reverse_;
    std::mt19937_64 rng_;

    std::vector<float> attract_;
    std::vector<float> repel_;
    std::vector<float> region_;         // GLM-5.3-Flash: the static nontarget region
    std::size_t regionStatesSeen_ = 0;  // graph size at the last region refresh
    std::size_t regionEdgesSeen_ = 0;
    std::size_t expansionsLeft_ = 0;
    std::unordered_map<PairKey, PairPheromone, PairKeyHash> pairs_;  // GLM-5.3-Flash
    std::vector<std::uint32_t> stepHashes_;  // GLM-5.3-Flash: per transition, see ensure()
    AntStats stats_;
};

}  // namespace pn
