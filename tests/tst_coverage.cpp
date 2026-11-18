// Tests for the Qt-free coverage layer: fingerprint, reverse index, trace
// tree, CoverageDb and run replay.
#include "ElevatorModel.h"
#include "engine/CoverageDb.h"
#include "engine/ReachabilityGraph.h"
#include "engine/ReverseIndex.h"
#include "engine/RunLog.h"
#include "engine/TraceTree.h"
#include "model/Fingerprint.h"
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

// Fires the transition with the given id from the db's current state.
static bool fireNamed(CoverageDb &db, ReachabilityGraph &rg, const char *transitionId)
{
    const auto t = rg.net().findTransition(transitionId);
    if (!t || !db.runOpen())
        return false;
    rg.expand(db.currentState());
    const auto e = rg.findEdge(db.currentState(), *t);
    return e && db.step(*e);
}

static std::vector<const char *> kJamPath = {"open_cmd", "opened", "close_cmd", "jam"};
static std::vector<const char *> kCleanPath = {"open_cmd", "opened", "close_cmd", "closed_ev"};

static bool runPath(CoverageDb &db, ReachabilityGraph &rg, const std::vector<const char *> &path)
{
    for (const char *t : path)
        if (!fireNamed(db, rg, t))
            return false;
    return true;
}

// ---------------------------------------------------------------------------
// Net policy helpers and fingerprint
// ---------------------------------------------------------------------------

static void testEffectiveTargetHits()
{
    NamedMarking m;
    CHECK(effectiveTargetHits(m, 5) == 5);  // until_covered, no explicit target: global threshold
    m.targetHits = 3;
    CHECK(effectiveTargetHits(m, 5) == 3);
    m.policy = TargetPolicy::Once;
    CHECK(effectiveTargetHits(m, 5) == 1);  // once ignores target_hits
    m.policy = TargetPolicy::Avoid;
    CHECK(effectiveTargetHits(m, 5) == 0);  // satisfied from the start
}

static void testFingerprintIgnoresOrderAndIrrelevantParts()
{
    const auto build = [](bool reversed, TokenCount weight, bool withError) {
        NetBuilder b;
        PlaceIndex a, c;
        if (reversed) {
            c = b.place("c");
            a = b.place("a");
        } else {
            a = b.place("a");
            c = b.place("c");
        }
        const auto t = b.transition("t", reversed ? TransitionKind::Internal : TransitionKind::Controllable,
                                    reversed ? "" : "go");
        b.input(a, t, weight).output(t, c);
        b.initial("start", {{a, 2}});
        if (withError)
            b.errorMarking("e", {{c, 1}});
        return b.build();
    };
    const std::string base = graphFingerprint(build(false, 1, false));
    CHECK(base.size() == 16);
    CHECK(base == graphFingerprint(build(true, 1, true)));   // order, kind, label, error markings
    CHECK(base != graphFingerprint(build(false, 2, false)));  // an arc weight changes the graph
}

// ---------------------------------------------------------------------------
// Reverse index
// ---------------------------------------------------------------------------

static void testReverseIndexDepthLimitedWalk()
{
    NetBuilder b;
    std::vector<PlaceIndex> p;
    for (int i = 0; i < 8; ++i)
        p.push_back(b.place("p" + std::to_string(i)));
    for (int i = 0; i < 7; ++i) {
        const auto t = b.transition("t" + std::to_string(i));
        b.input(p[i], t).output(t, p[i + 1]);
    }
    b.initial("s", {{p[0], 1}});
    const Net net = b.build();

    ReachabilityGraph g(net);
    g.addInitial(0);
    g.exploreAll();
    CHECK(g.numStates() == 8);  // chain S0..S7, ids in discovery order

    ReverseIndex ri(g);
    ri.sync();
    CHECK(ri.numReverseEdges() == 7);
    CHECK(ri.predecessors(0).empty());

    std::vector<unsigned> depths;
    std::vector<StateId> froms;
    ri.backwardWalk(7, 5, [&](EdgeId, StateId from, StateId, unsigned d) {
        depths.push_back(d);
        froms.push_back(from);
    });
    CHECK((depths == std::vector<unsigned>{0, 1, 2, 3, 4}));
    CHECK(froms.size() == 5 && froms.back() == 2);  // five nodes behind S7: S6..S2

    int n7 = 0, n100 = 0;
    ri.backwardWalk(7, 7, [&](EdgeId, StateId, StateId, unsigned) { ++n7; });
    ri.backwardWalk(7, 100, [&](EdgeId, StateId, StateId, unsigned) { ++n100; });
    CHECK(n7 == 7 && n100 == 7);

    int n0 = 0;
    ri.backwardWalk(7, 0, [&](EdgeId, StateId, StateId, unsigned) { ++n0; });
    CHECK(n0 == 0);
}

