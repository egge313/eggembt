#include "engine/Session.h"

#include <fcntl.h>    // GLM-5.3-Flash: /dev/null for the asynchronous hooks
#include <sys/wait.h>  // GLM-5.3-Flash: reaping the asynchronous hook children
#include <unistd.h>    // GLM-5.3-Flash: fork/exec for the hook commands

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstring>
#include <optional>
#include <poll.h>

namespace pn {

namespace {

// DeepSeek V4.1 Flash: a goal priority is a preference expressed in the
// suggester's "steps" currency (lower wins) plus the ants' deposit size. The
// numbers are deliberately simple and documented in doku section 27.
void applyGoalPriority(SessionConfig &c, GoalPriority priority)
{
    switch (priority) {
    case GoalPriority::Balanced:
        break;  // the historical weights: targets and fresh coverage close together
    case GoalPriority::Targets:
        // Targets and milestones before fresh coverage: a fresh transition,
        // edge or frontier must be markedly closer to win, and
        // target/milestone deposits speak louder.
        c.suggester.penaltyNewTransition = 2;
        c.suggester.penaltyEdge = 4;
        c.suggester.penaltyFrontier = 8;
        c.ants.depositTarget *= 1.5;
        c.ants.depositMilestone *= 1.5;
        break;
    case GoalPriority::Coverage:
        // Fresh behaviour before targets: a still-sought target must be
        // clearly closer than an uncovered edge, so targets stop pulling the
        // campaign away from plain coverage.
        c.suggester.penaltyTarget = 4;
        c.suggester.penaltyEdge = 0;
        c.suggester.penaltyFrontier = 1;
        break;
    }
}

// DeepSeek V4.1 Flash: error priority scales the two risk terms of the
// suggester's score. "Seek" accepts a less robust input and more hazard on the
// way; "avoid" demands robustness and punishes a known way into a satisfied
// error much harder.
void applyErrorPriority(SessionConfig &c, ErrorPriority priority)
{
    switch (priority) {
    case ErrorPriority::Balanced:
        break;
    case ErrorPriority::Seek:
        c.suggester.riskWeight = 1.0;
        c.suggester.hazardWeight = 10.0;
        break;
    case ErrorPriority::Avoid:
        c.suggester.riskWeight = 5.0;
        c.suggester.hazardWeight = 40.0;
        break;
    }
}

SessionConfig normalized(SessionConfig c, const Net &net)
{
    c.suggester.targets = c.targets;
    c.suggester.tieBreakSeed = c.selectionSeed;  // GLM-5.3-Flash
    c.ants.targets = c.targets;
    // GLM-5.3-Flash: the carry-over factor of the restored pheromones
    // (doku section 16), kept in 0..100 for direct API users.
    c.restoredPheromonePercent = std::clamp(c.restoredPheromonePercent, 0u, 100u);
    // DeepSeek V4.1 Flash: the strategy (doku section 27). The command line
    // overrides the model; the model overrides the built-in Balanced default.
    // The resolved values stay in the config so reports can name them.
    const GoalPriority goals = c.goalPriority.value_or(net.strategy.goals);
    const ErrorPriority errors = c.errorPriority.value_or(net.strategy.errors);
    c.goalPriority = goals;
    c.errorPriority = errors;
    applyGoalPriority(c, goals);
    applyErrorPriority(c, errors);
    return c;
}

std::string joinLabels(const std::vector<std::string> &labels)
{
    std::string out;
    for (std::size_t i = 0; i < labels.size(); ++i) {
        if (i)
            out += ", ";
        out += labels[i];
    }
    return out;
}

}  // namespace

Session::Session(const Net &net, SessionConfig config)
    : net_(net),
      cfg_(normalized(std::move(config), net)),  // DeepSeek V4.1 Flash: the net's strategy
      rg_(net_, cfg_.graph),
      db_(rg_, cfg_.coverage),
      tracker_(rg_, &db_, cfg_.tracker),
      suggester_(rg_, db_, cfg_.suggester),
      ants_(rg_, db_, [&] {
          // GLM-5.3-Flash: the ants' environment feed (doku section 12, item
          // 3). The colony's environment moves are no longer weighted
          // uniformly: each is weighted by how often the SUT actually took
          // that move when it was offered (CoverageDb::takeRate; 1.0 while a
          // move was never offered, so silent internal steps stay neutral).
          // A move the SUT keeps refusing stops pulling the ants.
          AntConfig a = cfg_.ants;
          a.environmentWeight = [this](EdgeId e) {
              const Transition &t = net_.transitions[rg_.edge(e).transition];
              return db_.takeRate(t.label);
          };
          return a;
      }()),
      queue_(cfg_.eventQueueCapacity)
{
    // GLM-5.3-Flash: the T-suffix layer (doku section 16). The run cascade's
    // length comes from the ant configuration; the suggester reads it for its
    // pair consults. With tsuffixLength 0 both are inert.
    run_.setLength(cfg_.ants.tsuffixLength);
    suggester_.setTraceCascade(&run_);
    if (cfg_.useAnts)
        suggester_.setAntColony(&ants_);
    // DeepSeek V4.1 Flash: the model's synchronized events (doku section 19). The
    // labels go to the plug-in only when the campaign switch is on and the
    // plug-in can acknowledge them.
    for (const Transition &t : net_.transitions)
        if (t.synchronized)
            syncLabels_.push_back(t.label);
}

// GLM-5.3-Flash: the asynchronous hook children were fire-and-forget: they
// are never killed. At destruction those that have already ended are reaped
// (no zombie litters the process table); the ones still running keep running
// -- eggembt orphans them to the init process, which reaps them. The synchronous
// children are always waited for at their execution.
Session::~Session()
{
    for (const pid_t pid : hookChildren_) {
        int status = 0;
        while (::waitpid(pid, &status, WNOHANG) > 0) { }  // already ended: reap
    }
}

const LivelockReport &Session::analyzeModel()
{
    for (std::size_t i = 0; i < net_.initialMarkings.size(); ++i)
        rg_.addInitial(i);
    rg_.exploreAll();
    static_ = analyzeLivelocks(rg_, cfg_.livelock);
    suggester_.setLivelockFindings(static_.findings);
    ants_.setLivelockFindings(static_.findings);
    analyzed_ = true;

    warnings_.clear();
    std::size_t terminal = 0;
    bool initialInside = false;
    for (const LivelockFinding &f : static_.findings) {
        if (f.kind != LivelockKind::TerminalCycle || !f.certain)
            continue;
        ++terminal;
        for (StateId s : rg_.initialStates())
            if (std::binary_search(f.states.begin(), f.states.end(), s))
                initialInside = true;
    }
    if (terminal > 0) {
        warnings_.push_back(std::to_string(terminal) + " terminal cycle(s) without a target marking found: "
                            "by definition livelocks of the specification");
    }
    if (initialInside) {
        warnings_.push_back("an initial marking lies inside such a cycle: declare a target marking (e.g. the "
                            "idle state), otherwise every move counts as entering a livelock and is vetoed");
    }
    if (!static_.graphComplete) {
        warnings_.push_back("the model was not explored completely (state limit or bounds): the static "
                            "analysis may have missed livelocks");
    }
    return static_;
}

ReplayResult Session::restore(const PersistedRun &run)
{
    const ReplayResult result = replay(db_, rg_, run, cfg_.tracker);
    // GLM-5.3-Flash: continue the crash experiments in the recorded order
    // (before the pheromone rebuild, whose seek/catastrophe choice reads the
    // entry statuses this re-derives).
    if (result.ok)
        rebuildCrashExperiment(run);
    // GLM-5.3-Flash: rebuild the pheromones from the recording (doku section
    // 12, item 4). The replay only reconstructs coverage; whatever the live
    // run taught the colony has to be rebuilt from what was recorded.
    if (result.ok && cfg_.useAnts)
        rebuildPheromones(run, db_.run(result.run));
    return result;
}

// GLM-5.3-Flash: re-derive what a restored run contributed to the crash
// experiments, so a continued campaign continues each experiment where it
// stopped. The run's experiment TRAIL (the attempt keys in counting order)
// and the death's attribution travel with the recording: which credits the
// final settle adds after a death can be replayed from neither the steps nor
// the events, so the live session carried them. The rebuild applies the trail
// in order (the db counts only while an entry seeks, exactly as live), then
// the attribution -- creating an implicit entry when the log names one nobody
// predicted -- and notes the crash. Applied over the restored runs in order,
// the statuses evolve exactly as they did live.
void Session::rebuildCrashExperiment(const PersistedRun &run)
{
    if (run.outcome == RunOutcome::SutCrashed && !run.crashAttribution.empty()) {
        const std::string &key = run.crashAttribution;
        if (!db_.findCrashEntry(key) && key.rfind("t:", 0) == 0) {
            const auto t = net_.findTransition(key.substr(2));
            if (t)
                db_.addCrashEntry(key, *t, std::nullopt);
        }
    }
    for (const std::string &key : run.crashAttempts)
        db_.noteCrashAttempt(key);
    if (run.outcome == RunOutcome::SutCrashed && !run.crashAttribution.empty())
        db_.noteCrash(run.crashAttribution);
}

// GLM-5.3-Flash: what a finished run taught the colony, rebuilt from the
// recording (doku section 12, item 4). A restored session continues a
// campaign; without this, the pheromone knowledge would be lost whenever the
// tool is restarted, and the coming runs would walk straight back into a
// recorded crash and ignore a recorded milestone.
//
// The rebuild is a documented approximation of the live deposits. Live, the
// session deposits at every state the belief spanned and prices by the inputs
// it actually sent; a recording only carries the definite steps and the
// acknowledged events. What is rebuilt here:
//   - every definitely recorded step of a MILESTONE transition lays positive
//     pheromone on its recorded target state (AntConfig::milestoneDeposit;
//     certain-but-state-ambiguous milestone steps have no definite edge and
//     are not rebuilt);
//   - with the T-suffix layer on (GLM-5.3-Flash, doku section 16), the same
//     recorded steps also fill the (marking, T-suffix) pair store: one
//     milestone deposit at the arrival pair of its step, one catastrophe
//     deposit at the arrival pair of the run's end; both flat, the same
//     amounts as the edge deposits;
//   - a run that ended SutCrashed, and a run stranded in a recorded deadlock,
//     mark the way into their last recorded state as repellent, priced by the
//     way in exactly like the live deposits (AntConfig::catastropheDeposit:
//     flat for an "unpredictable" way in, flat with --flat-catastrophe) --
//     UNLESS the crash was attributed to a crash entry that still seeks
//     (GLM-5.3-Flash): that run taught the seek lesson live, the way in as
//     attractive (depositTarget), and the rebuild lays the same attract.
// Every amount is scaled by SessionConfig::restoredPheromonePercent
// (GLM-5.3-Flash, doku section 16): knowledge from earlier rounds enters a
// continued campaign toned down -- second-hand and possibly stale, it should
// weigh less than what the fresh round learns itself. 100 keeps the full
// strength, 0 rebuilds nothing.
// The colony's own walk deposits (unexplored territory, sought targets) are
// transient state of the walk, not knowledge about the SUT: not rebuilt.
void Session::rebuildPheromones(const PersistedRun &run, const RunRecord &rec)
{
    // GLM-5.3-Flash: inherited knowledge enters toned down (doku section 16).
    // Every deposit the rebuild lays -- milestone and catastrophe, edge
    // channels and (marking, T-suffix) pairs alike -- carries this factor of
    // the amount the live run laid. The merge of several rounds is the
    // accumulation it always was; the factor is applied per deposit, before
    // tauMax caps, so a much-marked edge stays capped at the same ceiling
    // while the single-visit marks around it fade.
    const double factor = static_cast<double>(cfg_.restoredPheromonePercent) / 100.0;

    // --- milestones: positive pheromone at the recorded step targets ---------
    // GLM-5.3-Flash: the (marking, T-suffix) pair layer walks the same steps
    // (doku section 16): the cascade is pushed per recorded step, and every
    // milestone step lands its pair deposit at the arrival pair the record
    // names -- the same walk the live run's cascade took over the same
    // record, so the rebuilt pair keys are the live ones. No
    // back-propagation: the edge channels still do the propagating, the
    // pairs only refine what they already say.
    TraceCascade trace;
    trace.setLength(cfg_.ants.tsuffixLength);
    for (const RunStep &step : rec.steps) {
        const TransitionIndex t = rg_.edge(step.edge).transition;
        if (t >= net_.transitions.size())
            continue;
        trace.push(stepHashOf(net_.transitions[t]));
        if (!net_.transitions[t].milestone)
            continue;
        const double amount = factor * cfg_.ants.milestoneDeposit(net_.transitions[t].milestoneRank);
        if (amount <= 0.0)
            continue;
        ants_.depositAttract(step.target, amount);
        ants_.depositPairAttract(step.target, trace, amount);
    }

    // --- catastrophes: the way into the last recorded state is repellent -----
    const StateId at = rec.steps.empty() ? rec.initial : rec.steps.back().target;
    // GLM-5.3-Flash: the graph grows lazily -- a replayed run's last state is
    // known but not necessarily explored, and isDeadlock is only meaningful
    // for expanded states. Explore it here (a no-op when it already is); the
    // live session had expanded it too before it ended the run stranded.
    if (at != kNoState)
        rg_.expand(at);
    const bool strandedInDeadlock =
        run.outcome == RunOutcome::Completed && run.detail.rfind("stuck:", 0) == 0
        && at != kNoState && rg_.isDeadlock(at);
    if (run.outcome != RunOutcome::SutCrashed && !strandedInDeadlock)
        return;

    // GLM-5.3-Flash: a crash attributed to an entry that still seeks taught
    // the SEEK lesson live: the way in is attractive (depositTarget), so the
    // continued campaign comes back and reproduces the crash. The rebuild
    // mirrors it at the last recorded arrival -- the single state a recording
    // can name, where the live deposit spanned the belief (the documented
    // approximation, same as the catastrophe below).
    if (run.outcome == RunOutcome::SutCrashed && !run.crashAttribution.empty()) {
        const CrashEntry *e = db_.findCrashEntry(run.crashAttribution);
        if (e && e->status == CrashStatus::Seeking) {
            const double amount = factor * cfg_.ants.depositTarget;
            if (amount > 0.0) {
                ants_.depositAttract(at, amount);
                ants_.depositPairAttract(at, trace, amount);
            }
            return;
        }
    }

    // The way in, in inputs: an event run carries the acknowledged stimuli; a
    // path run names its transitions, of which the controllable ones were the
    // sent inputs.
    std::size_t way = 0;
    if (!run.events.empty()) {
        for (const SutEvent &e : run.events)
            if (e.kind == EventKind::Stimulated)
                ++way;
    } else {
        for (const std::string &id : run.transitions) {
            const auto t = net_.findTransition(id);
            if (t && net_.transitions[*t].kind == TransitionKind::Controllable)
                ++way;
        }
    }
    // An "unpredictable" way in stays flat: the wall clock then measured a
    // human's reaction time, not the model's depth (same rule as live).
    const bool unpredictableWay = std::any_of(rec.transitions.begin(), rec.transitions.end(),
                                              [&](TransitionIndex t) {
                                                  return t < net_.transitions.size()
                                                         && net_.transitions[t].unpredictable;
                                              });
    if (!cfg_.catastropheTimePricing || unpredictableWay)
        way = 0;
    const double amount = factor * cfg_.ants.catastropheDeposit(way);
    if (amount > 0.0) {
        ants_.depositRepel(at, amount);
        // GLM-5.3-Flash: the same lesson lands in the (marking, T-suffix) pair
        // store (doku section 16), flat at the arrival pair: the last recorded
        // arrival -- the initial state when nothing was recorded -- with the
        // trace the run arrived there by. The live deposit sits after endRun for
        // the same reason the rebuild can use the final record here: both walk
        // the same recorded steps, so the pair keys agree.
        const StateId pairAt = rec.steps.empty() ? rec.initial : rec.steps.back().target;
        ants_.depositPairRepel(pairAt, trace, amount);
    }
}

std::size_t Session::pickInitialMarking()
{
    if (cfg_.initialMarkings.empty())
        return runCounter_ % net_.initialMarkings.size();
    return cfg_.initialMarkings[runCounter_ % cfg_.initialMarkings.size()];
}

RunReport Session::runOnce(ISutPlugin &plugin)
{
    using Clock = std::chrono::steady_clock;

    RunReport rep;
    if (net_.initialMarkings.empty()) {
        rep.detail = "the model has no initial marking";
        return rep;
    }

    const std::size_t initialIdx = pickInitialMarking();
    const Clock::time_point startedAt = Clock::now();  // GLM-5.3-Flash: run duration
    // DeepSeek V4.1 Flash: a direct runOnce with a budget gets its own deadline;
    // runCampaign already set the campaign-wide one.
    const bool ownDeadline = !deadline_ && cfg_.timeBudget.count() > 0;
    if (ownDeadline)
        deadline_ = startedAt + cfg_.timeBudget;
    const PluginCapabilities caps = plugin.capabilities();

    // DeepSeek V4.1 Flash: does this run hold synchronized events (doku section
    // 19)? The model opts in per transition and the campaign as a whole; the
    // plug-in must also be able to acknowledge. Without that capability the
    // run goes on unsynchronized -- and says exactly why, once per campaign.
    PluginRunOptions runOptions = cfg_.plugin;
    const bool syncWanted = cfg_.syncEvents && !syncLabels_.empty();
    if (syncWanted && !caps.eventAcks) {
        if (!syncNoteGiven_) {
            rep.notes.push_back(
                "the plug-in cannot acknowledge events (its capabilities do not offer \"sync\"): "
                "the synchronized events are not held, the run is not synchronized");
            syncNoteGiven_ = true;
        }
    } else if (syncWanted) {
        runOptions.syncLabels = syncLabels_;
    }

    queue_.reset();
    tracker_.beginRun(initialIdx, "session run " + std::to_string(runCounter_));
    ++runCounter_;
    rep.id = static_cast<RunId>(db_.numRuns() - 1);

    RunOutcome outcome = RunOutcome::Completed;
    std::string detail;
    bool ended = false;
    bool pluginStarted = false;

    const auto finish = [&](RunOutcome o, std::string d, StopReason why) {
        if (ended)
            return;
        ended = true;
        outcome = o;
        detail = std::move(d);
        if (pluginStarted)
            plugin.stopRun(why);
    };

    // Takes a post-mortem if the plug-in can; returns true if the SUT is gone afterwards.
    const auto takeDump = [&](DumpMode mode) -> bool {
        if (mode == DumpMode::None)
            return false;
        DumpMode use = mode;
        if (use == DumpMode::Crash && !caps.crash) {
            if (!caps.snapshot) {
                rep.notes.push_back("the plug-in cannot crash the SUT: no post-mortem taken");
                return false;
            }
            use = DumpMode::Snapshot;
            rep.notes.push_back("the plug-in cannot crash the SUT: took a snapshot instead");
        } else if (use == DumpMode::Snapshot && !caps.snapshot) {
            rep.notes.push_back("the plug-in cannot snapshot the SUT: no post-mortem taken");
            return false;
        }
        const PostMortem pm = plugin.postMortem(use);
        if (!pm.ok) {
            rep.notes.push_back("post-mortem failed: " + pm.error);
            return use == DumpMode::Crash;  // after a failed crash the SUT is most likely gone
        }
        tracker_.recordDump(use, pm.files, pm.stackText);
        return use == DumpMode::Crash;
    };

    std::string err;
    if (plugin.startRun(net_.initialMarkings[initialIdx].name, runOptions, queue_, &err))
        pluginStarted = true;
    else
        finish(RunOutcome::Aborted, "the plug-in could not start the run: " + err, StopReason::Aborted);

    std::size_t pending = 0;  // inputs sent but not yet reported back
    Clock::time_point sentAt = Clock::now();
    Clock::time_point lastActivity = Clock::now();
    bool changed = true;      // something happened since the last suggestion
    bool waiting = false;     // the last suggestion was to wait for the SUT
    std::optional<std::size_t> inError;
    std::optional<TransitionIndex> inErrorTransition;  // GLM-5.3-Flash: reaction-once guard
    std::optional<std::size_t> milestoneDepositedAt;   // GLM-5.3-Flash: tracker event count of the last milestone deposit
    bool offeredThisEpisode = false;  // GLM-5.3-Flash: the current wait episode already offered the expected labels
    bool sutGone = false;     // GLM-5.3-Flash: a crash post-mortem was taken (or attempted)

    // GLM-5.3-Flash: the run ended because the SUT died on its own, and the
    // crash entry ("t:<id>" / "m:<name>") the death was attributed to --
    // empty when nothing was credited (unattributed: a catastrophe at once).
    bool diedSpontaneously = false;
    std::string deathAttribution;

    // GLM-5.3-Flash: the predicted MARKINGS this run already counted an
    // attempt for (one attempt per run, at the first certain moment).
    std::vector<std::string> markingAttempts;

    // GLM-5.3-Flash: the belief a hook's "when" guard reads (doku section
    // 18): the belief after the step that made its transition certain. Set
    // right before every advanceCascade call -- per event the live belief,
    // after the run's final settle the belief captured before endRun (the
    // tracker clears its belief there, and a credit is a fact about the run,
    // not about the cleared tracker).
    std::vector<StateId> hookGuardBelief;

    // GLM-5.3-Flash: executes ONE command hook (doku section 18). The command
    // line runs through sh -c in eggembt's working directory; the model's data is
    // never interpolated into it -- context arrives as environment variables
    // (EGGEMBT_TRANSITION, EGGEMBT_TRANSITION_LABEL, EGGEMBT_RUN, and EGGEMBT_LOG when a run
    // log is open) beside the inherited environment. A synchronous command
    // blocks the run loop (the SUT's own events queue up meanwhile) and is
    // bounded by --hook-timeout: a child that runs longer is killed and its
    // execution counts as failed; its stdout+stderr is captured (truncated)
    // into the run record. An asynchronous one is fire-and-forget: eggembt does
    // not wait for it and never kills it; the child is reaped -- not killed --
    // when the session dies. A failure is a note ("Note only, run continues"):
    // the campaign carries on.
    const auto executeHook = [&](TransitionIndex t, const CommandHook &hook) {
        const Transition &tr = net_.transitions[t];
        if (hook.times && hooksFired_[t] >= *hook.times)
            return;  // the campaign's budget for this hook is spent
        const auto guardHolds = [&] {
            if (hook.when == kNoPlace)
                return true;
            for (const StateId s : hookGuardBelief) {
                const Marking m = rg_.marking(s);
                if (hook.when < m.size() && m[hook.when] == 0)
                    return false;  // the guard place is empty in some believed state
            }
            return true;
        };
        if (!guardHolds())
            return;

        HookRecord rec;
        rec.transition = tr.id;
        rec.label = tr.label;
        rec.async = hook.mode == HookMode::Async;

        if (rec.async) {
            // Fire-and-forget: the output goes to /dev/null, the child is
            // reaped (never killed) at the session's destruction.
            const pid_t pid = ::fork();
            if (pid == 0) {
                const int devnull = ::open("/dev/null", O_WRONLY);
                if (devnull >= 0) {
                    ::dup2(devnull, STDOUT_FILENO);
                    ::dup2(devnull, STDERR_FILENO);
                    if (devnull > STDERR_FILENO)
                        ::close(devnull);
                }
                ::setenv("EGGEMBT_TRANSITION", tr.id.c_str(), 1);
                ::setenv("EGGEMBT_TRANSITION_LABEL", tr.label.c_str(), 1);
                const std::string runId = std::to_string(rep.id);
                ::setenv("EGGEMBT_RUN", runId.c_str(), 1);
                if (!cfg_.logPath.empty())
                    ::setenv("EGGEMBT_LOG", cfg_.logPath.c_str(), 1);
                ::execl("/bin/sh", "sh", "-c", hook.run.c_str(), static_cast<char *>(nullptr));
                ::_exit(127);  // exec failed
            }
            if (pid > 0)
                hookChildren_.push_back(pid);
            rec.status = pid > 0 ? "launched" : "failed";  // a failed fork is a failed launch
            rec.exit = kHookNoExit;
            db_.recordHookRun(std::move(rec));
            if (pid <= 0)
                rep.notes.push_back("the command of '" + tr.id + "' could not be launched: "
                                    + std::strerror(errno));
            ++hooksFired_[t];
            return;
        }

        // Synchronous: capture stdout+stderr through a pipe, bounded by the
        // hook timeout.
        int out[2];
        if (::pipe(out) != 0) {
            rec.status = "failed";
            rec.exit = kHookNoExit;
            db_.recordHookRun(std::move(rec));
            rep.notes.push_back("the command of '" + tr.id + "' could not be launched: "
                                + std::strerror(errno));
            ++hooksFired_[t];
            return;
        }
        const pid_t pid = ::fork();
        if (pid < 0) {
            ::close(out[0]);
            ::close(out[1]);
            rec.status = "failed";
            rec.exit = kHookNoExit;
            db_.recordHookRun(std::move(rec));
            rep.notes.push_back("the command of '" + tr.id + "' could not be launched: "
                                + std::strerror(errno));
            ++hooksFired_[t];
            return;
        }
        if (pid == 0) {  // child
            ::dup2(out[1], STDOUT_FILENO);
            ::dup2(out[1], STDERR_FILENO);
            ::close(out[0]);
            ::close(out[1]);
            ::setenv("EGGEMBT_TRANSITION", tr.id.c_str(), 1);
            ::setenv("EGGEMBT_TRANSITION_LABEL", tr.label.c_str(), 1);
            const std::string runId = std::to_string(rep.id);
            ::setenv("EGGEMBT_RUN", runId.c_str(), 1);
            if (!cfg_.logPath.empty())
                ::setenv("EGGEMBT_LOG", cfg_.logPath.c_str(), 1);
            ::execl("/bin/sh", "sh", "-c", hook.run.c_str(), static_cast<char *>(nullptr));
            ::_exit(127);  // exec failed (sh not runnable at all)
        }

        // parent
        ::close(out[1]);
        std::string captured;
        bool truncated = false;
        bool timedOut = false;
        {
            constexpr std::size_t kMaxOutput = 4096;  // GLM-5.3-Flash: capture budget
            const auto deadline = Clock::now() + std::chrono::seconds(cfg_.hookTimeoutSeconds);
            char buf[512];
            bool eof = false;
            while (!eof) {
                const auto remain = std::chrono::duration_cast<std::chrono::milliseconds>(
                    deadline - Clock::now()).count();
                if (remain <= 0) {
                    timedOut = true;
                    break;
                }
                ::pollfd pfd{out[0], POLLIN, 0};
                const int r = ::poll(&pfd, 1, static_cast<int>(remain));
                if (r < 0) {
                    if (errno == EINTR)
                        continue;
                    break;
                }
                if (r == 0)
                    continue;  // re-checked the deadline
                const ssize_t n = ::read(out[0], buf, sizeof buf);
                if (n <= 0) {
                    eof = n == 0 || errno != EINTR;
                    if (n < 0 && errno != EINTR)
                        break;
                    continue;
                }
                const std::size_t got = static_cast<std::size_t>(n);
                const std::size_t room =
                    captured.size() < kMaxOutput ? kMaxOutput - captured.size() : 0;
                if (room > 0) {
                    const std::size_t take = std::min(got, room);
                    captured.append(buf, take);
                    if (take < got)
                        truncated = true;  // the capture budget is spent
                } else {
                    truncated = true;
                }
            }
            if (truncated)
                captured += "\n[output truncated]";
        }
        ::close(out[0]);
        if (timedOut) {
            ::kill(pid, SIGKILL);  // the child is past its deadline
            int status = 0;
            while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) { }
            rec.status = "timeout";
            rec.exit = kHookNoExit;
            rep.notes.push_back("the command of '" + tr.id + "' timed out after "
                                + std::to_string(cfg_.hookTimeoutSeconds) + " s: the run continues");
        } else {
            int status = 0;
            while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) { }
            if (WIFEXITED(status)) {
                rec.exit = WEXITSTATUS(status);
                rec.status = rec.exit == 0 ? "ok" : "failed";
                if (rec.exit != 0)
                    rep.notes.push_back("the command of '" + tr.id + "' failed (exit "
                                        + std::to_string(rec.exit) + "): the run continues");
            } else {
                rec.exit = kHookNoExit;  // killed by a signal of its own
                rec.status = "failed";
                rep.notes.push_back("the command of '" + tr.id + "' failed: the run continues");
            }
        }
        rec.output = std::move(captured);
        db_.recordHookRun(std::move(rec));
        ++hooksFired_[t];
    };

    // GLM-5.3-Flash: the run's T-suffix cascade (doku section 16). CoverageDb
    // credits a RunStep only for a step that went over an edge, so the run
    // record's steps are exactly the trace the cascade follows; the restore's
    // rebuild replays the same walk over the same record, which keeps the
    // live and rebuilt pair keys identical. Every newly credited step is
    // pushed here, and a credited milestone step lands its pair deposit at
    // the arrival pair the record names -- the same rule the rebuild applies
    // to every recorded milestone step, whether it was credited mid-run or by
    // the run's final settle. (The milestone reaction site below still does
    // the belief-spanning edge-channel deposit and the report bookkeeping.)
    std::size_t cascadedSteps = 0;
    // GLM-5.3-Flash: the crash attempts are counted from the transitions the
    // tracker CREDITED (one entry per certainly fired step, however many
    // edges it spanned) -- a different stream than the run record's steps,
    // which a multi-state certain step fans out over. The cursor freezes
    // when the SUT dies: the experiment of a dead run is closed, and the
    // credits the final settle adds afterwards (the death step's own credit,
    // retroactively resolved segments) belong to no attempt.
    std::size_t cascadedTransitions = 0;
    bool crashCursorFrozen = false;
    // GLM-5.3-Flash: the command hooks follow the same credited stream (one
    // execution per certainly fired step); their cursor is never frozen.
    std::size_t cascadedHooks = 0;
    const auto advanceCascade = [&] {
        const RunRecord &steps = db_.run(rep.id);
        while (cascadedSteps < steps.steps.size()) {
            const TransitionIndex t = rg_.edge(steps.steps[cascadedSteps].edge).transition;
            run_.push(stepHashOf(net_.transitions[t]));
            if (cfg_.useAnts && net_.transitions[t].milestone)
                ants_.depositPairAttract(steps.steps[cascadedSteps].target, run_,
                                         cfg_.ants.milestoneDeposit(net_.transitions[t].milestoneRank));
            ++cascadedSteps;
        }
        if (!crashCursorFrozen) {
            while (cascadedTransitions < steps.transitions.size()) {
                const TransitionIndex t = steps.transitions[cascadedTransitions++];
                // Every definite visit of a crash site is one attempt while
                // its experiment is unresolved: of a PREDICTED transition
                // (the net says so) and of an IMPLICIT one (the db created
                // its entry at an earlier death; the db ignores unknown keys
                // and resolved entries, so a transition nobody ever crashed
                // on costs nothing).
                if (t < net_.transitions.size())
                    db_.noteCrashAttempt("t:" + net_.transitions[t].id);
            }
        }
        // GLM-5.3-Flash: the command hooks fire from the same credited stream
        // (doku section 18), one execution per certainly fired step, in
        // counting order. The cursor is NOT frozen at the SUT's death: a step
        // the SUT certainly took is a step, whatever happened to the SUT
        // afterwards -- its command still runs. (The crash attempts above
        // stay frozen: the experiment of a dead run is closed.)
        while (cascadedHooks < steps.transitions.size()) {
            const TransitionIndex t = steps.transitions[cascadedHooks++];
            if (t < net_.transitions.size() && net_.transitions[t].command)
                executeHook(t, *net_.transitions[t].command);
        }
    };

    while (!ended) {
        // DeepSeek V4.1 Flash: the wall-clock budget (doku section 27). Checked
        // at every loop turn, so a run that would keep testing ends as a
        // normal Completed run -- the budget is a planning limit, not a fault.
        if (deadline_ && Clock::now() >= *deadline_) {
            rep.budgetReached = true;
            finish(RunOutcome::Completed, "campaign time budget reached", StopReason::Finished);
            break;
        }
        if (stop_) {
            finish(RunOutcome::Aborted, "stopped on request", StopReason::Aborted);
            break;
        }
        if (queue_.overflowed()) {
            finish(RunOutcome::Aborted, "event queue overflow: events were lost", StopReason::Aborted);
            break;
        }
        const std::string fault = plugin.pollFault();
        if (!fault.empty()) {
            finish(RunOutcome::Aborted, "plug-in fault: " + fault, StopReason::Aborted);
            break;
        }

        queue_.waitForEvents(cfg_.poll);  // returns at once if events are queued
        // GLM-5.3-Flash: pumped one event at a time so every model-known
        // Observed event can be credited as "taken" (the offered/taken
        // statistics, doku section 12 item 3). The aggregate is what a single
        // pumpEvents call would have returned.
        PumpResult pump;
        bool sawDied = false;  // GLM-5.3-Flash: a Died event came through this pump
        for (;;) {
            const PumpResult one = pumpEvents(queue_, tracker_, 1);
            if (one.processed == 0)
                break;
            pump.processed += one.processed;
            pump.stimulated += one.stimulated;
            pump.lastSeq = one.lastSeq;
            pump.last = one.last;
            pump.lastKind = one.lastKind;
            pump.lastLabel = one.lastLabel;
            pump.lastNeedsAck = one.lastNeedsAck;  // DeepSeek V4.1 Flash
            pump.lastAckSeq = one.lastAckSeq;
            // The SUT took a move the model knew about: credit its label. A
            // Deviation is NOT a take (the model did not allow the move), and
            // neither are quiescence steps nor the tester's own Stimulated
            // acknowledgements.
            if (one.lastKind == EventKind::Observed && one.last.status == StepStatus::Ok)
                db_.recordTaken(one.lastLabel);
            // DeepSeek V4.1 Flash: the reserved lifecycle inputs (doku section 26)
            // are worth a note: the engine just moved the run's position (or
            // closed the gate), and the report should say so.
            if (one.lastKind == EventKind::Stimulated && one.last.status == StepStatus::Ok) {
                for (const Transition &t : net_.transitions) {
                    if (t.kind != TransitionKind::Controllable || t.label != one.lastLabel
                        || t.lifecycle == LifecycleAction::None)
                        continue;
                    switch (t.lifecycle) {
                    case LifecycleAction::Start:
                        rep.notes.push_back("the SUT was started by the model's 'start' input");
                        break;
                    case LifecycleAction::Restart:
                        rep.notes.push_back("the SUT was restarted by the model's 'restart' input");
                        break;
                    case LifecycleAction::Stop:
                        rep.notes.push_back("the SUT was stopped by the model's 'stop' input: only start and "
                                            "restart are offered until it is up again");
                        break;
                    case LifecycleAction::Pause:
                        rep.notes.push_back("the SUT was paused by the model's 'pause' input");
                        break;
                    case LifecycleAction::Resume:
                        rep.notes.push_back("the SUT was resumed by the model's 'resume' input");
                        break;
                    case LifecycleAction::None:
                        break;
                    }
                    break;
                }
            }
            // GLM-5.3-Flash: the plug-in reported the SUT died on its own.
            if (one.lastKind == EventKind::Died)
                sawDied = true;
            // GLM-5.3-Flash: the credited stream may have grown with this
            // event: the T-suffix cascade, the crash attempts and the command
            // hooks follow it per event, so a hook's "when" guard reads the
            // belief right after the event that made the step certain
            // (doku section 18). A synchronous hook blocks the loop here --
            // the SUT's own events queue up meanwhile.
            hookGuardBelief = tracker_.belief();
            advanceCascade();
            if (one.runEnded) {
                pump.runEnded = true;
                break;
            }
            // DeepSeek V4.1 Flash: the SUT is held at this event (doku section 19).
            // Stop pumping: the reactions below -- including a post-mortem,
            // which must see the stack frozen -- run before the ack.
            if (one.lastNeedsAck)
                break;
        }
        if (queue_.overflowed()) {
            finish(RunOutcome::Aborted, "event queue overflow: events were lost", StopReason::Aborted);
            break;
        }
        if (pump.processed > 0) {
            lastActivity = Clock::now();
            changed = true;
            offeredThisEpisode = false;  // GLM-5.3-Flash: the next wait is a new episode
        }
        pending -= std::min(pending, pump.stimulated);

        // --- the tracker says the run is over ------------------------------------------
        if (pump.runEnded) {
            RunOutcome o = RunOutcome::Aborted;
            DumpMode mode = DumpMode::None;
            std::string text = pump.last.message;
            switch (pump.last.status) {
            case StepStatus::Deviation:
                o = RunOutcome::Deviation;
                mode = cfg_.dumps.onDeviation;
                if (!pump.last.expected.empty())
                    text += " (expected: " + joinLabels(pump.last.expected) + ")";
                if (pump.last.modelIncomplete)
                    text += " [the explored model may be incomplete here]";
                break;
            case StepStatus::Deadlock:
                o = RunOutcome::Deadlock;
                mode = cfg_.dumps.onDeadlock;
                break;
            case StepStatus::Livelock:
                o = RunOutcome::Livelock;
                mode = cfg_.dumps.onLivelock;
                break;
            default:
                break;
            }
            // GLM-5.3-Flash: remember that the SUT really died here, so the
            // colony can learn from it below.
            sutGone = takeDump(mode);
            finish(o, text, StopReason::Finished);
            break;
        }

        // --- the SUT died on its own (GLM-5.3-Flash) -------------------------------------
        //
        // Egge: "Sometimes an SUT crash is nondeterministic. It can be
        // random, rare, sometimes, or often (but not always). Any of these
        // indicate a bug. Therefore an SUT crash should be treated as a
        // target until we are certain that it is deterministic." The death is
        // attributed first: to the predicted transition of the last credited
        // step, else to a predicted marking the SUT was certainly in, else to
        // an implicit entry at the last credited step's transition. The
        // attributed death is one hit of that entry's reproduction experiment
        // (the attempt of the death step was already counted above); an entry
        // that still seeks afterwards teaches SEEKING below, a resolved one
        // teaches the catastrophe like every other crash. A death with no
        // credited step at all stays unattributed and is a catastrophe at
        // once, as every crash has always been.
        if (sawDied) {
            diedSpontaneously = true;
            crashCursorFrozen = true;  // GLM-5.3-Flash: the experiment of this run is closed
            // The death batch never reaches the per-event marking attempts
            // below (the run ends here), so the predicted markings the SUT
            // was certainly in at death count their attempt now -- a hit
            // needs its visit first.
            const auto noteDeathMarkingAttempt = [&](const NamedMarking &m) {
                const std::string key = "m:" + m.name;
                if (std::find(markingAttempts.begin(), markingAttempts.end(), key) != markingAttempts.end())
                    return;
                markingAttempts.push_back(key);
                db_.noteCrashAttempt(key);
            };
            if (const auto em = tracker_.definiteErrorMarking()) {
                if (net_.errorMarkings[*em].crash)
                    noteDeathMarkingAttempt(net_.errorMarkings[*em]);
            } else if (const auto tm = tracker_.definiteTargetMarking()) {
                if (net_.targetMarkings[*tm].crash)
                    noteDeathMarkingAttempt(net_.targetMarkings[*tm]);
            }
            // GLM-5.3-Flash: the death step's own visit is counted exactly
            // once. When its step settled at the event itself (the belief was
            // a single state), the cascade above already counted it; when the
            // belief was ambiguous, the settle only comes with the run's
            // final settle (after the frozen cursor), so nobody counted it
            // yet. The implicit entry born at this very death missed the
            // cascade's call as well (the db drops attempts on keys without
            // an entry) -- it counts here instead.
            const auto deathStepCounted = [&](TransitionIndex t) {
                const RunRecord &stepsNow = db_.run(rep.id);
                return std::find(stepsNow.transitions.begin(), stepsNow.transitions.end(), t)
                       != stepsNow.transitions.end();
            };
            const std::string reason = pump.lastLabel;
            // The step in flight when the SUT died: the last event's certain
            // step (known at once, before its settle), or -- when that step
            // was not certain -- the last transition the tracker credited.
            const TransitionIndex lastT = tracker_.definiteStep().value_or(
                [&] {
                    const RunRecord &stepsNow = db_.run(rep.id);
                    return stepsNow.transitions.empty() ? kNoTransition : stepsNow.transitions.back();
                }());
            // The predicted transition of the last credited step wins: the
            // death happened at the step in flight.
            if (lastT != kNoTransition && net_.transitions[lastT].crashPrediction) {
                deathAttribution = "t:" + net_.transitions[lastT].id;
                // The death step itself was this visit of the place: counted
                // above by the cascade when it settled in time, here when it
                // did not -- exactly once either way.
                if (!deathStepCounted(lastT))
                    db_.noteCrashAttempt(deathAttribution);
            }
            // Then a predicted marking the SUT was certainly in at death --
            // possible even when nothing was credited (silent drift).
            if (deathAttribution.empty()) {
                if (const auto em = tracker_.definiteErrorMarking()) {
                    if (net_.errorMarkings[*em].crash)
                        deathAttribution = "m:" + net_.errorMarkings[*em].name;
                } else if (const auto tm = tracker_.definiteTargetMarking()) {
                    if (net_.targetMarkings[*tm].crash)
                        deathAttribution = "m:" + net_.targetMarkings[*tm].name;
                }
            }
            // Then the implicit entry: nobody predicted this crash; it is
            // keyed by the last credited step's transition, and the death's
            // own step was the entry's first visit.
            if (deathAttribution.empty() && lastT != kNoTransition) {
                deathAttribution = "t:" + net_.transitions[lastT].id;
                const bool born = db_.findCrashEntry(deathAttribution) == nullptr;
                db_.addCrashEntry(deathAttribution, lastT, std::nullopt);
                // The entry's opening visit: the cascade's call was dropped
                // on the missing entry when it is born here, and the death
                // step may not have settled at all -- exactly once either way.
                if (born || !deathStepCounted(lastT))
                    db_.noteCrashAttempt(deathAttribution);
            }
            if (deathAttribution.empty())
                rep.notes.push_back("the SUT died without a credited step: the crash cannot be attributed");
            if (!deathAttribution.empty()) {
                db_.recordCrashAttribution(deathAttribution);
                db_.noteCrash(deathAttribution);
                const CrashEntry *e = db_.findCrashEntry(deathAttribution);
                rep.notes.push_back("the crash was attributed to " + deathAttribution
                                    + " (" + std::string(toString(e->status))
                                    + ": " + std::to_string(e->stats.attempts) + " attempt(s), "
                                    + std::to_string(e->stats.crashes) + " crash(es))");
                // the prediction's post-mortem request, with its own budget;
                // the SUT is already down, so the plug-in may well refuse
                if (db_.crashDumpAllowed(deathAttribution) && takeDump(e->prediction->crashDump))
                    sutGone = true;  // a post-mortem was taken for the dead SUT
                if (e->status != CrashStatus::Seeking)
                    sutGone = true;  // resolved: the death teaches the catastrophe (below)
            } else {
                sutGone = true;  // unattributed: a catastrophe at once, as every crash has always been
            }
            finish(RunOutcome::SutCrashed,
                   reason.empty() ? std::string("the SUT died") : "the SUT died: " + reason,
                   StopReason::Finished);
            break;
        }

        // --- the SUT is certainly in an error marking -----------------------------------
        const std::optional<std::size_t> inErrorNow = tracker_.definiteErrorMarking();
        if (inErrorNow && inErrorNow != inError) {
            inError = inErrorNow;
            const NamedMarking &m = net_.errorMarkings[*inErrorNow];
            rep.errorsReached.push_back(m.name);
            if (m.crashDump != DumpMode::None && db_.dumpAllowed(*inErrorNow)) {
                if (takeDump(m.crashDump)) {
                    sutGone = true;  // GLM-5.3-Flash: the SUT was crashed here
                    finish(RunOutcome::SutCrashed,
                           "error marking '" + m.name + "': the SUT was crashed for a post-mortem",
                           StopReason::Finished);
                    break;
                }
            }
        } else if (!inErrorNow) {
            inError.reset();
        }

        // GLM-5.3-Flash: a run that is certainly in a predicted MARKING counts
        // one attempt of that prediction's crash experiment -- once per run,
        // at the first certain moment (the same "certainly in" rule the
        // attribution uses). The db ignores it once the prediction resolved.
        if (inErrorNow) {
            const NamedMarking &m = net_.errorMarkings[*inErrorNow];
            if (m.crash
                && std::find(markingAttempts.begin(), markingAttempts.end(), "m:" + m.name)
                       == markingAttempts.end()) {
                markingAttempts.push_back("m:" + m.name);
                db_.noteCrashAttempt("m:" + m.name);
            }
        }
        if (const auto tm = tracker_.definiteTargetMarking()) {
            const NamedMarking &m = net_.targetMarkings[*tm];
            if (m.crash
                && std::find(markingAttempts.begin(), markingAttempts.end(), "m:" + m.name)
                       == markingAttempts.end()) {
                markingAttempts.push_back("m:" + m.name);
                db_.noteCrashAttempt("m:" + m.name);
            }
        }

        // GLM-5.3-Flash: the SUT certainly stepped an error TRANSITION. Same
        // reaction as for an error marking: record it, take the post-mortem
        // its settings ask for (its own max_dumps budget), end the run on a
        // Crash. The reaction fires once per distinct transition, like the
        // marking's does. A certain error MARKING wins: when the step also
        // landed the SUT in an error marking, the block above already reacted
        // (and recordDump prefers the marking's budget).
        std::optional<TransitionIndex> inErrorTransNow;
        if (!inErrorNow)
            inErrorTransNow = tracker_.definiteErrorTransition();
        if (inErrorTransNow && inErrorTransNow != inErrorTransition) {
            inErrorTransition = inErrorTransNow;
            const Transition &t = net_.transitions[*inErrorTransNow];
            rep.errorsReached.push_back(t.label.empty() ? t.id : t.label);
            if (t.crashDump != DumpMode::None && db_.errorTransitionDumpAllowed(*inErrorTransNow)) {
                if (takeDump(t.crashDump)) {
                    sutGone = true;  // the SUT was crashed here
                    finish(RunOutcome::SutCrashed,
                           "error transition '" + t.id + "': the SUT was crashed for a post-mortem",
                           StopReason::Finished);
                    break;
                }
            }
        }

        // GLM-5.3-Flash: the SUT certainly stepped a MILESTONE transition.
        // Reaching a milestone is a success, so the reaction is the opposite
        // of the error one: positive pheromones (AntConfig::depositMilestone,
        // times the milestone's rank) at every state the belief spans, once
        // per step. Same certainty rule as the error transition: only a
        // *certain* step reacts (definiteMilestoneTransition); ambiguous steps
        // never reach one. Guarded by the tracker's event count: certainStep_
        // survives until the next event or a quiescence step, so a repeated
        // milestone step (a loop through the milestone) deposits again. An
        // error step of the same event wins: the run is failing there, not
        // succeeding. Runs the ants are switched off for skip the deposit.
        if (cfg_.useAnts && !inErrorTransNow) {
            if (const auto ms = tracker_.definiteMilestoneTransition()) {
                if (!milestoneDepositedAt || *milestoneDepositedAt != tracker_.eventCount()) {
                    milestoneDepositedAt = tracker_.eventCount();
                    const Transition &t = net_.transitions[*ms];
                    rep.milestonesReached.push_back(t.label.empty() ? t.id : t.label);
                    const double amount = cfg_.ants.milestoneDeposit(t.milestoneRank);
                    for (const StateId s : tracker_.belief())
                        ants_.depositAttract(s, amount);
                }
            }
        }

        // GLM-5.3-Flash: the SUT is certainly in an intentional ("good") target
        // marking. This is the campaign's normal target reached: the run and
        // the whole campaign end here. Non-intentional target markings never
        // end a run (they are idle states for the livelock analysis).
        if (const auto goodNow = tracker_.definiteTargetMarking()) {
            const NamedMarking &m = net_.targetMarkings[*goodNow];
            if (m.intentional) {
                rep.goodTarget = m.name;
                finish(RunOutcome::Completed,
                       "good target marking '" + m.name + "': the campaign ends here",
                       StopReason::Finished);
                break;
            }
        }

        // DeepSeek V4.1 Flash: every consequence of the held event has been
        // handled above (a post-mortem saw the SUT frozen), so let it go on.
        // A run that is ending does not ack: stop_run releases the SUT.
        if (pump.lastNeedsAck && !ended) {
            plugin.acknowledgeEvent(pump.lastAckSeq);
            pump.lastNeedsAck = false;
        }

        if (tracker_.eventCount() >= cfg_.maxEventsPerRun) {
            finish(RunOutcome::Completed, "run length limit", StopReason::Finished);
            break;
        }

        // --- an input is in flight: wait for its report ---------------------------------
        if (pending > 0) {
            if (cfg_.stimulusAckTimeout.count() > 0 && Clock::now() - sentAt > cfg_.stimulusAckTimeout)
                finish(RunOutcome::Aborted, "the plug-in never reported the input it was asked to send",
                       StopReason::Aborted);
            continue;
        }

        if (!changed) {
            // DeepSeek V4.1 Flash: a paused or down SUT is expected to be silent
            // (doku section 26); the idle timeout is a fault only while it runs.
            if (waiting && cfg_.idleTimeout.count() > 0
                && tracker_.lifecycleGate() == LifecycleGate::Running
                && Clock::now() - lastActivity > cfg_.idleTimeout)
                finish(RunOutcome::Aborted,
                       "no event from the SUT for " + std::to_string(cfg_.idleTimeout.count())
                           + " ms although one was expected",
                       StopReason::Aborted);
            continue;
        }
        changed = false;

        // --- guidance ---------------------------------------------------------------------
        if (cfg_.useAnts) {
            for (std::size_t i = 0; i < cfg_.antIterationsPerSuggestion; ++i)
                ants_.iterate(tracker_.belief(), /*evaporate=*/!cfg_.evaporateBetweenRuns);
        }
        const Suggestion s = suggester_.suggest(tracker_);
        waiting = false;
        switch (s.status) {
        case SuggestionStatus::Suggest:
            if (plugin.stimulate(s.label, &err)) {
                ++pending;
                sentAt = Clock::now();
                ++rep.stimuli;
            } else {
                finish(RunOutcome::Aborted, "could not send input '" + s.label + "': " + err,
                       StopReason::Aborted);
            }
            break;
        case SuggestionStatus::Wait:
            // GLM-5.3-Flash: a wait episode OFFERS the SUT every move the
            // model expects now, once per episode (the guard resets on the
            // next event or suggestion). What the SUT does with the offer is
            // credited above as it arrives.
            if (!offeredThisEpisode) {
                offeredThisEpisode = true;
                for (const std::string &l : tracker_.expectedLabels())
                    db_.recordOffered(l);
            }
            waiting = true;
            break;
        case SuggestionStatus::Done:
        case SuggestionStatus::Stuck:
            if (!tracker_.expectedLabels().empty()) {
                waiting = true;  // not settled: the SUT may still be busy, keep listening
                break;
            }
            rep.coverageDone = s.status == SuggestionStatus::Done;
            rep.stuck = s.status == SuggestionStatus::Stuck;
            finish(RunOutcome::Completed,
                   std::string(rep.coverageDone ? "coverage complete: " : "stuck: ") + s.reason,
                   StopReason::Finished);
            break;
        }
    }

    // A run that simply ran out of things to do while the SUT sits in an error marking ended there.
    if (outcome == RunOutcome::Completed) {
        if (const auto em = tracker_.definiteErrorMarking()) {
            outcome = RunOutcome::ErrorReached;
            detail = net_.errorMarkings[*em].name + (detail.empty() ? "" : " (" + detail + ")");
        }
    }
    // GLM-5.3-Flash: a real catastrophe teaches the colony. When the SUT was
    // crashed for a post-mortem (or the crash was attempted), the way into
    // wherever the run ended is marked as repellent. Root-causing a crash
    // takes wall-clock time -- an engineer reads the dump, the SUT is
    // restarted -- and during that time the coming runs should test somewhere
    // else instead of re-finding the crash. This is the AntColony hook for
    // "callers with their own knowledge (e.g. a catastrophe seen in a real
    // run)": the model's own target phases cannot know about a crash the SUT
    // really suffered, and a target that is still being sought would otherwise
    // keep pulling the campaign straight back in.
    //
    // A run that ended stranded in a deadlock teaches the same lesson: the SUT
    // hung there, the run is lost and someone has to recover it -- wall-clock
    // time again. (A livelock ends the run through its own outcome; if it is
    // configured to take a crash post-mortem, the crash case above covers it.)
    //
    // GLM-5.3-Flash: how strong that lesson is depends on the way in. An error
    // state deserves worse pheromones the more time it takes to reach it: the
    // deposit grows with the inputs the run spent getting there, because a
    // repeat visit pays that way in again, in wall-clock time.
    const RunRecord &rec = db_.run(rep.id);
    double catastropheAmount = 0.0;  // GLM-5.3-Flash: remembered for the pair deposit after endRun
    double crashAttractAmount = 0.0;  // GLM-5.3-Flash: same, for a sought crash's attract deposit
    if (cfg_.useAnts) {
        const std::vector<StateId> belief = tracker_.belief();
        const bool strandedInDeadlock =
            rep.stuck && !belief.empty()
            && std::all_of(belief.begin(), belief.end(), [&](StateId s) { return rg_.isDeadlock(s); });

        // GLM-5.3-Flash: a spontaneous death attributed to a crash entry that
        // still seeks teaches the OPPOSITE of the catastrophe: the way in is
        // attractive (AntConfig::depositTarget, like a target marking), so
        // the campaign comes back and reproduces the crash -- every visit is
        // one more attempt of the binomial experiment. Flat, not priced by
        // the way in: seeking a reproduction is the point, not avoiding a
        // cost. Everything else keeps today's lesson (the branch below).
        const CrashEntry *deathEntry =
            diedSpontaneously && !deathAttribution.empty() ? db_.findCrashEntry(deathAttribution) : nullptr;
        if (deathEntry && deathEntry->status == CrashStatus::Seeking) {
            const double amount = cfg_.ants.depositTarget;
            for (const StateId s : belief)
                ants_.depositAttract(s, amount);
            crashAttractAmount = amount;
            rep.notes.push_back("the ant colony marked the way into the crash site as attractive: "
                                "the crash is still being sought (" + deathAttribution + ")");
        } else if (sutGone || strandedInDeadlock) {
            // GLM-5.3-Flash: price the deposit by the way in. The longer the
            // path the run walked to reach the error state, the more campaign
            // time a repeat visit wastes -- the wall-clock time of getting
            // there grows with the inputs sent -- so the stronger the repel
            // the colony lays down (see AntConfig::catastropheDeposit).
            //
            // Two switches keep the price flat (the base deposit, a way in of
            // length 0): the feature can be switched off entirely
            // (SessionConfig::catastropheTimePricing, --flat-catastrophe), and
            // a way in that crossed an "unpredictable" transition -- one a
            // human operator drives -- stays flat too, because the wall clock
            // then measured a human's reaction time, not the model's depth.
            const bool unpredictableWay = std::any_of(rec.transitions.begin(), rec.transitions.end(),
                                                      [&](TransitionIndex t) {
                                                          return t < net_.transitions.size()
                                                                 && net_.transitions[t].unpredictable;
                                                      });
            const std::size_t way = (!cfg_.catastropheTimePricing || unpredictableWay) ? 0 : rep.stimuli;
            const double amount = cfg_.ants.catastropheDeposit(way);
            for (const StateId s : belief)
                ants_.depositRepel(s, amount);
            catastropheAmount = amount;
            rep.notes.push_back(sutGone
                                    ? "the ant colony marked the crash site as repellent for the coming runs"
                                    : "the ant colony marked the way into the deadlock as repellent for the coming runs");
        }
    }
    // GLM-5.3-Flash: the belief the run's final settle credits against (the
    // tracker clears its belief in endRun, so the hooks fired afterwards read
    // this copy: a credit is a fact about the run, not about the tracker).
    const std::vector<StateId> finalBelief = tracker_.belief();
    tracker_.endRun(outcome, detail);

    // GLM-5.3-Flash: the run's final settle (inside endRun) may still have
    // credited the last segment's definite steps, so the cascade is brought
    // up to date only now -- and then the catastrophe lesson lands in the
    // (marking, T-suffix) pair store (doku section 16), flat at the arrival
    // pair the run record names: the last credited arrival, or the initial
    // state when nothing was credited. The restore's rebuild applies the
    // same rule to the same record, so the pair keys agree.
    // GLM-5.3-Flash: the cascade is brought up to date UNCONDITIONALLY: the
    // pair deposits below need it when the T-suffix layer is on, and the
    // command hooks fire for the credits the final settle adds even when the
    // layer is off (doku section 18).
    hookGuardBelief = finalBelief;
    advanceCascade();
    if (cfg_.ants.tsuffixLength > 0) {
        if (catastropheAmount > 0.0) {
            const StateId at = rec.steps.empty() ? rec.initial : rec.steps.back().target;
            ants_.depositPairRepel(at, run_, catastropheAmount);
        }
        // GLM-5.3-Flash: the seek lesson lands in the pair store the same way,
        // flat at the arrival pair -- the attract counterpart of the
        // catastrophe's pair deposit.
        if (crashAttractAmount > 0.0) {
            const StateId at = rec.steps.empty() ? rec.initial : rec.steps.back().target;
            ants_.depositPairAttract(at, run_, crashAttractAmount);
        }
    }

    // GLM-5.3-Flash: Egge: "suspect steps which do not execute need to be
    // reported." The tracker collects the command-carrying transitions a
    // resolved stretch ruled out; a hooked transition that never certainly
    // fired in this run gets its note here (one that DID fire elsewhere in
    // the run executed its command, so no note).
    for (const TransitionIndex t : tracker_.ruledOutCommands()) {
        if (t >= net_.transitions.size() || !net_.transitions[t].command)
            continue;
        if (std::find(rec.transitions.begin(), rec.transitions.end(), t) != rec.transitions.end())
            continue;  // its command did run somewhere in this run
        rep.notes.push_back("the command of '" + net_.transitions[t].id
                            + "' did not run: its suspect step was ruled out");
    }

    // DeepSeek V4.1 Flash: the run boundary (doku section 27). Every deposit
    // this run made -- milestones and catastrophes, the final settle included
    // -- has landed by now; aging once here instead of at every suggestion is
    // what lets the lesson survive a long run.
    if (cfg_.useAnts && cfg_.evaporateBetweenRuns)
        ants_.age();

    rep.outcome = outcome;
    rep.detail = detail;
    rep.events = rec.events.size();
    rep.dumps = rec.dumps.size();
    rep.newStates = rec.newStates;
    rep.newEdges = rec.newEdges;
    // GLM-5.3-Flash: how long the run took, in wall-clock milliseconds. This
    // is the second goodness criterion next to coverage: how much testing a
    // seed/config buys per second.
    rep.durationMs = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - startedAt).count();
    if (cfg_.runSink)
        cfg_.runSink(toPersisted(rg_, rec));
    if (cfg_.progress)
        cfg_.progress(rep);
    if (ownDeadline)
        deadline_.reset();
    return rep;
}

