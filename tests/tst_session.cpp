// End-to-end tests: a Session drives a simulated SUT (a plug-in on its own
// thread) through whole test runs.
#include "ElevatorModel.h"
#include "FakeSut.h"
#include "TempDir.h"
#include "engine/Session.h"
#include "engine/TList.h"
#include "model/NetBuilder.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <set>
#include <string>
#include <thread>
#include <vector>

using namespace pn;
using testsut::FakeSut;
using testsut::FakeSutOptions;

static int g_failures = 0;

#define CHECK(cond)                                                                       \
    do {                                                                                  \
        if (!(cond)) {                                                                    \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                                                 \
        }                                                                                 \
    } while (0)

static bool contains(const std::string &text, const char *part)
{
    return text.find(part) != std::string::npos;
}

static bool near(double a, double b, double eps = 1e-6)
{
    return std::fabs(a - b) <= eps;
}

static SessionConfig quick()
{
    SessionConfig cfg;
    cfg.poll = std::chrono::milliseconds(2);
    cfg.plugin.quiescenceTimeoutMs = 30;
    cfg.idleTimeout = std::chrono::milliseconds(3000);
    cfg.stimulusAckTimeout = std::chrono::milliseconds(3000);
    cfg.maxRuns = 5;
    return cfg;
}

// s0 offers three inputs; each is answered by an output and returns to s0.
// s0 is the idle state and a target marking; without one the whole model would
// be a terminal cycle, which the static analysis rightly calls a livelock.
static Net makeThreeBranchNet(bool withTargetMarking = true)
{
    NetBuilder b;
    const auto s0 = b.place("s0");
    std::vector<PlaceIndex> mid;
    for (const char *name : {"s1", "s2", "s3"})
        mid.push_back(b.place(name));
    const char *inputs[] = {"a", "b", "c"};
    const char *outputs[] = {"oa", "ob", "oc"};
    for (std::size_t i = 0; i < 3; ++i) {
        const auto in = b.transition(inputs[i], TransitionKind::Controllable, inputs[i]);
        const auto out = b.transition(outputs[i], TransitionKind::Observable, outputs[i]);
        b.input(s0, in).output(in, mid[i]);
        b.input(mid[i], out).output(out, s0);
    }
    b.initial("start", {{s0, 1}});
    if (withTargetMarking)
        b.targetMarking("idle", {{s0, 1}});
    return b.build();
}

// GLM-5.3-Flash: one corridor: s0 -a-> s1 -oa-> s0, with s0 the intentional
// target marking. The environment-feed test drives this with a slowed SUT so
// the session's wait episodes are deterministic (see below).
static Net makeOneCorridorNet()
{
    NetBuilder b("one-corridor");
    const auto s0 = b.place("s0");
    const auto s1 = b.place("s1");
    const auto a = b.transition("a", TransitionKind::Controllable, "a");
    const auto oa = b.transition("oa", TransitionKind::Observable, "oa");
    b.input(s0, a).output(a, s1);
    b.input(s1, oa).output(oa, s0);
    b.initial("start", {{s0, 1}});
    b.targetMarking("idle", {{s0, 1}});
    return b.build();
}

// A SUT that can only cycle: a -ping-> b -pong-> a, no inputs, no target marking.
static Net makePingPongNet()
{
    NetBuilder b;
    const auto a = b.place("a");
    const auto pb = b.place("b");
    const auto ping = b.transition("ping", TransitionKind::Observable, "ping");
    const auto pong = b.transition("pong", TransitionKind::Observable, "pong");
    b.input(a, ping).output(ping, pb);
    b.input(pb, pong).output(pong, a);
    b.initial("start", {{a, 1}});
    return b.build();
}

// s0 emits "tick" all the time (self-loop); the input "a" leads to s1, which answers "oa".
static Net makeTickNet()
{
    NetBuilder b;
    const auto s0 = b.place("s0");
    const auto s1 = b.place("s1");
    const auto tick = b.transition("tick", TransitionKind::Observable, "tick");
    const auto a = b.transition("a", TransitionKind::Controllable, "a");
    const auto oa = b.transition("oa", TransitionKind::Observable, "oa");
    b.input(s0, tick).output(tick, s0);
    b.input(s0, a).output(a, s1);
    b.input(s1, oa).output(oa, s0);
    b.initial("start", {{s0, 1}});
    b.targetMarking("idle", {{s0, 1}});
    return b.build();
}

// A SUT that jams whenever it closes the door.
static FakeSutOptions jamming()
{
    FakeSutOptions o;
    o.weight = {{"obstruct", 0}, {"clear", 0}, {"closed_ev", 0}};
    return o;
}

// ---------------------------------------------------------------------------

static void testACampaignReachesFullCoverageAndRestoresLater()
{
    const Net net = makeThreeBranchNet();
    FakeSut sut(net);

    std::vector<PersistedRun> saved;
    SessionConfig cfg = quick();
    cfg.runSink = [&saved](const PersistedRun &r) { saved.push_back(r); };
    Session session(net, cfg);

    const CampaignReport rep = session.runCampaign(sut);
    CHECK(rep.complete && rep.stopReason == "coverage complete");
    CHECK(rep.runs.size() == 1);
    CHECK(rep.runs[0].outcome == RunOutcome::Completed && rep.runs[0].coverageDone);
    CHECK(rep.runs[0].stimuli == 3 && rep.runs[0].events == 6);
    CHECK(rep.summary.transitionsCovered == 6 && rep.summary.statesCovered == 4);

    const auto received = sut.stimuliReceived();
    CHECK(received.size() == 3);
    CHECK((std::set<std::string>(received.begin(), received.end()) == std::set<std::string>{"a", "b", "c"}));
    CHECK(saved.size() == 1 && saved[0].events.size() == 6);
    CHECK(sut.stopCalls() == 1);

    // A later session continues where this one stopped.
    Session later(net, quick());
    CHECK(later.restore(saved[0]).ok);
    CHECK(later.coverage().summary().transitionsCovered == 6);
    FakeSut again(net);
    const CampaignReport rep2 = later.runCampaign(again);
    CHECK(rep2.complete && rep2.runs.size() == 1 && rep2.runs[0].events == 0);  // nothing left to do
    CHECK(again.stimuliReceived().empty());
}

static void testNoSecondInputWhileOneIsInFlight()
{
    // While the input "a" is on its way (60 ms), the SUT keeps ticking. Every tick
    // changes the picture, and a naive driver would send "a" again and again.
    const Net net = makeTickNet();
    FakeSutOptions opt;
    opt.ackDelayMs = 60;
    FakeSut sut(net, opt);

    std::vector<PersistedRun> saved;
    SessionConfig cfg = quick();
    cfg.maxEventsPerRun = 60;
    cfg.maxRuns = 1;
    cfg.runSink = [&saved](const PersistedRun &r) { saved.push_back(r); };
    Session session(net, cfg);

    const RunReport rep = session.runOnce(sut);
    CHECK(rep.stimuli == 1);
    CHECK(sut.stimuliReceived().size() == 1);
    CHECK(rep.outcome == RunOutcome::Completed && contains(rep.detail, "run length limit"));

    CHECK(saved.size() == 1);
    // The stimulus was acknowledged among the ticks, and answered by "oa".
    bool answered = false;
    const auto &events = saved[0].events;
    for (std::size_t i = 0; i < events.size(); ++i) {
        if (events[i].kind == EventKind::Stimulated) {
            for (std::size_t j = i + 1; j < events.size(); ++j) {
                if (events[j].label == "tick")
                    continue;
                answered = events[j].kind == EventKind::Observed && events[j].label == "oa";
                break;
            }
        }
    }
    CHECK(answered);
}

static void testAJamIsFoundByQuiescenceAndDumpedExactlyOnce()
{
    ErrorOptions jammed;
    jammed.crashDump = DumpMode::Snapshot;
    jammed.maxDumps = 1;
    const Net net = testmodels::makeElevatorNet(jammed);
    FakeSut sut(net, jamming());

    std::vector<PersistedRun> saved;
    SessionConfig cfg = quick();
    cfg.maxRuns = 2;
    cfg.runSink = [&saved](const PersistedRun &r) { saved.push_back(r); };
    Session session(net, cfg);

    const CampaignReport rep = session.runCampaign(sut);
    CHECK(rep.runs.size() == 2 && !rep.complete && rep.stopReason == "run limit reached");

    // Run 1: open, opened, close, then silence: the jam is the only state that may be silent.
    const RunReport &r1 = rep.runs[0];
    CHECK(r1.outcome == RunOutcome::ErrorReached && contains(r1.detail, "jammed"));
    CHECK(r1.dumps == 1);
    CHECK((r1.errorsReached == std::vector<std::string>{"jammed"}));
    CHECK(r1.events == 4);

    // Run 2 enters the error again, but the dump budget (max_dumps = 1) is used up.
    const RunReport &r2 = rep.runs[1];
    CHECK(r2.outcome == RunOutcome::ErrorReached);
    CHECK(r2.dumps == 0);

    CHECK(session.coverage().errorMarkingHits(0) == 2);
    CHECK(session.coverage().errorMarkingDumps(0) == 1);
    CHECK((sut.dumpModes() == std::vector<DumpMode>{DumpMode::Snapshot}));

    CHECK(saved.size() == 2);
    const auto &ev = saved[0].events;
    CHECK(ev.size() == 4);
    CHECK(ev[0].kind == EventKind::Stimulated && ev[0].label == "door.open");
    CHECK(ev[1].kind == EventKind::Observed && ev[1].label == "door.opened");
    CHECK(ev[2].kind == EventKind::Stimulated && ev[2].label == "door.close");
    CHECK(ev[3].kind == EventKind::Quiescent);
    CHECK(saved[0].dumps.size() == 1 && saved[0].dumps[0].stepIndex == 4);
    CHECK(saved[0].dumps[0].marking == "jammed" && saved[0].dumps[0].stackText == "frame #0 simulated_sut_main()");

    // Restoring both runs restores the dump budget and the hit count.
    Session later(net, quick());
    for (const PersistedRun &pr : saved)
        CHECK(later.restore(pr).ok);
    CHECK(later.coverage().errorMarkingHits(0) == 2);
    CHECK(later.coverage().errorMarkingDumps(0) == 1 && !later.coverage().dumpAllowed(0));
}

static void testACrashEndsTheRunAndTheNextRunRestartsTheSut()
{
    ErrorOptions jammed;
    jammed.crashDump = DumpMode::Crash;
    jammed.maxDumps = 1;
    const Net net = testmodels::makeElevatorNet(jammed);
    FakeSut sut(net, jamming());

    SessionConfig cfg = quick();
    cfg.maxRuns = 1;
    Session session(net, cfg);

    const RunReport r1 = session.runOnce(sut);
    CHECK(r1.outcome == RunOutcome::SutCrashed && contains(r1.detail, "jammed"));
    CHECK(r1.dumps == 1);
    CHECK(sut.crashed());
    CHECK((sut.dumpModes() == std::vector<DumpMode>{DumpMode::Crash}));
    CHECK(session.coverage().run(r1.id).dumps[0].mode == DumpMode::Crash);

    // The budget is used up: the second run reaches the error again, without a crash.
    const RunReport r2 = session.runOnce(sut);
    CHECK(r2.outcome == RunOutcome::ErrorReached);
    CHECK(r2.dumps == 0);
    CHECK(!sut.crashed());  // it was restarted, and stays up
}

static void testDumpsFallBackToWhatThePluginCan()
{
    ErrorOptions jammed;
    jammed.crashDump = DumpMode::Crash;
    const Net net = testmodels::makeElevatorNet(jammed);

    {   // cannot crash, can snapshot: a snapshot is taken instead and the run goes on
        FakeSutOptions opt = jamming();
        opt.crash = false;
        FakeSut sut(net, opt);
        Session session(net, quick());
        const RunReport r = session.runOnce(sut);
        CHECK(r.outcome == RunOutcome::ErrorReached && r.dumps == 1);
        CHECK((sut.dumpModes() == std::vector<DumpMode>{DumpMode::Snapshot}));
        CHECK(!r.notes.empty() && contains(r.notes[0], "snapshot instead"));
    }
    {   // can do neither: no post-mortem, said so, run unaffected
        FakeSutOptions opt = jamming();
        opt.crash = false;
        opt.snapshot = false;
        FakeSut sut(net, opt);
        Session session(net, quick());
        const RunReport r = session.runOnce(sut);
        CHECK(r.outcome == RunOutcome::ErrorReached && r.dumps == 0);
        CHECK(sut.dumpModes().empty());
        CHECK(!r.notes.empty() && contains(r.notes[0], "no post-mortem"));
        CHECK(session.coverage().errorMarkingDumps(0) == 0);
    }
}

// ---------------------------------------------------------------------------
// GLM-5.3-Flash: the case for the ants. A crash costs wall-clock time: the
// dump must be root-caused and the SUT must be restarted, and while that is
// going on the campaign should test somewhere else instead of re-finding the
// crash. Both scenarios have a cheap crash region (the cheapest goal: a
// target that is still being sought) and two deeper safe corridors left to
// cover; the campaign has to hit the crash target twice, so a second crash is
// unavoidable -- what the colony buys is WHERE it happens: after the rest of
// the model is covered, not before anything else was tested.
// ---------------------------------------------------------------------------

// The labels the session stimulated, per recorded run (from the run log).
static std::vector<std::string> stimulatedLabels(const PersistedRun &pr)
{
    std::vector<std::string> out;
    for (const SutEvent &e : pr.events)
        if (e.kind == EventKind::Stimulated)
            out.push_back(e.label);
    return out;
}

