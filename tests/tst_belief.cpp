// Tests for the BeliefTracker: belief sets, deviations, quiescence, definite vs.
// ambiguous crediting, and replay of event runs.
#include "ElevatorModel.h"
#include "engine/BeliefTracker.h"
#include "engine/CoverageDb.h"
#include "engine/ReachabilityGraph.h"
#include "engine/RunLog.h"
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

static SutEvent obs(const char *label) { return SutEvent{EventKind::Observed, label}; }
static SutEvent stim(const char *label) { return SutEvent{EventKind::Stimulated, label}; }
static SutEvent quiet() { return SutEvent{EventKind::Quiescent, {}}; }

static StateId findState(const ReachabilityGraph &g, const Marking &m)
{
    for (StateId s = 0; s < g.numStates(); ++s)
        if (g.marking(s) == m)
            return s;
    return kNoState;
}

// Marking order of the elevator model: closed, opening, open, closing, obstructed, fault.
static const Marking kClosed{1, 0, 0, 0, 0, 0};
static const Marking kOpening{0, 1, 0, 0, 0, 0};
static const Marking kOpen{0, 0, 1, 0, 0, 0};
static const Marking kOpenObstructed{0, 0, 1, 0, 1, 0};
static const Marking kClosing{0, 0, 0, 1, 0, 0};
static const Marking kFault{0, 0, 0, 0, 0, 1};

static EdgeId edgeOf(const ReachabilityGraph &rg, const Marking &m, const char *transition)
{
    const StateId s = findState(rg, m);
    if (s == kNoState || !rg.isExpanded(s))
        return kNoEdge;
    const auto t = rg.net().findTransition(transition);
    const auto e = t ? rg.findEdge(s, *t) : std::nullopt;
    return e ? *e : kNoEdge;
}

// ---------------------------------------------------------------------------
// Following events
// ---------------------------------------------------------------------------

static void testBeliefGrowsAndCollapses()
{
    const Net net = testmodels::makeElevatorNet();
    ReachabilityGraph rg(net);
    BeliefTracker tr(rg);  // no database: pure tracking
    tr.beginRun(0);
    CHECK(tr.isDefinite());

    CHECK(tr.onEvent(stim("door.open")).status == StepStatus::Ok);
    CHECK(tr.isDefinite());  // opening
    CHECK(tr.onEvent(obs("door.opened")).status == StepStatus::Ok);
    CHECK(tr.belief().size() == 2);  // open, or open + silently obstructed

    CHECK(tr.onEvent(stim("door.close")).status == StepStatus::Ok);
    CHECK(tr.belief().size() == 2);  // closing, or fault (a silent jam may have happened)
    CHECK(tr.onEvent(obs("door.closed")).status == StepStatus::Ok);
    CHECK(tr.isDefinite());          // only closing can close
    CHECK(tr.possiblyTarget());
    tr.endRun(RunOutcome::Completed);
    CHECK(!tr.runActive());
}

static void testDeviationAndPreemption()
{
    const Net net = testmodels::makeElevatorNet();
    ReachabilityGraph rg(net);
    BeliefTracker tr(rg);
    tr.beginRun(0);

    // An input the model does not allow in the closed state.
    const StepResult pre = tr.onEvent(stim("door.close"));
    CHECK(pre.status == StepStatus::Preempted);
    CHECK((pre.expected == std::vector<std::string>{"door.open"}));
    CHECK(tr.isDefinite());  // unchanged

    CHECK(tr.onEvent(stim("door.open")).status == StepStatus::Ok);

    // The SUT reports an output the model does not allow while opening.
    const StepResult dev = tr.onEvent(obs("door.closed"));
    CHECK(dev.status == StepStatus::Deviation);
    CHECK((dev.expected == std::vector<std::string>{"door.opened"}));
    CHECK(!dev.modelIncomplete);
    CHECK(tr.isDefinite());

    const StepResult unknown = tr.onEvent(obs("door.exploded"));
    CHECK(unknown.status == StepStatus::Deviation);
    CHECK(unknown.message.find("does not know") != std::string::npos);

    const StepResult unknownStimulus = tr.onEvent(stim("door.fly"));
    CHECK(unknownStimulus.status == StepStatus::Preempted);

    // The tracker still accepts the right continuation.
    CHECK(tr.onEvent(obs("door.opened")).status == StepStatus::Ok);
    tr.endRun(RunOutcome::Completed);
}

