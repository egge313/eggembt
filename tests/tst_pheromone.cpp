// DeepSeek V4.1 Flash: the pheromone benchmark. One campaign shape, run with
// and without the ant colony, shows what the guidance buys: a large model
// (1000 reachable states, 2016 edges) whose only way into the region under
// test is a fork, where the one-step way crashes the SUT and the two-step way
// is safe. The campaign starts a new run at the fork every time, so the
// catastrophe lesson has to survive from one run to the next.
//
// The measured effect, and the reason this test exists (doku section 25):
//   without the colony the campaign explores the region once and then walks
//   into the crashing shortcut at the start of every following run; with the
//   colony's edge pheromone alone nothing changes, because a sought crash is
//   attractive while it is still being reproduced and the attract saturates
//   the channel; with the (marking, T-suffix) pair layer on, the catastrophe
//   pair keeps the lesson that the still-sought target's attract cannot
//   overwrite, and the campaign alternates safe run / crash instead of
//   crashing every time -- roughly three times the coverage for the same run
//   budget.
//
// The second benchmark (DeepSeek V4.1 Flash) runs the same model with and
// without --evaporate-between-runs: per-suggestion aging largely forgets a
// lesson within a long run, aging at run boundaries keeps it (doku section 27).
//
// The SUT is a synchronous plug-in (it applies the input while stimulate()
// runs and pushes its events at once): no thread and no SUT-side moves, so the
// benchmark measures the engine's own guidance work. The model is
// deterministic, so every number below is reproducible; the campaign seeds
// vary the seeded tie-breaks, and the graph seed varies the region's skip
// edge. The benchmark is deliberately fast (about six seconds): a larger
// graph measures the same thing for minutes (a 10000-state run measured 1994
// covered edges with the pair layer against 1001 without any, 4 runs of 1000).
#include "engine/Session.h"
#include "engine/SutPlugin.h"
#include "model/NetBuilder.h"

#include <chrono>
#include <cstdio>
#include <map>
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

// A plug-in that behaves exactly as the control part of the model says, in the
// caller's thread: the input is applied inside stimulate(), the Stimulated
// event is pushed at once, and 'mine' kills the SUT after the acknowledgement
// (the crash a real SUT would suffer). No own moves, no quiescence report.
class SynchronousSut : public ISutPlugin {
public:
    SynchronousSut(const Net &net, std::map<std::string, double> crashAt)
        : net_(net), crashAt_(std::move(crashAt))
    {
    }

    PluginCapabilities capabilities() const override
    {
        PluginCapabilities c;
        c.crash = true;
        return c;
    }

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
        dead_ = false;
        clock_ = 0;
        return true;
    }

    bool stimulate(const std::string &label, std::string *error) override
    {
        if (dead_) {
            if (error)
                *error = "the SUT is down";
            return false;
        }
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
        if (crashAt_.count(label) && crashAt_.at(label) > 0.0) {
            dead_ = true;
            push(EventKind::Died, "crashed while handling stimulus '" + label + "'");
        }
        return true;
    }

    PostMortem postMortem(DumpMode mode) override
    {
        PostMortem pm;
        pm.ok = mode != DumpMode::None;
        pm.files.push_back("synchronous-dump");
        return pm;
    }

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
    std::map<std::string, double> crashAt_;
    std::vector<int> marking_;
    EventQueue *events_ = nullptr;
    std::uint64_t clock_ = 0;
    bool dead_ = false;
};

