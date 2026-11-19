// DeepSeek V4.1 Flash: the reserved lifecycle labels (doku section 26). A
// controllable transition labeled start/restart/stop/pause/resume is sent to
// the SUT like any input -- the SUT's own control does the work, which is the
// point for SUTs that cannot be restarted from the command line -- and the
// engine adds the lifecycle meaning:
//   * a start/restart without post arcs lands the run at its initial marking;
//   * a resume without post arcs lands it where the pause suspended it;
//   * stop/pause only close the lifecycle gate;
//   * while the gate is not Running, only the way back is offered, and
//     quiescence is expected rather than a fault.
#include "engine/BeliefTracker.h"
#include "engine/CoverageDb.h"
#include "engine/ReachabilityGraph.h"
#include "engine/RunLog.h"  // DeepSeek V4.1 Flash: the replay parity test
#include "engine/Session.h"
#include "engine/SimulatedSut.h"
#include "model/NetBuilder.h"

#include <chrono>
#include <cstdio>
#include <string>
#include <utility>
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

namespace {

SutEvent stim(const char *label, std::int64_t t = 0)
{
    SutEvent e;
    e.kind = EventKind::Stimulated;
    e.label = label;
    e.timestampMs = t;
    return e;
}

bool lists(const std::vector<ControllableCandidate> &cands, const Net &net, const char *label)
{
    for (const ControllableCandidate &c : cands)
        if (net.transitions[c.transition].label == label)
            return true;
    return false;
}

std::size_t candidateCount(const std::vector<ControllableCandidate> &cands)
{
    return cands.size();
}

// A run that begins at the app's start screen; an arc-less start brings the
// SUT up (engine-landed), then one ordinary input ends the campaign at an
// intentional target.
Net makeStartModel()
{
    NetBuilder b("lifecycle-start");
    const auto screen = b.place("screen");
    const auto done = b.place("done");
    const auto start = b.transition("start", TransitionKind::Controllable, "start");
    b.lifecycle(start, LifecycleAction::Start);
    const auto go = b.transition("go", TransitionKind::Controllable, "go");
    b.input(screen, go).output(go, done);
    b.initial("boot", {{screen, 1}});
    b.targetMarking("finished", {{done, 1}}, MatchMode::Exact, /*intentional=*/true);
    return b.build();
}

// Pause is an ordinary step into a paused marking; resume is arc-less, so the
// engine lands the run back at the state the pause suspended.
Net makePauseModel()
{
    NetBuilder b("lifecycle-pause");
    const auto run = b.place("run");
    const auto paused = b.place("paused");
    const auto done = b.place("done");
    const auto pause = b.transition("pause", TransitionKind::Controllable, "pause");
    b.input(run, pause).output(pause, paused);
    const auto resume = b.transition("resume", TransitionKind::Controllable, "resume");
    b.lifecycle(resume, LifecycleAction::Resume);
    const auto go = b.transition("go", TransitionKind::Controllable, "go");
    b.input(run, go).output(go, done);
    b.initial("up", {{run, 1}});
    b.targetMarking("finished", {{done, 1}});
    return b.build();
}

// Start and stop are both arc-less: start lands at the initial marking, stop
// only closes the gate. The model has no arc-less restart, so the run does
// not begin down -- it begins running and the test presses stop itself.
Net makeStopModel()
{
    NetBuilder b("lifecycle-stop");
    const auto run = b.place("run");
    const auto done = b.place("done");
    const auto start = b.transition("start", TransitionKind::Controllable, "start");
    b.lifecycle(start, LifecycleAction::Start);
    const auto stop = b.transition("stop", TransitionKind::Controllable, "stop");
    b.lifecycle(stop, LifecycleAction::Stop);
    const auto go = b.transition("go", TransitionKind::Controllable, "go");
    b.input(run, go).output(go, done);
    b.initial("boot", {{run, 1}});
    b.targetMarking("finished", {{done, 1}});
    return b.build();
}

bool holdsPlace(const ReachabilityGraph &rg, StateId s, PlaceIndex p)
{
    return rg.tokens(s)[p] > 0;
}

void theExactReservedWordsAreRecognized()
{
    CHECK(lifecycleOf("start") == LifecycleAction::Start);
    CHECK(lifecycleOf("restart") == LifecycleAction::Restart);
    CHECK(lifecycleOf("stop") == LifecycleAction::Stop);
    CHECK(lifecycleOf("pause") == LifecycleAction::Pause);
    CHECK(lifecycleOf("resume") == LifecycleAction::Resume);
    CHECK(lifecycleOf("Start") == LifecycleAction::None);   // case-sensitive
    CHECK(lifecycleOf("started") == LifecycleAction::None);  // whole word only
    CHECK(lifecycleOf("door.open") == LifecycleAction::None);
}

void anArcLessStartIsEngineLandedAndOpensTheGate()
{
    const Net net = makeStartModel();
    ReachabilityGraph rg(net);
    CoverageDb db(rg);
    BeliefTracker tracker(rg, &db);
    tracker.beginRun(0);

    // The model has an engine-landed start: every run begins with the gate
    // down, so the campaign presses the SUT's own start before testing.
    CHECK(tracker.lifecycleGate() == LifecycleGate::Down);
    const auto start = net.findTransition("start");
    CHECK(start.has_value());
    CHECK(tracker.suggestedLifecycle() == *start);
    {
        const auto cands = tracker.enabledControllable();
        CHECK(candidateCount(cands) == 1);
        CHECK(lists(cands, net, "start"));
    }

    CHECK(tracker.onEvent(stim("start", 1)).status == StepStatus::Ok);
    CHECK(tracker.lifecycleGate() == LifecycleGate::Running);
    CHECK(tracker.suggestedLifecycle() == kNoTransition);
    {
        const auto cands = tracker.enabledControllable();
        CHECK(lists(cands, net, "go"));
        CHECK(!lists(cands, net, "start"));  // engine-landed inputs are not ordinary moves
    }
    // The landing is the run's initial marking.
    CHECK(tracker.belief().size() == 1);
    const PlaceIndex screen = *net.findPlace("screen");
    CHECK(holdsPlace(rg, tracker.belief().front(), screen));

    const auto go = net.findTransition("go");
    CHECK(go.has_value());
    CHECK(tracker.onEvent(stim("go", 2)).status == StepStatus::Ok);
    const PlaceIndex done = *net.findPlace("done");
    CHECK(tracker.belief().size() == 1);
    CHECK(holdsPlace(rg, tracker.belief().front(), done));
    // The lifecycle transition was credited even though it has no RG edge.
    CHECK(db.transitionHits(*start) == 1);
    CHECK(db.transitionHits(*go) == 1);
    tracker.endRun(RunOutcome::Completed, "test");
}

void pauseSuspendsTheRunAndResumeLandsWhereItStopped()
{
    const Net net = makePauseModel();
    ReachabilityGraph rg(net);
    CoverageDb db(rg);
    BeliefTracker tracker(rg, &db);
    tracker.beginRun(0);
    CHECK(tracker.lifecycleGate() == LifecycleGate::Running);  // no arc-less start/restart

    const auto pause = net.findTransition("pause");
    const auto resume = net.findTransition("resume");
    CHECK(pause.has_value() && resume.has_value());
    const PlaceIndex run = *net.findPlace("run");
    const PlaceIndex paused = *net.findPlace("paused");

    CHECK(tracker.onEvent(stim("pause", 1)).status == StepStatus::Ok);
    CHECK(tracker.lifecycleGate() == LifecycleGate::Paused);
    CHECK(tracker.belief().size() == 1);
    CHECK(holdsPlace(rg, tracker.belief().front(), paused));
    CHECK(tracker.suggestedLifecycle() == *resume);
    {
        // Only lifecycle inputs are offered while paused -- not the ordinary
        // 'go', although the model still has it.
        const auto cands = tracker.enabledControllable();
        CHECK(candidateCount(cands) == 1);
        CHECK(lists(cands, net, "resume"));
        CHECK(!lists(cands, net, "go"));
    }

    // A silent SUT is expected while paused: quiescence is not a deviation,
    // a deadlock or a narrowing.
    {
        SutEvent q;
        q.kind = EventKind::Quiescent;
        q.timestampMs = 2;
        const StepResult r = tracker.onEvent(q);
        CHECK(r.status == StepStatus::Ok);
        CHECK(tracker.belief().size() == 1);
        CHECK(holdsPlace(rg, tracker.belief().front(), paused));
    }

    // An arc-less resume lands back at the pre-pause state, not at the paused
    // marking the model happened to be in.
    CHECK(tracker.onEvent(stim("resume", 3)).status == StepStatus::Ok);
    CHECK(tracker.lifecycleGate() == LifecycleGate::Running);
    CHECK(tracker.belief().size() == 1);
    CHECK(holdsPlace(rg, tracker.belief().front(), run));
    {
        const auto cands = tracker.enabledControllable();
        CHECK(lists(cands, net, "go"));
        CHECK(lists(cands, net, "pause"));
    }
    CHECK(db.transitionHits(*pause) == 1);
    CHECK(db.transitionHits(*resume) == 1);
    tracker.endRun(RunOutcome::Completed, "test");
}

void stopLeavesOnlyStartAndRestart()
{
    const Net net = makeStopModel();
    ReachabilityGraph rg(net);
    CoverageDb db(rg);
    BeliefTracker tracker(rg, &db);
    tracker.beginRun(0);
    CHECK(tracker.lifecycleGate() == LifecycleGate::Down);  // the model has an arc-less start

    const auto start = net.findTransition("start");
    const auto stop = net.findTransition("stop");
    const auto go = net.findTransition("go");
    CHECK(start.has_value() && stop.has_value() && go.has_value());
    const PlaceIndex run = *net.findPlace("run");
    const PlaceIndex done = *net.findPlace("done");

    CHECK(tracker.onEvent(stim("start", 1)).status == StepStatus::Ok);
    CHECK(tracker.lifecycleGate() == LifecycleGate::Running);
    CHECK(tracker.belief().size() == 1);
    CHECK(holdsPlace(rg, tracker.belief().front(), run));

    CHECK(tracker.onEvent(stim("stop", 2)).status == StepStatus::Ok);
    CHECK(tracker.lifecycleGate() == LifecycleGate::Down);
    CHECK(tracker.suggestedLifecycle() == *start);
    {
        // Nothing but the way back: no ordinary input, and no resume (the SUT
        // is down, not paused).
        const auto cands = tracker.enabledControllable();
        CHECK(candidateCount(cands) == 1);
        CHECK(lists(cands, net, "start"));
        CHECK(!lists(cands, net, "go"));
        CHECK(!lists(cands, net, "resume"));
    }

    CHECK(tracker.onEvent(stim("start", 3)).status == StepStatus::Ok);
    CHECK(tracker.lifecycleGate() == LifecycleGate::Running);
    CHECK(holdsPlace(rg, tracker.belief().front(), run));
    CHECK(tracker.onEvent(stim("go", 4)).status == StepStatus::Ok);
    CHECK(holdsPlace(rg, tracker.belief().front(), done));
    tracker.endRun(RunOutcome::Completed, "test");
}

// DeepSeek V4.1 Flash: the gate and the landings are engine state, but a run's
// event record carries them (the lifecycle words are ordinary stimuli). A
// replay through a fresh tracker must therefore rebuild the same coverage --
// including the engine-landed resume's credit -- on a fresh graph.
void aLifecycleRunReplaysToTheSameCoverage()
{
    const Net net = makePauseModel();
    ReachabilityGraph rg(net);
    CoverageDb db(rg);
    {
        BeliefTracker tracker(rg, &db);
        tracker.beginRun(0);
        CHECK(tracker.onEvent(stim("pause", 1)).status == StepStatus::Ok);
        CHECK(tracker.onEvent(stim("resume", 2)).status == StepStatus::Ok);
        CHECK(tracker.onEvent(stim("go", 3)).status == StepStatus::Ok);
        tracker.endRun(RunOutcome::Completed, "test");
    }
    const PersistedRun pr = toPersisted(rg, db.run(0));
    CHECK(!pr.events.empty());  // a live run records events, not a bare path

    ReachabilityGraph rg2(net);  // a fresh graph: same coverage, new state ids
    CoverageDb db2(rg2);
    const ReplayResult rr = replay(db2, rg2, pr);
    CHECK(rr.ok);
    if (rr.ok) {
        CHECK(db2.transitionHits(*net.findTransition("pause")) == 1);
        CHECK(db2.transitionHits(*net.findTransition("resume")) == 1);
        CHECK(db2.transitionHits(*net.findTransition("go")) == 1);
        const CoverageSummary a = db.summary();
        const CoverageSummary b = db2.summary();
        CHECK(a.statesCovered == b.statesCovered);
        CHECK(a.edgesCovered == b.edgesCovered);
        CHECK(a.transitionsCovered == b.transitionsCovered);
        CHECK(a.placesCovered == b.placesCovered);
    }

    // A path record (transition ids, no events) cannot express an engine-landed
    // step: there is no RG edge for it, so the replay refuses the run instead
    // of inventing a position. Documented in doku section 26.
    PersistedRun path = pr;
    path.events.clear();
    path.transitions = {"pause", "resume", "go"};
    ReachabilityGraph rg3(net);
    CoverageDb db3(rg3);
    const ReplayResult pr3 = replay(db3, rg3, path);
    CHECK(!pr3.ok);
    CHECK(pr3.error.find("not enabled") != std::string::npos);
    CHECK(db3.numRuns() == 0);  // nothing was recorded by the refused replay
}

void aCampaignPressesTheModelsStartBeforeItTests()
{
    const Net net = makeStartModel();
    SimulatedSutOptions options;
    options.seed = 1;
    options.stepDelayMs = 1;
    SimulatedSut sut(net, options);

    SessionConfig cfg;
    cfg.useAnts = false;
    cfg.analyzeModel = false;
    cfg.poll = std::chrono::milliseconds(5);
    cfg.idleTimeout = std::chrono::milliseconds(1000);
    cfg.stimulusAckTimeout = std::chrono::milliseconds(1000);
    cfg.plugin.quiescenceTimeoutMs = 100;
    cfg.maxEventsPerRun = 50;
    cfg.maxRuns = 1;

    Session session(net, cfg);
    const CampaignReport rep = session.runCampaign(sut);
    CHECK(rep.runs.size() == 1);
    if (!rep.runs.empty()) {
        const RunReport &r = rep.runs.front();
        CHECK(r.outcome == RunOutcome::Completed);
        CHECK(r.goodTarget == "finished");
        bool noted = false;
        for (const std::string &n : r.notes)
            if (n.find("started") != std::string::npos)
                noted = true;
        CHECK(noted);
    }
    const std::vector<std::string> sent = sut.stimuliReceived();
    CHECK(sent.size() >= 2);
    if (sent.size() >= 2) {
        CHECK(sent[0] == "start");  // the model's own start comes first
        CHECK(sent[1] == "go");
    }
    CHECK(rep.summary.transitionsCovered == 2);  // the lifecycle input is covered
}

}  // namespace

int main()
{
    const std::pair<const char *, void (*)()> tests[] = {
        {"theExactReservedWordsAreRecognized", theExactReservedWordsAreRecognized},
        {"anArcLessStartIsEngineLandedAndOpensTheGate", anArcLessStartIsEngineLandedAndOpensTheGate},
        {"pauseSuspendsTheRunAndResumeLandsWhereItStopped", pauseSuspendsTheRunAndResumeLandsWhereItStopped},
        {"stopLeavesOnlyStartAndRestart", stopLeavesOnlyStartAndRestart},
        {"aLifecycleRunReplaysToTheSameCoverage", aLifecycleRunReplaysToTheSameCoverage},
        {"aCampaignPressesTheModelsStartBeforeItTests", aCampaignPressesTheModelsStartBeforeItTests},
    };
    for (const auto &t : tests) {
        const int before = g_failures;
        t.second();
        std::printf("%s %s\n", g_failures == before ? "PASS" : "FAIL", t.first);
    }
    std::printf("%d failure(s)\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