static void testQuiescence()
{
    const Net net = testmodels::makeElevatorNet();
    ReachabilityGraph rg(net);
    {
        // Silence while an output is due is a deviation (missing output).
        BeliefTracker tr(rg);
        tr.beginRun(0);
        tr.onEvent(stim("door.open"));
        const StepResult r = tr.onEvent(quiet());
        CHECK(r.status == StepStatus::Deviation);
        CHECK((r.expected == std::vector<std::string>{"door.opened"}));
        tr.endRun(RunOutcome::Deviation);
    }
    {
        // Silence after closing: only the jammed state is quiescent, so the
        // SUT must have jammed, which is an error marking.
        BeliefTracker tr(rg);
        tr.beginRun(0);
        tr.onEvent(stim("door.open"));
        tr.onEvent(obs("door.opened"));
        tr.onEvent(stim("door.close"));
        CHECK(!tr.definiteErrorMarking().has_value());
        CHECK(tr.possiblyInError());
        const StepResult r = tr.onEvent(quiet());
        CHECK(r.status == StepStatus::Ok);  // an error marking is not a deadlock
        CHECK(tr.isDefinite());
        CHECK(tr.definiteErrorMarking().has_value() && *tr.definiteErrorMarking() == 0);
        tr.endRun(RunOutcome::ErrorReached, "jammed");
    }
    {
        // Silence in a state that merely waits for an input is fine.
        BeliefTracker tr(rg);
        tr.beginRun(0);
        CHECK(tr.onEvent(quiet()).status == StepStatus::Ok);
        CHECK(tr.isDefinite());
        tr.endRun(RunOutcome::Completed);
    }
}

static void testDeadlockOnQuiescence()
{
    for (bool qTarget : {false, true}) {
        NetBuilder b;
        const auto p = b.place("p");
        const auto q = b.place("q");
        const auto go = b.transition("go", TransitionKind::Controllable, "go");
        b.input(p, go).output(go, q);
        b.initial("s", {{p, 1}});
        if (qTarget)
            b.targetMarking("done", {{q, 1}});
        const Net net = b.build();

        ReachabilityGraph rg(net);
        BeliefTracker tr(rg);
        tr.beginRun(0);
        CHECK(tr.onEvent(stim("go")).status == StepStatus::Ok);
        const StepResult r = tr.onEvent(quiet());
        CHECK(r.status == (qTarget ? StepStatus::Ok : StepStatus::Deadlock));
        tr.endRun(qTarget ? RunOutcome::Completed : RunOutcome::Deadlock);
    }
}

static void testTracedInternalStepsAreNotSilent()
{
    // p --tick (internal, labelled)--> q, p --out (observable)--> r
    NetBuilder b;
    const auto p = b.place("p");
    const auto q = b.place("q");
    const auto r = b.place("r");
    const auto tick = b.transition("tick", TransitionKind::Internal, "tick");
    const auto out = b.transition("out", TransitionKind::Observable, "out");
    b.input(p, tick).output(tick, q);
    b.input(p, out).output(out, r);
    b.initial("s", {{p, 1}});
    const Net net = b.build();

    ReachabilityGraph rg(net);
    BeliefTracker tr(rg);
    tr.beginRun(0);
    CHECK(tr.belief().size() == 1);  // the traced step is not part of the silent closure
    CHECK((tr.expectedLabels() == std::vector<std::string>{"out", "tick"}));
    CHECK(tr.onEvent(obs("tick")).status == StepStatus::Ok);
    CHECK(tr.belief().size() == 1 && tr.belief()[0] == findState(rg, Marking{0, 1, 0}));
    tr.endRun(RunOutcome::Completed);
}

