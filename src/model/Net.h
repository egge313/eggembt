// Plain C++17 Petri net model. Deliberately free of Qt so the engine
// (enabling/firing, reachability graph, coverage) can be unit-tested and
// reused without a GUI. Only the loader at the edge knows about Qt.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace pn {

using PlaceIndex = std::uint32_t;
using TransitionIndex = std::uint32_t;
using TokenCount = std::uint32_t;

// GLM-5.3-Flash: "no transition" sentinel for API slots that optionally carry
// a transition (e.g. which error transition a crash dump belongs to).
inline constexpr TransitionIndex kNoTransition = 0xFFFFFFFFu;

// GLM-5.3-Flash: "no place" sentinel, same idea (e.g. a command hook without
// a "when" guard).
inline constexpr PlaceIndex kNoPlace = 0xFFFFFFFFu;

enum class TransitionKind {
    Controllable,  // tester stimulates the SUT
    Observable,    // SUT emits an output the tester sees
    Internal       // not visible from outside
};

enum class MatchMode {
    Exact,   // marking must equal the given tokens (unlisted places hold 0)
    Covers   // marking must hold at least the given tokens; other places are free
};

struct Place {
    std::string id;
    std::optional<TokenCount> bound;  // optional per-place token limit
};

struct WeightedArc {
    PlaceIndex place;
    TokenCount weight;
};

// How the SUT is asked for a post-mortem when an error marking is reached.
// GLM-5.3-Flash: also used for error *transitions*, so it now lives above
// struct Transition.
enum class DumpMode {
    None,      // spec/protocol fault: report and cover it, but no dump, no restart
    Snapshot,  // non-destructive dump; the SUT keeps running
    Crash      // controlled crash for a genuine crash dump; the run ends
};

// GLM-5.3-Flash: the probable class of a predicted SUT crash. Egge: "Sometimes
// an SUT crash is nondeterministic. It can be random, rare, sometimes, or
// often (but not always). Any of these indicate a bug." The class words
// calibrate the credibility machinery (engine/CrashCredibility.h): each gives
// the prior mean of the crash probability and the give-up budget of the
// reproduction experiment.
enum class CrashClass {
    Always,     // crashes (nearly) every time the place is reached
    Often,      // crashes often, but not always
    Sometimes,  // crashes about every other time
    Rare,       // crashes seldom
    Random      // no idea about the frequency
};

inline const char *toString(CrashClass c)
{
    switch (c) {
    case CrashClass::Always:    return "always";
    case CrashClass::Often:     return "often";
    case CrashClass::Sometimes: return "sometimes";
    case CrashClass::Rare:      return "rare";
    case CrashClass::Random:    return "random";
    }
    return "random";
}

// GLM-5.3-Flash: one crash prediction in the specification, either on a
// transition (crash while it steps) or on a named marking (crash while the SUT
// is certainly in it). A crash is a TARGET until it is certain to be
// deterministic: the campaign seeks the place and reproduces the crash (the
// binomial credibility experiment, engine/CrashCredibility.h) before it lets
// the catastrophe teaching avoid the way in.
//
//   class       the probable class (the word above); calibrates the prior
//               mean and the give-up budget
//   p           explicit prior mean in (0,1); overrides the class default
//   attempts    explicit give-up budget (attempts of the reproduction
//               experiment); overrides the class default
//   crashDump   post-mortem to ask the plug-in for when the crash is
//               attributed to this prediction (same words as everywhere)
//   maxDumps    how many such post-mortems over all runs
struct CrashPrediction {
    CrashClass crashClass = CrashClass::Random;  // spelled "class" in the file
    std::optional<double> p;
    std::optional<std::uint32_t> attempts;
    DumpMode crashDump = DumpMode::None;
    std::uint32_t maxDumps = 1;
};

// GLM-5.3-Flash: when the hook's shell command runs. A synchronous command
// blocks the run loop while it executes (bounded by --hook-timeout, the child
// is killed at the timeout); an asynchronous one is fire-and-forget: mbt does
// not wait for it and never kills it -- the children are reaped when mbt
// exits.
enum class HookMode {
    Sync,
    Async
};