// s0 -c1-> crash: one input into the "boom" error marking, which wants two
// definite hits and may crash the SUT for a post-mortem each time. The safe
// part is two corridors s0 -a1-> a1p -a2-> a2p -oa-> s0 and its b twin. The
// crash input is declared first: once the crash path is covered, a colony-less
// campaign ties between the crash target (one step, still sought) and the
// fresh corridor edges (one step each), and the tie goes to the lower
// transition index -- back into the crash.
static Net makeCrashRegionNet(ErrorOptions boom)
{
    NetBuilder b("crash-region");
    const auto s0 = b.place("s0");
    const auto crash = b.place("crash");
    const auto a1p = b.place("a1p");
    const auto a2p = b.place("a2p");
    const auto b1p = b.place("b1p");
    const auto b2p = b.place("b2p");

    const auto c1 = b.transition("c1", TransitionKind::Controllable, "c1");
    const auto a1 = b.transition("a1", TransitionKind::Controllable, "a1");
    const auto a2 = b.transition("a2", TransitionKind::Controllable, "a2");
    const auto oa = b.transition("oa", TransitionKind::Controllable, "oa");
    const auto b1 = b.transition("b1", TransitionKind::Controllable, "b1");
    const auto b2 = b.transition("b2", TransitionKind::Controllable, "b2");
    const auto ob = b.transition("ob", TransitionKind::Controllable, "ob");

    b.input(s0, c1).output(c1, crash);
    b.input(s0, a1).output(a1, a1p);
    b.input(a1p, a2).output(a2, a2p);
    b.input(a2p, oa).output(oa, s0);
    b.input(s0, b1).output(b1, b1p);
    b.input(b1p, b2).output(b2, b2p);
    b.input(b2p, ob).output(ob, s0);

    b.initial("start", {{s0, 1}});
    b.targetMarking("idle", {{s0, 1}});
    b.errorMarking("boom", {{crash, 1}}, MatchMode::Exact, boom);
    return b.build();
}

static void testAfterACrashTheAntsKeepTheCampaignProductive()
{
    ErrorOptions boom;
    boom.targetHits = 2;   // the marking wants two definite visits
    boom.crashDump = DumpMode::Crash;
    boom.maxDumps = 2;     // and may crash the SUT for each of them
    const Net net = makeCrashRegionNet(boom);

    // --- with the colony -----------------------------------------------------------------------
    std::vector<std::vector<std::string>> antLabels;
    SessionConfig cfg = quick();
    cfg.maxRuns = 6;
    // A real crash costs a restart and an engineer's investigation; this
    // campaign weighs the colony's repel heavily enough that the marked way
    // in loses to fresh corridors, but lightly enough that once the rest is
    // covered the still-sought crash target wins again.
    cfg.suggester.pheromoneRepelWeight = 12.0;
    cfg.runSink = [&antLabels](const PersistedRun &pr) { antLabels.push_back(stimulatedLabels(pr)); };
    Session session(net, cfg);
    FakeSut sut(net);
    const CampaignReport with = session.runCampaign(sut);
    CHECK(with.runs.size() == 3);
    CHECK(with.complete);
    // Run 1 finds the crash: one input, one dump, and the colony is told.
    CHECK(with.runs[0].outcome == RunOutcome::SutCrashed && with.runs[0].dumps == 1);
    CHECK(with.runs[0].stimuli == 1 && with.runs[0].newEdges == 1);
    CHECK(!with.runs[0].notes.empty() && contains(with.runs[0].notes.back(), "crash site"));
    // The research window: while dump #1 is being analyzed, run 2 covers both
    // corridors (six new edges) and only then delivers the second,
    // policy-mandated hit -- the way back into the crash region is the last
    // thing it does.
    CHECK(with.runs[1].outcome == RunOutcome::SutCrashed && with.runs[1].dumps == 1);
    CHECK(with.runs[1].stimuli == 7 && with.runs[1].newEdges == 6);
    CHECK(!with.runs[1].notes.empty() && contains(with.runs[1].notes.back(), "crash site"));
    CHECK(antLabels.size() == 3 && antLabels[1].size() == 7);
    CHECK(antLabels[1].front() == "a1" || antLabels[1].front() == "b1");
    CHECK(antLabels[1][6] == "c1");
    CHECK(with.runs[2].coverageDone);

    // --- without the colony: the campaign re-finds the crash -----------------------------------
    std::vector<std::vector<std::string>> plainLabels;
    SessionConfig noAnts = quick();
    noAnts.maxRuns = 6;
    noAnts.useAnts = false;
    noAnts.runSink = [&plainLabels](const PersistedRun &pr) { plainLabels.push_back(stimulatedLabels(pr)); };
    Session plain(net, noAnts);
    FakeSut plainSut(net);
    const CampaignReport without = plain.runCampaign(plainSut);

    CHECK(without.runs.size() == 3);
    CHECK(without.runs[0].outcome == RunOutcome::SutCrashed && without.runs[0].dumps == 1);
    CHECK(without.runs[0].notes.empty());  // no colony: nothing is told anything
    // Run 2 goes straight back into the crash region: the same one input, no
    // new coverage, and dump #2 of the same bug before anything else was
    // tested. The tie between the covered crash path (still sought) and the
    // fresh corridor edges goes to the lower transition index.
    CHECK(without.runs[1].outcome == RunOutcome::SutCrashed && without.runs[1].dumps == 1);
    CHECK(without.runs[1].stimuli == 1 && without.runs[1].newEdges == 0);
    CHECK(plainLabels.size() == 3 && plainLabels[1].size() == 1 && plainLabels[1][0] == "c1");
    CHECK(without.runs[2].coverageDone);
    CHECK(without.runs[2].stimuli == 6 && without.runs[2].newEdges == 6);
}

// The same defence for a deadlock: s0 -d1-> dead is a sink (the SUT stops
// answering there), next to the two safe corridors. A true sink deadlock never
// produces the run-time Deadlock outcome: the suggester runs out of work while
// the SUT sits in the sink, and the run ends stranded ("stuck") long before a
// quiescence report could name a deadlock -- so no post-mortem is taken and
// the deadlock target is credited by the visit itself, not by a dump. The
// colony still has to learn from the stranded run, which is what the
// stuck-in-deadlock deposit is for. The deadlock is a target to visit twice;
// without the colony the campaign walks straight back in.
static Net makeDeadlockRegionNet()
{
    NetBuilder b("deadlock-region");
    const auto s0 = b.place("s0");
    const auto dead = b.place("dead");
    const auto a1p = b.place("a1p");
    const auto a2p = b.place("a2p");
    const auto b1p = b.place("b1p");
    const auto b2p = b.place("b2p");

    const auto d1 = b.transition("d1", TransitionKind::Controllable, "d1");
    const auto a1 = b.transition("a1", TransitionKind::Controllable, "a1");
    const auto a2 = b.transition("a2", TransitionKind::Controllable, "a2");
    const auto oa = b.transition("oa", TransitionKind::Controllable, "oa");
    const auto b1 = b.transition("b1", TransitionKind::Controllable, "b1");
    const auto b2 = b.transition("b2", TransitionKind::Controllable, "b2");
    const auto ob = b.transition("ob", TransitionKind::Controllable, "ob");

    b.input(s0, d1).output(d1, dead);  // dead has no way out
    b.input(s0, a1).output(a1, a1p);
    b.input(a1p, a2).output(a2, a2p);
    b.input(a2p, oa).output(oa, s0);
    b.input(s0, b1).output(b1, b1p);
    b.input(b1p, b2).output(b2, b2p);
    b.input(b2p, ob).output(ob, s0);

    b.initial("start", {{s0, 1}});
    b.targetMarking("idle", {{s0, 1}});
    return b.build();
}

static void testAfterADeadlockCrashTheAntsDoTheSame()
{
    const Net net = makeDeadlockRegionNet();

    // --- with the colony -----------------------------------------------------------------------
    std::vector<std::vector<std::string>> antLabels;
    SessionConfig cfg = quick();
    cfg.maxRuns = 6;
    cfg.targets.deadlock = TargetPolicy::UntilCovered;
    cfg.targets.deadlockHits = 2;
    cfg.suggester.pheromoneRepelWeight = 12.0;
    cfg.runSink = [&antLabels](const PersistedRun &pr) { antLabels.push_back(stimulatedLabels(pr)); };
    Session session(net, cfg);
    FakeSut sut(net);
    const CampaignReport with = session.runCampaign(sut);

    CHECK(with.runs.size() == 2);
    CHECK(with.complete);
    // Run 1 strands in the deadlock: the SUT stops answering, the run is lost
    // (no dump: a hang is not a crash) -- and the colony is told.
    CHECK(with.runs[0].outcome == RunOutcome::Completed && with.runs[0].dumps == 0);
    CHECK(with.runs[0].stimuli == 1 && with.runs[0].newEdges == 1);
    CHECK(with.runs[0].stuck);
    CHECK(!with.runs[0].notes.empty()
          && contains(with.runs[0].notes.back(), "way into the deadlock"));
    // The research window covers both corridors (six new edges) and only then
    // returns to the deadlock, whose second visit satisfies the model's
    // target: the campaign ends there, two runs instead of the colony-less
    // three.
    CHECK(with.runs[1].outcome == RunOutcome::Completed && with.runs[1].dumps == 0);
    CHECK(with.runs[1].stimuli == 7 && with.runs[1].newEdges == 6);
    CHECK(with.runs[1].coverageDone);
    CHECK(antLabels.size() == 2 && antLabels[1].size() == 7);
    CHECK(antLabels[1].front() == "a1" || antLabels[1].front() == "b1");
    CHECK(antLabels[1].back() == "d1");

    // --- without the colony --------------------------------------------------------------------
    std::vector<std::vector<std::string>> plainLabels;
    SessionConfig noAnts = quick();
    noAnts.maxRuns = 6;
    noAnts.useAnts = false;
    noAnts.targets.deadlock = TargetPolicy::UntilCovered;
    noAnts.targets.deadlockHits = 2;
    noAnts.runSink = [&plainLabels](const PersistedRun &pr) { plainLabels.push_back(stimulatedLabels(pr)); };
    Session plain(net, noAnts);
    FakeSut plainSut(net);
    const CampaignReport without = plain.runCampaign(plainSut);

    CHECK(without.runs.size() == 3);
    CHECK(without.runs[0].outcome == RunOutcome::Completed && without.runs[0].dumps == 0);
    CHECK(without.runs[0].stuck);
    // Run 2 walks straight back into the deadlock: one stimulus, no new
    // coverage, a second hang of the same kind before anything else was
    // tested. The tie between the covered deadlock path (still sought) and
    // the fresh corridor edges goes to the lower transition index.
    CHECK(without.runs[1].outcome == RunOutcome::Completed && without.runs[1].dumps == 0);
    CHECK(without.runs[1].stimuli == 1 && without.runs[1].newEdges == 0);
    CHECK(without.runs[1].stuck);
    CHECK(plainLabels.size() == 3 && plainLabels[1].size() == 1 && plainLabels[1][0] == "d1");
    CHECK(without.runs[2].coverageDone);
    CHECK(without.runs[2].stimuli == 6 && without.runs[2].newEdges == 6);
}

// GLM-5.3-Flash: an error state deserves worse pheromones the more time it
// takes to reach it. A chain of `inputs` controllable steps ends in a "boom"
// error marking that crashes the SUT for a post-mortem: the run pays one
// stimulus per step, then the catastrophe. The colony's deposit grows with
// the stimuli spent on the way in, so the crash of a deep chain leaves a much
// harder mark on its entrance edge than the crash of a short one.
// GLM-5.3-Flash: the step with index `unpredictableStep` is tagged temporally
// unpredictable (a human operator drives it); size_t(-1) tags none.
static Net makeWayInNet(std::size_t inputs, std::size_t unpredictableStep = std::size_t(-1))
{
    NetBuilder b("way-in");
    std::vector<PlaceIndex> places;
    places.push_back(b.place("s0"));
    for (std::size_t i = 1; i < inputs; ++i)
        places.push_back(b.place("p" + std::to_string(i)));
    const auto crash = b.place("crash");
    std::vector<TransitionIndex> step;
    for (std::size_t i = 0; i < inputs; ++i) {
        step.push_back(b.transition("i" + std::to_string(i + 1), TransitionKind::Controllable,
                                    "i" + std::to_string(i + 1)));
        if (i == unpredictableStep)
            b.unpredictable(step.back());  // GLM-5.3-Flash: the human operator's step
    }
    for (std::size_t i = 0; i < inputs; ++i)
        b.input(places[i], step[i]).output(step[i], i + 1 < inputs ? places[i + 1] : crash);

    b.initial("start", {{places[0], 1}});
    ErrorOptions boom;
    boom.crashDump = DumpMode::Crash;  // a real crash: the SUT is gone for a post-mortem
    b.errorMarking("boom", {{crash, 1}}, MatchMode::Exact, boom);
    return b.build();
}

// GLM-5.3-Flash: one crashing run over the chain; the strongest repellent
// pheromone it leaves behind sits on the edge into the error state, the
// deposit's depth 0. `timePricing` = false stands for --flat-catastrophe.
static double strongestRepelAfterCrash(std::size_t inputs, bool timePricing = true,
                                       std::size_t unpredictableStep = std::size_t(-1))
{
    const Net net = makeWayInNet(inputs, unpredictableStep);
    SessionConfig cfg = quick();
    cfg.catastropheTimePricing = timePricing;
    Session session(net, cfg);
    FakeSut sut(net);
    const RunReport rep = session.runOnce(sut);
    CHECK(rep.outcome == RunOutcome::SutCrashed && rep.stimuli == inputs);
    CHECK(!rep.notes.empty() && contains(rep.notes.back(), "crash site"));
    double best = 0.0;
    for (EdgeId e = 0; e < session.graph().numEdges(); ++e)
        best = std::max(best, session.ants().repel(e));
    return best;
}

static void testTheLongerTheWayInTheWorseTheCatastrophePheromone()
{
    const double oneStep = strongestRepelAfterCrash(1);
    const double fourSteps = strongestRepelAfterCrash(4);
    const double deep = strongestRepelAfterCrash(10);
    const double beyondTheCap = strongestRepelAfterCrash(12);
    CHECK(near(oneStep, 3.0 * 1.25));   // one input in: the base deposit plus a quarter
    CHECK(near(fourSteps, 3.0 * 2.0));  // four inputs in: twice the base deposit
    CHECK(near(deep, 3.0 * 3.0));       // ten inputs in: the cap counted eight of them
    CHECK(near(beyondTheCap, 3.0 * 3.0));  // and however much longer changes nothing
    CHECK(oneStep < fourSteps && fourSteps < deep);
}

// GLM-5.3-Flash: the wall-clock pricing is a feature that can be switched off
// (--flat-catastrophe / SessionConfig::catastropheTimePricing = false): the
// deposit is then the plain base level, however deep the crash was.
static void testTheCatastrophePricingCanBeSwitchedOff()
{
    CHECK(near(strongestRepelAfterCrash(10), 3.0 * 3.0));        // priced: the deep way in
    CHECK(near(strongestRepelAfterCrash(10, false), 3.0 * 1.0));  // flat: the switch is off
    CHECK(near(strongestRepelAfterCrash(1, false), 3.0 * 1.0));   // ... whatever the depth
}