static void testEnabledControllableReportsRobustness()
{
    const Net net = testmodels::makeElevatorNet();
    ReachabilityGraph rg(net);
    BeliefTracker tr(rg);
    tr.beginRun(0);

    auto c = tr.enabledControllable();
    CHECK(c.size() == 1 && c[0].transition == *net.findTransition("open_cmd") && c[0].enabledIn == 1);

    tr.onEvent(stim("door.open"));
    tr.onEvent(obs("door.opened"));
    CHECK(tr.belief().size() == 2);
    c = tr.enabledControllable();
    // close_cmd is enabled in "open" but inhibited in "open + obstructed": risky.
    CHECK(c.size() == 1 && c[0].transition == *net.findTransition("close_cmd"));
    CHECK(c[0].enabledIn == 1 && tr.belief().size() == 2);
    tr.endRun(RunOutcome::Completed);
}

static void testDroppedEdgesAreFlaggedAsModelIncomplete()
{
    // The model bounds q to 1 token; the SUT emits "out" twice.
    NetBuilder b;
    const auto p = b.place("p");
    const auto q = b.place("q", 1);
    const auto out = b.transition("out", TransitionKind::Observable, "out");
    b.input(p, out).output(out, q).output(out, p);  // p is a self-loop source of tokens
    b.initial("s", {{p, 1}});
    const Net net = b.build();

    ReachabilityGraph rg(net);
    BeliefTracker tr(rg);
    tr.beginRun(0);
    CHECK(tr.onEvent(obs("out")).status == StepStatus::Ok);
    const StepResult r = tr.onEvent(obs("out"));  // would exceed the bound: edge was dropped
    CHECK(r.status == StepStatus::Deviation);
    CHECK(r.modelIncomplete);
    tr.endRun(RunOutcome::Deviation);
}

static void testEventsOutsideARun()
{
    const Net net = testmodels::makeElevatorNet();
    ReachabilityGraph rg(net);
    CoverageDb db(rg);
    BeliefTracker tr(rg, &db);
    CHECK(tr.onEvent(stim("door.open")).status == StepStatus::Deviation);

    tr.beginRun(0);
    tr.beginRun(0);  // supersedes
    CHECK(db.numRuns() == 2);
    CHECK(db.run(0).outcome == RunOutcome::Aborted);
    tr.endRun(RunOutcome::Completed);
}

// ---------------------------------------------------------------------------
// Crediting
// ---------------------------------------------------------------------------

static void testAmbiguityIsCreditedAsAmbiguous()
{
    const Net net = testmodels::makeElevatorNet();
    ReachabilityGraph rg(net);
    CoverageDb db(rg);
    BeliefTracker tr(rg, &db);

    tr.beginRun(0);
    // The initial state is certain and is credited at once.
    CHECK(db.state(findState(rg, kClosed)).definite == 1);

    tr.onEvent(stim("door.open"));
    CHECK(db.state(findState(rg, kOpening)).definite == 1);  // credited before the run ends
    tr.onEvent(obs("door.opened"));
    tr.onEvent(stim("door.close"));
    tr.onEvent(obs("door.closed"));
    tr.endRun(RunOutcome::Completed);

    // Certain: the round trip.
    CHECK(db.state(findState(rg, kClosed)).definite == 2);
    CHECK(db.state(findState(rg, kOpening)).definite == 1);
    CHECK(db.state(findState(rg, kOpen)).definite == 1);
    CHECK(db.state(findState(rg, kClosing)).definite == 1);
    for (const char *t : {"open_cmd", "opened", "close_cmd", "closed_ev"}) {
        const StateId from = t == std::string("open_cmd")    ? findState(rg, kClosed)
                             : t == std::string("opened")    ? findState(rg, kOpening)
                             : t == std::string("close_cmd") ? findState(rg, kOpen)
                                                             : findState(rg, kClosing);
        const auto e = rg.findEdge(from, *net.findTransition(t));
        CHECK(e && db.edge(*e).definite == 1);
    }

    // Ambiguous: the silent obstruct/clear loop may or may not have happened.
    const StateId openObs = findState(rg, kOpenObstructed);
    CHECK(openObs != kNoState);
    CHECK(db.state(openObs).definite == 0 && db.state(openObs).ambiguous == 1);
    const EdgeId obstruct = edgeOf(rg, kOpen, "obstruct");
    const EdgeId clear = edgeOf(rg, kOpenObstructed, "clear");
    CHECK(obstruct != kNoEdge && db.edge(obstruct).ambiguous == 1 && db.edge(obstruct).definite == 0);
    CHECK(clear != kNoEdge && db.edge(clear).ambiguous == 1 && db.edge(clear).definite == 0);

    // Ruled out by the later "door.closed": the jam did not happen, so it is not credited at all.
    const StateId fault = findState(rg, kFault);
    CHECK(fault != kNoState);
    CHECK(db.state(fault).definite == 0 && db.state(fault).ambiguous == 0);
    const EdgeId jam = edgeOf(rg, kClosing, "jam");
    CHECK(jam != kNoEdge && db.edge(jam).definite == 0 && db.edge(jam).ambiguous == 0);

    const CoverageSummary s = db.summary();
    CHECK(s.transitionsCovered == 4);  // internal steps are only ambiguous
    CHECK(db.run(0).events.size() == 4);
}

