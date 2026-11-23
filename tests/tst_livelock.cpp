// Tests for both kinds of livelock: the static one (strongly connected
// components of the specification's reachability graph) and the run-time one
// (events keep coming without progress, with a settable timeout).
#include "ElevatorModel.h"
#include "engine/BeliefTracker.h"
#include "engine/CoverageDb.h"
#include "engine/LivelockAnalysis.h"
#include "engine/ReachabilityGraph.h"
#include "engine/RunLog.h"
#include "engine/Scc.h"
#include "model/NetBuilder.h"

#include <cstdio>
#include <functional>
#include <string>
#include <vector>

using namespace pn;

static int g_failures = 0;

#define CHECK(cond)                                                                       \
    do {                                                                                  \
        if (!(cond)) {                                                                    \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                                                 \
        }                                                                                 \
    } while (0)

static StateId findState(const ReachabilityGraph &g, const Marking &m)
{
    for (StateId s = 0; s < g.numStates(); ++s)
        if (g.marking(s) == m)
            return s;
    return kNoState;
}

// p -> q, and q <-> r form a cycle that cannot be left.
static Net makeCycleNet(bool targetInCycle)
{
    NetBuilder b;
    const auto p = b.place("p");
    const auto q = b.place("q");
    const auto r = b.place("r");
    const auto t = b.transition("t");
    const auto u = b.transition("u");
    const auto v = b.transition("v");
    b.input(p, t).output(t, q);
    b.input(q, u).output(u, r);
    b.input(r, v).output(v, q);
    b.initial("s", {{p, 1}});
    if (targetInCycle)
        b.targetMarking("settled", {{q, 1}});
    return b.build();
}

// ---------------------------------------------------------------------------
// Static analysis
// ---------------------------------------------------------------------------

static void testSccDecomposition()
{
    const Net net = makeCycleNet(false);
    ReachabilityGraph g(net);
    g.addInitial(0);
    g.exploreAll();
    CHECK(g.numStates() == 3);  // {p}, {q}, {r}

    const Sccs scc = computeScc(g);
    CHECK(scc.members.size() == 2);
    const StateId sp = findState(g, Marking{1, 0, 0});
    const StateId sq = findState(g, Marking{0, 1, 0});
    const StateId sr = findState(g, Marking{0, 0, 1});
    CHECK(scc.componentOf[sq] == scc.componentOf[sr]);
    CHECK(scc.componentOf[sp] != scc.componentOf[sq]);
    // Reverse topological order: the sink component {q, r} comes before {p}.
    CHECK(scc.componentOf[sq] < scc.componentOf[sp]);

    // Restricting the edges: without "u" (q -> r) the cycle is broken.
    const auto u = *net.findTransition("u");
    const Sccs broken = computeScc(g, [u](const Edge &e) { return e.transition != u; });
    CHECK(broken.members.size() == 3);
}

static void testTerminalCycleIsALivelock()
{
    const Net net = makeCycleNet(false);
    ReachabilityGraph g(net);
    g.addInitial(0);
    g.exploreAll();

    const LivelockReport r = analyzeLivelocks(g);
    CHECK(r.graphComplete);
    CHECK(r.components == 2 && r.cyclicComponents == 1);
    CHECK(r.findings.size() == 1);
    CHECK(r.findings[0].kind == LivelockKind::TerminalCycle);
    CHECK(r.findings[0].certain);
    CHECK(r.findings[0].states.size() == 2);
    CHECK(r.findings[0].states[0] == findState(g, Marking{0, 1, 0}));
    CHECK(g.deadlocks().empty());  // a livelock is not a deadlock
}

static void testCycleWithTargetMarkingIsLegitimate()
{
    const Net net = makeCycleNet(true);
    ReachabilityGraph g(net);
    g.addInitial(0);
    g.exploreAll();
    const LivelockReport r = analyzeLivelocks(g);
    CHECK(r.cyclicComponents == 1);
    CHECK(r.findings.empty());
}

static void testSelfLoopAndDeadEnd()
{
    {  // a self-loop that cannot be left is a livelock of one state
        NetBuilder b;
        const auto p = b.place("p");
        const auto t = b.transition("t");
        b.input(p, t).output(t, p);
        b.initial("s", {{p, 1}});
        const Net net = b.build();
        ReachabilityGraph g(net);
        g.addInitial(0);
        g.exploreAll();
        const LivelockReport r = analyzeLivelocks(g);
        CHECK(r.findings.size() == 1 && r.findings[0].states.size() == 1);
    }
    {  // a plain dead end is a deadlock, not a livelock
        NetBuilder b;
        const auto p = b.place("p");
        const auto q = b.place("q");
        const auto t = b.transition("t");
        b.input(p, t).output(t, q);
        b.initial("s", {{p, 1}});
        const Net net = b.build();
        ReachabilityGraph g(net);
        g.addInitial(0);
        g.exploreAll();
        CHECK(analyzeLivelocks(g).findings.empty());
        CHECK(g.deadlocks().size() == 1);
    }
}

