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
// The third (DeepSeek V4.1 Flash) sweeps the evaporation rates themselves and
// pins the measured plateau: 0.005 to 0.10 per suggestion give the same
// outcome here, because the fork is reached about five suggestions after the
// deposit; the run boundary still matters (doku section 31).
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

#include <algorithm>  // DeepSeek V4.1 Flash: the evaporation sweep's maxStimuli
#include <chrono>
#include <cmath>  // DeepSeek V4.1 Flash: the retention figures in the sweep
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
    // keep the corridor and the region out of the livelock and off-target-region
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
    // DeepSeek V4.1 Flash: the evaporation sweep (doku section 31).
    double evaporationAttract = 0.10;
    double evaporationRepel = 0.02;
};

struct Outcome {
    std::size_t runs = 0;
    std::size_t stimuli = 0;
    std::size_t crashes = 0;
    std::size_t covered = 0;
    std::size_t longRuns = 0;  // runs that reached at least half the event budget
    std::size_t maxStimuli = 0;  // suggestions in the longest run (DeepSeek V4.1 Flash)
    std::int64_t wallMs = 0;
};

Outcome runCampaign(const Net &net, const Guidance &guidance, std::uint64_t seed, int maxEvents, int maxRuns)
{
    SynchronousSut sut(net, std::map<std::string, double>{{"mine", 1.0}});

    SessionConfig cfg;
    cfg.useAnts = guidance.ants;
    cfg.ants.seed = seed;
    cfg.ants.tsuffixLength = guidance.tsuffix;
    // DeepSeek V4.1 Flash: the evaporation sweep (doku section 31).
    cfg.ants.evaporationAttract = guidance.evaporationAttract;
    cfg.ants.evaporationRepel = guidance.evaporationRepel;
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
        o.maxStimuli = std::max(o.maxStimuli, r.stimuli);  // DeepSeek V4.1 Flash: the run interval
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

// DeepSeek V4.1 Flash: the evaporation sweep (doku section 31). The endpoints
// were pinned above (2 % per suggestion, 2 % per run); this one moves the
// parameters gradually and records what the benchmark actually does.
//
// Measured, and now asserted: the per-suggestion repel rate from 0.005 to 0.10
// does not move the outcome at all -- 3 crashes and 3 productive runs of 6,
// about 586 covered edges -- even though the nominal retention over a 200-event
// run falls from 37 % to essentially zero. The fork is reached about five
// suggestions after a run starts, and the lesson was laid at the previous run's
// end, so only that short interval has to be survived, not the whole run. Two
// overlapping protections carry it: the (marking, T-suffix) pair repel and the
// attract of the safe route around the crash. The run boundary still matters
// (2 crashes, 4 productive runs, about 777 edges), and the same rate x run
// length keeps the same outcome. A cliff would need a much longer interval
// between the deposit and the fork, or a much weaker deposit; that is a
// calibration this benchmark deliberately does not manufacture.
void testEvaporationParametersSitOnAPlateau()
{
    constexpr int kRegion = 1000;
    constexpr int kMaxEvents = 200;
    constexpr int kMaxRuns = 6;
    const std::uint64_t kGraphSeed = 7;
    const Net net = makeDiamondNet(kRegion, kGraphSeed);
    const double kRates[] = {0.005, 0.01, 0.02, 0.05, 0.10};
    const std::uint64_t kSeeds[] = {1, 2};

    std::printf("evaporation sweep: %d runs of %d events, graph seed %llu, tsuffix 4\n", kMaxRuns,
                kMaxEvents, (unsigned long long)kGraphSeed);

    struct Row {
        double rate;
        bool betweenRuns;
        std::uint64_t seed;
        Outcome outcome;
    };
    std::vector<Row> rows;

    for (std::uint64_t seed : kSeeds) {
        for (double rate : kRates) {
            Guidance g;
            g.ants = true;
            g.tsuffix = 4;
            g.name = "per suggestion";
            g.evaporationRepel = rate;
            rows.push_back({rate, false, seed, runCampaign(net, g, seed, kMaxEvents, kMaxRuns)});
        }
        for (double rate : {0.02, 0.10}) {
            Guidance g;
            g.ants = true;
            g.tsuffix = 4;
            g.name = "between runs";
            g.betweenRuns = true;
            g.evaporationRepel = rate;
            rows.push_back({rate, true, seed, runCampaign(net, g, seed, kMaxEvents, kMaxRuns)});
        }
    }

    for (const Row &r : rows) {
        const double retention = std::pow(1.0 - r.rate, r.betweenRuns ? 1.0 : static_cast<double>(r.outcome.maxStimuli));
        std::printf("  rate %.3f %-14s seed %llu crashes %zu long %zu covered %zu maxStimuli %zu retention %.4f\n",
                    r.rate, r.betweenRuns ? "between runs" : "per suggestion", (unsigned long long)r.seed,
                    r.outcome.crashes, r.outcome.longRuns, r.outcome.covered, r.outcome.maxStimuli, retention);
    }

    // The scaling pair: the same per-run loss of repel, twice the run length.
    struct ScaleRow {
        double rate;
        int events;
        std::uint64_t seed;
        Outcome outcome;
    };
    std::vector<ScaleRow> scaling;
    std::printf("evaporation scaling: (rate, events)\n");
    for (std::uint64_t seed : kSeeds) {
        for (const std::pair<double, int> &p : {std::pair<double, int>{0.04, 100}, std::pair<double, int>{0.02, 200}}) {
            Guidance g;
            g.ants = true;
            g.tsuffix = 4;
            g.name = "scaling";
            g.evaporationRepel = p.first;
            const Outcome o = runCampaign(net, g, seed, p.second, kMaxRuns);
            std::printf("  rate %.3f events %d seed %llu crashes %zu long %zu covered %zu maxStimuli %zu\n",
                        p.first, p.second, (unsigned long long)seed, o.crashes, o.longRuns, o.covered, o.maxStimuli);
            scaling.push_back({p.first, p.second, seed, o});
        }
    }

    // The other edge of the plateau: a much faster attract decay. Even this
    // does not move the choice in this benchmark; the goal attract is re-laid
    // and the pair layer carries the lesson.
    std::vector<Outcome> attract;
    std::printf("evaporation attract side:\n");
    for (std::uint64_t seed : kSeeds) {
        Guidance g;
        g.ants = true;
        g.tsuffix = 4;
        g.name = "attract 0.50";
        g.evaporationAttract = 0.50;
        const Outcome o = runCampaign(net, g, seed, kMaxEvents, kMaxRuns);
        std::printf("  rate attract 0.500 seed %llu crashes %zu long %zu covered %zu maxStimuli %zu\n",
                    (unsigned long long)seed, o.crashes, o.longRuns, o.covered, o.maxStimuli);
        attract.push_back(o);
    }

    // --- the measured plateau ------------------------------------------------------------
    for (std::uint64_t seed : kSeeds) {
        std::vector<const Row *> per;
        std::vector<const Row *> between;
        for (const Row &r : rows) {
            if (r.seed != seed)
                continue;
            (r.betweenRuns ? between : per).push_back(&r);
        }
        CHECK(per.size() == 5);
        CHECK(between.size() == 2);
        if (per.empty() || between.empty())
            continue;

        // Every per-suggestion rate lands on the same outcome: same crashes,
        // same productive runs, coverage within the one-edge tie-break wobble.
        for (const Row *r : per) {
            CHECK(r->outcome.crashes == per.front()->outcome.crashes);
            CHECK(r->outcome.longRuns == per.front()->outcome.longRuns);
            CHECK(r->outcome.covered + 1 >= per.front()->outcome.covered);
            CHECK(r->outcome.covered <= per.front()->outcome.covered + 1);
            // The nominal retention over the longest run is tiny at the top
            // rates -- the flat outcome is not because the run was short.
            CHECK(r->outcome.maxStimuli >= 100);
        }
        CHECK(per.front()->outcome.crashes == 3);
        CHECK(per.front()->outcome.longRuns == 3);

        // Aging at the run boundary is still visibly better.
        for (const Row *r : between) {
            CHECK(r->outcome.crashes == 2);
            CHECK(r->outcome.longRuns == 4);
            CHECK(r->outcome.covered >= 700);
        }
        CHECK(between.front()->outcome.crashes < per.front()->outcome.crashes);
        CHECK(between.front()->outcome.longRuns > per.front()->outcome.longRuns);
        CHECK(between.front()->outcome.covered >= per.front()->outcome.covered + 100);
    }

    // rate x run length: the guidance profile is the same, only the run length
    // changes how much coverage a run buys.
    for (std::uint64_t seed : kSeeds) {
        const ScaleRow *shortRun = nullptr;
        const ScaleRow *longRun = nullptr;
        for (const ScaleRow &r : scaling) {
            if (r.seed != seed)
                continue;
            if (r.events == 100)
                shortRun = &r;
            else
                longRun = &r;
        }
        CHECK(shortRun && longRun);
        if (!shortRun || !longRun)
            continue;
        CHECK(shortRun->outcome.crashes == longRun->outcome.crashes);
        CHECK(shortRun->outcome.longRuns == longRun->outcome.longRuns);
        CHECK(longRun->outcome.covered > shortRun->outcome.covered * 3 / 2);
    }

    // The attract side of the plateau: no collapse at 50 % per suggestion.
    CHECK(attract.size() == 2);
    for (const Outcome &o : attract) {
        CHECK(o.crashes == 3);
        CHECK(o.longRuns == 3);
        CHECK(o.covered >= 585);
    }
}

}  // namespace

int main()
{
    const std::pair<const char *, void (*)()> tests[] = {
        {"thePheromonesKeepTheCampaignOffACrashingShortcut",
         testThePheromonesKeepTheCampaignOffACrashingShortcut},
        {"evaporationBetweenRunsKeepsProgress", testEvaporationBetweenRunsKeepsProgress},  // DeepSeek V4.1 Flash
        {"evaporationParametersSitOnAPlateau",
         testEvaporationParametersSitOnAPlateau},  // DeepSeek V4.1 Flash
    };
    for (const auto &t : tests) {
        const int before = g_failures;
        t.second();
        std::printf("%s %s\n", g_failures == before ? "PASS" : "FAIL", t.first);
    }
    std::printf("%d failure(s)\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
