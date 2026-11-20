#include "engine/CoverageDb.h"

#include <algorithm>
#include <cassert>

namespace pn {

namespace {
const ElementCoverage kEmptyCoverage;
}

CoverageDb::CoverageDb(const ReachabilityGraph &rg, CoverageConfig cfg)
    : rg_(rg),
      cfg_(cfg),
      transitionHits_(rg.net().transitions.size(), 0),
      rareAttempts_(rg.net().transitions.size(), 0),  // DeepSeek V4.1 Flash: the rare hunt
      placeVisits_(rg.net().places.size(), 0),
      errorHits_(rg.net().errorMarkings.size(), 0),
      targetHits_(rg.net().targetMarkings.size(), 0),
      errorDumps_(rg.net().errorMarkings.size(), 0),
      errorTransDumps_(rg.net().transitions.size(), 0)  // GLM-5.3-Flash
{
    // GLM-5.3-Flash: seed the crash experiments from the net. Every predicted
    // transition and every predicted marking starts its experiment with zero
    // counts; implicit entries appear later, at their first attributed crash.
    for (std::size_t t = 0; t < rg_.net().transitions.size(); ++t)
        if (rg_.net().transitions[t].crashPrediction)
            addCrashEntry("t:" + rg_.net().transitions[t].id,
                          static_cast<TransitionIndex>(t), rg_.net().transitions[t].crashPrediction);
    for (const NamedMarking *m : rg_.net().crashMarkings())
        addCrashEntry("m:" + m->name, kNoTransition, m->crash);
}

void CoverageDb::ensureSized()
{
    if (stateCov_.size() < rg_.numStates())
        stateCov_.resize(rg_.numStates());
    if (edgeCov_.size() < rg_.numEdges())
        edgeCov_.resize(rg_.numEdges());
}

void CoverageDb::touch(ElementCoverage &c, std::uint32_t &newCounter)
{
    const RunId run = runs_.back().id;
    if (c.definite == 0) {
        c.firstNode = node_;
        c.firstRun = run;
        ++newCounter;
    }
    ++c.definite;
    c.lastNode = node_;
    c.lastRun = run;
}

void CoverageDb::onStateVisited(StateId s)
{
    RunRecord &run = runs_.back();
    run.states.push_back(s);  // GLM-5.3-Flash: per-run attribution for the overlap analysis
    touch(stateCov_[s], run.newStates);

    const TokenCount *m = rg_.tokens(s);
    for (std::size_t p = 0; p < placeVisits_.size(); ++p)
        if (m[p] > 0)
            ++placeVisits_[p];

    const int idx = rg_.markingIndex(s);
    switch (rg_.kind(s)) {
    case StateKind::Error:
        assert(idx >= 0);
        ++errorHits_[static_cast<std::size_t>(idx)];
        break;
    case StateKind::Target:
        assert(idx >= 0);
        ++targetHits_[static_cast<std::size_t>(idx)];
        break;
    case StateKind::Normal:
        break;
    }
}

RunId CoverageDb::beginRun(StateId initial, std::string note, bool creditInitial)
{
    assert(initial < rg_.numStates());
    if (open_)
        endRun(RunOutcome::Aborted, "superseded by a new run");
    ensureSized();

    RunRecord run;
    run.id = static_cast<RunId>(runs_.size());
    run.initial = initial;
    run.note = std::move(note);
    runs_.push_back(std::move(run));

    open_ = true;
    current_ = initial;
    if (creditInitial) {
        node_ = tree_.child(kRootNode, kNoEdge, initial);
        onStateVisited(initial);
    } else {
        node_ = kRootNode;
    }
    return runs_.back().id;
}

bool CoverageDb::step(EdgeId e)
{
    if (!open_ || !rg_.isExpanded(current_))
        return false;
    const EdgeId first = rg_.firstEdge(current_);
    const std::size_t count = rg_.successors(current_).size();
    if (e < first || static_cast<std::size_t>(e) >= static_cast<std::size_t>(first) + count)
        return false;

    ensureSized();
    const Edge &edge = rg_.edge(e);
    RunRecord &run = runs_.back();

    node_ = tree_.child(node_, e, edge.target);
    touch(edgeCov_[e], run.newEdges);
    ++transitionHits_[edge.transition];
    run.transitions.push_back(edge.transition);  // GLM-5.3-Flash
    run.steps.push_back({e, edge.target});
    current_ = edge.target;
    onStateVisited(edge.target);
    return true;
}

void CoverageDb::endRun(RunOutcome outcome, std::string detail)
{
    if (!open_)
        return;
    RunRecord &run = runs_.back();
    run.outcome = outcome;
    run.detail = std::move(detail);
    open_ = false;
    current_ = kNoState;
    node_ = kRootNode;
}

void CoverageDb::recordDump(DumpMode mode, std::vector<std::string> files, std::string stackText)
{
    int idx = -1;
    if (open_ && rg_.kind(current_) == StateKind::Error)
        idx = rg_.markingIndex(current_);
    recordDump(mode, std::move(files), std::move(stackText), idx);
}

void CoverageDb::recordDump(DumpMode mode, std::vector<std::string> files, std::string stackText,
                            int errorMarking)
{
    recordDump(mode, std::move(files), std::move(stackText), errorMarking, kNoTransition);
}

void CoverageDb::recordDump(DumpMode mode, std::vector<std::string> files, std::string stackText,
                            int errorMarking, TransitionIndex errorTransition)
{
    if (!open_)
        return;
    RunRecord &run = runs_.back();

    DumpRecord rec;
    // Event-driven runs count events, path-driven runs count steps.
    rec.stepIndex = static_cast<std::uint32_t>(run.events.empty() ? run.steps.size() : run.events.size());
    rec.mode = mode;
    rec.files = std::move(files);
    rec.stackText = std::move(stackText);
    if (errorMarking >= 0) {
        const auto idx = static_cast<std::size_t>(errorMarking);
        rec.marking = rg_.net().errorMarkings[idx].name;
        ++errorDumps_[idx];
    } else if (errorTransition != kNoTransition) {
        // GLM-5.3-Flash: a dump taken for a certain error transition counts
        // toward that transition's max_dumps.
        const auto idx = static_cast<std::size_t>(errorTransition);
        if (idx < errorTransDumps_.size())
            ++errorTransDumps_[idx];
        rec.transition = errorTransition;
    }
    run.dumps.push_back(std::move(rec));
}

void CoverageDb::creditDefinite(StateId state, EdgeId via, bool countTransition)
{
    if (!open_)
        return;
    ensureSized();
    RunRecord &run = runs_.back();

    node_ = tree_.child(node_, via, state);
    if (via != kNoEdge) {
        touch(edgeCov_[via], run.newEdges);
        if (countTransition) {
            ++transitionHits_[rg_.edge(via).transition];
            run.transitions.push_back(rg_.edge(via).transition);  // GLM-5.3-Flash
        }
        run.steps.push_back({via, state});
    }
    current_ = state;
    onStateVisited(state);
}

void CoverageDb::creditTransition(TransitionIndex t)
{
    if (open_) {
        ++transitionHits_[t];
        runs_.back().transitions.push_back(t);  // GLM-5.3-Flash
    }
}

void CoverageDb::recordEvent(const SutEvent &ev)
{
    if (open_)
        runs_.back().events.push_back(ev);
}

// GLM-5.3-Flash: the offered/taken statistics are global, not per run: the
// campaign accumulates how often each SUT move was offered and taken, so the
// rates stabilise as the campaign goes on.
void CoverageDb::recordOffered(const std::string &label)
{
    ++moveStats_[label].offered;
}

void CoverageDb::recordTaken(const std::string &label)
{
    ++moveStats_[label].taken;
}

double CoverageDb::takeRate(const std::string &label) const
{
    const auto it = moveStats_.find(label);
    if (it == moveStats_.end() || it->second.offered == 0)
        return 1.0;  // never offered: no evidence, stay neutral
    return std::min(1.0, static_cast<double>(it->second.taken)
                            / static_cast<double>(it->second.offered));
}

void CoverageDb::addAmbiguousState(StateId s)
{
    ensureSized();
    if (s < stateCov_.size())
        ++stateCov_[s].ambiguous;
}

void CoverageDb::addAmbiguousEdge(EdgeId e)
{
    ensureSized();
    if (e < edgeCov_.size())
        ++edgeCov_[e].ambiguous;
}

const ElementCoverage &CoverageDb::state(StateId s) const
{
    return s < stateCov_.size() ? stateCov_[s] : kEmptyCoverage;
}

const ElementCoverage &CoverageDb::edge(EdgeId e) const
{
    return e < edgeCov_.size() ? edgeCov_[e] : kEmptyCoverage;
}

std::uint32_t CoverageDb::errorTarget(std::size_t i) const
{
    return effectiveTargetHits(rg_.net().errorMarkings[i], cfg_.threshold);
}

bool CoverageDb::dumpAllowed(std::size_t i) const
{
    const NamedMarking &m = rg_.net().errorMarkings[i];
    return m.crashDump != DumpMode::None && errorDumps_[i] < m.maxDumps;
}

// GLM-5.3-Flash: mirror of dumpAllowed for error transitions.
bool CoverageDb::errorTransitionDumpAllowed(TransitionIndex t) const
{
    if (t >= rg_.net().transitions.size())
        return false;
    const Transition &tr = rg_.net().transitions[t];
    return tr.error && tr.crashDump != DumpMode::None
           && errorTransitionDumps(t) < tr.maxDumps;
}

// --- crash predictions (GLM-5.3-Flash) -----------------------------------------

void CoverageDb::addCrashEntry(std::string key, TransitionIndex transition,
                               std::optional<CrashPrediction> prediction)
{
    if (crashIndex_.count(key))
        return;  // idempotent: an implicit creation reuses what is already there
    CrashEntry e;
    e.key = std::move(key);
    e.transition = transition;
    e.predicted = prediction.has_value();
    e.prediction = std::move(prediction);
    crashIndex_.emplace(e.key, crashEntries_.size());
    crashEntries_.push_back(std::move(e));
}

const CrashEntry *CoverageDb::findCrashEntry(const std::string &key) const
{
    const auto it = crashIndex_.find(key);
    return it == crashIndex_.end() ? nullptr : &crashEntries_[it->second];
}

// The calibration an entry evaluates itself with: the probable class carries
// prior and budget, the explicit "p"/"attempts" override them, and an
// implicit entry uses the neutral prior and the --crash-attempts budget.
CrashStatus CoverageDb::evaluateCrashStatus(const CrashEntry &e) const
{
    CrashCalibration cal;
    if (e.prediction) {
        cal = classCalibration(e.prediction->crashClass);
        if (e.prediction->p) cal.priorP = *e.prediction->p;
        if (e.prediction->attempts && *e.prediction->attempts > 0)  // GLM-5.3-Flash: 0 = no explicit budget
            cal.giveUpAfter = *e.prediction->attempts;
    } else {
        cal = CrashCalibration{0.5, cfg_.crashAttempts};
    }
    return crashStatus(e.stats, cal, cfg_.crashLevel);
}

void CoverageDb::noteCrashAttempt(const std::string &key)
{
    const auto it = crashIndex_.find(key);
    if (it == crashIndex_.end())
        return;
    CrashEntry &e = crashEntries_[it->second];
    if (e.status != CrashStatus::Seeking)
        return;  // resolved: the experiment is over, nothing counts anymore
    ++e.stats.attempts;
    e.status = evaluateCrashStatus(e);
    if (open_)
        runs_.back().crashAttempts.push_back(key);  // GLM-5.3-Flash: the run's experiment trail
}

void CoverageDb::recordCrashAttribution(std::string key)
{
    if (!open_)
        return;
    runs_.back().crashAttribution = std::move(key);
}

// GLM-5.3-Flash: restore/replay adopts the recorded hook executions (see
// RunLog.h): the evidence travels with the run, the commands are not
// re-executed.
void CoverageDb::adoptHookRuns(RunId id, std::vector<HookRecord> hooks)
{
    if (id < runs_.size())
        runs_[id].hookRuns = std::move(hooks);
}

// GLM-5.3-Flash: one executed command hook, appended to the open run's
// record in firing order.
void CoverageDb::recordHookRun(HookRecord rec)
{
    if (!open_)
        return;
    runs_.back().hookRuns.push_back(std::move(rec));
}

void CoverageDb::noteCrash(const std::string &key)
{
    const auto it = crashIndex_.find(key);
    if (it == crashIndex_.end())
        return;
    CrashEntry &e = crashEntries_[it->second];
    if (e.status != CrashStatus::Seeking)
        return;
    ++e.stats.crashes;
    e.status = evaluateCrashStatus(e);
}

// The prediction's crash_dump request with its own max_dumps budget
// (mirror of dumpAllowed). An implicit entry takes no post-mortem.
bool CoverageDb::crashDumpAllowed(const std::string &key) const
{
    const CrashEntry *e = findCrashEntry(key);
    return e && e->prediction && e->prediction->crashDump != DumpMode::None
           && e->dumps < e->prediction->maxDumps;
}

void CoverageDb::recordCrashDump(const std::string &key, DumpMode mode,
                                 std::vector<std::string> files, std::string stackText)
{
    if (!open_)
        return;
    const auto it = crashIndex_.find(key);
    if (it == crashIndex_.end())
        return;
    CrashEntry &e = crashEntries_[it->second];
    ++e.dumps;
    RunRecord &run = runs_.back();
    DumpRecord rec;
    // Event-driven runs count events, path-driven runs count steps.
    rec.stepIndex = static_cast<std::uint32_t>(run.events.empty() ? run.steps.size() : run.events.size());
    rec.mode = mode;
    rec.files = std::move(files);
    rec.stackText = std::move(stackText);
    // The dump belongs to the crash entry: a marking entry by its marking
    // name (any list -- the names are unique across the net), a transition
    // entry by its transition index.
    if (e.transition == kNoTransition)
        rec.marking = e.key.substr(2);  // strip "m:"
    else
        rec.transition = e.transition;
    run.dumps.push_back(std::move(rec));
}

CoverageSummary CoverageDb::summary() const
{
    CoverageSummary s;
    s.statesKnown = rg_.numStates();
    s.edgesKnown = rg_.numEdges();
    s.statesExpanded = rg_.numExpanded();
    for (const ElementCoverage &c : stateCov_)
        if (c.definite > 0)
            ++s.statesCovered;
    for (const ElementCoverage &c : edgeCov_)
        if (c.definite > 0)
            ++s.edgesCovered;
    s.transitionsTotal = transitionHits_.size();
    for (std::uint64_t h : transitionHits_)
        if (h > 0)
            ++s.transitionsCovered;
    s.placesTotal = placeVisits_.size();
    for (std::uint64_t v : placeVisits_)
        if (v > 0)
            ++s.placesCovered;
    return s;
}

std::vector<TraceStep> CoverageDb::stateBacktrace(StateId s, Which which, std::size_t maxSteps) const
{
    const ElementCoverage &c = state(s);
    const NodeId n = (which == Which::First) ? c.firstNode : c.lastNode;
    return n == kNoNode ? std::vector<TraceStep>{} : tree_.path(n, maxSteps);
}

std::vector<TraceStep> CoverageDb::edgeBacktrace(EdgeId e, Which which, std::size_t maxSteps) const
{
    const ElementCoverage &c = edge(e);
    const NodeId n = (which == Which::First) ? c.firstNode : c.lastNode;
    return n == kNoNode ? std::vector<TraceStep>{} : tree_.path(n, maxSteps);
}

}  // namespace pn
