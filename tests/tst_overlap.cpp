// Tests for the overlap analysis: the exact per-run attribution of covered
// elements, the Jaccard index, the pairwise matrices and the campaign union.
#include "MiniTest.h"

#include "engine/CoverageDb.h"
#include "engine/OverlapAnalysis.h"
#include "engine/ReachabilityGraph.h"
#include "model/NetBuilder.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

using namespace pn;

namespace {

// s0 -x-> s1 -ox-> s0 and s0 -y-> s2 -oy-> s0, so two runs can take disjoint
// branches and share exactly the initial state.
Net makeTwoBranchNet()
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

// One complete path-mode run along transition ids.
RunId pathRun(CoverageDb &db, ReachabilityGraph &rg, StateId initial, const std::vector<const char *> &ids)
{
    db.beginRun(rg.addInitial(initial));
    for (const char *id : ids) {
        const auto t = rg.net().findTransition(id);
        T_VERIFY(t.has_value());
        rg.expand(db.currentState());
        const auto e = rg.findEdge(db.currentState(), *t);
        T_VERIFY(e.has_value());
        T_VERIFY(db.step(*e));
    }
    db.endRun(RunOutcome::Completed);
    return db.run(db.numRuns() - 1).id;
}

bool hasState(const std::vector<std::uint32_t> &v, std::uint32_t s)
{
    return std::find(v.begin(), v.end(), s) != v.end();
}

bool closeTo(double a, double b)
{
    return std::fabs(a - b) < 1e-9;
}

}  // namespace

struct TstOverlap {
    void jaccardIndexBasics()
    {
        const std::vector<std::uint32_t> empty, a = {1, 2, 3}, b = {2, 3, 4}, half1 = {1, 2}, half2 = {2, 3};
        T_VERIFY(closeTo(jaccardIndex(a, a), 1.0));
        T_VERIFY(closeTo(jaccardIndex(a, b), 2.0 / 4.0));  // {2,3} over {1,2,3,4}
        T_VERIFY(closeTo(jaccardIndex(half1, half2), 1.0 / 3.0));
        T_VERIFY(closeTo(jaccardIndex(a, empty), 0.0));
        T_VERIFY(closeTo(jaccardIndex(empty, empty), 1.0));
    }

    void perRunSetsAreExact()
    {
        const Net net = makeTwoBranchNet();
        ReachabilityGraph rg(net);
        CoverageDb db(rg);
        rg.addInitial(0);
        rg.exploreAll();

        const RunId xa = pathRun(db, rg, 0, {"x", "ox"});
        const RunId ya = pathRun(db, rg, 0, {"y", "oy"});

        const RunCoverage cx = runCoverage(db, xa);
        const RunCoverage cy = runCoverage(db, ya);
        const auto xt = net.findTransition("x"), oxt = net.findTransition("ox");
        const auto yt = net.findTransition("y"), oyt = net.findTransition("oy");
        T_VERIFY(hasState(cx.transitions, *xt) && hasState(cx.transitions, *oxt));
        T_VERIFY(!hasState(cx.transitions, *yt) && !hasState(cx.transitions, *oyt));
        T_VERIFY(hasState(cy.transitions, *yt) && hasState(cy.transitions, *oyt));
        T_COMPARE(cx.states.size(), std::size_t(2));  // s0 and s1
        T_COMPARE(cx.edges.size(), std::size_t(2));
        T_COMPARE(cx.transitions.size(), std::size_t(2));

        // The branches share only the initial state.
        T_VERIFY(closeTo(jaccardIndex(cx.states, cy.states), 1.0 / 3.0));
        T_VERIFY(closeTo(jaccardIndex(cx.transitions, cy.transitions), 0.0));
        T_VERIFY(closeTo(jaccardIndex(cx.edges, cy.edges), 0.0));
    }