static void testReverseIndexIsIncremental()
{
    NetBuilder b;
    const auto p = b.place("p");
    const auto q = b.place("q");
    const auto r = b.place("r");
    const auto t = b.transition("t");
    const auto u = b.transition("u");
    b.input(p, t).output(t, q);
    b.input(q, u).output(u, r);
    b.initial("s", {{p, 1}});
    const Net net = b.build();

    ReachabilityGraph g(net);
    g.addInitial(0);
    ReverseIndex ri(g);

    g.expand(0);
    ri.sync();
    CHECK(ri.predecessors(1).size() == 1);
    CHECK(ri.predecessors(1)[0].from == 0);
    CHECK(ri.predecessors(1)[0].edge == g.firstEdge(0));

    g.expand(1);
    CHECK(ri.numReverseEdges() == 1);  // not synced yet
    ri.sync();
    CHECK(ri.numReverseEdges() == 2);
    ri.sync();  // idempotent
    CHECK(ri.numReverseEdges() == 2);
}

static void testReverseWalkVisitsDiamondOnce()
{
    // x,y -> a: x->z ; b: y->z. Both orders meet in the marking {0,0,2}.
    NetBuilder b;
    const auto x = b.place("x");
    const auto y = b.place("y");
    const auto z = b.place("z");
    const auto a = b.transition("a");
    const auto bt = b.transition("b");
    b.input(x, a).output(a, z);
    b.input(y, bt).output(bt, z);
    b.initial("s", {{x, 1}, {y, 1}});
    const Net net = b.build();

    ReachabilityGraph g(net);
    g.addInitial(0);
    g.exploreAll();
    CHECK(g.numStates() == 4);

    ReverseIndex ri(g);
    ri.sync();
    const StateId join = 3;
    CHECK(ri.predecessors(join).size() == 2);

    int edges = 0;
    ri.backwardWalk(join, 5, [&](EdgeId, StateId, StateId, unsigned) { ++edges; });
    CHECK(edges == 4);  // every edge of the diamond exactly once
}

// ---------------------------------------------------------------------------
// Trace tree
// ---------------------------------------------------------------------------

static void testTraceTreeSharesPrefixes()
{
    TraceTree tree;
    const NodeId a = tree.child(kRootNode, kNoEdge, 0);
    const NodeId b = tree.child(a, 10, 1);
    const NodeId c1 = tree.child(b, 11, 2);
    const NodeId c2 = tree.child(b, 12, 3);
    const NodeId again = tree.child(kRootNode, kNoEdge, 0);

    CHECK(again == a);
    CHECK(tree.node(a).visits == 2);
    CHECK(tree.size() == 5);  // root + a + b + c1 + c2
    CHECK(c1 != c2);

    const auto path = tree.path(c1);
    CHECK(path.size() == 3);
    CHECK(path[0].state == 0 && path[0].via == kNoEdge);
    CHECK(path[2].state == 2 && path[2].via == 11);

    const auto tail = tree.path(c1, 2);  // the last two steps only
    CHECK(tail.size() == 2 && tail[0].state == 1);
    CHECK(tree.path(kRootNode).empty());
}

// ---------------------------------------------------------------------------
// CoverageDb
// ---------------------------------------------------------------------------