// GLM-5.3-Flash: a way in that crossed a temporally unpredictable transition
// (a human operator's step) keeps the deposit flat as well -- the wall clock
// then measured the human, not the model's depth. The feature itself stays on.
static void testAnUnpredictableStepKeepsTheDepositFlat()
{
    CHECK(near(strongestRepelAfterCrash(10, true, 0), 3.0 * 1.0));  // the first step is theirs
    CHECK(near(strongestRepelAfterCrash(10, true, 9), 3.0 * 1.0));  // the last one, too
    CHECK(near(strongestRepelAfterCrash(10, true, 10), 3.0 * 3.0));  // out of range: no tag, priced again
}

// ---------------------------------------------------------------------------
// GLM-5.3-Flash: the two kinds of target/error states the model can declare.
// ---------------------------------------------------------------------------

// s0 -go-> s1 -back-> s0, and s1 carries the target marking "reached". With the
// "intentional" tag it is a good target state: a normal target the tester
// seeks, whose certain reach ends the campaign. Without it the marking is
// just an idle state (for the livelock analysis) and testing continues.
static Net makeGoodTargetNet(bool intentional)
{
    NetBuilder b("good-target");
    const auto s0 = b.place("s0");
    const auto s1 = b.place("s1");
    const auto go = b.transition("go", TransitionKind::Controllable, "go");
    const auto back = b.transition("back", TransitionKind::Controllable, "back");
    b.input(s0, go).output(go, s1);
    b.input(s1, back).output(back, s0);
    b.initial("start", {{s0, 1}});
    b.targetMarking("reached", {{s1, 1}}, MatchMode::Exact, intentional);
    return b.build();
}

static void testAGoodTargetMarkingEndsTheCampaign()
{
    {   // intentional: the run and the campaign end the moment the SUT is certainly there
        const Net net = makeGoodTargetNet(true);
        Session session(net, quick());
        FakeSut sut(net);
        const CampaignReport rep = session.runCampaign(sut);
        CHECK(rep.complete);
        CHECK(rep.stopReason == "the good target marking 'reached' was reached");
        CHECK(rep.runs.size() == 1);
        CHECK(rep.runs[0].outcome == RunOutcome::Completed);
        CHECK(rep.runs[0].goodTarget == "reached");
        CHECK(contains(rep.runs[0].detail, "good target marking 'reached'"));
        CHECK(rep.runs[0].stimuli == 1);  // the tester steered into the goal
        CHECK(session.coverage().targetMarkingHits(0) == 1);
    }
    {   // without the tag: the same marking never ends a run
        const Net net = makeGoodTargetNet(false);
        Session session(net, quick());
        FakeSut sut(net);
        const CampaignReport rep = session.runCampaign(sut);
        CHECK(rep.complete);
        CHECK(rep.runs.size() == 1);
        CHECK(rep.runs[0].goodTarget.empty());
        CHECK(rep.runs[0].coverageDone);
        CHECK(contains(rep.runs[0].detail, "coverage complete"));
        CHECK(session.coverage().targetMarkingHits(0) == 1);  // it was reached all the same
    }
}

// s0 -c1-> boom -back-> s0 with the error tag on the TRANSITION c1 (no error
// marking anywhere). A certain step of c1 is caught like an error marking: it
// is recorded in errors_reached, its own crash_dump/max_dumps are honoured,
// and a Crash ends the run. The target marking on s0 keeps the static analysis
// from calling the s0-boom cycle a livelock.
static Net makeErrorTransitionNet(DumpMode dump, std::uint32_t maxDumps)
{
    NetBuilder b("error-transition");
    const auto s0 = b.place("s0");
    const auto boom = b.place("boom");
    const auto c1 = b.transition("c1", TransitionKind::Controllable, "c1");
    const auto back = b.transition("back", TransitionKind::Controllable, "back");
    b.input(s0, c1).output(c1, boom);
    b.input(boom, back).output(back, s0);
    b.initial("start", {{s0, 1}});
    b.targetMarking("idle", {{s0, 1}});
    b.error(c1, dump, maxDumps);
    return b.build();
}

static void testAnErrorTransitionIsCaughtLikeAnErrorMarking()
{
    const Net net = makeErrorTransitionNet(DumpMode::Crash, 1);
    const auto c1 = *net.findTransition("c1");
    Session session(net, quick());
    FakeSut sut(net);

    // Run 1: the certain step of c1 crashes the SUT for a post-mortem.
    const RunReport r1 = session.runOnce(sut);
    CHECK(r1.outcome == RunOutcome::SutCrashed);
    CHECK(contains(r1.detail, "error transition 'c1'"));
    CHECK(r1.errorsReached.size() == 1 && r1.errorsReached[0] == "c1");
    CHECK(r1.dumps == 1);
    CHECK(!r1.notes.empty() && contains(r1.notes.back(), "crash site"));
    CHECK(sut.crashed());
    CHECK(session.coverage().errorTransitionDumps(c1) == 1);
    const auto &dump = session.coverage().run(r1.id).dumps[0];
    CHECK(dump.transition == c1 && dump.marking.empty());

    // Run 2: the budget is used up, the run continues past the same step.
    const RunReport r2 = session.runOnce(sut);
    CHECK(r2.outcome == RunOutcome::Completed);
    CHECK(r2.errorsReached.size() == 1 && r2.errorsReached[0] == "c1");
    CHECK(r2.dumps == 0);
    CHECK(session.coverage().errorTransitionDumps(c1) == 1);
}

static void testAnErrorTransitionWithoutADumpLetsTheRunContinue()
{
    const Net net = makeErrorTransitionNet(DumpMode::None, 1);
    const auto c1 = *net.findTransition("c1");
    Session session(net, quick());
    FakeSut sut(net);
    const RunReport rep = session.runOnce(sut);
    CHECK(rep.outcome == RunOutcome::Completed);
    CHECK(rep.errorsReached.size() == 1 && rep.errorsReached[0] == "c1");
    CHECK(rep.dumps == 0);
    CHECK(!sut.crashed());
    // the step was covered like an ordinary transition
    CHECK(session.coverage().transitionHits(c1) == 1);
}

static void testADeviationEndsTheRunWithAnOptionalSnapshot()
{
    const Net net = testmodels::makeElevatorNet();
    FakeSutOptions opt;
    opt.weight = {{"jam", 0}, {"obstruct", 0}, {"clear", 0}};
    opt.deviateAfterEvents = 2;
    opt.deviationLabel = "door.exploded";
    FakeSut sut(net, opt);

    std::vector<PersistedRun> saved;
    SessionConfig cfg = quick();
    cfg.dumps.onDeviation = DumpMode::Snapshot;
    cfg.runSink = [&saved](const PersistedRun &r) { saved.push_back(r); };
    Session session(net, cfg);

    const RunReport r = session.runOnce(sut);
    CHECK(r.outcome == RunOutcome::Deviation);
    CHECK(contains(r.detail, "does not know"));
    CHECK(r.dumps == 1);
    CHECK(saved.size() == 1 && saved[0].dumps.size() == 1);
    CHECK(saved[0].dumps[0].marking.empty());  // a deviation is not an error marking
    CHECK(sut.stopCalls() == 1);
}

static void testARunTimeLivelockEndsTheRun()
{
    const Net net = makePingPongNet();
    FakeSutOptions opt;
    opt.clockStepMs = 100;  // deterministic capture times: 100, 200, ...
    opt.stepDelayMs = 0;
    FakeSut sut(net, opt);

    SessionConfig cfg = quick();
    cfg.tracker.livelockTimeoutMs = 1000;
    cfg.dumps.onLivelock = DumpMode::Snapshot;
    cfg.maxRuns = 1;
    Session session(net, cfg);

    const RunReport r = session.runOnce(sut);
    CHECK(r.outcome == RunOutcome::Livelock);
    CHECK(contains(r.detail, "without progress"));
    CHECK(r.dumps == 1);
    CHECK(r.events == 11);  // the 11th event, at 1100 ms, is the first 1000 ms after the last progress
}

static void testAnEventQueueOverflowAbortsTheRun()
{
    const Net net = makePingPongNet();
    FakeSutOptions opt;
    opt.burstAtStart = 50;  // pushed inside startRun, before the Session reads anything
    FakeSut sut(net, opt);

    SessionConfig cfg = quick();
    cfg.eventQueueCapacity = 4;
    Session session(net, cfg);

    const RunReport r = session.runOnce(sut);
    CHECK(r.outcome == RunOutcome::Aborted && contains(r.detail, "overflow"));
    CHECK(sut.stopCalls() == 1);
}

static void testAPluginThatCannotStartAbortsTheCampaign()
{
    const Net net = makeThreeBranchNet();
    FakeSutOptions opt;
    opt.failStart = true;
    FakeSut sut(net, opt);
    Session session(net, quick());
    const CampaignReport rep = session.runCampaign(sut);
    CHECK(rep.runs.size() == 3);  // maxConsecutiveAborts
    CHECK(rep.runs[0].outcome == RunOutcome::Aborted && contains(rep.runs[0].detail, "could not start"));
    CHECK(contains(rep.stopReason, "aborted"));
    CHECK(!rep.complete);
    CHECK(sut.stopCalls() == 0);  // a SUT that never started is not stopped
}

static void testASilentSutIsNoticed()
{
    {   // it owes an output and never sends one, nor a Quiescent report
        const Net net = makePingPongNet();
        FakeSutOptions opt;
        opt.silent = true;
        FakeSut sut(net, opt);
        SessionConfig cfg = quick();
        cfg.idleTimeout = std::chrono::milliseconds(100);
        Session session(net, cfg);
        const RunReport r = session.runOnce(sut);
        CHECK(r.outcome == RunOutcome::Aborted && contains(r.detail, "no event from the SUT"));
    }
    {   // it swallows the input it was sent
        const Net net = makeThreeBranchNet();
        FakeSutOptions opt;
        opt.silent = true;
        FakeSut sut(net, opt);
        SessionConfig cfg = quick();
        cfg.stimulusAckTimeout = std::chrono::milliseconds(100);
        Session session(net, cfg);
        const RunReport r = session.runOnce(sut);
        CHECK(r.outcome == RunOutcome::Aborted && contains(r.detail, "never reported"));
        CHECK(r.stimuli == 1);
    }
}

static void testAPluginFaultAbortsTheRun()
{
    const Net net = makeThreeBranchNet();
    FakeSut sut(net);
    sut.setFault("lost the connection to the SUT");
    Session session(net, quick());
    const RunReport r = session.runOnce(sut);
    CHECK(r.outcome == RunOutcome::Aborted && contains(r.detail, "lost the connection"));
}

static void testStopRequestEndsTheCampaign()
{
    const Net net = makePingPongNet();
    FakeSut sut(net);  // cycles forever, and nothing detects a livelock here
    SessionConfig cfg = quick();
    cfg.maxEventsPerRun = 100000000;
    cfg.maxRuns = 50;
    Session session(net, cfg);

    std::thread stopper([&session] {
        std::this_thread::sleep_for(std::chrono::milliseconds(80));
        session.requestStop();
    });
    const CampaignReport rep = session.runCampaign(sut);
    stopper.join();
    CHECK(rep.stopReason == "stopped on request");
    CHECK(!rep.runs.empty() && rep.runs.back().outcome == RunOutcome::Aborted);
    CHECK(contains(rep.runs.back().detail, "stopped on request"));
}

static void testAModelWithoutAnIdleStateIsFlagged()
{
    const Net net = makeThreeBranchNet(false);  // a cycle, and no target marking anywhere
    FakeSut sut(net);
    Session session(net, quick());
    const CampaignReport rep = session.runCampaign(sut);
    CHECK(rep.warnings.size() >= 2);
    bool mentionsInitial = false;
    for (const std::string &w : rep.warnings)
        mentionsInitial = mentionsInitial || contains(w, "initial marking lies inside");
    CHECK(mentionsInitial);
    // And nothing can be done: every move leads into the (satisfied) livelock.
    CHECK(rep.runs.size() == 1 && rep.runs[0].events == 0);
    CHECK(contains(rep.runs[0].detail, "satisfied targets"));
}

static void testStaticLivelocksBecomeTargets()
{
    // s0 -trap-> q <-> r (a cycle that cannot be left), s0 -safe-> s1 -back-> s0.
    NetBuilder b;
    const auto s0 = b.place("s0");
    const auto s1 = b.place("s1");
    const auto q = b.place("q");
    const auto r = b.place("r");
    const auto trap = b.transition("trap", TransitionKind::Controllable, "trap");
    const auto u = b.transition("u", TransitionKind::Observable, "u");
    const auto v = b.transition("v", TransitionKind::Observable, "v");
    const auto safe = b.transition("safe", TransitionKind::Controllable, "safe");
    const auto back = b.transition("back", TransitionKind::Observable, "back");
    b.input(s0, trap).output(trap, q);
    b.input(q, u).output(u, r);
    b.input(r, v).output(v, q);
    b.input(s0, safe).output(safe, s1);
    b.input(s1, back).output(back, s0);
    b.initial("start", {{s0, 1}});
    const Net net = b.build();

    Session session(net, quick());
    const LivelockReport &rep = session.analyzeModel();
    CHECK(rep.findings.size() == 1 && rep.findings[0].certain);
    CHECK(rep.findings[0].states.size() == 2);
}

// GLM-5.3-Flash: switching the ant colony off (--no-ants, SessionConfig::useAnts).
// With the switch off the colony must not walk the model at all, leave no
// pheromone behind, and the campaign must still cover the model with the
// suggester alone.
static void testTheAntColonyCanBeSwitchedOff()
{
    const Net net = makeThreeBranchNet();

    {   // By default the colony takes part and walks the model.
        Session session(net, quick());
        CHECK(session.antsEnabled());
        FakeSut sut(net);
        const CampaignReport rep = session.runCampaign(sut);
        CHECK(rep.complete && rep.stopReason == "coverage complete");
        CHECK(session.ants().stats().iterations > 0);
        CHECK(session.ants().stats().ants > 0);
    }
    {   // Switched off: no walk, no pheromone, full coverage anyway.
        SessionConfig cfg = quick();
        cfg.useAnts = false;
        Session session(net, cfg);
        CHECK(!session.antsEnabled());
        FakeSut sut(net);
        const CampaignReport rep = session.runCampaign(sut);
        CHECK(rep.complete && rep.stopReason == "coverage complete");
        CHECK(rep.summary.transitionsCovered == 6);
        CHECK(session.ants().stats().iterations == 0);
        CHECK(session.ants().stats().ants == 0);
        const EdgeId edge = session.graph().firstEdge(0);
        CHECK(edge != kNoEdge);
        CHECK(session.ants().attract(edge) == 0.0);
        CHECK(session.ants().repel(edge) == 0.0);
    }
}

