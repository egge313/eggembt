// DeepSeek V4.1 Flash: the run-size benchmark (doku section 32).
//
// Four communicating state machines share a ring of one-place channels. Each
// machine has three working phases plus one goal state and one error state,
// and one goal/one error marking of its own. With four channels the reachable
// state space is exactly 5^4 * 2^4 = 10,000 states and 64,000 edges: big
// enough that a ten-minute campaign cannot finish it, small enough to run.
//
// The benchmark asks a practical question: for a fixed wall-clock budget, is
// it better to make many short runs or fewer long ones? The test therefore
// varies --max-events (the run length) and keeps everything else fixed. The
// default CTest mode runs a short, structural smoke test; --long runs the
// ten-minute measurement, and --run-events runs one configuration so several
// campaigns can be started in parallel.
//
// The SUT is a synchronous, deterministic plug-in (it applies a controllable
// input inside stimulate() and pushes the event at once), so the measurement
// is about the engine's guidance and the run boundary, not SUT timing.
#include "engine/ReachabilityGraph.h"
#include "engine/Session.h"
#include "engine/SutPlugin.h"
#include "model/NetBuilder.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
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

namespace {

// DeepSeek V4.1 Flash: a deterministic SUT that behaves exactly as the
// controllable part of the model says, in the caller's thread.
class SynchronousSut : public ISutPlugin {
public:
    explicit SynchronousSut(const Net &net) : net_(net) {}

    PluginCapabilities capabilities() const override { return {}; }

    bool startRun(const std::string &initialMarking, const PluginRunOptions &, EventQueue &events,
                  std::string *error) override
    {
        const NamedMarking *m = nullptr;
        for (const NamedMarking &c : net_.initialMarkings)
            if (c.name == initialMarking)
                m = &c;
        if (!m) {
            if (error)
                *error = "no initial marking '" + initialMarking + "'";
            return false;
        }
        marking_.assign(net_.places.size(), 0);
        for (const TokenEntry &e : m->tokens)
            marking_[e.place] = static_cast<int>(e.tokens);
        events_ = &events;
        clock_ = 0;
        return true;
    }

    bool stimulate(const std::string &label, std::string *error) override
    {
        TransitionIndex found = kNoTransition;
        for (std::size_t i = 0; i < net_.transitions.size(); ++i)
            if (net_.transitions[i].label == label) {
                found = static_cast<TransitionIndex>(i);
                break;
            }
        if (found == kNoTransition || !isEnabled(net_.transitions[found], marking_)) {
            if (error)
                *error = "'" + label + "' is not enabled";
            return false;
        }
        fire(net_.transitions[found]);
        push(EventKind::Stimulated, label);
        return true;
    }

    PostMortem postMortem(DumpMode) override { return {}; }
    void stopRun(StopReason) override { events_ = nullptr; }
    std::string pollFault() override { return {}; }

private:
    static bool isEnabled(const Transition &t, const std::vector<int> &marking)
    {
        for (const WeightedArc &a : t.pre)
            if (marking[a.place] < static_cast<int>(a.weight))
                return false;
        for (const WeightedArc &a : t.inhibit)
            if (marking[a.place] >= static_cast<int>(a.weight))
                return false;
        return true;
    }

    void fire(const Transition &t)
    {
        for (const WeightedArc &a : t.pre)
            marking_[a.place] -= static_cast<int>(a.weight);
        for (const WeightedArc &a : t.post)
            marking_[a.place] += static_cast<int>(a.weight);
    }

    void push(EventKind kind, const std::string &label)
    {
        if (events_)
            events_->push(SutEvent{kind, label, static_cast<std::int64_t>(++clock_)});
    }