static void testElevatorHasNoTerminalCycleButAnInternalOne()
{
    const Net net = testmodels::makeElevatorNet();
    ReachabilityGraph g(net);
    g.addInitial(0);
    g.exploreAll();

    LivelockReport r = analyzeLivelocks(g);
    CHECK(r.graphComplete);
    CHECK(r.components == 2);       // the big cycle and the fault state
    CHECK(r.cyclicComponents == 1);
    CHECK(r.findings.empty());      // the cycle can be left through "jam"

    // Opt in to divergence: obstruct/clear toggle forever without any visible event.
    LivelockOptions options;
    options.internalCycles = true;
    r = analyzeLivelocks(g, options);
    CHECK(r.findings.size() == 1);
    CHECK(r.findings[0].kind == LivelockKind::InternalCycle);
    CHECK(r.findings[0].certain);
    CHECK(r.findings[0].states.size() == 2);
    const StateId open = findState(g, Marking{0, 0, 1, 0, 0, 0});
    const StateId openObstructed = findState(g, Marking{0, 0, 1, 0, 1, 0});
    CHECK((r.findings[0].states == std::vector<StateId>{std::min(open, openObstructed),
                                                         std::max(open, openObstructed)}));

    // And the terminal-cycle check can be switched off.
    options.terminalCycles = false;
    CHECK(analyzeLivelocks(g, options).findings.size() == 1);
}

static void testPartialGraphsNeverClaimTerminalCycles()
{
    // p <-> q is a cycle, but q also has an exit q -> r. Expand only p and q.
    NetBuilder b;
    const auto p = b.place("p");
    const auto q = b.place("q");
    const auto r = b.place("r");
    const auto t = b.transition("t");
    const auto u = b.transition("u");
    const auto w = b.transition("w");
    b.input(p, t).output(t, q);
    b.input(q, u).output(u, p);
    b.input(q, w).output(w, r);
    b.initial("s", {{p, 1}});
    const Net net = b.build();

    ReachabilityGraph g(net);
    const StateId sp = g.addInitial(0);
    g.expand(sp);
    g.expand(findState(g, Marking{0, 1, 0}));

    const LivelockReport partial = analyzeLivelocks(g);
    CHECK(!partial.graphComplete);          // "r" has not been expanded
    CHECK(partial.cyclicComponents == 1);
    CHECK(partial.findings.empty());        // the edge into "r" is an exit

    g.exploreAll();
    CHECK(analyzeLivelocks(g).findings.empty());
}

static void testDroppedFiringMakesAFindingUncertain()
{
    // a <-> b cycle; in b, transition t3 would put a second token into the
    // bounded place c, so its firing is dropped. An exit might hide there.
    NetBuilder b;
    const auto a = b.place("a");
    const auto pb = b.place("b");
    const auto c = b.place("c", 1);
    const auto t1 = b.transition("t1");
    const auto t2 = b.transition("t2");
    const auto t3 = b.transition("t3");
    b.input(a, t1).output(t1, pb);
    b.input(pb, t2).output(t2, a);
    b.input(pb, t3).output(t3, pb).output(t3, c);
    b.initial("s", {{a, 1}, {c, 1}});
    const Net net = b.build();

    ReachabilityGraph g(net);
    g.addInitial(0);
    g.exploreAll();
    CHECK(!g.boundViolations().empty());

    const LivelockReport r = analyzeLivelocks(g);
    CHECK(!r.graphComplete);
    CHECK(r.findings.size() == 1);
    CHECK(!r.findings[0].certain);
}

// ---------------------------------------------------------------------------
// Run-time livelock
// ---------------------------------------------------------------------------

// a --ping--> b --pong--> a, both observable; plus a controllable self-loop "kick" in a.
static Net makePingPongNet(bool aIsTarget)
{
    NetBuilder b;
    const auto a = b.place("a");
    const auto pb = b.place("b");
    const auto ping = b.transition("ping", TransitionKind::Observable, "ping");
    const auto pong = b.transition("pong", TransitionKind::Observable, "pong");
    const auto kick = b.transition("kick", TransitionKind::Controllable, "kick");
    b.input(a, ping).output(ping, pb);
    b.input(pb, pong).output(pong, a);
    b.input(a, kick).output(kick, a);
    b.initial("s", {{a, 1}});
    if (aIsTarget)
        b.targetMarking("idle", {{a, 1}});
    return b.build();
}

static SutEvent at(EventKind kind, const char *label, std::int64_t ms)
{
    return SutEvent{kind, label, ms};
}