// ===========================================================================
// GLM-5.3-Flash: what the tester does when the state limit cuts the reachability
// graph off. The graph is lazy, so a limit stops the CREATION of new states:
// edges into markings that are not interned yet are dropped, the source state
// is marked incomplete and the graph truncated. These tests pin the promised
// behaviour: the tester keeps expanding what exists (and grows the graph on
// demand during runs), never mistakes a cut-off frontier for a deadlock or a
// livelock, says in the warnings that the exploration was incomplete, and,
// when the SUT walks past the region the graph can represent, flags a
// deviation but says honestly that the explored model may be incomplete there.
// ===========================================================================

// A counter the tester can grow without end: "add" is an input that puts one
// more token into "tokens" (weight 1 in, weight 2 out). The reachability graph
// is a chain 1, 2, 3, ... bounded only by the state limit, so a small limit
// truncates it mid-chain.
static Net makeCounterNet()
{
    NetBuilder b("counter");
    const auto tokens = b.place("tokens", 20);
    const auto add = b.transition("add", TransitionKind::Controllable, "add");
    b.input(tokens, add).output(add, tokens, 2);
    b.initial("start", {{tokens, 1}});
    return b.build();
}

// The counter again, plus an output "tick" the SUT fires on its own (also
// growing the counter), so the SUT can walk past the truncated region without
// any help from the tester. "noop" is a harmless input so the tester has
// something to send.
static Net makeRunawayNet()
{
    NetBuilder b("runaway");
    const auto tokens = b.place("tokens", 20);
    const auto tick = b.transition("tick", TransitionKind::Observable, "tick");
    b.input(tokens, tick).output(tick, tokens, 2);
    const auto noop = b.transition("noop", TransitionKind::Controllable, "noop");
    b.input(tokens, noop).output(noop, tokens);
    b.initial("start", {{tokens, 1}});
    return b.build();
}

// A scripted plug-in: on the first suggested input it acknowledges it and then
// dumps a burst of its autonomous "tick" outputs, the way a fast real SUT
// fills the queue between two polls. The burst is longer than the truncated
// graph is deep, so the SUT ends up where the tracker has no states.
class RunawaySut final : public ISutPlugin
{
public:
    PluginCapabilities capabilities() const override { return {}; }
    bool startRun(const std::string &initialMarking, const PluginRunOptions &options, EventQueue &events,
                  std::string *error) override
    {
        (void)initialMarking;
        (void)options;
        (void)error;
        events_ = &events;
        burstSent_ = false;
        return true;
    }
    bool stimulate(const std::string &label, std::string *error) override
    {
        (void)error;
        push(EventKind::Stimulated, label);
        if (!burstSent_) {  // the machine runs away on its own after the first input
            burstSent_ = true;
            for (int i = 0; i < 5; ++i)
                push(EventKind::Observed, "tick");
        }
        return true;
    }
    PostMortem postMortem(DumpMode mode) override
    {
        (void)mode;
        return {};
    }
    void stopRun(StopReason reason) override { (void)reason; }
    std::string pollFault() override { return {}; }

private:
    void push(EventKind kind, const std::string &label)
    {
        SutEvent ev;
        ev.kind = kind;
        ev.label = label;
        ev.timestampMs = stamp_;
        stamp_ += 10;
        events_->push(ev);
    }
    EventQueue *events_ = nullptr;
    bool burstSent_ = false;
    std::int64_t stamp_ = 0;
};

// The explored model is truncated up front (analyzeModel explores everything
// the limit allows), the warning says so, the static analysis reports nothing
// false, and a whole campaign over the truncated graph still completes and
// never calls the frontier a deadlock.
static void testAStateLimitTruncatesTheGraphWithoutFalseFindings()
{
    const Net net = makeCounterNet();
    SessionConfig cfg = quick();
    cfg.graph.maxStates = 4;  // the chain would go on to the bound of 20
    Session session(net, cfg);
    session.analyzeModel();

    CHECK(session.graph().numStates() == 4);
    CHECK(session.graph().numExpanded() == 4);  // everything that exists was expanded
    CHECK(session.graph().truncated());
    CHECK(session.graph().isIncomplete(3));      // its edge to tokens=5 was dropped
    CHECK(!session.graph().isIncomplete(0));
    CHECK(session.graph().numEdges() == 3);

    bool warned = false;
    for (const std::string &w : session.warnings())
        warned = warned || contains(w, "not explored completely");
    CHECK(warned);
    CHECK(session.staticReport().findings.empty());  // no false livelock

    FakeSut sut(net);
    const CampaignReport rep = session.runCampaign(sut);
    CHECK(rep.complete && rep.stopReason == "coverage complete");
    for (const RunReport &r : rep.runs)
        CHECK(r.outcome == RunOutcome::Completed);
    CHECK(rep.summary.transitionsCovered == 1);
    CHECK(rep.summary.statesCovered == 4);
    CHECK(rep.summary.edgesCovered == 3);
}

// With the up-front exploration switched off, nothing is known before the
// first run: the runs themselves create and expand the graph on demand until
// the limit stops them. That is the answer to "does the tester expand
// unresolved RG nodes": yes, it grows the graph while it tests.
static void testTheGraphGrowsLazilyDuringTheCampaign()
{
    const Net net = makeCounterNet();
    SessionConfig cfg = quick();
    cfg.graph.maxStates = 4;
    cfg.analyzeModel = false;  // nothing is explored up front
    cfg.useAnts = false;       // keep the expansion tracker/suggester-driven
    Session session(net, cfg);
    CHECK(session.graph().numStates() == 0);
    CHECK(session.graph().numExpanded() == 0);
    CHECK(!session.graph().truncated());

    FakeSut sut(net);
    const CampaignReport rep = session.runCampaign(sut);
    CHECK(session.graph().numStates() == 4);
    CHECK(session.graph().numExpanded() == 4);
    CHECK(session.graph().truncated());
    CHECK(session.graph().isIncomplete(3));
    CHECK(rep.complete && rep.stopReason == "coverage complete");
    CHECK(rep.warnings.empty());  // no up-front analysis, no static warning
}

// The SUT walks past the truncated region (an autonomous output grows the
// counter beyond every state the capped graph has). The tracker cannot follow:
// the run ends in a deviation, but the message says the explored model may be
// incomplete there, instead of pretending the SUT is simply wrong. A stimulus
// whose edge was dropped is Preempted (the run goes on), and the cut-off
// frontier is never reported as a deadlock.
static void testAnSutBeyondTheTruncatedModelIsFlaggedAsPossiblyIncomplete()
{
    const Net net = makeRunawayNet();
    SessionConfig cfg = quick();
    cfg.graph.maxStates = 4;
    cfg.maxRuns = 1;
    cfg.useAnts = false;
    Session session(net, cfg);
    RunawaySut sut;
    const CampaignReport rep = session.runCampaign(sut);

    CHECK(session.graph().numStates() == 4);
    CHECK(session.graph().truncated());
    CHECK(session.graph().isIncomplete(3));
    CHECK(rep.runs.size() == 1);
    CHECK(rep.runs[0].outcome == RunOutcome::Deviation);
    CHECK(contains(rep.runs[0].detail, "not possible in any possible state"));
    CHECK(contains(rep.runs[0].detail, "[the explored model may be incomplete here]"));
}

// GLM-5.3-Flash: a named internal step is visible only when the model traces
// it. The scripted SUT reports its internal step as the Observed event
// "work", the way a real SUT whose logs name that step would.
class ReportingSut final : public ISutPlugin
{
public:
    PluginCapabilities capabilities() const override { return {}; }
    bool startRun(const std::string &initialMarking, const PluginRunOptions &options, EventQueue &events,
                  std::string *error) override
    {
        (void)initialMarking;
        (void)options;
        (void)error;
        events_ = &events;
        reported_ = false;
        return true;
    }
    bool stimulate(const std::string &label, std::string *error) override
    {
        (void)error;
        push(EventKind::Stimulated, label);
        if (!reported_) {
            reported_ = true;
            push(EventKind::Observed, "work");
            push(EventKind::Observed, "oa");
        }
        return true;
    }
    PostMortem postMortem(DumpMode mode) override
    {
        (void)mode;
        return {};
    }
    void stopRun(StopReason reason) override { (void)reason; }
    std::string pollFault() override { return {}; }

private:
    void push(EventKind kind, const std::string &label)
    {
        SutEvent ev;
        ev.kind = kind;
        ev.label = label;
        ev.timestampMs = stamp_;
        stamp_ += 10;
        events_->push(ev);
    }
    EventQueue *events_ = nullptr;
    bool reported_ = false;
    std::int64_t stamp_ = 0;
};

// s0 -a-> s1 -oa-> s0, plus a named internal self-loop "work" on both states.
// With traced(false) the step is named but silent; with traced(true) (the
// default for a labeled internal) the SUT may report it.
static Net makeInternalStepNet(bool traced)
{
    NetBuilder b("internal-step");
    const auto s0 = b.place("s0");
    const auto s1 = b.place("s1");
    const auto a = b.transition("a", TransitionKind::Controllable, "a");
    b.input(s0, a).output(a, s1);
    const auto oa = b.transition("oa", TransitionKind::Observable, "oa");
    b.input(s1, oa).output(oa, s0);
    const auto work = b.transition("work", TransitionKind::Internal, "work");
    b.input(s1, work).output(work, s1);  // the SUT reports it right after the input, in s1
    b.traced(work, traced);  // true is the derived default; false names a silent step
    b.initial("start", {{s0, 1}});
    b.targetMarking("idle", {{s0, 1}});
    return b.build();
}

static void testANamedInternalStepIsReportedOnlyWhenTraced()
{
    // The model keeps the step silent: the reported "work" is unknown to the
    // model and the run ends in a deviation.
    {
        const Net net = makeInternalStepNet(false);
        SessionConfig cfg = quick();
        cfg.useAnts = false;
        cfg.maxRuns = 1;
        Session session(net, cfg);
        ReportingSut sut;
        const CampaignReport rep = session.runCampaign(sut);
        CHECK(rep.runs.size() == 1);
        CHECK(rep.runs[0].outcome == RunOutcome::Deviation);
        CHECK(contains(rep.runs[0].detail, "which the model does not know"));
    }

    // The model traces the step: the same report is matched, the step counts
    // as covered, and the campaign completes.
    {
        const Net net = makeInternalStepNet(true);
        SessionConfig cfg = quick();
        cfg.useAnts = false;
        Session session(net, cfg);
        ReportingSut sut;
        const CampaignReport rep = session.runCampaign(sut);
        CHECK(rep.complete);
        bool workReported = false;
        for (std::size_t i = 0; i < session.coverage().numRuns(); ++i)
            for (const SutEvent &e : session.coverage().run(i).events)
                workReported = workReported || (e.kind == EventKind::Observed && e.label == "work");
        CHECK(workReported);
        CHECK(rep.summary.transitionsCovered == 3);  // a, oa and the traced step
    }
}

// ===========================================================================
// GLM-5.3-Flash: internal transitions with NO counterpart in the SUT. An
// unlabeled internal step only furthers the specification from one external
// behaviour to the next: the SUT has no action for it, so the tester can
// neither stimulate it nor wait for a report of it. The tracker follows it
// as a silent step; when it is the only bridge between the external steps,
// every consistent path crosses it, so it is credited definitely.
// ===========================================================================

// s0 -a(input)-> s1 -advance(internal, unlabeled)-> s2 -oa(output)-> s0:
// "advance" is the only bridge between the input and the output.
static Net makeInternalBridgeNet()
{
    NetBuilder b("internal-bridge");
    const auto s0 = b.place("s0");
    const auto s1 = b.place("s1");
    const auto s2 = b.place("s2");
    const auto a = b.transition("a", TransitionKind::Controllable, "a");
    const auto advance = b.transition("advance", TransitionKind::Internal);  // no label: no SUT counterpart
    const auto oa = b.transition("oa", TransitionKind::Observable, "oa");
    b.input(s0, a).output(a, s1);
    b.input(s1, advance).output(advance, s2);
    b.input(s2, oa).output(oa, s0);
    b.initial("start", {{s0, 1}});
    b.targetMarking("idle", {{s0, 1}});
    return b.build();
}

static void testAnInternalStepWithoutACounterpartIsFollowedAndCovered()
{
    const Net net = makeInternalBridgeNet();
    const auto advance = *net.findTransition("advance");
    Session session(net, quick());
    FakeSut sut(net);
    const CampaignReport rep = session.runCampaign(sut);

    // One run suffices: the input, the silent advance, the output.
    CHECK(rep.complete && rep.runs.size() == 1);
    CHECK(rep.runs[0].outcome == RunOutcome::Completed && rep.runs[0].coverageDone);
    // The tester saw exactly the two external events, in order -- and nothing
    // for the internal step, which has no label and no counterpart to report.
    const auto &events = session.coverage().run(0).events;
    CHECK(events.size() == 2);
    CHECK(events[0].kind == EventKind::Stimulated && events[0].label == "a");
    CHECK(events[1].kind == EventKind::Observed && events[1].label == "oa");
    // The SUT was never asked for the internal step: it is not an input.
    const auto stim = sut.stimuliReceived();
    CHECK(stim.size() == 1 && stim[0] == "a");
    // Yet the specification advanced through it: the silent edge is definitely
    // covered, because the output can only come from the state it leads to.
    CHECK(session.coverage().transitionHits(advance) == 1);
    CHECK(rep.summary.transitionsCovered == 3);
    CHECK(rep.summary.statesCovered == 3 && rep.summary.edgesCovered == 3);
}

// The flip side of "the internal step furthers the specification": while it is
// enabled, the model says autonomous progress is due, so a SUT that goes
// silent there misses its output. The run ends in a deviation -- and the
// silent stretch stays ambiguous: the tester could not know whether the step
// happened, so it is not credited.
static void testAnInternalOnlyStallIsAMissingOutput()
{
    const Net net = makeInternalBridgeNet();
    const auto advance = *net.findTransition("advance");
    SimulatedSutOptions opt;
    opt.weight = {{"advance", 0}};  // this SUT never takes the internal step
    FakeSut sut(net, opt);
    Session session(net, quick());
    const RunReport rep = session.runOnce(sut);

    CHECK(rep.outcome == RunOutcome::Deviation);
    CHECK(contains(rep.detail, "no output arrived"));
    CHECK(contains(rep.detail, "expected: oa"));
    // The tester saw the stimulus, then silence -- nothing to report, and in
    // particular no output although one was due behind the internal step.
    const auto &events = session.coverage().run(rep.id).events;
    CHECK(events.size() == 2);
    CHECK(events[0].kind == EventKind::Stimulated && events[0].label == "a");
    CHECK(events[1].kind == EventKind::Quiescent);
    // The input was certain; the silent advance was not (it may or may not
    // have fired before the SUT stalled), so it is not credited.
    CHECK(session.coverage().transitionHits(*net.findTransition("a")) == 1);
    CHECK(session.coverage().transitionHits(advance) == 0);
}