    const Net &net_;
    std::vector<int> marking_;
    EventQueue *events_ = nullptr;
    std::uint64_t clock_ = 0;
};

// DeepSeek V4.1 Flash: the 10,000-state model (doku section 32).
//
// Machine i runs  p0 -step-> p1 -step-> p2 -pass-> p0;  the "send" edge
// shortcuts p1 -> p2 and drops a token into channel i (only while the
// channel is empty), and the receive edge p2 -recv-> p0 takes a token from
// channel i-1. The two alternatives out of p2 -- pass and recv -- make the
// ring strongly connected. From p1 the machine may instead enter its goal
// state and return; from p0 it may enter its error state and recover. The
// four channels are the communication: a message can wait in a channel while
// the sender moves on, and any machine can take it when it comes round
// again. Because the step from p1 to p2 does not depend on the channel,
// every combination of machine places and channel occupancy is reachable:
// 5^4 * 2^4 = 10,000 states.
Net makeCommunicatingNet()
{
    constexpr int kMachines = 4;
    NetBuilder b("comm-10k");
    std::vector<std::vector<PlaceIndex>> p(kMachines);
    std::vector<PlaceIndex> goal(kMachines), err(kMachines), ch(kMachines);
    for (int i = 0; i < kMachines; ++i) {
        for (int s = 0; s < 3; ++s)
            p[i].push_back(b.place("m" + std::to_string(i) + "s" + std::to_string(s)));
        goal[i] = b.place("m" + std::to_string(i) + "goal");
        err[i] = b.place("m" + std::to_string(i) + "err");
        ch[i] = b.place("ch" + std::to_string(i), 1);
    }
    for (int i = 0; i < kMachines; ++i) {
        const std::string pre = "m" + std::to_string(i);
        const auto step = b.transition(pre + "step", TransitionKind::Controllable, pre + ".step");
        b.input(p[i][0], step).output(step, p[i][1]);
        // DeepSeek V4.1 Flash: the free step p1 -> p2 is what makes every
        // channel occupancy reachable; only the send shortcut needs an empty
        // channel.
        const auto step2 = b.transition(pre + "step2", TransitionKind::Controllable, pre + ".step2");
        b.input(p[i][1], step2).output(step2, p[i][2]);
        const auto send = b.transition(pre + "send", TransitionKind::Controllable, pre + ".send");
        b.input(p[i][1], send).output(send, p[i][2]);
        b.output(send, ch[i]);
        b.inhibitor(ch[i], send);
        const auto recv = b.transition(pre + "recv", TransitionKind::Controllable, pre + ".recv");
        b.input(p[i][2], recv).output(recv, p[i][0]);
        b.input(ch[(i + kMachines - 1) % kMachines], recv);
        const auto pass = b.transition(pre + "pass", TransitionKind::Controllable, pre + ".pass");
        b.input(p[i][2], pass).output(pass, p[i][0]);
        const auto finish = b.transition(pre + "finish", TransitionKind::Controllable, pre + ".finish");
        b.input(p[i][1], finish).output(finish, goal[i]);
        const auto again = b.transition(pre + "again", TransitionKind::Controllable, pre + ".again");
        b.input(goal[i], again).output(again, p[i][0]);
        const auto fault = b.transition(pre + "fault", TransitionKind::Controllable, pre + ".fault");
        b.input(p[i][0], fault).output(fault, err[i]);
        const auto recover = b.transition(pre + "recover", TransitionKind::Controllable, pre + ".recover");
        b.input(err[i], recover).output(recover, p[i][0]);
    }
    b.initial("start", {{p[0][0], 1}, {p[1][0], 1}, {p[2][0], 1}, {p[3][0], 1}});
    for (int i = 0; i < kMachines; ++i) {
        b.targetMarking("goal" + std::to_string(i), {{goal[i], 1}}, MatchMode::Covers);
        b.errorMarking("err" + std::to_string(i), {{err[i], 1}}, MatchMode::Covers);
    }
    return b.build();
}

struct CampaignOptions {
    std::size_t runEvents = 200;
    std::size_t maxRuns = 1000000;
    std::chrono::milliseconds budget{600000};
    std::uint64_t seed = 1;
    bool ants = true;
    std::size_t tsuffix = 4;
};

struct Outcome {
    std::size_t runs = 0;
    std::size_t events = 0;
    std::size_t stimuli = 0;
    std::size_t statesCovered = 0;
    std::size_t edgesCovered = 0;
    std::size_t errorsSeen = 0;
    std::size_t longestRun = 0;
    bool complete = false;
    std::string stopReason;
    std::int64_t wallMs = 0;
};

Outcome runCampaign(const Net &net, const CampaignOptions &opts)
{
    SynchronousSut sut(net);
    SessionConfig cfg;
    cfg.useAnts = opts.ants;
    cfg.ants.seed = opts.seed;
    cfg.ants.tsuffixLength = opts.tsuffix;
    cfg.selectionSeed = opts.seed;
    cfg.poll = std::chrono::milliseconds(0);
    cfg.idleTimeout = std::chrono::milliseconds(1000);
    cfg.stimulusAckTimeout = std::chrono::milliseconds(1000);
    cfg.maxEventsPerRun = opts.runEvents;
    cfg.maxRuns = opts.maxRuns;
    cfg.timeBudget = opts.budget;
    cfg.analyzeModel = true;
    // DeepSeek V4.1 Flash: keep the phase veto from reshaping the model after
    // the per-process goals are satisfied. All run sizes then face the same
    // coverage task; the target markings are still ordinary goals while they
    // are sought.
    cfg.suggester.satisfiedTargets = SatisfiedTargetPolicy::Ignore;

    Session session(net, cfg);
    const auto t0 = std::chrono::steady_clock::now();
    const CampaignReport rep = session.runCampaign(sut);

    Outcome o;
    o.runs = rep.runs.size();
    o.statesCovered = rep.summary.statesCovered;
    o.edgesCovered = rep.summary.edgesCovered;
    o.complete = rep.complete;
    o.stopReason = rep.stopReason;
    o.wallMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now() - t0)
                   .count();
    std::set<std::string> errors;
    for (const RunReport &r : rep.runs) {
        o.events += r.events;
        o.stimuli += r.stimuli;
        o.longestRun = std::max(o.longestRun, r.events);
        for (const std::string &e : r.errorsReached)
            errors.insert(e);
    }
    o.errorsSeen = errors.size();
    return o;
}

