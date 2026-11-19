// Suggests the next input (controllable transition) to send to the SUT.
//
// WHAT IS WORTH REACHING (goals), cheapest first:
//   Target         a state of a target still being sought (error marking,
//                  deadlock, terminal cycle; see TargetBoard)
//   NewTransition  an edge whose transition has never been definitely covered
//   Edge           an RG edge that has not been definitely covered
//   Frontier       a state that has not been expanded yet: unknown territory
// Each tier has a penalty (in "steps") so that, say, an uncovered edge one step
// away competes sensibly with a never-covered transition three steps away.
//
// WHAT IS OFF LIMITS: states of satisfied targets (TargetBoard::Forbidden).
// With the default phase policy (SatisfiedTargetPolicy::Veto), a candidate
// whose every possible effect leads straight into one is VETOED, plans never
// pass through one, and a candidate that leads there from only some of the
// possible states is penalised by the hazard fraction. DeepSeek V4.1 Flash:
// --satisfied-target-policy can relax the hard part: Penalise lets plans
// enter and pass through a satisfied target but keeps the hazard, Ignore
// treats the states as ordinary. The Seeking -> Satisfied transition itself
// is untouched; only the suggester's reaction to it changes.
//
// HOW CANDIDATES ARE RANKED. Every input enabled in at least one possible
// state (BeliefTracker::enabledControllable) is a candidate. Its score is the
// number of steps to the best goal after taking it, plus penalties for risk:
//
//   robustness  how likely the input still works when it arrives. It is the
//               fraction of possible states that enable it, times the mean
//               over those states of the fraction of the SUT's own possible
//               moves (outputs, internal steps) after which it is still
//               enabled. A silent step that takes the token, or switches on an
//               inhibitor arc, lowers it. Environment moves are weighted
//               equally; observed offered/taken statistics can refine this later.
//   hazard      the fraction of enabling states in which the input leads
//               straight into a forbidden state.
//
//   pheromone   with an AntColony attached, the mean attractive pheromone over
//               the input's edges (as a fraction of tauMax) lowers the score,
//               the mean repellent pheromone raises it. The same means carry
//               the static nontarget region with the repellent channel, and
//               -- with the T-suffix layer on -- the (marking, T-suffix) pair
//               pheromone of the arrival context each edge would create,
//               arrived at with the live run's trace extended by the edge's
//               step (GLM-5.3-Flash, doku section 16).
//
//   effective score = steps + penalty + riskWeight * (1 - robustness)
//                     + hazardWeight * hazard
//                     - pheromoneAttractWeight * attract + pheromoneRepelWeight * repel
//
// Distances come from one backward Dijkstra from all goals over the explored
// graph (using the ReverseIndex). Before it, the suggester expands up to
// `expansionBudget` states ahead of the candidates, so goals beyond the
// currently known graph can be found.
//
// Not thread-safe. Calls TargetBoard::refresh(), which is O(states).
#pragma once

#include "engine/BeliefTracker.h"
#include "engine/CoverageDb.h"
#include "engine/LivelockAnalysis.h"
#include "engine/ReachabilityGraph.h"
#include "engine/ReverseIndex.h"
#include "engine/TSuffix.h"
#include "engine/TargetBoard.h"

#include <cstdint>
#include <string>
#include <vector>