// ===========================================================================
// GLM-5.3-Flash: the seed steers and reproduces campaigns. The three-branch
// net is symmetric, so every first suggestion is an exact tie between the
// inputs a, b and c; the seeded tie-break is the only thing that decides.
// With ants switched off the scores are clean ties, so the assertions below
// really test the seed and nothing else. (The SUT itself is deterministic
// here: the simulated SUT has its own seed, --sim-seed.)
// ===========================================================================
static std::string stimuliOf(const Session &session, const CampaignReport &rep)
{
    std::string out;
    for (const RunReport &r : rep.runs)
        for (const SutEvent &e : session.coverage().run(r.id).events)
            if (e.kind == EventKind::Stimulated) {
                if (!out.empty())
                    out += ' ';
                out += e.label;
            }
    return out;
}

static void testTheSeedSteersAndReproducesCampaigns()
{
    const Net net = makeThreeBranchNet();

    // Seed 0 keeps the deterministic order: the first stimulus is a, the
    // lowest transition index among the tied inputs.
    {
        SessionConfig cfg = quick();
        cfg.useAnts = false;
        cfg.selectionSeed = 0;
        Session session(net, cfg);
        FakeSut sut(net);
        const CampaignReport rep = session.runCampaign(sut);
        CHECK(rep.complete);
        CHECK(stimuliOf(session, rep).find('a') == 0);  // begins with "a ..."
    }

    // The same seed reproduces the same campaign, byte for byte in spirit:
    // identical stimulus sequences over all runs.
    std::string reference;
    for (int repeat = 0; repeat < 2; ++repeat) {
        SessionConfig cfg = quick();
        cfg.useAnts = false;
        cfg.selectionSeed = 7;
        Session session(net, cfg);
        FakeSut sut(net);
        const CampaignReport rep = session.runCampaign(sut);
        CHECK(rep.complete);
        const std::string stimuli = stimuliOf(session, rep);
        if (repeat == 0)
            reference = stimuli;
        else
            CHECK(stimuli == reference);
    }
    CHECK(!reference.empty());

    // Different seeds steer different campaigns: over seeds 1..6 at least two
    // distinct first stimuli occur.
    std::set<char> firsts;
    for (std::uint64_t seed = 1; seed <= 6; ++seed) {
        SessionConfig cfg = quick();
        cfg.useAnts = false;
        cfg.selectionSeed = seed;
        Session session(net, cfg);
        FakeSut sut(net);
        const CampaignReport rep = session.runCampaign(sut);
        CHECK(rep.complete);
        const std::string stimuli = stimuliOf(session, rep);
        CHECK(!stimuli.empty());
        firsts.insert(stimuli.front());
    }
    CHECK(firsts.size() >= 2);
}

// GLM-5.3-Flash: the time criterion. Every run measures its wall-clock
// duration and the campaign its total, so different seeds or configurations
// can be compared by how much testing a second buys.
static void testTheCampaignMeasuresItsTime()
{
    const Net net = makeThreeBranchNet();
    SessionConfig cfg = quick();
    cfg.useAnts = false;
    Session session(net, cfg);
    FakeSut sut(net);
    const CampaignReport rep = session.runCampaign(sut);
    CHECK(rep.complete);
    CHECK(!rep.runs.empty());
    CHECK(rep.wallMs > 0);
    std::int64_t longest = 0;
    std::int64_t total = 0;
    for (const RunReport &r : rep.runs) {
        CHECK(r.durationMs > 0);
        longest = std::max(longest, r.durationMs);
        total += r.durationMs;
    }
    CHECK(rep.wallMs >= longest);   // the campaign spans at least its longest run
    CHECK(rep.wallMs >= total);     // and everything else in between
}

// GLM-5.3-Flash: the milestone label. A certainly stepped milestone
// transition is a success: the run continues and the colony is told (positive
// pheromones, almost as much as a target marking gets). The milestone here is
// deliberately NOT the good target: serving leads to a state that still wants
// the "reset" input, so the run demonstrably goes on past the milestone and
// only ends at the intentional target marking.
static Net makeMilestoneNet()
{
    NetBuilder b("milestones");
    const auto s0 = b.place("s0");
    const auto mid = b.place("mid");
    const auto served = b.place("served");
    const auto go = b.transition("go", TransitionKind::Controllable, "go");
    const auto serve = b.transition("serve", TransitionKind::Observable, "serve");
    const auto reset = b.transition("reset", TransitionKind::Controllable, "reset");
    b.input(s0, go).output(go, mid);
    b.input(mid, serve).output(serve, served);
    b.input(served, reset).output(reset, s0);
    b.initial("start", {{s0, 1}});
    b.targetMarking("idle", {{s0, 1}});
    b.milestone(serve, 2);  // rank 2: a deposit of 2 * 1.5 = 3.0
    return b.build();
}

static void testAMilestoneStepIsRewarded()
{
    const Net net = makeMilestoneNet();

    // A high coverage threshold keeps the milestone edge unsaturated, so its
    // attract pheromone is not wiped again by the campaign's end.
    SessionConfig cfg = quick();
    cfg.coverage.threshold = 1000;
    cfg.maxRuns = 1;
    cfg.maxEventsPerRun = 20;
    Session session(net, cfg);
    FakeSut sut(net);
    const CampaignReport rep = session.runCampaign(sut);
    CHECK(rep.runs.size() == 1);
    CHECK(rep.complete);
    // The milestone was certainly stepped, is reported, and did NOT end the
    // run: the run went on to send "reset", and only ended when the coverage
    // was complete. (The target marking here is not intentional -- a plain
    // target marking never ends a run; the milestone must not either.)
    CHECK(rep.runs[0].milestonesReached.size() == 1 && rep.runs[0].milestonesReached[0] == "serve");
    CHECK(rep.runs[0].outcome == RunOutcome::Completed);
    CHECK(contains(rep.runs[0].detail, "coverage complete"));
    {
        bool servedSeen = false;
        bool resetAfterServe = false;
        for (const SutEvent &e : session.coverage().run(rep.runs[0].id).events) {
            if (e.kind == EventKind::Observed && e.label == "serve")
                servedSeen = true;
            if (servedSeen && e.kind == EventKind::Stimulated && e.label == "reset")
                resetAfterServe = true;
        }
        CHECK(servedSeen);
        CHECK(resetAfterServe);
    }
    const auto serve = *net.findTransition("serve");
    CHECK(session.coverage().transitionHits(serve) > 0);

    // The colony is told: the edge into the milestone's target carries
    // attractive pheromone (evaporated a little by later iterations).
    EdgeId serveEdge = kNoEdge;
    for (EdgeId e = 0; e < session.graph().numEdges(); ++e)
        if (session.graph().edge(e).transition == serve)
            serveEdge = e;
    CHECK(serveEdge != kNoEdge);
    CHECK(session.ants().attract(serveEdge) > 0.0);
}

// GLM-5.3-Flash: the rebuild (doku section 12, item 4). A restored session
// gets from the recording what the live run would have left behind: the
// milestones' attract and the catastrophes' repel, priced by the way in.
static Net makeDeadEndNet()
{
    NetBuilder b("dead-end");
    const auto s0 = b.place("s0");
    const auto dead = b.place("dead");
    const auto x = b.transition("x", TransitionKind::Controllable, "x");
    b.input(s0, x).output(x, dead);
    b.initial("start", {{s0, 1}});
    b.targetMarking("idle", {{s0, 1}});
    return b.build();
}

static double bestChannel(const Session &session, TransitionIndex t, bool repel)
{
    double best = 0.0;
    for (EdgeId e = 0; e < session.graph().numEdges(); ++e)
        if (session.graph().edge(e).transition == t)
            best = std::max(best, repel ? session.ants().repel(e) : session.ants().attract(e));
    return best;
}

static void testTheRebuildReplaysWhatTheRunTaught()
{
    // --- the milestone's attract is rebuilt, exactly --------------------------
    const Net net = makeMilestoneNet();
    PersistedRun milestoneRun;
    milestoneRun.initialMarking = "start";
    milestoneRun.transitions = {"go", "serve"};
    milestoneRun.outcome = RunOutcome::Completed;
    milestoneRun.detail = "coverage complete: everything is covered";
    {
        Session session(net, quick());
        const ReplayResult r = session.restore(milestoneRun);
        CHECK(r.ok);
        const auto serve = *net.findTransition("serve");
        CHECK(session.coverage().transitionHits(serve) == 1);
        // GLM-5.3-Flash: inherited knowledge enters toned down (doku section
        // 16). No evaporation in a rebuild, but the default carry-over factor
        // is 50 percent: the recorded step deposits half the rank-2 amount at
        // the recorded target state.
        CHECK(near(bestChannel(session, serve, /*repel=*/false), 1.5));
        // And nothing is marked as a catastrophe.
        const auto go = *net.findTransition("go");
        CHECK(bestChannel(session, go, true) == 0.0);
    }
    {   // GLM-5.3-Flash: 100 percent keeps the old full strength (doku section 16).
        SessionConfig full = quick();
        full.restoredPheromonePercent = 100;
        Session session(net, full);
        CHECK(session.restore(milestoneRun).ok);
        CHECK(near(bestChannel(session, *net.findTransition("serve"), false), 3.0));
    }
    {   // GLM-5.3-Flash: 0 percent ignores the inherited teaching entirely.
        SessionConfig none = quick();
        none.restoredPheromonePercent = 0;
        Session session(net, none);
        CHECK(session.restore(milestoneRun).ok);
        CHECK(bestChannel(session, *net.findTransition("serve"), false) == 0.0);
    }
    {   // GLM-5.3-Flash: several rounds merge as before, each toned down:
        // two restored runs of the same milestone add 1.5 + 1.5.
        Session session(net, quick());
        CHECK(session.restore(milestoneRun).ok);
        CHECK(session.restore(milestoneRun).ok);
        CHECK(session.coverage().transitionHits(*net.findTransition("serve")) == 2);
        CHECK(near(bestChannel(session, *net.findTransition("serve"), false), 3.0));
    }

    // --- a recorded crash marks the way in, priced by the way in --------------
    ErrorOptions boom;
    boom.crashDump = DumpMode::Crash;
    const Net crashNet = makeCrashRegionNet(boom);
    PersistedRun crashRun;
    crashRun.initialMarking = "start";
    crashRun.transitions = {"c1"};
    crashRun.outcome = RunOutcome::SutCrashed;
    crashRun.detail = "error marking 'boom': the SUT was crashed for a post-mortem";
    {
        Session session(crashNet, quick());
        CHECK(session.restore(crashRun).ok);
        const auto c1 = *crashNet.findTransition("c1");
        // One input on the way in: depositCatastrophe * (1 + 0.25 * 1),
        // halved by the default 50 percent carry-over (GLM-5.3-Flash).
        CHECK(near(bestChannel(session, c1, true), 3.75 / 2));
    }
    {
        SessionConfig flat = quick();
        flat.catastropheTimePricing = false;
        Session session(crashNet, flat);
        CHECK(session.restore(crashRun).ok);
        CHECK(near(bestChannel(session, *crashNet.findTransition("c1"), true), 1.5));  // GLM-5.3-Flash: 3.0 / 2
    }
    {
        // An unpredictable way in stays flat, exactly as live.
        Net uNet = crashNet;
        uNet.transitions[*uNet.findTransition("c1")].unpredictable = true;
        Session session(uNet, quick());
        CHECK(session.restore(crashRun).ok);
        CHECK(near(bestChannel(session, *crashNet.findTransition("c1"), true), 1.5));  // GLM-5.3-Flash: 3.0 / 2
    }
    {   // GLM-5.3-Flash: with the factor at 100 the old full amounts come back.
        SessionConfig full = quick();
        full.restoredPheromonePercent = 100;
        Session session(crashNet, full);
        CHECK(session.restore(crashRun).ok);
        CHECK(near(bestChannel(session, *crashNet.findTransition("c1"), true), 3.75));
    }

    // --- a run stranded in a recorded deadlock marks the way in ---------------
    const Net deadNet = makeDeadEndNet();
    PersistedRun stranded;
    stranded.initialMarking = "start";
    stranded.transitions = {"x"};
    stranded.outcome = RunOutcome::Completed;
    stranded.detail = "stuck: no reachable goal is left";
    {
        Session session(deadNet, quick());
        CHECK(session.restore(stranded).ok);
        CHECK(near(bestChannel(session, *deadNet.findTransition("x"), true), 3.75 / 2));  // GLM-5.3-Flash
    }
    {
        // A run that ended for another reason teaches nothing: "coverage
        // complete" in the same deadlock is the goal reached, not a hang.
        PersistedRun pr = stranded;
        pr.detail = "coverage complete: everything is covered";
        Session session(deadNet, quick());
        CHECK(session.restore(pr).ok);
        CHECK(bestChannel(session, *deadNet.findTransition("x"), true) == 0.0);
    }
    {
        // An event-run crash is priced by the acknowledged stimuli.
        PersistedRun pr;
        pr.initialMarking = "start";
        pr.events = {SutEvent{EventKind::Stimulated, "x", 5}};
        pr.outcome = RunOutcome::SutCrashed;
        pr.detail = "dump-on-deadlock: the SUT was crashed for a post-mortem";
        Session session(deadNet, quick());
        CHECK(session.restore(pr).ok);
        CHECK(near(bestChannel(session, *deadNet.findTransition("x"), true), 3.75 / 2));  // GLM-5.3-Flash
    }
}

// GLM-5.3-Flash: the pair layer in the session (doku section 16). One run
// whose first step is a milestone transition that is ALSO an error transition
// with a crash post-mortem: the step is certainly credited (its arrival pair
// gets the milestone's attract), and the run ends SutCrashed on that very
// step, so the catastrophe's repel lands under the SAME (marking, T-list)
// pair. The milestone report does not name it -- an error step of the same
// event wins, the run failed there -- but the pair deposit follows the
// credited step, exactly the rule the rebuild applies to every recorded
// milestone step. A restore rebuilds both, toned down by the carry-over
// factor.
static Net makeMilestoneCatastropheNet()
{
    NetBuilder b("milestone-catastrophe");
    const auto s0 = b.place("s0");
    const auto dead = b.place("dead");
    const auto serve = b.transition("serve", TransitionKind::Controllable, "serve");
    b.input(s0, serve).output(serve, dead);
    b.initial("start", {{s0, 1}});
    b.targetMarking("idle", {{s0, 1}});
    b.milestone(serve, 2);              // rank 2: a deposit of 2 * 1.5 = 3.0
    b.error(serve, DumpMode::Crash);    // the same step is the catastrophe
    return b.build();
}

