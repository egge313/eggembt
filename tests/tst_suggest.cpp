// Tests for the TargetBoard (phases of error/deadlock/livelock targets) and the
// Suggester (coverage-directed choice of the next input).
#include "ElevatorModel.h"
#include "engine/BeliefTracker.h"
#include "engine/CoverageDb.h"
#include "engine/LivelockAnalysis.h"
#include "engine/ReachabilityGraph.h"
#include "engine/Suggester.h"
#include "engine/TargetBoard.h"
#include "model/NetBuilder.h"

#include <algorithm>
#include <cstdio>
#include <functional>
#include <set>
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

static SutEvent obs(const char *label) { return SutEvent{EventKind::Observed, label, 0}; }
static SutEvent stim(const char *label) { return SutEvent{EventKind::Stimulated, label, 0}; }

static StateId findState(const ReachabilityGraph &g, const Marking &m)
{
    for (StateId s = 0; s < g.numStates(); ++s)
        if (g.marking(s) == m)
            return s;
    return kNoState;
}

// Follows transitions by id through the db's path-mode run API.
static bool firePath(CoverageDb &db, ReachabilityGraph &rg, const std::vector<const char *> &ids)
{
    for (const char *id : ids) {
        const auto t = rg.net().findTransition(id);
        if (!t || !db.runOpen())
            return false;
        rg.expand(db.currentState());
        const auto e = rg.findEdge(db.currentState(), *t);
        if (!e || !db.step(*e))
            return false;
    }
    return true;
}

// One complete path-mode run from initial marking `initial`.
static void pathRun(CoverageDb &db, ReachabilityGraph &rg, std::size_t initial,
                    const std::vector<const char *> &ids)
{
    db.beginRun(rg.addInitial(initial));
    CHECK(firePath(db, rg, ids));
    db.endRun(RunOutcome::Completed);
}

static const std::vector<const char *> kJam = {"open_cmd", "opened", "close_cmd", "jam"};

// ---------------------------------------------------------------------------
// TargetBoard
// ---------------------------------------------------------------------------

static void testErrorMarkingsSeekThenForbid()
{
    ErrorOptions jammed;
    jammed.targetHits = 2;
    const Net net = testmodels::makeElevatorNet(jammed);
    ReachabilityGraph rg(net);
    rg.addInitial(0);
    rg.exploreAll();
    CoverageDb db(rg);
    TargetBoard board(rg, db);
    const StateId fault = findState(rg, Marking{0, 0, 0, 0, 0, 1});

    board.refresh();
    CHECK(board.classOf(fault) == TargetBoard::StateClass::Seeking);
    CHECK(board.errorMarking(0).phase == TargetPhase::Seeking && board.errorMarking(0).target == 2);

    pathRun(db, rg, 0, kJam);
    board.refresh();
    CHECK(board.classOf(fault) == TargetBoard::StateClass::Seeking);  // 1 of 2
    CHECK(board.errorMarking(0).hits == 1);

    pathRun(db, rg, 0, kJam);
    board.refresh();
    CHECK(board.classOf(fault) == TargetBoard::StateClass::Forbidden);
    CHECK(board.errorMarking(0).phase == TargetPhase::Satisfied);
    CHECK(board.forbidden(fault));
}

static void testErrorPoliciesAvoidAndOnce()
{
    {
        ErrorOptions o;
        o.policy = TargetPolicy::Avoid;
        const Net net = testmodels::makeElevatorNet(o);
        ReachabilityGraph rg(net);
        rg.addInitial(0);
        rg.exploreAll();
        CoverageDb db(rg);
        TargetBoard board(rg, db);
        board.refresh();
        CHECK(board.forbidden(findState(rg, Marking{0, 0, 0, 0, 0, 1})));  // forbidden from the start
    }
    {
        ErrorOptions o;
        o.policy = TargetPolicy::Once;
        o.targetHits = 9;
        const Net net = testmodels::makeElevatorNet(o);
        ReachabilityGraph rg(net);
        rg.addInitial(0);
        rg.exploreAll();
        CoverageDb db(rg);
        TargetBoard board(rg, db);
        const StateId fault = findState(rg, Marking{0, 0, 0, 0, 0, 1});
        board.refresh();
        CHECK(!board.forbidden(fault));
        pathRun(db, rg, 0, kJam);
        board.refresh();
        CHECK(board.forbidden(fault));
    }
}

static void testDeadlockPolicies()
{
    const auto makeNet = [] {
        NetBuilder b;
        const auto p = b.place("p");
        const auto q = b.place("q");
        const auto t = b.transition("t");
        b.input(p, t).output(t, q);
        b.initial("s", {{p, 1}});
        return b.build();
    };
    const Net net = makeNet();
    const Marking dead{0, 1};

    {  // default: once
        ReachabilityGraph rg(net);
        rg.addInitial(0);
        rg.exploreAll();
        CoverageDb db(rg);
        TargetBoard board(rg, db);
        board.refresh();
        CHECK(board.classOf(findState(rg, dead)) == TargetBoard::StateClass::Seeking);
        pathRun(db, rg, 0, {"t"});
        board.refresh();
        CHECK(board.forbidden(findState(rg, dead)));
        CHECK(board.deadlocks().hits == 1 && board.deadlocks().target == 1);
    }
    {  // avoid
        ReachabilityGraph rg(net);
        rg.addInitial(0);
        rg.exploreAll();
        CoverageDb db(rg);
        TargetPolicies pol;
        pol.deadlock = TargetPolicy::Avoid;
        TargetBoard board(rg, db, pol);
        board.refresh();
        CHECK(board.forbidden(findState(rg, dead)));
    }
    {  // until_covered with an explicit count of 2
        ReachabilityGraph rg(net);
        rg.addInitial(0);
        rg.exploreAll();
        CoverageDb db(rg);
        TargetPolicies pol;
        pol.deadlock = TargetPolicy::UntilCovered;
        pol.deadlockHits = 2;
        TargetBoard board(rg, db, pol);
        pathRun(db, rg, 0, {"t"});
        board.refresh();
        CHECK(!board.forbidden(findState(rg, dead)));
        pathRun(db, rg, 0, {"t"});
        board.refresh();
        CHECK(board.forbidden(findState(rg, dead)));
    }
}