inline const char *toString(HookMode mode)
{
    switch (mode) {
    case HookMode::Sync:  return "sync";
    case HookMode::Async: return "async";
    }
    return "sync";
}

// GLM-5.3-Flash: a shell command attached to a transition (the "command" key
// of the JSON model): after all, a Petri net model only covers the
// concurrent/distributed aspect of computing, so the campaign may want to
// invoke e.g. data oriented testing during a run, too. The command runs when
// a step of the transition is *certain* (the same certainty rule as an error
// transition or a milestone) and, with a "when" place, only while that place
// holds a token in every state the belief spans after the step (the guard
// reads, it never consumes). "times" caps the executions over the whole
// campaign (unset: every qualifying step; "once" = times 1).
struct CommandHook {
    std::string run;                     // handed to sh -c verbatim
    HookMode mode = HookMode::Sync;
    std::optional<std::uint32_t> times;  // unset: unlimited over the campaign
    PlaceIndex when = kNoPlace;          // kNoPlace: no guard
};

struct Transition {
    std::string id;
    std::string label;  // maps to a SUT action/event; may be empty for Internal
    TransitionKind kind = TransitionKind::Internal;

    // GLM-5.3-Flash: whether a step of this transition is reported by the SUT
    // as a visible event (an Observed event carrying the label). Observable
    // transitions are always traced; internal ones are traced when they carry
    // a label -- unless the model overrides it with an explicit "traced" key
    // (JSON loader), which makes a named internal step silent. NetBuilder::
    // build() and the JSON loader derive the default from kind and label.
    bool traced = false;

    // GLM-5.3-Flash: the transition is driven by a human operator, so the
    // wall clock says nothing about it. A run that steps this transition does
    // not let the catastrophe pheromones grow with the way in (they stay flat
    // for that run). Nothing else changes: the step still counts as coverage.
    bool unpredictable = false;

    // GLM-5.3-Flash: the transition itself is an error. When a step of it is
    // *certain* (the belief set knows exactly which transition fired), the
    // session reacts as if an error marking had been reached: the run fails,
    // the crash-dump request (below) is honoured and the ant colony marks the
    // spot. Untraced internal transitions cannot carry this tag because their
    // steps are never visible to the tester (the loader rejects that).
    bool error = false;

    // GLM-5.3-Flash: post-mortem request for error transitions, same meaning
    // as the crashDump field of error markings.
    DumpMode crashDump = DumpMode::None;
    std::uint32_t maxDumps = 1;  // dumps to collect over all runs

    // GLM-5.3-Flash: the transition is a MILESTONE. When a step of it is
    // *certain* (same certainty rule as an error transition: every possible
    // state advanced by this very transition), the session deposits positive
    // pheromones at every possible state -- almost as much as a target marking
    // gets (AntConfig::depositMilestone, times this rank), so the colony
    // advertises the way to it. Untraced internal transitions cannot carry
    // the tag because their steps are never visible (the loader rejects that,
    // exactly as for "error"). milestoneRank >= 1: a higher rank means a more
    // important milestone and a stronger deposit.
    bool milestone = false;
    std::uint32_t milestoneRank = 1;

    // GLM-5.3-Flash: the transition may crash the SUT nondeterministically.
    // While the crash is sought (not yet certain to be deterministic), a
    // definite step of this transition is one attempt of the reproduction
    // experiment and a run that dies with this step as the last credited one
    // is a hit. Untraced internal transitions cannot carry the tag because
    // their steps are never visible to the tester (the loader rejects that,
    // exactly as for "error" and "milestone"). A transition that is also
    // "error" cannot carry it either: an error transition is a certain
    // reaction, not a probable crash (the loader rejects that too).
    std::optional<CrashPrediction> crashPrediction;

    // GLM-5.3-Flash: the shell command this transition fires when a step of
    // it is certain (see CommandHook). Untraced internal transitions cannot
    // carry one because their steps are never visible to the tester (the
    // loader rejects that, exactly as for "error" and "milestone").
    std::optional<CommandHook> command;

    // All three lists are sorted by place index.
    std::vector<WeightedArc> pre;      // normal input arcs: need m(p) >= w, consume w
    std::vector<WeightedArc> post;     // output arcs: produce w
    std::vector<WeightedArc> inhibit;  // inhibitor arcs: need m(p) < w, consume nothing
};

