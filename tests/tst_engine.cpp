// Tests for the Qt-free engine. A minimal harness keeps this runnable anywhere.
#include "engine/ReachabilityGraph.h"
#include "engine/Semantics.h"
#include "model/NetBuilder.h"

#include <cstdio>
#include <functional>
#include <string>
#include <vector>

using namespace pn;

static int g_failures = 0;

#define CHECK(cond)                                                                  \
    do {                                                                             \
        if (!(cond)) {                                                               \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                                            \
        }                                                                            \
    } while (0)

static StateId findState(const ReachabilityGraph &g, const Marking &m)
{
    for (StateId s = 0; s < g.numStates(); ++s)
        if (g.marking(s) == m)
            return s;
    return kNoState;
}

// ---------------------------------------------------------------------------

static void testEnablingAndFiringWithWeights()
{
    NetBuilder b;
    const auto p = b.place("p");
    const auto q = b.place("q");
    const auto t = b.transition("t");
    b.input(p, t, 2).output(t, q, 3);
    const Net net = b.build();
    const Transition &tr = net.transitions[t];

    CHECK(!isEnabled(tr, Marking{1, 0}));
    CHECK(isEnabled(tr, Marking{2, 0}));

    Marking to(2);
    const FireResult r = fire(net, tr, Marking{5, 1}.data(), to.data());
    CHECK(r.status == FireStatus::Ok);
    CHECK((to == Marking{3, 4}));
}

static void testInhibitorArcs()
{
    NetBuilder b;
    const auto p = b.place("p");
    const auto r = b.place("r");
    const auto t = b.transition("t");
    b.input(p, t).inhibitor(r, t, 2);
    const Net net = b.build();
    const Transition &tr = net.transitions[t];

    CHECK(isEnabled(tr, Marking{1, 0}));
    CHECK(isEnabled(tr, Marking{1, 1}));   // 1 < 2
    CHECK(!isEnabled(tr, Marking{1, 2}));  // 2 >= 2 inhibits
    CHECK(!isEnabled(tr, Marking{0, 0}));  // still needs its normal input

    // Firing never touches the inhibitor place.
    Marking to(2);
    CHECK(fire(net, tr, Marking{1, 1}.data(), to.data()).status == FireStatus::Ok);
    CHECK((to == Marking{0, 1}));
}

static void testDefaultInhibitorWeightMeansEmpty()
{
    NetBuilder b;
    const auto r = b.place("r");
    const auto t = b.transition("t");
    b.inhibitor(r, t);
    const Net net = b.build();
    CHECK(isEnabled(net.transitions[t], Marking{0}));
    CHECK(!isEnabled(net.transitions[t], Marking{1}));
}

static void testSelfLoopTransition()
{
    NetBuilder b;
    const auto p = b.place("p");
    const auto t = b.transition("t");
    b.input(p, t).output(t, p);
    b.initial("s", {{p, 1}});
    const Net net = b.build();

    ReachabilityGraph g(net);
    g.addInitial(0);
    g.exploreAll();
    CHECK(g.numStates() == 1);
    CHECK(g.numEdges() == 1);
    CHECK(g.successors(0).first->target == 0);
}

static void testMatchModes()
{
    NetBuilder b;
    const auto p = b.place("p");
    const auto q = b.place("q");
    b.targetMarking("f1", {{p, 1}});                        // exact: p=1, q=0
    b.targetMarking("f2", {{q, 1}});                        // exact: p=0, q=1
    b.errorMarking("e", {{q, 1}}, MatchMode::Covers);      // q>=1
    const Net net = b.build();

    CHECK(classify(net, Marking{1, 0}.data()).kind == StateKind::Target);
    CHECK(classify(net, Marking{1, 1}.data()).kind == StateKind::Error);  // covers; target exact fails
    CHECK(classify(net, Marking{0, 1}.data()).kind == StateKind::Error);  // both match: error wins
    CHECK(classify(net, Marking{0, 0}.data()).kind == StateKind::Normal);
    CHECK(classify(net, Marking{0, 1}.data()).markingIndex == 0);
}