// The scenario. A five-step corridor leads to a fork F:
//   F --mine--> B   one step, and the SUT dies handling it;
//   F --safe1-> C --safe2-> B   two steps, safe.
// Behind B lies the region (a ring with a skip edge), the part of the model
// the campaign has to cover. Every run starts in the corridor, so every run
// faces the same fork with the same memory.
Net makeDiamondNet(int region, std::uint64_t graphSeed)
{
    NetBuilder b("pheromone-benchmark");
    std::vector<PlaceIndex> corridor;
    for (int i = 0; i < 5; ++i)
        corridor.push_back(b.place("p" + std::to_string(i)));
    const auto fork = b.place("F");
    const auto safeWay = b.place("C");
    const auto inner = b.place("B");

    for (int i = 0; i < 4; ++i) {
        const auto t = b.transition("corr" + std::to_string(i), TransitionKind::Controllable,
                                    "corr" + std::to_string(i));
        b.input(corridor[static_cast<std::size_t>(i)], t)
            .output(t, corridor[static_cast<std::size_t>(i) + 1]);
    }
    const auto last = b.transition("corr4", TransitionKind::Controllable, "corr4");
    b.input(corridor[4], last).output(last, fork);

    const auto mine = b.transition("mine", TransitionKind::Controllable, "mine");
    b.input(fork, mine).output(mine, inner);
    const auto safe1 = b.transition("safe1", TransitionKind::Controllable, "safe1");
    b.input(fork, safe1).output(safe1, safeWay);
    const auto safe2 = b.transition("safe2", TransitionKind::Controllable, "safe2");
    b.input(safeWay, safe2).output(safe2, inner);

    std::vector<PlaceIndex> r;
    for (int i = 0; i < region; ++i)
        r.push_back(b.place("r" + std::to_string(i)));
    const auto in0 = b.transition("in0", TransitionKind::Controllable, "in0");
    b.input(inner, in0).output(in0, r[0]);
    const auto in1 = b.transition("in1", TransitionKind::Controllable, "in1");
    b.input(inner, in1).output(in1, r[1 % static_cast<std::size_t>(region)]);
    const int skipBy = 2 + static_cast<int>(graphSeed % 13);
    for (int i = 0; i < region; ++i) {
        const auto ring = b.transition("r" + std::to_string(i) + "_ring", TransitionKind::Controllable,
                                       "r" + std::to_string(i) + "_ring");
        b.input(r[static_cast<std::size_t>(i)], ring)
            .output(ring, r[static_cast<std::size_t>((i + 1) % region)]);
        const auto skip = b.transition("r" + std::to_string(i) + "_skip", TransitionKind::Controllable,
                                       "r" + std::to_string(i) + "_skip");
        b.input(r[static_cast<std::size_t>(i)], skip)
            .output(skip, r[static_cast<std::size_t>((i + skipBy) % region)]);
    }

    b.initial("start", {{corridor[0], 1}});
    // Non-intentional targets are legitimate ends, not campaign goals: they
    // keep the corridor and the region out of the livelock and nontarget-region
    // analyses, which would otherwise veto the very moves under test.
    b.targetMarking("idle", {{corridor[0], 1}});
    b.targetMarking("region-idle", {{r[0], 1}});
    return b.build();
}

struct Guidance {
    bool ants = false;
    std::size_t tsuffix = 0;
    const char *name = "";
    bool betweenRuns = false;  // DeepSeek V4.1 Flash
};

struct Outcome {
    std::size_t runs = 0;
    std::size_t stimuli = 0;
    std::size_t crashes = 0;
    std::size_t covered = 0;
    std::size_t longRuns = 0;  // runs that reached at least half the event budget
    std::int64_t wallMs = 0;
};

Outcome runCampaign(const Net &net, const Guidance &guidance, std::uint64_t seed, int maxEvents, int maxRuns)
{
    SynchronousSut sut(net, std::map<std::string, double>{{"mine", 1.0}});

    SessionConfig cfg;
    cfg.useAnts = guidance.ants;
    cfg.ants.seed = seed;
    cfg.ants.tsuffixLength = guidance.tsuffix;
    cfg.selectionSeed = seed;
    cfg.coverage.crashAttempts = 2;
    cfg.poll = std::chrono::milliseconds(0);
    cfg.idleTimeout = std::chrono::milliseconds(1000);
    cfg.stimulusAckTimeout = std::chrono::milliseconds(1000);
    cfg.maxEventsPerRun = static_cast<std::size_t>(maxEvents);
    cfg.maxRuns = static_cast<std::size_t>(maxRuns);
    cfg.analyzeModel = true;
    cfg.evaporateBetweenRuns = guidance.betweenRuns;  // DeepSeek V4.1 Flash

    Session session(net, cfg);
    const auto t0 = std::chrono::steady_clock::now();
    const CampaignReport rep = session.runCampaign(sut);
    Outcome o;
    o.runs = rep.runs.size();
    o.wallMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    for (const RunReport &r : rep.runs) {
        o.stimuli += r.stimuli;
        if (r.outcome == RunOutcome::SutCrashed)
            ++o.crashes;
        if (r.events >= static_cast<std::size_t>(maxEvents) / 2)
            ++o.longRuns;
    }
    o.covered = rep.summary.edgesCovered;
    return o;
}

