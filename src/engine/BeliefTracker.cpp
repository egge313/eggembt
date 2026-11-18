#include "engine/BeliefTracker.h"

#include <algorithm>
#include <cassert>
#include <set>
#include <utility>

namespace pn {

namespace {

// Cooper-Harvey-Kennedy iterative dominators. Returns idom per node (the
// source is its own idom, nodes unreachable from the source get -1).
std::vector<int> dominators(const std::vector<std::vector<int>> &succ,
                            const std::vector<std::vector<int>> &pred, int source)
{
    const int n = static_cast<int>(succ.size());
    std::vector<int> postorder;
    postorder.reserve(static_cast<std::size_t>(n));
    std::vector<int> po(static_cast<std::size_t>(n), -1);
    std::vector<char> seen(static_cast<std::size_t>(n), 0);

    // Iterative DFS: a long ambiguous stretch must not overflow the call stack.
    std::vector<std::pair<int, std::size_t>> stack;
    stack.push_back({source, 0});
    seen[static_cast<std::size_t>(source)] = 1;
    while (!stack.empty()) {
        const int u = stack.back().first;
        std::size_t &i = stack.back().second;
        if (i < succ[static_cast<std::size_t>(u)].size()) {
            const int v = succ[static_cast<std::size_t>(u)][i++];
            if (!seen[static_cast<std::size_t>(v)]) {
                seen[static_cast<std::size_t>(v)] = 1;
                stack.push_back({v, 0});
            }
        } else {
            po[static_cast<std::size_t>(u)] = static_cast<int>(postorder.size());
            postorder.push_back(u);
            stack.pop_back();
        }
    }

    std::vector<int> idom(static_cast<std::size_t>(n), -1);
    idom[static_cast<std::size_t>(source)] = source;

    const auto intersect = [&](int a, int b) {
        while (a != b) {
            while (po[static_cast<std::size_t>(a)] < po[static_cast<std::size_t>(b)])
                a = idom[static_cast<std::size_t>(a)];
            while (po[static_cast<std::size_t>(b)] < po[static_cast<std::size_t>(a)])
                b = idom[static_cast<std::size_t>(b)];
        }
        return a;
    };

    bool changed = true;
    while (changed) {
        changed = false;
        for (int k = static_cast<int>(postorder.size()) - 1; k >= 0; --k) {
            const int b = postorder[static_cast<std::size_t>(k)];
            if (b == source)
                continue;
            int newIdom = -1;
            for (int p : pred[static_cast<std::size_t>(b)]) {
                if (idom[static_cast<std::size_t>(p)] == -1)
                    continue;
                newIdom = (newIdom == -1) ? p : intersect(p, newIdom);
            }
            if (newIdom != -1 && idom[static_cast<std::size_t>(b)] != newIdom) {
                idom[static_cast<std::size_t>(b)] = newIdom;
                changed = true;
            }
        }
    }
    return idom;
}

std::vector<std::string> toVector(const std::set<std::string> &s)
{
    return std::vector<std::string>(s.begin(), s.end());
}

}  // namespace

// ---------------------------------------------------------------------------
// Construction and bookkeeping
// ---------------------------------------------------------------------------

BeliefTracker::BeliefTracker(ReachabilityGraph &rg, CoverageDb *db, TrackerConfig config)
    : rg_(rg), db_(db), net_(rg.net()), cfg_(config)
{
    silent_.assign(net_.transitions.size(), 0);
    for (std::size_t i = 0; i < net_.transitions.size(); ++i) {
        const Transition &t = net_.transitions[i];
        const auto ti = static_cast<TransitionIndex>(i);
        switch (t.kind) {
        case TransitionKind::Controllable:
            if (!t.label.empty())
                stimulatedByLabel_[t.label].push_back(ti);
            break;
        case TransitionKind::Observable:
            if (!t.label.empty())
                observedByLabel_[t.label].push_back(ti);
            break;
        case TransitionKind::Internal:
            if (!t.traced)  // GLM-5.3-Flash: explicit "traced" flag, not the label
                silent_[i] = 1;
            else
                observedByLabel_[t.label].push_back(ti);  // traced: reported like an output
            break;
        }
    }
}

void BeliefTracker::nextStamp()
{
    if (++generation_ == 0) {
        std::fill(stamp_.begin(), stamp_.end(), 0u);
        generation_ = 1;
    }
}

bool BeliefTracker::mark(StateId s)
{
    if (s >= stamp_.size())
        stamp_.resize(std::max<std::size_t>(rg_.numStates(), std::size_t(s) + 1), 0);
    if (stamp_[s] == generation_)
        return false;
    stamp_[s] = generation_;
    return true;
}

// ---------------------------------------------------------------------------
// Run lifecycle
// ---------------------------------------------------------------------------

void BeliefTracker::beginRun(std::size_t initialMarkingIndex, std::string note)
{
    if (active_)
        endRun(RunOutcome::Aborted, "superseded by a new run");

    const StateId init = rg_.addInitial(initialMarkingIndex);
    if (db_)
        db_->beginRun(init, std::move(note), /*creditInitial=*/false);

    Layer base;
    base.states = closure({init}, base.silentEdges);
    belief_ = base.states;
    segment_.clear();
    segment_.push_back(std::move(base));
    segmentStart_ = init;
    baseCredited_ = false;
    eventCount_ = 0;
    active_ = true;

    visited_.clear();
    clockStarted_ = false;
    lastEventMs_ = 0;
    lastProgressMs_ = 0;
    eventsSinceProgress_ = 0;
    certainStep_.reset();  // GLM-5.3-Flash
    ruledOutCommands_.clear();  // GLM-5.3-Flash: the suspect report is per run
    bool fresh = false;
    markVisited(fresh);

    maybeSettle();
}

void BeliefTracker::endRun(RunOutcome outcome, std::string detail)
{
    if (!active_)
        return;
    settle(true);
    if (db_)
        db_->endRun(outcome, std::move(detail));
    active_ = false;
    belief_.clear();
    segment_.clear();
}

// ---------------------------------------------------------------------------
// Following the SUT
// ---------------------------------------------------------------------------

std::vector<StateId> BeliefTracker::closure(const std::vector<StateId> &seeds,
                                            std::vector<EventEdge> &silentEdges)
{
    nextStamp();
    std::vector<StateId> out;
    for (StateId s : seeds)
        if (mark(s))
            out.push_back(s);
    for (std::size_t i = 0; i < out.size(); ++i) {
        const StateId u = out[i];
        rg_.expand(u);
        EdgeId e = rg_.firstEdge(u);
        for (const Edge &edge : rg_.successors(u)) {
            if (silent_[edge.transition]) {
                silentEdges.push_back({e, u, edge.target});
                if (mark(edge.target))
                    out.push_back(edge.target);
            }
            ++e;
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

BeliefTracker::Applied BeliefTracker::applyMatching(const std::vector<TransitionIndex> &matching)
{
    Applied a;
    nextStamp();
    for (StateId u : belief_) {
        rg_.expand(u);
        EdgeId e = rg_.firstEdge(u);
        for (const Edge &edge : rg_.successors(u)) {
            if (std::find(matching.begin(), matching.end(), edge.transition) != matching.end()) {
                a.eventEdges.push_back({e, u, edge.target});
                if (mark(edge.target))
                    a.targets.push_back(edge.target);
            }
            ++e;
        }
    }
    return a;
}

StepResult BeliefTracker::onEvent(const SutEvent &ev)
{
    if (!active_) {
        StepResult r;
        r.status = StepStatus::Deviation;
        r.message = "no run in progress";
        return r;
    }
    if (db_)
        db_->recordEvent(ev);
    ++eventCount_;

    StepResult result;
    switch (ev.kind) {
    case EventKind::Observed:
        result = handleLabelEvent(ev, false);
        break;
    case EventKind::Stimulated:
        result = handleLabelEvent(ev, true);
        break;
    case EventKind::Quiescent:
        result = handleQuiescence();
        break;
    case EventKind::Died:
        // GLM-5.3-Flash: the SUT is gone. No step to reason about; the session
        // sees the same event and ends the run.
        break;
    }

    // --- run-time livelock watch ---------------------------------------------------
    const std::int64_t t = std::max(ev.timestampMs, lastEventMs_);  // tolerate a clock that steps back
    lastEventMs_ = t;
    if (!clockStarted_) {
        clockStarted_ = true;  // the first event only sets the baseline
        lastProgressMs_ = t;
    }
    switch (ev.kind) {
    case EventKind::Stimulated:
    case EventKind::Quiescent:
        noteProgress(t);
        break;
    case EventKind::Died:
        break;  // GLM-5.3-Flash: the run is over; no progress to note
    case EventKind::Observed:
        if (result.status != StepStatus::Ok)
            break;
        {
            bool fresh = false;
            markVisited(fresh);
            if (fresh || possiblyTarget()) {
                noteProgress(t);
            } else {
                ++eventsSinceProgress_;
                if (cfg_.livelockTimeoutMs > 0 && eventsSinceProgress_ >= cfg_.livelockMinEvents
                    && t - lastProgressMs_ >= cfg_.livelockTimeoutMs) {
                    result.status = StepStatus::Livelock;
                    result.message = std::to_string(eventsSinceProgress_) + " events over "
                                     + std::to_string(t - lastProgressMs_)
                                     + " ms without progress (no new state, no stimulus, no target marking)";
                }
            }
        }
        break;
    }
    return result;
}

void BeliefTracker::noteProgress(std::int64_t t)
{
    lastProgressMs_ = t;
    eventsSinceProgress_ = 0;
}

// Marks every state of the belief as seen; `fresh` is set if one was new.
void BeliefTracker::markVisited(bool &fresh)
{
    for (StateId s : belief_) {
        if (s >= visited_.size())
            visited_.resize(std::max<std::size_t>(rg_.numStates(), std::size_t(s) + 1), 0);
        if (!visited_[s]) {
            visited_[s] = 1;
            fresh = true;
        }
    }
}

StepResult BeliefTracker::handleLabelEvent(const SutEvent &ev, bool stimulus)
{
    // GLM-5.3-Flash: a new event means the previous step's certainty no longer
    // applies; recomputed below for a successful step.
    certainStep_.reset();

    const auto &byLabel = stimulus ? stimulatedByLabel_ : observedByLabel_;
    const auto it = byLabel.find(ev.label);
    if (it == byLabel.end()) {
        if (stimulus)
            return failure(StepStatus::Preempted,
                           "stimulus \"" + ev.label + "\" is not an input of the model", false, nullptr);
        return failure(StepStatus::Deviation,
                       "observed \"" + ev.label + "\", which the model does not know", true, nullptr);
    }

    Applied a = applyMatching(it->second);
    if (a.eventEdges.empty()) {
        if (stimulus)
            return failure(StepStatus::Preempted,
                           "stimulus \"" + ev.label + "\" is not enabled in any possible state; "
                           "a silent step probably got there first",
                           false, &it->second);
        return failure(StepStatus::Deviation,
                       "observed \"" + ev.label + "\", which is not possible in any possible state",
                       true, &it->second);
    }

    // GLM-5.3-Flash: the step is CERTAIN when every possible state advanced by
    // the very same transition and no matching edge was lost to the
    // exploration limits (state cap, place bound). Only then does the tester
    // know exactly which transition fired -- the hook the error-transition
    // tag (Session) builds on. belief_ is still the pre-event belief here.
    if (a.eventEdges.size() == belief_.size() && !enabledButDropped(it->second)) {
        const TransitionIndex fired = rg_.edge(a.eventEdges.front().edge).transition;
        const bool same = std::all_of(a.eventEdges.begin(), a.eventEdges.end(),
                                      [this, fired](const EventEdge &e) {
                                          return rg_.edge(e.edge).transition == fired;
                                      });
        if (same)
            certainStep_ = fired;
    }

    Layer layer;
    layer.eventEdges = std::move(a.eventEdges);
    layer.states = closure(a.targets, layer.silentEdges);
    belief_ = layer.states;
    segment_.push_back(std::move(layer));
    maybeSettle();
    return {};
}

StepResult BeliefTracker::handleQuiescence()
{
    // GLM-5.3-Flash: quiescence is not a transition step, so no step can be
    // certain across it.
    certainStep_.reset();

    std::vector<StateId> quiet;
    for (StateId s : belief_)
        if (!anyEnabled(s, /*onlyOutputAndInternal=*/true))
            quiet.push_back(s);

    if (quiet.empty())
        return failure(StepStatus::Deviation,
                       "no output arrived, but the model requires one in every possible state", true,
                       nullptr);

    Layer layer;
    layer.identity = true;
    layer.states = quiet;
    belief_ = quiet;
    segment_.push_back(std::move(layer));

    bool allDead = true;
    for (StateId s : quiet) {
        if (anyEnabled(s, false) || rg_.kind(s) != StateKind::Normal) {
            allDead = false;
            break;
        }
    }
    maybeSettle();

    if (allDead) {
        StepResult r;
        r.status = StepStatus::Deadlock;
        r.message = "the SUT is quiescent in a state where the model has a deadlock";
        return r;
    }
    return {};
}

bool BeliefTracker::anyEnabled(StateId s, bool onlyOutputAndInternal) const
{
    const TokenCount *m = rg_.tokens(s);
    for (const Transition &t : net_.transitions) {
        if (onlyOutputAndInternal && t.kind == TransitionKind::Controllable)
            continue;
        if (isEnabled(t, m))
            return true;
    }
    return false;
}

bool BeliefTracker::enabledButDropped(const std::vector<TransitionIndex> &matching) const
{
    for (StateId s : belief_) {
        if (!rg_.isExpanded(s))
            continue;
        if (rg_.isIncomplete(s))
            return true;
        const TokenCount *m = rg_.tokens(s);
        for (TransitionIndex t : matching) {
            if (isEnabled(net_.transitions[t], m) && !rg_.findEdge(s, t))
                return true;
        }
    }
    return false;
}

StepResult BeliefTracker::failure(StepStatus status, std::string message, bool outputSide,
                                  const std::vector<TransitionIndex> *matching)
{
    StepResult r;
    r.status = status;
    r.message = std::move(message);
    if (outputSide) {
        r.expected = expectedLabels();
    } else {
        std::set<std::string> labels;
        for (const ControllableCandidate &c : enabledControllable())
            labels.insert(net_.transitions[c.transition].label);
        r.expected = toVector(labels);
    }
    if (matching) {
        r.modelIncomplete = enabledButDropped(*matching);
    } else {
        for (StateId s : belief_)
            if (rg_.isExpanded(s) && rg_.isIncomplete(s))
                r.modelIncomplete = true;
    }
    return r;
}

// ---------------------------------------------------------------------------
// What is known now
// ---------------------------------------------------------------------------

std::optional<std::size_t> BeliefTracker::definiteErrorMarking() const
{
    if (belief_.empty())
        return std::nullopt;
    int index = -1;
    for (StateId s : belief_) {
        if (rg_.kind(s) != StateKind::Error)
            return std::nullopt;
        if (index == -1)
            index = rg_.markingIndex(s);
        else if (index != rg_.markingIndex(s))
            return std::nullopt;
    }
    return static_cast<std::size_t>(index);
}

bool BeliefTracker::possiblyInError() const
{
    return std::any_of(belief_.begin(), belief_.end(),
                       [this](StateId s) { return rg_.kind(s) == StateKind::Error; });
}

bool BeliefTracker::possiblyTarget() const
{
    return std::any_of(belief_.begin(), belief_.end(),
                       [this](StateId s) { return rg_.kind(s) == StateKind::Target; });
}

// GLM-5.3-Flash: mirror of definiteErrorMarking for target markings -- the
// "good target states" whose certain reach ends the campaign.
std::optional<std::size_t> BeliefTracker::definiteTargetMarking() const
{
    if (belief_.empty())
        return std::nullopt;
    int index = -1;
    for (StateId s : belief_) {
        if (rg_.kind(s) != StateKind::Target)
            return std::nullopt;
        if (index == -1)
            index = rg_.markingIndex(s);
        else if (index != rg_.markingIndex(s))
            return std::nullopt;
    }
    return static_cast<std::size_t>(index);
}

// GLM-5.3-Flash: the transition the last event certainly stepped, when that
// transition is tagged as an error. Certain means every possible state
// advanced by the very same transition and nothing was lost to the
// exploration limits (see handleLabelEvent).
std::optional<TransitionIndex> BeliefTracker::definiteErrorTransition() const
{
    if (!certainStep_ || !net_.transitions[*certainStep_].error)
        return std::nullopt;
    return certainStep_;
}

// GLM-5.3-Flash: mirror of definiteErrorTransition for milestone
// transitions -- the session reacts to a certainly stepped milestone with
// positive pheromones instead of ending the run.
std::optional<TransitionIndex> BeliefTracker::definiteMilestoneTransition() const
{
    if (!certainStep_ || !net_.transitions[*certainStep_].milestone)
        return std::nullopt;
    return certainStep_;
}

std::vector<ControllableCandidate> BeliefTracker::enabledControllable() const
{
    std::vector<ControllableCandidate> out;
    for (std::size_t i = 0; i < net_.transitions.size(); ++i) {
        const Transition &t = net_.transitions[i];
        if (t.kind != TransitionKind::Controllable)
            continue;
        std::size_t count = 0;
        for (StateId s : belief_)
            if (isEnabled(t, rg_.tokens(s)))
                ++count;
        if (count > 0)
            out.push_back({static_cast<TransitionIndex>(i), count});
    }
    return out;
}

std::vector<std::string> BeliefTracker::expectedLabels() const
{
    std::set<std::string> labels;
    for (const Transition &t : net_.transitions) {
        const bool visibleOutput =
            t.kind == TransitionKind::Observable || (t.kind == TransitionKind::Internal && t.traced);  // GLM-5.3-Flash
        if (!visibleOutput)
            continue;
        for (StateId s : belief_) {
            if (isEnabled(t, rg_.tokens(s))) {
                labels.insert(t.label);
                break;
            }
        }
    }
    return toVector(labels);
}

void BeliefTracker::recordDump(DumpMode mode, std::vector<std::string> files, std::string stackText)
{
    if (!db_)
        return;
    const auto marking = definiteErrorMarking();
    // GLM-5.3-Flash: when no error MARKING is certain, the dump may still
    // belong to a certain error TRANSITION (the session asks for it right
    // after that step); the db credits whichever is known.
    const TransitionIndex errorTransition =
        marking ? kNoTransition : definiteErrorTransition().value_or(kNoTransition);
    db_->recordDump(mode, std::move(files), std::move(stackText),
                    marking ? static_cast<int>(*marking) : -1, errorTransition);
}

// ---------------------------------------------------------------------------
// Crediting coverage
// ---------------------------------------------------------------------------

void BeliefTracker::maybeSettle()
{
    if (belief_.size() == 1)
        settle(false);
}

void BeliefTracker::settle(bool inTarget)
{
    const bool trivial = segment_.size() == 1 && segment_[0].silentEdges.empty() && baseCredited_;
    if (db_ && !trivial && !segment_.empty())
        creditSegment();

    segment_.clear();
    if (!inTarget) {
        // The belief is a single state: every consistent path passes through
        // it, so the next segment starts there and its first state is already
        // credited.
        Layer base;
        base.states = belief_;
        segment_.push_back(std::move(base));
        segmentStart_ = belief_[0];
        baseCredited_ = true;
    }
}

void BeliefTracker::creditSegment()
{
    enum Kind : std::uint8_t { Source, Sink, StateOcc, EdgeOcc };
    struct Node {
        Kind kind;
        StateId state;  // StateOcc: the state; EdgeOcc: the state the edge leads to
        EdgeId edge;    // EdgeOcc only
        int target;     // EdgeOcc: node index of the target state occurrence
        bool event;     // EdgeOcc: event edge (true) or silent step (false)
        std::size_t layer;
    };

    std::vector<Node> nodes;
    std::vector<std::pair<int, int>> arcs;
    nodes.push_back({Source, kNoState, kNoEdge, -1, false, 0});
    nodes.push_back({Sink, kNoState, kNoEdge, -1, false, 0});

    std::vector<std::vector<int>> layerNodes(segment_.size());
    const auto nodeOf = [&](std::size_t layer, StateId s) {
        const std::vector<StateId> &st = segment_[layer].states;
        const auto it = std::lower_bound(st.begin(), st.end(), s);
        assert(it != st.end() && *it == s);
        return layerNodes[layer][static_cast<std::size_t>(it - st.begin())];
    };

    for (std::size_t i = 0; i < segment_.size(); ++i) {
        const Layer &layer = segment_[i];
        layerNodes[i].resize(layer.states.size());
        if (layer.identity) {
            // Quiescence is not a new visit: reuse the previous occurrences.
            for (std::size_t k = 0; k < layer.states.size(); ++k)
                layerNodes[i][k] = nodeOf(i - 1, layer.states[k]);
            continue;
        }
        for (std::size_t k = 0; k < layer.states.size(); ++k) {
            layerNodes[i][k] = static_cast<int>(nodes.size());
            nodes.push_back({StateOcc, layer.states[k], kNoEdge, -1, false, i});
        }
        for (const EventEdge &ee : layer.eventEdges) {
            const int en = static_cast<int>(nodes.size());
            const int to = nodeOf(i, ee.to);
            nodes.push_back({EdgeOcc, ee.to, ee.edge, to, true, i});
            arcs.push_back({nodeOf(i - 1, ee.from), en});
            arcs.push_back({en, to});
        }
        for (const EventEdge &se : layer.silentEdges) {
            const int en = static_cast<int>(nodes.size());
            const int to = nodeOf(i, se.to);
            nodes.push_back({EdgeOcc, se.to, se.edge, to, false, i});
            arcs.push_back({nodeOf(i, se.from), en});
            arcs.push_back({en, to});
        }
    }

    const int baseNode = nodeOf(0, segmentStart_);
    arcs.push_back({0, baseNode});
    for (StateId s : segment_.back().states)
        arcs.push_back({nodeOf(segment_.size() - 1, s), 1});

    const std::size_t n = nodes.size();
    std::vector<std::vector<int>> succ(n), pred(n);
    for (const auto &a : arcs) {
        succ[static_cast<std::size_t>(a.first)].push_back(a.second);
        pred[static_cast<std::size_t>(a.second)].push_back(a.first);
    }

    // Keep only nodes on a complete consistent path: those that can reach the sink.
    std::vector<char> live(n, 0);
    {
        std::vector<int> work{1};
        live[1] = 1;
        while (!work.empty()) {
            const int u = work.back();
            work.pop_back();
            for (int p : pred[static_cast<std::size_t>(u)]) {
                if (!live[static_cast<std::size_t>(p)]) {
                    live[static_cast<std::size_t>(p)] = 1;
                    work.push_back(p);
                }
            }
        }
    }
    // GLM-5.3-Flash: Egge: "suspect steps which do not execute need to be
    // reported." An event edge that cannot reach the sink was RULED OUT: the
    // SUT did not take that step, although it was a suspect while the stretch
    // was ambiguous. When the edge's transition carries a command, its hook
    // did not run for this step -- the session reports it (once per run and
    // transition, and only when the command never ran anywhere in the run:
    // that check reads the credited stream, which this segment's own credits
    // are part of).
    for (std::size_t v = 2; v < n; ++v) {
        if (live[v] || nodes[v].kind != EdgeOcc)
            continue;
        const TransitionIndex t = rg_.edge(nodes[v].edge).transition;
        if (t >= net_.transitions.size() || !net_.transitions[t].command)
            continue;
        if (std::find(ruledOutCommands_.begin(), ruledOutCommands_.end(), t)
                != ruledOutCommands_.end())
            continue;
        ruledOutCommands_.push_back(t);
    }

    if (!live[0])
        return;  // cannot happen: the belief is never empty

    std::vector<std::vector<int>> lsucc(n), lpred(n);
    for (const auto &a : arcs) {
        if (live[static_cast<std::size_t>(a.first)] && live[static_cast<std::size_t>(a.second)]) {
            lsucc[static_cast<std::size_t>(a.first)].push_back(a.second);
            lpred[static_cast<std::size_t>(a.second)].push_back(a.first);
        }
    }

    const std::vector<int> idom = dominators(lsucc, lpred, 0);
    if (idom[1] == -1)
        return;

    // Dominators of the sink lie on every consistent path.
    std::vector<int> chain;
    for (int v = 1; v != 0; v = idom[static_cast<std::size_t>(v)])
        chain.push_back(v);
    std::reverse(chain.begin(), chain.end());
    chain.pop_back();  // the sink itself

    std::vector<char> isDominator(n, 0);
    for (int v : chain)
        isDominator[static_cast<std::size_t>(v)] = 1;

    // --- definite visits, in path order ----------------------------------------
    std::vector<char> consumed(n, 0);  // a state occurrence credited together with its edge
    for (int v : chain) {
        const Node &node = nodes[static_cast<std::size_t>(v)];
        if (node.kind == EdgeOcc) {
            const Transition &t = net_.transitions[rg_.edge(node.edge).transition];
            const bool silent = t.kind == TransitionKind::Internal && !t.traced;  // GLM-5.3-Flash
            // Event edges: their transition is credited per layer below.
            db_->creditDefinite(node.state, node.edge, /*countTransition=*/silent);
            consumed[static_cast<std::size_t>(node.target)] = 1;
        } else if (node.kind == StateOcc) {
            if (consumed[static_cast<std::size_t>(v)])
                continue;
            if (v == baseNode && baseCredited_)
                continue;
            db_->creditDefinite(node.state, kNoEdge);
        }
    }

    // --- ambiguous visits ---------------------------------------------------------
    for (std::size_t v = 2; v < n; ++v) {
        if (!live[v] || isDominator[v] || consumed[v])
            continue;
        const Node &node = nodes[v];
        if (node.kind == StateOcc)
            db_->addAmbiguousState(node.state);
        else if (node.kind == EdgeOcc)
            db_->addAmbiguousEdge(node.edge);
    }

    // --- transitions that certainly fired ------------------------------------------
    // Every consistent path crosses exactly one event edge per event layer; if
    // all candidates there are the same transition, that transition certainly
    // fired even when it is not certain which edge (from which state) it was.
    std::vector<std::vector<TransitionIndex>> layerTransitions(segment_.size());
    for (std::size_t v = 2; v < n; ++v) {
        if (live[v] && nodes[v].kind == EdgeOcc && nodes[v].event)
            layerTransitions[nodes[v].layer].push_back(rg_.edge(nodes[v].edge).transition);
    }
    for (std::vector<TransitionIndex> &ts : layerTransitions) {
        if (ts.empty())
            continue;
        const bool unique = std::all_of(ts.begin(), ts.end(), [&](TransitionIndex t) { return t == ts[0]; });
        if (unique)
            db_->creditTransition(ts[0]);
    }
}

}  // namespace pn
