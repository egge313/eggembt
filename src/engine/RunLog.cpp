#include "engine/RunLog.h"

#include "engine/BeliefTracker.h"

#include <string>

namespace pn {

const char *toString(RunOutcome o)
{
    switch (o) {
    case RunOutcome::Open:         return "open";
    case RunOutcome::Completed:    return "completed";
    case RunOutcome::Deadlock:     return "deadlock";
    case RunOutcome::Livelock:     return "livelock";
    case RunOutcome::ErrorReached: return "error_reached";
    case RunOutcome::Deviation:    return "deviation";
    case RunOutcome::SutCrashed:   return "sut_crashed";
    case RunOutcome::Aborted:      return "aborted";
    }
    return "aborted";
}

bool parseOutcome(const std::string &s, RunOutcome &out)
{
    static const RunOutcome all[] = {RunOutcome::Completed, RunOutcome::Deadlock,
                                     RunOutcome::Livelock,  RunOutcome::ErrorReached,
                                     RunOutcome::Deviation, RunOutcome::SutCrashed,
                                     RunOutcome::Aborted};
    for (RunOutcome o : all) {
        if (s == toString(o)) {
            out = o;
            return true;
        }
    }
    return false;
}

const char *toString(DumpMode m)
{
    switch (m) {
    case DumpMode::None:     return "none";
    case DumpMode::Snapshot: return "snapshot";
    case DumpMode::Crash:    return "crash";
    }
    return "none";
}

bool parseDumpMode(const std::string &s, DumpMode &out)
{
    for (DumpMode m : {DumpMode::None, DumpMode::Snapshot, DumpMode::Crash}) {
        if (s == toString(m)) {
            out = m;
            return true;
        }
    }
    return false;
}

PersistedRun toPersisted(const ReachabilityGraph &rg, const RunRecord &run)
{
    const Net &net = rg.net();
    PersistedRun pr;
    pr.outcome = run.outcome == RunOutcome::Open ? RunOutcome::Aborted : run.outcome;
    pr.detail = run.detail;
    pr.note = run.note;
    pr.dumps = run.dumps;
    pr.crashAttribution = run.crashAttribution;  // GLM-5.3-Flash
    pr.crashAttempts = run.crashAttempts;        // GLM-5.3-Flash
    pr.hooks = run.hookRuns;                     // GLM-5.3-Flash

    const Marking start = rg.marking(run.initial);
    for (const NamedMarking &m : net.initialMarkings) {
        if (toDense(net, m.tokens) == start) {
            pr.initialMarking = m.name;
            break;
        }
    }
    if (!run.events.empty()) {
        pr.events = run.events;  // the steps are a by-product of tracking; the events are the record
    } else {
        for (const RunStep &s : run.steps)
            pr.transitions.push_back(net.transitions[rg.edge(s.edge).transition].id);
    }
    return pr;
}

namespace {
ReplayResult fail(std::string message)
{
    ReplayResult r;
    r.error = std::move(message);
    return r;
}
}  // namespace

namespace {

ReplayResult replayEvents(CoverageDb &db, ReachabilityGraph &rg, const PersistedRun &pr,
                          std::size_t initialIndex, const TrackerConfig &config)
{
    std::uint32_t previous = 0;
    for (const DumpRecord &d : pr.dumps) {
        if (d.stepIndex < previous || d.stepIndex > pr.events.size())
            return fail("dump records must have ascending event indices within the run");
        previous = d.stepIndex;
    }

    // Dry run without a database: a run may only end at a Deviation or
    // Deadlock with its last event. (A Preempted stimulus does not end a run.)
    {
        BeliefTracker dry(rg, nullptr, config);
        dry.beginRun(initialIndex);
        for (std::size_t k = 0; k < pr.events.size(); ++k) {
            const StepResult r = dry.onEvent(pr.events[k]);
            const bool ends = r.status == StepStatus::Deviation || r.status == StepStatus::Deadlock
                              || r.status == StepStatus::Livelock;
            if (ends && k + 1 != pr.events.size())
                return fail("event " + std::to_string(k) + ": replay diverged from the recording ("
                            + r.message + ")");
        }
        dry.endRun(RunOutcome::Aborted);
    }

    BeliefTracker tracker(rg, &db, config);
    tracker.beginRun(initialIndex, pr.note);
    ReplayResult result;
    result.run = static_cast<RunId>(db.numRuns() - 1);

    std::size_t nextDump = 0;
    const auto flushDumps = [&](std::size_t eventsDone) {
        while (nextDump < pr.dumps.size() && pr.dumps[nextDump].stepIndex == eventsDone) {
            const DumpRecord &d = pr.dumps[nextDump++];
            tracker.recordDump(d.mode, d.files, d.stackText);
        }
    };
    flushDumps(0);
    for (std::size_t k = 0; k < pr.events.size(); ++k) {
        tracker.onEvent(pr.events[k]);
        flushDumps(k + 1);
    }
    tracker.endRun(pr.outcome, pr.detail);
    db.adoptHookRuns(result.run, pr.hooks);  // GLM-5.3-Flash: records travel, commands do not re-run
    result.ok = true;
    return result;
}

}  // namespace

ReplayResult replay(CoverageDb &db, ReachabilityGraph &rg, const PersistedRun &pr,
                    const TrackerConfig &config)
{
    const Net &net = rg.net();

    // --- locate the initial marking -----------------------------------------
    std::size_t initialIndex = net.initialMarkings.size();
    for (std::size_t i = 0; i < net.initialMarkings.size(); ++i) {
        if (net.initialMarkings[i].name == pr.initialMarking) {
            initialIndex = i;
            break;
        }
    }
    if (initialIndex == net.initialMarkings.size())
        return fail("unknown initial marking \"" + pr.initialMarking + "\"");

    if (!pr.events.empty())
        return replayEvents(db, rg, pr, initialIndex, config);

    // --- validate dumps ------------------------------------------------------
    std::uint32_t previous = 0;
    for (const DumpRecord &d : pr.dumps) {
        if (d.stepIndex < previous || d.stepIndex > pr.transitions.size())
            return fail("dump records must have ascending step indices within the run");
        previous = d.stepIndex;
    }

    // --- validate the path without touching the db -----------------------------
    const StateId initial = rg.addInitial(initialIndex);
    std::vector<EdgeId> edges;
    edges.reserve(pr.transitions.size());
    StateId cur = initial;
    for (std::size_t k = 0; k < pr.transitions.size(); ++k) {
        const std::string &tid = pr.transitions[k];
        const auto t = net.findTransition(tid);
        if (!t)
            return fail("step " + std::to_string(k) + ": unknown transition \"" + tid + "\"");
        rg.expand(cur);
        const auto e = rg.findEdge(cur, *t);
        if (!e)
            return fail("step " + std::to_string(k) + ": transition \"" + tid
                        + "\" is not enabled here (or its firing was dropped by a bound or the state limit)");
        edges.push_back(*e);
        cur = rg.edge(*e).target;
    }

    // --- apply ---------------------------------------------------------------
    ReplayResult result;
    result.run = db.beginRun(initial, pr.note);
    std::size_t nextDump = 0;
    const auto flushDumps = [&](std::size_t stepsDone) {
        while (nextDump < pr.dumps.size() && pr.dumps[nextDump].stepIndex == stepsDone) {
            const DumpRecord &d = pr.dumps[nextDump++];
            db.recordDump(d.mode, d.files, d.stackText);
        }
    };
    flushDumps(0);
    for (std::size_t k = 0; k < edges.size(); ++k) {
        db.step(edges[k]);
        flushDumps(k + 1);
    }
    db.endRun(pr.outcome, pr.detail);
    db.adoptHookRuns(result.run, pr.hooks);  // GLM-5.3-Flash: records travel, commands do not re-run
    result.ok = true;
    return result;
}

}  // namespace pn