// Events 100 ms apart, alternating ping/pong, starting with ping at t = 0.
static SutEvent pingPong(int k)
{
    return at(EventKind::Observed, k % 2 == 0 ? "ping" : "pong", k * 100);
}

static void testRuntimeLivelockIsDeclaredAtTheTimeout()
{
    const Net net = makePingPongNet(false);
    ReachabilityGraph rg(net);
    TrackerConfig cfg;
    cfg.livelockTimeoutMs = 1000;
    BeliefTracker tr(rg, nullptr, cfg);
    tr.beginRun(0);

    for (int k = 0; k < 10; ++k)
        CHECK(tr.onEvent(pingPong(k)).status == StepStatus::Ok);  // t = 0 .. 900
    const StepResult r = tr.onEvent(pingPong(10));                // t = 1000
    CHECK(r.status == StepStatus::Livelock);
    CHECK(r.message.find("without progress") != std::string::npos);
    CHECK(tr.belief().size() == 1);  // unlike a deviation, the event was applied
    tr.endRun(RunOutcome::Livelock, r.message);
}

static void testLivelockDetectionIsOffByDefaultAndSettable()
{
    const Net net = makePingPongNet(false);
    {
        ReachabilityGraph rg(net);
        BeliefTracker tr(rg);  // timeout 0: disabled
        tr.beginRun(0);
        for (int k = 0; k < 40; ++k)
            CHECK(tr.onEvent(pingPong(k)).status == StepStatus::Ok);
        tr.endRun(RunOutcome::Completed);
    }
    {
        ReachabilityGraph rg(net);
        TrackerConfig cfg;
        cfg.livelockTimeoutMs = 3000;
        BeliefTracker tr(rg, nullptr, cfg);
        tr.beginRun(0);
        int firstLivelock = -1;
        for (int k = 0; k < 40 && firstLivelock < 0; ++k)
            if (tr.onEvent(pingPong(k)).status == StepStatus::Livelock)
                firstLivelock = k;
        CHECK(firstLivelock == 30);  // t = 3000 ms
        tr.endRun(RunOutcome::Livelock);
    }
}

static void testStimulusCountsAsProgress()
{
    const Net net = makePingPongNet(false);
    ReachabilityGraph rg(net);
    TrackerConfig cfg;
    cfg.livelockTimeoutMs = 1000;
    BeliefTracker tr(rg, nullptr, cfg);
    tr.beginRun(0);

    for (int k = 0; k < 8; ++k)  // t = 0 .. 700, ends in state a
        CHECK(tr.onEvent(pingPong(k)).status == StepStatus::Ok);
    CHECK(tr.onEvent(at(EventKind::Stimulated, "kick", 800)).status == StepStatus::Ok);
    // The kick restarted the clock: at t = 1000 the previous progress is only 200 ms old,
    // whereas without the kick the ping at t = 1000 is declared a livelock (see above).
    CHECK(tr.onEvent(at(EventKind::Observed, "ping", 900)).status == StepStatus::Ok);
    CHECK(tr.onEvent(at(EventKind::Observed, "pong", 1000)).status == StepStatus::Ok);
    CHECK(tr.onEvent(at(EventKind::Observed, "ping", 1100)).status == StepStatus::Ok);  // 3 events, 300 ms
    tr.endRun(RunOutcome::Completed);
}

static void testASingleSlowAnswerIsNotALivelock()
{
    const Net net = makePingPongNet(false);
    ReachabilityGraph rg(net);
    TrackerConfig cfg;
    cfg.livelockTimeoutMs = 1000;
    cfg.livelockMinEvents = 3;
    BeliefTracker tr(rg, nullptr, cfg);
    tr.beginRun(0);

    CHECK(tr.onEvent(at(EventKind::Observed, "ping", 0)).status == StepStatus::Ok);      // new state
    CHECK(tr.onEvent(at(EventKind::Observed, "pong", 100)).status == StepStatus::Ok);    // 1 event
    CHECK(tr.onEvent(at(EventKind::Observed, "ping", 9000)).status == StepStatus::Ok);   // 2 events, very late
    CHECK(tr.onEvent(at(EventKind::Observed, "pong", 9100)).status == StepStatus::Livelock);  // 3rd
    tr.endRun(RunOutcome::Livelock);
}

static void testReachingATargetMarkingCountsAsProgress()
{
    const Net net = makePingPongNet(true);  // state a is a target
    ReachabilityGraph rg(net);
    TrackerConfig cfg;
    cfg.livelockTimeoutMs = 1000;
    BeliefTracker tr(rg, nullptr, cfg);
    tr.beginRun(0);
    // Every second event returns to a target marking, so the run can always end: no livelock.
    for (int k = 0; k < 60; ++k) {
        const StepResult r = tr.onEvent(pingPong(k));
        CHECK(r.status == StepStatus::Ok);
        if (r.status != StepStatus::Ok)
            break;
    }
    tr.endRun(RunOutcome::Completed);
}