    void attributionWithoutAKnownEdge()
    {
        // A run can credit states it reached without a known edge (ambiguous
        // stretches) and transitions whose edge stayed ambiguous; both must
        // land in the run's sets.
        const Net net = makeTwoBranchNet();
        ReachabilityGraph rg(net);
        rg.addInitial(0);
        rg.exploreAll();
        CoverageDb db(rg);
        db.beginRun(rg.statesOfKind(StateKind::Normal).front(), "probe", /*creditInitial=*/false);
        const StateId s1 = rg.statesOfKind(StateKind::Normal).at(1);
        db.creditDefinite(s1, kNoEdge);
        db.creditTransition(*net.findTransition("x"));
        db.endRun(RunOutcome::Completed);

        const RunCoverage c = runCoverage(db, db.numRuns() - 1);
        T_VERIFY(hasState(c.states, s1));
        T_VERIFY(hasState(c.transitions, *net.findTransition("x")));
        T_VERIFY(c.edges.empty());
    }

    void matrixIsSymmetricWithDiagonalOne()
    {
        const Net net = makeTwoBranchNet();
        ReachabilityGraph rg(net);
        CoverageDb db(rg);
        rg.addInitial(0);
        rg.exploreAll();
        pathRun(db, rg, 0, {"x", "ox"});
        pathRun(db, rg, 0, {"y", "oy"});
        pathRun(db, rg, 0, {"x", "ox"});

        const OverlapMatrix m = runOverlapMatrix(db);
        T_COMPARE(m.n, std::size_t(3));
        T_VERIFY(m.transitions.size() == 9 && m.states.size() == 9 && m.edges.size() == 9);
        for (std::size_t i = 0; i < 3; ++i) {
            T_VERIFY(closeTo(m.at(m.transitions, i, i), 1.0));
            for (std::size_t j = 0; j < 3; ++j)
                T_VERIFY(closeTo(m.at(m.transitions, i, j), m.at(m.transitions, j, i)));
        }
        // Runs 0 and 2 are identical, run 1 is disjoint from both.
        T_VERIFY(closeTo(m.at(m.transitions, 0, 2), 1.0));
        T_VERIFY(closeTo(m.at(m.transitions, 0, 1), 0.0));
        T_VERIFY(closeTo(m.meanPairwise(m.transitions), 1.0 / 3.0));  // (0 + 1 + 0) / 3
    }

    void campaignCoverageIsTheUnion()
    {
        const Net net = makeTwoBranchNet();
        ReachabilityGraph rg(net);
        CoverageDb db(rg);
        rg.addInitial(0);
        rg.exploreAll();
        pathRun(db, rg, 0, {"x", "ox"});
        pathRun(db, rg, 0, {"y", "oy"});

        const CampaignCoverage c = campaignCoverage(db);
        T_COMPARE(c.transitions.size(), std::size_t(4));
        T_COMPARE(c.states.size(), std::size_t(3));
        T_COMPARE(c.edges.size(), std::size_t(4));
    }

    void emptyRunsAreIdentical()
    {
        const Net net = makeTwoBranchNet();
        ReachabilityGraph rg(net);
        CoverageDb db(rg);
        rg.addInitial(0);
        db.beginRun(rg.statesOfKind(StateKind::Normal).front());
        db.endRun(RunOutcome::Aborted, "nothing happened");
        db.beginRun(rg.statesOfKind(StateKind::Normal).front());
        db.endRun(RunOutcome::Aborted, "also nothing");

        const OverlapMatrix m = runOverlapMatrix(db);
        T_VERIFY(closeTo(m.at(m.transitions, 0, 1), 1.0));  // both covered nothing
        T_VERIFY(closeTo(m.meanPairwise(m.transitions), 1.0));
    }
};

int main()
{
    T_RUN(TstOverlap, jaccardIndexBasics);
    T_RUN(TstOverlap, perRunSetsAreExact);
    T_RUN(TstOverlap, attributionWithoutAKnownEdge);
    T_RUN(TstOverlap, matrixIsSymmetricWithDiagonalOne);
    T_RUN(TstOverlap, campaignCoverageIsTheUnion);
    T_RUN(TstOverlap, emptyRunsAreIdentical);
    return T_SUMMARY();
}