static void testQuiescenceResolvesAmbiguityTowardsTheJam()
{
    ErrorOptions jammed;
    jammed.crashDump = DumpMode::Snapshot;
    const Net net = testmodels::makeElevatorNet(jammed);
    ReachabilityGraph rg(net);
    CoverageDb db(rg);
    BeliefTracker tr(rg, &db);

    tr.beginRun(0);
    tr.onEvent(stim("door.open"));
    tr.onEvent(obs("door.opened"));
    tr.onEvent(stim("door.close"));
    CHECK(tr.onEvent(quiet()).status == StepStatus::Ok);

    // The belief collapsed to the fault state, which settles the silent jam.
    const StateId fault = findState(rg, kFault);
    CHECK(db.state(fault).definite == 1);
    CHECK(db.errorMarkingHits(0) == 1);
    const EdgeId jam = edgeOf(rg, kClosing, "jam");
    CHECK(jam != kNoEdge && db.edge(jam).definite == 1);
    CHECK(db.transitionHits(*net.findTransition("jam")) == 1);

    // Post-mortem while certainly in the error marking.
    CHECK(db.dumpAllowed(0));
    tr.recordDump(DumpMode::Snapshot, {"core.7"}, "frame #0 jam()");
    CHECK(!db.dumpAllowed(0));
    tr.endRun(RunOutcome::ErrorReached, "jammed");

    CHECK(db.run(0).dumps.size() == 1);
    CHECK(db.run(0).dumps[0].stepIndex == 4);  // events seen so far
    CHECK(db.run(0).dumps[0].marking == "jammed");

    // The definite chain is contiguous here, so the backtrace is a real path.
    const auto bt = db.stateBacktrace(fault);
    CHECK(bt.size() == 5);
    CHECK(bt.front().via == kNoEdge && bt.front().state == findState(rg, kClosed));
    CHECK(rg.edge(bt.back().via).transition == *net.findTransition("jam"));
}

static void testUnresolvedAmbiguityAtRunEnd()
{
    const Net net = testmodels::makeElevatorNet();
    ReachabilityGraph rg(net);
    CoverageDb db(rg);
    BeliefTracker tr(rg, &db);

    tr.beginRun(0);
    tr.onEvent(stim("door.open"));
    tr.onEvent(obs("door.opened"));
    CHECK(tr.belief().size() == 2);
    // Nothing about the open/obstructed question is credited while it is open...
    CHECK(db.state(findState(rg, kOpen)).definite == 0);
    tr.endRun(RunOutcome::Completed);

    // ...but at the end, "open" lies on every consistent path, "open + obstructed" does not.
    CHECK(db.state(findState(rg, kOpen)).definite == 1);
    CHECK(db.state(findState(rg, kOpenObstructed)).definite == 0);
    CHECK(db.state(findState(rg, kOpenObstructed)).ambiguous == 1);
    CHECK(db.edge(edgeOf(rg, kOpen, "obstruct")).ambiguous == 1);
    CHECK(db.edge(edgeOf(rg, kOpenObstructed, "clear")).ambiguous == 1);
}