static void testCoverageMergesRuns()
{
    ErrorOptions jammed;
    jammed.targetHits = 2;
    jammed.crashDump = DumpMode::Snapshot;
    jammed.maxDumps = 1;
    const Net net = testmodels::makeElevatorNet(jammed);

    ReachabilityGraph rg(net);
    const StateId start = rg.addInitial(0);
    rg.exploreAll();
    CHECK(rg.numStates() == 6 && rg.numEdges() == 7);

    CoverageConfig cfg;
    cfg.threshold = 2;
    CoverageDb db(rg, cfg);

    // --- run 0: a clean round trip -------------------------------------------
    const RunId r0 = db.beginRun(start, "clean");
    CHECK(r0 == 0);
    CHECK(runPath(db, rg, kCleanPath));
    db.endRun(RunOutcome::Completed);

    CHECK(db.run(r0).steps.size() == 4);
    CHECK(db.run(r0).newStates == 4);  // closed, opening, open, closing
    CHECK(db.run(r0).newEdges == 4);
    CHECK(db.state(start).definite == 2);  // initial visit + return
    CHECK(db.targetMarkingHits(0) == 2);

    CoverageSummary s = db.summary();
    CHECK(s.statesKnown == 6 && s.statesCovered == 4);
    CHECK(s.edgesKnown == 7 && s.edgesCovered == 4);
    CHECK(s.transitionsTotal == 7 && s.transitionsCovered == 4);
    CHECK(s.placesTotal == 6 && s.placesCovered == 4);  // closed, opening, open, closing

    // --- run 1: ends in the error marking -------------------------------------
    db.beginRun(start, "jam");
    CHECK(runPath(db, rg, kJamPath));
    CHECK(db.errorMarkingHits(0) == 1);
    CHECK(!db.isErrorSatisfied(0));  // target is 2
    CHECK(db.dumpAllowed(0));
    db.recordDump(DumpMode::Snapshot, {"core.1"}, "frame #0 jam()");
    CHECK(!db.dumpAllowed(0));  // max_dumps = 1
    CHECK(db.errorMarkingDumps(0) == 1);
    db.endRun(RunOutcome::ErrorReached, "jammed");

    CHECK(db.run(1).dumps.size() == 1);
    CHECK(db.run(1).dumps[0].stepIndex == 4);
    CHECK(db.run(1).dumps[0].marking == "jammed");
    CHECK(db.run(1).newStates == 1 && db.run(1).newEdges == 1);  // only the fault state and "jam"

    const StateId fault = rg.findEdge(db.run(1).steps[2].target, *net.findTransition("jam"))
                              ? rg.edge(*rg.findEdge(db.run(1).steps[2].target, *net.findTransition("jam"))).target
                              : kNoState;
    CHECK(fault != kNoState);
    const auto bt = db.stateBacktrace(fault);
    CHECK(bt.size() == 5);  // initial + 4 steps
    CHECK(bt.front().via == kNoEdge && bt.front().state == start);
    CHECK(bt.back().state == fault);
    CHECK(rg.edge(bt.back().via).transition == *net.findTransition("jam"));
    CHECK(db.stateBacktrace(fault, CoverageDb::Which::Last, 2).size() == 2);

    // --- run 2: the error again, which satisfies the target ---------------------
    db.beginRun(start);
    CHECK(runPath(db, rg, kJamPath));
    CHECK(db.errorMarkingHits(0) == 2);
    CHECK(db.isErrorSatisfied(0));
    CHECK(!db.dumpAllowed(0));
    db.endRun(RunOutcome::ErrorReached, "jammed");

    // --- merged figures ------------------------------------------------------
    CHECK(db.numRuns() == 3);
    CHECK(db.state(start).definite == 4);  // 2 + 1 + 1 initial visits; only run 0 returned
    CHECK(db.state(start).firstRun == 0 && db.state(start).lastRun == 2);
    const EdgeId openCmd = *rg.findEdge(start, *net.findTransition("open_cmd"));
    CHECK(db.edge(openCmd).definite == 3);
    CHECK(db.isSaturatedEdge(openCmd));            // 3 >= threshold 2
    CHECK(db.transitionHits(*net.findTransition("jam")) == 2);
    const EdgeId obstructEdge = [&] {
        for (EdgeId e = 0; e < rg.numEdges(); ++e)
            if (rg.edge(e).transition == *net.findTransition("obstruct"))
                return e;
        return kNoEdge;
    }();
    CHECK(obstructEdge != kNoEdge && !db.isSaturatedEdge(obstructEdge));

    // Three runs shared the prefix open_cmd, opened, close_cmd:
    // root + initial + 3 shared steps + closed_ev + jam.
    CHECK(db.traceTree().size() == 7);
}

static void testStepRejectsForeignEdges()
{
    const Net net = testmodels::makeElevatorNet();
    ReachabilityGraph rg(net);
    const StateId start = rg.addInitial(0);
    rg.exploreAll();

    CoverageDb db(rg);
    CHECK(!db.step(0));  // no open run
    db.beginRun(start);
    // The "jam" edge does not leave the start state.
    EdgeId jamEdge = kNoEdge;
    for (EdgeId e = 0; e < rg.numEdges(); ++e)
        if (rg.edge(e).transition == *net.findTransition("jam"))
            jamEdge = e;
    CHECK(jamEdge != kNoEdge);
    CHECK(!db.step(jamEdge));
    CHECK(db.run(0).steps.empty());
    CHECK(db.currentState() == start);
    db.endRun(RunOutcome::Completed);
    CHECK(!db.runOpen());
}

