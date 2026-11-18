#include "engine/TargetBoard.h"

namespace pn {

TargetBoard::TargetBoard(const ReachabilityGraph &rg, const CoverageDb &db, TargetPolicies policies)
    : rg_(rg), db_(db), policies_(policies)
{
}

void TargetBoard::setLivelockFindings(const std::vector<LivelockFinding> &findings)
{
    livelocks_.clear();
    for (const LivelockFinding &f : findings)
        if (f.kind == LivelockKind::TerminalCycle && f.certain)
            livelocks_.push_back(f.states);
    livelockStatus_.assign(livelocks_.size(), TargetStatus{});
}

TargetStatus TargetBoard::status(TargetPolicy policy, std::optional<std::uint32_t> explicitHits,
                                 std::uint64_t hits) const
{
    TargetStatus s;
    s.hits = hits;
    switch (policy) {
    case TargetPolicy::Once:
        s.target = 1;
        break;
    case TargetPolicy::Avoid:
        s.target = 0;
        break;
    case TargetPolicy::UntilCovered:
        s.target = explicitHits.value_or(db_.config().threshold);
        break;
    }
    s.phase = hits >= s.target ? TargetPhase::Satisfied : TargetPhase::Seeking;
    return s;
}

TargetStatus TargetBoard::errorMarking(std::size_t i) const
{
    TargetStatus s;
    s.hits = db_.errorMarkingHits(i);
    s.target = db_.errorTarget(i);
    s.phase = db_.isErrorSatisfied(i) ? TargetPhase::Satisfied : TargetPhase::Seeking;
    return s;
}

void TargetBoard::refresh()
{
    const std::size_t n = rg_.numStates();
    class_.assign(n, StateClass::Normal);

    const auto classFor = [](const TargetStatus &s) {
        return s.phase == TargetPhase::Satisfied ? StateClass::Forbidden : StateClass::Seeking;
    };

    // Error markings.
    for (StateId s = 0; s < n; ++s) {
        if (rg_.kind(s) == StateKind::Error)
            class_[s] = classFor(errorMarking(static_cast<std::size_t>(rg_.markingIndex(s))));
    }

    // GLM-5.3-Flash: intentional target markings ("good target states") are
    // normal targets -- the tester steers into them; the certain reach of one
    // ends the campaign. Non-intentional targets stay Normal: they are idle
    // states for the livelock analysis, not goals. (A target state is never a
    // deadlock -- isDeadlock requires kind Normal -- and the livelock pass
    // below only overrides Normal classes, so this assignment survives.)
    for (StateId s = 0; s < n; ++s) {
        if (rg_.kind(s) != StateKind::Target)
            continue;
        const auto idx = static_cast<std::size_t>(rg_.markingIndex(s));
        if (!rg_.net().targetMarkings[idx].intentional)
            continue;
        TargetStatus st;
        st.hits = db_.targetMarkingHits(idx);
        st.target = 1;  // the first certain visit ends the campaign
        st.phase = st.hits >= st.target ? TargetPhase::Satisfied : TargetPhase::Seeking;
        class_[s] = classFor(st);
    }

    // Deadlocks: one target for all of them.
    std::vector<StateId> dead;
    std::uint64_t deadlockHits = 0;
    for (StateId s = 0; s < n; ++s) {
        if (rg_.isDeadlock(s)) {
            dead.push_back(s);
            deadlockHits += db_.state(s).definite;
        }
    }
    deadlockStatus_ = status(policies_.deadlock, policies_.deadlockHits, deadlockHits);
    for (StateId s : dead)
        class_[s] = classFor(deadlockStatus_);

    // Livelocks: one target per certain terminal cycle.
    livelockStatus_.assign(livelocks_.size(), TargetStatus{});
    for (std::size_t i = 0; i < livelocks_.size(); ++i) {
        std::uint64_t hits = 0;
        for (StateId s : livelocks_[i])
            hits += db_.state(s).definite;
        livelockStatus_[i] = status(policies_.livelock, policies_.livelockHits, hits);
        for (StateId s : livelocks_[i])
            if (s < n && class_[s] == StateClass::Normal)  // an error marking keeps its own class
                class_[s] = classFor(livelockStatus_[i]);
    }

    // GLM-5.3-Flash: the crash predictions. While a crash entry still seeks
    // (its binomial experiment is unresolved), its place is a goal like any
    // sought target: the campaign must come back and reproduce the crash,
    // and "coverage complete" waits for the experiment. Once the entry
    // resolves it leaves the board -- a deterministic or exhausted-with-
    // crashes entry is avoided by the catastrophe pheromones the session
    // lays, exactly like an error transition, and an entry that never saw
    // its crash teaches nothing. These overrides run last, so a predicted
    // marking keeps its states sought even when the marking itself is
    // satisfied already: the reproduction is not done until the experiment
    // says so.
    for (const CrashEntry &e : db_.crashEntries()) {
        if (e.status != CrashStatus::Seeking)
            continue;
        if (e.transition != kNoTransition) {
            // A predicted TRANSITION is sought where it is enabled: from such
            // a state the campaign can step it, and the step is the attempt.
            const Transition &t = rg_.net().transitions[e.transition];
            for (StateId s = 0; s < n; ++s)
                if (isEnabled(t, rg_.tokens(s)))
                    class_[s] = StateClass::Seeking;
        } else {
            // A predicted MARKING is sought where the graph classified a
            // state as certainly in it.
            const std::string name = e.key.substr(2);  // strip "m:"
            int errorIdx = -1, targetIdx = -1;
            for (std::size_t i = 0; i < rg_.net().errorMarkings.size(); ++i)
                if (rg_.net().errorMarkings[i].name == name)
                    errorIdx = static_cast<int>(i);
            for (std::size_t i = 0; i < rg_.net().targetMarkings.size(); ++i)
                if (rg_.net().targetMarkings[i].name == name)
                    targetIdx = static_cast<int>(i);
            if (errorIdx < 0 && targetIdx < 0)
                continue;
            for (StateId s = 0; s < n; ++s) {
                const StateKind k = rg_.kind(s);
                const int idx = rg_.markingIndex(s);
                if ((k == StateKind::Error && idx == errorIdx)
                    || (k == StateKind::Target && idx == targetIdx))
                    class_[s] = StateClass::Seeking;
            }
        }
    }
}

}  // namespace pn