// s0 -cycle-> q <-> r (terminal cycle), s0 -safe-> s1 -back-> s0
static Net makeLivelockNet()
{
    NetBuilder b;
    const auto s0 = b.place("s0");
    const auto s1 = b.place("s1");
    const auto q = b.place("q");
    const auto r = b.place("r");
    const auto cycle = b.transition("cycle", TransitionKind::Controllable, "cycle");
    const auto u = b.transition("u", TransitionKind::Observable, "u");
    const auto v = b.transition("v", TransitionKind::Observable, "v");
    const auto safe = b.transition("safe", TransitionKind::Controllable, "safe");
    const auto back = b.transition("back", TransitionKind::Observable, "back");
    b.input(s0, cycle).output(cycle, q);
    b.input(q, u).output(u, r);
    b.input(r, v).output(v, q);
    b.input(s0, safe).output(safe, s1);
    b.input(s1, back).output(back, s0);
    b.initial("start", {{s0, 1}});
    return b.build();
}

static void testLivelockPhases()
{
    const Net net = makeLivelockNet();
    ReachabilityGraph rg(net);
    rg.addInitial(0);
    rg.exploreAll();
    const LivelockReport report = analyzeLivelocks(rg);
    CHECK(report.findings.size() == 1 && report.findings[0].certain);

    CoverageDb db(rg);
    TargetBoard board(rg, db);
    board.setLivelockFindings(report.findings);
    board.refresh();
    CHECK(board.numLivelocks() == 1);
    // Marking order: s0, s1, q, r.
    const StateId q = findState(rg, Marking{0, 0, 1, 0});
    const StateId r = findState(rg, Marking{0, 0, 0, 1});
    CHECK(board.classOf(q) == TargetBoard::StateClass::Seeking);
    CHECK(board.classOf(r) == TargetBoard::StateClass::Seeking);

    pathRun(db, rg, 0, {"cycle", "u"});  // visits q and r
    board.refresh();
    CHECK(board.livelock(0).phase == TargetPhase::Satisfied);
    CHECK(board.forbidden(q) && board.forbidden(r));

    // Uncertain findings and internal cycles are not targets.
    std::vector<LivelockFinding> weak = {
        {LivelockKind::TerminalCycle, {q, r}, false},
        {LivelockKind::InternalCycle, {q, r}, true},
    };
    board.setLivelockFindings(weak);
    board.refresh();
    CHECK(board.numLivelocks() == 0);
    CHECK(!board.forbidden(q));
}

// ---------------------------------------------------------------------------
// Suggester
// ---------------------------------------------------------------------------

static void testFirstSuggestionAndWaiting()
{
    const Net net = testmodels::makeElevatorNet();
    ReachabilityGraph rg(net);
    CoverageDb db(rg);
    BeliefTracker tr(rg, &db);
    Suggester sg(rg, db);

    tr.beginRun(0);
    Suggestion s = sg.suggest(tr);
    CHECK(s.status == SuggestionStatus::Suggest && s.label == "door.open");
    CHECK(s.robustness == 1.0 && s.hazard == 0.0);
    CHECK(s.goal == GoalTier::NewTransition && s.steps == 1);

    tr.onEvent(stim("door.open"));
    s = sg.suggest(tr);
    CHECK(s.status == SuggestionStatus::Wait);  // only the SUT can move on

    tr.onEvent(obs("door.opened"));
    CHECK(tr.belief().size() == 2);  // open, or open + silently obstructed
    s = sg.suggest(tr);
    CHECK(s.status == SuggestionStatus::Suggest && s.label == "door.close");
    // Enabled in one of two possible states, and the silent "obstruct" step would disable it.
    CHECK(s.robustness == 0.0);
    tr.endRun(RunOutcome::Completed);
}

// s0 -x-> s1 -ox-> s0 and s0 -y-> s2 -oy-> s0
static Net makeTwoBranchNet()
{
    NetBuilder b;
    const auto s0 = b.place("s0");
    const auto s1 = b.place("s1");
    const auto s2 = b.place("s2");
    const auto x = b.transition("x", TransitionKind::Controllable, "x");
    const auto ox = b.transition("ox", TransitionKind::Observable, "ox");
    const auto y = b.transition("y", TransitionKind::Controllable, "y");
    const auto oy = b.transition("oy", TransitionKind::Observable, "oy");
    b.input(s0, x).output(x, s1);
    b.input(s1, ox).output(ox, s0);
    b.input(s0, y).output(y, s2);
    b.input(s2, oy).output(oy, s0);
    b.initial("start", {{s0, 1}});
    return b.build();
}

static void testPrefersUncoveredAndFinishesWithDone()
{
    const Net net = makeTwoBranchNet();
    ReachabilityGraph rg(net);
    rg.addInitial(0);
    rg.exploreAll();
    CoverageDb db(rg);
    Suggester sg(rg, db);

    pathRun(db, rg, 0, {"x", "ox"});

    BeliefTracker tr(rg, &db);
    tr.beginRun(0);
    Suggestion s = sg.suggest(tr);
    CHECK(s.status == SuggestionStatus::Suggest && s.label == "y");  // x is covered, y is not
    CHECK(s.steps == 1);
    tr.endRun(RunOutcome::Completed);

    pathRun(db, rg, 0, {"y", "oy"});
    tr.beginRun(0);
    s = sg.suggest(tr);
    CHECK(s.status == SuggestionStatus::Done);
    tr.endRun(RunOutcome::Completed);
}