static void testBoundViolationIsRecordedNotExplored()
{
    NetBuilder b;
    const auto q = b.place("q", 2);
    const auto t = b.transition("src");
    b.output(t, q);  // source transition: always enabled
    b.initial("s", {});
    const Net net = b.build();

    ReachabilityGraph g(net);
    g.addInitial(0);
    g.exploreAll();
    CHECK(g.numStates() == 3);  // q = 0, 1, 2
    CHECK(g.numEdges() == 2);
    CHECK(!g.truncated());
    CHECK(g.boundViolations().size() == 1);
    CHECK(g.boundViolations()[0].place == q);
    CHECK(!g.boundViolations()[0].overflow);
    CHECK(g.enabledCount(2) == 1);  // enabled, but the firing would break the bound
    CHECK(!g.isDeadlock(2));
}

static void testStateLimitTruncatesHonestly()
{
    NetBuilder b;
    const auto q = b.place("q");  // unbounded
    const auto t = b.transition("src");
    b.output(t, q);
    b.initial("s", {});
    const Net net = b.build();

    GenerationLimits lim;
    lim.maxStates = 10;
    ReachabilityGraph g(net, lim);
    g.addInitial(0);
    g.exploreAll();

    CHECK(g.numStates() == 10);
    CHECK(g.truncated());
    CHECK(g.isIncomplete(9));
    CHECK(!g.isIncomplete(0));
    CHECK(g.enabledCount(9) == 1);
    CHECK(!g.isDeadlock(9));  // a cut-off frontier is not a deadlock
    CHECK(g.deadlocks().empty());
}

static void testLazyExpansion()
{
    NetBuilder b;
    const auto p = b.place("p");
    const auto q = b.place("q");
    const auto t = b.transition("t");
    b.input(p, t).output(t, q);
    b.initial("s", {{p, 1}});
    const Net net = b.build();

    ReachabilityGraph g(net);
    const StateId s0 = g.addInitial(0);
    CHECK(g.numStates() == 1);
    CHECK(!g.isExpanded(s0));
    CHECK(g.expand(s0) == 1);
    CHECK(g.expand(s0) == 0);  // idempotent
    CHECK(g.numStates() == 2);
    CHECK(!g.isExpanded(1));   // successor exists but is not expanded yet
}

static void testInitialStatesAreDeduplicated()
{
    NetBuilder b;
    const auto p = b.place("p");
    b.initial("a", {{p, 1}});
    b.initial("b", {{p, 1}});
    const Net net = b.build();

    ReachabilityGraph g(net);
    const StateId a = g.addInitial(0);
    const StateId c = g.addInitial(1);
    CHECK(a == c);
    CHECK(g.numStates() == 1);
    CHECK(g.initialStates().size() == 1);
}

static void testDeadlockAndTargetClassification()
{
    // p --t--> q. Without a target marking, the dead end q is a deadlock.
    {
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
        CHECK(g.deadlocks().size() == 1);
        CHECK(g.deadlocks()[0] == findState(g, Marking{0, 1}));
    }
    // With q declared a target, the same dead end is a legitimate end state.
    {
        NetBuilder b;
        const auto p = b.place("p");
        const auto q = b.place("q");
        const auto t = b.transition("t");
        b.input(p, t).output(t, q);
        b.initial("s", {{p, 1}});
        b.targetMarking("done", {{q, 1}});
        const Net net = b.build();

        ReachabilityGraph g(net);
        g.addInitial(0);
        g.exploreAll();
        CHECK(g.deadlocks().empty());
        CHECK(g.statesOfKind(StateKind::Target).size() == 1);
    }
}

static void testStopAtErrorStates()
{
    NetBuilder b;
    const auto p = b.place("p");
    const auto q = b.place("q");
    const auto t = b.transition("t");
    const auto u = b.transition("u");
    b.input(p, t).output(t, q);
    b.input(q, u).output(u, p);
    b.initial("s", {{p, 1}});
    b.errorMarking("bad", {{q, 1}}, MatchMode::Covers);
    const Net net = b.build();

    GenerationLimits lim;
    lim.stopAtErrorStates = true;
    ReachabilityGraph g(net, lim);
    g.addInitial(0);
    g.exploreAll();
    const StateId bad = findState(g, Marking{0, 1});
    CHECK(bad != kNoState);
    CHECK(!g.isExpanded(bad));
    CHECK(g.numStates() == 2);
}