void printRow(std::size_t runEvents, std::uint64_t seed, std::size_t budgetMs, const Outcome &o)
{
    std::printf("campaign run_events=%zu seed=%llu budget_ms=%zu runs=%zu events=%zu stimuli=%zu "
                "states_covered=%zu edges_covered=%zu errors_seen=%zu longest_run=%zu complete=%d "
                "wall_ms=%lld stop=\"%s\"\n",
                runEvents, (unsigned long long)seed, budgetMs, o.runs, o.events, o.stimuli,
                o.statesCovered, o.edgesCovered, o.errorsSeen, o.longestRun, o.complete ? 1 : 0,
                (long long)o.wallMs, o.stopReason.c_str());
    std::fflush(stdout);
}

void graphStats(const Net &net, std::size_t &states, std::size_t &edges)
{
    ReachabilityGraph rg(net);
    rg.addInitial(0);
    rg.exploreAll();
    states = rg.numStates();
    edges = 0;
    for (StateId s = 0; s < rg.numStates(); ++s)
        if (rg.isExpanded(s))
            edges += rg.successors(s).size();
}

// DeepSeek V4.1 Flash: the default CTest check. It pins the model's size and
// the one structural fact every run-size experiment relies on: for the same
// event budget, a smaller per-run event limit really does produce more runs.
// The check deliberately uses the run limit, not the wall clock, so it does
// not depend on the speed of the machine. The coverage comparison itself is
// the long measurement, not an assertion here.
void testTheCommunicatingModelAndShorterRuns()
{
    const Net net = makeCommunicatingNet();
    std::size_t states = 0, edges = 0;
    graphStats(net, states, edges);
    CHECK(net.places.size() == 24);
    CHECK(net.transitions.size() == 36);
    CHECK(net.initialMarkings.size() == 1);
    CHECK(net.targetMarkings.size() == 4);
    CHECK(net.errorMarkings.size() == 4);
    CHECK(states == 10000);
    CHECK(edges == 64000);

    // Same budget: three runs of 10 events at most (30 events) versus one run
    // of 30 events at most.
    CampaignOptions shortRuns;
    shortRuns.runEvents = 10;
    shortRuns.maxRuns = 3;
    shortRuns.budget = std::chrono::milliseconds(0);
    const Outcome a = runCampaign(net, shortRuns);

    CampaignOptions longRuns = shortRuns;
    longRuns.runEvents = 30;
    longRuns.maxRuns = 1;
    const Outcome b = runCampaign(net, longRuns);

    std::printf("comm-10k smoke: %zu states, %zu edges, %zu targets, %zu errors\n", states, edges,
                net.targetMarkings.size(), net.errorMarkings.size());
    printRow(shortRuns.runEvents, shortRuns.seed,
             static_cast<std::size_t>(shortRuns.budget.count()), a);
    printRow(longRuns.runEvents, longRuns.seed,
             static_cast<std::size_t>(longRuns.budget.count()), b);

    CHECK(a.stopReason == "run limit reached");
    CHECK(b.stopReason == "run limit reached");
    CHECK(a.runs == 3);
    CHECK(b.runs == 1);
    CHECK(a.runs > b.runs);          // the same event budget bought more runs
    CHECK(a.events > 0 && b.events > 0);
    CHECK(a.events <= 3 * shortRuns.runEvents);
    CHECK(b.events <= longRuns.runEvents);
    CHECK(a.edgesCovered > 0 && b.edgesCovered > 0);
    CHECK(a.longestRun <= shortRuns.runEvents);
    CHECK(b.longestRun <= longRuns.runEvents);
}