static void testVetoOfAnAvoidedError()
{
    // s -go-> e (error), s -safe-> t -back-> s
    const auto build = [](TargetPolicy policy) {
        NetBuilder b;
        const auto s = b.place("s");
        const auto e = b.place("e");
        const auto t = b.place("t");
        const auto go = b.transition("go", TransitionKind::Controllable, "go");
        const auto safe = b.transition("safe", TransitionKind::Controllable, "safe");
        const auto back = b.transition("back", TransitionKind::Observable, "back");
        b.input(s, go).output(go, e);
        b.input(s, safe).output(safe, t);
        b.input(t, back).output(back, s);
        b.initial("start", {{s, 1}});
        ErrorOptions o;
        o.policy = policy;
        o.targetHits = 1;
        b.errorMarking("bad", {{e, 1}}, MatchMode::Exact, o);
        return b.build();
    };

    {  // avoid: "go" is refused from the start
        const Net net = build(TargetPolicy::Avoid);
        ReachabilityGraph rg(net);
        rg.addInitial(0);
        rg.exploreAll();
        CoverageDb db(rg);
        Suggester sg(rg, db);
        BeliefTracker tr(rg, &db);
        tr.beginRun(0);
        const Suggestion s = sg.suggest(tr);
        CHECK(s.status == SuggestionStatus::Suggest && s.label == "safe");
        CHECK((s.vetoed == std::vector<std::string>{"go"}));
        tr.endRun(RunOutcome::Completed);
    }
    {  // until_covered: sought first, avoided once reached
        const Net net = build(TargetPolicy::UntilCovered);
        ReachabilityGraph rg(net);
        rg.addInitial(0);
        rg.exploreAll();
        CoverageDb db(rg);
        Suggester sg(rg, db);
        pathRun(db, rg, 0, {"safe", "back"});  // cover the harmless branch

        BeliefTracker tr(rg, &db);
        tr.beginRun(0);
        Suggestion s = sg.suggest(tr);
        CHECK(s.status == SuggestionStatus::Suggest && s.label == "go");  // the error is a goal
        CHECK(s.vetoed.empty());
        tr.endRun(RunOutcome::Completed);

        pathRun(db, rg, 0, {"go"});  // reached once: target satisfied
        tr.beginRun(0);
        s = sg.suggest(tr);
        CHECK(s.status == SuggestionStatus::Done);  // nothing else is left
        CHECK((s.vetoed == std::vector<std::string>{"go"}));
        tr.endRun(RunOutcome::Completed);
    }
}

static void testStuckWhenGoalsAreUnreachable()
{
    // Initial marking 0: s0 -go-> e (avoided error). Initial marking 1: z -k-> z2, never reachable from 0.
    NetBuilder b;
    const auto s0 = b.place("s0");
    const auto e = b.place("e");
    const auto z = b.place("z");
    const auto z2 = b.place("z2");
    const auto go = b.transition("go", TransitionKind::Controllable, "go");
    const auto k = b.transition("k", TransitionKind::Controllable, "k");
    b.input(s0, go).output(go, e);
    b.input(z, k).output(k, z2);
    b.initial("main", {{s0, 1}});
    b.initial("island", {{z, 1}});
    ErrorOptions o;
    o.policy = TargetPolicy::Avoid;
    b.errorMarking("bad", {{e, 1}}, MatchMode::Exact, o);
    const Net net = b.build();

    ReachabilityGraph rg(net);
    rg.addInitial(0);
    rg.addInitial(1);
    rg.exploreAll();
    CoverageDb db(rg);
    Suggester sg(rg, db);
    BeliefTracker tr(rg, &db);
    tr.beginRun(0);
    const Suggestion s = sg.suggest(tr);
    CHECK(s.status == SuggestionStatus::Stuck);
    CHECK((s.vetoed == std::vector<std::string>{"go"}));
    tr.endRun(RunOutcome::Completed);
}

static void testAvoidsASatisfiedLivelock()
{
    const Net net = makeLivelockNet();
    ReachabilityGraph rg(net);
    rg.addInitial(0);
    rg.exploreAll();
    CoverageDb db(rg);
    Suggester sg(rg, db);
    sg.setLivelockFindings(analyzeLivelocks(rg).findings);

    pathRun(db, rg, 0, {"cycle", "u", "v"});  // the livelock has been confirmed once
    pathRun(db, rg, 0, {"safe"});            // "back" is still uncovered

    BeliefTracker tr(rg, &db);
    tr.beginRun(0);
    const Suggestion s = sg.suggest(tr);
    CHECK(s.status == SuggestionStatus::Suggest && s.label == "safe");
    CHECK((s.vetoed == std::vector<std::string>{"cycle"}));
    CHECK(s.steps == 2);  // safe, then the uncovered "back"
    tr.endRun(RunOutcome::Completed);
}

static void testPrefersTheMoreRobustInput()
{
    // a holds the token. A silent step "block" puts a token into blk, which
    // inhibits "fragile" but not "sturdy". "fragile" is declared first and is
    // closer to a goal on paper: its transition has never been covered, while
    // the edge of "sturdy" is covered via the second initial marking. Only the
    // risk term can make "sturdy" win.
    NetBuilder b;
    const auto a = b.place("a");
    const auto blk = b.place("blk");
    const auto f = b.place("f");
    const auto g = b.place("g");
    const auto block = b.transition("block", TransitionKind::Internal);
    const auto fragile = b.transition("fragile", TransitionKind::Controllable, "fragile");
    const auto sturdy = b.transition("sturdy", TransitionKind::Controllable, "sturdy");
    b.input(a, block).output(block, a).output(block, blk).inhibitor(blk, block);
    b.input(a, fragile).output(fragile, f).inhibitor(blk, fragile);
    b.input(a, sturdy).output(sturdy, g);
    b.initial("start", {{a, 1}});
    b.initial("blocked", {{a, 1}, {blk, 1}});
    b.targetMarking("f", {{f, 1}});
    b.targetMarking("g", {{g, 1}});
    b.targetMarking("g_blocked", {{g, 1}, {blk, 1}});
    const Net net = b.build();

    ReachabilityGraph rg(net);
    CoverageDb db(rg);
    pathRun(db, rg, 1, {"sturdy"});  // "sturdy" is now a covered transition

    SuggesterConfig cfg;
    cfg.penaltyEdge = 1;  // the uncovered edge of "sturdy" costs 2, "fragile" costs 1
    Suggester sg(rg, db, cfg);
    BeliefTracker tr(rg, &db);
    tr.beginRun(0);
    CHECK(tr.belief().size() == 2);  // a, or a + blk after the silent step

    Suggestion s = sg.suggest(tr);
    CHECK(s.status == SuggestionStatus::Suggest && s.label == "sturdy");
    CHECK(s.robustness == 1.0);
    tr.endRun(RunOutcome::Completed);

    // Without the risk term the nearer goal wins.
    cfg.riskWeight = 0.0;
    Suggester careless(rg, db, cfg);
    tr.beginRun(0);
    s = careless.suggest(tr);
    CHECK(s.status == SuggestionStatus::Suggest && s.label == "fragile");
    CHECK(s.robustness == 0.0);  // enabled in 1 of 2 possible states, and "block" disables it
    tr.endRun(RunOutcome::Completed);
}