static void testCertainTransitionWithAmbiguousEdge()
{
    // Two silent routes a->b and a->c, then the same output "x" from both to d.
    NetBuilder b;
    const auto a = b.place("a");
    const auto pb = b.place("b");
    const auto pc = b.place("c");
    const auto d = b.place("d");
    const auto s1 = b.transition("s1");
    const auto s2 = b.transition("s2");
    const auto xb = b.transition("xb", TransitionKind::Observable, "x");
    const auto xc = b.transition("xc", TransitionKind::Observable, "x");
    b.input(a, s1).output(s1, pb);
    b.input(a, s2).output(s2, pc);
    b.input(pb, xb).output(xb, d);
    b.input(pc, xc).output(xc, d);
    b.initial("s", {{a, 1}});
    const Net net = b.build();

    ReachabilityGraph rg(net);
    CoverageDb db(rg);
    BeliefTracker tr(rg, &db);
    tr.beginRun(0);
    CHECK(tr.belief().size() == 3);  // a, b, c
    CHECK(tr.onEvent(obs("x")).status == StepStatus::Ok);
    CHECK(tr.isDefinite());
    tr.endRun(RunOutcome::Completed);

    const StateId sa = findState(rg, Marking{1, 0, 0, 0});
    const StateId sd = findState(rg, Marking{0, 0, 0, 1});
    const StateId sb = findState(rg, Marking{0, 1, 0, 0});
    const StateId sc = findState(rg, Marking{0, 0, 1, 0});
    CHECK(db.state(sa).definite == 1 && db.state(sd).definite == 1);
    CHECK(db.state(sb).definite == 0 && db.state(sb).ambiguous == 1);
    CHECK(db.state(sc).definite == 0 && db.state(sc).ambiguous == 1);

    // Neither "x" edge is certain, and xb and xc are different transitions, so
    // no transition can be credited definitely either (contrast with the next test).
    CHECK(db.transitionHits(xb) == 0 && db.transitionHits(xc) == 0);
    CHECK(db.edge(edgeOf(rg, Marking{0, 1, 0, 0}, "xb")).ambiguous == 1);
    CHECK(db.edge(edgeOf(rg, Marking{0, 0, 1, 0}, "xc")).ambiguous == 1);
    (void)s1; (void)s2;

    // The backtrace of d skips the ambiguous stretch: a gap, marked by via == kNoEdge at depth > 1.
    const auto bt = db.stateBacktrace(sd);
    CHECK(bt.size() == 2);
    CHECK(bt[0].state == sa && bt[0].via == kNoEdge);
    CHECK(bt[1].state == sd && bt[1].via == kNoEdge);
    CHECK(db.traceTree().node(db.state(sd).firstNode).depth == 2);
}

static void testSameTransitionIsCreditedEvenIfTheSourceIsAmbiguous()
{
    // Two silent routes leave a token in "left" or "right" and both leave one
    // in "m". The single observable transition x consumes m, so x fires from
    // two different states: which EDGE was taken is ambiguous, but that the
    // TRANSITION x fired is certain.
    NetBuilder b;
    const auto a = b.place("a");
    const auto m = b.place("m");
    const auto left = b.place("left");
    const auto right = b.place("right");
    const auto d = b.place("d");
    const auto t1 = b.transition("t1");
    const auto t2 = b.transition("t2");
    const auto x = b.transition("x", TransitionKind::Observable, "x");
    b.input(a, t1).output(t1, left).output(t1, m);
    b.input(a, t2).output(t2, right).output(t2, m);
    b.input(m, x).output(x, d);
    b.initial("s", {{a, 1}});
    const Net net = b.build();

    ReachabilityGraph rg(net);
    CoverageDb db(rg);
    BeliefTracker tr(rg, &db);
    tr.beginRun(0);
    CHECK(tr.onEvent(obs("x")).status == StepStatus::Ok);
    tr.endRun(RunOutcome::Completed);

    CHECK(db.transitionHits(x) == 1);
    CHECK(db.transitionHits(t1) == 0 && db.transitionHits(t2) == 0);  // silent and ambiguous
    // Marking order: a, m, left, right, d.
    const StateId viaLeft = findState(rg, Marking{0, 1, 1, 0, 0});
    const StateId viaRight = findState(rg, Marking{0, 1, 0, 1, 0});
    CHECK(viaLeft != kNoState && viaRight != kNoState);
    CHECK(db.state(viaLeft).definite == 0 && db.state(viaLeft).ambiguous == 1);
    CHECK(db.state(viaRight).definite == 0 && db.state(viaRight).ambiguous == 1);
    CHECK(db.edge(edgeOf(rg, Marking{0, 1, 1, 0, 0}, "x")).ambiguous == 1);
    CHECK(db.edge(edgeOf(rg, Marking{0, 1, 1, 0, 0}, "x")).definite == 0);
}