static void testBeginRunSupersedesOpenRun()
{
    const Net net = testmodels::makeElevatorNet();
    ReachabilityGraph rg(net);
    const StateId start = rg.addInitial(0);
    rg.exploreAll();

    CoverageDb db(rg);
    db.beginRun(start);
    db.beginRun(start);
    CHECK(db.numRuns() == 2);
    CHECK(db.run(0).outcome == RunOutcome::Aborted);
    CHECK(db.run(1).outcome == RunOutcome::Open);
}

static void testDumpPolicies()
{
    // No crash_dump set: a spec/protocol fault, never a dump.
    {
        const Net net = testmodels::makeElevatorNet();
        ReachabilityGraph rg(net);
        rg.addInitial(0);
        CoverageDb db(rg);
        CHECK(!db.dumpAllowed(0));
    }
    // Avoid: satisfied from the start, so the tester never seeks it.
    {
        ErrorOptions o;
        o.policy = TargetPolicy::Avoid;
        const Net net = testmodels::makeElevatorNet(o);
        ReachabilityGraph rg(net);
        rg.addInitial(0);
        CoverageDb db(rg);
        CHECK(db.errorTarget(0) == 0);
        CHECK(db.isErrorSatisfied(0));
    }
    // Once: satisfied after the first definite visit, regardless of the threshold.
    {
        ErrorOptions o;
        o.policy = TargetPolicy::Once;
        o.targetHits = 9;
        const Net net = testmodels::makeElevatorNet(o);
        ReachabilityGraph rg(net);
        const StateId start = rg.addInitial(0);
        rg.exploreAll();
        CoverageConfig cfg;
        cfg.threshold = 5;
        CoverageDb db(rg, cfg);
        CHECK(db.errorTarget(0) == 1);
        db.beginRun(start);
        CHECK(runPath(db, rg, kJamPath));
        CHECK(db.isErrorSatisfied(0));
    }
}

static void testAmbiguousCountersAreSeparate()
{
    const Net net = testmodels::makeElevatorNet();
    ReachabilityGraph rg(net);
    const StateId start = rg.addInitial(0);
    rg.exploreAll();
    CoverageDb db(rg);
    db.addAmbiguousState(start);
    db.addAmbiguousEdge(0);
    CHECK(db.state(start).ambiguous == 1 && db.state(start).definite == 0);
    CHECK(db.edge(0).ambiguous == 1 && db.edge(0).definite == 0);
    CHECK(db.summary().statesCovered == 0);  // ambiguity alone is not coverage
}

// ---------------------------------------------------------------------------
// Persistence and replay
// ---------------------------------------------------------------------------

static void testEnumStringRoundTrips()
{
    for (RunOutcome o : {RunOutcome::Completed, RunOutcome::Deadlock, RunOutcome::Livelock,
                         RunOutcome::ErrorReached, RunOutcome::Deviation, RunOutcome::SutCrashed,
                         RunOutcome::Aborted}) {
        RunOutcome back = RunOutcome::Open;
        CHECK(parseOutcome(toString(o), back) && back == o);
    }
    RunOutcome x;
    CHECK(!parseOutcome("open", x));
    CHECK(!parseOutcome("nonsense", x));
    for (DumpMode m : {DumpMode::None, DumpMode::Snapshot, DumpMode::Crash}) {
        DumpMode back = DumpMode::None;
        CHECK(parseDumpMode(toString(m), back) && back == m);
    }
}