static void testPlansNeverPassThroughForbiddenStates()
{
    // s0 -a-> m -mm-> e (avoided error) -q-> z -k-> z2. The only uncovered edge
    // (k) lies behind the forbidden state e, so it is out of reach.
    NetBuilder b;
    const auto s0 = b.place("s0");
    const auto m = b.place("m");
    const auto e = b.place("e");
    const auto z = b.place("z");
    const auto z2 = b.place("z2");
    const auto a = b.transition("a", TransitionKind::Controllable, "a");
    const auto mm = b.transition("mm", TransitionKind::Observable, "mm");
    const auto q = b.transition("q", TransitionKind::Observable, "q");
    const auto k = b.transition("k", TransitionKind::Controllable, "k");
    b.input(s0, a).output(a, m);
    b.input(m, mm).output(mm, e);
    b.input(e, q).output(q, z);
    b.input(z, k).output(k, z2);
    b.initial("start", {{s0, 1}});
    b.targetMarking("end", {{z2, 1}});
    ErrorOptions o;
    o.policy = TargetPolicy::Avoid;
    b.errorMarking("bad", {{e, 1}}, MatchMode::Exact, o);
    const Net net = b.build();

    ReachabilityGraph rg(net);
    rg.addInitial(0);
    rg.exploreAll();
    CoverageDb db(rg);
    pathRun(db, rg, 0, {"a", "mm", "q"});  // everything but k is covered

    Suggester sg(rg, db);
    BeliefTracker tr(rg, &db);
    tr.beginRun(0);
    const Suggestion s = sg.suggest(tr);
    CHECK(s.status == SuggestionStatus::Stuck);  // not "a": the way to k leads through the error
    tr.endRun(RunOutcome::Completed);
}

// DeepSeek V4.1 Flash: with the phase veto relaxed, the same model can be
// traversed: Penalise and Ignore both plan through the satisfied error to the
// still-uncovered edge behind it. The phase transition itself is unchanged.
static void testSatisfiedTargetPolicyPlansThroughTheTarget()
{
    NetBuilder b;
    const auto s0 = b.place("s0");
    const auto m = b.place("m");
    const auto e = b.place("e");
    const auto z = b.place("z");
    const auto z2 = b.place("z2");
    const auto a = b.transition("a", TransitionKind::Controllable, "a");
    const auto mm = b.transition("mm", TransitionKind::Observable, "mm");
    const auto q = b.transition("q", TransitionKind::Observable, "q");
    const auto k = b.transition("k", TransitionKind::Controllable, "k");
    b.input(s0, a).output(a, m);
    b.input(m, mm).output(mm, e);
    b.input(e, q).output(q, z);
    b.input(z, k).output(k, z2);
    b.initial("start", {{s0, 1}});
    b.targetMarking("end", {{z2, 1}});
    ErrorOptions o;
    o.policy = TargetPolicy::Avoid;
    b.errorMarking("bad", {{e, 1}}, MatchMode::Exact, o);
    const Net net = b.build();

    ReachabilityGraph rg(net);
    rg.addInitial(0);
    rg.exploreAll();
    CoverageDb db(rg);
    pathRun(db, rg, 0, {"a", "mm", "q"});  // everything but k is covered

    for (const SatisfiedTargetPolicy policy : {SatisfiedTargetPolicy::Penalise, SatisfiedTargetPolicy::Ignore}) {
        SuggesterConfig cfg;
        cfg.satisfiedTargets = policy;
        Suggester sg(rg, db, cfg);
        BeliefTracker tr(rg, &db);
        tr.beginRun(0);
        const Suggestion s = sg.suggest(tr);
        CHECK(s.status == SuggestionStatus::Suggest && s.label == "a");
        CHECK(s.vetoed.empty());
        tr.endRun(RunOutcome::Completed);
    }
}

// DeepSeek V4.1 Flash: entering a satisfied target directly is the hazard
// case. Penalise still allows the move but reports the hazard fraction;
// Ignore drops it; Veto refuses it as before.
static void testSatisfiedTargetPolicyHazard()
{
    NetBuilder b;
    const auto s0 = b.place("s0");
    const auto e = b.place("e");
    const auto z = b.place("z");
    const auto z2 = b.place("z2");
    const auto direct = b.transition("direct", TransitionKind::Controllable, "direct");
    const auto q = b.transition("q", TransitionKind::Observable, "q");
    const auto k = b.transition("k", TransitionKind::Controllable, "k");
    b.input(s0, direct).output(direct, e);
    b.input(e, q).output(q, z);
    b.input(z, k).output(k, z2);
    b.initial("start", {{s0, 1}});
    b.targetMarking("end", {{z2, 1}});
    ErrorOptions o;
    o.policy = TargetPolicy::Avoid;
    b.errorMarking("bad", {{e, 1}}, MatchMode::Exact, o);
    const Net net = b.build();

    ReachabilityGraph rg(net);
    rg.addInitial(0);
    rg.exploreAll();
    CoverageDb db(rg);

    {
        Suggester sg(rg, db);  // default: Veto
        BeliefTracker tr(rg, &db);
        tr.beginRun(0);
        const Suggestion s = sg.suggest(tr);
        CHECK(s.status == SuggestionStatus::Stuck);
        CHECK(s.vetoed.size() == 1 && s.vetoed[0] == "direct");
        tr.endRun(RunOutcome::Completed);
    }
    {
        SuggesterConfig cfg;
        cfg.satisfiedTargets = SatisfiedTargetPolicy::Penalise;
        Suggester sg(rg, db, cfg);
        BeliefTracker tr(rg, &db);
        tr.beginRun(0);
        const Suggestion s = sg.suggest(tr);
        CHECK(s.status == SuggestionStatus::Suggest && s.label == "direct");
        CHECK(s.hazard == 1.0);
        tr.endRun(RunOutcome::Completed);
    }
    {
        SuggesterConfig cfg;
        cfg.satisfiedTargets = SatisfiedTargetPolicy::Ignore;
        Suggester sg(rg, db, cfg);
        BeliefTracker tr(rg, &db);
        tr.beginRun(0);
        const Suggestion s = sg.suggest(tr);
        CHECK(s.status == SuggestionStatus::Suggest && s.label == "direct");
        CHECK(s.hazard == 0.0);
        tr.endRun(RunOutcome::Completed);
    }
}

