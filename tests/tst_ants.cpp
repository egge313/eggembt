// Tests for the ant colony: back-propagation depth, the three kinds of
// events (catastrophes, targets, unexplored territory), saturation,
// evaporation, determinism, and the colony's influence on the suggester.
#include "ElevatorModel.h"
#include "engine/AntColony.h"
#include "engine/BeliefTracker.h"
#include "engine/CoverageDb.h"
#include "engine/ReachabilityGraph.h"
#include "engine/Suggester.h"
#include "engine/TSuffix.h"
#include "model/NetBuilder.h"

#include <cmath>
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

static bool near(double a, double b, double eps = 1e-6) { return std::fabs(a - b) <= eps; }

static StateId findState(const ReachabilityGraph &g, const Marking &m)
{
    for (StateId s = 0; s < g.numStates(); ++s)
        if (g.marking(s) == m)
            return s;
    return kNoState;
}

static bool firePath(CoverageDb &db, ReachabilityGraph &rg, const std::vector<std::string> &ids)
{
    for (const std::string &id : ids) {
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

static void pathRun(CoverageDb &db, ReachabilityGraph &rg, std::size_t initial,
                    const std::vector<std::string> &ids)
{
    db.beginRun(rg.addInitial(initial));
    CHECK(firePath(db, rg, ids));
    db.endRun(RunOutcome::Completed);
}

// A chain p0 -t0-> p1 -t1-> ... -t6-> p7: eight states S0..S7 and edges 0..6
// (edge i is S_i -> S_{i+1}) once fully explored. All inputs.
static Net makeChain(bool errorAtEnd)
{
    NetBuilder b;
    std::vector<PlaceIndex> p;
    for (int i = 0; i < 8; ++i)
        p.push_back(b.place("p" + std::to_string(i)));
    for (int i = 0; i < 7; ++i) {
        const auto t = b.transition("t" + std::to_string(i), TransitionKind::Controllable,
                                    "t" + std::to_string(i));
        b.input(p[static_cast<std::size_t>(i)], t).output(t, p[static_cast<std::size_t>(i) + 1]);
    }
    b.initial("start", {{p[0], 1}});
    if (errorAtEnd) {
        ErrorOptions o;
        o.targetHits = 2;
        b.errorMarking("end", {{p[7], 1}}, MatchMode::Exact, o);
    }
    return b.build();
}

static std::vector<std::string> chainPath()
{
    return {"t0", "t1", "t2", "t3", "t4", "t5", "t6"};
}

// ---------------------------------------------------------------------------
// Back-propagation
// ---------------------------------------------------------------------------

static void testBackpropDepthIsAtLeastFive()
{
    const Net net = makeChain(false);
    ReachabilityGraph rg(net);
    rg.addInitial(0);
    rg.exploreAll();
    CoverageDb db(rg);

    for (unsigned asked : {0u, 1u, 4u, 5u}) {
        AntConfig cfg;
        cfg.backpropDepth = asked;
        CHECK(AntColony(rg, db, cfg).backpropDepth() == 5);
    }
    AntConfig deeper;
    deeper.backpropDepth = 8;
    CHECK(AntColony(rg, db, deeper).backpropDepth() == 8);
}

static void testDepositReachesExactlyFivePreviousNodes()
{
    const Net net = makeChain(false);
    ReachabilityGraph rg(net);
    rg.addInitial(0);
    rg.exploreAll();
    CoverageDb db(rg);

    AntConfig cfg;
    cfg.depthDecay = 0.5;
    AntColony colony(rg, db, cfg);
    colony.depositAttract(7, 1.0);  // an event at S7

    // Five nodes behind S7 are S6..S2; the edges entering S7..S3 get 1, 1/2, 1/4, 1/8, 1/16.
    CHECK(near(colony.attract(6), 1.0));
    CHECK(near(colony.attract(5), 0.5));
    CHECK(near(colony.attract(4), 0.25));
    CHECK(near(colony.attract(3), 0.125));
    CHECK(near(colony.attract(2), 0.0625));
    CHECK(colony.attract(1) == 0.0);  // the sixth node back is out of reach
    CHECK(colony.attract(0) == 0.0);
    CHECK(colony.repel(6) == 0.0);    // the other channel is untouched

    // A deeper setting reaches further.
    AntConfig wide = cfg;
    wide.backpropDepth = 8;
    AntColony deepColony(rg, db, wide);
    deepColony.depositRepel(7, 1.0);
    CHECK(near(deepColony.repel(0), 1.0 / 64.0));  // depth 6
}

static void testDepositsAreCappedAtTauMax()
{
    const Net net = makeChain(false);
    ReachabilityGraph rg(net);
    rg.addInitial(0);
    rg.exploreAll();
    CoverageDb db(rg);
    AntConfig cfg;
    cfg.depthDecay = 0.5;
    cfg.tauMax = 10.0;
    AntColony colony(rg, db, cfg);
    colony.depositAttract(7, 100.0);
    CHECK(near(colony.attract(6), 10.0));  // 100 -> capped
    CHECK(near(colony.attract(5), 10.0));  // 50 -> capped
    CHECK(near(colony.attract(4), 10.0));  // 25 -> capped
    CHECK(near(colony.attract(3), 10.0));  // 12.5 -> capped
    CHECK(near(colony.attract(2), 6.25));  // below the cap
    colony.depositAttract(7, 100.0);
    CHECK(near(colony.attract(2), 10.0));  // repeated deposits add up to the cap, not beyond
}

// GLM-5.3-Flash: a catastrophe's deposit is priced by the way in. The longer
// the path to the error state, the more campaign time a repeat visit wastes,
// so the base deposit grows with the inputs the run spent getting there --
// up to the cap, beyond which however long the run was makes no difference.
static void testCatastropheDepositGrowsWithTheWayIn()
{
    const AntConfig cfg;  // base 3.0, a quarter extra per input, 8 inputs counted at most
    CHECK(near(cfg.catastropheDeposit(0), 3.0));    // no way in: the flat deposit
    CHECK(near(cfg.catastropheDeposit(1), 3.75));   // one input in: base plus a quarter
    CHECK(near(cfg.catastropheDeposit(4), 6.0));    // four inputs in: twice the base
    CHECK(near(cfg.catastropheDeposit(8), 9.0));    // the cap: three times the base
    CHECK(near(cfg.catastropheDeposit(100), 9.0));  // a very long run changes nothing more

    AntConfig flat;                 // the old behaviour is one switch away
    flat.catastropheTimeWeight = 0.0;
    CHECK(near(flat.catastropheDeposit(5), 3.0));

    AntConfig shortReach;           // weight and cap are independent knobs
    shortReach.catastropheTimeWeight = 1.0;
    shortReach.catastropheTimeCap = 2;
    CHECK(near(shortReach.catastropheDeposit(9), 9.0));
    CHECK(near(shortReach.catastropheDeposit(1), 6.0));
}

// ---------------------------------------------------------------------------
// What ants meet
// ---------------------------------------------------------------------------

static void testAnAntThatFindsATargetMarksTheWayIn()
{
    const Net net = makeChain(true);  // error marking at S7, target 2 hits
    ReachabilityGraph rg(net);
    rg.addInitial(0);
    rg.exploreAll();
    CoverageConfig ccfg;
    ccfg.threshold = 5;  // edges visited once are not saturated
    CoverageDb db(rg, ccfg);
    pathRun(db, rg, 0, chainPath());  // covers every edge and reaches S7 once: 1 of 2 hits

    AntConfig cfg;
    cfg.antsPerIteration = 1;
    cfg.depthDecay = 0.5;
    cfg.evaporationAttract = 0.0;
    cfg.maxSteps = 20;
    AntColony colony(rg, db, cfg);
    colony.iterate({0});

    CHECK(colony.stats().foundTarget == 1 && colony.stats().died == 0);
    // Target deposit 2.0 at S7, five nodes back.
    CHECK(near(colony.attract(6), 2.0));
    CHECK(near(colony.attract(5), 1.0));
    CHECK(near(colony.attract(4), 0.5));
    CHECK(near(colony.attract(3), 0.25));
    CHECK(near(colony.attract(2), 0.125));
    CHECK(colony.attract(1) == 0.0 && colony.attract(0) == 0.0);
}

// s0 -go-> e (an avoided error: a catastrophe), s0 -safe-> t -back-> s0
static Net makeCatastropheNet()
{
    NetBuilder b;
    const auto s0 = b.place("s0");
    const auto e = b.place("e");
    const auto t = b.place("t");
    const auto go = b.transition("go", TransitionKind::Controllable, "go");
    const auto safe = b.transition("safe", TransitionKind::Controllable, "safe");
    const auto back = b.transition("back", TransitionKind::Observable, "back");
    b.input(s0, go).output(go, e);
    b.input(s0, safe).output(safe, t);
    b.input(t, back).output(back, s0);
    b.initial("start", {{s0, 1}});
    ErrorOptions o;
    o.policy = TargetPolicy::Avoid;
    b.errorMarking("bad", {{e, 1}}, MatchMode::Exact, o);
    return b.build();
}

static void testCatastrophesRepelAndAreAvoidedAfterwards()
{
    const Net net = makeCatastropheNet();
    ReachabilityGraph rg(net);
    const StateId s0 = rg.addInitial(0);
    rg.exploreAll();
    CoverageDb db(rg);

    AntConfig cfg;
    cfg.seed = 7;
    cfg.maxSteps = 10;
    AntColony colony(rg, db, cfg);

    const EdgeId go = *rg.findEdge(s0, *net.findTransition("go"));
    const EdgeId safe = *rg.findEdge(s0, *net.findTransition("safe"));
    CHECK(near(colony.inputWeight(go), colony.inputWeight(safe)));  // no preference to start with

    for (int i = 0; i < 10; ++i)
        colony.iterate({s0});

    CHECK(colony.stats().died >= 1);  // ants learn about the catastrophe by dying in it
    CHECK(colony.stats().died < colony.stats().ants / 4);
    CHECK(colony.repel(go) > 0.0);
    // The signal also spills, weaker, onto the edges that lead back to the place
    // where the catastrophe can happen (safe -> back -> s0 -> go): that is
    // back-propagation through a cycle. The direct way in is marked far more.
    CHECK(colony.repel(safe) < colony.repel(go));
    CHECK(colony.attract(safe) > 0.0);  // the other way leads to unexplored edges
    // The colony now strongly prefers the safe input.
    CHECK(colony.inputWeight(go) < 0.1 * colony.inputWeight(safe));

    // Repellent pheromone evaporates slowly, and keeps ageing without ants.
    const double before = colony.repel(go);
    for (int i = 0; i < 50; ++i)
        colony.iterate({});
    CHECK(colony.repel(go) < before);
    CHECK(near(colony.repel(go), before * std::pow(0.98, 50), before * 0.01));
}

// DeepSeek V4.1 Flash: evaporation can wait for the run boundary (doku
// section 27). Without it, a lesson halves every ~34 suggestions: a run with
// hundreds of suggestions keeps only a few percent of what it learned early.
// With the boundary, the ants still walk, but one run costs one aging step.
static void testAgingCanWaitForTheRunBoundary()
{
    const Net net = makeChain(false);
    ReachabilityGraph rg(net);
    rg.addInitial(0);
    rg.exploreAll();
    CoverageDb db(rg);

    const auto t2 = *net.findTransition("t2");  // S2 -> S3
    const StateId s3 = findState(rg, Marking{0, 0, 0, 1, 0, 0, 0, 0});
    const EdgeId intoS3 = *rg.findEdge(s3 - 1, t2);

    AntConfig cfg;
    cfg.tsuffixLength = 4;
    cfg.antsPerIteration = 0;  // no walks: only the aging is under test
    AntColony colony(rg, db, cfg);
    TraceCascade cascade;
    cascade.setLength(4);
    cascade.push(stepHashOf(net.transitions[t2]));

    colony.depositRepel(s3, 9.0);
    colony.depositPairRepel(s3, cascade, 9.0);
    const double edgeBefore = colony.repel(intoS3);
    const double pairBefore = colony.pairRepel(s3, cascade);
    CHECK(near(edgeBefore, 9.0));
    CHECK(near(pairBefore, 9.0));

    // 200 suggestions of one long run: nothing ages.
    for (int i = 0; i < 200; ++i)
        colony.iterate({}, /*evaporate=*/false);
    CHECK(near(colony.repel(intoS3), edgeBefore));
    CHECK(near(colony.pairRepel(s3, cascade), pairBefore));

    // One run boundary: exactly one evaporation step.
    colony.age();
    CHECK(near(colony.repel(intoS3), edgeBefore * 0.98, 1e-6));
    CHECK(near(colony.pairRepel(s3, cascade), pairBefore * 0.98, 1e-6));

    // The old behaviour: 200 per-suggestion agings leave 1.8% of the lesson.
    AntColony perSuggestion(rg, db, cfg);
    perSuggestion.depositRepel(s3, 9.0);
    for (int i = 0; i < 200; ++i)
        perSuggestion.iterate({});
    CHECK(near(perSuggestion.repel(intoS3), 9.0 * std::pow(0.98, 200), 1e-3));
    CHECK(perSuggestion.repel(intoS3) < 0.02 * 9.0);
}

static void testMaturedTargetsAreSparedByMergeEvaporation()
{
    // DeepSeek V4.1 Flash: the merge-time exception (doku section 34). The
    // chain ends in an error marking with targetHits 2; two full paths mature
    // it. Its way in then keeps the catastrophe pheromone through age(true),
    // while an ordinary state's deposits and pairs age at the usual rates.
    const Net net = makeChain(true);
    ReachabilityGraph rg(net);
    rg.addInitial(0);
    rg.exploreAll();
    CoverageDb db(rg);
    pathRun(db, rg, 0, chainPath());
    pathRun(db, rg, 0, chainPath());
    CHECK(db.isErrorSatisfied(0));

    const auto t5 = *net.findTransition("t5");
    const auto t6 = *net.findTransition("t6");
    const StateId s6 = findState(rg, Marking{0, 0, 0, 0, 0, 0, 1, 0});
    const StateId s7 = findState(rg, Marking{0, 0, 0, 0, 0, 0, 0, 1});
    const EdgeId intoS6 = *rg.findEdge(s6 - 1, t5);
    const EdgeId intoS7 = *rg.findEdge(s6, t6);

    TraceCascade atS6;
    atS6.setLength(4);
    atS6.push(stepHashOf(net.transitions[t5]));
    TraceCascade atS7;
    atS7.setLength(4);
    atS7.push(stepHashOf(net.transitions[t6]));

    AntConfig cfg;
    cfg.tsuffixLength = 4;
    cfg.antsPerIteration = 0;
    AntColony colony(rg, db, cfg);
    colony.depositRepel(s7, 9.0);  // the matured error's way in
    colony.depositPairRepel(s7, atS7, 9.0);
    colony.depositPairRepel(s6, atS6, 9.0);  // an ordinary arrival
    CHECK(near(colony.repel(intoS7), 9.0));
    CHECK(near(colony.pairRepel(s7, atS7), 9.0));
    const double ordinaryEdge = colony.repel(intoS6);
    CHECK(near(ordinaryEdge, 6.3));  // one back-propagation step at depthDecay 0.7

    colony.age(/*spareMatureTargets=*/true);
    CHECK(near(colony.repel(intoS7), 9.0));       // matured: spared
    CHECK(near(colony.pairRepel(s7, atS7), 9.0));  // its pair: spared too
    CHECK(near(colony.repel(intoS6), ordinaryEdge * 0.98));
    CHECK(near(colony.pairRepel(s6, atS6), 9.0 * 0.98));

    // Without the exception the same boundary ages the matured way in too.
    colony.age();
    CHECK(near(colony.repel(intoS7), 9.0 * 0.98));
    CHECK(near(colony.pairRepel(s7, atS7), 9.0 * 0.98));
}

static void testUnexploredTerritoryAttractsAndLazyExpansionIsBudgeted()
{
    // s0 -a-> s1 -b-> s2, explored lazily; nothing covered yet.
    NetBuilder b;
    const auto s0 = b.place("s0");
    const auto s1 = b.place("s1");
    const auto s2 = b.place("s2");
    const auto a = b.transition("a", TransitionKind::Controllable, "a");
    const auto bt = b.transition("b", TransitionKind::Controllable, "b");
    b.input(s0, a).output(a, s1);
    b.input(s1, bt).output(bt, s2);
    b.initial("start", {{s0, 1}});
    const Net net = b.build();

    {   // the ants may expand: they reach s1, see the uncovered edge b, and mark the way in
        ReachabilityGraph rg(net);
        const StateId start = rg.addInitial(0);
        CoverageDb db(rg);
        AntConfig cfg;
        cfg.antsPerIteration = 1;
        cfg.evaporationAttract = 0.0;
        AntColony colony(rg, db, cfg);
        colony.iterate({start});
        CHECK(colony.stats().foundUnexplored == 1);
        CHECK(near(colony.attract(0), 1.0));  // the edge s0 -> s1
    }
    {   // no budget to expand: the ant cannot even leave the unexpanded start
        ReachabilityGraph rg(net);
        const StateId start = rg.addInitial(0);
        CoverageDb db(rg);
        AntConfig cfg;
        cfg.antsPerIteration = 1;
        cfg.expansionBudget = 0;
        AntColony colony(rg, db, cfg);
        colony.iterate({start});
        CHECK(colony.stats().exhausted == 1 && colony.stats().foundUnexplored == 0);
        CHECK(rg.numExpanded() == 0);
    }
}

static void testSaturatedEdgesLoseTheirAttraction()
{
    const Net net = makeChain(false);
    ReachabilityGraph rg(net);
    rg.addInitial(0);
    rg.exploreAll();
    CoverageDb db(rg);  // threshold 1
    AntConfig cfg;
    cfg.depthDecay = 0.5;
    cfg.evaporationAttract = 0.0;
    AntColony colony(rg, db, cfg);

    colony.depositAttract(7, 1.0);
    CHECK(colony.attract(6) > 0.0 && colony.attract(5) > 0.0);

    pathRun(db, rg, 0, {"t0", "t1", "t2", "t3", "t4", "t5"});  // saturates edges 0..5, not edge 6
    colony.iterate({});                                         // only ages / sweeps
    CHECK(colony.attract(5) == 0.0);   // saturated: wiped
    CHECK(colony.attract(2) == 0.0);
    CHECK(colony.attract(6) > 0.0);    // still worth reaching
}

static void testAttractiveAndRepellentPheromoneEvaporateAtDifferentRates()
{
    const Net net = makeChain(false);
    ReachabilityGraph rg(net);
    rg.addInitial(0);
    rg.exploreAll();
    CoverageDb db(rg);
    AntConfig cfg;
    AntColony colony(rg, db, cfg);
    colony.depositAttract(7, 1.0);
    colony.depositRepel(7, 1.0);
    for (int i = 0; i < 10; ++i)
        colony.iterate({});
    CHECK(near(colony.attract(6), std::pow(0.90, 10), 1e-4));
    CHECK(near(colony.repel(6), std::pow(0.98, 10), 1e-4));
    CHECK(colony.repel(6) > colony.attract(6));  // the memory of a catastrophe lasts longer
}

static void testInputWeightCombinesPheromoneAndNovelty()
{
    // s0 -x-> s1 -ox-> s0 and s0 -y-> s2 -oy-> s0
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
    const Net net = b.build();

    ReachabilityGraph rg(net);
    const StateId start = rg.addInitial(0);
    rg.exploreAll();
    const EdgeId ex = *rg.findEdge(start, x);
    const EdgeId ey = *rg.findEdge(start, y);
    const StateId sx = rg.edge(ex).target;
    const StateId sy = rg.edge(ey).target;

    {   // untouched: equal, and 0.1 each (no pheromone, no hits)
        CoverageDb db(rg);
        AntColony colony(rg, db);
        CHECK(near(colony.inputWeight(ex), 0.1) && near(colony.inputWeight(ey), 0.1));
    }
    // The model is a cycle, so a deposit also spills onto the other branch: from the
    // event state the walk goes back through s0 and reaches the other input two steps
    // behind, with the default decay of 0.7 per step (0.7^2 = 0.49).
    {   // repellent pheromone lowers the weight: exp(-3) at the event, exp(-1.47) at the spill
        CoverageDb db(rg);
        AntColony colony(rg, db);
        colony.depositRepel(sx, 3.0);
        CHECK(near(colony.repel(ex), 3.0) && near(colony.repel(ey), 3.0 * 0.49, 1e-5));
        CHECK(near(colony.inputWeight(ex), 0.1 * std::exp(-3.0), 1e-6));
        CHECK(near(colony.inputWeight(ey), 0.1 * std::exp(-3.0 * 0.49), 1e-5));
        CHECK(colony.inputWeight(ex) < colony.inputWeight(ey));
    }
    {   // attractive pheromone raises it
        CoverageDb db(rg);
        AntColony colony(rg, db);
        colony.depositAttract(sy, 5.0);
        CHECK(near(colony.inputWeight(ey), 5.1, 1e-5));
        CHECK(near(colony.inputWeight(ex), 0.1 + 5.0 * 0.49, 1e-5));
        CHECK(colony.inputWeight(ey) > colony.inputWeight(ex));
    }
    {   // novelty: a visited edge is less interesting, a saturated one hardly at all
        CoverageConfig ccfg;
        ccfg.threshold = 3;
        CoverageDb db(rg, ccfg);
        AntColony colony(rg, db);
        pathRun(db, rg, 0, {"x"});
        CHECK(near(colony.inputWeight(ex), 0.1 * 0.5));  // 1/(1+1)
        pathRun(db, rg, 0, {"x"});
        pathRun(db, rg, 0, {"x"});
        CHECK(near(colony.inputWeight(ex), 0.1 * 0.01));  // saturated
        CHECK(near(colony.inputWeight(ey), 0.1));
    }
}

// ---------------------------------------------------------------------------
// Determinism and sanity
// ---------------------------------------------------------------------------

static void testSameSeedSameColony()
{
    const Net net = testmodels::makeElevatorNet();
    std::vector<double> attract[2], repel[2];
    AntStats stats[2];
    for (int run = 0; run < 2; ++run) {
        ReachabilityGraph rg(net);
        const StateId start = rg.addInitial(0);
        CoverageDb db(rg);
        AntConfig cfg;
        cfg.seed = 42;
        AntColony colony(rg, db, cfg);
        for (int i = 0; i < 30; ++i)
            colony.iterate({start});
        for (EdgeId e = 0; e < rg.numEdges(); ++e) {
            attract[run].push_back(colony.attract(e));
            repel[run].push_back(colony.repel(e));
        }
        stats[run] = colony.stats();
    }
    CHECK(attract[0] == attract[1] && repel[0] == repel[1]);
    CHECK(stats[0].ants == stats[1].ants && stats[0].foundUnexplored == stats[1].foundUnexplored);
    CHECK(!attract[0].empty());
}

static void testPheromoneStaysWithinBounds()
{
    const Net net = testmodels::makeElevatorNet();
    ReachabilityGraph rg(net);
    const StateId start = rg.addInitial(0);
    CoverageDb db(rg);
    AntConfig cfg;
    cfg.tauMax = 3.0;
    AntColony colony(rg, db, cfg);
    for (int i = 0; i < 100; ++i)
        colony.iterate({start});
    CHECK(colony.stats().ants == 100 * cfg.antsPerIteration);
    bool inBounds = true;
    for (EdgeId e = 0; e < rg.numEdges(); ++e) {
        inBounds = inBounds && colony.attract(e) >= 0.0 && colony.attract(e) <= 3.0
                   && colony.repel(e) >= 0.0 && colony.repel(e) <= 3.0;
    }
    CHECK(inBounds);
}

// ---------------------------------------------------------------------------
// The colony's influence on the suggester
// ---------------------------------------------------------------------------

static void testPheromoneSteersTheSuggester()
{
    // s0 -x-> s1 -ox-> s0 and s0 -y-> s2 -oy-> s0, both inputs equally good on paper.
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
    const Net net = b.build();

    ReachabilityGraph rg(net);
    rg.addInitial(0);
    rg.exploreAll();
    CoverageDb db(rg);
    const StateId sx = findState(rg, Marking{0, 1, 0});
    const StateId sy = findState(rg, Marking{0, 0, 1});

    BeliefTracker tr(rg, &db);
    tr.beginRun(0);

    Suggester plain(rg, db);
    CHECK(plain.suggest(tr).label == "x");  // a tie, the lower index wins

    {   // repellent pheromone on the way to x
        AntColony colony(rg, db);
        colony.depositRepel(sx, 3.0);
        Suggester sg(rg, db);
        sg.setAntColony(&colony);
        const Suggestion s = sg.suggest(tr);
        CHECK(s.status == SuggestionStatus::Suggest && s.label == "y");
    }
    {   // attractive pheromone on the way to y
        AntColony colony(rg, db);
        colony.depositAttract(sy, 5.0);
        Suggester sg(rg, db);
        sg.setAntColony(&colony);
        CHECK(sg.suggest(tr).label == "y");
    }
    {   // no pheromone: nothing changes
        AntColony colony(rg, db);
        Suggester sg(rg, db);
        sg.setAntColony(&colony);
        CHECK(sg.suggest(tr).label == "x");
    }
    tr.endRun(RunOutcome::Completed);
}

// ---------------------------------------------------------------------------
// GLM-5.3-Flash: the off-target region (doku section 12 item 5)
// ---------------------------------------------------------------------------

// GLM-5.3-Flash: states from which no target marking is reachable are a
// further repellent region (doku section 12 item 5). The corridor serves
// back to the target idle state; the "b" branch ends in a place that can only
// spin in place -- no target marking is reachable from it, and it is no
// target either (it moves, so it is no deadlock; no livelock findings are
// wired in here). The way into it carries static repel; the serving corridor
// does not.
static void testStatesWithoutATargetMarkingRepel()
{
    NetBuilder b;
    const auto s0 = b.place("s0");
    const auto s1 = b.place("s1");
    const auto lost = b.place("lost");
    const auto a = b.transition("a", TransitionKind::Controllable, "a");
    const auto oa = b.transition("oa", TransitionKind::Observable, "oa");
    const auto bb = b.transition("b", TransitionKind::Controllable, "b");
    const auto spin = b.transition("spin", TransitionKind::Observable, "spin");
    b.input(s0, a).output(a, s1);
    b.input(s1, oa).output(oa, s0);
    b.input(s0, bb).output(bb, lost);
    b.input(lost, spin).output(spin, lost);
    b.initial("start", {{s0, 1}});
    b.targetMarking("idle", {{s0, 1}});
    const Net net = b.build();

    // Grown lazily: at first only the start state is expanded, so nothing is
    // PROVEN lost yet and the way into the unknown is not priced.
    ReachabilityGraph rg(net);
    const StateId start = rg.addInitial(0);
    rg.expand(start);
    CoverageDb db(rg);
    AntColony colony(rg, db);
    colony.iterate({});
    const EdgeId ea = *rg.findEdge(start, a);
    const EdgeId eb = *rg.findEdge(start, bb);
    CHECK(near(colony.region(ea), 0.0) && near(colony.region(eb), 0.0));

    // Fully explored, the lost place cannot reach the target marking (and
    // there is no unknown left to hope into, and it is no target): its way in
    // is repellent, the serving corridor is not. The region is set, not
    // accumulated: it reads the same after every further iteration.
    rg.exploreAll();
    colony.iterate({});
    colony.iterate({});
    CHECK(near(colony.region(eb), 1.0));
    CHECK(near(colony.region(ea), 0.0));
    const StateId s1s = rg.edge(ea).target;
    const EdgeId eoa = *rg.findEdge(s1s, oa);
    CHECK(near(colony.region(eoa), 0.0));
    // The spin edge leads deeper into the region: also a way in.
    const StateId lostS = rg.edge(eb).target;
    CHECK(near(colony.region(*rg.findEdge(lostS, spin)), 1.0));

    // An ant's weight for the edge into the region carries the region repel.
    CHECK(near(colony.inputWeight(eb), 0.1 * std::exp(-1.0), 1e-6));
    CHECK(near(colony.inputWeight(ea), 0.1));

    {   // Switched off, the region says nothing.
        AntConfig cfg;
        cfg.offTargetRepel = 0.0;
        AntColony quiet(rg, db, cfg);
        quiet.iterate({});
        CHECK(near(quiet.region(eb), 0.0));
        CHECK(near(quiet.inputWeight(eb), 0.1));
    }
}

// GLM-5.3-Flash: the region never convicts unknown territory. From s0 the
// branch "a" ends in a certainly lost place (only a spin move, no target
// reachable, no target), the branch "c" leads into an unexpanded state -- a
// target marking might lie beyond it, so its way in stays clean.
static void testTheRegionSparesUnknownTerritory()
{
    NetBuilder b;
    const auto s0 = b.place("s0");
    const auto lost = b.place("lost");
    const auto u = b.place("u");
    const auto a = b.transition("a", TransitionKind::Controllable, "a");
    const auto c = b.transition("c", TransitionKind::Controllable, "c");
    const auto spin = b.transition("spin", TransitionKind::Observable, "spin");
    b.input(s0, a).output(a, lost);
    b.input(s0, c).output(c, u);
    b.input(lost, spin).output(spin, lost);
    b.initial("start", {{s0, 1}});
    b.targetMarking("idle", {{s0, 1}});
    const Net net = b.build();

    ReachabilityGraph rg(net);
    const StateId start = rg.addInitial(0);
    rg.expand(start);
    rg.expand(rg.edge(*rg.findEdge(start, a)).target);  // lost, but not u
    CoverageDb db(rg);
    AntColony colony(rg, db);
    colony.iterate({});
    const EdgeId ea = *rg.findEdge(start, a);
    const EdgeId ec = *rg.findEdge(start, c);
    CHECK(near(colony.region(ea), 1.0));  // certainly lost: no target marking, no sought target, no unknown
    CHECK(near(colony.region(ec), 0.0));  // unknown: a target marking might lie beyond
}

// GLM-5.3-Flash: the region steers the suggester. Two equally good inputs;
// the x branch ends in a spin-loop dead end from which no target marking is
// reachable, the y branch serves back to the target idle state. The colony's
// region makes y win the tie; without the colony (or with the region switched
// off) the deterministic tie-break keeps the lower index, x.
static void testTheRegionSteersTheSuggester()
{
    NetBuilder b;
    const auto s0 = b.place("s0");
    const auto s1 = b.place("s1");
    const auto s2 = b.place("s2");
    const auto x = b.transition("x", TransitionKind::Controllable, "x");
    const auto spin = b.transition("spin", TransitionKind::Observable, "spin");
    const auto y = b.transition("y", TransitionKind::Controllable, "y");
    const auto oy = b.transition("oy", TransitionKind::Observable, "oy");
    b.input(s0, x).output(x, s1);
    b.input(s1, spin).output(spin, s1);
    b.input(s0, y).output(y, s2);
    b.input(s2, oy).output(oy, s0);
    b.initial("start", {{s0, 1}});
    b.targetMarking("idle", {{s0, 1}});
    const Net net = b.build();

    ReachabilityGraph rg(net);
    rg.addInitial(0);
    rg.exploreAll();
    const StateId start = rg.addInitial(0);
    const EdgeId ex = *rg.findEdge(start, x);
    const EdgeId ey = *rg.findEdge(start, y);

    CoverageDb plainDb(rg);
    BeliefTracker tr(rg, &plainDb);
    Suggester plain(rg, plainDb);
    tr.beginRun(0);
    CHECK(plain.suggest(tr).label == "x");  // the tie, lower index wins

    {   // The colony's region repels the way into the dead end.
        CoverageDb db(rg);
        AntColony colony(rg, db);
        colony.iterate({});
        CHECK(near(colony.region(ex), 1.0));
        CHECK(near(colony.region(ey), 0.0));
        Suggester sg(rg, db);
        sg.setAntColony(&colony);
        const Suggestion s = sg.suggest(tr);
        CHECK(s.status == SuggestionStatus::Suggest && s.label == "y");
    }
    {   // Region switched off: the plain tie-break again.
        CoverageDb db(rg);
        AntConfig cfg;
        cfg.offTargetRepel = 0.0;
        AntColony colony(rg, db, cfg);
        colony.iterate({});
        CHECK(near(colony.region(ex), 0.0));
        Suggester sg(rg, db);
        sg.setAntColony(&colony);
        CHECK(sg.suggest(tr).label == "x");
    }
    tr.endRun(RunOutcome::Completed);
}

// ---------------------------------------------------------------------------
// GLM-5.3-Flash: the T-suffix layer (doku section 16)
// ---------------------------------------------------------------------------

// GLM-5.3-Flash: a T-suffix is a list of truncated traces, each element a
// mini-hash created from the current trace. THE SUFFIX CASCADE, EXACTLY. A
// run's trace is its sequence of steps s_1, ..., s_m, each hashed by
// stepHashOf() of the transition that fired. With kTraceSeed as the hash of
// an empty suffix, element k after m steps is
//
//     H_k(m) = mix(H_{k-1}(m-1), s_m)     for k = 1..N,     H_k(0) = kTraceSeed
//
// so H_1(m) = mix(kTraceSeed, s_m) depends only on the last step, H_2(m) on
// the last two, ..., and H_N(m) is the mini-hash created from the current
// trace, truncated to its last N steps. The reference fold of the last
// min(k, m) steps onto kTraceSeed must reproduce every element.
static void testTraceCascadeFollowsTheRecurrence()
{
    const std::vector<std::uint32_t> steps = {stepHash("p"), stepHash("q"), stepHash("r"),
                                              stepHash("s"), stepHash("t")};
    const auto fold = [&](std::size_t from, std::size_t count) {
        std::uint32_t h = kTraceSeed;
        for (std::size_t i = 0; i < count; ++i)
            h = mix(h, steps[from + i]);
        return h;
    };

    TraceCascade tc;
    tc.setLength(4);
    CHECK(tc.length() == 4);
    CHECK(tc.elements()[0] == kTraceSeed && tc.elements()[kMaxTSuffix - 1] == kTraceSeed);

    for (std::size_t m = 1; m <= steps.size(); ++m) {
        tc.push(steps[m - 1]);
        for (std::size_t k = 1; k <= 4; ++k) {
            // element k folds the last min(k, m) steps onto kTraceSeed: with
            // fewer steps taken, the missing deeper history contributes
            // kTraceSeed, the hash of nothing.
            const std::size_t count = std::min(k, m);
            CHECK(tc.elements()[k - 1] == fold(m - count, count));
        }
        // entries at or beyond the length stay kTraceSeed, so a pair key is
        // determined by the length alone
        CHECK(tc.elements()[4] == kTraceSeed);
    }

    // A reset is a run's begin: every element back to kTraceSeed.
    tc.reset();
    CHECK(tc.elements()[0] == kTraceSeed && tc.elements()[3] == kTraceSeed);

    // The length is capped at kMaxTSuffix; 0 is off.
    TraceCascade capped;
    capped.setLength(1000);
    CHECK(capped.length() == kMaxTSuffix);
    capped.setLength(0);
    CHECK(capped.length() == 0);
    capped.setLength(kMaxTSuffix + 1);
    CHECK(capped.length() == kMaxTSuffix);

    // extended() pushes one step on a copy; the original is untouched.
    TraceCascade base;
    base.setLength(4);
    base.push(steps[0]);
    const TraceCascade extended = base.extended(steps[1]);
    TraceCascade twice;
    twice.setLength(4);
    twice.push(steps[0]);
    twice.push(steps[1]);
    CHECK(extended == twice);
    CHECK(base != twice);
    CHECK(base.elements()[0] == fold(0, 1));
    CHECK(base.elements()[1] == fold(0, 1));  // the short-trace rule: min(2, 1) steps

    // Order sensitivity: the same two steps in the other order give other
    // elements.
    TraceCascade ab, ba;
    ab.setLength(2);
    ba.setLength(2);
    ab.push(steps[0]);
    ab.push(steps[1]);
    ba.push(steps[1]);
    ba.push(steps[0]);
    CHECK(ab != ba);
    CHECK(ab.elements()[0] != ba.elements()[0]);
    CHECK(ab.elements()[1] != ba.elements()[1]);

    // Two traces ending in the same two steps share the first two elements
    // and differ from the third on: the T-suffix identifies the arrival ROUTE.
    TraceCascade a, b;
    a.setLength(4);
    b.setLength(4);
    a.push(steps[0]);                                      // p q r
    a.push(steps[1]);
    a.push(steps[2]);
    b.push(steps[3]);                                      // s t q r
    b.push(steps[4]);
    b.push(steps[1]);
    b.push(steps[2]);
    CHECK(a.elements()[0] == b.elements()[0]);  // last step r
    CHECK(a.elements()[1] == b.elements()[1]);  // last two q r
    CHECK(a.elements()[2] != b.elements()[2]);  // third: p q r vs t q r
    CHECK(a.elements()[3] != b.elements()[3]);
}

// s0 -x-> s1 and s0 -y-> s1, both inputs arriving at the SAME state; s1
// -ox-> s0 keeps the graph a cycle. Two routes, one arrival state.
static Net makeTwoRouteNet()
{
    NetBuilder b;
    const auto s0 = b.place("s0");
    const auto s1 = b.place("s1");
    const auto x = b.transition("x", TransitionKind::Controllable, "x");
    const auto y = b.transition("y", TransitionKind::Controllable, "y");
    const auto ox = b.transition("ox", TransitionKind::Observable, "ox");
    b.input(s0, x).output(x, s1);
    b.input(s0, y).output(y, s1);
    b.input(s1, ox).output(ox, s0);
    b.initial("start", {{s0, 1}});
    return b.build();
}

static void testPairsSeparateRoutesAndAreCapped()
{
    const Net net = makeTwoRouteNet();
    ReachabilityGraph rg(net);
    const StateId start = rg.addInitial(0);
    rg.exploreAll();
    const TransitionIndex tx = *net.findTransition("x");
    const TransitionIndex ty = *net.findTransition("y");
    const EdgeId ex = *rg.findEdge(start, tx);
    const StateId s1 = rg.edge(ex).target;

    AntConfig cfg;
    cfg.tsuffixLength = 4;
    CoverageDb db(rg);
    AntColony colony(rg, db, cfg);

    const std::uint32_t hx = stepHashOf(net.transitions[tx]);
    const std::uint32_t hy = stepHashOf(net.transitions[ty]);
    TraceCascade viaX, viaY;
    viaX.setLength(4);
    viaY.setLength(4);
    viaX.push(hx);
    viaY.push(hy);
    CHECK(viaX != viaY);

    colony.depositPairRepel(s1, viaX, 2.0);
    CHECK(near(colony.pairRepel(s1, viaX), 2.0));
    CHECK(near(colony.pairRepel(s1, viaY), 0.0));  // the other route is untouched
    CHECK(near(colony.pairAttract(s1, viaX), 0.0));
    CHECK(colony.pairCount() == 1);

    colony.depositPairAttract(s1, viaY, 1.5);
    CHECK(near(colony.pairAttract(s1, viaY), 1.5));
    CHECK(colony.pairCount() == 2);

    // tauMax caps every pair deposit, like every other.
    colony.depositPairAttract(s1, viaY, 100.0);
    CHECK(near(colony.pairAttract(s1, viaY), cfg.tauMax));
    colony.depositPairRepel(s1, viaX, 100.0);
    CHECK(near(colony.pairRepel(s1, viaX), cfg.tauMax));

    {   // the default is off: the deposits are no-ops, the store stays empty
        CoverageDb db2(rg);
        AntColony off(rg, db2);
        CHECK(off.tsuffixLength() == 0);
        off.depositPairAttract(s1, viaX, 5.0);
        off.depositPairRepel(s1, viaX, 5.0);
        CHECK(off.pairCount() == 0);
        CHECK(near(off.pairAttract(s1, viaX), 0.0));
        CHECK(near(off.pairRepel(s1, viaX), 0.0));
    }
}

static void testPairsEvaporateAndArePruned()
{
    const Net net = makeTwoRouteNet();
    ReachabilityGraph rg(net);
    rg.addInitial(0);
    rg.exploreAll();
    const StateId s1 = findState(rg, Marking{0, 1});

    AntConfig cfg;
    cfg.tsuffixLength = 4;
    cfg.antsPerIteration = 2;  // the walks are irrelevant here; keep them cheap
    cfg.maxSteps = 4;
    CoverageDb db(rg);
    AntColony colony(rg, db, cfg);

    const std::uint32_t hx = stepHashOf(net.transitions[*net.findTransition("x")]);
    TraceCascade viaX;
    viaX.setLength(4);
    viaX.push(hx);

    colony.depositPairAttract(s1, viaX, 1.0);
    colony.depositPairRepel(s1, viaX, 1.0);
    for (int i = 0; i < 10; ++i)
        colony.iterate({});
    // The pair store evaporates with the same per-iteration rates as the
    // edge channels.
    CHECK(near(colony.pairAttract(s1, viaX), std::pow(0.90, 10), 1e-4));
    CHECK(near(colony.pairRepel(s1, viaX), std::pow(0.98, 10), 1e-4));
    CHECK(colony.pairCount() == 1);  // both channels above the 1e-4 floor
    // When both channels fall below the floor, the entry is pruned.
    for (int i = 0; i < 500; ++i)
        colony.iterate({});
    CHECK(colony.pairCount() == 0);
    CHECK(near(colony.pairAttract(s1, viaX), 0.0));
    CHECK(near(colony.pairRepel(s1, viaX), 0.0));
}

// s0 -x-> s1 -ox-> s0 and s0 -y-> s2 -oy-> s0: two inputs with different
// arrival states.
static Net makeTwoTargetNet()
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

// DeepSeek V4.1 Flash: the readable view of the pair store (--dump-pheromone,
// doku section 21): every entry carries its state, its T-suffix and both
// channels, sorted so a dump is reproducible.
static void testPairEntriesAreReadableAndSorted()
{
    const Net net = makeTwoRouteNet();
    ReachabilityGraph rg(net);
    const StateId start = rg.addInitial(0);
    rg.exploreAll();
    const TransitionIndex tx = *net.findTransition("x");
    const TransitionIndex ty = *net.findTransition("y");
    const EdgeId ex = *rg.findEdge(start, tx);
    const StateId s1 = rg.edge(ex).target;
    const EdgeId ey = *rg.findEdge(start, ty);
    const StateId s2 = rg.edge(ey).target;

    AntConfig cfg;
    cfg.tsuffixLength = 4;
    CoverageDb db(rg);
    AntColony colony(rg, db, cfg);
    CHECK(colony.pairEntries().empty());  // nothing deposited yet

    // The two routes may converge on the same marking, so a row is identified
    // by its T-suffix element, not by its state.
    TraceCascade viaX;
    viaX.setLength(4);
    viaX.push(stepHashOf(net.transitions[tx]));
    colony.depositPairAttract(s1, viaX, 1.5);
    colony.depositPairRepel(s1, viaX, 0.25);

    TraceCascade viaY;
    viaY.setLength(4);
    viaY.push(stepHashOf(net.transitions[ty]));
    colony.depositPairAttract(s2, viaY, 2.0);

    const std::vector<AntColony::PairEntry> rows = colony.pairEntries();
    CHECK(rows.size() == 2);
    CHECK(rows[0].state <= rows[1].state);  // sorted by state first
    bool sawX = false, sawY = false;
    for (const AntColony::PairEntry &e : rows) {
        if (e.tsuffix == viaX.elements()) {
            sawX = true;
            CHECK(e.state == s1);
            CHECK(near(e.attract, 1.5));
            CHECK(near(e.repel, 0.25));
        } else if (e.tsuffix == viaY.elements()) {
            sawY = true;
            CHECK(e.state == s2);
            CHECK(near(e.attract, 2.0));
            CHECK(near(e.repel, 0.0));
        }
    }
    CHECK(sawX && sawY);
}

static void testPairWeightRidesOnTheEdgeWeight()
{
    const Net net = makeTwoTargetNet();
    ReachabilityGraph rg(net);
    const StateId start = rg.addInitial(0);
    rg.exploreAll();
    const TransitionIndex tx = *net.findTransition("x");
    const EdgeId ex = *rg.findEdge(start, tx);
    const EdgeId ey = *rg.findEdge(start, *net.findTransition("y"));
    const StateId sx = rg.edge(ex).target;

    AntConfig cfg;
    cfg.tsuffixLength = 4;
    CoverageDb db(rg);
    AntColony colony(rg, db, cfg);

    // The pair pheromone of the arrival context this edge would create rides
    // on top of the edge channels: the pair repulsion joins the exponent of
    // the decay. `trace` is the ant's cascade; the consult extends it by the
    // edge's own step, so the deposit sits under exactly that key.
    const TraceCascade trace;  // length 4, all kTraceSeed
    colony.depositPairRepel(sx, trace.extended(stepHashOf(net.transitions[tx])), 2.0);
    CHECK(near(colony.inputWeight(ex, trace), 0.1 * std::exp(-2.0), 1e-6));
    CHECK(near(colony.inputWeight(ey, trace), 0.1));      // the other route untouched
    CHECK(near(colony.inputWeight(ex), 0.1));             // the edge channels untouched

    {   // with the layer off, the two-argument form is the plain weight
        CoverageDb db2(rg);
        AntColony off(rg, db2);
        CHECK(near(off.inputWeight(ex, trace), off.inputWeight(ex)));
    }
}

static void testPairLayerSteersTheSuggester()
{
    const Net net = makeTwoTargetNet();
    ReachabilityGraph rg(net);
    rg.addInitial(0);
    rg.exploreAll();
    const StateId sx = findState(rg, Marking{0, 1, 0});
    const StateId sy = findState(rg, Marking{0, 0, 1});

    CoverageDb db(rg);
    BeliefTracker tr(rg, &db);
    tr.beginRun(0);

    Suggester plain(rg, db);
    CHECK(plain.suggest(tr).label == "x");  // a tie, the lower index wins

    const TraceCascade trace;  // the run cascade at the first suggestion: all kTraceSeed
    const std::uint32_t hx = stepHashOf(net.transitions[*net.findTransition("x")]);
    const std::uint32_t hy = stepHashOf(net.transitions[*net.findTransition("y")]);

    {   // a catastrophe suffered via the x route repels that route only
        AntConfig cfg;
        cfg.tsuffixLength = 4;
        AntColony colony(rg, db, cfg);
        colony.depositPairRepel(sx, trace.extended(hx), 2.0);
        Suggester sg(rg, db);
        sg.setAntColony(&colony);
        sg.setTraceCascade(&trace);
        CHECK(sg.suggest(tr).label == "y");
    }
    {   // a milestone reached via the y route attracts that route only
        AntConfig cfg;
        cfg.tsuffixLength = 4;
        AntColony colony(rg, db, cfg);
        colony.depositPairAttract(sy, trace.extended(hy), 5.0);
        Suggester sg(rg, db);
        sg.setAntColony(&colony);
        sg.setTraceCascade(&trace);
        CHECK(sg.suggest(tr).label == "y");
    }
    {   // without the run cascade the pairs are not consulted at all
        AntConfig cfg;
        cfg.tsuffixLength = 4;
        AntColony colony(rg, db, cfg);
        colony.depositPairRepel(sx, trace.extended(hx), 2.0);
        Suggester sg(rg, db);
        sg.setAntColony(&colony);
        CHECK(sg.suggest(tr).label == "x");
    }
    {   // with the layer off the pairs are empty and the tie stands
        CoverageDb db2(rg);
        BeliefTracker tr2(rg, &db2);
        tr2.beginRun(0);
        AntColony colony(rg, db2);  // tsuffixLength 0 (the default)
        Suggester sg(rg, db2);
        sg.setAntColony(&colony);
        sg.setTraceCascade(&trace);
        CHECK(sg.suggest(tr2).label == "x");
        tr2.endRun(RunOutcome::Completed);
    }
    tr.endRun(RunOutcome::Completed);
}

// ---------------------------------------------------------------------------

int main()
{
    const std::vector<std::pair<const char *, std::function<void()>>> tests = {
        {"backpropDepthIsAtLeastFive", testBackpropDepthIsAtLeastFive},
        {"depositReachesExactlyFivePreviousNodes", testDepositReachesExactlyFivePreviousNodes},
        {"depositsAreCappedAtTauMax", testDepositsAreCappedAtTauMax},
        {"catastropheDepositGrowsWithTheWayIn", testCatastropheDepositGrowsWithTheWayIn},  // GLM-5.3-Flash
        {"anAntThatFindsATargetMarksTheWayIn", testAnAntThatFindsATargetMarksTheWayIn},
        {"catastrophesRepelAndAreAvoidedAfterwards", testCatastrophesRepelAndAreAvoidedAfterwards},
        {"unexploredTerritoryAttractsAndLazyExpansionIsBudgeted",
         testUnexploredTerritoryAttractsAndLazyExpansionIsBudgeted},
        {"saturatedEdgesLoseTheirAttraction", testSaturatedEdgesLoseTheirAttraction},
        {"attractiveAndRepellentPheromoneEvaporateAtDifferentRates",
         testAttractiveAndRepellentPheromoneEvaporateAtDifferentRates},
        {"inputWeightCombinesPheromoneAndNovelty", testInputWeightCombinesPheromoneAndNovelty},
        {"sameSeedSameColony", testSameSeedSameColony},
        {"pheromoneStaysWithinBounds", testPheromoneStaysWithinBounds},
        {"pheromoneSteersTheSuggester", testPheromoneSteersTheSuggester},
        {"statesWithoutATargetMarkingRepel", testStatesWithoutATargetMarkingRepel},  // GLM-5.3-Flash
        {"theRegionSparesUnknownTerritory", testTheRegionSparesUnknownTerritory},  // GLM-5.3-Flash
        {"theRegionSteersTheSuggester", testTheRegionSteersTheSuggester},  // GLM-5.3-Flash
        {"traceCascadeFollowsTheRecurrence", testTraceCascadeFollowsTheRecurrence},  // GLM-5.3-Flash
        {"pairsSeparateRoutesAndAreCapped", testPairsSeparateRoutesAndAreCapped},  // GLM-5.3-Flash
        {"pairsEvaporateAndArePruned", testPairsEvaporateAndArePruned},  // GLM-5.3-Flash
        {"agingCanWaitForTheRunBoundary", testAgingCanWaitForTheRunBoundary},  // DeepSeek V4.1 Flash
        {"maturedTargetsAreSparedByMergeEvaporation", testMaturedTargetsAreSparedByMergeEvaporation},  // DeepSeek V4.1 Flash
        {"pairWeightRidesOnTheEdgeWeight", testPairWeightRidesOnTheEdgeWeight},  // GLM-5.3-Flash
        {"pairLayerSteersTheSuggester", testPairLayerSteersTheSuggester},  // GLM-5.3-Flash
        {"pairEntriesAreReadableAndSorted", testPairEntriesAreReadableAndSorted},  // DeepSeek V4.1 Flash
    };
    for (const auto &t : tests) {
        const int before = g_failures;
        t.second();
        std::printf("%s %s\n", g_failures == before ? "PASS" : "FAIL", t.first);
    }
    std::printf("%d failure(s)\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
