// Follows a live SUT through the reachability graph when not everything the
// SUT does is visible.
//
// The tracker keeps a BELIEF SET: every RG state the SUT may be in, given the
// events seen so far. The set is always closed under SILENT internal
// transitions (internal transitions without a label, which are never
// reported), so after "SUT said X" the tracker knows both where X can lead and
// everything that may have happened silently afterwards.
//
//   Observed(label)    keep the successors, via observable or traced-internal
//                      transitions with that label, of the states where one is
//                      enabled; none enabled anywhere is a DEVIATION.
//   Stimulated(label)  same with controllable transitions. If none is enabled
//                      in any possible state the stimulus was PREEMPTED: a
//                      silent step got there first and the model says nothing
//                      about the SUT's reaction. The belief is left unchanged.
//   Quiescent          the SUT stayed silent for the quiescence timeout (the
//                      plug-in decides when; the tracker has no clock for it). Allowed in the
//                      states with no enabled output or internal transition;
//                      the belief narrows to those. None is a DEVIATION
//                      (missing output). If all remaining states are non-target,
//                      non-error dead ends the SUT is DEADLOCKED.
//
// RUN-TIME LIVELOCK (kind 2; the static kind is in LivelockAnalysis). With
// TrackerConfig::livelockTimeoutMs > 0 the tracker watches for a SUT that
// keeps producing events without making progress. PROGRESS is any of:
//   * the tester sends a stimulus,
//   * the belief contains an RG state not seen earlier in this run,
//   * the belief may be in a target marking,
//   * the SUT falls quiet.
// If at least `livelockMinEvents` observed events in a row bring no progress
// and the time since the last progress is >= the timeout, the event is
// reported as Livelock. This is a heuristic: choose the timeout longer than
// any legitimate autonomous activity of the SUT. Time comes from the events'
// timestamps, so replaying a recorded run gives the same verdicts.
//
// COVERAGE. Crediting a run to the CoverageDb needs to know the path the SUT
// really took, which ambiguity hides. The tracker therefore keeps the layers
// of belief sets and, when the belief collapses to one state (and at the end
// of the run), builds the graph of all paths consistent with the observations
// and computes dominators from its start to its end:
//   * elements on EVERY consistent path are credited as definite visits;
//   * elements on SOME but not all are credited as ambiguous;
//   * elements on no complete consistent path (dead ends the later events
//     ruled out) are not credited at all.
// Definiteness is monotone, so crediting at a collapse point is definitive.
// Observed/stimulated events whose edge is ambiguous but whose transition is
// certain still credit that transition.
//
// Not thread-safe. The graph is expanded lazily as the tracker needs it.
#pragma once

#include "engine/CoverageDb.h"
#include "engine/Events.h"
#include "engine/ReachabilityGraph.h"

#include <cstddef>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace pn {

// DeepSeek V4.1 Flash: the lifecycle gate (doku section 26). It says what the
// campaign may send next, not where the model is: while the SUT is paused or
// down, ordinary inputs would reach a system that cannot answer, so only the
// lifecycle inputs that bring it back are offered, and quiescence is expected
// rather than a fault.
enum class LifecycleGate {
    Running,  // ordinary suggestions; the SUT answers inputs
    Paused,   // only resume, stop or restart; silence is expected
    Down      // only start or restart; the SUT is not running
};

struct TrackerConfig {
    // 0 disables run-time livelock detection.
    std::int64_t livelockTimeoutMs = 0;
    // Guard against a single slow answer: this many consecutive observed
    // events without progress are needed before a livelock is declared.
    std::size_t livelockMinEvents = 3;
};

enum class StepStatus {
    Ok,
    Deviation,  // the SUT did, or failed to do, something the model does not allow
    Preempted,  // a stimulus that is not enabled in any possible state (not the SUT's fault)
    Deadlock,   // quiescent in a model deadlock
    Livelock    // run-time livelock; unlike Deviation the event WAS applied to the belief
};

struct StepResult {
    StepStatus status = StepStatus::Ok;
    std::string message;
    // Labels the model would have allowed at this point (sorted, unique):
    // outputs/traced steps for Observed and Quiescent, inputs for Stimulated.
    std::vector<std::string> expected;
    // True if the mismatch may be an artifact of the exploration: an enabled
    // matching transition whose edge was dropped (state limit / bound), or an
    // incomplete state in the belief.
    bool modelIncomplete = false;
};