static void testThePairLayerFollowsTheRunAndTheRestore()
{
    const Net net = makeMilestoneCatastropheNet();
    const auto serve = *net.findTransition("serve");

    SessionConfig cfg = quick();
    cfg.maxRuns = 1;             // one crashing run: exact pair amounts
    cfg.ants.tlistLength = 4;
    Session session(net, cfg);
    SimulatedSut sut(net, SimulatedSutOptions{});
    const CampaignReport rep = session.runCampaign(sut);
    CHECK(rep.runs.size() == 1);
    CHECK(rep.runs[0].outcome == RunOutcome::SutCrashed);
    CHECK(rep.runs[0].milestonesReached.empty());  // the error step wins the run
    CHECK(rep.runs[0].errorsReached.size() == 1 && rep.runs[0].errorsReached[0] == "serve");

    const StateId dead = session.graph().edge(*session.graph().findEdge(0, serve)).target;
    // The run cascade the pair was learned under: the one serve step.
    TraceCascade arrival;
    arrival.setLength(4);
    arrival.push(stepHashOf(net.transitions[serve]));

    // The milestone's attract landed at the credited arrival pair, exactly --
    // the run ended on that very step, so no evaporation touched it.
    CHECK(near(session.ants().pairAttract(dead, arrival), 3.0));
    // The crash's repel landed under the same pair, priced by the one input
    // on the way in: 3.0 * (1 + 0.25).
    CHECK(near(session.ants().pairRepel(dead, arrival), 3.75));
    CHECK(session.ants().pairCount() == 1);

    // --- the restore rebuilds the same pair keys, toned down ------------------
    PersistedRun recorded;
    recorded.initialMarking = "start";
    recorded.transitions = {"serve"};
    recorded.outcome = RunOutcome::SutCrashed;
    recorded.detail = "error transition 'serve': the SUT was crashed for a post-mortem";
    {
        Session restored(net, cfg);
        CHECK(restored.restore(recorded).ok);
        const StateId dead2 = restored.graph().edge(*restored.graph().findEdge(0, serve)).target;
        // Default carry-over 50 percent: half the milestone attract, half the
        // catastrophe repel -- under the same pair key the live run used.
        CHECK(near(restored.ants().pairAttract(dead2, arrival), 1.5));
        CHECK(near(restored.ants().pairRepel(dead2, arrival), 1.875));
        CHECK(restored.ants().pairCount() == 1);
    }
    {   // 100 percent rebuilds the live amounts.
        SessionConfig full = cfg;
        full.restoredPheromonePercent = 100;
        Session restored(net, full);
        CHECK(restored.restore(recorded).ok);
        CHECK(near(restored.ants().pairAttract(dead, arrival), 3.0));
        CHECK(near(restored.ants().pairRepel(dead, arrival), 3.75));
    }
    {   // With the layer off no pair exists, and nothing else changes.
        SessionConfig off = quick();
        off.maxRuns = 1;
        Session sessionOff(net, off);
        SimulatedSut sutOff(net, SimulatedSutOptions{});
        sessionOff.runCampaign(sutOff);
        CHECK(sessionOff.ants().pairCount() == 0);
    }
}

// GLM-5.3-Flash: the environment feed (doku section 12, item 3). How often
// each SUT move was offered versus taken is counted per label and feeds the
// ants' environment moves and the suggester's robustness estimate.
static void testTheEnvironmentFeedCountsOffersAndTakes()
{
    // --- the rates themselves --------------------------------------------------
    {
        const Net net = makeThreeBranchNet();
        ReachabilityGraph rg(net);
        CoverageDb db(rg);
        // Never offered: no evidence either way, stay neutral. In particular
        // silent internal steps (empty label) are never offered.
        CHECK(near(db.takeRate("never_seen"), 1.0));
        CHECK(near(db.takeRate(""), 1.0));
        db.recordOffered("oa");
        CHECK(near(db.takeRate("oa"), 0.0));   // offered, not taken
        db.recordTaken("oa");
        CHECK(near(db.takeRate("oa"), 1.0));
        db.recordOffered("oa");
        CHECK(near(db.takeRate("oa"), 0.5));   // one of two chances taken
        db.recordTaken("oa");
        db.recordTaken("oa");                  // taken more often than offered: clamped
        CHECK(near(db.takeRate("oa"), 1.0));
    }

    // --- a campaign: every answer the SUT gives was offered and taken ---------
    //
    // GLM-5.3-Flash: one corridor, and one-sided timing: the SUT's own move
    // comes ~150 ms after the input's acknowledgement, while the session
    // suggests within ~2 ms. The session therefore really WAITS for the
    // output (and offers it) instead of seeing the acknowledgement and the
    // output arrive in one batch -- which would skip the wait episode and
    // make this test depend on thread scheduling.
    {
        const Net net = makeOneCorridorNet();
        SessionConfig cfg = quick();
        cfg.maxRuns = 1;
        cfg.plugin.quiescenceTimeoutMs = 1000;  // longer than the SUT's step
        FakeSutOptions opts;
        opts.stepDelayMs = 150;
        Session session(net, cfg);
        FakeSut sut(net, opts);
        const CampaignReport rep = session.runCampaign(sut);
        CHECK(rep.complete);
        CHECK(rep.runs.size() == 1);
        const auto &stats = session.coverage().moveStats();
        const auto it = stats.find("oa");
        CHECK(it != stats.end());
        CHECK(it->second.offered >= 1);
        CHECK(it->second.taken >= 1);
        CHECK(near(session.coverage().takeRate("oa"), 1.0));
        // The wiring: the colony's environment moves are weighted by the
        // take rate of their label, and 1.0 for never-offered moves.
        CHECK(bool(session.ants().config().environmentWeight));
        const auto oa = *net.findTransition("oa");
        EdgeId oaEdge = kNoEdge;
        for (EdgeId e = 0; e < session.graph().numEdges(); ++e)
            if (session.graph().edge(e).transition == oa)
                oaEdge = e;
        CHECK(oaEdge != kNoEdge);
        CHECK(near(session.ants().config().environmentWeight(oaEdge), 1.0));
    }

    // --- a SUT that refuses a move registers a rate below one ------------------
    {
        const Net net = makeOneCorridorNet();
        FakeSutOptions opts;
        opts.stepDelayMs = 150;
        opts.weight["oa"] = 0.0;  // the SUT never serves the corridor's output
        SessionConfig cfg = quick();
        cfg.maxRuns = 2;
        cfg.plugin.quiescenceTimeoutMs = 300;  // the silence that convicts it
        Session session(net, cfg);
        FakeSut sut(net, opts);
        const CampaignReport rep = session.runCampaign(sut);
        CHECK(!rep.runs.empty());
        const auto &stats = session.coverage().moveStats();
        const auto oa = stats.find("oa");
        CHECK(oa != stats.end());
        CHECK(oa->second.offered >= 1);
        CHECK(oa->second.taken == 0);
        CHECK(near(session.coverage().takeRate("oa"), 0.0));
    }
}

// GLM-5.3-Flash: the crash-prediction experiment (doku section 17). The nets
// below are all-Controllable on purpose: the simulated SUT moves on its own
// only for observable/internal steps, so the session drives every step and
// the campaigns are deterministic. `crashAt` seeds the predicted crash the
// model declares on a CONTROLLABLE transition (handling that stimulus kills
// the SUT with the prediction's prior mean); `crashAfterEvents` kills it
// wherever it is, the seed of an implicit entry.
// GLM-5.3-Flash: the idle target marking keeps the static analysis from
// calling the only cycle of these all-Controllable nets a terminal livelock
// (it is non-intentional, so it is no goal and pulls no pheromones).
static Net makeAlwaysCrashNet()
{
    NetBuilder b("always-crash");
    const auto s0 = b.place("s0");
    const auto hit = b.transition("hit", TransitionKind::Controllable, "hit");
    const auto walk = b.transition("walk", TransitionKind::Controllable, "walk");
    b.input(s0, hit).output(hit, s0);
    b.input(s0, walk).output(walk, s0);
    b.crash(hit, CrashPrediction{CrashClass::Always});
    b.initial("start", {{s0, 1}});
    b.targetMarking("idle", {{s0, 1}});
    return b.build();
}

// go: s0 -> s1; hit: a predicted self-loop at s1; back: the way home. The
// transition order makes "hit" the lower index, so a pheromone tie suggests
// it first: the crash site is exercised before the campaign can walk away.
static Net makeRareCrashNet(CrashClass crashClass, std::uint32_t attempts)
{
    NetBuilder b("rare-crash");
    const auto s0 = b.place("s0");
    const auto s1 = b.place("s1");
    const auto go = b.transition("go", TransitionKind::Controllable, "go");
    const auto hit = b.transition("hit", TransitionKind::Controllable, "hit");
    const auto back = b.transition("back", TransitionKind::Controllable, "back");
    b.input(s0, go).output(go, s1);
    b.input(s1, hit).output(hit, s1);
    b.input(s1, back).output(back, s0);
    CrashPrediction p;
    p.crashClass = crashClass;
    p.attempts = attempts;
    b.crash(hit, p);
    b.initial("start", {{s0, 1}});
    b.targetMarking("idle", {{s0, 1}});
    return b.build();
}

// go: s0 -> s1 (into the error marking), back: the way home. `boomHits` is
// the error marking's own visit budget: high keeps the marking sought (its
// states never turn forbidden, so the way in stays open), 1 lets the first
// visit satisfy it so the campaign can complete.
static Net makeBoomMarkingNet(CrashClass crashClass, std::uint32_t attempts, std::uint32_t boomHits)
{
    NetBuilder b("boom-marking");
    const auto s0 = b.place("s0");
    const auto s1 = b.place("s1");
    const auto go = b.transition("go", TransitionKind::Controllable, "go");
    const auto back = b.transition("back", TransitionKind::Controllable, "back");
    b.input(s0, go).output(go, s1);
    b.input(s1, back).output(back, s0);
    ErrorOptions boom;
    boom.targetHits = boomHits;
    b.errorMarking("boom", {{s1, 1}}, MatchMode::Exact, boom);
    CrashPrediction p;
    p.crashClass = crashClass;
    p.attempts = attempts;
    b.crash("boom", p);
    b.initial("start", {{s0, 1}});
    b.targetMarking("idle", {{s0, 1}});
    return b.build();
}

static bool sameCrashEntry(const CrashEntry &e, const std::string &key, CrashStatus status,
                           std::uint32_t attempts, std::uint32_t crashes, bool predicted,
                           CrashClass crashClass = CrashClass::Random)
{
    return e.key == key && e.status == status && e.stats.attempts == attempts
           && e.stats.crashes == crashes && e.predicted == predicted && e.prediction.has_value() == predicted
           && (!predicted || e.prediction->crashClass == crashClass);
}

// A prediction of the strong class "always" is certainly deterministic at the
// first hit: the SUT dies at the stimulus, the death is attributed to the
// predicted transition, and the campaign stops seeking it -- the way in is
// marked as repellent and the next run walks the other branch to completion.
static void testAPredictedCrashIsCertainAtFirstHit()
{
    const Net net = makeAlwaysCrashNet();
    FakeSutOptions opt;
    opt.crashAt["hit"] = 1.0;
    SessionConfig cfg = quick();
    cfg.maxRuns = 4;
    Session session(net, cfg);
    FakeSut sut(net, opt);
    const CampaignReport rep = session.runCampaign(sut);
    CHECK(rep.runs.size() == 2 && rep.complete);

    const RunReport &r1 = rep.runs[0];
    CHECK(r1.outcome == RunOutcome::SutCrashed);
    CHECK(contains(r1.detail, "crashed while handling stimulus 'hit'"));
    CHECK(contains(r1.notes[0], "the crash was attributed to t:hit (deterministic: 1 attempt(s), 1 crash(es))"));
    bool repellent = false;
    for (const std::string &n : r1.notes)
        repellent = repellent || contains(n.c_str(), "the ant colony marked the crash site as repellent");
    CHECK(repellent);

    const RunReport &r2 = rep.runs[1];
    CHECK(r2.outcome == RunOutcome::Completed && r2.coverageDone);

    const std::vector<CrashEntry> &entries = session.coverage().crashEntries();
    CHECK(entries.size() == 1);
    CHECK(sameCrashEntry(entries[0], "t:hit", CrashStatus::Deterministic, 1, 1, true, CrashClass::Always));
}

// A death nobody predicted gets an implicit entry at the last credited step.
// Every run that visits the site is one attempt; every death is one hit; at
// the give-up budget (6 for implicit entries) the experiment resolves as
// exhausted -- and while it sought, the way in was attractive, only the final
// resolution marks the crash site as repellent.
static void testAnImplicitCrashEntryGrowsToItsBudget()
{
    NetBuilder b("implicit-crash");
    const auto s0 = b.place("s0");
    const auto s1 = b.place("s1");
    const auto a = b.transition("a", TransitionKind::Controllable, "a");
    const auto bb = b.transition("b", TransitionKind::Controllable, "b");
    b.input(s0, a).output(a, s1);
    b.input(s1, bb).output(bb, s0);
    b.initial("start", {{s0, 1}});
    b.targetMarking("idle", {{s0, 1}});
    const Net net = b.build();

    FakeSutOptions opt;
    opt.crashAfterEvents = 1;  // dies with the first acknowledgement, every run
    SessionConfig cfg = quick();
    cfg.maxRuns = 6;
    Session session(net, cfg);
    FakeSut sut(net, opt);
    const CampaignReport rep = session.runCampaign(sut);
    CHECK(rep.runs.size() == 6 && !rep.complete);

    std::size_t attractive = 0, repellent = 0;
    for (std::size_t i = 0; i < rep.runs.size(); ++i) {
        const RunReport &r = rep.runs[i];
        CHECK(r.outcome == RunOutcome::SutCrashed);
        const std::string expected = i + 1 < 6
            ? "the crash was attributed to t:a (seeking: " + std::to_string(i + 1) + " attempt(s), "
                  + std::to_string(i + 1) + " crash(es))"
            : "the crash was attributed to t:a (exhausted: 6 attempt(s), 5 crash(es))";
        bool sawAttribution = false;
        for (const std::string &n : r.notes) {
            sawAttribution = sawAttribution || n.find(expected) != std::string::npos;
            attractive += n.find("marked the way into the crash site as attractive") != std::string::npos ? 1 : 0;
            repellent += n.find("marked the crash site as repellent") != std::string::npos ? 1 : 0;
        }
        CHECK(sawAttribution);
    }
    CHECK(attractive == 5 && repellent == 1);

    const std::vector<CrashEntry> &entries = session.coverage().crashEntries();
    CHECK(entries.size() == 1);
    CHECK(sameCrashEntry(entries[0], "t:a", CrashStatus::Exhausted, 6, 5, false));
}