static void testPlaceAndMarkingCountersFollowCrediting()
{
    const Net net = testmodels::makeElevatorNet();
    ReachabilityGraph rg(net);
    CoverageDb db(rg);
    BeliefTracker tr(rg, &db);
    tr.beginRun(0);
    tr.onEvent(stim("door.open"));
    tr.onEvent(obs("door.opened"));
    tr.onEvent(stim("door.close"));
    tr.onEvent(obs("door.closed"));
    tr.endRun(RunOutcome::Completed);
    CHECK(db.targetMarkingHits(0) == 2);  // start and end
    const CoverageSummary s = db.summary();
    CHECK(s.placesCovered == 4);         // closed, opening, open, closing
    CHECK(s.statesCovered == 4);
}

// ---------------------------------------------------------------------------
// Replay of event runs
// ---------------------------------------------------------------------------

static void testReplayOfEventRunsReproducesCoverage()
{
    ErrorOptions jammed;
    jammed.targetHits = 2;
    jammed.crashDump = DumpMode::Snapshot;
    const Net net = testmodels::makeElevatorNet(jammed);

    std::vector<PersistedRun> saved;
    CoverageSummary live;
    std::uint64_t liveAmbiguousOpenObs = 0, liveErrorHits = 0, liveDumps = 0;
    {
        ReachabilityGraph rg(net);
        CoverageDb db(rg);
        BeliefTracker tr(rg, &db);

        tr.beginRun(0, "clean");
        for (const SutEvent &e : {stim("door.open"), obs("door.opened"), stim("door.close"), obs("door.closed")})
            tr.onEvent(e);
        tr.endRun(RunOutcome::Completed);

        tr.beginRun(0, "jam");
        for (const SutEvent &e : {stim("door.open"), obs("door.opened"), stim("door.close"), quiet()})
            tr.onEvent(e);
        tr.recordDump(DumpMode::Snapshot, {"core.1"}, "stack\nline \"two\"");
        tr.endRun(RunOutcome::ErrorReached, "jammed");

        tr.beginRun(0, "deviation");
        tr.onEvent(stim("door.open"));
        const StepResult r = tr.onEvent(obs("door.closed"));
        CHECK(r.status == StepStatus::Deviation);
        tr.endRun(RunOutcome::Deviation, r.message);

        for (RunId i = 0; i < db.numRuns(); ++i)
            saved.push_back(toPersisted(rg, db.run(i)));
        live = db.summary();
        liveAmbiguousOpenObs = db.state(findState(rg, kOpenObstructed)).ambiguous;
        liveErrorHits = db.errorMarkingHits(0);
        liveDumps = db.errorMarkingDumps(0);
    }
    CHECK(saved.size() == 3);
    CHECK(saved[0].transitions.empty() && saved[0].events.size() == 4);
    CHECK(saved[0].events[0].kind == EventKind::Stimulated && saved[0].events[0].label == "door.open");
    CHECK(saved[1].dumps.size() == 1 && saved[1].dumps[0].stepIndex == 4);
    CHECK(saved[2].outcome == RunOutcome::Deviation);

    ReachabilityGraph rg2(net);
    CoverageDb db2(rg2);
    for (const PersistedRun &pr : saved) {
        const ReplayResult r = replay(db2, rg2, pr);
        CHECK(r.ok);
    }
    CHECK(db2.numRuns() == 3);
    const CoverageSummary s2 = db2.summary();
    CHECK(s2.statesCovered == live.statesCovered);
    CHECK(s2.edgesCovered == live.edgesCovered);
    CHECK(s2.transitionsCovered == live.transitionsCovered);
    CHECK(s2.placesCovered == live.placesCovered);
    CHECK(db2.state(findState(rg2, kOpenObstructed)).ambiguous == liveAmbiguousOpenObs);
    CHECK(db2.errorMarkingHits(0) == liveErrorHits);
    CHECK(db2.errorMarkingDumps(0) == liveDumps);
    CHECK(db2.run(1).dumps[0].stackText == "stack\nline \"two\"");
    CHECK(db2.run(2).outcome == RunOutcome::Deviation);
}