struct ControllableCandidate {
    TransitionIndex transition;
    std::size_t enabledIn;  // in how many of the belief's states it is enabled
};

class BeliefTracker {
public:
    // `db` may be null: the tracker then only tracks (used for dry runs).
    explicit BeliefTracker(ReachabilityGraph &rg, CoverageDb *db = nullptr, TrackerConfig config = {});

    BeliefTracker(const BeliefTracker &) = delete;
    BeliefTracker &operator=(const BeliefTracker &) = delete;

    // --- run lifecycle -------------------------------------------------------

    // Starts a run in net.initialMarkings[initialMarkingIndex]. A run still in
    // progress is ended as Aborted.
    void beginRun(std::size_t initialMarkingIndex, std::string note = {});

    // Feeds one event. The event is logged in the CoverageDb even if it turns
    // out to be a deviation. After a non-Ok status the belief is unchanged
    // (Quiescent+Deadlock narrows it), and what to do next is up to the caller.
    StepResult onEvent(const SutEvent &ev);

    // Credits whatever is still pending and closes the CoverageDb run.
    void endRun(RunOutcome outcome, std::string detail = {});

    bool runActive() const { return active_; }
    std::size_t eventCount() const { return eventCount_; }

    // --- what is known now ---------------------------------------------------

    const std::vector<StateId> &belief() const { return belief_; }  // sorted
    bool isDefinite() const { return belief_.size() == 1; }

    // The error marking the SUT is certainly in (all possible states match the
    // same one), if any.
    std::optional<std::size_t> definiteErrorMarking() const;
    bool possiblyInError() const;
    bool possiblyTarget() const;

    // GLM-5.3-Flash: the target marking the SUT is certainly in (all possible
    // states match the same one), if any. Mirror of definiteErrorMarking().
    std::optional<std::size_t> definiteTargetMarking() const;

    // GLM-5.3-Flash: the transition the last event certainly stepped (every
    // possible state advanced by the very same transition, nothing dropped),
    // when that transition is tagged as an error. nullopt otherwise.
    std::optional<TransitionIndex> definiteErrorTransition() const;

    // GLM-5.3-Flash: the transition the last event certainly stepped, when
    // that transition is tagged as a milestone. Same certainty rule as
    // definiteErrorTransition(): a certain STEP (the transition is known),
    // not necessarily a certain state -- a milestone may be certain while the
    // belief still spans several states, and the session then deposits at
    // every one of them. nullopt otherwise.
    std::optional<TransitionIndex> definiteMilestoneTransition() const;

    // GLM-5.3-Flash: the transition the last event certainly stepped, WHATEVER
    // tag it carries -- the raw certain step. This is "the last credited
    // step's transition" the crash attribution asks for: the step in flight
    // when the SUT died. Unlike definiteErrorTransition() above it does not
    // filter by a tag, and unlike the coverage record it does not wait for
    // the step's settle: the certainty is known the moment the event is
    // processed.
    std::optional<TransitionIndex> definiteStep() const { return certainStep_; }

    // GLM-5.3-Flash: the command-carrying transitions a resolved stretch
    // RULED OUT -- an event edge that cannot reach the final belief: the SUT
    // did not take that step, although it was a suspect while the stretch was
    // ambiguous (Egge: "suspect steps which do not execute need to be
    // reported"). Deduped per run; the session reports those whose command
    // never ran anywhere in the run. Cleared at every beginRun.
    const std::vector<TransitionIndex> &ruledOutCommands() const { return ruledOutCommands_; }

    // Inputs the tester could send now, with how many possible states enable
    // each (enabledIn == belief().size() means a safe choice). DeepSeek V4.1
    // Flash: while the lifecycle gate is not Running, only the lifecycle inputs
    // it allows are listed; an engine-landed lifecycle input (no post arcs) is
    // never listed while Running, because it is not an ordinary model step.
    std::vector<ControllableCandidate> enabledControllable() const;

    // DeepSeek V4.1 Flash: the lifecycle gate (doku section 26) and the input it
    // wants next (kNoTransition: the model offers none, so the run is stuck).
    LifecycleGate lifecycleGate() const { return gate_; }
    TransitionIndex suggestedLifecycle() const;

    // Labels of outputs / traced steps that may come next.
    std::vector<std::string> expectedLabels() const;