CampaignReport Session::runCampaign(ISutPlugin &plugin)
{
    using Clock = std::chrono::steady_clock;
    CampaignReport report;
    const Clock::time_point startedAt = Clock::now();  // GLM-5.3-Flash: campaign wall time
    if (cfg_.analyzeModel && !analyzed_)
        analyzeModel();
    report.warnings = warnings_;

    // DeepSeek V4.1 Flash: the wall-clock budget spans the whole campaign, not
    // each run (doku section 27).
    if (cfg_.timeBudget.count() > 0)
        deadline_ = Clock::now() + cfg_.timeBudget;

    std::size_t aborts = 0;
    for (std::size_t i = 0; i < cfg_.maxRuns && !stop_; ++i) {
        if (deadline_ && Clock::now() >= *deadline_) {
            report.stopReason = "time budget reached";
            break;
        }
        RunReport r = runOnce(plugin);
        report.runs.push_back(r);

        if (r.outcome == RunOutcome::Aborted) {
            if (++aborts >= cfg_.maxConsecutiveAborts) {
                report.stopReason = "too many aborted runs in a row";
                break;
            }
        } else {
            aborts = 0;
        }
        // DeepSeek V4.1 Flash: the run was cut short by the campaign's budget.
        if (r.budgetReached) {
            report.stopReason = "time budget reached";
            break;
        }
        if (r.coverageDone) {
            report.complete = true;
            report.stopReason = "coverage complete";
            break;
        }
        // GLM-5.3-Flash: the campaign's purpose is fulfilled -- a good target
        // marking was certainly reached. This is the normal way a campaign
        // ends when the model declares intentional target states.
        if (!r.goodTarget.empty()) {
            report.complete = true;
            report.stopReason = "the good target marking '" + r.goodTarget + "' was reached";
            break;
        }
    }
    deadline_.reset();  // DeepSeek V4.1 Flash: the budget belongs to this campaign
    if (report.stopReason.empty())
        report.stopReason = stop_ ? "stopped on request" : "run limit reached";
    report.summary = db_.summary();
    // GLM-5.3-Flash: wall-clock time of the whole campaign, so campaigns (e.g.
    // under different seeds) can be compared by how much testing a second buys.
    report.wallMs = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - startedAt).count();
    return report;
}

}  // namespace pn