static void testFrontierGoalsLeadIntoUnknownTerritory()
{
    // s0 -a-> s1 -b-> s2, explored lazily: after one covered run only s0 is expanded.
    NetBuilder b;
    const auto s0 = b.place("s0");
    const auto s1 = b.place("s1");
    const auto s2 = b.place("s2");
    const auto a = b.transition("a", TransitionKind::Controllable, "a");
    const auto bt = b.transition("b", TransitionKind::Controllable, "b");
    b.input(s0, a).output(a, s1);
    b.input(s1, bt).output(bt, s2);
    b.initial("start", {{s0, 1}});
    b.targetMarking("end", {{s2, 1}});
    const Net net = b.build();

    ReachabilityGraph rg(net);
    CoverageDb db(rg);
    pathRun(db, rg, 0, {"a"});  // steps into s1 without expanding it
    CHECK(rg.numExpanded() == 1);

    {   // no look-ahead: the only reason to move is the unexplored state behind "a"
        SuggesterConfig cfg;
        cfg.expansionBudget = 0;
        Suggester sg(rg, db, cfg);
        BeliefTracker tr(rg, &db);
        tr.beginRun(0);
        const Suggestion s = sg.suggest(tr);
        CHECK(s.status == SuggestionStatus::Suggest && s.label == "a");
        CHECK(s.goal == GoalTier::Frontier && s.steps == 1);
        tr.endRun(RunOutcome::Completed);
    }
    {   // with look-ahead the suggester discovers the uncovered edge "b" itself
        Suggester sg(rg, db);
        BeliefTracker tr(rg, &db);
        tr.beginRun(0);
        const Suggestion s = sg.suggest(tr);
        CHECK(s.status == SuggestionStatus::Suggest && s.label == "a");
        CHECK(s.goal == GoalTier::NewTransition && s.steps == 2);
        tr.endRun(RunOutcome::Completed);
    }
}

static void testExpansionBudgetBoundsLookahead()
{
    // s -a-> m -b-> n (dead end)
    NetBuilder b;
    const auto s = b.place("s");
    const auto m = b.place("m");
    const auto n = b.place("n");
    const auto a = b.transition("a", TransitionKind::Controllable, "a");
    const auto bt = b.transition("b", TransitionKind::Controllable, "b");
    b.input(s, a).output(a, m);
    b.input(m, bt).output(bt, n);
    b.initial("start", {{s, 1}});
    const Net net = b.build();

    for (std::size_t budget : {std::size_t(0), std::size_t(1), std::size_t(500)}) {
        ReachabilityGraph rg(net);
        CoverageDb db(rg);
        SuggesterConfig cfg;
        cfg.expansionBudget = budget;
        Suggester sg(rg, db, cfg);
        BeliefTracker tr(rg, &db);
        tr.beginRun(0);
        CHECK(rg.numExpanded() == 1);  // only the initial state, via the tracker's closure
        const Suggestion x = sg.suggest(tr);
        CHECK(x.status == SuggestionStatus::Suggest && x.label == "a");
        CHECK(rg.numExpanded() == 1 + std::min<std::size_t>(budget, 2));
        tr.endRun(RunOutcome::Completed);
    }
}

static void testCoverageDirectedRunReachesFullCoverage()
{
    // s0 offers three inputs; each is answered by an output and returns to s0.
    NetBuilder b;
    const auto s0 = b.place("s0");
    std::vector<PlaceIndex> mid;
    for (const char *name : {"s1", "s2", "s3"})
        mid.push_back(b.place(name));
    const char *inputs[] = {"a", "b", "c"};
    const char *outputs[] = {"oa", "ob", "oc"};
    for (int i = 0; i < 3; ++i) {
        const auto in = b.transition(inputs[i], TransitionKind::Controllable, inputs[i]);
        const auto out = b.transition(outputs[i], TransitionKind::Observable, outputs[i]);
        b.input(s0, in).output(in, mid[static_cast<std::size_t>(i)]);
        b.input(mid[static_cast<std::size_t>(i)], out).output(out, s0);
    }
    b.initial("start", {{s0, 1}});
    const Net net = b.build();

    ReachabilityGraph rg(net);  // lazily explored: the suggester expands ahead
    CoverageDb db(rg);
    Suggester sg(rg, db);
    BeliefTracker tr(rg, &db);
    tr.beginRun(0);

    std::vector<std::string> sent;
    SuggestionStatus last = SuggestionStatus::Stuck;
    for (int step = 0; step < 30; ++step) {
        const Suggestion s = sg.suggest(tr);
        last = s.status;
        if (s.status == SuggestionStatus::Done || s.status == SuggestionStatus::Stuck)
            break;
        if (s.status == SuggestionStatus::Suggest) {
            sent.push_back(s.label);
            CHECK(tr.onEvent(stim(s.label.c_str())).status == StepStatus::Ok);
        } else {  // Wait: the SUT answers
            const auto expected = tr.expectedLabels();
            CHECK(!expected.empty());
            CHECK(tr.onEvent(obs(expected[0].c_str())).status == StepStatus::Ok);
        }
    }
    tr.endRun(RunOutcome::Completed);

    CHECK(last == SuggestionStatus::Done);
    CHECK(sent.size() == 3);  // never repeats an input while others are still uncovered
    CHECK((std::set<std::string>(sent.begin(), sent.end()) == std::set<std::string>{"a", "b", "c"}));
    const CoverageSummary cov = db.summary();
    CHECK(cov.transitionsCovered == 6 && cov.transitionsTotal == 6);
    CHECK(cov.statesCovered == 4);
}