// A prediction with its own budget gives up there: the crashes up to the
// budget are sought (attractive), the last one resolves the experiment as
// exhausted (repellent), and the freed campaign walks home to completion.
static void testAnExhaustedPredictionCatastrophesThenReleases()
{
    const Net net = makeRareCrashNet(CrashClass::Rare, 4);
    FakeSutOptions opt;
    opt.crashAt["hit"] = 1.0;
    SessionConfig cfg = quick();
    cfg.maxRuns = 6;
    Session session(net, cfg);
    FakeSut sut(net, opt);
    const CampaignReport rep = session.runCampaign(sut);
    CHECK(rep.runs.size() == 5 && rep.complete);

    for (std::size_t i = 0; i < 3; ++i) {
        const RunReport &r = rep.runs[i];
        CHECK(r.outcome == RunOutcome::SutCrashed);
        const std::string note = r.notes.empty() ? std::string() : r.notes[0];
        CHECK(note.find("the crash was attributed to t:hit (seeking: " + std::to_string(i + 1)
                        + " attempt(s), " + std::to_string(i + 1) + " crash(es))") != std::string::npos);
    }
    const RunReport &r4 = rep.runs[3];
    CHECK(r4.outcome == RunOutcome::SutCrashed);
    CHECK(contains(r4.notes[0], "the crash was attributed to t:hit (exhausted: 4 attempt(s), 3 crash(es))"));
    const RunReport &r5 = rep.runs[4];
    CHECK(r5.outcome == RunOutcome::Completed && r5.coverageDone);

    const std::vector<CrashEntry> &entries = session.coverage().crashEntries();
    CHECK(entries.size() == 1);
    CHECK(sameCrashEntry(entries[0], "t:hit", CrashStatus::Exhausted, 4, 3, true, CrashClass::Rare));
}

// A prediction whose crash never shows up resolves as unobserved at its own
// budget -- from definite visits alone, no death needed -- and teaches the
// colony nothing: no attractive and no repellent note in the whole campaign.
static void testANeverFiringPredictionResolvesUnobserved()
{
    const Net net = makeRareCrashNet(CrashClass::Rare, 1);
    FakeSutOptions opt;  // no crashAt: the SUT never dies
    SessionConfig cfg = quick();
    cfg.maxRuns = 2;
    Session session(net, cfg);
    FakeSut sut(net, opt);
    const CampaignReport rep = session.runCampaign(sut);
    CHECK(rep.runs.size() == 1 && rep.complete);
    CHECK(rep.runs[0].outcome == RunOutcome::Completed && rep.runs[0].coverageDone);
    CHECK(rep.runs[0].notes.empty());

    const std::vector<CrashEntry> &entries = session.coverage().crashEntries();
    CHECK(entries.size() == 1);
    CHECK(sameCrashEntry(entries[0], "t:hit", CrashStatus::Unobserved, 1, 0, true, CrashClass::Rare));
}

// A prediction on a MARKING: the death while the SUT is certainly in the
// error marking is attributed to m:boom, not to the transition in flight;
// resolved at the first hit (class always), it stays there over repeated
// crashes. Without a death the marking's certain visits alone count as
// attempts and the experiment resolves unobserved at its budget.
static void testAPredictedMarkingCrashIsAttributedToTheMarking()
{
    {   // the SUT dies at the stimulus, certainly inside "boom": attributed to the marking
        const Net net = makeBoomMarkingNet(CrashClass::Always, 0, 10);
        FakeSutOptions opt;
        opt.crashAt["go"] = 1.0;
        SessionConfig cfg = quick();
        cfg.maxRuns = 2;
        Session session(net, cfg);
        FakeSut sut(net, opt);
        const CampaignReport rep = session.runCampaign(sut);
        CHECK(rep.runs.size() == 2 && !rep.complete);
        for (const RunReport &r : rep.runs) {
            CHECK(r.outcome == RunOutcome::SutCrashed);
            CHECK(contains(r.notes[0],
                           "the crash was attributed to m:boom (deterministic: 1 attempt(s), 1 crash(es))"));
        }
        const std::vector<CrashEntry> &entries = session.coverage().crashEntries();
        CHECK(entries.size() == 1);
        CHECK(sameCrashEntry(entries[0], "m:boom", CrashStatus::Deterministic, 1, 1, true, CrashClass::Always));
        CHECK(entries[0].transition == kNoTransition);
    }
    {   // no death at all: the certain visits count, the budget resolves unobserved
        const Net net = makeBoomMarkingNet(CrashClass::Rare, 1, 1);
        SessionConfig cfg = quick();
        cfg.maxRuns = 2;
        Session session(net, cfg);
        FakeSut sut(net, FakeSutOptions{});
        const CampaignReport rep = session.runCampaign(sut);
        CHECK(rep.runs.size() == 1 && rep.complete);
        CHECK(rep.runs[0].outcome == RunOutcome::Completed);
        CHECK(rep.runs[0].notes.empty());
        const std::vector<CrashEntry> &entries = session.coverage().crashEntries();
        CHECK(entries.size() == 1);
        CHECK(sameCrashEntry(entries[0], "m:boom", CrashStatus::Unobserved, 1, 0, true, CrashClass::Rare));
    }
}

// The trail (attempt keys in counting order) and the attribution travel with
// the recording; a fresh session restored from the runs evolves its
// experiment to exactly the live one.
static void testTheCrashExperimentRebuildsExactly()
{
    const Net net = makeRareCrashNet(CrashClass::Sometimes, 3);
    FakeSutOptions opt;
    opt.crashAt["hit"] = 1.0;
    SessionConfig cfg = quick();
    cfg.maxRuns = 4;
    std::vector<PersistedRun> saved;
    cfg.runSink = [&saved](const PersistedRun &r) { saved.push_back(r); };
    Session session(net, cfg);
    FakeSut sut(net, opt);
    const CampaignReport rep = session.runCampaign(sut);
    CHECK(rep.runs.size() == 4 && rep.complete);
    CHECK(saved.size() == 4);

    Session later(net, cfg);
    for (const PersistedRun &pr : saved)
        CHECK(later.restore(pr).ok);
    const std::vector<CrashEntry> &live = session.coverage().crashEntries();
    const std::vector<CrashEntry> &rebuilt = later.coverage().crashEntries();
    CHECK(live.size() == rebuilt.size());
    for (std::size_t i = 0; i < live.size() && i < rebuilt.size(); ++i) {
        CHECK(rebuilt[i].key == live[i].key);
        CHECK(rebuilt[i].status == live[i].status);
        CHECK(rebuilt[i].stats.attempts == live[i].stats.attempts);
        CHECK(rebuilt[i].stats.crashes == live[i].stats.crashes);
        CHECK(rebuilt[i].predicted == live[i].predicted);
        CHECK(rebuilt[i].transition == live[i].transition);
        CHECK(rebuilt[i].dumps == live[i].dumps);
    }
    CHECK(live.size() == 1);
    CHECK(sameCrashEntry(live[0], "t:hit", CrashStatus::Exhausted, 3, 2, true, CrashClass::Sometimes));
}

// ---------------------------------------------------------------------------
// GLM-5.3-Flash: command hooks (doku section 18). The hooks run through sh -c
// in the test's environment, so a hook proves what it saw and what it did by
// writing files the test points at with MBT_HOOKFILE / MBT_ENVFILE. Every
// synchronous hook here is bounded: its transition fires a known, small
// number of times (or the hook carries a "times" budget).
// ---------------------------------------------------------------------------