void runSweep(const Net &net, const std::vector<std::size_t> &sizes, std::uint64_t seed,
              std::chrono::milliseconds budget, bool ants, std::size_t tsuffix)
{
    std::printf("comm-10k sweep: budget %lld ms, seed %llu, ants %s, tsuffix %zu\n",
                (long long)budget.count(), (unsigned long long)seed, ants ? "on" : "off", tsuffix);
    for (std::size_t n : sizes) {
        CampaignOptions opts;
        opts.runEvents = n;
        opts.budget = budget;
        opts.seed = seed;
        opts.ants = ants;
        opts.tsuffix = tsuffix;
        const Outcome o = runCampaign(net, opts);
        printRow(n, seed, static_cast<std::size_t>(budget.count()), o);
    }
}

void usage()
{
    std::printf("usage: tst_comm10k [--budget SECONDS] [--run-events N] [--seed N]\n"
                "                   [--long] [--no-ants] [--tsuffix N]\n"
                "       no arguments: the quick CTest smoke test\n"
                "       --long:      four campaigns, 50/200/800/3200 events per run\n"
                "       --run-events N --budget S: one campaign, for a parallel sweep\n");
}

}  // namespace

int main(int argc, char **argv)
{
    bool longMode = false;
    bool selfTest = argc == 1;
    bool ants = true;
    std::size_t tsuffix = 4;
    std::size_t singleRunEvents = 0;
    std::chrono::seconds budget{600};
    std::uint64_t seed = 1;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--long") {
            longMode = true;
        } else if (arg == "--no-ants") {
            ants = false;
        } else if (arg == "--budget" && i + 1 < argc) {
            budget = std::chrono::seconds(std::strtoll(argv[++i], nullptr, 10));
        } else if (arg == "--run-events" && i + 1 < argc) {
            singleRunEvents = static_cast<std::size_t>(std::strtoull(argv[++i], nullptr, 10));
        } else if (arg == "--seed" && i + 1 < argc) {
            seed = std::strtoull(argv[++i], nullptr, 10);
        } else if (arg == "--tsuffix" && i + 1 < argc) {
            tsuffix = static_cast<std::size_t>(std::strtoull(argv[++i], nullptr, 10));
        } else if (arg == "--help") {
            usage();
            return 0;
        } else {
            usage();
            return 2;
        }
    }
    if (budget.count() <= 0) {
        usage();
        return 2;
    }

    const Net net = makeCommunicatingNet();
    std::size_t states = 0, edges = 0;
    graphStats(net, states, edges);
    std::printf("model comm-10k: %zu places, %zu transitions, %zu states, %zu edges, "
                "%zu targets, %zu errors\n",
                net.places.size(), net.transitions.size(), states, edges,
                net.targetMarkings.size(), net.errorMarkings.size());

    if (selfTest) {
        testTheCommunicatingModelAndShorterRuns();
        std::printf("%s theCommunicatingModelAndShorterRuns\n", g_failures == 0 ? "PASS" : "FAIL");
        std::printf("%d failure(s)\n", g_failures);
        return g_failures == 0 ? 0 : 1;
    }
    if (singleRunEvents > 0) {
        CampaignOptions opts;
        opts.runEvents = singleRunEvents;
        opts.budget = std::chrono::duration_cast<std::chrono::milliseconds>(budget);
        opts.seed = seed;
        opts.ants = ants;
        opts.tsuffix = tsuffix;
        const Outcome o = runCampaign(net, opts);
        printRow(singleRunEvents, seed, static_cast<std::size_t>(opts.budget.count()), o);
        return 0;
    }
    if (longMode) {
        runSweep(net, {50, 200, 800, 3200}, seed,
                 std::chrono::duration_cast<std::chrono::milliseconds>(budget), ants, tsuffix);
        return 0;
    }
    usage();
    return 2;
}