static void testReplayReproducesCoverage()
{
    ErrorOptions jammed;
    jammed.targetHits = 2;
    jammed.crashDump = DumpMode::Snapshot;
    jammed.maxDumps = 1;
    const Net net = testmodels::makeElevatorNet(jammed);

    // Record three runs live and persist them.
    std::vector<PersistedRun> saved;
    CoverageSummary live;
    {
        ReachabilityGraph rg(net);
        const StateId start = rg.addInitial(0);
        rg.exploreAll();
        CoverageDb db(rg);

        db.beginRun(start, "clean");
        runPath(db, rg, kCleanPath);
        db.endRun(RunOutcome::Completed);

        db.beginRun(start);
        runPath(db, rg, kJamPath);
        db.recordDump(DumpMode::Snapshot, {"core.1"}, "stack\nwith \"quotes\"");
        db.endRun(RunOutcome::ErrorReached, "jammed");

        db.beginRun(start);
        runPath(db, rg, kJamPath);
        db.endRun(RunOutcome::ErrorReached, "jammed");

        for (RunId i = 0; i < db.numRuns(); ++i)
            saved.push_back(toPersisted(rg, db.run(i)));
        live = db.summary();
    }
    CHECK(saved.size() == 3);
    CHECK(saved[1].initialMarking == "start");
    CHECK((saved[1].transitions == std::vector<std::string>{"open_cmd", "opened", "close_cmd", "jam"}));
    CHECK(saved[1].dumps.size() == 1 && saved[1].dumps[0].stepIndex == 4);

    // Replay into a fresh, completely unexplored graph.
    ReachabilityGraph rg2(net);
    CoverageDb db2(rg2);
    for (const PersistedRun &pr : saved) {
        const ReplayResult r = replay(db2, rg2, pr);
        CHECK(r.ok);
    }
    CHECK(db2.numRuns() == 3);
    CHECK(db2.errorMarkingHits(0) == 2);
    CHECK(db2.errorMarkingDumps(0) == 1);
    CHECK(!db2.dumpAllowed(0));
    CHECK(db2.run(1).dumps.size() == 1);
    CHECK(db2.run(1).dumps[0].stackText == "stack\nwith \"quotes\"");
    CHECK(db2.run(2).outcome == RunOutcome::ErrorReached);

    const CoverageSummary s2 = db2.summary();
    CHECK(s2.statesCovered == live.statesCovered);
    CHECK(s2.edgesCovered == live.edgesCovered);
    CHECK(s2.transitionsCovered == live.transitionsCovered);
    CHECK(s2.placesCovered == live.placesCovered);
    CHECK(live.statesCovered == 5);  // closed, opening, open, closing, fault; "open+obstructed" is known but unvisited
}

static void testReplayRejectsBadRunsAtomically()
{
    const Net net = testmodels::makeElevatorNet();
    ReachabilityGraph rg(net);
    CoverageDb db(rg);

    PersistedRun unknownInitial;
    unknownInitial.initialMarking = "nope";
    CHECK(!replay(db, rg, unknownInitial).ok);

    PersistedRun unknownTransition;
    unknownTransition.initialMarking = "start";
    unknownTransition.transitions = {"open_cmd", "teleport"};
    const ReplayResult r1 = replay(db, rg, unknownTransition);
    CHECK(!r1.ok && r1.error.find("step 1") != std::string::npos);

    PersistedRun disabled;
    disabled.initialMarking = "start";
    disabled.transitions = {"close_cmd"};  // nothing to close at the start
    const ReplayResult r2 = replay(db, rg, disabled);
    CHECK(!r2.ok && r2.error.find("step 0") != std::string::npos);

    PersistedRun badDumps;
    badDumps.initialMarking = "start";
    badDumps.dumps.push_back(DumpRecord{3, "", DumpMode::Snapshot, {}, ""});  // beyond the run
    CHECK(!replay(db, rg, badDumps).ok);

    CHECK(db.numRuns() == 0);  // nothing was recorded by any rejected replay
    CHECK(db.summary().statesCovered == 0);
}

// ---------------------------------------------------------------------------

int main()
{
    const std::vector<std::pair<const char *, std::function<void()>>> tests = {
        {"effectiveTargetHits", testEffectiveTargetHits},
        {"fingerprintIgnoresOrderAndIrrelevantParts", testFingerprintIgnoresOrderAndIrrelevantParts},
        {"reverseIndexDepthLimitedWalk", testReverseIndexDepthLimitedWalk},
        {"reverseIndexIsIncremental", testReverseIndexIsIncremental},
        {"reverseWalkVisitsDiamondOnce", testReverseWalkVisitsDiamondOnce},
        {"traceTreeSharesPrefixes", testTraceTreeSharesPrefixes},
        {"coverageMergesRuns", testCoverageMergesRuns},
        {"stepRejectsForeignEdges", testStepRejectsForeignEdges},
        {"beginRunSupersedesOpenRun", testBeginRunSupersedesOpenRun},
        {"dumpPolicies", testDumpPolicies},
        {"ambiguousCountersAreSeparate", testAmbiguousCountersAreSeparate},
        {"enumStringRoundTrips", testEnumStringRoundTrips},
        {"replayReproducesCoverage", testReplayReproducesCoverage},
        {"replayRejectsBadRunsAtomically", testReplayRejectsBadRunsAtomically},
    };
    for (const auto &t : tests) {
        const int before = g_failures;
        t.second();
        std::printf("%s %s\n", g_failures == before ? "PASS" : "FAIL", t.first);
    }
    std::printf("%d failure(s)\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