static void testSuggestWithoutARun()
{
    const Net net = testmodels::makeElevatorNet();
    ReachabilityGraph rg(net);
    CoverageDb db(rg);
    Suggester sg(rg, db);
    BeliefTracker tr(rg, &db);
    CHECK(sg.suggest(tr).status == SuggestionStatus::Stuck);
}

// GLM-5.3-Flash: the seeded tie-break. In the symmetric two-branch net the
// first suggestion is an exact tie: x and y both lead to a never-covered
// transition in 1 step with full robustness. Without a seed the deterministic
// last resort (lowest transition index) always picks x; with a tie-break seed
// the pick is drawn at random, but reproducibly: the same seed picks the same
// input in a fresh suggester, and different seeds do not all pick alike.
static void testSeededTieBreaksVaryAndReproduce()
{
    const Net net = makeTwoBranchNet();

    // Seed 0 is exactly the old behaviour: x wins by transition index.
    for (int repeat = 0; repeat < 3; ++repeat) {
        ReachabilityGraph rg(net);
        CoverageDb db(rg);
        SuggesterConfig cfg;
        cfg.tieBreakSeed = 0;
        Suggester sg(rg, db, cfg);
        BeliefTracker tr(rg, &db);
        tr.beginRun(0);
        const Suggestion s = sg.suggest(tr);
        CHECK(s.status == SuggestionStatus::Suggest && s.label == "x");
        tr.endRun(RunOutcome::Completed);
    }

    // The same seed reproduces the same pick in a fresh suggester.
    SuggesterConfig cfg;
    cfg.tieBreakSeed = 42;
    std::string first;
    for (int repeat = 0; repeat < 3; ++repeat) {
        ReachabilityGraph rg(net);
        CoverageDb db(rg);
        Suggester sg(rg, db, cfg);
        BeliefTracker tr(rg, &db);
        tr.beginRun(0);
        const Suggestion s = sg.suggest(tr);
        CHECK(s.status == SuggestionStatus::Suggest);
        if (repeat == 0)
            first = s.label;
        else
            CHECK(s.label == first);
        tr.endRun(RunOutcome::Completed);
    }

    // Different seeds steer different campaigns: over seeds 1..8 at least two
    // distinct first picks occur (both branches are equally good, so the seed
    // is the only thing that decides).
    std::set<std::string> picks;
    for (std::uint64_t seed = 1; seed <= 8; ++seed) {
        ReachabilityGraph rg(net);
        CoverageDb db(rg);
        SuggesterConfig seeded;
        seeded.tieBreakSeed = seed;
        Suggester sg(rg, db, seeded);
        BeliefTracker tr(rg, &db);
        tr.beginRun(0);
        const Suggestion s = sg.suggest(tr);
        CHECK(s.status == SuggestionStatus::Suggest);
        picks.insert(s.label);
        tr.endRun(RunOutcome::Completed);
    }
    CHECK(picks.size() >= 2);
}

// GLM-5.3-Flash: the robustness estimate weights the SUT's own moves by the
// offered/taken statistics (doku section 12, item 3). The corridor: the input
// "a" sits in s0, and from s0 the SUT may "keep" (back to s0: a stays enabled)
// or "kill" (into a dead state: a is disabled). What the SUT actually does --
// not just what the model allows -- decides how robust "a" is.
static void testTheRobustnessWeightsTheEnvironmentByTakeRate()
{
    NetBuilder b;
    const auto s0 = b.place("s0");
    const auto m = b.place("m");
    const auto z = b.place("z");
    const auto a = b.transition("a", TransitionKind::Controllable, "a");
    const auto keep = b.transition("keep", TransitionKind::Observable, "keep");
    const auto kill = b.transition("kill", TransitionKind::Observable, "kill");
    b.input(s0, a).output(a, m);
    b.input(s0, keep).output(keep, s0);
    b.input(s0, kill).output(kill, z);
    b.initial("start", {{s0, 1}});
    b.targetMarking("done", {{m, 1}});
    const Net net = b.build();

    // The net stays the same; only the statistics change per scenario.
    ReachabilityGraph rg(net);

    {   // No statistics: both moves rate 1.0 (never offered), so they weigh
        // as before -- keep keeps "a" enabled, kill does not: 1 of 2 survive.
        CoverageDb db(rg);
        Suggester sg(rg, db);
        BeliefTracker tr(rg, &db);
        tr.beginRun(0);
        const Suggestion s = sg.suggest(tr);
        CHECK(s.status == SuggestionStatus::Suggest && s.label == "a");
        CHECK(s.robustness == 0.5);
        tr.endRun(RunOutcome::Completed);
    }
    {   // Both moves taken: full evidence, same weights as no evidence.
        CoverageDb db(rg);
        db.recordOffered("keep");
        db.recordTaken("keep");
        db.recordOffered("kill");
        db.recordTaken("kill");
        Suggester sg(rg, db);
        BeliefTracker tr(rg, &db);
        tr.beginRun(0);
        const Suggestion s = sg.suggest(tr);
        CHECK(s.robustness == 0.5);
        tr.endRun(RunOutcome::Completed);
    }
    {   // The SUT keeps serving "keep" and never takes "kill": the only move
        // that counts preserves "a" -- fully robust.
        CoverageDb db(rg);
        db.recordOffered("keep");
        db.recordTaken("keep");
        db.recordOffered("kill");  // offered, refused
        Suggester sg(rg, db);
        BeliefTracker tr(rg, &db);
        tr.beginRun(0);
        const Suggestion s = sg.suggest(tr);
        CHECK(s.robustness == 1.0);
        tr.endRun(RunOutcome::Completed);
    }
    {   // The SUT keeps taking "kill" and refuses "keep": the only move that
        // counts disables "a" -- no robustness at all.
        CoverageDb db(rg);
        db.recordOffered("keep");  // offered, refused
        db.recordOffered("kill");
        db.recordTaken("kill");
        Suggester sg(rg, db);
        BeliefTracker tr(rg, &db);
        tr.beginRun(0);
        const Suggestion s = sg.suggest(tr);
        CHECK(s.robustness == 0.0);
        tr.endRun(RunOutcome::Completed);
    }
    {   // Both refused: the SUT makes no moves at all, so nothing stands
        // between the input and its goal -- fully robust again.
        CoverageDb db(rg);
        db.recordOffered("keep");  // offered, refused
        db.recordOffered("kill");  // offered, refused
        Suggester sg(rg, db);
        BeliefTracker tr(rg, &db);
        tr.beginRun(0);
        const Suggestion s = sg.suggest(tr);
        CHECK(s.robustness == 1.0);
        tr.endRun(RunOutcome::Completed);
    }
}