// Mirrors examples/elevator-door.json. The expected numbers were derived by
// hand: closed -> opening -> open -> {closing, open+obstructed}; closing ->
// {closed, fault}; open+obstructed -> open (via clear).
static void testElevatorDoorModel()
{
    NetBuilder b("elevator-door");
    const auto closed = b.place("closed");
    const auto opening = b.place("opening");
    const auto open = b.place("open");
    const auto closing = b.place("closing");
    const auto obstructed = b.place("obstructed", 1);
    const auto fault = b.place("fault", 1);

    const auto openCmd = b.transition("open_cmd", TransitionKind::Controllable, "door.open");
    const auto opened = b.transition("opened", TransitionKind::Observable, "door.opened");
    const auto closeCmd = b.transition("close_cmd", TransitionKind::Controllable, "door.close");
    const auto closedEv = b.transition("closed_ev", TransitionKind::Observable, "door.closed");
    const auto obstruct = b.transition("obstruct");
    const auto clear = b.transition("clear");
    const auto jam = b.transition("jam");

    b.input(closed, openCmd).output(openCmd, opening);
    b.input(opening, opened).output(opened, open);
    b.input(open, closeCmd).output(closeCmd, closing).inhibitor(obstructed, closeCmd);
    b.input(closing, closedEv).output(closedEv, closed);
    b.input(open, obstruct).output(obstruct, open).output(obstruct, obstructed)
        .inhibitor(obstructed, obstruct);
    b.input(obstructed, clear);
    b.input(closing, jam).output(jam, fault);

    b.initial("start", {{closed, 1}});
    b.targetMarking("idle_closed", {{closed, 1}});
    b.errorMarking("jammed", {{fault, 1}}, MatchMode::Covers);
    const Net net = b.build();

    ReachabilityGraph g(net);
    g.addInitial(0);
    g.exploreAll();

    CHECK(g.numStates() == 6);
    CHECK(g.numEdges() == 7);
    CHECK(!g.truncated());
    CHECK(g.boundViolations().empty());
    CHECK(g.deadlocks().empty());
    CHECK(g.statesOfKind(StateKind::Error).size() == 1);
    CHECK(g.statesOfKind(StateKind::Target).size() == 1);

    // Marking order: closed, opening, open, closing, obstructed, fault.
    const StateId blocked = findState(g, Marking{0, 0, 1, 0, 1, 0});
    CHECK(blocked != kNoState);
    CHECK(g.successors(blocked).size() == 1);  // only "clear": both inhibitors bite
    CHECK(g.successors(blocked).first->transition == clear);

    const StateId open0 = findState(g, Marking{0, 0, 1, 0, 0, 0});
    CHECK(open0 != kNoState);
    CHECK(g.successors(open0).size() == 2);  // close_cmd and obstruct

    const StateId jammed = findState(g, Marking{0, 0, 0, 0, 0, 1});
    CHECK(jammed != kNoState);
    CHECK(g.kind(jammed) == StateKind::Error);
    CHECK(g.enabledCount(jammed) == 0);
    CHECK(!g.isDeadlock(jammed));  // reported through its error kind instead
}

static void testHashDistinguishesPermutations()
{
    const Marking a{1, 0, 2};
    const Marking b{0, 1, 2};
    CHECK(hashTokens(a.data(), a.size()) != hashTokens(b.data(), b.size()));
    CHECK(hashTokens(a.data(), a.size()) == hashTokens(Marking{1, 0, 2}.data(), 3));
}

// ---------------------------------------------------------------------------

int main()
{
    const std::vector<std::pair<const char *, std::function<void()>>> tests = {
        {"enablingAndFiringWithWeights", testEnablingAndFiringWithWeights},
        {"inhibitorArcs", testInhibitorArcs},
        {"defaultInhibitorWeightMeansEmpty", testDefaultInhibitorWeightMeansEmpty},
        {"selfLoopTransition", testSelfLoopTransition},
        {"matchModes", testMatchModes},
        {"boundViolationIsRecordedNotExplored", testBoundViolationIsRecordedNotExplored},
        {"stateLimitTruncatesHonestly", testStateLimitTruncatesHonestly},
        {"lazyExpansion", testLazyExpansion},
        {"initialStatesAreDeduplicated", testInitialStatesAreDeduplicated},
        {"deadlockAndTargetClassification", testDeadlockAndTargetClassification},
        {"stopAtErrorStates", testStopAtErrorStates},
        {"elevatorDoorModel", testElevatorDoorModel},
        {"hashDistinguishesPermutations", testHashDistinguishesPermutations},
    };
    for (const auto &t : tests) {
        const int before = g_failures;
        t.second();
        std::printf("%s %s\n", g_failures == before ? "PASS" : "FAIL", t.first);
    }
    std::printf("%d failure(s)\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
