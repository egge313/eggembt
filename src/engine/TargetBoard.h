// The phases of the "bad" targets: error markings, deadlocks, livelocks.
//
// All three are things the tester WANTS to reach a few times (they are part of
// the specification to cover) and then wants to AVOID, because dealing with
// them is expensive: collecting a dump, restarting the run. So every target
// is in one of two phases:
//
//   Seeking    not reached often enough yet; the states count as goals.
//   Satisfied  reached `target` times (definite visits); the states are
//              FORBIDDEN: the suggester never steps into them and plans
//              around them.
//
// Error markings follow their own policy (`until_covered` / `once` / `avoid`,
// `target_hits`) from the model. Deadlocks and livelocks are set by
// TargetPolicies (the session configuration), `once` by default: visit once to
// confirm the fault in the SUT, then avoid.
//
// GLM-5.3-Flash: the crash predictions fold in here too (engine/Crash-
// Credibility.h). While a crash entry seeks, its place -- the states where a
// predicted transition is enabled, or the states certainly in a predicted
// marking -- counts as Seeking: a goal the campaign steers into, and one that
// keeps "coverage complete" waiting. A resolved entry leaves the board: the
// catastrophe pheromones take over, or (never reproduced) nothing does.
//
//   * Deadlocks: all deadlock states of the explored graph form one target;
//     hits are the definite visits to any of them.
//   * Livelocks: every CERTAIN terminal cycle found by analyzeLivelocks is a
//     target of its own; hits are the definite visits to its states.
//     Uncertain findings and internal cycles are ignored.
//
// The board is recomputed from the graph and the coverage database by
// refresh(), which is O(states); call it when you need current classes.
#pragma once

#include "engine/CoverageDb.h"
#include "engine/LivelockAnalysis.h"
#include "engine/ReachabilityGraph.h"

#include <cstdint>
#include <optional>
#include <vector>

namespace pn {

enum class TargetPhase { Seeking, Satisfied };

struct TargetStatus {
    TargetPhase phase = TargetPhase::Seeking;
    std::uint64_t hits = 0;
    std::uint32_t target = 0;
};

struct TargetPolicies {
    TargetPolicy deadlock = TargetPolicy::Once;
    TargetPolicy livelock = TargetPolicy::Once;
    // Used with until_covered; unset means the coverage threshold of the CoverageDb.
    std::optional<std::uint32_t> deadlockHits;
    std::optional<std::uint32_t> livelockHits;
};

class TargetBoard {
public:
    enum class StateClass : std::uint8_t {
        Normal,
        Seeking,    // part of a target that is still being sought
        Forbidden   // part of a satisfied target
    };

    TargetBoard(const ReachabilityGraph &rg, const CoverageDb &db, TargetPolicies policies = {});

    // Keeps the certain terminal cycles of `findings`.
    void setLivelockFindings(const std::vector<LivelockFinding> &findings);

    // Recomputes every state's class and every target's status.
    void refresh();

    StateClass classOf(StateId s) const { return s < class_.size() ? class_[s] : StateClass::Normal; }
    bool forbidden(StateId s) const { return classOf(s) == StateClass::Forbidden; }

    TargetStatus errorMarking(std::size_t i) const;  // always current: read from the CoverageDb
    TargetStatus deadlocks() const { return deadlockStatus_; }
    std::size_t numLivelocks() const { return livelocks_.size(); }
    TargetStatus livelock(std::size_t i) const { return livelockStatus_[i]; }

private:
    TargetStatus status(TargetPolicy policy, std::optional<std::uint32_t> explicitHits,
                        std::uint64_t hits) const;

    const ReachabilityGraph &rg_;
    const CoverageDb &db_;
    TargetPolicies policies_;
    std::vector<std::vector<StateId>> livelocks_;
    std::vector<TargetStatus> livelockStatus_;
    TargetStatus deadlockStatus_;
    std::vector<StateClass> class_;
};

}  // namespace pn