    // Attaches a post-mortem to the run. Counts toward max_dumps of the error
    // marking the SUT is certainly in (none if it is not certain).
    void recordDump(DumpMode mode, std::vector<std::string> files, std::string stackText = {});

private:
    struct EventEdge {
        EdgeId edge;
        StateId from;
        StateId to;
    };
    // One layer = the belief after one event (layer 0: after the initial closure).
    struct Layer {
        bool identity = false;              // quiescence: same states, no new visits
        std::vector<EventEdge> eventEdges;  // edges of the event, from the previous layer
        std::vector<EventEdge> silentEdges; // silent steps taken inside this layer
        std::vector<StateId> states;        // the belief, sorted
    };

    struct Applied {
        std::vector<EventEdge> eventEdges;
        std::vector<StateId> targets;
    };

    std::vector<StateId> closure(const std::vector<StateId> &seeds, std::vector<EventEdge> &silentEdges);
    Applied applyMatching(const std::vector<TransitionIndex> &matching);
    StepResult handleLabelEvent(const SutEvent &ev, bool stimulus);
    // DeepSeek V4.1 Flash: one stimulating event on the matching transitions,
    // including the layer and the certainty rule (shared by ordinary inputs and
    // arc-driven lifecycle inputs).
    StepResult applyStimulusMatching(const SutEvent &ev, bool stimulus,
                                     const std::vector<TransitionIndex> &matching);
    // DeepSeek V4.1 Flash: a stimulus whose label is a reserved lifecycle word
    // (doku section 26).
    StepResult handleLifecycle(const SutEvent &ev, const std::vector<TransitionIndex> &matching);
    // DeepSeek V4.1 Flash: the engine's landing for a lifecycle input without
    // post arcs -- credit the path so far, then continue the same run from
    // `state` (a definite visit, reached without an RG edge) with `via`
    // credited as the step that brought the SUT there.
    void landAt(StateId state, TransitionIndex via);
    StepResult handleQuiescence();
    StepResult failure(StepStatus status, std::string message, bool outputSide,
                       const std::vector<TransitionIndex> *matching);

    bool anyEnabled(StateId s, bool onlyOutputAndInternal) const;
    bool enabledButDropped(const std::vector<TransitionIndex> &matching) const;

    void noteProgress(std::int64_t t);
    void markVisited(bool &fresh);
    void maybeSettle();
    void settle(bool inTarget);
    void creditSegment();

    bool mark(StateId s);
    void nextStamp();

    ReachabilityGraph &rg_;
    CoverageDb *db_;
    const Net &net_;
    TrackerConfig cfg_;

    std::vector<char> silent_;  // per transition: internal without a label
    std::unordered_map<std::string, std::vector<TransitionIndex>> observedByLabel_;
    std::unordered_map<std::string, std::vector<TransitionIndex>> stimulatedByLabel_;

    bool active_ = false;
    std::size_t eventCount_ = 0;
    std::vector<StateId> belief_;

    // DeepSeek V4.1 Flash: the run's initial marking (the landing of an
    // engine-landed start/restart), the gate, and the belief a pause suspended
    // (the landing of an engine-landed resume).
    std::size_t initialIndex_ = 0;
    StateId initialState_ = kNoState;
    LifecycleGate gate_ = LifecycleGate::Running;
    std::vector<StateId> pausedBelief_;

    // GLM-5.3-Flash: the transition the most recent label event certainly
    // stepped, if any (see definiteErrorTransition). Reset by quiescence and
    // beginRun: a quiescence step is not a transition step.
    std::optional<TransitionIndex> certainStep_;

    // GLM-5.3-Flash: the command-carrying transitions the resolved stretches
    // ruled out (see ruledOutCommands).
    std::vector<TransitionIndex> ruledOutCommands_;

    std::vector<Layer> segment_;  // layers since the last collapse point
    StateId segmentStart_ = kNoState;
    bool baseCredited_ = false;   // the segment's first state was credited by the previous segment

    // Run-time livelock watch.
    std::vector<char> visited_;          // states in any belief so far in this run
    bool clockStarted_ = false;
    std::int64_t lastEventMs_ = 0;
    std::int64_t lastProgressMs_ = 0;
    std::size_t eventsSinceProgress_ = 0;

    std::vector<std::uint32_t> stamp_;
    std::uint32_t generation_ = 0;
};

}  // namespace pn