struct TokenEntry {
    PlaceIndex place;
    TokenCount tokens;  // always > 0 in a sparse marking
};

// Sparse marking: sorted by place index, no zero entries.
using SparseMarking = std::vector<TokenEntry>;

// What the tester does about an error marking.
enum class TargetPolicy {
    UntilCovered,  // seek it until `targetHits` definite visits, then avoid it
    Once,          // seek it until the first definite visit, then avoid it
    Avoid          // never seek it; treat it as a catastrophe from the start
};

// GLM-5.3-Flash: the one spelling used everywhere a policy is printed or
// parsed (--deadlock-policy, the --json report); same words as DumpMode's
// toString in engine/RunLog.cpp.
inline const char *toString(TargetPolicy policy)
{
    switch (policy) {
    case TargetPolicy::UntilCovered: return "until_covered";
    case TargetPolicy::Once:         return "once";
    case TargetPolicy::Avoid:        return "avoid";
    }
    return "until_covered";
}

struct NamedMarking {
    std::string name;
    SparseMarking tokens;
    MatchMode match = MatchMode::Exact;  // only meaningful for target/error markings

    // Error markings only (ignored for initial and target markings):
    TargetPolicy policy = TargetPolicy::UntilCovered;
    std::optional<TokenCount> targetHits;  // unset: use the global coverage threshold
    DumpMode crashDump = DumpMode::None;
    std::uint32_t maxDumps = 1;  // dumps to collect over all runs; relevant if crashDump != None

    // GLM-5.3-Flash: a target marking can be intentional ("good target state").
    // Such states are normal targets during testing, but the moment the SUT is
    // certainly in one the campaign ends -- that marking *is* the campaign's
    // purpose. Non-intentional target markings stay what they were: idle
    // states used by the livelock analysis that never end a run.
    bool intentional = false;

    // GLM-5.3-Flash: the marking may crash the SUT nondeterministically while
    // it is certainly in this marking. Same experiment as on a transition:
    // a run that certainly reaches the marking is one attempt, a run that
    // dies while certainly in it is a hit. Works on any named marking
    // (initial, target or error). A marking with policy "avoid" cannot carry
    // it (the loader rejects that): what the tester avoids cannot be sought.
    std::optional<CrashPrediction> crash;
};

// Definite visits after which an error marking counts as satisfied (and the
// tester starts avoiding it). 0 means "satisfied from the start".
inline std::uint32_t effectiveTargetHits(const NamedMarking &m, std::uint32_t globalThreshold)
{
    switch (m.policy) {
    case TargetPolicy::Once:
        return 1;
    case TargetPolicy::Avoid:
        return 0;
    case TargetPolicy::UntilCovered:
        break;
    }
    return m.targetHits.value_or(globalThreshold);
}

struct Net {
    std::string name;
    std::vector<Place> places;
    std::vector<Transition> transitions;

    std::vector<NamedMarking> initialMarkings;  // at least one
    std::vector<NamedMarking> targetMarkings;    // legitimate end states
    std::vector<NamedMarking> errorMarkings;    // error wins over target if both match

    std::unordered_map<std::string, PlaceIndex> placeIndex;
    std::unordered_map<std::string, TransitionIndex> transitionIndex;

    std::optional<PlaceIndex> findPlace(const std::string &id) const {
        auto it = placeIndex.find(id);
        if (it == placeIndex.end()) return std::nullopt;
        return it->second;
    }

    std::optional<TransitionIndex> findTransition(const std::string &id) const {
        auto it = transitionIndex.find(id);
        if (it == transitionIndex.end()) return std::nullopt;
        return it->second;
    }

    // GLM-5.3-Flash: all named markings that carry a crash prediction, in the
    // order error, target, then initial markings -- error wins over target if
    // both match, the same rule the net's markings follow. A death of the SUT
    // while it is certainly in one of them is attributed to that prediction.
    std::vector<const NamedMarking *> crashMarkings() const {
        std::vector<const NamedMarking *> out;
        for (const auto *list : {&errorMarkings, &targetMarkings, &initialMarkings})
            for (const NamedMarking &m : *list)
                if (m.crash) out.push_back(&m);
        return out;
    }
};

}  // namespace pn