static void testReplayRejectsDivergingEventRuns()
{
    const Net net = testmodels::makeElevatorNet();
    ReachabilityGraph rg(net);
    CoverageDb db(rg);

    PersistedRun diverging;
    diverging.initialMarking = "start";
    diverging.events = {stim("door.open"), obs("door.closed"), obs("door.opened")};  // deviation mid-run
    const ReplayResult r = replay(db, rg, diverging);
    CHECK(!r.ok && r.error.find("event 1") != std::string::npos);

    PersistedRun badDump;
    badDump.initialMarking = "start";
    badDump.events = {stim("door.open")};
    badDump.dumps.push_back(DumpRecord{5, "", DumpMode::Snapshot, {}, ""});
    CHECK(!replay(db, rg, badDump).ok);

    CHECK(db.numRuns() == 0);

    // A deviation as the LAST event is fine: that is how such a run ended.
    PersistedRun endsInDeviation;
    endsInDeviation.initialMarking = "start";
    endsInDeviation.events = {stim("door.open"), obs("door.closed")};
    endsInDeviation.outcome = RunOutcome::Deviation;
    CHECK(replay(db, rg, endsInDeviation).ok);
    CHECK(db.numRuns() == 1 && db.run(0).outcome == RunOutcome::Deviation);
}

static void testEventKindStrings()
{
    for (EventKind k : {EventKind::Observed, EventKind::Stimulated, EventKind::Quiescent}) {
        EventKind back = EventKind::Observed;
        CHECK(parseEventKind(toString(k), back) && back == k);
    }
    EventKind x;
    CHECK(!parseEventKind("nonsense", x));
}

// ---------------------------------------------------------------------------

int main()
{
    const std::vector<std::pair<const char *, std::function<void()>>> tests = {
        {"beliefGrowsAndCollapses", testBeliefGrowsAndCollapses},
        {"deviationAndPreemption", testDeviationAndPreemption},
        {"quiescence", testQuiescence},
        {"deadlockOnQuiescence", testDeadlockOnQuiescence},
        {"tracedInternalStepsAreNotSilent", testTracedInternalStepsAreNotSilent},
        {"enabledControllableReportsRobustness", testEnabledControllableReportsRobustness},
        {"droppedEdgesAreFlaggedAsModelIncomplete", testDroppedEdgesAreFlaggedAsModelIncomplete},
        {"eventsOutsideARun", testEventsOutsideARun},
        {"ambiguityIsCreditedAsAmbiguous", testAmbiguityIsCreditedAsAmbiguous},
        {"quiescenceResolvesAmbiguityTowardsTheJam", testQuiescenceResolvesAmbiguityTowardsTheJam},
        {"unresolvedAmbiguityAtRunEnd", testUnresolvedAmbiguityAtRunEnd},
        {"certainTransitionWithAmbiguousEdge", testCertainTransitionWithAmbiguousEdge},
        {"sameTransitionIsCreditedEvenIfTheSourceIsAmbiguous",
         testSameTransitionIsCreditedEvenIfTheSourceIsAmbiguous},
        {"placeAndMarkingCountersFollowCrediting", testPlaceAndMarkingCountersFollowCrediting},
        {"replayOfEventRunsReproducesCoverage", testReplayOfEventRunsReproducesCoverage},
        {"replayRejectsDivergingEventRuns", testReplayRejectsDivergingEventRuns},
        {"eventKindStrings", testEventKindStrings},
    };
    for (const auto &t : tests) {
        const int before = g_failures;
        t.second();
        std::printf("%s %s\n", g_failures == before ? "PASS" : "FAIL", t.first);
    }
    std::printf("%d failure(s)\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