// DeepSeek V4.1 Flash: the goal priority really changes a choice (doku
// section 27). The two-step path "a1", "a2" ends in a still-sought error
// marking; "b" is a fresh self-loop one step away. Balanced prefers the
// one-step discovery ("b"); the Targets profile prices fresh transitions so
// that the error two steps away wins.
static void testGoalPriorityFlipsTheChoice()
{
    NetBuilder b;
    const auto p = b.place("p");
    const auto m = b.place("m");
    const auto q = b.place("q");
    const auto a1 = b.transition("a1", TransitionKind::Controllable, "a1");
    const auto a2 = b.transition("a2", TransitionKind::Controllable, "a2");
    const auto bb = b.transition("b", TransitionKind::Controllable, "b");
    b.input(p, a1).output(a1, m);
    b.input(m, a2).output(a2, q);
    b.input(p, bb).output(bb, p);
    b.initial("start", {{p, 1}});
    ErrorOptions fault;
    fault.targetHits = 2;  // one visit leaves it still sought
    b.errorMarking("fault", {{q, 1}}, MatchMode::Exact, fault);
    const Net net = b.build();

    ReachabilityGraph rg(net);
    rg.addInitial(0);
    rg.exploreAll();
    CoverageDb db(rg);
    pathRun(db, rg, 0, {"a1", "a2"});  // the target path is covered; the error still seeks

    const auto firstPick = [&](const SuggesterConfig &cfg) {
        Suggester sg(rg, db, cfg);
        BeliefTracker tr(rg, &db);
        tr.beginRun(0);
        const Suggestion s = sg.suggest(tr);
        tr.endRun(RunOutcome::Completed);
        CHECK(s.status == SuggestionStatus::Suggest);
        return s.label;
    };

    // Balanced: the fresh self-loop is one step away, the error two.
    CHECK(firstPick(SuggesterConfig{}) == "b");

    // Targets: what Session::config() becomes under GoalPriority::Targets.
    SuggesterConfig targets;
    targets.penaltyNewTransition = 2;
    targets.penaltyEdge = 4;
    targets.penaltyFrontier = 8;
    CHECK(firstPick(targets) == "a1");

    // Coverage: what Session::config() becomes under GoalPriority::Coverage.
    SuggesterConfig coverage;
    coverage.penaltyTarget = 4;
    coverage.penaltyEdge = 0;
    coverage.penaltyFrontier = 1;
    CHECK(firstPick(coverage) == "b");
}