static std::string readFile(const std::string &path)
{
    std::ifstream in(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

// A linear chain s0 -a-> s1 -b-> s2 -c-> s3, whose end is the intentional
// target: the campaign completes in one run of three inputs. The first two
// inputs may carry command hooks (empty string: none) with an optional budget
// on b; the third may carry an asynchronous one. No observables: the simulated
// SUT makes no own moves, so every step is certain the moment its
// acknowledgement arrives.
static Net makeHookChainNet(const std::string &cmdA = "", const std::string &cmdB = "",
                            const std::string &cmdC = "", bool cAsync = false,
                            std::uint32_t timesB = 0)
{
    NetBuilder b("hook-chain");
    const auto s0 = b.place("s0");
    const auto s1 = b.place("s1");
    const auto s2 = b.place("s2");
    const auto s3 = b.place("s3");
    const auto ta = b.transition("ta", TransitionKind::Controllable, "a");
    const auto tb = b.transition("tb", TransitionKind::Controllable, "b");
    const auto tc = b.transition("tc", TransitionKind::Controllable, "c");
    b.input(s0, ta).output(ta, s1);
    b.input(s1, tb).output(tb, s2);
    b.input(s2, tc).output(tc, s3);
    if (!cmdA.empty())
        b.command(ta, CommandHook{cmdA, HookMode::Sync, std::nullopt, kNoPlace});
    if (!cmdB.empty()) {
        CommandHook hook{cmdB, HookMode::Sync, std::nullopt, kNoPlace};
        if (timesB > 0)
            hook.times = timesB;  // "once" is spelled times 1
        b.command(tb, hook);
    }
    if (!cmdC.empty())
        b.command(tc, CommandHook{cmdC, cAsync ? HookMode::Async : HookMode::Sync,
                                  std::nullopt, kNoPlace});
    b.initial("start", {{s0, 1}});
    b.targetMarking("done", {{s3, 1}}, MatchMode::Exact, true);
    return b.build();
}

// The same chain, but the first two hooks carry a "when" guard on s2: ta's
// step leaves the tester in s1 (the guard fails), tb's step in s2 (it holds).
// The command is shared: it appends the transition's own id, which the hook
// receives as MBT_TRANSITION.
static Net makeHookGuardNet(const std::string &cmd)
{
    NetBuilder b("hook-guard");
    const auto s0 = b.place("s0");
    const auto s1 = b.place("s1");
    const auto s2 = b.place("s2");
    const auto s3 = b.place("s3");
    const auto ta = b.transition("ta", TransitionKind::Controllable, "a");
    const auto tb = b.transition("tb", TransitionKind::Controllable, "b");
    const auto tc = b.transition("tc", TransitionKind::Controllable, "c");
    b.input(s0, ta).output(ta, s1);
    b.input(s1, tb).output(tb, s2);
    b.input(s2, tc).output(tc, s3);
    b.command(ta, CommandHook{cmd, HookMode::Sync, std::nullopt, s2});
    b.command(tb, CommandHook{cmd, HookMode::Sync, std::nullopt, s2});
    b.initial("start", {{s0, 1}});
    b.targetMarking("done", {{s3, 1}}, MatchMode::Exact, true);
    return b.build();
}

// Ambiguity by label: x1 and x2 both observe "x" from s0. The simulated SUT
// never fires x2 (weight 0), but the tester cannot know that: after "x" the
// belief spans s1 and s2, and only "y" -- possible from s1 alone -- resolves
// the stretch, ruling x2 out. Its command never ran, and the run says so. y
// ends in the intentional target, so the run ends as soon as the stretch is
// resolved (the resolution's credits and rule-outs land in the final settle).
static Net makeRuledOutNet(const std::string &cmd)
{
    NetBuilder b("ruled-out");
    const auto s0 = b.place("s0");
    const auto s1 = b.place("s1");
    const auto s2 = b.place("s2");
    const auto s3 = b.place("s3");
    const auto x1 = b.transition("x1", TransitionKind::Observable, "x");
    const auto x2 = b.transition("x2", TransitionKind::Observable, "x");
    const auto y = b.transition("y", TransitionKind::Observable, "y");
    b.input(s0, x1).output(x1, s1);
    b.input(s0, x2).output(x2, s2);
    b.input(s1, y).output(y, s3);
    b.command(x2, CommandHook{cmd, HookMode::Sync, std::nullopt, kNoPlace});
    b.initial("start", {{s0, 1}});
    b.targetMarking("done", {{s3, 1}}, MatchMode::Exact, true);
    return b.build();
}

static void testCommandHooksRunSyncAndReportFailures()
{
    const Net net = makeHookChainNet("printf 'A-out\\n'", "printf 'B-err\\n' >&2; exit 3");
    SessionConfig cfg = quick();
    Session session(net, cfg);
    FakeSut sut(net);
    const CampaignReport rep = session.runCampaign(sut);

    CHECK(rep.runs.size() == 1 && rep.runs[0].outcome == RunOutcome::Completed);
    const RunRecord &rec = session.coverage().run(0);
    CHECK(rec.hookRuns.size() == 2);
    if (rec.hookRuns.size() == 2) {
        const HookRecord &a = rec.hookRuns[0];
        CHECK(a.transition == "ta" && a.label == "a" && !a.async);
        CHECK(a.status == "ok" && a.exit == 0 && a.output == "A-out\n");
        const HookRecord &c = rec.hookRuns[1];
        CHECK(c.transition == "tb" && c.status == "failed" && c.exit == 3);
        CHECK(c.output == "B-err\n");
    }
    // "Note only, run continues": the failure is a note, the campaign went on.
    CHECK(rep.runs[0].notes.size() == 1
          && rep.runs[0].notes[0] == "the command of 'tb' failed (exit 3): the run continues");
}

static void testCommandHookBudgetLimitsExecutions()
{
    // A self-loop the simulated SUT fires all the time: every tick is a
    // certain step, so an unlimited hook would run on every one of them. The
    // budget of 2 stops it after the second execution -- and the next run (a
    // fresh run, the same campaign) gets nothing: the budget is the whole
    // campaign's.
    NetBuilder b("ticker");
    const auto s0 = b.place("s0");
    const auto tick = b.transition("tick", TransitionKind::Observable, "tick");
    b.input(s0, tick).output(tick, s0);
    b.command(tick, CommandHook{"true", HookMode::Sync, std::optional<std::uint32_t>(2), kNoPlace});
    b.initial("start", {{s0, 1}});
    const Net net = b.build();

    SessionConfig cfg = quick();
    cfg.maxEventsPerRun = 12;  // the run ends when the event budget is spent
    cfg.analyzeModel = false;
    cfg.maxRuns = 2;
    Session session(net, cfg);
    FakeSut sut(net);
    const CampaignReport rep = session.runCampaign(sut);

    CHECK(rep.runs.size() == 2);
    const RunRecord &first = session.coverage().run(0);
    const TransitionIndex tickIndex = *net.findTransition("tick");
    CHECK(first.hookRuns.size() == 2);
    for (const HookRecord &h : first.hookRuns)
        CHECK(h.transition == "tick" && h.status == "ok");
    // The cap guarantees at least 12 certain ticks (a burst may overshoot
    // it); the budget, not the exact count, is what this test pins.
    CHECK(std::count(first.transitions.begin(), first.transitions.end(), tickIndex) >= 12);
    CHECK(session.coverage().run(1).hookRuns.empty());
    CHECK(rep.runs[0].notes.empty());  // a spent budget is not a failure
}

static void testCommandHookGuardReadsTheBeliefAfterTheStep()
{
    TempDir dir;
    const std::string marker = dir.filePath("hooks.txt");
    ::setenv("MBT_HOOKFILE", marker.c_str(), 1);
    // One command for both hooks: it appends the transition's own id.
    const Net net = makeHookGuardNet("echo \"$MBT_TRANSITION\" >> \"$MBT_HOOKFILE\"");
    SessionConfig cfg = quick();
    Session session(net, cfg);
    FakeSut sut(net);
    const CampaignReport rep = session.runCampaign(sut);

    CHECK(rep.runs.size() == 1 && rep.runs[0].outcome == RunOutcome::Completed);
    const RunRecord &rec = session.coverage().run(0);
    CHECK(rec.hookRuns.size() == 1 && rec.hookRuns[0].transition == "tb");
    CHECK(readFile(marker) == "tb\n");  // ta's guard failed: its command never ran
}

static void testAnAsyncCommandHookIsLaunchedNotWaited()
{
    TempDir dir;
    const std::string marker = dir.filePath("hooks.txt");
    ::setenv("MBT_HOOKFILE", marker.c_str(), 1);
    const Net net = makeHookChainNet("", "", "echo tc >> \"$MBT_HOOKFILE\"", /*cAsync=*/true);
    SessionConfig cfg = quick();
    Session session(net, cfg);
    FakeSut sut(net);
    const CampaignReport rep = session.runCampaign(sut);

    CHECK(rep.runs.size() == 1 && rep.runs[0].outcome == RunOutcome::Completed);
    const RunRecord &rec = session.coverage().run(0);
    CHECK(rec.hookRuns.size() == 1);
    if (rec.hookRuns.size() == 1) {
        const HookRecord &h = rec.hookRuns[0];
        CHECK(h.transition == "tc" && h.async && h.status == "launched" && h.exit == kHookNoExit);
        CHECK(h.output.empty());
    }
    // Fire-and-forget: the child was not waited for inside the run, but it was
    // not killed either -- give it its moment, then the marker must be there.
    for (int i = 0; i < 100 && readFile(marker) != "tc\n"; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    CHECK(readFile(marker) == "tc\n");
}

static void testATimedOutSyncHookIsKilledAndNoted()
{
    const Net net = makeHookChainNet("", "sleep 30");
    SessionConfig cfg = quick();
    cfg.hookTimeoutSeconds = 1;  // the hook runs 30 s: it is killed after 1 s
    Session session(net, cfg);
    FakeSut sut(net);
    const CampaignReport rep = session.runCampaign(sut);

    CHECK(rep.runs.size() == 1 && rep.runs[0].outcome == RunOutcome::Completed);
    const RunRecord &rec = session.coverage().run(0);
    CHECK(rec.hookRuns.size() == 1);
    if (rec.hookRuns.size() == 1) {
        const HookRecord &h = rec.hookRuns[0];
        CHECK(h.transition == "tb" && h.status == "timeout" && h.exit == kHookNoExit);
    }
    CHECK(rep.runs[0].notes.size() == 1
          && rep.runs[0].notes[0] == "the command of 'tb' timed out after 1 s: the run continues");
}

static void testARuledOutSuspectCommandIsReported()
{
    // "Certain steps only but suspect steps which do not execute need to be
    // reported." x2 is a suspect while the stretch is ambiguous, then the
    // stretch resolves against it: its hook did not run, and the run notes it.
    const Net net = makeRuledOutNet("true");
    FakeSutOptions opt;
    opt.weight = {{"x1", 1}, {"x2", 0}, {"y", 1}, {"go", 1}};
    SessionConfig cfg = quick();
    Session session(net, cfg);
    FakeSut sut(net, opt);
    const CampaignReport rep = session.runCampaign(sut);

    CHECK(rep.runs.size() == 1);
    CHECK(session.coverage().run(0).hookRuns.empty());  // the command never ran
    CHECK(rep.runs[0].notes.size() == 1
          && rep.runs[0].notes[0] == "the command of 'x2' did not run: its suspect step was ruled out");
}

static void testRestoredRunsAdoptHookRecordsWithoutReexecuting()
{
    TempDir dir;
    const std::string marker = dir.filePath("hooks.txt");
    ::setenv("MBT_HOOKFILE", marker.c_str(), 1);
    const Net net = makeHookChainNet("echo \"$MBT_TRANSITION\" >> \"$MBT_HOOKFILE\"");
    SessionConfig cfg = quick();
    std::vector<PersistedRun> saved;
    cfg.runSink = [&saved](const PersistedRun &r) { saved.push_back(r); };
    Session session(net, cfg);
    FakeSut sut(net);
    const CampaignReport rep = session.runCampaign(sut);
    CHECK(rep.runs.size() == 1);
    CHECK(readFile(marker) == "ta\n");  // the live run executed the command once
    CHECK(saved.size() == 1 && saved[0].hooks.size() == 1 && saved[0].hooks[0].status == "ok");

    // The recording travels, the command does not run again.
    Session later(net, quick());
    CHECK(later.restore(saved[0]).ok);
    CHECK(readFile(marker) == "ta\n");
    const RunRecord &rec = later.coverage().run(0);
    CHECK(rec.hookRuns.size() == 1 && rec.hookRuns[0].transition == "ta"
          && rec.hookRuns[0].status == "ok" && rec.hookRuns[0].exit == 0);
}

static void testCommandHooksReceiveTheirContext()
{
    TempDir dir;
    const std::string envFile = dir.filePath("env.txt");
    const std::string logPath = dir.filePath("trail.log");
    ::setenv("MBT_ENVFILE", envFile.c_str(), 1);
    // The hook writes what it saw: transition id | label | run id | log path.
    const Net net = makeHookChainNet(
        "printf '%s|%s|%s|%s\\n' \"$MBT_TRANSITION\" \"$MBT_TRANSITION_LABEL\" \"$MBT_RUN\" \"$MBT_LOG\""
        " > \"$MBT_ENVFILE\"");
    SessionConfig cfg = quick();
    cfg.logPath = logPath;  // the open run log travels to every hook as MBT_LOG
    Session session(net, cfg);
    FakeSut sut(net);
    const CampaignReport rep = session.runCampaign(sut);
    CHECK(rep.runs.size() == 1);
    CHECK(readFile(envFile) == "ta|a|0|" + logPath + "\n");
}

// ---------------------------------------------------------------------------

int main()
{
    const std::vector<std::pair<const char *, std::function<void()>>> tests = {
        {"aCampaignReachesFullCoverageAndRestoresLater", testACampaignReachesFullCoverageAndRestoresLater},
        {"noSecondInputWhileOneIsInFlight", testNoSecondInputWhileOneIsInFlight},
        {"aJamIsFoundByQuiescenceAndDumpedExactlyOnce", testAJamIsFoundByQuiescenceAndDumpedExactlyOnce},
        {"aCrashEndsTheRunAndTheNextRunRestartsTheSut", testACrashEndsTheRunAndTheNextRunRestartsTheSut},
        {"afterACrashTheAntsKeepTheCampaignProductive", testAfterACrashTheAntsKeepTheCampaignProductive},  // GLM-5.3-Flash
        {"afterADeadlockCrashTheAntsDoTheSame", testAfterADeadlockCrashTheAntsDoTheSame},  // GLM-5.3-Flash
        {"theLongerTheWayInTheWorseTheCatastrophePheromone", testTheLongerTheWayInTheWorseTheCatastrophePheromone},  // GLM-5.3-Flash
        {"theCatastrophePricingCanBeSwitchedOff", testTheCatastrophePricingCanBeSwitchedOff},  // GLM-5.3-Flash
        {"anUnpredictableStepKeepsTheDepositFlat", testAnUnpredictableStepKeepsTheDepositFlat},  // GLM-5.3-Flash
        {"aGoodTargetMarkingEndsTheCampaign", testAGoodTargetMarkingEndsTheCampaign},  // GLM-5.3-Flash
        {"anErrorTransitionIsCaughtLikeAnErrorMarking", testAnErrorTransitionIsCaughtLikeAnErrorMarking},  // GLM-5.3-Flash
        {"anErrorTransitionWithoutADumpLetsTheRunContinue", testAnErrorTransitionWithoutADumpLetsTheRunContinue},  // GLM-5.3-Flash
        {"dumpsFallBackToWhatThePluginCan", testDumpsFallBackToWhatThePluginCan},
        {"aDeviationEndsTheRunWithAnOptionalSnapshot", testADeviationEndsTheRunWithAnOptionalSnapshot},
        {"aRunTimeLivelockEndsTheRun", testARunTimeLivelockEndsTheRun},
        {"anEventQueueOverflowAbortsTheRun", testAnEventQueueOverflowAbortsTheRun},
        {"aPluginThatCannotStartAbortsTheCampaign", testAPluginThatCannotStartAbortsTheCampaign},
        {"aSilentSutIsNoticed", testASilentSutIsNoticed},
        {"aPluginFaultAbortsTheRun", testAPluginFaultAbortsTheRun},
        {"stopRequestEndsTheCampaign", testStopRequestEndsTheCampaign},
        {"aModelWithoutAnIdleStateIsFlagged", testAModelWithoutAnIdleStateIsFlagged},
        {"staticLivelocksBecomeTargets", testStaticLivelocksBecomeTargets},
        {"theAntColonyCanBeSwitchedOff", testTheAntColonyCanBeSwitchedOff},  // GLM-5.3-Flash
        {"aStateLimitTruncatesTheGraphWithoutFalseFindings", testAStateLimitTruncatesTheGraphWithoutFalseFindings},  // GLM-5.3-Flash
        {"theGraphGrowsLazilyDuringTheCampaign", testTheGraphGrowsLazilyDuringTheCampaign},  // GLM-5.3-Flash
        {"anSutBeyondTheTruncatedModelIsFlaggedAsPossiblyIncomplete", testAnSutBeyondTheTruncatedModelIsFlaggedAsPossiblyIncomplete},  // GLM-5.3-Flash
        {"theSeedSteersAndReproducesCampaigns", testTheSeedSteersAndReproducesCampaigns},  // GLM-5.3-Flash
        {"aNamedInternalStepIsReportedOnlyWhenTraced", testANamedInternalStepIsReportedOnlyWhenTraced},  // GLM-5.3-Flash
        {"anInternalStepWithoutACounterpartIsFollowedAndCovered", testAnInternalStepWithoutACounterpartIsFollowedAndCovered},  // GLM-5.3-Flash
        {"anInternalOnlyStallIsAMissingOutput", testAnInternalOnlyStallIsAMissingOutput},  // GLM-5.3-Flash
        {"theCampaignMeasuresItsTime", testTheCampaignMeasuresItsTime},  // GLM-5.3-Flash
        {"aMilestoneStepIsRewarded", testAMilestoneStepIsRewarded},  // GLM-5.3-Flash
        {"theRebuildReplaysWhatTheRunTaught", testTheRebuildReplaysWhatTheRunTaught},  // GLM-5.3-Flash
        {"thePairLayerFollowsTheRunAndTheRestore", testThePairLayerFollowsTheRunAndTheRestore},  // GLM-5.3-Flash
        {"theEnvironmentFeedCountsOffersAndTakes", testTheEnvironmentFeedCountsOffersAndTakes},  // GLM-5.3-Flash
        {"aPredictedCrashIsCertainAtFirstHit", testAPredictedCrashIsCertainAtFirstHit},  // GLM-5.3-Flash
        {"anImplicitCrashEntryGrowsToItsBudget", testAnImplicitCrashEntryGrowsToItsBudget},  // GLM-5.3-Flash
        {"anExhaustedPredictionCatastrophesThenReleases", testAnExhaustedPredictionCatastrophesThenReleases},  // GLM-5.3-Flash
        {"aNeverFiringPredictionResolvesUnobserved", testANeverFiringPredictionResolvesUnobserved},  // GLM-5.3-Flash
        {"aPredictedMarkingCrashIsAttributedToTheMarking", testAPredictedMarkingCrashIsAttributedToTheMarking},  // GLM-5.3-Flash
        {"theCrashExperimentRebuildsExactly", testTheCrashExperimentRebuildsExactly},  // GLM-5.3-Flash
        {"commandHooksRunSyncAndReportFailures", testCommandHooksRunSyncAndReportFailures},  // GLM-5.3-Flash
        {"commandHookBudgetLimitsExecutions", testCommandHookBudgetLimitsExecutions},  // GLM-5.3-Flash
        {"commandHookGuardReadsTheBeliefAfterTheStep", testCommandHookGuardReadsTheBeliefAfterTheStep},  // GLM-5.3-Flash
        {"anAsyncCommandHookIsLaunchedNotWaited", testAnAsyncCommandHookIsLaunchedNotWaited},  // GLM-5.3-Flash
        {"aTimedOutSyncHookIsKilledAndNoted", testATimedOutSyncHookIsKilledAndNoted},  // GLM-5.3-Flash
        {"aRuledOutSuspectCommandIsReported", testARuledOutSuspectCommandIsReported},  // GLM-5.3-Flash
        {"restoredRunsAdoptHookRecordsWithoutReexecuting", testRestoredRunsAdoptHookRecordsWithoutReexecuting},  // GLM-5.3-Flash
        {"commandHooksReceiveTheirContext", testCommandHooksReceiveTheirContext},  // GLM-5.3-Flash
    };
    for (const auto &t : tests) {
        const int before = g_failures;
        t.second();
        std::printf("%s %s\n", g_failures == before ? "PASS" : "FAIL", t.first);
    }
    std::printf("%d failure(s)\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