static void testClockThatStepsBackIsTolerated()
{
    const Net net = makePingPongNet(false);
    ReachabilityGraph rg(net);
    TrackerConfig cfg;
    cfg.livelockTimeoutMs = 1000;
    BeliefTracker tr(rg, nullptr, cfg);
    tr.beginRun(0);
    CHECK(tr.onEvent(at(EventKind::Observed, "ping", 5000)).status == StepStatus::Ok);
    CHECK(tr.onEvent(at(EventKind::Observed, "pong", 100)).status == StepStatus::Ok);  // earlier stamp
    CHECK(tr.onEvent(at(EventKind::Observed, "ping", 200)).status == StepStatus::Ok);
    tr.endRun(RunOutcome::Completed);
}

static void testLivelockRunsRoundTripThroughTheLog()
{
    const Net net = makePingPongNet(false);
    TrackerConfig cfg;
    cfg.livelockTimeoutMs = 1000;

    PersistedRun saved;
    {
        ReachabilityGraph rg(net);
        CoverageDb db(rg);
        BeliefTracker tr(rg, &db, cfg);
        tr.beginRun(0, "spin");
        for (int k = 0; k <= 10; ++k) {
            const StepResult r = tr.onEvent(pingPong(k));
            if (r.status == StepStatus::Livelock) {
                tr.endRun(RunOutcome::Livelock, r.message);
                break;
            }
        }
        CHECK(db.numRuns() == 1 && db.run(0).outcome == RunOutcome::Livelock);
        saved = toPersisted(rg, db.run(0));
    }
    CHECK(saved.events.size() == 11);
    CHECK(saved.events.back().timestampMs == 1000);  // the timestamps are part of the record
    CHECK(saved.outcome == RunOutcome::Livelock);

    {   // replay with the same timeout: the livelock reappears at the last event, which is fine
        ReachabilityGraph rg(net);
        CoverageDb db(rg);
        CHECK(replay(db, rg, saved, cfg).ok);
        CHECK(db.run(0).outcome == RunOutcome::Livelock);
    }
    {   // replay with detection off: same coverage, same outcome
        ReachabilityGraph rg(net);
        CoverageDb db(rg);
        CHECK(replay(db, rg, saved).ok);
        CHECK(db.summary().statesCovered == 2);
    }
    {   // an event after the livelock verdict contradicts the recording when detection is on
        PersistedRun longer = saved;
        longer.events.push_back(pingPong(11));
        ReachabilityGraph rg(net);
        CoverageDb db(rg);
        const ReplayResult r = replay(db, rg, longer, cfg);
        CHECK(!r.ok && r.error.find("event 10") != std::string::npos);
        CHECK(db.numRuns() == 0);
        ReachabilityGraph rg2(net);
        CoverageDb db2(rg2);
        CHECK(replay(db2, rg2, longer).ok);  // detection off: nothing to contradict
    }
}

// ---------------------------------------------------------------------------

int main()
{
    const std::vector<std::pair<const char *, std::function<void()>>> tests = {
        {"sccDecomposition", testSccDecomposition},
        {"terminalCycleIsALivelock", testTerminalCycleIsALivelock},
        {"cycleWithTargetMarkingIsLegitimate", testCycleWithTargetMarkingIsLegitimate},
        {"selfLoopAndDeadEnd", testSelfLoopAndDeadEnd},
        {"elevatorHasNoTerminalCycleButAnInternalOne", testElevatorHasNoTerminalCycleButAnInternalOne},
        {"partialGraphsNeverClaimTerminalCycles", testPartialGraphsNeverClaimTerminalCycles},
        {"droppedFiringMakesAFindingUncertain", testDroppedFiringMakesAFindingUncertain},
        {"runtimeLivelockIsDeclaredAtTheTimeout", testRuntimeLivelockIsDeclaredAtTheTimeout},
        {"livelockDetectionIsOffByDefaultAndSettable", testLivelockDetectionIsOffByDefaultAndSettable},
        {"stimulusCountsAsProgress", testStimulusCountsAsProgress},
        {"aSingleSlowAnswerIsNotALivelock", testASingleSlowAnswerIsNotALivelock},
        {"reachingATargetMarkingCountsAsProgress", testReachingATargetMarkingCountsAsProgress},
        {"clockThatStepsBackIsTolerated", testClockThatStepsBackIsTolerated},
        {"livelockRunsRoundTripThroughTheLog", testLivelockRunsRoundTripThroughTheLog},
    };
    for (const auto &t : tests) {
        const int before = g_failures;
        t.second();
        std::printf("%s %s\n", g_failures == before ? "PASS" : "FAIL", t.first);
    }
    std::printf("%d failure(s)\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
