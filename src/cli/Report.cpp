#include "cli/Report.h"

#include <algorithm>
#include <cstdio>

namespace cli {

using namespace pn;

std::string formatMarking(const Net &net, const Marking &marking)
{
    std::string out;
    for (std::size_t p = 0; p < marking.size() && p < net.places.size(); ++p) {
        if (marking[p] == 0)
            continue;
        if (!out.empty())
            out += ", ";
        out += net.places[p].id + ":" + std::to_string(marking[p]);
    }
    return out.empty() ? "(empty)" : out;
}

bool isFailure(RunOutcome outcome)
{
    return outcome == RunOutcome::Deviation || outcome == RunOutcome::Deadlock || outcome == RunOutcome::Livelock;
}

static std::string kindName(TransitionKind k)
{
    switch (k) {
    case TransitionKind::Controllable: return "input";
    case TransitionKind::Observable:   return "output";
    case TransitionKind::Internal:     return "internal";
    }
    return "?";
}

void printCoverage(const Console &con, const Net &net, const CoverageDb &db)
{
    const CoverageSummary s = db.summary();
    const auto line = [&](const char *what, std::size_t covered, std::size_t total, const char *note) {
        con.out << "  " << padRight(what, 13) << covered << " of " << total << note << "  (" << percent(covered, total)
                << ")\n";
    };
    con.out << con.bold("Coverage") << "\n";
    line("transitions", s.transitionsCovered, s.transitionsTotal, "");
    line("places", s.placesCovered, s.placesTotal, "");
    line("states", s.statesCovered, s.statesKnown, " known");
    line("edges", s.edgesCovered, s.edgesKnown, " known");

    std::vector<std::string> missing;
    for (std::size_t t = 0; t < net.transitions.size(); ++t) {
        if (db.transitionHits(static_cast<TransitionIndex>(t)) == 0) {
            const Transition &tr = net.transitions[t];
            missing.push_back(tr.id + " (" + kindName(tr.kind) + (tr.label.empty() ? "" : ", " + tr.label) + ")");
        }
    }
    if (!missing.empty()) {
        con.out << "\n" << con.bold("Transitions not definitely covered") << "\n";
        for (const std::string &m : missing)
            con.out << "  " << m << "\n";
    }
    std::string places;
    for (std::size_t p = 0; p < net.places.size(); ++p)
        if (db.placeVisits(static_cast<PlaceIndex>(p)) == 0)
            places += (places.empty() ? "" : ", ") + net.places[p].id;
    if (!places.empty())
        con.out << "\n" << con.bold("Places never marked") << "\n  " << places << "\n";

    if (!net.errorMarkings.empty()) {
        con.out << "\n" << con.bold("Error markings (targets)") << "\n";
        std::vector<std::vector<std::string>> rows = {{"name", "hits", "dumps", "status"}};
        for (std::size_t i = 0; i < net.errorMarkings.size(); ++i) {
            const NamedMarking &m = net.errorMarkings[i];
            const std::string hits = std::to_string(db.errorMarkingHits(i)) + " / " + std::to_string(db.errorTarget(i));
            const std::string dumps = m.crashDump == DumpMode::None
                                          ? "-"
                                          : std::to_string(db.errorMarkingDumps(i)) + " / " + std::to_string(m.maxDumps);
            rows.push_back({m.name, hits, dumps, db.isErrorSatisfied(i) ? "satisfied (avoided from now on)" : "being sought"});
        }
        printTable(con, rows);
    }
}

void printRunsAndFindings(const Console &con, const CoverageDb &db)
{
    std::size_t counts[8] = {};
    for (RunId i = 0; i < db.numRuns(); ++i)
        ++counts[static_cast<int>(db.run(i).outcome)];
    con.out << "\n" << con.bold("Runs") << "  " << db.numRuns() << " recorded";
    const RunOutcome all[] = {RunOutcome::Completed, RunOutcome::ErrorReached, RunOutcome::Deviation, RunOutcome::Deadlock,
                              RunOutcome::Livelock, RunOutcome::SutCrashed, RunOutcome::Aborted};
    std::string parts;
    for (RunOutcome o : all)
        if (counts[static_cast<int>(o)] > 0)
            parts += (parts.empty() ? "" : ", ") + std::to_string(counts[static_cast<int>(o)]) + " " + toString(o);
    if (!parts.empty())
        con.out << "  (" << parts << ")";
    con.out << "\n";

    bool anyFinding = false;
    for (RunId i = 0; i < db.numRuns(); ++i) {
        const RunRecord &r = db.run(i);
        if (!isFailure(r.outcome))
            continue;
        if (!anyFinding)
            con.out << "\n" << con.bold("Findings") << "\n";
        anyFinding = true;
        con.out << "  " << con.red("run " + std::to_string(i + 1) + "  " + toString(r.outcome)) << "  " << r.detail << "\n";
    }

    bool anyDump = false;
    for (RunId i = 0; i < db.numRuns(); ++i) {
        for (const DumpRecord &d : db.run(i).dumps) {
            if (!anyDump)
                con.out << "\n" << con.bold("Post-mortems") << "\n";
            anyDump = true;
            std::string files;
            for (const std::string &f : d.files)
                files += (files.empty() ? "" : ", ") + f;
            con.out << "  run " << (i + 1) << "  " << toString(d.mode) << (d.marking.empty() ? "" : "  at " + d.marking)
                    << "  " << files << "\n";
        }
    }
}

// GLM-5.3-Flash: the crash predictions and their reproduction experiments
// (engine/CrashCredibility.h). Predicted entries exist from the start; an
// implicit one appears at its first attributed spontaneous crash, so a model
// without predictions and without deaths prints no block at all.
void printCrashPredictions(const Console &con, const CoverageDb &db)
{
    const std::vector<CrashEntry> &entries = db.crashEntries();
    if (entries.empty())
        return;
    const CoverageConfig &cfg = db.config();
    char level[16];
    std::snprintf(level, sizeof level, "%.3g", cfg.crashLevel);
    con.out << "\n" << con.bold("Crash predictions") << "  credibility level " << level
            << ", give-up budget " << cfg.crashAttempts << " for crashes nobody predicted\n";
    std::vector<std::vector<std::string>> rows = {{"crash site", "prediction", "status", "attempts", "crashes", "dumps"}};
    for (const CrashEntry &e : entries) {
        std::string prediction = "implicit";
        if (e.prediction) {
            prediction = toString(e.prediction->crashClass);
            if (e.prediction->p) {
                char b[16];
                std::snprintf(b, sizeof b, " p=%.3g", *e.prediction->p);
                prediction += b;
            }
            if (e.prediction->attempts)
                prediction += " attempts=" + std::to_string(*e.prediction->attempts);
        }
        std::string dumps = "-";
        if (e.prediction && e.prediction->crashDump != DumpMode::None)
            dumps = std::to_string(e.dumps) + " / " + std::to_string(e.prediction->maxDumps);
        rows.push_back({e.key, prediction, toString(e.status), std::to_string(e.stats.attempts),
                        std::to_string(e.stats.crashes), dumps});
    }
    printTable(con, rows);
}

// GLM-5.3-Flash: the command hooks the runs executed (the "command" key of
// the JSON, doku section 18). Every execution is one line: the mode, the
// status word, the transition (its label when it has one), the exit status
// when known, and the first line of the captured output. Restored runs show
// their records too (they travel in the run log; the commands are not
// re-executed). Prints no block when no run executed a hook.
void printHookRuns(const Console &con, const CoverageDb &db)
{
    std::size_t total = 0;
    for (RunId i = 0; i < db.numRuns(); ++i)
        total += db.run(i).hookRuns.size();
    if (total == 0)
        return;
    con.out << "\n" << con.bold("Command hooks") << "\n";
    std::vector<std::vector<std::string>> rows = {{"run", "mode", "status", "transition", "exit", "output"}};
    for (RunId i = 0; i < db.numRuns(); ++i) {
        for (const HookRecord &h : db.run(i).hookRuns) {
            const std::string exit = h.exit == kHookNoExit ? std::string("-") : std::to_string(h.exit);
            std::string out = h.output;
            const std::size_t nl = out.find('\n');
            if (nl != std::string::npos)
                out = out.substr(0, nl) + " ...";
            rows.push_back({std::to_string(i + 1), std::string(h.async ? "async" : "sync"), h.status,
                            h.label.empty() ? h.transition : h.label, exit, truncate(out, 48)});
        }
    }
    printTable(con, rows);
}

// ---------------------------------------------------------------------------
// GLM-5.3-Flash: backtraces (doku section 12, item 2)
// ---------------------------------------------------------------------------

// A state marking against the tokens of a named marking (sparse, sorted).
static bool sameTokens(const Marking &m, const SparseMarking &tokens)
{
    Marking dense(m.size(), 0);
    for (const TokenEntry &te : tokens) {
        if (te.place >= dense.size())
            return false;
        dense[te.place] = te.tokens;
    }
    return dense == m;
}

// The name of the initial marking a run started from, for the head of a
// backtrace; falls back to the marking itself.
static std::string initialName(const Net &net, const ReachabilityGraph &rg, StateId s)
{
    const Marking m = rg.marking(s);
    for (const NamedMarking &im : net.initialMarkings)
        if (sameTokens(m, im.tokens))
            return im.name;
    return formatMarking(net, m);
}

// The chain of a backtrace, oldest first: the initial marking, then the
// transition taken at every step. Steps without an edge are special: the
// first step of a run has none (nothing led into the initial marking), and
// an ambiguous stretch has none either -- the run certainly went on, but the
// exact edge is not certain; both render so that the reader can tell them
// apart.
static std::string chainText(const Net &net, const ReachabilityGraph &rg, const std::vector<TraceStep> &steps)
{
    std::string out;
    for (std::size_t i = 0; i < steps.size(); ++i) {
        std::string piece;
        if (steps[i].via == kNoEdge) {
            piece = i == 0 ? initialName(net, rg, steps[i].state) : "...";
        } else {
            const Transition &tr = net.transitions[rg.edge(steps[i].via).transition];
            piece = !tr.label.empty() ? tr.label : tr.id;
        }
        if (!out.empty())
            out += " > ";
        out += piece;
    }
    return out;
}

// GLM-5.3-Flash: the GUI half of item 2 was closed as a textual report: for
// every definitely covered transition, the chain of moves that led into the
// first definite visit of its earliest covered edge, oldest first. A
// transition whose visits are all transition-level (observed certainly, but
// never on one certain edge) says so instead of guessing a chain.
void printBacktraces(const Console &con, const Net &net, const ReachabilityGraph &rg, const CoverageDb &db)
{
    // The anchor of each covered transition: its earliest covered edge,
    // ordered by first run, then by the earlier trace-tree node within that
    // run (nodes are created in run order, so both keys sort chronologically).
    std::vector<std::pair<RunId, NodeId>> anchor(net.transitions.size(), {kNoRun, kNoNode});
    std::vector<EdgeId> anchorEdge(net.transitions.size(), kNoEdge);
    for (EdgeId e = 0; e < rg.numEdges(); ++e) {
        const ElementCoverage &c = db.edge(e);
        if (c.definite == 0)
            continue;
        const TransitionIndex t = rg.edge(e).transition;
        if (anchorEdge[t] == kNoEdge
            || std::make_pair(c.firstRun, c.firstNode) < anchor[t]) {
            anchor[t] = {c.firstRun, c.firstNode};
            anchorEdge[t] = e;
        }
    }

    // DeepSeek V4.1 Flash: the name column follows the longest name printed, so a
    // long label never runs into its chain (a fixed 16 made `bank.stolen_report`
    // and its chain read as one word).
    std::size_t nameWidth = 16;
    for (std::size_t t = 0; t < net.transitions.size(); ++t) {
        if (db.transitionHits(static_cast<TransitionIndex>(t)) == 0)
            continue;
        const Transition &tr = net.transitions[t];
        nameWidth = std::max(nameWidth, tr.label.empty() ? tr.id.size() : tr.label.size());
    }

    std::string rows;
    for (std::size_t t = 0; t < net.transitions.size(); ++t) {
        if (db.transitionHits(static_cast<TransitionIndex>(t)) == 0)
            continue;
        const Transition &tr = net.transitions[t];
        std::string chain;
        if (anchorEdge[t] != kNoEdge)
            chain = chainText(net, rg, db.edgeBacktrace(anchorEdge[t]));
        else
            chain = "(visited, but never on a certain edge: the visits are transition-level only)";
        // One separator space on top of the column: padRight leaves a name
        // that fills the column untouched, and it must not touch its chain.
        rows += "  " + padRight(!tr.label.empty() ? tr.label : tr.id, nameWidth) + " " + chain + "\n";
    }
    if (rows.empty())
        return;
    con.out << "\n" << con.bold("Backtraces")
            << "  (the way into the first definite visit of each covered transition, oldest first)\n"
            << rows;
    if (rows.find("...") != std::string::npos)
        con.out << "  \"...\" marks an ambiguous stretch: the run certainly went on, but the exact edge is not certain\n";
}

// GLM-5.3-Flash: the time criterion. Coverage says how much was tested; this
// says what it cost: the campaign's wall-clock time, the mean and longest run,
// and how many SUT events a second of testing buys.
void printTime(const Console &con, const CampaignReport &report)
{
    if (report.runs.empty())
        return;
    std::int64_t runTotal = 0, longest = 0;
    std::size_t events = 0;
    for (const RunReport &r : report.runs) {
        runTotal += r.durationMs;
        longest = std::max(longest, r.durationMs);
        events += r.events;
    }
    const double meanRun = static_cast<double>(runTotal) / static_cast<double>(report.runs.size());
    const double wall = static_cast<double>(report.wallMs);
    const double perSecond = wall > 0.0 ? static_cast<double>(events) * 1000.0 / wall : 0.0;

    char wallS[32], meanS[32], rateS[32];
    std::snprintf(wallS, sizeof wallS, "%.2f", wall / 1000.0);
    std::snprintf(meanS, sizeof meanS, "%.0f", meanRun);
    std::snprintf(rateS, sizeof rateS, "%.1f", perSecond);
    con.out << "\n" << con.bold("Time") << "  campaign " << wallS << " s, mean run " << meanS << " ms";
    con.out << " (longest " << longest << " ms)";
    con.out << ", " << rateS << " events/s\n";
}

// GLM-5.3-Flash: how much the runs of a campaign overlap. Low numbers mean
// the steering spread the testing out; 100% would mean every run repeated
// the same coverage. The full pairwise matrices are in the --json report
// and in `eggembt compare`.
void printOverlap(const Console &con, const OverlapMatrix &m)
{
    if (m.n < 2)
        return;
    const auto pct = [](double v) {
        char buf[16];
        std::snprintf(buf, sizeof buf, "%.0f%%", v * 100.0);
        return std::string(buf);
    };
    con.out << "\n" << con.bold("Overlap") << "  mean pairwise Jaccard: transitions "
            << pct(m.meanPairwise(m.transitions)) << ", states " << pct(m.meanPairwise(m.states)) << ", edges "
            << pct(m.meanPairwise(m.edges)) << "\n";
}

JsonValue overlapJson(const OverlapMatrix &m)
{
    JsonValue o = JsonValue::object();
    o.set("runs", JsonValue::number(static_cast<double>(m.n)));
    const auto matrix = [&](const std::vector<double> &v) {
        JsonValue rows = JsonValue::array();
        for (std::size_t i = 0; i < m.n; ++i) {
            JsonValue row = JsonValue::array();
            for (std::size_t j = 0; j < m.n; ++j)
                row.push(JsonValue::number(v[i * m.n + j]));
            rows.push(row);
        }
        return rows;
    };
    o.set("transitions", matrix(m.transitions));
    o.set("states", matrix(m.states));
    o.set("edges", matrix(m.edges));
    JsonValue mean = JsonValue::object();
    mean.set("transitions", JsonValue::number(m.meanPairwise(m.transitions)));
    mean.set("states", JsonValue::number(m.meanPairwise(m.states)));
    mean.set("edges", JsonValue::number(m.meanPairwise(m.edges)));
    o.set("mean_pairwise", mean);
    return o;
}

JsonValue coverageJson(const Net &net, const CoverageDb &db, bool elements)
{
    const CoverageSummary s = db.summary();
    JsonValue cov = JsonValue::object();
    const auto pair = [](std::size_t covered, std::size_t total) {
        JsonValue o = JsonValue::object();
        o.set("covered", JsonValue::number(static_cast<double>(covered)));
        o.set("total", JsonValue::number(static_cast<double>(total)));
        return o;
    };
    cov.set("transitions", pair(s.transitionsCovered, s.transitionsTotal));
    cov.set("places", pair(s.placesCovered, s.placesTotal));
    cov.set("states", pair(s.statesCovered, s.statesKnown));
    cov.set("edges", pair(s.edgesCovered, s.edgesKnown));

    JsonValue uncovered = JsonValue::array();
    for (std::size_t t = 0; t < net.transitions.size(); ++t)
        if (db.transitionHits(static_cast<TransitionIndex>(t)) == 0)
            uncovered.push(JsonValue::string(net.transitions[t].id));
    cov.set("uncovered_transitions", uncovered);

    JsonValue errors = JsonValue::array();
    for (std::size_t i = 0; i < net.errorMarkings.size(); ++i) {
        JsonValue e = JsonValue::object();
        e.set("name", JsonValue::string(net.errorMarkings[i].name));
        e.set("hits", JsonValue::number(static_cast<double>(db.errorMarkingHits(i))));
        e.set("target", JsonValue::number(db.errorTarget(i)));
        e.set("dumps", JsonValue::number(static_cast<double>(db.errorMarkingDumps(i))));
        e.set("satisfied", JsonValue::boolean(db.isErrorSatisfied(i)));
        errors.push(e);
    }
    cov.set("error_markings", errors);

    // GLM-5.3-Flash: error transitions. Their visits are the ordinary
    // transition hits; dumps counts the post-mortems taken for them.
    JsonValue errorTransitions = JsonValue::array();
    for (std::size_t t = 0; t < net.transitions.size(); ++t) {
        const Transition &tr = net.transitions[t];
        if (!tr.error)
            continue;
        const auto idx = static_cast<TransitionIndex>(t);
        JsonValue e = JsonValue::object();
        e.set("id", JsonValue::string(tr.id));
        e.set("label", JsonValue::string(tr.label));
        e.set("hits", JsonValue::number(static_cast<double>(db.transitionHits(idx))));
        e.set("dumps", JsonValue::number(static_cast<double>(db.errorTransitionDumps(idx))));
        errorTransitions.push(e);
    }
    cov.set("error_transitions", errorTransitions);

    // GLM-5.3-Flash: milestone transitions. Their visits are the ordinary
    // transition hits; the rank is the deposit multiplier.
    JsonValue milestones = JsonValue::array();
    for (std::size_t t = 0; t < net.transitions.size(); ++t) {
        const Transition &tr = net.transitions[t];
        if (!tr.milestone)
            continue;
        JsonValue m = JsonValue::object();
        m.set("id", JsonValue::string(tr.id));
        m.set("label", JsonValue::string(tr.label));
        m.set("rank", JsonValue::number(static_cast<double>(tr.milestoneRank)));
        m.set("hits", JsonValue::number(static_cast<double>(db.transitionHits(static_cast<TransitionIndex>(t)))));
        milestones.push(m);
    }
    cov.set("milestones", milestones);

    // DeepSeek V4.1 Flash: the per-element view (--coverage-elements, doku section
    // 21). Off by default because a large graph makes this large; on, every
    // known state and edge is listed with its hit counts and runs, so a
    // campaign can be inspected in detail after the fact.
    if (elements) {
        const ReachabilityGraph &rg = db.graph();
        JsonValue states = JsonValue::array();
        for (StateId s = 0; s < rg.numStates(); ++s) {
            const ElementCoverage &c = db.state(s);
            JsonValue j = JsonValue::object();
            j.set("marking", JsonValue::string(formatMarking(net, rg.marking(s))));
            j.set("definite", JsonValue::number(static_cast<double>(c.definite)));
            j.set("ambiguous", JsonValue::number(static_cast<double>(c.ambiguous)));
            j.set("covered", JsonValue::boolean(c.definite >= db.config().threshold));
            if (c.firstRun != kNoRun)
                j.set("first_run", JsonValue::number(static_cast<double>(c.firstRun + 1)));
            if (c.lastRun != kNoRun)
                j.set("last_run", JsonValue::number(static_cast<double>(c.lastRun + 1)));
            states.push(j);
        }
        cov.set("states", states);

        JsonValue edges = JsonValue::array();
        for (StateId s = 0; s < rg.numStates(); ++s) {
            if (!rg.isExpanded(s))
                continue;
            const EdgeRange range = rg.successors(s);
            for (std::size_t i = 0; i < range.size(); ++i) {
                const EdgeId e = rg.firstEdge(s) + static_cast<EdgeId>(i);
                const Edge &ed = rg.edge(e);
                const Transition &tr = net.transitions[ed.transition];
                const ElementCoverage &c = db.edge(e);
                JsonValue j = JsonValue::object();
                j.set("from", JsonValue::string(formatMarking(net, rg.marking(s))));
                j.set("transition", JsonValue::string(tr.id));
                if (!tr.label.empty())
                    j.set("label", JsonValue::string(tr.label));
                j.set("to", JsonValue::string(formatMarking(net, rg.marking(ed.target))));
                j.set("definite", JsonValue::number(static_cast<double>(c.definite)));
                j.set("ambiguous", JsonValue::number(static_cast<double>(c.ambiguous)));
                j.set("covered", JsonValue::boolean(c.definite >= db.config().threshold));
                if (c.firstRun != kNoRun)
                    j.set("first_run", JsonValue::number(static_cast<double>(c.firstRun + 1)));
                if (c.lastRun != kNoRun)
                    j.set("last_run", JsonValue::number(static_cast<double>(c.lastRun + 1)));
                edges.push(j);
            }
        }
        cov.set("edges", edges);
    }
    return cov;
}

JsonValue campaignJson(const Net &net, const Session &session, const CampaignReport &report,
                       bool coverageElements)
{
    JsonValue o = JsonValue::object();
    o.set("model", JsonValue::string(net.name));
    o.set("complete", JsonValue::boolean(report.complete));
    o.set("stop_reason", JsonValue::string(report.stopReason));
    // DeepSeek V4.1 Flash: the effective testing strategy (doku section 27).
    if (const auto goals = session.config().goalPriority) {
        JsonValue strategy = JsonValue::object();
        strategy.set("goals", JsonValue::string(toString(*goals)));
        strategy.set("errors", JsonValue::string(toString(*session.config().errorPriority)));
        o.set("strategy", strategy);
    }
    // DeepSeek V4.1 Flash: where the pheromone ages, and the campaign budget.
    o.set("evaporate_between_runs", JsonValue::boolean(session.config().evaporateBetweenRuns));
    o.set("time_budget_ms", JsonValue::number(static_cast<double>(session.config().timeBudget.count())));
    // DeepSeek V4.1 Flash: the rare hunt (doku section 30). Both keys are
    // always emitted so the schema does not depend on the model or the option.
    o.set("hunt_rare", JsonValue::boolean(session.config().huntRare));
    {
        const CoverageDb &db = session.coverage();
        JsonValue rare = JsonValue::array();
        for (std::size_t t = 0; t < net.transitions.size(); ++t) {
            const Transition &tr = net.transitions[t];
            if (!tr.rare)
                continue;
            const auto ti = static_cast<TransitionIndex>(t);
            JsonValue j = JsonValue::object();
            j.set("transition", JsonValue::string(tr.id));
            if (!tr.label.empty())
                j.set("label", JsonValue::string(tr.label));
            j.set("p", JsonValue::number(tr.rare->p));
            j.set("attempts", JsonValue::number(static_cast<double>(tr.rare->attempts)));
            j.set("wait_for_ms", JsonValue::number(static_cast<double>(tr.rare->waitForMs)));
            j.set("seen", JsonValue::boolean(db.transitionHits(ti) > 0));
            j.set("attempts_used", JsonValue::number(static_cast<double>(db.rareAttempts(ti))));
            rare.push(j);
        }
        o.set("rare_transitions", rare);
    }

    JsonValue warnings = JsonValue::array();
    for (const std::string &w : report.warnings)
        warnings.push(JsonValue::string(w));
    o.set("warnings", warnings);

    JsonValue runs = JsonValue::array();
    for (const RunReport &r : report.runs) {
        JsonValue j = JsonValue::object();
        j.set("outcome", JsonValue::string(toString(r.outcome)));
        j.set("detail", JsonValue::string(r.detail));
        j.set("events", JsonValue::number(static_cast<double>(r.events)));
        j.set("inputs", JsonValue::number(static_cast<double>(r.stimuli)));
        j.set("dumps", JsonValue::number(static_cast<double>(r.dumps)));
        j.set("new_states", JsonValue::number(r.newStates));
        j.set("new_edges", JsonValue::number(r.newEdges));
        j.set("duration_ms", JsonValue::number(static_cast<double>(r.durationMs)));  // GLM-5.3-Flash
        // DeepSeek V4.1 Flash: this run was cut short by the campaign's budget.
        j.set("budget_reached", JsonValue::boolean(r.budgetReached));
        // GLM-5.3-Flash: the good target marking the run ended in ("" when none;
        // always emitted so the schema does not depend on the model).
        j.set("good_target", JsonValue::string(r.goodTarget));
        JsonValue errs = JsonValue::array();
        for (const std::string &e : r.errorsReached)
            errs.push(JsonValue::string(e));
        j.set("errors_reached", errs);
        // GLM-5.3-Flash: the milestone transitions certainly stepped in this
        // run (mirror of errors_reached).
        JsonValue msts = JsonValue::array();
        for (const std::string &m : r.milestonesReached)
            msts.push(JsonValue::string(m));
        j.set("milestones_reached", msts);
        runs.push(j);
    }
    o.set("runs", runs);
    o.set("coverage", coverageJson(net, session.coverage(), coverageElements));

    // GLM-5.3-Flash: the time criterion. Total wall time, mean run duration,
    // and how many SUT events a second of testing bought. Also per run, as
    // duration_ms above, so different seeds or configurations can be compared
    // by testing efficiency next to their coverage.
    {
        std::int64_t runTotal = 0;
        std::size_t events = 0;
        for (const RunReport &r : report.runs) {
            runTotal += r.durationMs;
            events += r.events;
        }
        const double meanRun =
            report.runs.empty() ? 0.0 : static_cast<double>(runTotal) / static_cast<double>(report.runs.size());
        const double wall = static_cast<double>(report.wallMs);
        JsonValue time = JsonValue::object();
        time.set("wall_ms", JsonValue::number(wall));
        time.set("mean_run_ms", JsonValue::number(meanRun));
        time.set("events_per_s", JsonValue::number(wall > 0.0 ? static_cast<double>(events) * 1000.0 / wall : 0.0));
        o.set("time", time);
    }

    // GLM-5.3-Flash: how much the runs of this session overlap (pairwise
    // Jaccard indices over transitions/states/edges, and their means).
    o.set("overlap", overlapJson(runOverlapMatrix(session.coverage())));

    // GLM-5.3-Flash: which guidance steered the campaign. The ant colony can
    // be switched off (--no-ants / SessionConfig::useAnts = false); its
    // counters say how much it actually walked the model (0 when switched
    // off, so the schema does not depend on the setting).
    o.set("ants", JsonValue::boolean(session.antsEnabled()));
    const pn::AntStats &ants = session.ants().stats();
    o.set("ant_iterations", JsonValue::number(static_cast<double>(ants.iterations)));
    o.set("ant_walks", JsonValue::number(static_cast<double>(ants.ants)));

    // GLM-5.3-Flash: the remaining guidance switches, always emitted so the
    // --json schema does not depend on what was passed on the command line.
    const SessionConfig &cfg = session.config();
    // GLM-5.3-Flash: the seeds, so a campaign can be reproduced from the report.
    o.set("seed", JsonValue::number(static_cast<double>(cfg.selectionSeed)));
    o.set("ant_seed", JsonValue::number(static_cast<double>(cfg.ants.seed)));
    o.set("deadlock_policy", JsonValue::string(toString(cfg.targets.deadlock)));
    o.set("livelock_policy", JsonValue::string(toString(cfg.targets.livelock)));
    // DeepSeek V4.1 Flash: the suggester's reaction to a satisfied target
    // (doku section 28). Always emitted, so the schema does not depend on it.
    o.set("satisfied_target_policy", JsonValue::string(toString(cfg.suggester.satisfiedTargets)));
    o.set("dump_on_deviation", JsonValue::string(toString(cfg.dumps.onDeviation)));
    o.set("dump_on_deadlock", JsonValue::string(toString(cfg.dumps.onDeadlock)));
    o.set("dump_on_livelock", JsonValue::string(toString(cfg.dumps.onLivelock)));
    o.set("coverage_threshold", JsonValue::number(cfg.coverage.threshold));
    // GLM-5.3-Flash: whether the catastrophe pheromones are priced by the way
    // in (--flat-catastrophe switches that off).
    o.set("catastrophe_time_pricing", JsonValue::boolean(cfg.catastropheTimePricing));
    // GLM-5.3-Flash: the T-suffix layer (doku section 16). The length of the
    // lists, 0 when the layer is off; always emitted so the schema does not
    // depend on the setting.
    o.set("tsuffix", JsonValue::number(static_cast<double>(cfg.ants.tsuffixLength)));
    // GLM-5.3-Flash: how much of the earlier rounds' pheromone teaching a
    // continued campaign starts with, in percent (doku section 16).
    o.set("restored_pheromones", JsonValue::number(static_cast<double>(cfg.restoredPheromonePercent)));

    // GLM-5.3-Flash: the crash predictions of the model and their reproduction
    // experiments (doku section 17), plus the two knobs so a campaign can be
    // reproduced. Always emitted: an empty array when the model predicts
    // nothing and nothing died, so the schema does not depend on the model.
    o.set("crash_level", JsonValue::number(cfg.coverage.crashLevel));
    o.set("crash_attempts", JsonValue::number(static_cast<double>(cfg.coverage.crashAttempts)));
    JsonValue crashes = JsonValue::array();
    for (const CrashEntry &e : session.coverage().crashEntries()) {
        JsonValue c = JsonValue::object();
        c.set("key", JsonValue::string(e.key));
        c.set("prediction", JsonValue::string(e.prediction ? toString(e.prediction->crashClass) : "implicit"));
        c.set("status", JsonValue::string(toString(e.status)));
        c.set("attempts", JsonValue::number(static_cast<double>(e.stats.attempts)));
        c.set("crashes", JsonValue::number(static_cast<double>(e.stats.crashes)));
        c.set("dumps", JsonValue::number(static_cast<double>(e.dumps)));
        crashes.push(c);
    }
    o.set("crash_predictions", crashes);

    // GLM-5.3-Flash: the command hooks (doku section 18): the bound, plus the
    // executions per run. Always emitted so the schema does not depend on
    // the model or on what ran.
    o.set("hook_timeout", JsonValue::number(static_cast<double>(cfg.hookTimeoutSeconds)));
    // DeepSeek V4.1 Flash: synchronized events (doku section 19): the campaign
    // switch and how many transitions the model marks. Always emitted.
    o.set("sync_events", JsonValue::boolean(cfg.syncEvents));
    std::size_t syncTransitions = 0;
    for (const Transition &t : net.transitions)
        if (t.synchronized)
            ++syncTransitions;
    o.set("sync_transitions", JsonValue::number(static_cast<double>(syncTransitions)));
    JsonValue hookRuns = JsonValue::array();
    for (RunId i = 0; i < session.coverage().numRuns(); ++i) {
        const RunRecord &r = session.coverage().run(i);
        if (r.hookRuns.empty())
            continue;
        JsonValue hr = JsonValue::object();
        hr.set("run", JsonValue::number(static_cast<double>(i + 1)));
        JsonValue hooks = JsonValue::array();
        for (const HookRecord &h : r.hookRuns) {
            JsonValue hobj = JsonValue::object();
            hobj.set("transition", JsonValue::string(h.transition));
            hobj.set("label", JsonValue::string(h.label));
            hobj.set("async", JsonValue::boolean(h.async));
            hobj.set("status", JsonValue::string(h.status));
            if (h.exit != kHookNoExit)
                hobj.set("exit", JsonValue::number(h.exit));
            if (!h.output.empty())
                hobj.set("output", JsonValue::string(h.output));
            hooks.push(hobj);
        }
        hr.set("hooks", hooks);
        hookRuns.push(hr);
    }
    o.set("hook_runs", hookRuns);
    return o;
}


// ---------------------------------------------------------------------------
// DeepSeek V4.1 Flash: inspection views (doku section 21)
// ---------------------------------------------------------------------------

// One step's display name: the transition's label when it has one, else its
// id -- the same rule backtraces use. The log records ids, so the net has to
// say what to print.
static std::string stepName(const Net &net, const std::string &id)
{
    const std::optional<TransitionIndex> t = net.findTransition(id);
    if (!t)
        return id;
    const Transition &tr = net.transitions[*t];
    return tr.label.empty() ? tr.id : tr.label;
}

void printTraces(const Console &con, const Net &net, const std::vector<PersistedRun> &runs, int onlyRun, bool full)
{
    if (runs.empty())
        return;
    if (onlyRun > 0 && static_cast<std::size_t>(onlyRun) > runs.size()) {
        con.out << "\n" << con.bold("Traces") << "  the log has " << runs.size() << " run(s), no run " << onlyRun
                << "\n";
        return;
    }
    const std::size_t cap = full ? 0 : 20;  // --trace shows everything
    con.out << "\n" << con.bold("Traces") << "  ("
            << (onlyRun > 0 ? "run " + std::to_string(onlyRun) : "the recorded steps of every run")
            << ", oldest first)\n";
    for (std::size_t i = 0; i < runs.size(); ++i) {
        if (onlyRun > 0 && i + 1 != static_cast<std::size_t>(onlyRun))
            continue;
        const PersistedRun &r = runs[i];
        std::vector<std::string> steps;
        if (!r.transitions.empty()) {
            for (const std::string &id : r.transitions)
                steps.push_back(stepName(net, id));
        } else {
            // An event run records its live events instead of a path.
            for (const SutEvent &e : r.events)
                steps.push_back(e.label.empty() ? "[" + std::string(toString(e.kind)) + "]" : e.label);
        }

        const bool elided = cap > 0 && steps.size() > cap;
        const std::size_t first = elided ? steps.size() - cap : 0;
        std::string chain = r.initialMarking.empty() ? "?" : r.initialMarking;
        if (elided)
            chain += " > ...";
        for (std::size_t k = first; k < steps.size(); ++k)
            chain += " > " + steps[k];
        if (full && !r.detail.empty())
            chain += "  (" + truncate(r.detail, 60) + ")";

        con.out << "  run " << padRight(std::to_string(i + 1), 4) << padRight(std::string(toString(r.outcome)), 14)
                << chain;
        if (elided)
            con.out << "  [" << steps.size() << " steps]";
        con.out << "\n";
    }
}

JsonValue pheromoneJson(const Net &net, const Session &session)
{
    const ReachabilityGraph &rg = session.graph();
    const AntColony &ants = session.ants();
    JsonValue o = JsonValue::object();
    o.set("model", JsonValue::string(net.name));
    o.set("tsuffix", JsonValue::number(static_cast<double>(ants.tsuffixLength())));
    o.set("tau_max", JsonValue::number(ants.tauMax()));
    o.set("iterations", JsonValue::number(static_cast<double>(ants.stats().iterations)));
    o.set("ant_walks", JsonValue::number(static_cast<double>(ants.stats().ants)));

    JsonValue edges = JsonValue::array();
    for (StateId s = 0; s < rg.numStates(); ++s) {
        if (!rg.isExpanded(s))
            continue;
        const EdgeRange range = rg.successors(s);
        for (std::size_t i = 0; i < range.size(); ++i) {
            const EdgeId e = rg.firstEdge(s) + static_cast<EdgeId>(i);
            const Edge &ed = rg.edge(e);
            const Transition &tr = net.transitions[ed.transition];
            JsonValue j = JsonValue::object();
            j.set("from", JsonValue::string(formatMarking(net, rg.marking(s))));
            j.set("transition", JsonValue::string(tr.id));
            if (!tr.label.empty())
                j.set("label", JsonValue::string(tr.label));
            j.set("to", JsonValue::string(formatMarking(net, rg.marking(ed.target))));
            j.set("attract", JsonValue::number(ants.attract(e)));
            j.set("repel", JsonValue::number(ants.repel(e)));
            j.set("region", JsonValue::number(ants.region(e)));
            edges.push(j);
        }
    }
    o.set("edges", edges);

    JsonValue pairs = JsonValue::array();
    for (const AntColony::PairEntry &p : ants.pairEntries()) {
        JsonValue j = JsonValue::object();
        j.set("state", JsonValue::string(formatMarking(net, rg.marking(p.state))));
        JsonValue list = JsonValue::array();
        for (std::size_t k = 0; k < ants.tsuffixLength() && k < kMaxTSuffix; ++k)
            list.push(JsonValue::number(static_cast<double>(p.tsuffix[k])));
        j.set("tsuffix", list);
        j.set("attract", JsonValue::number(p.attract));
        j.set("repel", JsonValue::number(p.repel));
        pairs.push(j);
    }
    o.set("pairs", pairs);
    return o;
}

}  // namespace cli