namespace pn {

class AntColony;

// DeepSeek V4.1 Flash: how the suggester reacts to a SATISFIED target
// (TargetBoard::Forbidden). The phase transition itself is unchanged; this
// only decides what the plan search and the hard veto do with the states.
//
//   Veto      the historical behaviour: refuse every candidate whose every
//             effect enters one, never plan through one
//   Penalise  allow the move and the plan, but keep the hazard fraction in
//             the score (a certain entry costs hazardWeight steps)
//   Ignore    allow the move and the plan, and do not count the hazard
//
// The run outcome is not changed: a run that reaches a satisfied error
// marking is still ErrorReached, and an intentional target still ends the
// campaign. The switch only says whether the suggester may choose the move.
enum class SatisfiedTargetPolicy : std::uint8_t { Veto, Penalise, Ignore };

inline const char *toString(SatisfiedTargetPolicy policy)
{
    switch (policy) {
    case SatisfiedTargetPolicy::Veto:     return "veto";
    case SatisfiedTargetPolicy::Penalise: return "penalise";
    case SatisfiedTargetPolicy::Ignore:   return "ignore";
    }
    return "veto";
}

struct SuggesterConfig {
    std::size_t expansionBudget = 500;   // states expanded ahead per suggestion
    std::uint32_t penaltyTarget = 0;     // tier penalties, in steps
    std::uint32_t penaltyNewTransition = 0;
    std::uint32_t penaltyEdge = 2;
    std::uint32_t penaltyFrontier = 4;
    double riskWeight = 2.0;
    double hazardWeight = 20.0;
    double pheromoneAttractWeight = 1.0;  // in steps, at full (tauMax) pheromone
    double pheromoneRepelWeight = 3.0;
    TargetPolicies targets;

    // DeepSeek V4.1 Flash: what a satisfied target means to the suggester
    // (--satisfied-target-policy). Veto is the historical hard refusal.
    SatisfiedTargetPolicy satisfiedTargets = SatisfiedTargetPolicy::Veto;

    // GLM-5.3-Flash: 0 breaks exact ties deterministically (more robust input,
    // then lower transition index); a non-zero seed chooses reproducibly at
    // random among exactly equal candidates. Copied from
    // SessionConfig::selectionSeed by the Session.
    std::uint64_t tieBreakSeed = 0;
};

enum class SuggestionStatus {
    Suggest,  // send `label`
    Wait,     // nothing to send; the SUT is expected to produce an output
    Done,     // no goal is left (everything explored is covered, targets satisfied)
    Stuck     // goals exist but none is reachable from here: end the run, restart
};

enum class GoalTier : std::uint8_t { Target, NewTransition, Edge, Frontier, None };

struct Suggestion {
    SuggestionStatus status = SuggestionStatus::Stuck;
    TransitionIndex transition = 0;  // valid for Suggest
    std::string label;
    double robustness = 0.0;         // 0..1, see above
    double hazard = 0.0;             // 0..1
    std::uint32_t steps = 0;         // steps to the goal, including the suggested one
    GoalTier goal = GoalTier::None;
    std::string reason;
    std::vector<std::string> vetoed; // labels refused because they lead straight into a forbidden state
                                     // (DeepSeek V4.1 Flash: only under SatisfiedTargetPolicy::Veto)
};

class Suggester {
public:
    Suggester(ReachabilityGraph &rg, const CoverageDb &db, SuggesterConfig config = {});

    // Use the certain terminal cycles of a static analysis as livelock targets.
    void setLivelockFindings(const std::vector<LivelockFinding> &findings);

    // Optional: use this colony's pheromone as a further ranking input. The
    // colony must work on the same graph and outlive the suggester (or be detached
    // with nullptr).
    void setAntColony(const AntColony *ants) { ants_ = ants; }

    // GLM-5.3-Flash: the live run's T-suffix cascade (doku section 16). The
    // session sets it once; it must outlive the suggester (the session owns
    // it). Only read when the attached colony has the pair layer on.
    void setTraceCascade(const TraceCascade *trace) { trace_ = trace; }

    // `tracker` must have a run in progress; it supplies the belief set.
    Suggestion suggest(const BeliefTracker &tracker);

    const TargetBoard &targets() const { return board_; }

private:
    std::uint32_t penalty(GoalTier tier) const;

    ReachabilityGraph &rg_;
    const CoverageDb &db_;
    SuggesterConfig cfg_;
    TargetBoard board_;
    ReverseIndex reverse_;
    const AntColony *ants_ = nullptr;
    const TraceCascade *trace_ = nullptr;  // GLM-5.3-Flash: the run's T-suffix
    std::uint64_t tieCall_ = 0;  // GLM-5.3-Flash: advances with every random tie-break
};

}  // namespace pn