// DeepSeek V4.1 Flash: the benchmark. The assertions hold for every campaign
// seed on this model, not as an average: the effect is structural (the fork
// is faced afresh by every run), not a lucky sample.
void testThePheromonesKeepTheCampaignOffACrashingShortcut()
{
    constexpr int kRegion = 1000;
    constexpr int kMaxEvents = 200;
    constexpr int kMaxRuns = 6;
    const std::uint64_t kGraphSeed = 7;
    const Net net = makeDiamondNet(kRegion, kGraphSeed);

    struct Sample {
        std::uint64_t seed;
        Guidance guidance;
        Outcome outcome;
    };
    std::vector<Sample> samples;
    for (std::uint64_t seed : {1ull, 2ull, 3ull}) {
        for (const Guidance &g : {Guidance{false, 0, "no ants"},
                                  Guidance{true, 0, "ants, no T-suffix"},
                                  Guidance{true, 4, "ants, T-suffix 4"}}) {
            samples.push_back({seed, g, runCampaign(net, g, seed, kMaxEvents, kMaxRuns)});
        }
    }

    std::printf("pheromone benchmark: %d states, %d edges, %d runs of %d events, graph seed %llu\n",
                static_cast<int>(net.places.size()), static_cast<int>(net.transitions.size()), kMaxRuns,
                kMaxEvents, (unsigned long long)kGraphSeed);
    for (const Sample &s : samples)
        std::printf("  seed %llu %-16s runs %zu crashes %zu long %zu covered %zu stimuli %zu (%lld ms)\n",
                    (unsigned long long)s.seed, s.guidance.name, s.outcome.runs, s.outcome.crashes,
                    s.outcome.longRuns, s.outcome.covered, s.outcome.stimuli, (long long)s.outcome.wallMs);

    for (std::uint64_t seed : {1ull, 2ull, 3ull}) {
        const Outcome *plain = nullptr;
        const Outcome *edge = nullptr;
        const Outcome *pair = nullptr;
        for (const Sample &s : samples) {
            if (s.seed != seed)
                continue;
            if (!s.guidance.ants)
                plain = &s.outcome;
            else if (s.guidance.tsuffix == 0)
                edge = &s.outcome;
            else
                pair = &s.outcome;
        }
        CHECK(plain && edge && pair);
        if (!plain || !edge || !pair)
            continue;

        // Without the pair layer the campaign explores the region once and
        // then crashes at the start of every following run: one long run of
        // the six, five crashes, and only the first run's coverage.
        CHECK(plain->longRuns == 1);
        CHECK(plain->crashes == static_cast<std::size_t>(kMaxRuns - 1));
        CHECK(edge->covered <= plain->covered);
        CHECK(edge->crashes >= plain->crashes);

        // With both layers the catastrophe pair survives the still-sought
        // target's attract: the campaign alternates a safe run and a crash,
        // so half the runs do real work and the coverage more than doubles.
        CHECK(pair->longRuns >= plain->longRuns + 2);
        CHECK(pair->crashes <= plain->crashes - 2);
        CHECK(pair->covered >= 2 * plain->covered);
        CHECK(pair->covered > 2 * edge->covered);
        CHECK(pair->stimuli >= 2 * plain->stimuli);
    }
}

// DeepSeek V4.1 Flash: does evaporation ruin progress (doku section 27)?
// The same benchmark, twice: once with the historical per-suggestion aging
// and once with --evaporate-between-runs. The default halves a lesson every
// ~34 suggestions (0.98 per suggestion), so during a run of hundreds of
// suggestions a lesson from its start is almost gone by its end; at a run
// boundary one run costs one 2% step. The measured effect below is
// structural on this model: the per-suggestion campaign crashes at the fork
// three times in six runs and covers about 586 edges, the between-runs
// campaign crashes twice, keeps four runs productive and covers about 777.
void testEvaporationBetweenRunsKeepsProgress()
{
    constexpr int kRegion = 1000;
    constexpr int kMaxEvents = 200;
    constexpr int kMaxRuns = 6;
    const std::uint64_t kGraphSeed = 7;
    const Net net = makeDiamondNet(kRegion, kGraphSeed);

    std::printf("evaporation benchmark: %d runs of %d events, graph seed %llu\n", kMaxRuns, kMaxEvents,
                (unsigned long long)kGraphSeed);
    for (std::uint64_t seed : {1ull, 2ull}) {
        const Guidance perSuggestion{true, 4, "per-suggestion", false};
        const Guidance betweenRuns{true, 4, "between runs", true};
        const Outcome per = runCampaign(net, perSuggestion, seed, kMaxEvents, kMaxRuns);
        const Outcome between = runCampaign(net, betweenRuns, seed, kMaxEvents, kMaxRuns);
        std::printf("  seed %llu per-suggestion: crashes %zu long %zu covered %zu (%lld ms)\n",
                    (unsigned long long)seed, per.crashes, per.longRuns, per.covered, (long long)per.wallMs);
        std::printf("  seed %llu between runs:   crashes %zu long %zu covered %zu (%lld ms)\n",
                    (unsigned long long)seed, between.crashes, between.longRuns, between.covered,
                    (long long)between.wallMs);

        CHECK(per.runs == between.runs);  // both hit the same run budget
        CHECK(between.crashes < per.crashes);
        CHECK(between.longRuns > per.longRuns);
        CHECK(between.covered >= per.covered + 100);
        CHECK(between.stimuli > per.stimuli);
    }
}

}  // namespace

int main()
{
    const std::pair<const char *, void (*)()> tests[] = {
        {"thePheromonesKeepTheCampaignOffACrashingShortcut",
         testThePheromonesKeepTheCampaignOffACrashingShortcut},
        {"evaporationBetweenRunsKeepsProgress", testEvaporationBetweenRunsKeepsProgress},  // DeepSeek V4.1 Flash
    };
    for (const auto &t : tests) {
        const int before = g_failures;
        t.second();
        std::printf("%s %s\n", g_failures == before ? "PASS" : "FAIL", t.first);
    }
    std::printf("%d failure(s)\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