// DeepSeek V4.1 Flash: the rare hunt (doku section 30). A rare transition is a
// goal of its own: it outranks fresh coverage, an enabled SUT move is dwelt
// on for its wait-for, the attempt budget ends the hunt, and a controllable
// rare input carries the dwell as a delay before it is sent.
static void testRareHuntGoalsAndBudgets()
{
    {   // A fresh one-step discovery competes with a rare transition two steps
        // away. Without the hunt the fresh edge wins; with it the rare source
        // does -- even though a fresh transition is nearer.
        NetBuilder b;
        const auto p = b.place("p");
        const auto m = b.place("m");
        const auto other = b.transition("other", TransitionKind::Controllable, "other");
        const auto go = b.transition("go", TransitionKind::Controllable, "go");
        const auto pop = b.transition("pop", TransitionKind::Observable, "pop");
        b.input(p, other).output(other, p);
        b.input(p, go).output(go, m);
        b.input(m, pop).output(pop, p);
        b.rare(pop, RareSpec{0.01, 5, 1000});
        b.initial("start", {{p, 1}});
        const Net net = b.build();

        ReachabilityGraph rg(net);
        rg.addInitial(0);
        rg.exploreAll();
        CoverageDb db(rg);

        Suggester plain(rg, db);
        BeliefTracker tr(rg, &db);
        tr.beginRun(0);
        const Suggestion normal = plain.suggest(tr);
        CHECK(normal.status == SuggestionStatus::Suggest);
        CHECK(normal.label == "other");  // the marker is inert without the hunt
        tr.endRun(RunOutcome::Completed);

        SuggesterConfig hunt;
        hunt.huntRare = true;
        Suggester hunter(rg, db, hunt);
        BeliefTracker tr2(rg, &db);
        tr2.beginRun(0);
        const Suggestion rare = hunter.suggest(tr2);
        CHECK(rare.status == SuggestionStatus::Suggest);
        CHECK(rare.label == "go");
        CHECK(rare.goal == GoalTier::Rare);
        tr2.endRun(RunOutcome::Completed);
    }
    {   // An enabled SUT move marked rare: dwell at the enabling state for its
        // wait-for. The attempt budget bounds the hunt, and wait-for 0 makes it
        // stay put and listen instead of dwelling.
        const auto makeNet = [](std::uint32_t waitForMs) {
            NetBuilder b;
            const auto s0 = b.place("s0");
            const auto s1 = b.place("s1");
            const auto spin = b.transition("spin", TransitionKind::Controllable, "spin");
            const auto blip = b.transition("blip", TransitionKind::Observable, "blip");
            const auto back = b.transition("back", TransitionKind::Observable, "back");
            b.input(s0, spin).output(spin, s0);
            b.input(s0, blip).output(blip, s1);
            b.input(s1, back).output(back, s0);
            b.rare(blip, RareSpec{0.01, 3, waitForMs});
            b.initial("start", {{s0, 1}});
            return b.build();
        };
        {
            const Net net = makeNet(60000);
            ReachabilityGraph rg(net);
            rg.addInitial(0);
            rg.exploreAll();
            CoverageDb db(rg);
            const TransitionIndex blip = *net.findTransition("blip");

            SuggesterConfig cfg;
            cfg.huntRare = true;
            Suggester hunter(rg, db, cfg);
            BeliefTracker tr(rg, &db);
            tr.beginRun(0);
            const Suggestion d = hunter.suggest(tr);
            CHECK(d.status == SuggestionStatus::Dwell);
            CHECK(d.transition == blip);
            CHECK(d.goal == GoalTier::Rare);
            CHECK(d.dwell == std::chrono::milliseconds(60000));
            tr.endRun(RunOutcome::Completed);

            // Three dwells are the model's budget; then the ordinary guidance
            // takes over (the != blip catches a dwell-looking fallback too).
            for (int i = 0; i < 3; ++i)
                db.noteRareAttempt(blip);
            CHECK(db.rareAttempts(blip) == 3u);
            Suggester spent(rg, db, cfg);
            BeliefTracker tr2(rg, &db);
            tr2.beginRun(0);
            const Suggestion s = spent.suggest(tr2);
            CHECK(s.status != SuggestionStatus::Dwell);
            CHECK(s.goal != GoalTier::Rare);
            CHECK(s.label == "spin");  // the ordinary uncovered edge
            tr2.endRun(RunOutcome::Completed);
        }
        {   // wait-for 0: no dwell, but the hunt still stays put and listens.
            const Net net = makeNet(0);
            ReachabilityGraph rg(net);
            rg.addInitial(0);
            rg.exploreAll();
            CoverageDb db(rg);
            const TransitionIndex blip = *net.findTransition("blip");

            SuggesterConfig cfg;
            cfg.huntRare = true;
            Suggester hunter(rg, db, cfg);
            BeliefTracker tr(rg, &db);
            tr.beginRun(0);
            const Suggestion w = hunter.suggest(tr);
            CHECK(w.status == SuggestionStatus::Wait);
            CHECK(w.transition == blip);
            CHECK(w.goal == GoalTier::Rare);
            tr.endRun(RunOutcome::Completed);
        }
    }
    {   // A controllable rare transition is a suggestion with a delay: the
        // input goes out only after the wait-for at the enabling state.
        NetBuilder b;
        const auto s0 = b.place("s0");
        const auto s1 = b.place("s1");
        const auto blip = b.transition("blip", TransitionKind::Controllable, "blip");
        const auto spin = b.transition("spin", TransitionKind::Controllable, "spin");
        b.input(s0, blip).output(blip, s1);
        b.input(s0, spin).output(spin, s0);
        b.rare(blip, RareSpec{0.01, 2, 250});
        b.initial("start", {{s0, 1}});
        const Net net = b.build();

        ReachabilityGraph rg(net);
        rg.addInitial(0);
        rg.exploreAll();
        CoverageDb db(rg);

        SuggesterConfig cfg;
        cfg.huntRare = true;
        Suggester hunter(rg, db, cfg);
        BeliefTracker tr(rg, &db);
        tr.beginRun(0);
        const Suggestion s = hunter.suggest(tr);
        CHECK(s.status == SuggestionStatus::Suggest);
        CHECK(s.label == "blip");
        CHECK(s.goal == GoalTier::Rare);
        CHECK(s.dwell == std::chrono::milliseconds(250));
        tr.endRun(RunOutcome::Completed);
    }
}

// ---------------------------------------------------------------------------

int main()
{
    const std::vector<std::pair<const char *, std::function<void()>>> tests = {
        {"errorMarkingsSeekThenForbid", testErrorMarkingsSeekThenForbid},
        {"errorPoliciesAvoidAndOnce", testErrorPoliciesAvoidAndOnce},
        {"deadlockPolicies", testDeadlockPolicies},
        {"livelockPhases", testLivelockPhases},
        {"firstSuggestionAndWaiting", testFirstSuggestionAndWaiting},
        {"prefersUncoveredAndFinishesWithDone", testPrefersUncoveredAndFinishesWithDone},
        {"vetoOfAnAvoidedError", testVetoOfAnAvoidedError},
        {"stuckWhenGoalsAreUnreachable", testStuckWhenGoalsAreUnreachable},
        {"avoidsASatisfiedLivelock", testAvoidsASatisfiedLivelock},
        {"prefersTheMoreRobustInput", testPrefersTheMoreRobustInput},
        {"plansNeverPassThroughForbiddenStates", testPlansNeverPassThroughForbiddenStates},
        {"satisfiedTargetPolicyPlansThroughTheTarget", testSatisfiedTargetPolicyPlansThroughTheTarget},  // DeepSeek V4.1 Flash
        {"satisfiedTargetPolicyHazard", testSatisfiedTargetPolicyHazard},  // DeepSeek V4.1 Flash
        {"frontierGoalsLeadIntoUnknownTerritory", testFrontierGoalsLeadIntoUnknownTerritory},
        {"expansionBudgetBoundsLookahead", testExpansionBudgetBoundsLookahead},
        {"coverageDirectedRunReachesFullCoverage", testCoverageDirectedRunReachesFullCoverage},
        {"suggestWithoutARun", testSuggestWithoutARun},
        {"seededTieBreaksVaryAndReproduce", testSeededTieBreaksVaryAndReproduce},  // GLM-5.3-Flash
        {"robustnessWeightsTheEnvironmentByTakeRate", testTheRobustnessWeightsTheEnvironmentByTakeRate},  // GLM-5.3-Flash
        {"goalPriorityFlipsTheChoice", testGoalPriorityFlipsTheChoice},  // DeepSeek V4.1 Flash
        {"rareHuntGoalsAndBudgets", testRareHuntGoalsAndBudgets},  // DeepSeek V4.1 Flash
    };
    for (const auto &t : tests) {
        const int before = g_failures;
        t.second();
        std::printf("%s %s\n", g_failures == before ? "PASS" : "FAIL", t.first);
    }
    std::printf("%d failure(s)\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
