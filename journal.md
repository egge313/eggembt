# Petri-net model-based testing tool: design and status

Status as of 7 November 2026. All Qt-free code is implemented and tested (126 tests in nine programs, clean under AddressSanitizer, UBSan and, for the threaded ones, ThreadSanitizer). The Qt-dependent parts (JSON model loader, run-log file I/O, socket transport and their tests) are written but have not been compiled yet.

## 1. What the tool does

The specification of a system is a Petri net. The tool generates its reachability graph (RG) lazily and uses it as an oracle while a live system under test (SUT) runs. A plug-in next to the SUT reports what happens as events; the tool follows the SUT through the RG, flags deviations from the specification, deadlocks and livelocks, records which parts of the specification were covered (merged over any number of runs), and suggests the next input so that testing is steered towards uncovered parts.

```
model (JSON) -> loader -> Net -> lazy reachability graph
                                   |
SUT <-> plug-in/agent --events--> queue -> belief tracker -> coverage + run log
            ^                                   |
            +------ suggested input <- suggester <- ant colony, targets
```

## 2. The model

The model is a JSON document (a Petri-net standard such as PNML is not implemented yet).

- **Places** with an optional `bound` (maximum tokens).
- **Transitions**, each with a `kind`:
  - `controllable`: an input the tester sends; needs a `label`.
  - `observable`: an output of the SUT; needs a `label`.
  - `internal`: a step inside the SUT. With a `label` it is *traced* (the plug-in reports it); without one it is *silent* and never reported.
- **Arcs**: normal arcs (place to transition consumes tokens, transition to place produces them) and **inhibitor arcs** (place to transition, enabled only while the place holds fewer tokens than the weight; they consume nothing). Weights default to 1.
- **Markings**, as sparse maps of place to tokens:
  - `initial`: at least one, each with a name.
  - `target`: legitimate end states.
  - `error`: states that are targets of testing. Each can match `exact` (default) or `covers` (at least these tokens), and carries `policy` (`until_covered`, `once`, `avoid`), `target_hits`, `crash_dump` (`none`, `snapshot`, `crash`) and `max_dumps`.

Example fragment:

```json
"markings": {
  "initial": [{"name": "start", "tokens": {"closed": 1}}],
  "target":   [{"name": "idle_closed", "tokens": {"closed": 1}}],
  "error":   [{"name": "jammed", "match": "covers", "tokens": {"fault": 1},
               "policy": "until_covered", "target_hits": 3, "crash_dump": "snapshot"}]
}
```

The loader never stops at the first problem. It reports every error and warning with a path such as `$.arcs[3].to`, and produces a net only if there are no errors. Ids must be unique across places and transitions; inhibitor arcs must run from a place to a transition; an initial marking above a place bound is an error, a target or error marking above it only a warning. Warnings cover unknown keys (typos), transitions without arcs, and a model without target markings.

## 3. Semantics and the reachability graph

- **Enabling and firing.** A transition is enabled if every normal input place holds at least the arc weight and every inhibitor place holds fewer tokens than its weight. Firing consumes first, then produces.
- **Bounds.** A firing that would exceed a place bound (or overflow 32 bits) is not turned into an edge; it is recorded as a bound violation.
- **Lazy graph.** States are interned markings with dense ids; a state is expanded (its successors computed) only when asked. A state limit (default one million) stops the creation of new states; dropped edges mark the source state incomplete and the graph truncated, so a cut-off frontier is never mistaken for a deadlock. Edges have stable global ids.
- **Deadlock.** A state that was expanded, has nothing enabled, and is neither target nor an error marking.
- **Error versus target.** If a marking matches both, error wins.

### Livelocks: two kinds

1. **Static, in the specification's RG.** A *terminal cycle* is a strongly connected component that contains a cycle, cannot be left by any edge, and contains no target marking. The analysis works on whatever has been explored: edges into unexpanded states count as exits, and a finding is only *certain* if no state of the component is incomplete and none had a firing dropped for a bound. Optionally, an *internal cycle* (a cycle of internal transitions only) is reported as divergence; it is off by default because legitimate environment-like behaviour (an obstruction that appears and clears) forms such cycles.
2. **Run-time, with a settable timeout.** With `livelockTimeoutMs` greater than zero, the tracker declares a livelock when at least `livelockMinEvents` (default 3) observed events in a row bring no progress and the time since the last progress reaches the timeout. Progress is: a stimulus was sent, the belief contains a state not seen earlier in the run, the belief may be in a target marking, or the SUT fell quiet. This is a heuristic: choose the timeout longer than any legitimate autonomous activity of the SUT. Time comes from event timestamps, so replays are deterministic.

Note that a cyclic model without any target marking is, by this definition, one big terminal cycle. The session warns about this and tells you to declare the idle state as a target.

## 4. Following a live SUT

Not everything the SUT does is visible, so the tracker keeps a **belief set**: all RG states the SUT may be in, closed under silent internal steps.

| Event | Effect |
| --- | --- |
| Observed(label) | Keep the successors, via observable or traced internal transitions with that label. Nothing possible anywhere: **Deviation**. |
| Stimulated(label) | Same with controllable transitions. Not enabled in any possible state: **Preempted** (a silent step got there first; the model says nothing about the reaction; the belief is unchanged and the run goes on). |
| Quiescent | The SUT was silent for the quiescence timeout. Allowed in states with no enabled output or internal step; the belief narrows to those. None: Deviation (missing output). If all remaining states are non-target, non-error dead ends: **Deadlock**. |

The plug-in decides when silence has lasted long enough and sends the Quiescent event, because the tracker has no clock. A deviation names the labels that would have been allowed and says if the explored model may be incomplete there.

### Coverage under ambiguity

The tracker keeps the layers of belief sets. When the belief collapses to one state, and at the end of a run, it builds the graph of all paths consistent with the observations and computes dominators:

- elements on **every** consistent path are credited as definite visits;
- elements on **some** but not all are credited as ambiguous;
- elements on no complete consistent path (ruled out by later events) are not credited.

Definiteness is monotone, so crediting at a collapse point is definitive. A transition that certainly fired, but from an ambiguous source, is still credited at transition level. Backtraces skip ambiguous stretches; such a trace step has no edge although its depth is greater than one.

## 5. Coverage and run logs

The coverage database lives beside the graph and merges any number of runs. It keeps, per state and edge, definite and ambiguous counts and the first and last visit (run plus trace-tree node); per transition and place, hit counts; per error marking, hits and dumps taken; and a saturation threshold. All runs share one prefix tree of paths, so a backtrace costs one node per distinct prefix.

Runs are persisted as JSON lines by transition id (path runs) or as the live events with capture timestamps (event runs), never by internal state or edge ids, because those depend on expansion order. The log header holds a fingerprint of the graph (independent of declaration order, labels, kinds and target/error markings). Replaying a log into a fresh session rebuilds coverage, backtraces, target phases and dump budgets, and rejects, without recording anything, a run that no longer fits the model.

## 6. Guidance

### Targets and their phases

Error markings, deadlocks and terminal cycles are things to reach a few times and then avoid, because dealing with them is expensive (dumps, restarts). Each target is **Seeking** until it has been reached its required number of times (definite visits), then **Satisfied**: its states are forbidden. Error markings follow their policy from the model; deadlocks and livelocks follow session settings, `once` by default.

### Suggester

It ranks every input that is enabled in at least one possible state. The score is the number of steps to the nearest goal, plus penalties.

- **Goals**, nearest first: a target still being sought, a transition never covered, an edge not yet covered, an unexpanded state. Tier penalties default to 0, 0, 2 and 4 steps. Distances come from one backward Dijkstra over the explored graph; the suggester first expands up to 500 states ahead.
- **Robustness** is the fraction of possible states that enable the input, times how often the SUT's own moves leave it enabled (a silent step that takes the token or switches on an inhibitor lowers it).
- **Hazard** is the fraction of enabling states where the input leads straight into a forbidden state. If every state does, the input is vetoed. Plans never pass through forbidden states.
- **Answers:** Suggest, Wait (only the SUT can move on), Done (no goal left), Stuck (goals remain but none is reachable).

### Ant colony

Ants walk the model and leave two kinds of pheromone on edges: attractive, and repellent (which evaporates more slowly). An ant that reaches a forbidden state dies and marks the way in as repellent; one that reaches a target still being sought, or unexplored territory, marks the way in as attractive. Saturated edges lose their attraction. Deposits are back-propagated through the reverse edges of the graph to at least five previous nodes (enforced minimum), fading by a factor per step, so predecessors the ant never visited learn too. In a cycle the signal also reaches edges that lead back to the event, more weakly. Ants choose between the SUT's own moves and the tester's inputs in two steps; runs are deterministic for a given seed. The suggester uses the pheromone as a further ranking input. Pheromone is not persisted between sessions.

## 7. Running tests

A **Session** owns the graph, coverage, tracker, suggester, ants and event queue, and drives a plug-in through runs on one thread:

1. start the SUT in an initial marking;
2. wait briefly for events and feed all queued events to the tracker;
3. a deviation, deadlock or run-time livelock ends the run (optionally with a post-mortem); entering an error marking takes a post-mortem if its settings ask for one and its budget allows;
4. if something changed, let the ants walk, ask the suggester and send the input; while an input is in flight, no second one is sent;
5. end on Done or Stuck once the SUT has settled, on the run-length limit, or on a misbehaving SUT or plug-in (Aborted).

Done and Stuck end a run only if the model expects no further output. If outputs are still possible the session keeps listening, because the SUT may be busy: that is how a livelock, or a SUT that fell silent when it owed an output, gets noticed.

**Asynchronous events.** The plug-in never waits for the tracker. It pushes events, stamped at capture, into a bounded queue; the queue's sequence number defines the one order the tracker works with. A race between a suggested input and a fast SUT is therefore preserved and shows up as a Preempted stimulus, not as a false deviation. The queue never drops an event silently: when it is full it refuses, and the run is aborted.

**Post-mortems.** A *snapshot* dumps the SUT without stopping it; a *crash* ends it in a controlled way for a genuine crash dump and the next run restarts it. The number of dumps is limited per error marking over all runs (`max_dumps`); if the plug-in cannot do what was asked it falls back to a snapshot, or reports that no dump was taken. A crash-capable agent must run in the SUT's process or next to it, never inside the tracker.

**Run outcomes:** Completed, ErrorReached, Deviation, Deadlock, Livelock, SutCrashed, Aborted. Defaults: poll 20 ms, idle timeout 10 s, acknowledgement timeout 5 s, 100 runs, 10,000 events per run, three consecutive aborts stop the campaign.

## 8. The plug-in protocol

The plug-in interface is abstract (start a run, stimulate, post-mortem, stop, fault polling). For agents in another process or language there is a line protocol: one JSON object per line over any byte transport. Agent to tracker: `hello`, `run_started`, `event`, `post_mortem_result`, `fault`. Tracker to agent: `start_run`, `stimulate`, `post_mortem`, `stop_run`, `shutdown`. Events carry a kind (observed, stimulated, quiescent), a label, a capture timestamp and the run number, so late events of an earlier run are dropped. Unknown fields are ignored. The full specification, with an example run and a checklist for agent authors, is in `docs/PLUGIN_PROTOCOL.md`.

## 9. Code layout and build

- `src/model`: `Net`, `NetBuilder`, graph fingerprint.
- `src/engine` (plain C++17, no Qt): semantics, reachability graph, reverse index, trace tree, coverage database, belief tracker, livelock analysis, targets, suggester, ant colony, event queue, session, JSON codec, protocol, remote plug-in, run-log replay.
- `src/io` (Qt): JSON model loader, run-log reader and writer, line transport over any `QIODevice`.
- `tests`, `examples/elevator-door.json`, `docs`.

CMake builds a static engine library and a Qt library, and registers all test programs. About 7,300 lines of source and 5,800 lines of tests.

## 10. Verification

- Nine Qt-free test programs, 126 tests: engine and graph, coverage and replay, belief tracking, livelocks, suggestions and targets, ants, asynchronous queue, end-to-end sessions against a simulated SUT, and the JSON codec, protocol and remote plug-in over a loopback agent.
- All built with warnings enabled and run under AddressSanitizer and UBSan; the threaded ones also under ThreadSanitizer, and the session tests repeated and run under CPU load.
- Mutation checks: key behaviours were deliberately broken (dominator crediting, veto, back-propagation depth, in-flight blocking, overflow detection, dump budget and others) to confirm the tests notice. Where a mutation survived, the tests were strengthened.
- Real defects found this way include a queue-lifetime race in the remote plug-in, an off-by-one in the JSON nesting limit, and wrong test expectations about cyclic pheromone spill.
- **Not yet verified:** everything that needs Qt (no Qt in the build environment). Please run `ctest` first.

## 11. Assumptions worth revisiting

- A livelock is a terminal cycle without a target marking; a cyclic protocol must declare its idle state as a target.
- The run-time progress heuristic (section 3) and its default of three events.
- The SUT's own moves are weighted equally in the ants and in the robustness estimate.
- Hits on a livelock target count definite visits to its states, so an initial marking inside a terminal cycle satisfies it immediately.
- Error markings are sought until satisfied and then avoided; deadlocks and livelocks default to `once`.
- Ambiguous hits (error markings whose belief spans several states) are credited when the run ends.

## 12. Open items

1. Compile and run the Qt parts.
2. A command-line runner (model, run log, agent address) and a GUI for coverage and backtraces.
3. Record how often each SUT move is offered versus taken, and feed it to the ants and the robustness estimate.
4. Persist pheromone, or rebuild it from the run log.
5. Optionally, states from which no target marking is reachable as a further repellent region.
6. PNML import.
7. A reference agent shim for C++ SUTs.

---

## 13. Update: 14 November 2026 (GLM-5.3-Flash)

Everything above is the original document of 7 November 2026, left unchanged. This section records what has happened since; all changes are marked `GLM-5.3-Flash` in the source.

### Corrections to the status above

- There are no Qt-dependent parts in this tree. The "written but not compiled" Qt loader, run-log I/O and line transport of §9 do not exist here: the project is Qt-free end to end. `src/io` holds the plain C++17 JSON model loader (`JsonNetLoader`), the run-log reader and writer (`RunLogIO`) and the transports (`StreamTransport`, `FdTransport`, `ProcessTransport`, `TcpListener` / `tcpConnect`).
- The command-line runner (first half of open item 2) is implemented and tested: `eggembt run`, `check`, `explore`, `report` and `agent-sim`, with `--json` reports, run-log continuation and agents over standard streams, child processes or TCP.
- Everything compiles and passes: 12 unit test suites plus `cli_test.sh` (22 scenarios) — 13 CTest entries, all green, now built with `-Wall -Wextra` as §10 promises. §10's "not yet verified" no longer applies.

### Changes made this session

- **Stdio exit deadlock fixed.** `StreamTransport`'s reader blocks in `std::getline` over `std::cin`, holding stdin's C stdio lock; `exit()` then waits for that lock forever while flushing the streams, so `eggembt run --stdio` and `eggembt agent-sim` hung instead of exiting. On POSIX both now use the poll-based, stoppable `FdTransport(0, 1, false)`; `StreamTransport` remains for non-POSIX builds (two sites in `src/cli/Commands.cpp`, marked).
- **`--no-ants`.** The ant colony can be switched off (`SessionConfig::useAnts`, surfaced as `Session::antsEnabled()`). The report says which guidance ran: a `Guidance` block on the console and `ants` / `ant_iterations` / `ant_walks` in the `--json` output, always emitted so the schema does not depend on the switch. Tested in `tests/tst_session.cpp` (`theAntColonyCanBeSwitchedOff`) and `cli_test.sh` (`no-ants`).
- **The remaining guidance switches are surfaced in the report.** `--deadlock-policy`, `--livelock-policy`, `--dump-on-deviation`, `--dump-on-deadlock`, `--dump-on-livelock` and `--threshold` were accepted but invisible afterwards. The `--json` report now always carries `deadlock_policy`, `livelock_policy`, `dump_on_deviation`, `dump_on_deadlock`, `dump_on_livelock` and `coverage_threshold`; the console `Guidance` block lists the same values (`targets: deadlock …, livelock …; coverage threshold N`, `post-mortems: on deviation …, on deadlock …, on livelock …`); `Session::config()` exposes the effective configuration; `toString(TargetPolicy)` in `src/model/Net.h` fixes the one spelling (`until_covered` / `once` / `avoid`). Tested in `cli_test.sh` (`guidance-in-report`).
- **`docs/PLUGIN_PROTOCOL.md` written.** The dangling reference in §8 (and in `src/engine/Protocol.h` / `AgentServer.h`) now resolves: the full wire specification with field tables, an example run, the checklist for agent authors, and a map of the code that implements it.
- **`--seed N`: reproducible campaigns with steered tie-breaks.** Until now the input choice was fully deterministic — among equally scored inputs the more robust one won, then the lower transition index — so every campaign over the same model and settings picked exactly the same inputs; the only seeds were the ant colony (`--ant-seed`) and the simulated SUT (`--sim-seed`). A non-zero `--seed` now randomises the *last resort only*: when several inputs are exactly tied (same effective score and same robustness), the pick is drawn from a seeded `std::mt19937_64` stream (`SessionConfig::selectionSeed` → `SuggesterConfig::tieBreakSeed`; seed 0 keeps the old behaviour byte for byte). One campaign stays reproducible from its seed, different seeds steer different campaigns, and the quality ranking is untouched. The `--json` report names both seeds (`seed`, `ant_seed`) so a campaign can be reproduced from the report alone. The SUT's own indeterminacy is unaffected — a real SUT may still answer differently, which is fine and interesting. Tested in `tests/tst_suggest.cpp` (`seededTieBreaksVaryAndReproduce`), `tests/tst_session.cpp` (`theSeedSteersAndReproducesCampaigns`) and `cli_test.sh` (`seed`, byte-compare on the deterministic counter model).
- **Testing time as a second criterion next to coverage.** Coverage alone does not say what a campaign cost. Every run now measures its wall-clock duration (`RunReport::durationMs`), the campaign its total (`CampaignReport::wallMs`); the `--json` report gains per-run `duration_ms` and a `time` section (`wall_ms`, `mean_run_ms`, `events_per_s` — how many SUT events a second of testing bought), and the console output a `Time` block. This is the number that tells seeds and configurations apart when their coverage is equal: the same coverage in half the time is the better campaign. Wall-clock numbers are, by nature, not reproducible — the `seed` scenario's byte-compare strips the time fields before comparing, because the reproducibility claim is about the *selection*, not about timing. Tested in `tests/tst_session.cpp` (`theCampaignMeasuresItsTime`) and `cli_test.sh` (`time-and-overlap`).
- **Coverage overlap: how much do two test runs have in common?** New `src/engine/OverlapAnalysis.{h,cpp}`: the Jaccard index |A∩B|/|A∪B| of the covered element sets, computed per run, per campaign and pairwise, separately for transitions, states and edges. For exact per-run attribution `CoverageDb` now records, alongside `steps` and `events`, every state a run definitely visited (including those credited without a known edge in ambiguous stretches) and every transition that certainly fired (including the ambiguous-layer credits); a replayed run reconstructs the same sets, so campaigns can be compared after the fact from their run logs. The campaign `--json` report gained an `overlap` section (full pairwise matrices and `mean_pairwise` means; low means mean the steering spread the testing out, 100% would mean every run repeated the same coverage), the console a one-line `Overlap` summary. New command **`eggembt compare MODEL LOG_A LOG_B [--csv FILE] [--json FILE]`**: replays both run logs and reports the campaigns' mutual overlap, the diversity within each campaign, and the run-against-run matrix (printed as a percent grid while it fits the terminal; `--csv` writes every pair as `group,i,j,transitions,states,edges` rows for plots). Tested in `tests/tst_overlap.cpp` (new suite: set attribution, the index, matrix symmetry, empty runs count as identical) and `cli_test.sh` (`compare`: a log against itself is 100%, CSV/JSON output).
- **The `"traced"` key for internal transitions.** Internal steps were tagged only implicitly: an internal transition *with* a label was reported like an output ("traced"), one *without* was silent. That had a footgun — adding a label purely for display or the run log silently flipped the step from silent to reported. The loader now accepts an explicit `"traced": true|false` on internal transitions (default unchanged: labeled ⇒ traced, unlabeled ⇒ silent). It is an error elsewhere: `traced: true` without a label has nothing to report, and on controllable/observable transitions the visibility is part of the testing protocol, not a per-model choice. Engine-wide the decision now lives in `Transition::traced` (derived in `NetBuilder::build()`, settable via `NetBuilder::traced()` for hand-built models), used by the belief tracker (label maps, `expectedLabels`, the silent-step crediting), the simulated SUT and the `check` kind counts — so a *named but silent* internal step works everywhere. Tested in `tests/tst_jsonnetloader.cpp` (`tracedKeyDefaultsAndOverrides`, `tracedKeyIsRejectedWhereItDoesNotApply`), `tests/tst_session.cpp` (`aNamedInternalStepIsReportedOnlyWhenTraced`) and `cli_test.sh` (`check-reports-every-model-error`).
- **Truncated reachability graphs tested (state limit).** The question "how does the tester react when `--max-states` cuts the graph off, and does it expand unresolved nodes?" is now pinned by tests (`tests/tst_session.cpp`, marked): the graph is lazy and the limit stops the *creation* of states, not their expansion — during runs the tracker and the suggester expand every state they stand on (plus up to `expansionBudget`, 500, states ahead), so the graph grows on demand until the cap; at the cap, edges into new markings are dropped and the source is marked incomplete. A cut-off frontier is never mistaken for a deadlock or a livelock (deadlock is judged on the marking, not on edges; the static analysis only reports *certain* findings when no involved state is incomplete), the session warns "the model was not explored completely", and when the SUT walks past the region the graph can represent, the run ends in a deviation whose message says "[the explored model may be incomplete here]". Tests: `aStateLimitTruncatesTheGraphWithoutFalseFindings`, `theGraphGrowsLazilyDuringTheCampaign`, `anSutBeyondTheTruncatedModelIsFlaggedAsPossiblyIncomplete`; end to end, `cli_test.sh` scenario `state-limit` (`eggembt explore --max-states` and `eggembt run --max-states` over a counting model).
- **Missing pieces reconstructed** during assembly: `tests/ElevatorModel.h` (the shared elevator model), `README.md`, `.gitignore`, and the Qt-free `CMakeLists.txt` (`mbt_engine`, `mbt_core`, `mbt_agent`, the `eggembt` application, the tests and the `cli_test.sh` wiring).
- **The case for having ants, pinned by tests.** When the SUT crashes, finding the root cause takes wall-clock time; the campaign should spend that time testing somewhere else instead of re-finding the crash — and the same holds for a deadlock. The Session already tells the colony at the end of a run that really suffered a catastrophe (a crash post-mortem taken or attempted, or a run stranded in a deadlock), via `AntColony::depositRepel`, the back-propagating hook for "callers with their own knowledge". Two scenario campaigns in `tests/tst_session.cpp` now pin what that buys (`afterACrashTheAntsKeepTheCampaignProductive`, `afterADeadlockCrashTheAntsDoTheSame`), on a region model: a one-input way into a `boom` error marking that demands two visits and may crash the SUT for a post-mortem each time (`targetHits = 2, crashDump = Crash, maxDumps = 2`), next to two safe two-input corridors; the deadlock twin uses a true sink deadlock. With the colony, run 1 finds the catastrophe — one input, the way in is marked repellent — and run 2, the research window while dump #1 is being analysed, covers the six corridor edges and only then delivers the second, policy-mandated hit. Without the colony, run 2 walks straight back in (the tie between the covered, still-sought catastrophe path and the fresh corridor edges falls to the lower transition index): no new coverage, and dump #2 — or a second hang — of the same fault before anything else was tested. A true sink deadlock never produces the run-time Deadlock outcome (the suggester runs out of work while the SUT hangs, so the run ends *stranded*, `rep.stuck`, before any quiescence report could name a deadlock; no post-mortem is taken and the visit itself credits the deadlock target) — which is exactly why the stuck-in-deadlock deposit exists. The scenario is tuned so the catastrophe sits exactly one input deep: covered-and-still-sought it ties with the fresh corridor edges, and only the pheromone decides — `pheromoneRepelWeight = 12` sits in the window where fresh corridors beat the marked way in, yet the catastrophe wins again as soon as the corridors are covered (its best falls back to `1 + cost = 1` while a re-walked corridor costs `1 + 3` through covered edges). The colony's deadlock campaign finishes in 2 runs, the colony-less one in 3.
- **Bad pheromones priced by the way in.** An error state — deadlock, livelock, SUT crash — deserves worse pheromones the more time it takes to reach it: a repeat visit pays the way in again, and for a given SUT the wall-clock time of that way grows with the number of inputs on it, the quantity a run can measure reproducibly (the run's own wall-clock duration is not reproducible, so it must not steer). The Session's real-run catastrophe deposits (a crashed SUT, or a run stranded in a deadlock) are now priced by `AntConfig::catastropheDeposit(stimuli)`:

      depositCatastrophe * (1 + catastropheTimeWeight * min(stimuli, catastropheTimeCap))

  with `catastropheTimeWeight = 0.25` (a quarter of the base deposit per input the run spent getting there: a crash one input in deposits 3.75, four inputs in 6.0, from eight inputs on 9.0) and `catastropheTimeCap = 8` (inputs counted at most — however long the run was, one catastrophe cannot out-shout everything else; `tauMax` remains the absolute ceiling). `catastropheTimeWeight = 0` restores the old flat deposit. The colony's own deposits — ants that die in a Forbidden state — stay flat: an ant's walk through the model costs the campaign nothing. Tested in `tests/tst_ants.cpp` (`catastropheDepositGrowsWithTheWayIn`) and `tests/tst_session.cpp` (`theLongerTheWayInTheWorseTheCatastrophePheromone`: one crashing run each on chains of 1, 4, 10 and 12 inputs — the strongest repel left behind grows 3.75 → 6.0 → 9.0 and then stays).
- **Good target states: `"intentional": true` on a target marking.** Target markings existed from the start (§2) but were only idle states for the livelock analysis — reaching one never ended a run. A target marking can now be tagged `"intentional"` (`NamedMarking::intentional`, `NetBuilder::targetMarking(..., bool intentional)`; the loader warns when the key appears on initial or error markings, where it means nothing). Such markings are the campaign's normal targets: the target board steers into them like into any other sought goal, and the moment the belief tracker knows the SUT is certainly in one (`BeliefTracker::definiteTargetMarking()`, the mirror of `definiteErrorMarking()`) the run ends `Completed` with detail "good target marking 'X': the campaign ends here" and the campaign stops with stop reason "the good target marking 'X' was reached". The per-run report names it (`RunReport::goodTarget`, JSON `good_target`, always emitted so the schema does not depend on the model); no new run outcome was needed. Non-intentional target markings behave exactly as before. Tested in `tests/tst_session.cpp` (`aGoodTargetMarkingEndsTheCampaign`: the intentional twin ends the campaign after the single stimulus that reaches the marking, the plain twin tests on and finishes by coverage) and `cli_test.sh` (`good-target`).
- **Error transitions.** Places could be tagged as errors from the start (§2); transitions could not. A transition can now carry `"error": true` plus its own post-mortem settings `"crash_dump"` / `"max_dumps"` (`Transition::error/crashDump/maxDumps`, `NetBuilder::error()`), and the session reacts like it does for an error marking — but only when the step is *certain*: every possible belief state must have advanced by the very same transition, with nothing lost to the exploration limits (`BeliefTracker::certainStep_`, set per label event and reset by quiescence/`beginRun`; `definiteErrorTransition()`). Ambiguous steps stay ordinary coverage. The certain step is recorded in `errors_reached` (its label, or its id when unlabeled), the post-mortem is taken when the transition's own budget allows (`CoverageDb::errorTransitionDumpAllowed`, dumps counted per transition in `errorTransDumps_`, attached to the run's `DumpRecord::transition`), a Crash ends the run `SutCrashed` with "error transition 'X': the SUT was crashed for a post-mortem" and marks the crash site for the ants as before, and a `none` request lets the run continue past it. A certain error *marking* wins when both would fire on the same step. Steering stays ordinary transition coverage for now: the loader warns that `"policy"`/`"target_hits"` have no effect on transitions yet, and it *rejects* an error tag on an untraced internal transition, whose steps the tester can never see. Tested in `tests/tst_jsonnetloader.cpp` (`transitionTagsAreParsed`, `ineffectiveTransitionSettingsWarn`, `errorTransitionMustBeVisible`) and `tests/tst_session.cpp` (`anErrorTransitionIsCaughtLikeAnErrorMarking` with crash + budget-exhausted second run, `anErrorTransitionWithoutADumpLetsTheRunContinue`).
- **`--flat-catastrophe`: the wall-clock pricing can be switched off.** The time-priced catastrophe pheromones above reason about the wall clock; when that reasoning does not apply to a SUT, the feature can be turned off as a whole: `--flat-catastrophe` sets `SessionConfig::catastropheTimePricing = false` and every real-run deposit is then the plain base level (`catastropheDeposit(0)` = `depositCatastrophe`), however deep the crash was. The report names the switch in the `--json` file (`catastrophe_time_pricing`) and on the console (`catastrophes: priced by the way in | flat (--flat-catastrophe)`). Tested in `tests/tst_session.cpp` (`theCatastrophePricingCanBeSwitchedOff`) and `cli_test.sh` (`guidance-in-report`).
- **`"unpredictable": true` on a transition: a human operator drives it.** The wall clock of a run that steps such a transition measured a human's reaction time, not the model's depth, so it must not price the catastrophe pheromones: a run whose way in (`RunRecord::transitions`) crossed an unpredictable transition deposits flat even with the pricing feature on. Nothing else changes — the step is still coverage, the transition can still be an error, the tag is allowed on any kind of transition. The console Guidance line counts them ("unpredictable transitions N"). Tested in `tests/tst_jsonnetloader.cpp` (`transitionTagsAreParsed`) and `tests/tst_session.cpp` (`anUnpredictableStepKeepsTheDepositFlat`, on the way-in chains of `theLongerTheWayInTheWorseTheCatastrophePheromone` — first step, last step, and the untagged control).
- **Internal transitions without a counterpart in the SUT, pinned end to end.** This aspect was designed in from the start but had no dedicated session-level test. An *unlabeled* internal transition is exactly the "no counterpart" case: it can never be stimulated (only controllable transitions can) and is never reported (only observable and traced-internal steps are), so the tester neither sends nor waits for it — the simulated SUT simply takes such moves by itself between external ones, without an event. What the belief tracker does with it was already pinned at unit level in `tests/tst_belief.cpp` (silent closure, ambiguous silent stretches, ruled-out silent paths not credited). New in `tests/tst_session.cpp`, the end-to-end pin for the bridge case — an internal step as the *only* connection between an input and the output that follows it (`makeInternalBridgeNet`: `s0 -a-> s1 -advance-> s2 -oa-> s0`): the run records exactly two events (`Stimulated a`, `Observed oa`) and never a third one for the internal step, the SUT was never asked for it (`stimuliReceived()`), and yet the silent edge is credited *definitely* (`transitionHits == 1`), because the output can only come from the state the internal step leads to — every consistent path crosses it (`anInternalStepWithoutACounterpartIsFollowedAndCovered`; the definite silent edge is credited with its transition by `creditSegment`'s dominator walk). The flip side is pinned too: while such a step is enabled the model says autonomous progress is due, so a SUT that never takes it and goes silent is a *missing output* — the run ends `Deviation` ("no output arrived … (expected: oa)"), and the silent stretch stays ambiguous and uncredited, because the tester could not know whether it fired (`anInternalOnlyStallIsAMissingOutput`). This complements `aNamedInternalStepIsReportedOnlyWhenTraced`, which covers the other internal case: a *named* internal step whose visibility the model chooses with `"traced"`.

The suite stands at 14 CTest entries — 13 unit suites plus `cli_test.sh` (26 scenarios) — all green.

### The ATM example: one model that exercises the whole toolbox

`examples/atm.json` is a worked example of the classic ATM case, built to show every recent feature on one coherent model: *a person operates the ATM, inserts a card, enters a PIN — three invalid PINs and the machine swallows the card; a card reported stolen is confiscated; with card and PIN accepted the person may withdraw cash, subject to a funds/credit check; there are several ATMs; every ATM visit opens a session in the bank computer, every session connects to the database; every operation is acknowledged; and each of the three components — ATM, session, database — can fail.*

**The model** (22 places, 22 transitions, 39 RG states): `card.insert` opens the serialized bank chain `open_session → connect_db` (silent internals: the per-visit bank session and its database connection), then a three-way silent fan-out at `db_up` — `reader_check` / `pad_check` (both silent, into `reading`) or `stolen_flag` (a *traced* internal, `bank.stolen_report`, into `confiscating`). The traced announcement is what makes the stolen branch testable: after `bank.stolen_report` the belief narrows to `{confiscating}`, so the observable error transition `confiscate` (`card.confiscated`, crash dump) is caught *certainly* and crashes the SUT for a post-mortem. The fork softens the branch to one visit in three — the price is that the two silent routes into `reading` are indistinguishable from outside, so `reader_check` and `pad_check` are the only two transitions a campaign can never cover *definitely* (the ambiguous silent stretch is credited only when every consistent path crosses it).

The PIN phase is a direct stimulus→answer window: `pin.enter` (`await_pin → pin_answering`, tagged `"unpredictable": true` — a human types it, so a run's way through it must keep the catastrophe deposit flat) with only visible continuations: `pin_ok`, `reject_more` (consumes one of the two `tries` tokens) and `reject_last` (the same label, enabled only while an inhibitor arc holds `tries` empty). The third invalid PIN therefore takes `reject_last → swallow_card` (`card.kept`), and `swallowed` is the model's error **marking** target `card-swallowed` (`match: covers`, `until_covered`, one hit, one crash dump): the campaign hunts it, dumps it, and moves on. There are no silent steps anywhere in the windows where the tester can act — that is what keeps `certainStep_` meaningful (an error transition is only detectable when the belief is a single state at event time, so every failure transition must compete only with *visible* continuations reached directly from a stimulus or an ack).

The three failure transitions sit where their belief is single and the competitors visible: `session_fail` (`session.down`) and `db_fail` (`db.down`) compete with `cash_out` / `funds_reject` at `funds_answering` (4-way), `atm_fail` (`atm.down`) competes with `eject_after_cash` at `dispensed`. Each is deterministic-only enabled (a failure with no visible competitor fires every visit — this simulation cannot "sometimes not fail"). Broken states (`session_broken`, `db_broken`, `atm_broken`, `confiscated`) are plain dead-ends: once a transition's dump budget is spent, later runs through it end Completed/"stuck" and the ant colony keeps the campaign away from them.

**Several ATMs, visibly.** The reachability graph interns identical markings, so three initial markings with the same tokens would rotate the runs (round-robin over the three ATMs) *invisibly* — every run would log `atm-lobby`. The ATMs therefore differ in their cash cassette: `atm-lobby` (2), `atm-drivein` (1), `atm-branch` (0 — out of cash, its withdrawal window is 3-way and can never complete the good target). `cash_out` takes a `cassette` token, so cassette levels propagate into the story and the rotation is observable in the run log.

**The good target requires a real withdrawal.** `visit-done` (`match: covers`) needs `visit_done ≥ 1` *and* `cash_taken ≥ 1` (`cash_out` also produces `cash_taken`); the plain card-return exit reaches `visit-aborted` (`visit_done ≥ 1`, not intentional), which the target board ignores — so the campaign keeps working until an actual withdrawal happened, then stops with "the good target marking 'visit-done' was reached". The `covers` match (instead of `exact`) is necessary because leftover `tries` tokens persist at `visit_done`; draining them silently would pollute the belief at `dispensed` and break the `atm_fail` window.

**The deterministic story.** Campaigns are deterministic per `--sim-seed` (the suggester's `--seed` only steers tie-breaks). With `--sim-seed 5 --max-runs 45`: 37 runs, 5 crash post-mortems (`confiscate`, `session_fail`, `db_fail`, `atm_fail` — each dumped exactly once), the `card-swallowed` marking hit twice and satisfied, the good target at run 37, and the initial markings rotate 13/12/12 across the three ATMs. Other sim-seeds tell the same story in 37–58 runs.

**A characteristic of the simulated SUT worth knowing.** `SimulatedSut::startRun` reseeds its RNG per run (`opt_.seed + generation_`), so a campaign's runs draw from *consecutive* seeds. Probes against the engine library showed the pick itself is fair (a plain 4-way state over 400 spread-seed trials: 90/99/101/110), but consecutive small seeds measurably skew the early picks of a run (funds-window answers over 300 consecutive-seed trials: down 61%, cash 19%; over spread seeds: down 51%, cash 21%) — an early-output seeding artifact of MT19937-64, most visible in the first handful of draws. Nothing breaks: every element is still found (all four error transitions, the swallowed card, the good target), it only takes the ~37–58 runs. Documented rather than fixed this batch — changing the seeding would change every existing deterministic story; scenarios pin a sim-seed.

Tested in `cli_test.sh` (`atm-visit`: clean `check`, the run with `--sim-seed 5` stopping at the good target with all four error transitions dumped, the satisfied `card-swallowed` marking, catastrophe pricing reported, the rotation visible in the run log, and `Post-mortems`/`unpredictable transitions 1` on the console). The suite stands at 14 CTest entries — 13 unit suites plus `cli_test.sh` (27 scenarios) — all green.

### Still open

Items 3–7 of §12 stand; item 2 remains open in its GUI half, item 1 is void as described above. Build and check with:

    cmake -S . -B build -G Ninja && cmake --build build && ctest --test-dir build

The 20 `files*.zip` archives that held the original material this tree was assembled from have been moved out of the project root to `../originals/` (a sibling of this directory), so the tree now contains only the assembled project.

---

## 14. Update: 16 November 2026 (GLM-5.3-Flash)

Everything above stays untouched. This batch adds the **milestone label** to the Petri net and implements §12 items **3, 4, 6 and 7**, quoted verbatim from §12:

> 3. Record how often each SUT move is offered versus taken, and feed it to the ants and the robustness estimate.
> 4. Persist pheromone, or rebuild it from the run log.
> 6. PNML import.
> 7. A reference agent shim for C++ SUTs.

After this batch, of §12 only item 5 (states from which no target marking is reachable as a repellent region) and the GUI half of item 2 stand. All changes are marked `GLM-5.3-Flash` in the source; the help text and runtime output stay signature-free.

### The milestone label (Egge's assignment)

A transition can now be tagged as a **milestone** — a point on the way whose certain reach is a success worth advertising to the colony, "almost as much as a target marking gets":

    {"id":"serve","kind":"observable","label":"drink.served","milestone":true}   // rank 1
    {"id":"serve","kind":"observable","label":"drink.served","milestone":2}      // rank 2

`Transition::milestone` plus `milestoneRank` (≥ 1, default 1), `NetBuilder::milestone(t, rank = 1)`. The same visibility rule as `"error"` applies and the loader rejects the tag on an untraced internal transition, whose steps the tester can never see.

**The reaction.** The session reacts only to a *certain* step — every possible state of the belief advanced by the very same transition, nothing lost to the exploration limits (`BeliefTracker::definiteMilestoneTransition()`, the mirror of `definiteErrorTransition()`). On one, it deposits positive pheromones at **every state the belief spans** (`AntConfig::depositMilestone = 1.5` times the rank, so a rank-1 milestone sits just under the colony's own target deposit `depositTarget = 2.0`: reaching a milestone is nearly as good as the target marking, but the target marking is still what the campaign came for). The deposit fires once per step (guarded by the tracker's event count, so a loop through the milestone deposits again), *before* the good-target check, and is skipped when the same event certainly stepped an error transition — that step is a failure, not a success. The way in is deliberately **not** priced (unlike catastrophes): a success is a success however deep it lay, and pricing it would make deep milestones paradoxically more attractive than shallow ones of the same rank. `tauMax` remains the ceiling. Campaigns with the ants switched off skip the deposit.

A certain milestone step does **not** end the run — that is the whole point; the run goes on, the record notes it (`RunReport::milestonesReached`, JSON `milestones_reached`, the label or the id), the coverage section lists every milestone (`milestones` with `id`, `label`, `rank`, `hits`), and the console Guidance names the deposit ("milestones: deposit 1.5 per rank; milestone transitions N"). One interaction to know: the colony wipes attract pheromone from saturated edges, so once the way to a milestone is covered to the threshold, its live deposit is wiped again — the durable teaching lives in the rebuild (item 4 below) and in the exact-amount tests.

The rank multiplies, it does not add: a rank-3 milestone counts three times as much as a rank-1 one.

Tested in `tests/tst_session.cpp` (`aMilestoneStepIsRewarded`: the milestone is certainly stepped and reported, the run demonstrably continues past it — the next input is sent afterwards — and the edge into the milestone's target carries attract pheromone).

### Item 3: offered versus taken

> 3. Record how often each SUT move is offered versus taken, and feed it to the ants and the robustness estimate.

Per SUT-move **label** (not per transition: a label may map to several transitions), `CoverageDb` now counts how often the move was **offered** to the SUT and how often the SUT actually **took** it (`recordOffered` / `recordTaken` / `takeRate`; the rate is `taken/offered` clamped to 1.0, and a move that was never offered rates 1.0 — no evidence either way, which in particular keeps silent internal steps, never offered because never visible, neutral).

- **Offers**: whenever the session deliberately waits for the SUT (`SuggestionStatus::Wait`), it offers every move the model expects now — once per wait episode (the guard resets on the next processed event), because the labels were named to the SUT, in effect, by waiting for them.
- **Takes**: every model-known observed step (`EventKind::Observed` with `StepStatus::Ok`). A deviation is not a take. Events are now pumped one at a time so every event can be credited (`PumpResult::lastKind` / `lastLabel`; the aggregate is what a single `pumpEvents` call returned before).
- **Feed 1, the ants**: the colony's environment moves are no longer weighted uniformly — `SessionConfig` wires `AntConfig::environmentWeight` to the take rate of the move's label (set up in the `Session` constructor, over the *label* of each edge's transition).
- **Feed 2, the suggester**: the robustness estimate's environment survival (`envTotal`/`envKeep`) now weights each environment move by its take rate, so moves the SUT keeps refusing stop dominating the survival figure of the inputs around them. Never-offered moves weigh as before.

Tested in `tests/tst_session.cpp` (`theEnvironmentFeedCountsOffersAndTakes`: the rate arithmetic on a bare `CoverageDb`, a co-operative campaign in which every answer was offered and taken, the wiring assertion via the new read-only `AntColony::config()`, and a SUT that refuses a move — offered, never taken, rate 0. The session scenarios run a one-corridor model with one-sided timing, ~150 ms between the SUT's own move and the session's ~2 ms suggestion cycle, so the wait episodes are deterministic rather than dependent on thread scheduling), and in `tests/tst_suggest.cpp` (`robustnessWeightsTheEnvironmentByTakeRate`: in a corridor whose SUT moves are `keep` — the input stays enabled — and `kill` — it is disabled — the robustness of the input is 0.5 with no statistics and when both moves were taken, 1.0 when only `keep` is taken or both are refused (nothing between the input and its goal), and 0.0 when only `kill` is taken).

### Item 4: rebuild the pheromones from the run log

> 4. Persist pheromone, or rebuild it from the run log.

Decided for **rebuild**, not raw persistence: a run log already travels with a campaign (`--log`), it is human-inspectable, and the pheromones are derivable from what was recorded — persisting a parallel binary state would freeze the deposit formulae into old files. `Session::restore` now calls `rebuildPheromones(run, rec)` after a successful replay.

- **Milestones**: every definitely recorded step of a milestone transition lays `milestoneDeposit(rank)` on its recorded target state — with no evaporation, so a restored campaign resumes with the full teaching (a rank-2 milestone rebuilds at exactly 3.0). Certain-but-ambiguous milestone steps have no definite edge in the recording and are not rebuilt.
- **Catastrophes**: a run that ended `SutCrashed`, or a run stranded in a deadlock (`Completed` with a detail starting `stuck:` whose last recorded state is a deadlock — expanded first, the graph is lazy), marks the way into its last recorded state repellent, priced by the way in exactly like the live deposits: `catastropheDeposit(way)` with `way` = the acknowledged stimuli of an event run / the controllable transitions of a path run, flat when `--flat-catastrophe` is on or the recorded way crossed an `"unpredictable"` transition.
- **Not rebuilt**: the colony's own walk deposits (unexplored territory, sought targets) — they are transient state of a walk, not knowledge about the SUT.

This is a documented **approximation** of the live deposits: live, the session deposits at every state the belief spanned and prices by the inputs it actually sent; a recording carries the definite steps and the acknowledged events, so the rebuild places the milestone attract at the recorded targets and prices catastrophes by the recorded way in.

Tested in `tests/tst_session.cpp` (`theRebuildReplaysWhatTheRunTaught`: the rank-2 milestone rebuilds at exactly 3.0 with nothing marked repellent; a recorded one-input crash at 3.75, flat at 3.0 under `--flat-catastrophe` and for an unpredictable way in; a run stranded in a recorded deadlock at 3.75; a *completed* run through the same deadlock teaches nothing; an event-run crash priced by its acknowledged stimulus).

### Item 6: PNML import

> 6. PNML import.

`src/io/PnmlLoader.{h,cpp}` reuses the JSON loader's `LoadResult`/`LoadIssue` reporting and brings a minimal, dependency-free XML reader (namespace prefixes stripped, the five predefined entities plus numeric character references, CDATA, comments, processing instructions, a DOCTYPE with internal subset; malformed XML becomes errors). Like the JSON loader it collects every problem in one pass and produces a net only when there are no errors.

**The eggembt conventions.** A transition's `<toolspecific tool="eggembt" kind="controllable|observable|internal" label="...">` sets kind and SUT label (other tools' toolspecifics are ignored silently). A milestone is tagged as a *second* toolspecific so it composes with the kind in any order: `<toolspecific tool="eggembt" kind="milestone" rank="2"/>` (rank ≥ 1, default 1). Without any toolspecific, the transition id's first byte decides as a fallback: `?` → controllable, `!` → observable, anything else → internal **silent** (the id is never used as a label by accident). A controllable or observable transition without a label falls back to its id, stripped of a prefix. Per-place `<initialMarking>`s are **unioned into one initial marking** named after the net; `<capacity>` and place toolspecifics draw warnings, because PNML here yields **no bounds and no target or error markings** — declare those in the JSON form if needed.

The CLI recognises the format by the first non-space byte of the file (`<` means XML, i.e. PNML; anything else goes to the JSON loader as before), `eggembt check --help` says so, and a pre-existing duplicated `--strict` help line was removed while touching that text. `examples/coffee-pnml.pnml` is the shipped example: five transitions with the conventions above, a rank-2 milestone, an inhibitor arc, the `?`-prefix fallback — and a `switch_off` transition that consumes the brewing state and the lamp token, because with no target markings to declare, a net that is one big cycle would have every move vetoed as a livelock of the specification.

Tested in `tests/tst_pnml.cpp` (new suite, 21 tests: XML noise, conventions, fallbacks, inhibitor arcs and weights, the marking union, every reported problem, the shipped example, and a session campaign driven by an imported model) and `cli_test.sh` (`pnml-model`: `check` names the model and its 5 transitions, `--strict` passes, `run --simulate` reaches `coverage complete` with `transitions 5 of 5` and `milestone transitions 1`).

### Item 7: the reference agent shim

> 7. A reference agent shim for C++ SUTs.

`examples/agents/ref_agent.cpp` shows the whole path for a hand-written C++ SUT: a plain `CoffeeMachine` state machine (it knows nothing about Petri nets or the protocol), an `ISutPlugin` wrapper that drives it (`RefSutPlugin`), and `AgentServer` over `FdTransport`/stdio as the agent `eggembt-ref-agent`. Its header comment doubles as the checklist for wrapper authors: map each named initial marking of the model to a real SUT state (`startRun`), acknowledge exactly the inputs handed over (`stimulate` → `Stimulated`), report every model-relevant SUT move in capture order with millisecond timestamps (`Observed`), push **one** `Quiescent` per quiet stretch from a clock-bearing thread, be honest in `capabilities()` about what `postMortem` can do, and stop pushing events after `stopRun`. The model it serves is `examples/agents/coffee.json`; CMake builds it as the `ref-agent` target and hands it to `cli_test.sh` as `REF_AGENT`, whose `reference-agent` scenario runs `eggembt run examples/agents/coffee.json --agent "$REF_AGENT" --quiescence 40` to coverage complete (skipped when the variable is unset, for manual runs).

The scenario caught a real wrapper bug during development: the machine stayed in its brewing state after serving while the model says the serve returns to the idle place — tracker and machine diverged, and later inputs were ignored (a `no output arrived` deviation). The lesson is now part of the example's documentation: a wrapper's SUT state must land exactly where the model says the reported step leaves it.

### Suite status

The suite stands at **15 CTest entries** — 14 unit suites (new: `tst_pnml`) plus `cli_test.sh` (29 scenarios) — all green. Build and check with:

    cmake -S . -B build -G Ninja && cmake --build build && ctest --test-dir build

---

## 15. Update: 16 November 2026 (GLM-5.3-Flash)

Everything above stays untouched. This batch closes §12 items **2 and 5**, quoted verbatim from §12:

> 2. A command-line runner (model, run log, agent address) and a GUI for coverage and backtraces.
> 5. Optionally, states from which no target marking is reachable as a further repellent region.

With this batch, **§12 is closed**: item 1 was void (§13, no Qt parts in this tree), the command-line half of item 2 was delivered in §13, items 3, 4, 6 and 7 in §14, and the rest of this section is items 2 and 5. All changes are marked `GLM-5.3-Flash` in the source; help text and runtime output stay signature-free.

### Item 2: coverage and backtraces, as a textual report

The GUI half of item 2 was this batch's open question — a GUI in a Qt-free, zero-dependency tree — and the decision was Egge's: *"Scratch the idea of a GUI. A textual report is fine."* So the view the item asks for — coverage **and backtraces** — ships as text: `eggembt report` keeps its coverage sections and gains a **Backtraces** section.

For every definitely covered transition, it prints the chain of moves that led into the **first definite visit** of the transition's earliest covered edge, oldest first — the way the campaign first got there. The chain starts at the named initial marking the run began from (`start > coin.insert > button.coffee > coffee.served`); a step whose exact edge is not certain — an ambiguous stretch the run certainly took, but whose edge the observations cannot pin down — renders as `...`, with a legend line when one occurs; a transition whose visits are all transition-level (observed certainly, but never on one certain edge) says so instead of guessing a chain. Silent internal steps appear under their id, since they have no label. Per-state and per-edge traces (first and last visit, arbitrary depth caps) remain available through the existing API (`CoverageDb::stateBacktrace` / `edgeBacktrace`); the report picks the first visit of each transition because "how did the test first get here" is the question a report answers. Tested in `cli_test.sh` (`run-log-continues-and-reports` now checks the section and a chain).

### Item 5: the nontarget region

A state from which **no target marking is reachable** is a place a run can enter but never legitimately leave — whatever wanders in, the run must be restarted from. Such states now form a further repellent region in the ant colony, alongside the catastrophes:

- **Computation.** The colony recomputes the region whenever the graph grows (`refreshNontargetRegionIfGrown`, called from `iterate()` before anything walks or chooses). Seeds are the states behind which a legitimate end is known to exist or the future is unknown: states of kind `Target`, states the target board is still **Seeking**, and expanded states that can still step into unknown territory (an edge into an unexpanded state, or an incomplete expansion cut off by the limits). Everything expanded that reaches none of the seeds — one reverse walk over `ReverseIndex` — is the region; every edge into it is **set** to `AntConfig::nontargetRepel` (1.0, `0` switches it off).
- **Set, not learned.** Unlike the catastrophe repel, the region never accumulates and never evaporates: it is static knowledge about the explored model, overwritten at each recomputation. It is deliberately modest (half a milestone deposit), so uncovered behaviour inside the region can still win when the campaign needs it. An ant that walks in is not killed — it may still cover what lies there; the region only makes the way in unattractive.
- **Two excuses matter** (and one was caught by the existing session tests): the first version repelled the way into a *sought* error marking — the crash-region campaign went nowhere, because the campaign goes to its targets on purpose. Now states the board is still seeking (error markings, deadlocks, livelocks) are seeds like target markings; and unknown territory is presumed innocent, since a target marking might lie beyond the frontier. Satisfied (forbidden) targets are not excused: they are already avoided, and marking them lost is consistent.
- **Consumers.** The colony's input weight multiplies by `exp(-lambda * (repel + region))`, and the suggester folds the region into its repellent-pheromone term (`pheromoneRepelWeight`), so both the ants' walks and the suggestions avoid the way in. Like every pheromone mechanism, the region is ants-only: with the colony switched off it does nothing.

Tested in `tests/tst_ants.cpp` (`statesWithoutATargetMarkingRepel`: exact region values, the spin edge deeper into the region, the input weight `0.1 * exp(-1)`, and the switch; `theRegionSparesUnknownTerritory`: the certainly-lost branch is marked, the branch into unknown territory is not; `theRegionSteersTheSuggester`: the region breaks a two-branch tie towards the healthy branch, and the switch restores the plain tie-break).

### Suite status

The suite stands at **15 CTest entries** — 14 unit suites (`tst_ants` now 16 tests) plus `cli_test.sh` (29 scenarios) — all green. Build and check with:

    cmake -S . -B build -G Ninja && cmake --build build && ctest --test-dir build

---

## 16. Update: 17 November 2026 (GLM-5.3-Flash)

This batch is three things: a global rename, a new pheromone basis (the T-list), and a carry-over factor for the pheromone teaching of earlier rounds. All changes are marked `GLM-5.3-Flash` in the source; help text and runtime output stay signature-free.

### The rename: `final`/`nofinal` → `target`/`nontarget`

The terms were misleading. A *final marking* reads like net-theory's "final state" — a legitimacy property of a state — but the concept here is the tester's **target**: a marking the campaign seeks, steers into, and (when it is intentional) ends at. The batch renames the concept everywhere, in place:

- Identifiers: `finalMarkings`→`targetMarkings`, `StateKind::Final`→`StateKind::Target`, `definiteFinalMarking`→`definiteTargetMarking`, `possiblyFinal`→`possiblyTarget`, `settle(bool final)`→`settle(bool inTarget)`, `goodFinal`/`good_final`→`goodTarget`/`good_target`, `finalMarkingHits`/`finalHits_`→`targetMarkingHits`/`targetHits_`, `noFinalRepel`→`nontargetRepel`, `refreshNoFinalRegionIfGrown`→`refreshNontargetRegionIfGrown`, `Role::Final`→`Role::Target`, test names in sync.
- The model-JSON key `"final"`→`"target"` (`$.markings.target`); `examples/*.json` and the PNML example's comment renamed in sync.
- Output: the stop reason "the good target marking 'X' was reached", the check line "N target," (realigned to two spaces), test scenario names (`good-target`), and `doku.md`/`README.md` prose throughout — §12 item 5 and its §15 quote renamed **in sync** (they remain byte-identical to each other), although every other earlier section stays append-only. Item 5 said, originally:

> 5. Optionally, states from which no final marking is reachable as a further repellent region.

Four things were deliberately **kept**: the help line "only the final report" (`Commands.cpp`, ordinary English about the report, not the concept), the two C++ `final` class specifiers in `tests/tst_session.cpp` (`RunawaySut`, `ReportingSut` — a language keyword), and the foreign PNML fixture `kind="final"` in `tests/tst_pnml.cpp` (it tests that the loader *ignores* unknown toolspecific data, so it must stay foreign). A BeliefTracker comment that read "crediting at a collapse point is final" became "…is definitive" — same reason as the rename.

One compatibility note: model files that use the old `"final"` key no longer have their markings loaded — the loader reads `"target"`, so such a file loads with a warning for the unknown `$.markings.final` key and (having no target markings) the warning that every deadlock will be reported as an error.

### The T-list: (marking, T-list) pairs as a pheromone basis

New guidance from Egge, quoted verbatim:

> Yet another feature, which can be optionally on or off: To each marking, add a list of truncated traces, each of which is called a T-list, from which we arrived to the marking. The length of the list is N. By default, N is 4. The Nth element of the T-list is a mini-hash created from the current trace. The (marking, T-list) pairs are now the basis for pheromone generation instead of mere markings.

**The element of the list is the suffix cascade.** A run's trace is its sequence of steps s_1, …, s_m, each hashed by `stepHashOf()` of the transition that fired — the transition's label if it has one, else `"I:"` + transition id for a silent internal step, hashed by FNV-1a (`stepHash`). With `kTraceSeed` (0x5EED5EED) as the hash of an empty suffix, element k after m steps is

    H_k(m) = mix(H_{k-1}(m-1), s_m)     for k = 1..N,     H_k(0) = kTraceSeed

where `mix` folds one step into a 32-bit carry: xor with the step, multiply with 0x9E3779B9, xor-shift 15, multiply with 0x85EBCA6B, xor-shift 13, multiply with 0xC2B2AE35, xor-shift 16 — a Murmur-style final avalanche, written out so every platform and replay produces the same values. So H_1(m) = mix(kTraceSeed, s_m) depends only on the last step, H_2(m) on the last two, …, and H_N(m) is the mini-hash created from the current trace, truncated to its last N steps — the Nth element of the T-list. Two arrivals at a marking share the first j elements of their T-lists exactly when their traces' last j steps agree. The recurrence lets one rolling pass per arriving step maintain the whole list (`h_[i]` holds element i+1):

    carry = kTraceSeed
    for i in 0..len-1:
        older = h_[i]               // h_[i] held H_{i+1}(m-1)
        h_[i] = mix(carry, s_m)     // becomes H_{i+1}(m)
        carry = older               // H_{i+1}(m-1) is H_i(m-1) for the next i

One arriving step updates all N elements in O(N); pricing a candidate edge extends the list by the edge's step — the same pass on a copy, O(N) per candidate. At reset (a run's begin) all N elements are kTraceSeed, and a trace shorter than an element's length hashes deterministically: with m steps taken, element i+1 has been updated min(i+1, m) times and holds the hash of the whole trace so far — the missing deeper history contributes kTraceSeed, the hash of nothing. Entries at or beyond the configured length stay kTraceSeed, so a pair key is determined by the length alone. This paragraph is the contract; `tests/tst_ants.cpp` (`traceCascadeFollowsTheRecurrence`) checks it against a reference fold, including the short-trace rule, reset, cap, `extended()`, order sensitivity and the shared-suffix property.

**The pairs are a fine layer beside the edges, not a replacement.** The edge channels (attract, repel, nontarget region) and their back-propagation stay exactly as they were; `arrive()` is untouched; the ants' environment moves are untouched (the SUT's own moves have no arrival context an input trace could describe). What is added:

- **A per-pair store** in `AntColony` (`pairs_`, keyed by `(StateId, std::array<uint32_t, 8>)`), with `depositPairAttract`/`depositPairRepel`/`pairAttract`/`pairRepel`/`pairCount`. Deposits land flat at the arrival pair — no back-propagation, the edge channels still do the propagating, the pairs only refine what they already say — capped at `tauMax` like every deposit, evaporating with the same per-iteration rates as the edge channels, entries below 1e-4 on both channels pruned. With the layer off (`tlistLength == 0`) every pair operation is a no-op and the store stays empty.
- **The ants weigh an edge under the pair its target would create**: `inputWeight(e, trace)` = `weight(e, pairAttract(target, trace.extended(step)), pairRepel(target, trace.extended(step)))` — the pair attraction joins the base of the power, the pair repulsion joins the exponent of the decay, exactly where the edge channels sit. The ant carries its own cascade from its start (all kTraceSeed), one step pushed per edge it walks.
- **The suggester consults the same context** per candidate edge: the pair pheromone of `(ed.target, run cascade extended by the edge's step)`, summed over the candidate's edges and folded into the existing pheromone block (attract lowers, repel raises, through `pheromoneAttractWeight` = 1.0 and `pheromoneRepelWeight` = 3.0 as before). Guarded by the run cascade being attached *and* the colony having the layer on.
- **The session deposits the same amounts it already deposits**, into the pair store under the arrival pair. **The advance rule:** the run cascade advances per newly credited **definite step** — one `push` per entry the run record gains (`CoverageDb::creditDefinite` appends a `RunStep` only when the step went over an edge, so `rec.steps` never has edge-less steps), quiescence included only when it credits steps. Reason: `RunRecord.steps` is the shared material of live runs and rebuilds, so advancing on it gives exact replay alignment. A credited **milestone** step deposits `milestoneDeposit(rank)` at `(step.target, cascade-including-that-step)` right in the advance loop; the milestone *reaction site* keeps its belief-spanning edge-channel deposit and its report bookkeeping, so a milestone step that also ends the run as an error keeps its pair deposit (the rebuild applies the same rule) while the report still does not name it — the run failed there, not succeeded. The **catastrophe** deposit lands after `tracker_.endRun` — the run's final settle may still credit the last segment's definite steps — at `(last credited arrival, full cascade)`, or `(initial state, all-seed cascade)` when nothing was credited, with `catastropheDeposit(way)` as before.
- **The rebuild walks its own local `TraceCascade` over `rec.steps`** — the same sequence live credited, so the same pair keys — depositing a pair for every recorded milestone step and one for the run's end, same amounts as the edge deposits.
- **Switches.** `AntConfig::tlistLength`, default 0 (off — the pinned pheromone-amount tests keep their exact values), capped at `kMaxTList` = 8; `--tlist N` (0..8, a required value, absent = off); the campaign `--json` always reports `"tlist"`.

Tested in `tests/tst_ants.cpp` (`traceCascadeFollowsTheRecurrence`, `pairsSeparateRoutesAndAreCapped`: two routes to one arrival state deposit independently and are tauMax-capped, off = no-op; `pairsEvaporateAndArePruned`: the pair store evaporates 0.90/0.98 per iteration and is pruned below 1e-4; `pairWeightRidesOnTheEdgeWeight`: `inputWeight(e, trace)` = plain weight × exp(−pair repel), the edge channels untouched; `pairLayerSteersTheSuggester`: a pair catastrophe repels only its route, a pair milestone attracts only its route, no cascade or layer off = plain tie-break) and `tests/tst_session.cpp` (`thePairLayerFollowsTheRunAndTheRestore`: one run whose first step is a milestone that is also an error transition with a crash post-mortem — the credited step deposits the pair attract 3.0, the crash deposits the pair repel 3.75 under the *same* key, the restore rebuilds both at the carry-over factor, off = `pairCount()` 0) and `cli_test.sh` (`tlist-and-carryover`: `"tlist":4` and `"tlist":0` in the campaign json, plus the usage errors).

### Pheromone carry-over: several rounds are saved, merged, and toned down

New guidance from Egge: pheromones from several rounds are saved and merged, and toned down in the next one. What already existed: the run log (`--log`) saves every round's runs as recordings, and a continued campaign replays them and rebuilds their pheromone teaching (§13, item 4) — the merge is that accumulation, unchanged. What the rebuild lacked was the tone-down: it laid every deposit at the full strength the live run used ("a restored campaign resumes with the full teaching").

Now every deposit the rebuild lays — milestone and catastrophe, edge channels and (marking, T-list) pairs alike — is scaled by `SessionConfig::restoredPheromonePercent`, default **50**: inherited knowledge is second-hand and possibly stale, so a fresh round should weigh what it learns itself higher and keep exploring. The scaling is applied **per deposit, before `tauMax` caps** — a much-marked edge still ends capped at the same ceiling while the single-visit marks around it fade. `100` keeps the old full strength byte for byte, `0` ignores the inherited teaching entirely. The CLI switch is `--restored-pheromones N` (0..100), and the campaign `--json` always reports `"restored_pheromones"`. The factor does **not** compound across rounds: the log stores recordings, not pheromone state, so every continuation re-derives the knowledge at log strength times this factor — exactly the property that keeps the rebuild derivable instead of frozen (the §13 decision stands; §4's "pheromone is not persisted between sessions" remains true: no raw pheromone state is ever stored).

This supersedes the full-strength wording of §13's item 4 and its test amounts: `theRebuildReplaysWhatTheRunTaught` now pins the halved default (the rank-2 milestone at 1.5, the one-input crash at 1.875, flat cases at 1.5), the 100-percent amounts (3.0/3.75, the old exact values), the 0-percent empty store, and the two-run merge (1.5 + 1.5 = 3.0 — several rounds merge as before, each toned down). The pair layer inherits the same rule (see the T-list test above: the restored pair amounts are half the live ones under identical keys).

### Suite status

The suite stands at **15 CTest entries** — 14 unit suites (`tst_ants` now 21 tests, `tst_session` 35) plus `cli_test.sh` (30 scenarios) — all green. Build and check with:

    cmake -S . -B build -G Ninja && cmake --build build && ctest --test-dir build

---

## 17. Update: 17 November 2026 (GLM-5.3-Flash)

This batch is Egge's **crash predictions**: an SUT crash is no longer only a catastrophe to avoid — a *nondeterministic* crash is a bug, so the campaign treats it as a **target** and tries to reproduce it until the experiment is certain the crash is deterministic. Beside it, one small command: `--credits`. All changes are marked `GLM-5.3-Flash` in the source; help text and runtime output stay signature-free.

### Egge's assignment, verbatim

Egge read `userdoc.md` and wrote (byte-exact):

> Reading the user documentation. I'm happy! Detected something interesting: "### 6.7 SUT crashes — `sut_crashed`
>
> The SUT dies, or the connection to it breaks. The run ends as `sut_crashed`.
> The same outcome occurs *on purpose* when a post-mortem policy says `crash`
> (§6.2): the tester crashes the SUT at the error to collect its state. Either
> way the way in is treated as a catastrophe (below)." -- This elicits an interesting special case: Sometimes an SUT crash is nondeterministic. It can be random, rare, sometimes, or often (but not always). Any of these indicate a bug. Therefore an SUT crash should be treated as a target until we are certain that it is deterministic. The user should be able to explicitly predict an SUT crash in the specification, and its probable class.

Four design questions were put to Egge, four answers came back (byte-exact):

| the question | Egge's answer |
|--------------|---------------|
| Where in the model file is a crash prediction declared? | **Both** — on a transition and on a marking |
| What should the specification offer as a probable class? | **Word plus optional number** — a class word as the default calibration, with an optional explicit probability overriding it |
| Should the seek-until-certain rule apply to every crash, or only to predicted ones? | **All crashes** — every crash is sought first under a global default calibration; predictions only refine place and calibration |
| When is a crash "certain to be deterministic"? | **"Credibility level on a binomial distribution."** |

The last answer is Egge's own wording, typed as a free answer. It decided the mathematics: every visit of the predicted place is a Bernoulli trial (crash or no crash), the observed crashes in the attempts give the crash probability a binomial posterior, and "certain" is a *credibility* statement about the future, not an observed rate.

### The specification syntax

A prediction sits on a transition (a crash while handling that stimulus) or on a named target/error marking (a crash while the SUT is certainly in that situation). The value takes three forms:

    "crash": "sometimes"     a class word: random, rare, sometimes, often, always
    "crash": 0.7             shorthand for {"p": 0.7}: the neutral class with an explicit mean
    "crash": {"class": "rare", "p": 0.3, "attempts": 7, "crash_dump": "snapshot", "max_dumps": 2}

`p` must lie strictly inside (0, 1) — a crash that never or always happens is not a prediction, and the Beta prior behind the experiment cannot express either. `attempts` overrides the class's give-up budget; the loader rejects `0` (a budget of zero means nothing), while a *builder* passing `attempts = 0` means "no explicit budget" and falls back to the class. The usual visibility rule applies: the step must be visible to the tester, so an untraced internal transition cannot carry a prediction (its crash could never be attributed), nor can an error transition (a certain reaction, not a probable crash), an initial marking (nothing is observed before the campaign starts the SUT there), or a marking with policy `avoid` (what the tester avoids is never sought).

### The experiment (engine/CrashCredibility.h)

Every **visit** of the predicted place while the prediction is unresolved is one **attempt**; every attributed crash is one **crash**. With k crashes in n attempts the crash probability has the posterior

    Beta(a0 + k, b0 + n - k),     prior Beta(a0, b0) = Beta(k0*p0, k0*(1-p0))

with k0 = 2 pseudo-observations and p0 the prior mean of the class word (or the explicit `p`). The three states a prediction can reach:

- **Certain deterministic.** k = n is the logic of it (a deterministic crash has no misses); the credibility level turns it into a statement about the future: with credibility γ (the `--crash-level` option, default 0.95) the crash probability is at least θ = 0.90 iff

      k = n   and   I_θ(a0 + n, b0) ≤ 1 − γ,

  where I_x(a, b) is the regularized incomplete beta function (the Beta CDF, computed by Lentz's continued fraction). Then the crash is as good as deterministic and the way in is treated like any other crash: a catastrophe. The CLI allows γ in 0.5..1.0; **γ = 1.0 means never certain** (1 − γ = 0 can never be met by a positive CDF), so the experiment always ends at the budget instead. Measured against the closed form, an explicit p = 0.9 (a0 = 1.8, b0 = 0.2) becomes certain at the ninth hit: I_0.9(10.8, 0.2) = 0.0453 ≤ 0.05, while the eighth still seeks (0.0531).
- **Exhausted.** n reaches the give-up budget without certainty. With k ≥ 1 the bug is real: treat the way in like any other crash. With k = 0 the crash never showed up: stop seeking, teach nothing (**Unobserved**).
- **Seeking.** Anything else, including n = 0: the crash is still a target — the board keeps it sought, and the colony's deposits mark the way in **attractive** ("the ant colony marked the way into the crash site as attractive: the crash is still being sought (<key>)"), the inverse of the catastrophe teaching.

The class table (a stronger claim needs fewer reproductions):

| class | p0 | give-up budget |
|-------|-----|----------------|
| always | 0.98 | 2 |
| often | 0.80 | 4 |
| sometimes | 0.50 | 8 |
| rare | 0.10 | 20 |
| random | 0.50 | 20 |

A crash nobody predicted (**implicit**) starts from the neutral prior p0 = 0.5 and the `--crash-attempts` budget (default 6). Explicit `attempts`/`p` in the prediction override the class row; statuses only ever advance, and attempts stop being counted once a prediction is resolved.

### How a run is accounted

- **Attempts.** Every *definite* visit of the predicted place counts — the T-list cascade counts a visit per credited definite step of the predicted transition, unconditionally, and a marking prediction counts when the tracker is certainly in the marking (at the death or when the certain reach is reported). **The death step itself is counted exactly once**: when the in-flight step settles at the event itself (the belief was a single state), the cascade already counted it; when the belief was ambiguous, the settle only comes with the run's final settle — after the crash cursor froze — and the death branch counts it instead. An implicit entry *born at the death* also missed the cascade's call (the db drops attempts on keys without an entry), so it counts its opening visit here. The live check is `deathStepCounted`: the step is credited only when it is not already in the run record's credited transitions.
- **Exhaustion can land mid-run.** The status is re-evaluated after every count, so a campaign with the implicit budget 6 that dies on every run reads "6 attempt(s), 5 crash(es)" on the sixth run: the entry resolved (attempts = 6) before that run's death could be counted, so the sixth crash goes unrecorded and the run ends `Completed`/"coverage complete".
- **The trail.** Every attempt the db accepted is appended to the run's experiment trail (`crash_attempts` in the run record and the run log) — *only the keys the db accepted*: no entry, or an entry no longer seeking, records nothing. Together with the run's `crash_attribution` this is what a restore replays: attribution first (which creates an implicit `t:`-entry when the log names one nobody predicted), then the trail in counting order, then the crash — over the restored runs in order, the statuses evolve exactly as they did live. Verified byte-for-byte in `theCrashExperimentRebuildsExactly`.
- **A known gap.** The dumps a predicted entry took (`crash_dump`/`max_dumps` of the prediction) are *not* re-derived on restore: `DumpRecord` persists `step`, `marking`, `mode`, `files` and `stack` but not which crash entry a dump belonged to, and the replay re-counts only the error-marking budgets. A restored session under-counts a prediction's dump budget (the recorded files and stacks still round-trip); fixing it needs the entry key in the persisted dump record — deliberately left as is, existing behavior, noted here.

### Where the pieces sit

- **`EventKind::Died`** (engine/Events.h): the plug-in observed the SUT die spontaneously — one event, pushed like any other, round-tripping through the run log. The tracker treats it as "no step to reason about, the run is over" (no progress noted); the session ends the run `SutCrashed`. `SutPlugin.h` stays without a Died mention: the duty is documented in Events.h and the reference agent's checklist.
- **Attribution order at the death**: the last credited step's transition (its prediction, if any) → a predicted marking the SUT was certainly in → the implicit entry on the last credited step's transition → nothing ("the SUT died without a credited step: the crash cannot be attributed", the old catastrophe behavior).
- **The target board** folds the entries in: while an entry seeks, its place is a goal like any sought target (a predicted marking keeps its place a goal even when the marking itself is satisfied); deterministic and exhausted entries leave the board — their catastrophe repel takes over; unobserved entries teach nothing.
- **The simulated SUT** gains the two knobs the tests needed: `--sim-crash-after N` dies after N events wherever it is (the seed of an implicit entry), and predicted crashes on *controllable* transitions are seeded automatically — the simulator looks up every labeled controllable transition with a prediction and dies handling that stimulus with the prediction's prior mean, reproducibly under `--sim-seed` (its dice are `rng_() >> 11 / 2^53 < p`, per run reseeded `opt_.seed + generation_`; p = 1.0 always dies). Marking predictions cannot be seeded this way: the simulator only dies at a stimulus it handles.
- **The report.** After the run lines, a `Crash predictions` block names the credibility level and the implicit budget and lists every entry (site, prediction word — with `p=`/`attempts=` when explicit —, status, attempts, crashes, dumps). The campaign `--json` always carries `crash_level`, `crash_attempts` and `crash_predictions` (key, prediction, status, attempts, crashes, dumps) so the schema does not depend on the model. `eggembt report` prints the same block from a run log.
- **The CLI**: `--crash-level L` (0.5..1.0, default 0.95) and `--crash-attempts N` (1..1000000, default 6) — the two knobs of the experiment; `--sim-crash-after N` (0 = never) on the simulator.

### `--credits`

Between the crash design and the hooks, Egge asked for a credits command:

> Feature: Add a new command line parameter --credits. It should tell the following: "Egge Model Based Tester\n Designer: Esa Kettunen\n Copyright (c) Esa Kettunen 2026\n Software written by artificial intelligence: Claude Sonnet 5.5 Medium and GLM-5.3-Flash\n"

followed by:

> Indent the --credit lines with 4 spaces instead of one. Show GLM credits on it own line, starting with "    Software written by artificial intelligence:"

`eggembt credits` and `eggembt --credits` both print, byte for byte:

    Egge Model Based Tester
        Designer: Esa Kettunen
        Copyright (c) Esa Kettunen 2026
        Software written by artificial intelligence: Claude Sonnet 5.5 Medium
        Software written by artificial intelligence: GLM-5.3-Flash

This is the one output in the tool that carries the signature, by Egge's explicit request; the `--credits` scenario in `cli_test.sh` pins it with `cat -A` (the `cat`-style line ends verified). Everything else stays signature-free.

### The tests

- `tests/tst_crash.cpp` (new suite, 5 tests): `closedForms` (I_x against direct Beta-CDF values), `classTable` (every class row), `pinnedStatuses` (the table's statuses at their budgets), `aMissFalsifiesDeterminism` (one miss falsifies determinism for good — k = n is the logic of it; an explicit p = 0.9 is certain at the ninth hit, pinned to the measured I values), `moreHitsAreMoreCertain` (a stronger prior needs fewer hits).
- `tests/tst_session.cpp` (+6 scenarios, now 41): `aPredictedCrashIsCertainAtFirstHit` (class always, first hit: deterministic 1/1, the seek-turned-repel notes, run 2 completes), `anImplicitCrashEntryGrowsToItsBudget` (budget 6: five seeking runs marked attractive, the sixth exhausted and repellent, "6 attempt(s), 5 crash(es)"), `anExhaustedPredictionCatastrophesThenReleases` (rare, budget 4: three seeking runs, the fourth "exhausted: 4 attempt(s), 3 crash(es)", the fifth completes), `aNeverFiringPredictionResolvesUnobserved` (definite visits alone count; no death, no notes, nothing taught), `aPredictedMarkingCrashIsAttributedToTheMarking` (the death while certainly in the marking is attributed `m:boom` over two crash runs; without a death the visits count and the budget resolves unobserved), `theCrashExperimentRebuildsExactly` (the trail + attribution restore to identical entries).
- `tests/tst_jsonnetloader.cpp` (+3 tests, now 35): `crashKeyFormsAreParsed` (word, number shorthand, full object, marking form), `crashKeyIsRejectedWhereItDoesNotApply` (untraced internal, error transition, initial marking, policy avoid), `crashPredictionValuesAreValidated` (0/1/1.5/−0.2, an unknown word, attempts 0, `p` out of range, a boolean value).
- `cli_test.sh` (`crash-predictions`): the predicted crash `t:strike` (class always, snapshot dump) is deterministic at the first hit under `--seed 7 --sim-seed 3`, the table row `t:strike    always      deterministic  1         1        0 / 1`, the json fields; and the implicit block (`--sim-crash-after 2`): `t:back      implicit    exhausted  6         5        -`.

The suite stands at **16 CTest entries** — 15 unit suites (new: `tst_crash`; `tst_session` 41 tests, `tst_jsonnetloader` 35) plus `cli_test.sh` (32 scenarios) — all green. Build and check with:

    cmake -S . -B build -G Ninja && cmake --build build && ctest --test-dir build

## 18. Update: 17 November 2026 (GLM-5.3-Flash)

This batch is Egge's **command hooks**: a shell command attached to a transition, run when that transition's step is certainly fired — synchronously (the run loop waits for it) or asynchronously (fire-and-forget), once or always, optionally guarded by a place. A Petri net model only covers the concurrent/distributed aspect of computing; hooks open the campaign to the rest — e.g. data-oriented testing while the run is going. All changes are marked `GLM-5.3-Flash` in the source; help text and runtime output stay signature-free (the one exception remains `--credits`).

### Egge's assignment, verbatim

Egge wrote (byte-exact):

> Feature: Run a shell command synchronously or asynchronously when a transition fires. Once or always. (Okay it can be controlled by a certain place.) After all, a Petri net model only covers the concurrent/distributed aspect of computing. We may want invoke e.g. data oriented testing, during a run, too.

The fixed design went out together with five questions; five answers came back (byte-exact as stored):

| the question | Egge's answer |
|--------------|---------------|
| When does the command run — on every firing, or only certain steps? | **"You make a valid point. Certain steps only but suspect steps which do not execute need to be reported.,"** (the trailing comma is Egge's own text) |
| Once or always? | **"Always; times limits (Recommended)"** |
| What does the guard place mean? | **"Certainly marked after (Recommended)"** |
| What if the command fails? | **"Note only, run continues"** |
| Hooks or the crash batch first? | **"Finish crash batch first (Recommended)"** |

The answers in the tool's terms:

1. **Certain steps only — and suspect steps which do not execute need to be reported.** A command fires from the *credited* stream, one execution per certainly fired step of its transition; an ambiguous stretch is a "maybe" and fires nothing. But a ruled-out suspicion is a finding: when a resolved stretch may have been a hooked transition's step and was ruled out, the run notes that the command did not run.
2. **Always, with `times` as the bound.** The default runs the command on every qualifying step; `"times": N` caps the executions over the whole campaign (per transition, across runs); `"once"` is spelled `"times": 1`.
3. **Certainly marked after.** The guard place must hold a token in *every* state the tester believes possible **after** the certain step; the guard is read-only.
4. **Note only, run continues.** A failing, timed-out or unlaunchable command is a note in the run record and the report; outcome, run and campaign are untouched.
5. **The crash batch first.** §17 above was finished before hooks; this section is that batch.

### The `command` key

A command sits on a transition, in two spellings:

    "command": "collect.sh"        the string shorthand: synchronous, always, unguarded
    "command": {"run": "verify.sh", "mode": "async", "times": 5, "when": "ready"}
                                   the full object; every key optional except "run"

`mode` is `"sync"` (default) or `"async"`; `times` is the hook's budget over the whole campaign (at least 1; `1` is "once"); `when` names a place of the net (the guard below). The loader rejects an empty command line (in both spellings — the object form's error names `run`), an unknown `mode` word, `times` below 1 and a `when` that names no place; unknown keys inside the object are warnings, and `"command"` itself is now a known transition key. The usual visibility rule applies: the step must be *visible* to the tester, so an untraced internal transition cannot carry a command — "a command needs a visible step: give the transition a label and "traced": true (or make it controllable/observable)". A traced internal transition may (its audit steps are seen), as may controllable and observable transitions.

### When the command runs

The hooks follow the **credited stream**: the run record's `transitions` — every certainly fired step's transition, in counting order, repetitions included (the same stream the crash attempts count, §17). One stream entry, one execution: a hook on a transition credited twice in a run runs twice. A certain step that fans out over several states credits each possible transition exactly once, so a hook runs once per credited transition, never once per edge occurrence.

Three cursors share the per-event cascade (`Session.cpp` `advanceCascade`), each with its own position over the record:

- the **T-list cascade** (§16) pushes every newly credited step into the run's suffix cascade — the pair deposits ride on it;
- the **crash attempts** (§17) count per credited transition and **freeze at the SUT's death**: the experiment of a dead run is closed, and the credits the final settle adds afterwards belong to no attempt;
- the **command hooks** (this section) fire per credited transition and are **never frozen**: a step the SUT certainly took is a step, whatever happened to the SUT afterwards — its command still runs.

Since the first cursor is the suffix cascade of §16, its contract, verbatim (the same paragraph as there):

> **The element of the list is the suffix cascade.** A run's trace is its sequence of steps s_1, …, s_m, each hashed by `stepHashOf()` of the transition that fired — the transition's label if it has one, else `"I:"` + transition id for a silent internal step, hashed by FNV-1a (`stepHash`). With `kTraceSeed` (0x5EED5EED) as the hash of an empty suffix, element k after m steps is
>
>     H_k(m) = mix(H_{k-1}(m-1), s_m)     for k = 1..N,     H_k(0) = kTraceSeed
>
> where `mix` folds one step into a 32-bit carry: xor with the step, multiply with 0x9E3779B9, xor-shift 15, multiply with 0x85EBCA6B, xor-shift 13, multiply with 0xC2B2AE35, xor-shift 16 — a Murmur-style final avalanche, written out so every platform and replay produces the same values. So H_1(m) = mix(kTraceSeed, s_m) depends only on the last step, H_2(m) on the last two, …, and H_N(m) is the mini-hash created from the current trace, truncated to its last N steps — the Nth element of the T-list. Two arrivals at a marking share the first j elements of their T-lists exactly when their traces' last j steps agree. The recurrence lets one rolling pass per arriving step maintain the whole list (`h_[i]` holds element i+1):
>
>     carry = kTraceSeed
>     for i in 0..len-1:
>         older = h_[i]               // h_[i] held H_{i+1}(m-1)
>         h_[i] = mix(carry, s_m)     // becomes H_{i+1}(m)
>         carry = older               // H_{i+1}(m-1) is H_i(m-1) for the next i
>
> One arriving step updates all N elements in O(N); pricing a candidate edge extends the list by the edge's step — the same pass on a copy, O(N) per candidate. At reset (a run's begin) all N elements are kTraceSeed, and a trace shorter than an element's length hashes deterministically: with m steps taken, element i+1 has been updated min(i+1, m) times and holds the hash of the whole trace so far — the missing deeper history contributes kTraceSeed, the hash of nothing. Entries at or beyond the configured length stay kTraceSeed, so a pair key is determined by the length alone. This paragraph is the contract; `tests/tst_ants.cpp` (`traceCascadeFollowsTheRecurrence`) checks it against a reference fold, including the short-trace rule, reset, cap, `extended()`, order sensitivity and the shared-suffix property.

The cascade is brought up to date **per event** (after each pumped event, before the run ends) and once more after the run's final settle — the settle may still credit the last segment's steps, and those credits fire their hooks too. The settle's hooks read the belief captured before `tracker_.endRun` (the tracker clears its belief there; a credit is a fact about the run, not about the cleared tracker).

### Suspect steps which do not execute are reported

An ambiguous stretch fires nothing while it is ambiguous. But a hooked transition **ruled out** of a resolved stretch is a finding: the tracker collects the command-carrying transitions a stretch ruled out, and after the run's final settle every one of them that did *not* certainly fire elsewhere in the run gets its note (byte-exact):

    the command of '<id>' did not run: its suspect step was ruled out

### Sync, async, and the bound

A **synchronous** command blocks the run loop while it runs; the SUT's own events queue up meanwhile and are processed when the command is done — nothing is lost. Its stdout+stderr is captured through a pipe into the record, cut at 4096 bytes (a spent capture budget appends one line `[output truncated]`). The bound is `--hook-timeout S` (1..3600, default 60): a command still running at its deadline is killed (`SIGKILL`), its record says `timeout`, and the run notes it.

An **asynchronous** command is fire-and-forget: eggembt does not wait for it and never kills it; its output goes to /dev/null and its record says `launched` with no exit. At session destruction the children that already ended are reaped (no zombie lingers in the process table); the ones still running are orphaned to init, which reaps them.

The record's status words: `ok` (exit 0), `failed` (a non-zero exit; or killed by a signal of its own; or the fork/pipe failed before any child existed), `timeout` (killed at the bound), `launched` (async, no exit known — the record's exit holds −1, `kHookNoExit`). Every failure is a note and nothing else — Egge's "Note only, run continues" (the notes appear under the run line in the console; the hook records themselves survive a restore, the notes of a live run do not round-trip):

    the command of '<id>' failed (exit N): the run continues
    the command of '<id>' failed: the run continues
    the command of '<id>' timed out after N s: the run continues
    the command of '<id>' could not be launched: <strerror>

### The environment of a command

The command line runs through `sh -c` in eggembt's working directory, with the inherited environment plus four variables — the model's data is never interpolated into the command line itself:

| variable | value |
|----------|-------|
| `EGGEMBT_TRANSITION` | the transition's id |
| `EGGEMBT_TRANSITION_LABEL` | its label (may be empty) |
| `EGGEMBT_RUN` | the run's id within the campaign (0 for the first run) |
| `EGGEMBT_LOG` | the open run log's path (`--log`); absent when there is none |

### The guard: `when`

Egge's "Certainly marked after": the guard names a place, and the command runs only when that place holds a token in **every** state the tester believes possible **after** the certain step. The guard is read-only — a condition on the believed marking, never a way for the command to steer (it cannot deposit tokens). Per event the guard reads the live belief; for the credits the final settle adds, the belief captured before `endRun`. A failed guard runs nothing and is not a failure — no record, no note, no budget spent; a spent budget is equally silent.

### What the run records

Every execution is one **hook record** in the run record — transition id, label, sync or async, the status word, the exit (−1 when unknown) and the captured output. The records round-trip through the run log and are **adopted on restore; the commands are not re-executed** (`CoverageDb::adoptHookRuns`). A restored session shows the same `Command hooks` table as the live one; what it cannot show is the live run's notes (`PersistedRun` has no notes field — the records are what survives).

Three views on one feature:

- the console prints a `Command hooks` table after the run lines (run, mode, status, transition — its label when it has one, exit, first line of the output) whenever any run executed a hook;
- the campaign `--json` always carries `hook_timeout` and `hook_runs` (per run with executions: transition, label, async, status, exit, output), so the schema does not depend on the model;
- the Guidance block names the feature next to the other switches: `commands: N transition(s), timeout S s` — nothing when the model declares no hooks.

### The tests

- `tests/tst_jsonnetloader.cpp` (+3, now 38): `commandKeyFormsAreParsed` (the shorthand, the full object, `"times": 1` as "once"), `commandValuesAreValidated` (an unknown mode word, `times` 0, an unknown guard place, an empty command line in both spellings — the object form's error names `run` —, a missing `run`, a non-string value, unknown keys inside the object warned), `commandNeedsVisibleStep` (the untraced internal rejected, a traced internal accepted).
- `tests/tst_session.cpp` (+8, now 49): `commandHooksRunSyncAndReportFailures` (stdout and stderr captured, exit 3 is a `failed` record plus the byte-exact note, the run completes), `commandHookBudgetLimitsExecutions` (a self-loop under a 12-event cap: exactly two executions in run 1, none in run 2 — the budget is the campaign's), `commandHookGuardReadsTheBeliefAfterTheStep` (two guarded hooks share one command that appends its own transition id; only the hook whose guard holds after its step ran), `anAsyncCommandHookIsLaunchedNotWaited` (`launched`, no exit, the child still writes its marker after the campaign returned), `aTimedOutSyncHookIsKilledAndNoted` (`--hook-timeout 1` against `sleep 30`: `timeout`, the note, the run completes), `aRuledOutSuspectCommandIsReported` (an ambiguous stretch resolved against the hooked transition: no execution, the ruled-out note), `restoredRunsAdoptHookRecordsWithoutReexecuting` (the marker file gains exactly one line live; the restore re-adopts the record without a second line), `commandHooksReceiveTheirContext` (the four environment variables, `EGGEMBT_LOG` only when a log is open).
- `tests/tst_runlogio.cpp` (+1, now 8): `hookRunsRoundTrip` (three records — `ok` with captured output, `timeout` at exit −1, `launched` async — round-trip through the log unchanged).
- `cli_test.sh` (+1 scenario, now 33): `command-hooks` (a sync `printf` hook's output in the table, an async `true` hook as `launched`, the Guidance line `commands: 2 transition(s), timeout 60 s`, the json keys, `--hook-timeout 0` rejected with the bounds in the message, `--hook-timeout 2` reflected in the Guidance line, and `eggembt report` over the log showing the same table).

The suite stands at **16 CTest entries** — 15 unit suites (`tst_session` 49 tests, `tst_jsonnetloader` 38, `tst_runlogio` 8 with the hook-records round-trip) plus `cli_test.sh` (33 scenarios) — all green. Build and check with:

    cmake -S . -B build -G Ninja && cmake --build build && ctest --test-dir build

## 19. Update: 18 November 2026 (DeepSeek V4.1 Flash)

This batch is two things: the project has a new name, and the SUT can now be
**held at an event until the tracker has acknowledged it**. The second is
Egge's *synchronized events*: a fast SUT may run so far ahead that a snapshot
taken for an error transition no longer shows the stack the event left behind;
synchronizing the two sides trades speed for control. All changes are marked
`DeepSeek V4.1 Flash` in the source; help text and runtime output stay signature-free
(the one exception remains `--credits`).

### Egge's assignment, verbatim

Egge wrote (byte-exact):

> I added the project to git and github. After each session, do "git add" for every updated file. I'll be doing the git commits and pushes myself. I changed the name of the project to "Egge's Model-Based Tester", abbreviated "eggembt". Yet another feature: We need an option to synchronize the communication between the SUT and the tracker such that having sent the transition information to the tracker the SUT can only continue after having received an ACK from the tracker. This will slow down the SUT but there will also be more control e.g. in the case of error transitions where you need to be able to make a copy of the stack (which would be inaccurate/misleading if the program is running wild). Another feature: Make the synchronized event an option for named transitions.

Five questions went out with the fixed design; five answers came back
(byte-exact as stored):

| the question | Egge's answer |
|--------------|---------------|
| How far does the rename reach? | **"Docs and titles (Recommended), CMake project name (Recommended), Credits first line (Recommended), The mbt command itself, MBT_* prefix"** |
| Which transitions may be synchronized? | **"Only the SUT's own moves"** |
| How is it spelled in the model? | **""sync": true (Recommended)"** |
| Per transition only, or a campaign switch too? | **"Per-transition and also master switch, default off"** |
| What if the plug-in cannot acknowledge? | **"Please explain why it could not? Okay, note and continue."** |

The answers in the tool's terms:

1. **The rename reaches the docs, the titles, the CMake project, the credits'
   first line, the command and the `MBT_*` prefix** — nothing else; the
   library targets and one macro keep their old names (below).
2. **Only the SUT's own moves.** A synchronized event is an observable step or
   a traced internal one. A controllable transition is rejected: its stimulus
   is acknowledged as a `stimulated` event already, so there is nothing left to
   hold. An invisible internal step is rejected too: the tester never sees it.
3. **`"sync": true`**, per transition, default false.
4. **Both.** The model opts in per transition and the campaign opts in as a
   whole with `--sync-events` (default off); synchronization happens only when
   both agree.
5. **Explain and continue.** A plug-in that cannot acknowledge does not stop
   the campaign; the run is simply unsynchronized, and a note says exactly why
   (once per campaign).

### The rename

The product is **Egge's Model-Based Tester**, abbreviated **`eggembt`** — also
the binary, the command in every example, and the CMake project name. What
changed:

- `project(eggembt)`, target `eggembt`, `EGGEMBT_VERSION`;
- every `eggembt` invocation in the docs, examples and `cli_test.sh` (`EGGEMBT=`);
- the env prefix `MBT_*` → `EGGEMBT_*`: `EGGEMBT_VERSION`, and the hooks'
  `EGGEMBT_TRANSITION`, `EGGEMBT_TRANSITION_LABEL`, `EGGEMBT_RUN`,
  `EGGEMBT_LOG`;
- the agent names (`eggembt-agent`, `eggembt-agent-sim`, `eggembt-ref-agent`)
  and the tests' temp-directory prefix;
- the credits' **first line**, now `Egge's Model-Based Tester` in
  `src/main.cpp`, `README.md` and the `cli_test.sh` pin. That line supersedes
  the §17 render above; §17's quoted texts are Egge's own and stay byte-exact,
  as does the indented render there (history, not documentation of the current
  output).

What did **not** change, deliberately: the library targets `mbt_engine`,
`mbt_core`, `mbt_agent` and the macro `MBT_POSIX_IO`. They are internal names;
renaming them would churn every build file for no reader's benefit.

Two **format tags** changed with backward compatibility, so old files keep
loading:

| format | new tag | still accepted |
|--------|---------|----------------|
| PNML toolspecific | `tool="eggembt"` | `tool="mbt"` |
| run log header | `"format":"eggembt-runlog"` | `"format":"mbt-runlog"` |

The run-log reader now says "not an eggembt run log" when neither matches.

### The `sync` key

    "sync": true        hold the SUT at this transition's event until the tracker acks it

The loader's rules follow Egge's "Only the SUT's own moves": a controllable
transition is an error ("a synchronized event must be the SUT's own move: a
controllable transition is acknowledged as a stimulus already"), and so is an
invisible internal step ("a synchronized event must be visible to the tester:
give it a label and \"traced\": true (or make it observable)"). The value must
be a boolean. `NetBuilder::sync(t)` is the programmatic spelling.

The campaign switch is **`--sync-events`** (default off). With it off the key
is inert — the model may carry it, the run does not hold. The Guidance block
names the state whenever the model marks any:

    sync: 2 transition(s), held until acknowledged (--sync-events)
    sync: 2 transition(s), not held (no --sync-events)

and the campaign `--json` always carries `sync_events` (the switch) and
`sync_transitions` (how many the model marks).

### Holding, and what the ack waits for

The event travels with the ack requirement: `EventQueue::push(..., needsAck)`
sets `SequencedEvent::needsAck`, and a pump step reports
`PumpResult::lastNeedsAck` / `lastAckSeq`. The Session then

1. stops pumping at the held event (nothing after it can be captured yet),
2. handles every consequence of that event — including a post-mortem, which
   must see the stack frozen; that is the whole point of the feature,
3. acknowledges **after** those reactions and **before** the
   `maxEventsPerRun` check, so an error transition's dump happens first and the
   SUT only then moves on.

A run that ends while the SUT is held does not ack: `stop_run`, `shutdown` or
the closing connection releases the SUT instead, so it can always be stopped
or restarted. A synthetic burst (`burstAtStart`) is never held — nothing could
ack it before the run has begun.

The plug-in side is `ISutPlugin::acknowledgeEvent(queueSeq)`, a no-op by
default; the capability is `PluginCapabilities::eventAcks`, the labels travel
as `PluginRunOptions::syncLabels`. The simulated SUT holds its worker thread on
a condition variable while it waits, releases the mutex meanwhile, lets `halt()`
wake it, and counts what happened (`acknowledgements()`, `holding()`,
`dumpSawHeldEvent()` — the last says a post-mortem really saw the held stack).

### The wire protocol

Protocol version stays **1**: every addition is capability-gated and unknown
fields are ignored, so an old agent and an old tracker still talk.

| direction | message | addition |
|-----------|---------|----------|
| agent → tracker | `hello` | `"sync": true` — the agent can hold the SUT at an event |
| tracker → agent | `start_run` | `"sync_labels": [...]` — the run's synchronized labels |
| agent → tracker | `event` | `"seq": N` (per-run number), `"sync": true` (the SUT is held) |
| tracker → agent | `ack` | `{"type":"ack","run":N,"seq":M}` — handled, go on |

`AgentServer` numbers every event of a run and maps the wire sequence back to
the plug-in's queue sequence for `acknowledgeEvent()`; `RemotePlugin` maps the
queue sequence forward to the wire sequence before sending the ack, and drops
the map when the run ends. `docs/PLUGIN_PROTOCOL.md` has the line-by-line
example.

### A plug-in that cannot acknowledge

Capability-gated, and honest about it: the note is

    the plug-in cannot acknowledge events (its capabilities do not offer "sync"):
    the synchronized events are not held, the run is not synchronized

written once per campaign (`syncNoteGiven_`), not once per run. The campaign
goes on unsynchronized; the switch and the key stay as they are, so nothing in
the model or the report pretends otherwise.

### The tests

- `tests/tst_jsonnetloader.cpp` (+2, now 40): `syncKeyIsParsed` (observable,
  traced internal and plain transitions side by side), `syncKeyIsRejectedWhereItDoesNotApply`
  (controllable, invisible internal, non-boolean value).
- `tests/tst_session.cpp` (+4, now 53): `aSynchronizedEventHoldsTheSutUntilTheAck`
  (one held output, one acknowledgement, nothing left holding),
  `withoutTheSwitchNothingIsHeld` (same model, switch off), `thePostMortemSeesTheHeldStack`
  (a snapshot dump on a synchronized error transition: the dump ran while the
  SUT stood still, the ack came after), `aPluginThatCannotAckIsNoted` (the note,
  its once-per-campaign behaviour, nothing held).
- `tests/tst_remote.cpp` (+2, now 19): `synchronizedEventsTravelOverTheProtocol`
  (hello capability, wire seq, ack back to the SUT), `anAgentWithoutTheAckCapabilityIsNoted`,
  plus codec round-trips for the four additions above.
- `cli_test.sh` (+1 scenario, now 34): `sync-events` (the held and unheld
  Guidance lines, the simulated SUT and the agent-sim path, the two `--json`
  keys).

The suite stands at **16 CTest entries** — 15 unit suites (`tst_session` 53
tests, `tst_jsonnetloader` 40, `tst_remote` 19) plus `cli_test.sh` (34
scenarios) — all green. Build and check with:

    cmake -S . -B build -G Ninja && cmake --build build && ctest --test-dir build

## 20. Update: 18 November 2026 (DeepSeek V4.1 Flash)

Two small corrections to §19, both from Egge: the version was premature, and
the AI credits gained a line.

### Egge's assignment, verbatim

> Retract the version to the current git branch, given by e.g. git status. No, create a new header file and update it there. Add DeepSekk V4.1 Flash as an AI credit under --credits-

### The version follows the branch

§19 had left `set(EGGEMBT_VERSION "1.0.0")` in the CMake project. The tree is
on the git branch `v0.0.1` (as `git status` says), so `1.0.0` claimed more than
exists. The number now lives in **`src/Version.h`** and nowhere else:

    #define EGGEMBT_VERSION "v0.0.1"

`src/main.cpp` and `src/cli/Commands.cpp` include that header; CMake no longer
sets the macro (`target_compile_definitions` is gone, and the header is listed
in the `eggembt` sources so it shows up in IDEs). `eggembt version` prints
`eggembt v0.0.1`, and the usage header starts the same way. Bumping the version
means editing one line when the branch moves — and the `version` scenario in
`cli_test.sh` pins it (`^eggembt v0[.]0[.]1$`), so a forgetful bump fails
loudly instead of drifting.

### The new credit line

The credits gain a sixth line, after DeepSeek V4.1 Flash:

    Egge's Model-Based Tester
        Designer: Esa Kettunen
        Copyright (c) Esa Kettunen 2026
        Software written by artificial intelligence: Claude Sonnet 5.5 Medium
        Software written by artificial intelligence: DeepSeek V4.1 Flash
        Software written by artificial intelligence: DeepSeek V4.1 Flash

Egge's message spells the name "DeepSekk"; the credit uses the model's own
spelling, **DeepSeek V4.1 Flash**, like the other AI lines — a name, not prose.
The `credits` scenario pins the line (and the others) byte for byte, and the
`README.md` render now matches. This supersedes the credit renders in §17 and
§19; their quoted texts stay byte-exact history.

### The tests

No new test files, no new scenarios: the `version` pin and the `credits` line
were updated in place in `cli_test.sh`. The suite remains **16 CTest entries**
(`tst_session` 53, `tst_jsonnetloader` 40, `tst_remote` 19) plus `cli_test.sh`
(34 scenarios) — all green:

    cmake -S . -B build -G Ninja && cmake --build build && ctest --test-dir build

## 21. Update: 18 November 2026 (DeepSeek V4.1 Flash)

The campaign is no longer a black box once it ends. Three inspection views
read what the recording holds — the traces a run walked, the coverage per
state and edge, and the pheromone that steered the ants — and a fourth
paints the graph with that coverage. None of them changes the recording: the
traces come from the log's own steps, and the pheromone is rebuilt from them.

### The traces of a campaign

`report --runs` prints every run's recorded steps, oldest first:

    Traces  (the recorded steps of every run, oldest first)
      run 1   sut_crashed   atm-lobby > card.insert > card.read > pin.enter > pin.accepted > withdraw.request > db.down
      run 2   sut_crashed   atm-drivein > card.insert > bank.stolen_report > card.confiscated
      run 3   completed     atm-branch > card.insert > card.read > pin.enter > pin.rejected > ...

A long run keeps its **last 20 steps** behind a `>`-chain that starts with
`...` and ends with `[N steps]`; a step prints its label, or its id when it
has none. `report --trace N` prints one run in full, with the run's detail
(the error or crash note) in parentheses:

    Traces  (run 2, oldest first)
      run 2   sut_crashed   atm-drivein > card.insert > bank.stolen_report > card.confiscated  (error transition 'confiscate': the SUT was crashed for a po...)

The source is `PersistedRun::transitions` — the transition ids the log
recorded — and for an event run, which records no path, its `events` (each
event's label, or `[kind]`). Nothing is re-simulated: a trace shows what was
recorded, which is exactly what an after-the-fact inspection wants. An
out-of-range request is not an error: `--trace 99` on a four-run log prints
`the log has 4 run(s), no run 99` and exits 0. `--trace 0` is a usage error
(the range is 1..1000000), because 0 would silently mean something other than
"no such run".

### Coverage per element

`--coverage-elements` on `run` or `report` adds two arrays to the `--json`
report, under `coverage`:

- `coverage.states[]` — per known marking: `marking`, `definite`,
  `ambiguous`, `covered`, and (when reached) `first_run`/`last_run`;
- `coverage.edges[]` — per known edge: `from`, `transition`, `label` (when
  the transition has one), `to`, and the same four counters.

`covered` is the campaign's own rule — `definite >= --threshold` — and the
run numbers are 1-based, so they index the same runs `--runs` lists. `definite`
counts the visits that lie on the one known path; `ambiguous` keeps what only
possibly happened during an ambiguous stretch (§4), so a reader can tell
"visited" from "may have visited" without guessing. The default report stays
compact: a large graph turns this into a big file, and most readers only want
the summary. The arrays are sorted by graph order, which is deterministic for
a given model.

### The graph, coloured by coverage

`report --dot FILE` now writes the same Graphviz drawing `explore --dot` does,
but coloured by what the campaign reached:

| colour | pen width | meaning |
|--------|-----------|---------|
| `#2e7d32` green | 3 | `definite >= threshold` — covered |
| `#66bb6a` light green | 1 | visited, below the threshold |
| `#f0a000` orange | 1 | `ambiguous > 0` only — possibly visited |
| `#9e9e9e` grey | 1 | known, never visited |

The drawing carries a legend line, so a graph that travels without this text
still explains itself. `explore --dot` keeps its plain drawing: it runs before
any campaign, so it has no coverage to show (it passes no database rather than
an empty one). The 5000-state cap and its error message are unchanged.

### The pheromone dump

`--dump-pheromone FILE` on `run` writes the colony's pheromone after the
campaign, and on `report` after the rebuild:

    {
      "model": "atm", "tlist": 4, "tau_max": 10, "iterations": 37, "ant_walks": 740,
      "edges": [
        {"from": "idle:1, cassette:2, tries:2", "transition": "insert_card",
         "label": "card.insert", "to": "inserting:1, cassette:2, tries:2",
         "attract": 0, "repel": 0.5007, "region": 0}
      ],
      "pairs": [
        {"state": "confiscated:1, cassette:1, tries:2", "tlist": [2597307718, 78764499, ...],
         "attract": 0, "repel": 2.6599}
      ]
    }

The two channels are the ones of §7: `edges[]` is the per-edge layer with the
static `region` (§12, item 5), `pairs[]` the (marking, T-list) store (§16),
where `tlist` is the key in full. Everything is keyed by readable names —
markings, transition ids, T-list elements — because **edge ids are
session-local** and would point at nothing after a restart. `pairs[]` is empty
with the layer off (`--tlist 0`, the default) and holds only entries whose
evaporation has not yet fallen below the floor.

The `AntColony::pairEntries()` view behind the dump is sorted by (state,
list): the store is a hash map, whose order is not a contract, so sorting is
what makes two dumps of the same state byte-identical.

### Rebuilding the pair store

The log records the campaign's steps, not its colony settings. A rebuilt
session therefore starts with `--tlist 0`, and a `report --dump-pheromone`
would show an empty pair store even when the campaign had one. `report` takes
`--tlist N` for exactly this: give it the campaign's value and the rebuild
fills the same pairs. On the smoke campaign below (`examples/atm.json`,
4 runs, `--tlist 4`) the live dump and `report --tlist 4 --dump-pheromone`
both hold the same three pair rows.

A rebuild is an approximation, as §16 and `Session::rebuildPheromones` say: the
recording carries the definite steps and the acknowledged events, so the
rebuilt deposits are the milestone steps' and the catastrophe ends' at the
campaign's `--restored-pheromones` percentage (default 50). That is visible in
the dump: `iterations` and `ant_walks` are 0 on `report`, because the log does
not record how many rounds the campaign ran — the edges and pairs are what the
campaign taught, not a claim about its length.

### Smoke test

    eggembt run examples/atm.json --simulate --max-runs 6 \
        --json run.json --coverage-elements --dump-pheromone ph.json
    eggembt report examples/atm.json --log campaign.log --runs

`run.json` then carries 117 `coverage.states[]` and 126 `coverage.edges[]`
entries, and `ph.json` the same 126 edges with their channels (`pairs` empty at
`--tlist 0`). With `--tlist 4` and four runs the dump holds three pair rows,
which `report --tlist 4 --dump-pheromone` reproduces.

### The tests

- `tests/tst_ants.cpp` (+1 test, now 22):
  `pairEntriesAreReadableAndSorted` — an empty store first, then two deposits,
  the exact channels and lists, and the sort order. The two routes of that
  model may converge on one marking, so a row is identified by its T-list;
- `cli_test.sh` (+1 scenario, now 35): `inspection` — the run's `--json`
  elements and the dump with its pairs and T-list, `--runs`, `--trace 1`,
  `--trace 99`, the two usage errors, then `report --dot --coverage-elements
  --tlist 4 --dump-pheromone`, checking the drawing's colours and legend and
  that the rebuilt pair store is not empty. It also asserts that the dump
  leaks no signature.

The suite stands at **16 CTest entries** — 15 unit suites (`tst_session` 53
tests, `tst_jsonnetloader` 40, `tst_remote` 19) plus `cli_test.sh` (35
scenarios) — all green:

    cmake -S . -B build -G Ninja && cmake --build build && ctest --test-dir build

## 22. Update: 18 November 2026 (DeepSeek V4.1 Flash)

Three follow-ups to §21: a cosmetic fix in the backtrace table, an audit of
the pheromone and ant-colony documentation against the code, and the Jaccard
index written out — the last one at Egge's request ("add some verbatim
explanation on Jaccard").

### The backtrace column

The backtrace table padded every transition name to a fixed 16 characters.
`bank.stolen_report` (18) overran it, so the ATM report read
`bank.stolen_reportatm-drivein > card.insert ...` — a name glued to its
chain. The column now follows the longest name printed (16 minimum) and gets
one separator space on top, because `padRight` leaves a name that fills the
column untouched:

    Backtraces  (the way into the first definite visit of each covered transition, oldest first)
      card.insert        atm-lobby > card.insert
      bank.stolen_report atm-drivein > card.insert > open_session > connect_db > bank.stolen_report
      card.read          atm-lobby > card.insert > open_session > connect_db > ... > card.read

The `inspection` scenario in `cli_test.sh` pins the space
(`bank.stolen_report atm-drivein > card.insert`), so a regression to the
fixed width fails the suite. Nothing else about the table changed — the
anchor rule, the `...` legend and the transition-level-only fallback stay as
§18 described them.

### The pheromone and colony documentation, checked against the code

The colony's numbers were audited against `AntConfig` and
`AntColony::weight()`: 20 ants per iteration, 40 steps per ant, a
back-propagation depth of 5 (an enforced minimum) decaying by 0.7 per step,
evaporation of 10 % (attract) and 2 % (repel), a ceiling of `tauMax` 10, a
target deposit of 2.0, a milestone deposit of 1.5 × rank, a catastrophe
deposit of 3.0 × (1 + 0.25 · min(stimuli, 8)), a static nontarget repel of
1.0, `pEnvironment` 0.5 and an expansion budget of 200 states per round. All
of these match what §16–§18 and the user manual claimed; two statements did
not, and the user manual now states them exactly:

- **The choice rule is written out.** §7.1 spoke of the attract channel
  "lowering the weight's cost" and the repel channel "raising it", which reads
  backwards unless one knows that the weight is a probability. The manual now
  gives the formula the code uses,

      weight(edge) = (attract + 0.1)^alpha · novelty^beta · exp(-lambda · (repel + region))
      novelty      = 1 / (1 + hits)      (0.01 when the edge is saturated)
      alpha = beta = lambda = 1          (the defaults)

  and the two-step draw (between the SUT's own moves and the tester's inputs
  at 0.5 each, then by weight inside the chosen pool).

- **The novelty deposit sits at a state, not on an edge.** §7.1 said "edges
  into states nothing has visited yet get a deposit of 1.0". What
  `AntColony::arrive()` does: an ant standing where an uncovered edge leaves —
  or on a state it cannot expand within the round's budget — deposits 1.0 at
  *that state* and its walk ends; the deposit then back-propagates along the
  way in. The per-edge `1/(1 + hits)` factor in the weight is a second,
  separate thing. Both are now said separately.

Two smaller corrections in the same pass: the manual's command list gained
`credits` (the help text has carried it since the rename, §19), and its
workflow step now points at the Jaccard section below. The early design
overview's looser sentence — "one that reaches a target still being sought,
or unexplored territory, marks the way in as attractive" — stays as history;
it is true of where the deposit lands after back-propagation, but §7.1 is now
the exact statement.

### Jaccard's index, verbatim

For two sets A and B of covered elements, the Jaccard index is

    J(A, B) = |A ∩ B| / |A ∪ B| = |A ∩ B| / (|A| + |B| − |A ∩ B|)

with `|X|` the number of elements in X. It is the fraction of the elements
either side covered that both covered: 1 = the same set, 0 = nothing in
common. Three sets are compared separately, never merged — transitions,
states, edges — because a campaign can cover few transitions over a wide
state space, or the other way round.

What is in a run's sets (`OverlapAnalysis`, fed by `CoverageDb`):

- **states** — every state the run definitely visited, the initial state
  included;
- **transitions** — every transition that certainly fired, including the
  ambiguous stretches' credits (a transition certainly seen to fire, even
  where the exact edge is not certain);
- **edges** — every edge the run definitely traversed.

Presence only: a state visited once and a state visited ten times are one
element each, so the index does not drift with run length or visit counts. A
run that covered nothing has an empty set. The conventions are pinned in
`tests/tst_overlap.cpp`:

    J(A, A) = 1                    every element is in both
    J(∅, ∅) = 1                    0/0 is not computable; nothing = nothing
    J(∅, B) = 0                    for a non-empty B
    campaign sets = the union over its runs; the campaign index is J of those unions
    mean_pairwise = mean over the n(n−1)/2 unordered pairs; 0 for fewer than two runs

A worked example, transitions: A = `{insert_card, open_session, connect_db,
reader_check}`, B = `{insert_card, open_session, connect_db, atm_fail}`;
`|A ∩ B| = 3`, `|A ∪ B| = 5`, `J = 3/5 = 0.60`. And a real one, the ATM demo
of §21 (a four-run log against a six-run log): campaigns 93 % / 80 % / 80 %,
within the four-run log 49 / 8 / 6, within the six-run log 49 / 7 / 6, cross
means 58 / 23 / 22 — per transitions, states, edges.

What it is not: not a coverage measure (the `Coverage` block is), not the
overlap coefficient `|A ∩ B| / min(|A|, |B|)` (which would call a small set
inside a big one 100 %), and not frequency- or order-aware. The console
rounds to whole percent; `--csv` and `--json` carry the exact values, and
both are computed from the run logs, so a replay reproduces the live numbers
exactly.

### The tests

No new suite entries: the backtrace assertion rides inside the `inspection`
scenario. `tst_ants` stays at 22 tests, `cli_test.sh` at 35 scenarios, and
the suite at **16 CTest entries** — all green:

    cmake -S . -B build -G Ninja && cmake --build build && ctest --test-dir build

## 23. Update: 18 November 2026 (DeepSeek V4.1 Flash)

### The gap

There was no installation documentation. `README.md` had a three-line "Build
and test" block; §9 described the original code layout; nothing said what a
user needs beforehand, how to check a build before trusting it, or where a
finished build goes. Worse, the CMake project had no `install()` rules at
all, so "install" had no defined meaning: copying `build/eggembt` by hand was
the only route, and nothing said so.

### The install rules

`CMakeLists.txt` now includes `GNUInstallDirs` and installs the two programs
into `${CMAKE_INSTALL_BINDIR}`:

    install(TARGETS eggembt ref-agent RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

The product is the one command-line program; `ref-agent` travels with it
because it is the quickest way to check a fresh install against a SUT without
writing a wrapper. The internal static libraries (`mbt_engine`, `mbt_core`,
`mbt_agent`) are deliberately not installed — nothing outside links against
them — and there is no man page, shell completion or data payload to install:
the examples stay in the source tree, and the binary never looks for files
beside itself. So:

    cmake --install build --prefix "$HOME/.local"
    # -> $HOME/.local/bin/eggembt, $HOME/.local/bin/ref-agent

with `/usr/local` as the default prefix. Verified by installing into a scratch
prefix and running the installed binaries: `eggembt version` prints
`eggembt v0.0.1`, a simulated campaign ends `coverage complete`, and
`--agent ref-agent` (the installed shim, on `PATH`) passes the process
boundary too.

### `INSTALL.md`

A new top-level guide, the one the question asked for. It covers:

- **what you need** — CMake ≥ 3.16, a C++17 compiler, a thread library
  (`find_package(Threads)`), Ninja optionally, a POSIX shell for `cli_test.sh`
  only; and the platform split: on POSIX (`__unix__`/`__APPLE__`,
  `MBT_POSIX_IO`) the process and socket transports exist, elsewhere only the
  stream transport does, so `--simulate` and `--stdio` work and
  `--agent`/`--connect`/`--listen` are refused with a message that says why;
- **build** — the two-step configure/build with `-DCMAKE_BUILD_TYPE=Release`
  recommended (a campaign is CPU-bound), and what lands in `build/`;
- **test before installing** — `ctest --test-dir build` (16 entries) and the
  standalone `cli_test.sh` invocation with `EGGEMBT`, `EXAMPLES` and
  `REF_AGENT`, from the source tree, ending `35 tests, 0 failure(s)`;
- **use from the build tree** — installation is optional, since the binary has
  no data files to find;
- **install** — `cmake --install build --prefix …`, the two files, `PATH`, and
  the version (`src/Version.h`, following the branch);
- **verify the install** — `version`, `check`, a simulated campaign, and the
  reference-agent coffee campaign;
- **where files are written** — the run log (with its own format version, so
  an incompatible one is refused rather than misread), the `--json`/`--dot`/
  `--dump-pheromone`/`--csv` outputs, and the post-mortem names: the tester
  never writes a dump, the plug-in reports the names it produced, and the
  built-in simulator only reports `dump-N.core`/`dump-N.snapshot` without
  creating them. Nothing goes to `$HOME`; the `EGGEMBT_*` hook variables are
  *set* for a command, never read from the environment;
- **upgrading and removing** — rebuild and reinstall; delete two files to
  remove;
- **troubleshooting** — missing CMake/Ninja, `ctest` finding no tests, a
  `cli_test.sh` run from the wrong directory, refused agent options on
  non-POSIX builds, and an unoptimized default build type.

`README.md`'s build section and its layout block now point at it, and the user
manual's chapter 8 opens by saying it assumes `eggembt` is on `PATH` and
defers the installation itself to `INSTALL.md`.

### The tests

No new test entries: the install path was verified by hand into a scratch
prefix, and the suite is unchanged — **16 CTest entries** (`tst_ants` 22,
`tst_session` 53, `tst_jsonnetloader` 40, `tst_remote` 19) plus
`cli_test.sh` (35 scenarios), all green:

    cmake -S . -B build -G Ninja && cmake --build build && ctest --test-dir build

## 24. Update: 18 November 2026 (DeepSeek V4.1 Flash)

### The lost ack: a Release-only flake

`tst_remote`'s `synchronizedEventsTravelOverTheProtocol` failed 3–5 times in
60 runs of a Release build and never in the unoptimized tree. The failure was
always the same: `acknowledgements() == 1` saw `0`, `holding()` was already
false, and the run itself ended normally (`coverage complete`). Adding a
one-second wait in the test changed nothing, which ruled out "the ack is just
late" and pointed at "the ack was never sent". Instrumenting both ends
confirmed it: the tracker never produced an `ack` message at all, while the
agent's run number, capabilities (`sync`) and the run's notes were all
consistent — so the event was never *held* from the tracker's point of view,
even though the SUT had pushed it as a synchronized one.

The cause is an ordering gap on the tracker side of the wire. The agent's
forwarder numbers a held event and registers which queue sequence its ack must
wake *before* it sends, so that side is ordered:

    // AgentServer::forward
    m.seq = nextEventSeq_++;
    m.syncEvent = e.needsAck;
    if (e.needsAck)
        ackSeqs_[m.seq] = e.seq;
    send(m);

The plug-in receiving it did the opposite:

    // RemotePlugin::onLine -- before the fix
    const bool pushed = queue->push(SutEvent{...}, &qseq, m.syncEvent) == ...;
    std::lock_guard<std::mutex> lock(shared->mutex);
    if (pushed && m.syncEvent && shared->events == queue)
        shared->pendingAcks[qseq] = {m.run, m.seq};

`queue->push()` is what wakes the Session thread, and the Session may pump the
event, reach its ack block (`Session.cpp`, "let it go on") and call
`acknowledgeEvent()` before the pushing thread has written `pendingAcks`. The
lookup then found nothing, the function returned silently, and no ack ever
went on the wire. The SUT stayed held; the run finished through the normal
"coverage complete" suggestion, `stopRun()` released the SUT, and the test
saw a completed run with no ack. A heuristic `sleep` in the test would only
have widened the window, not closed it.

### The fix

`acknowledgeEvent()` now waits on the shared condition variable for the
mapping that is provably coming, and treats the end of the run as the other
way out:

    shared_->changed.wait(lock, [&] {
        return shared_->pendingAcks.count(queueSeq) > 0 || shared_->events == nullptr
               || shared_->shuttingDown || !shared_->fault.empty();
    });

`onLine()` already notifies `changed` right after registering the mapping, so
the ack is sent as soon as the mapping exists; `detachEvents()` now notifies
as well (it used to clear `events` and wait for in-flight pushes without a
wake-up, which would have left the new wait asleep until the process ended),
so a run that ends first still releases the SUT through `stop_run` — the
documented behaviour of a run that ends at a held event (§19), not a special
case. Nothing is timed: the ack is either sent because its event was pushed
and still belongs to a live run, or dropped because the run is over.

The test is back to the direct assertion — the ack must be on the wire when
`runOnce()` returns — so the window is pinned rather than papered over. The
rig delivers lines synchronously, so this is deterministic once the ordering
is right. Verified: **100/100** isolated Release runs of `tst_remote` and
**40/40** in the development tree, then the full suites.

### Warning cleanup

A fresh Release build of the whole tree reported five warnings; all are gone
without changing behaviour:

- `src/io/PnmlLoader.cpp` — the helper `mbtSpecific()` had no callers (every
  reader loops over `el.children` itself and accepts both the `eggembt` and
  legacy `mbt` tool attribute), so it is removed; the compatibility note stays
  where the parsing happens.
- `tests/tst_ants.cpp` — the two step hashes were computed into `hx`/`hy` and
  then recomputed for the cascade; the cascade now pushes the values it has.
- `tests/tst_session.cpp` — `CrashPrediction{CrashClass::Always}` triggered
  `-Wmissing-field-initializers`; the one field is now named.

Both trees build warning-free with `-Wall -Wextra`.

## 25. Update: 18 November 2026 (DeepSeek V4.1 Flash)

### The question

The ant colony and the pheromones have unit tests for their machinery
(deposits, evaporation, back-propagation, determinism, the pair store), but no
test showed that the guidance *buys* anything: nothing measured a campaign
with the colony against the same campaign without it. Egge asked for that
benchmark. This section describes the model, reports the measured numbers and
explains the one finding that decides them: the edge channel alone changes
nothing here, and the `(marking, T-list)` pair layer is what makes the
difference.

### The scenario

A five-step corridor leads to a fork `F`; behind the fork lies the region the
campaign has to cover, a ring of 1000 states with a skip edge (1008 states,
2010 edges in all). The fork has two ways in:

    F --mine-->  B             one step, and the SUT dies handling it
    F --safe1->  C --safe2-> B two steps, safe

Every run starts in the corridor, so every run faces the same fork with the
same pheromone memory. That is the point of the shape: the crash entry leaves
the target board when it is resolved, so only the pheromones can carry the
lesson from one run to the next, and the fork is met afresh by every run, so
the effect is structural rather than a lucky sample. `crashAttempts` is 2 (a
crash entry is exhausted after two attempts, after which a crash is a
catastrophe like any other), the campaign budget is 6 runs of 200 events, and
the two target markings are non-intentional (`idle` in the corridor,
`region-idle` in the region), which keeps the livelock and nontarget-region
analyses from vetoing the moves under test.

The SUT is a synchronous plug-in: `stimulate()` applies the input in the
caller's thread and pushes `Stimulated` (then `Died` for `mine`) at once, with
no own moves and no quiescence report. So the benchmark measures the engine's
own guidance work, not scheduling.

### The numbers

Region 1000, graph seed 7 (the skip edge jumps 9), 6 runs of 200 events, three
campaign seeds (the seeded tie-breaks), wall clock on the development
machine:

     seed 1 no ants          crashes 5 long 1 covered 201 stimuli 230 (453 ms)
     seed 1 ants, no T-list  crashes 5 long 1 covered 201 stimuli 230 (488 ms)
     seed 1 ants, T-list 4   crashes 3 long 3 covered 586 stimuli 618 (1195 ms)
     seed 2 no ants          crashes 5 long 1 covered 201 stimuli 230 (434 ms)
     seed 2 ants, no T-list  crashes 5 long 1 covered 201 stimuli 230 (471 ms)
     seed 2 ants, T-list 4   crashes 3 long 3 covered 585 stimuli 618 (1180 ms)
     seed 3 no ants          crashes 5 long 1 covered 201 stimuli 230 (448 ms)
     seed 3 ants, no T-list  crashes 5 long 1 covered 201 stimuli 230 (480 ms)
     seed 3 ants, T-list 4   crashes 3 long 3 covered 586 stimuli 618 (1203 ms)

("long" counts runs that reached at least half the event budget.) Without the
colony the campaign explores the region once — the first run — and walks into
the crashing shortcut at the start of each of the five following runs: one
long run, five crashes, 201 edges (the region's first-run share plus the
corridor and the fork). With the colony's edge pheromone alone the outcome is
identical on every seed. With both layers the campaign alternates a safe run
and a crash: three long runs, three crashes, 586 edges — about 2.9× the
coverage for the same run budget, at about 2.6× the time per campaign.

### Why the edge channel alone changes nothing

The edge channel saturates. A still-sought crash is *attractive* on purpose
(§16: the campaign must come back and reproduce the crash), and the ants'
target walks keep passing through the region's entry state `B` on their safe
way to the target, so the shared, per-state edge values around the fork sit at
`tauMax = 10` on both sides of the choice. The later catastrophe repel
(`3.0 · (1 + 0.25 · min(stimuli, 8))`, deposited when a crash is no longer
sought) lands on the same saturated values and cannot make the shortcut look
worse than its neighbours; measured: edge-only ants reproduce the no-ant
numbers to the event.

The pair layer carries no ant deposits at all — `depositPairAttract` and
`depositPairRepel` are called only from `Session.cpp`, flat at the run
record's arrival pair, with no back-propagation and no participation by the
walks. The crash's pair is `(B, T-list ending in mine)`: a walk that reaches
`B` safely arrives through `safe1, safe2` and lands on a different pair. So
the pair that names *this way into B* keeps its catastrophe repel while the
walks flood the neighbourhood, and the suggester's pair term steers the next
run onto the safe way. This is the decisive finding: it is not that "ants
help", it is that one shared value per state cannot carry "this way in is
fatal" while the target walks keep saying "this state is on the way"; the
second memory, keyed by how the state was reached, can.

### The test

`tests/tst_pheromone.cpp`, one test (`thePheromonesKeepTheCampaignOffACrashingShortcut`),
runs the three configurations on seeds 1–3, prints the table above (with
times), and asserts per seed rather than on an average:

    plain: longRuns == 1, crashes == 5
    edge:  covered <= plain, crashes >= plain
    pair:  longRuns >= plain + 2, crashes <= plain - 2,
           covered >= 2 * plain, covered > 2 * edge, stimuli >= 2 * plain

The model is deterministic (the seeds vary only seeded tie-breaks and the
region's skip edge), so the assertions are exact, and the graph seed 8 was
checked by hand during development: same shape, 201 / 201 / 586 edges.

Cost: about 6.5 s, the slowest unit suite (the guided campaigns are ~1.2 s
each). That is the price of 1000 states; the threshold is the point, not the
size — a bigger graph measures the same thing for minutes and weakens it,
because the repel evaporates at 2 % per suggestion (a memory of roughly 200
suggestions). A side experiment, not part of the suite: region 10000, 4 runs
of 1000 events — no ants 1001 edges / 3 crashes, edge-only identical, pair
1994 edges / 2 crashes, the guided campaign ~48 s. The 1000-state, 200-event
shape keeps the fork's lesson inside the memory's reach and the suite fast.

`tst_pheromone` is CTest entry 17 (it links only `mbt_engine`; the synchronous
SUT is local to the test). `README.md`'s suite summary and `INSTALL.md`'s test
count now say 17 entries / 16 unit suites, and `INSTALL.md` warns that this is
the suite's slow one. Verified:

    ctest --test-dir build        # 17/17, ~48 s
    sh cli_test.sh                # 35 tests, 0 failure(s)

## 26. Update: 18 November 2026 (DeepSeek V4.1 Flash)

### The requirement

Egge asked for reserved lifecycle transition labels — `start`, `restart`,
`stop`, `pause` — for SUTs that cannot be restarted from the command line. The
case is an SUT whose process or application is brought up by hand or by its own
controls, so the plug-in's `start_run` can attach but cannot start or restart
it: the model itself has to press the SUT's start/restart/stop/pause control.
The design question was what the engine does with such a transition; Egge chose
the recommended option, "plus a new label called `resume` (to be used after
`pause`)". This section is that design.

### The words

The reserved set is `start`, `restart`, `stop`, `pause`, `resume` — exactly,
case-sensitively, and only on a *controllable* transition. The JSON loader
recognizes the label (`Transition::lifecycle`) and warns when it sees one,
because the word now carries engine meaning; on an observable or internal
transition the same word stays an ordinary label (a SUT may legitimately report
"start"), and a warning says so. Marking names are untouched: the examples
already have an initial marking named `start` (`drink-machine`, `elevator-door`,
`heartbeat-livelock`) and it stays a name. `NetBuilder` derives the action from
the label for hand-built models, like the loader does.

The tester still *sends* the label as a stimulus: the SUT's own control is what
restarts it, which is the whole point. Everything below is the engine's
addition; the wire protocol is unchanged (`docs/PLUGIN_PROTOCOL.md` says what a
plug-in should do with the labels, and that a plug-in that cannot operate the
control reports a fault like for any other input).

### The gate

The engine keeps one piece of state beside the belief: the lifecycle gate.

| gate | offered | silence |
|------|---------|---------|
| **Running** | ordinary inputs; lifecycle transitions that have post arcs (ordinary model steps) | a fault after `--idle-timeout`, as before |
| **Paused** | `resume`, `stop`, `restart` only | expected: no narrowing, no deadlock verdict, no idle abort |
| **Down** | `start`, `restart` only | expected, same |

The gate is not a coverage state and does not enter the goal search: while it
is not Running the suggester returns the way back directly (`resume` before
`stop`/`restart`, `start` before `restart`), and with none in the model the run
is `stuck:` and says why. `stop` and `pause` set `Down`/`Paused`; `start`,
`restart` and `resume` set `Running`; a `Quiescent` event while not Running is
accepted quietly (`BeliefTracker::onEvent`) instead of being narrowed into the
belief or read as a deadlock, and the Session's idle timeout is disabled (it is
"no event although one was expected" only while the SUT runs).

### The landing

Whether the model's marking moves is decided by the transition's post arcs:

- **with post arcs** — it is an ordinary model step. Pause can go
  `running -> paused`, stop `running -> down`, resume `paused -> running`, and
  the suggester can route a goal through them. The gate applies on top.
- **without post arcs** — it is *engine-landed*. The token game does not fire
  it: `ReachabilityGraph::expand` counts it as enabled (a state whose only way
  on is a restart is not a dead end) but creates no edge, so it adds no
  phantom self-loop at every state and no coverage goal. The engine supplies
  the position:
  - `start`, `restart` — the run's initial marking (the SUT is back at its
    beginning). With a pre-arc but no post arc the pre-arc is a pure guard.
  - `resume` — the belief the `pause` suspended. Only a definite pre-pause
    belief is restored; otherwise the gate simply reopens.
  - `stop`, `pause` — no landing; the gate is the whole effect. (An arc-less
    `stop`/`pause` is therefore never offered while Running: with no arcs there
    is no way to say *when*; give it post arcs to make it a model step.)

A model that has an arc-less `start` or `restart` begins **every run with the
gate Down**: the campaign presses the SUT's own control before it tests
anything. That is the "cannot be restarted from the command line" contract —
the engine cannot restart such a SUT itself, so the model's restart does it,
and then the run continues from the initial marking with the coverage, the
pheromones and the run record intact.

The landing is a gap in the path (no RG edge leads to it). `landAt()` credits
the path so far, credits the lifecycle transition on its own
(`CoverageDb::creditTransition`: a hit, a `transitions` entry, no edge, no
backtrace), and continues the same run from the landing with
`creditDefinite(state, kNoEdge)` — the db's documented "definitely visited,
previous definite state is not the predecessor". The lifecycle transition is
therefore covered, but it has no backtrace line. `SimulatedSut` mirrors the
landing: on an engine-landed start/restart it resets its own marking to the
run's initial one, so the simulation cannot drift away from the model.

### The tests

`tests/tst_lifecycle.cpp`, six tests, all fast:

- the exact words are recognized (and `Start`/`started` are not);
- an arc-less start begins Down, is the only candidate, lands at the initial
  marking, is credited once, and afterwards `start` is no longer offered while
  `go` is;
- pause through an ordinary edge moves the model to `paused`, offers only
  `resume`, tolerates a `Quiescent` without narrowing, and an arc-less resume
  lands back at the pre-pause marking;
- stop (arc-less) closes the gate to `start` only, quiescence is quiet, and
  start reopens it and lands at the initial marking;
- a pause/resume/go run is persisted and replayed into a fresh graph and a
  fresh db: the replay succeeds and reproduces the coverage exactly, the
  engine-landed `resume`'s credit included; the same run as a *path* record
  (transition ids, no events) is refused, because an engine-landed step has no
  RG edge to replay;
- a whole campaign against `SimulatedSut`: the first stimulus is the model's
  `start`, then `go`, the run completes at the intentional target, the report
  notes the start, and both transitions are covered.

`tst_jsonnetloader` gained `reservedLifecycleLabelsAreParsed`: the controllable
`restart` becomes a lifecycle input (and is not warned about being arc-less),
the observable `start` stays ordinary and is warned about the reserved word.
No `cli_test.sh` scenario was added: the CLI path is the same loader plus
Session, and the campaign-level test drives the same `SimulatedSut` the CLI
does.

The suite is now **18 CTest entries** — 17 unit suites (`tst_lifecycle` is new;
`tst_ants` 22, `tst_session` 53, `tst_jsonnetloader` 41, `tst_remote` 19) plus
`cli_test.sh` (35 scenarios) — all green:

    ctest --test-dir build        # 18/18, ~38 s
    sh cli_test.sh                # 35 tests, 0 failure(s)

`README.md` and `INSTALL.md` say 18 entries / 17 unit suites; the user manual
has the new annotation in §2.3 and two glossary entries, and
`docs/PLUGIN_PROTOCOL.md` tells a plug-in what the five labels mean on the
wire.

### Known limits

- A run that ends immediately after a landing has no recorded step for it, so
  the catastrophe pair (§16) lands at the last *recorded* arrival — the
  documented single-state approximation; the live deposit and the restore's
  rebuild read the same record, so they still agree.
- The gate is engine state, not a recorded field: the run log stores the
  run's events and the JSON report has the notes, so neither *names* the gate.
  The events are enough to derive it, though, and that is exactly what a
  replay does: `replay()` feeds them through a fresh tracker to rebuild the
  coverage, so the same replay re-derives every gate change and every landing
  (the parity test above pins the coverage side of this). What does not
  survive is the *path* form of a record (transition ids, no events): an
  engine-landed step has no RG edge, so `replay()` refuses such a run with
  "not enabled here" instead of inventing a position. Live runs always record
  events (`toPersisted` prefers them), so this only affects hand-written or
  path-only logs.

## 27. Update: 19 November 2026 (DeepSeek V4.1 Flash)

### The requirements

This is the v0.0.2 batch. Egge asked for five things, verbatim:

> Also write a converted JSON specification into standard Petri net format. Can our extra information be included, too? Even as comments?

> Testing strategy as command line options, and possibly parts of the JSON. declare the budget available in wall-clock time. Declare preferred goals, such as "targets and milestones matter more than coverage" or "coverage is as important as targets", and the same about errors.

> Prepare a test to check if evaporation may ruin progress. At what stage does this happen? Would it only make sense to have some evaporation only between runs?

> We need to explore further the limits of our approach, e.g. in which cases pheromones won't work (and document it).

> Don't you think T-list is a bad name? Would T-suffix be better? Change it everywhere.

The benchmark size was settled earlier: "1000 states is okay. Too long tests are not that informative." Everything below is marked `DeepSeek V4.1 Flash` in the source; `src/Version.h` is now `v0.0.2`.

### PNML export

`src/io/PnmlWriter.{h,cpp}` is the counterpiece of `PnmlLoader`. `eggembt
export MODEL [--output FILE] [--strict]` loads the model through the same
validating path as every other command and writes a standard PNML document
(ISO/IEC 15909-2). Without `--output` the document is the only thing on
standard output, so it can be redirected; loader warnings stay on standard
error. With `--output`, a one-line summary names the file and its place and
transition counts.

**What standard PNML carries.** Places and transitions with their names,
normal arcs, inhibitor arcs (`<type value="inhibitor"/>`), their weights
(`<inscription>`), and **one** initial marking. PNML has no notion of "one of
several initial markings", so the model's *first* initial marking is the
standard one; the file's header comment says so explicitly.

**What eggembt adds in the loader's own dialect.** `PnmlLoader` reads a
transition's kind, label and milestone from `<toolspecific tool="eggembt"
kind="..." label="..." rank="..."/>`; the writer emits exactly that, so those
three survive an export/import round trip (kind, wire label, milestone rank;
`traced` is emitted too and read back). An unsafe XML id (a place called `1
bad place`) is replaced by `pN`/`tN`; the original stays in `<name>` and in
the annotations, but the imported net necessarily uses the generated id.

**Everything PNML cannot carry** — the other initial markings, target and
error markings with their match mode, policies, hit budgets and crash
settings, place bounds, transition tags (error, unpredictable, sync) and
the testing strategy — is
written twice:

- as XML comments beside the elements, for a human reading the file:
  `<!-- transition "press_coffee": controllable, label "button.coffee",
  traced -->`, `<!-- place "busy": bound 3 -->`, `<!-- target "idle"
  (exact): idle=1 -->`;
- as one machine-readable block at the end of the page:
  `<toolspecific tool="eggembt" version="v0.0.2"><text>{...}</text></toolspecific>`,
  whose JSON holds the same information in a structured form.

The block is deliberately a *tool-specific extension*: `PnmlLoader` ignores
it, and a PNML import therefore loses every annotation. The JSON model stays
the complete specification; the export's header comment states the contract.
The round-trip test proves both halves: the standard net re-imports with the
same places, transitions, weights, inhibitor arcs and kinds, and the
annotation block parses as the JSON that names the strategy, all initial,
target and error markings, the bounds and the transition tags.

One bug found while wiring it: the arc lambda indexed the place table with a
transition index (and vice versa), so every arc after the first was wrong.
The fixed writer takes the arc's place and transition separately and only
uses `intoPlace` to choose the attribute direction; the export test now
pins `source="idle" target="insert_coin"` for the drink machine.

### The testing strategy

The model can declare its preference at the root, beside `"places"`:

```json
"strategy": {"goals": "targets", "errors": "seek"}
```

Both keys are optional; missing means `balanced`. `JsonNetLoader` validates
the words and reports unknown keys as warnings, like elsewhere. The
`Session` resolves the effective strategy in this order: the command line
(`--goal-priority`, `--error-priority`), then the model, then Balanced. The
resolved values stay in `SessionConfig::goalPriority`/`errorPriority` (as
optionals with a value), so the reports can name them.

The profiles are expressed in the suggester's own currency (steps; lower
wins) and in the ants' deposits:

| profile | what it changes |
|---------|-----------------|
| goals `balanced` | the historical weights: target 0, new transition 0, edge 2, frontier 4 extra steps |
| goals `targets` | new transition 2, edge 4, frontier 8; `depositTarget` ×1.5, `depositMilestone` ×1.5 |
| goals `coverage` | target 4, edge 0, frontier 1 |
| errors `balanced` | `riskWeight` 2, `hazardWeight` 20 |
| errors `seek` | `riskWeight` 1, `hazardWeight` 10 |
| errors `avoid` | `riskWeight` 5, `hazardWeight` 40 |

The two halves are independent, and the command line overrides one at a
time. `check` prints the model's values, the campaign's `Guidance` block
prints the effective ones, and `--json` carries `"strategy": {"goals": ...,
"errors": ...}`. A test in `tst_suggest` proves the choice really flips: a
still-sought error two steps away wins under `targets` and loses under
`balanced` and `coverage`; `tst_session` pins every weight of every profile
and the override order.

### The wall-clock budget

`SessionConfig::timeBudget` is a `std::chrono::milliseconds`, default 0 (no
limit). `--budget DURATION` accepts a bare number (seconds), or `ms`, `s`,
`m`, `h`; the value must be positive and at most 30 days. The deadline spans
the **whole campaign**: `runCampaign` sets it once, checks it before every
run, and `runOnce` checks it at every turn of its loop. A run that meets the
deadline ends as `Completed` with the detail `campaign time budget reached`
and `budgetReached = true`; the campaign's `stopReason` is `time budget
reached` and the loop stops. Exit codes are untouched — a budget is
planning, not a fault — and `require-complete` still decides between 0 and 4.
A direct `runOnce` with a budget and no campaign gets its own deadline, and
clears it on return; `runCampaign` clears the campaign's deadline when it
returns, so a later campaign on the same session is unbudgeted again. The
campaign JSON carries `time_budget_ms` and per-run `"budget_reached"`.

The unit test runs a campaign with 1 ms and 100 runs: it stops with `time
budget reached`, at most one run exists, that run is `Completed` and
`budgetReached`, the wall time stays under 5 s, and a second campaign on a
fresh session without a budget does not inherit the deadline. The CLI test
uses 1 ms for the stop and 5 s for the "changes nothing" case, and rejects
`0`, `banana` and `2d`.

### Evaporation: the measurement and the boundary

Egge's question was where evaporation ruins progress, and whether it should
happen only between runs. The answer is in the measurement.

`AntColony::iterate(starts, evaporate = true)` now walks the ants and then
calls the new `AntColony::age()` unless the caller asked it not to.
`age()` is the old evaporation block, unchanged: attract loses 10 %, repel
2 %, saturated edges lose their attract, and the (marking, T-suffix) pair
store ages at the same rates and is pruned below 1e-4. `--evaporate-between-runs`
sets `SessionConfig::evaporateBetweenRuns`; the session then passes
`evaporate=false` at every suggestion and calls `ants_.age()` once after the
run — after the final settle, so every deposit of the run has landed.

**At what stage does it hurt?** Inside a *long run*. The repel half-life is
`ln(0.5)/ln(0.98) ≈ 34` suggestions; a run of 200 suggestions keeps
`0.98^200 ≈ 1.8 %` of the lesson its beginning laid. Between short runs it
does not hurt: the lesson is only a few suggestions old when the next run
starts. Boundary aging removes the intra-run loss but has its own cost: a
very long run ages nothing until it ends, so a wrong lesson can steer the
whole run, and a lesson from the previous run survives at 98 % even if the
SUT changed. There is no adaptive rate; this is why the default stays the
historical per-suggestion aging.

The benchmark uses the existing 1000-state diamond (1008 states, 2010
edges; graph seed 7), six runs of 200 events, the pair layer at length 4:

    per-suggestion   crashes 3  long runs 3  covered 586  stimuli 618
    between runs     crashes 2  long runs 4  covered 777  stimuli 812

The same contrast holds for seeds 1–3 at 200 events (585–586 vs 777 covered)
and at 400 events (1185 vs 1568). `tst_pheromone`'s new
`evaporationBetweenRunsKeepsProgress` asserts the structural relations at
200 events for seeds 1 and 2: same number of runs, fewer crashes, more long
runs, at least 100 more covered edges, and more stimuli — the campaign
spends its inputs testing instead of crashing. `tst_ants`'s
`agingCanWaitForTheRunBoundary` pins the mechanics: 200 walking iterations
with `evaporate=false` leave the edge and pair deposits exactly unchanged,
one `age()` leaves 98 %, and 200 per-suggestion iterations leave 1.8 %.

### The limits of the approach

The user manual's new chapter 12 collects what was measured or reasoned
through; the doku summary:

- **Saturation and the sought target.** `tauMax` is 10; a catastrophe
  deposits 3.0 growing to 9.0 with the way in, a target 2.0 per ant visit.
  A still-sought crash is kept attractive by TargetBoard while it is being
  reproduced, on the same edges as the catastrophe's repel. On the
  benchmark, the ants with the edge channel alone are *exactly* as good as
  no ants (`crashes 5, long 1, covered 201`, all three seeds); only the
  pair layer, which distinguishes the way in, improves it. Pheromones
  guided by markings alone do not work when the safe and the bad route
  share their recent history.
- **Exact-match sparsity.** The pair key is the marking plus the complete
  suffix list; there is no similarity or generalisation. Short lists merge
  routes and spill a crash lesson onto safe ones, long lists fragment the
  arrivals and never meet the same route twice; the store can grow with
  the distinct (marking, suffix) combinations until pruning. A mistake
  never repeated on the same recent history teaches nothing.
- **Evaporation versus long runs** (above), including the inverse risk of
  boundary aging.
- **Indistinguishable target and catastrophe.** If the crash and the safe
  continuation share their recent history, no suffix separates them; with
  no prefix at all there is no pair. Model design (an earlier observable,
  a milestone, a sync event) is the answer, not more pheromone.
- **Truncated graphs.** `--max-states`, the suggester's 500-state and the
  ants' 200-state expansion budgets bound the known graph. Pheromone,
  distances and targets live inside it; goals beyond the truncation are
  invisible and the suggester may report `Stuck`. The frontier deposit
  (1.0, ending the ant's walk) is the only signal toward the unknown.
- **Human and timing unpredictability.** `"unpredictable": true` keeps
  catastrophe deposits flat (the measured wall time was a human's), and a
  human action not pinned by an observable can invalidate the suffix the
  pair layer predicted. Run-time livelock and idle timeouts are wall-clock
  judgements and can condemn a slow but honest SUT.
- **The model must be right, and strategy is a trade.** A wrong model is
  learned confidently; `targets` can starve fresh coverage, `coverage` can
  postpone a sought error, `seek` accepts crashes and dumps, `avoid` can
  keep the campaign from the error it was asked to reproduce, and a budget
  can truncate just before the target.

### The T-suffix rename

Egge: "Don't you think T-list is a bad name? Would T-suffix be better?
Change it everywhere." Done in every live place: `src/engine/TSuffix.h`
(renamed from `TList.h`), `TraceCascade`'s constants and accessors
(`kMaxTSuffix`, `tsuffixLength()`, `tsuffix()`), the `"tsuffix"` JSON key,
the `--tsuffix` switch, the pheromone dump, `report --tsuffix`, the tests,
the user manual and `docs/`. The two persistence touchpoints stay
compatible: run logs carry the cascade elements, not the old key name, and
an old dump is read by element, not by field name; `--tlist` is gone (the
tool has never promised old spellings).

Historical sections keep the word where it was written: `doku.md` §§16,
21 and 25 quote Egge's original text and the decisions of their time, and
`README.md`'s earlier entries do the same. The user manual's §7.2 also
keeps Egge's quote verbatim, with a note that the live name is T-suffix.
This section is the record of the rename; no behaviour changed.

### The tests

The suite is now **18 CTest entries — 17 unit suites, 288 unit tests —
plus `cli_test.sh` (39 scenarios)**, all green:

    cmake --build build
    ctest --test-dir build          # 18/18, ~52 s
    sh cli_test.sh                  # 39 tests, 0 failure(s)

New and changed unit tests: `tst_pnml` 24 (export round trip, unsafe ids,
shipped-example round trip; it was 21), `tst_jsonnetloader` 43 (strategy
parsed and validated), `tst_session` 55 (strategy profiles and overrides,
the wall-clock budget), `tst_ants` 23 (boundary aging), `tst_pheromone` 2
(the evaporation contrast), `tst_suggest` 18 (the goal-priority choice
flip). The CLI grew four scenarios — `export`, `strategy`, `budget`,
`evaporation-between-runs` — and moved `version` to v0.0.2. There is still
no new CTest entry: the export and the strategy/budget are tested inside
the suites that own their layers, and `cli_test.sh` covers the command
surface.

The interpretation of the evaporation and limits results is in the user
manual (§7.4, §8.5, §8.6, §12); this section records how they were
produced and what was pinned.

## 28. Update: 19 November 2026 (DeepSeek V4.1 Flash)

### The question, and what was already there

Egge asked whether a marking should carry two kinds of pheromone, good and
bad, with good overriding bad until something avoidable happens, and whether
pheromones attach to markings or to the arcs between them. The answer in the
design discussion was that learned pheromone attaches to arcs — the edge
channels, back-propagated — and that the marking-aware store is the
`(marking, T-suffix)` pair, not a bare marking. The one place where "good
overrides bad until something avoidable happens" is the right rule is the
target phase: a target is a goal while it is `Seeking`, and its states become
`Forbidden` once it is `Satisfied`. That phase logic already existed in
`TargetBoard`, and the suggester already vetoed moves into `Forbidden`
states. Egge chose to make that veto optional rather than add marking-level
pheromone channels.

### The option

`--satisfied-target-policy P` (config:
`SuggesterConfig::satisfiedTargets`, type `SatisfiedTargetPolicy` in
`engine/Suggester.h`) has three values:

| value | goal seeding and plans | hazard |
|-------|------------------------|--------|
| `veto` (default) | satisfied targets are skipped; plans never pass through them; a candidate whose every effect enters one is vetoed | counted |
| `penalise` | satisfied targets are ordinary waypoints; a plan may enter and pass through them | counted |
| `ignore` | as `penalise` | not counted |

The phase transition itself, `TargetBoard::refresh()`, is untouched: the
states are still `Seeking` or `Forbidden`, the board still recomputes them
from the coverage database, and the ant colony still treats a `Forbidden`
arrival as a catastrophe. Only the suggester's reaction changes. The switch
is a campaign option, not a model property, and it is not exported to PNML.

`penalise` keeps the existing hazard term: `forbiddenTargets / edges` times
`hazardWeight` (20 at `balanced`, 10 at `seek`, 40 at `avoid`). That is
finite by design — a goal close enough behind a satisfied target can still
win — and is what separates it from `ignore`. The default in the config, the
help text and every report is `veto`, so a campaign that does not pass the
option behaves exactly as before.

The implementation points are small and local to `Suggester::suggest()`:
the goal-seeding loop no longer skips a `Forbidden` state's own frontier and
uncovered outgoing edges when the veto is off; the backward Dijkstra may
relax a `Forbidden` predecessor; the candidate evaluation counts the hazard
unless the policy is `ignore`; and only `veto` pushes a label into
`Suggestion::vetoed` and refuses the all-forbidden candidate. The console
`Guidance` block prints `satisfied targets: <policy>` beside the target
policies, and `--json` always carries `"satisfied_target_policy"`.

### What does not change

The price of entering a satisfied target is still paid in whole runs. A run
that is certainly in a satisfied error marking ends as `error_reached`; a
deadlock or livelock ends with its own outcome; an intentional target marking
still ends the campaign successfully. The option says whether the suggester
may choose the move, not what the engine does after it. This is the reason
the default stays `veto`: on a model where the satisfied target is a
shortcut rather than the only corridor, `penalise` and `ignore` lose runs.

### The tests

`tests/tst_suggest.cpp` gained two tests:

- `satisfiedTargetPolicyPlansThroughTheTarget` reuses the
  `plansNeverPassThroughForbiddenStates` model (the only uncovered edge lies
  behind an avoided error) and checks that `Penalise` and `Ignore` both
  suggest the input that starts the way through, with an empty `vetoed`
  list. The default still reports `Stuck`.
- `satisfiedTargetPolicyHazard` enters the satisfied target directly:
  `Veto` refuses the only candidate (`vetoed == {"direct"}`), `Penalise`
  suggests it with `hazard == 1.0`, and `Ignore` suggests it with
  `hazard == 0.0`.

`cli_test.sh` gained the `satisfied-target-policy` scenario: the default
console line, the `penalise` line, the `ignore` value in the JSON, and the
usage error for an unknown word. The suite is now **18 CTest entries — 17
unit suites, 290 unit tests — plus `cli_test.sh` (40 scenarios)**, all green:

    cmake --build build
    ctest --test-dir build          # 18/18, ~52 s
    sh cli_test.sh                  # 40 tests, 0 failure(s)

`tst_suggest` is 20 tests; the other suite counts are unchanged. The user
manual documents the option in §6.2, §8.3, §8.5, §10.1, §10.2, §11 and
§12.8; `README.md` and `INSTALL.md` carry the updated counts and the new
scenario.

## 29. Update: 19 November 2026 (DeepSeek V4.1 Flash)

### The version

Egge committed the batch as `c1e389c` on branch `v0.0.3` and named the new
version v0.0.3. The satisfied-target phase policy of §28 is that version.
`src/Version.h` (and therefore `eggembt version` and the usage header), the
`version` scenario in `cli_test.sh`, the `INSTALL.md` sample and the
`README.md` changelog now say v0.0.3; the v0.0.2 changelog entry keeps its own
feature list and its historical test counts. No behaviour changed.

## 30. Update: 19 November 2026 (DeepSeek V4.1 Flash)

### The request

Egge: "It is possible that there are transitions, which fire with a very low
probability. It should be possible to mark them as such, and it would be a
good idea to have an option to hunt them down even if this meant that the
rest of the RG exceeds permissible coverage."

Two things are asked for, and they are separate on purpose. Marking is a
property of the model: this move is rare. Hunting is a decision about one
campaign: spend it on that move. A model may carry the markers and a campaign
may ignore them; adding a marker then changes no behaviour at all.

### The model key

A transition carries `"rare"` in one of three forms:

```json
{"id": "glitch", "kind": "observable", "label": "glitch", "rare": true}
{"id": "coin", "kind": "controllable", "label": "coin",
 "rare": {"p": 0.001, "attempts": 100, "wait-for": 60000}}
{"id": "noise", "kind": "observable", "label": "noise", "rare": false}
```

- `true` takes the documented defaults: `p` 0.001, `attempts` 100,
  `wait-for` 0 (no dwell).
- An object may set any subset of `p`, `attempts` and `wait-for`; the rest
  keep the defaults. `"rare": false` is the same as leaving the key out.
- `p` must lie strictly between 0 and 1, `attempts` must be an integer of at
  least 1, `wait-for` an integer of at least 0 milliseconds. Unknown keys
  inside the object are warned about like every other object in the loader.
- The transition must be **visible** to the tester: an observable, or a
  traced internal transition with a label. An untraced internal step is
  rejected at load time (`$.transitions[i].rare`), for the same reason the
  error and milestone tags are: the tester would never see the step, so it
  could never tell whether the hunt succeeded. A controllable transition is
  visible by definition.
- `p` is **descriptive only**. The engine cannot verify the SUT's real odds,
  so it is reported in `check`, in the console and in the JSON but never
  used for scheduling. The hunt is driven by the attempt budget, not by a
  probability calculation.

`Net::Transition` carries an `std::optional<RareSpec>`; `NetBuilder::rare()`
sets it for the tests and the simulator. The PNML export writes the contract
into the transition comment (`rare p ..., attempts ..., wait-for ... ms`)
and into the annotation block (`"rare": {"p", "attempts", "wait-for"}`), so
an exported model keeps it for a reader even though a PNML import does not.

### The hunt: `--hunt-rare`

The campaign switch is off by default. With it on, every unseen rare
transition whose attempt budget is not spent is a goal of the new, highest
`GoalTier::Rare`:

- A rare goal outranks targets, fresh transitions, fresh edges and the
  frontier. In the backward Dijkstra and in the candidate evaluation a rare
  goal also wins an **equal-cost tie** against every other tier; without
  that tie-break a fresh uncovered edge on the last step would keep its own
  tier and the hunt would lose the choice it was supposed to make. The
  historical tiers keep their mutual order.
- The **enabling state** is the goal, not the edge. For a move of the SUT
  the tester cannot take the edge; being in the state where the move is
  enabled is what gives it a chance. For a controllable rare transition the
  ordinary candidate path applies — the input is chosen and sent — but only
  after the dwell described below.
- "Seen" means definite transition coverage (`transitionHits > 0`), the same
  rule the rest of the tool uses. An ambiguous "may have happened" does not
  end the hunt.

### The dwell, and the attempt budget

`wait-for` is a **dwell at the enabling state**, not a fixed delay before
some later input. When the suggester says `Dwell` (a rare move of the SUT is
enabled in the belief) or when it chooses a controllable rare input with a
non-zero `wait-for`, the session:

1. notes one rare attempt in `CoverageDb`, even if the same run has already
   dwelt on the same transition;
2. suspends the idle timeout for the duration and tells the belief tracker
   that silence is expected, so the SUT's `Quiescent` report is not treated
   as progress and the deliberate pause is not treated as a hung SUT;
3. waits until the deadline; any real event — `Observed` or `Died` — ends the
   dwell early because the state moved on, and the script is re-planned. A
   withheld controllable input is dropped in that case: it no longer belongs
   to the state it was planned for;
4. at the deadline, sends the withheld controllable input if there is one,
   or simply asks for the next suggestion.

A `Quiescent` report is not an event and is ignored by the dwell logic.
Stopping the campaign and the wall-clock budget are still checked at every
loop turn, so they cut a dwell short like any other wait.

Every dwell is one attempt, even several dwells inside one run. A
controllable rare input sent **without** a dwell (`wait-for` 0) counts its
attempt when it is sent. The attempt counts are campaign-scoped state in
`CoverageDb`; they are not written to the run log, so a campaign continued
from a log starts its attempt budget fresh while `transitionHits` is rebuilt
from the recorded runs. A transition already seen there is therefore not a
goal again.

With `wait-for: 0` a rare move of the SUT produces `Wait`, not `Dwell`: the
campaign still stays put and listens instead of moving away, but there is no
timed silence to suspend the idle timeout for and no dwell to charge an
attempt. That form is bounded by the idle timeout and the run limits, not by
the attempt budget.

### Completeness and the hard limits

Rare transitions outrank coverage, so an otherwise complete campaign does
not declare itself complete while `rareHuntPending()` is true — a rare
transition is still unseen and still has attempts left. `--max-runs`,
`--max-events`, `--max-states` and `--budget` remain hard limits; a campaign
cut short by one of them says so:

    run limit reached; rare transitions still unseen
    time budget reached; rare transitions still unseen

Once the attempt budget is spent and the transition is still unseen, it is
no longer pending: the hunt has given up as the model allowed, and the
campaign can end normally with `coverage complete`. `--require-complete`
judges the campaign as incomplete while a rare transition is still pending,
which is the intended reading of the switch.

### Reporting

`check` names the markers and their contracts:

    rare transitions: 1 (rare_go: p 0.001, 2 attempts, wait 30 ms)

The console `Guidance` block adds one line when the model declares any:

    rare: 1 transition(s), 1 seen, 1 attempt(s) used; hunting (--hunt-rare)
    rare: 1 transition(s), 1 seen, 0 attempt(s) used; not hunted (add --hunt-rare)

The campaign JSON always carries both keys, whether or not the model declares
a rare transition and whether or not the hunt is on: `hunt_rare` (boolean)
and `rare_transitions` (array, possibly empty), each entry
`{transition, label?, p, attempts, wait_for_ms, seen, attempts_used}`.

### The tests

New and changed tests for this batch:

- `tst_jsonnetloader` (45, +2): `rareKeyFormsAreParsed` pins `true`, an
  object, `false` and the defaults; `rareKeyIsValidatedAndNeedsVisibility`
  pins the visibility rule and every range error.
- `tst_suggest` (21, +1): `rareHuntGoalsAndBudgets` pins the priority flip
  against a nearer fresh edge, the `Dwell` and `Wait` forms, the attempt
  budget ending the hunt, and the controllable input's `dwell` delay.
- `tst_session` (58, +3): `aRareDwellSuspendsTheIdleTimeout` (three 120 ms
  dwells under a 50 ms idle timeout, then the ordinary aborted run),
  `aControllableRareInputIsSentAfterItsDwell` (the send is delayed, the
  attempt is counted, the marker is inert without the switch, and
  `wait-for` 0 still counts one attempt), and
  `aRareHuntKeepsTheCampaignAlivePastCoverage` (the unseen rare transition
  defers completeness and the run limit is the hard stop).
- `tst_pnml` (24): the export test now also checks the rare comment and
  annotation.
- `cli_test.sh` (41): the `rare` scenario runs a marker end to end with and
  without `--hunt-rare`, checking the console lines, `hunt_rare`,
  `rare_transitions`, `seen` and `attempts_used`.

The suite stands at 18 CTest entries, 297 unit tests and 41 CLI scenarios.

### The limits of the hunt

- **The markers are a promise, not a measurement.** `p` does not steer
  anything; a model that marks a common move as rare spends attempts on it
  at the cost of coverage. Mark only what is worth that price.
- **An attempt is a dwell, not a trigger.** The budget bounds how often the
  campaign waits, not how often the SUT fires. A hundred dwells of 60 s are
  a hundred minutes of deliberate silence, and every one of them is a
  suggestion not spent on fresh coverage.
- **An unreachable rare transition keeps the campaign alive.** If its
  enabling place never holds a token in the explored graph, no dwell is
  possible and no attempt can be spent, but it stays pending — the campaign
  runs to a hard limit. The stop reason names the rare hunt, so the cause is
  visible rather than silent.
- **The idle timeout cannot watch during a dwell.** A SUT that hangs inside
  a dwell is noticed only when the dwell ends; the budget and a stop request
  still apply. This is the price of asking the SUT to be left alone.
- **Attempt budgets do not survive a log.** A continued campaign starts the
  budget fresh (the hits do survive), so a rare transition can be hunted
  again after every restart of the tool.
- **The hunt is not a probability oracle.** A rare move that the SUT makes
  only under conditions the model does not describe cannot be hunted by
  waiting at a state the model believes in; that remains a modelling
  problem, exactly like the indistinguishable-target limit of §27.

## 31. Update: 19 November 2026 (DeepSeek V4.1 Flash)

### The question

§27 measured the two endpoints — 2 % per suggestion versus 2 % at the run
boundary — and found a real difference. It did not answer whether the
default sits on a plateau or near a cliff, nor whether the outcome follows
the nominal retention `(1 - r)^n`. Egge asked for the sweep, and the answer
turned out to be a plateau wide enough to pin.

### The measurement

The benchmark is the existing 1000-region-state diamond of §27: 1008 states,
2010 edges, graph seed 7, six runs of 200 events, the (marking, T-suffix)
pair layer at length 4. The swept parameter is the per-suggestion repel
evaporation (the attract stays at 10 % unless stated); seeds 1 and 2 are
both measured. The decision variable is the one that matters: how many of
the six runs crash on the shortcut, how many stay productive, and how many
edges the campaign covered.

    per suggestion      crashes  long runs  covered (seed 1 / seed 2)
      rate 0.005          3         3        586 / 586
      rate 0.010          3         3        586 / 586
      rate 0.020          3         3        586 / 585
      rate 0.050          3         3        586 / 586
      rate 0.100          3         3        586 / 586
    between runs
      rate 0.020          2         4        777 / 777
      rate 0.100          2         4        777 / 777
    same loss per run
      (0.04, 100 events)  3         3        286 / 286
      (0.02, 200 events)  3         3        586 / 585
    attract side
      attract 0.50        3         3        586 / 586

The nominal retention over a 200-event run falls from 37 % at 0.005 to
0.02^200 ≈ 0 at 0.10 — the parameter does change the arithmetic — and yet
the outcome does not move. The between-runs option remains visibly better
(2 crashes, 4 productive runs, 777 edges), and the same rate × run length
gives the same guidance profile: halving the run and doubling the rate
changes only how much coverage a shorter run can buy (286 versus 586).

### Why the plateau is flat

The fork is reached about **five suggestions after a run starts**, and the
catastrophe lesson is deposited at the end of the previous run. So only
that short interval has to be survived, not the whole 200-suggestion run:
`0.98^5 ≈ 90 %` of the lesson is still there at the decision, and even
`0.90^5 ≈ 59 %` at the top of the sweep is enough. Two independent
protections carry the rest:

- the (marking, T-suffix) **pair repel** keeps the lesson on the exact route
  that crashed, and it ages no faster than the edge channel; and
- the **safe route around the crash** keeps its attract, so even a weakened
  repel does not make the shortcut attractive by default.

The plateau is therefore a property of the benchmark's timing, not a claim
that evaporation never matters. A cliff would need a much longer interval
between the deposit and the fork — a deeper entry into the region — or a
much weaker deposit; manufacturing one would say more about the test than
about the tool. The test asserts what was measured.

### The test

`tst_pheromone`'s third test was renamed from the planned cliff hunt to
`evaporationParametersSitOnAPlateau`. It prints the whole table and asserts:

- every per-suggestion rate from 0.005 to 0.10 lands on the same crash and
  long-run counts as the 0.005 endpoint, with coverage within the one-edge
  tie-break wobble; the endpoint itself is 3 crashes and 3 productive runs;
- the longest per-suggestion run really used at least 100 suggestions, so
  the flat result is not an artefact of short runs;
- between-runs aging at 0.02 and 0.10 gives 2 crashes, 4 productive runs and
  at least 700 covered edges, and is better than per-suggestion aging on all
  three counts;
- the scaling pair (0.04, 100 events) and (0.02, 200 events) has the same
  crash and long-run counts, with the longer run covering more than 1.5 ×
  the shorter; and
- the attract side at 50 % per suggestion does not collapse: still 3
  crashes, 3 productive runs and at least 585 edges.

The interpretation for a user is in the user manual (§7.4 and §12.3): the
default is safe on this shape, the run boundary is the stronger lever, and
the parameter should be moved only for a model whose fork is much deeper
into the run than this benchmark's.

## 32. Update: 20 November 2026 (DeepSeek V4.1 Flash)

### The question

§27 added the wall-clock budget, but a budget does not say how to spend it:
one campaign can make many short runs or a few long ones, and `--max-events`
is the knob. Egge asked for a state space of about 10,000, made of 3-5
communicating state machines, with few error states and one or two targets
per process, and for a ten-minute comparison with growing run lengths. This
section builds that model and measures which end of the range wins.

### The model

Four machines share a ring of four one-place channels. Machine i runs

    p0 --step--> p1 --step--> p2 --pass--> p0
                     \--send--> p2, and a token into channel i if it is empty
    p2 --recv--> p0 (takes the token from channel i-1)
    p1 --finish--> goal --again--> p0
    p0 --fault--> error --recover--> p0

Every transition is controllable and has a distinct label. The benchmark's
SUT is a synchronous deterministic replica of the controllable part, so the
measurement is the engine's guidance and the run boundary, not the SUT's
speed. Each machine has five places and the four channels are binary. The
reachable state space is exactly 5^4 * 2^4 = 10,000 states over 24 places
and 36 transitions, with 64,000 edges. Each machine has one goal and one
error marking (four of each, matched with `Covers`); the four targets are
ordinary goals, not intentional campaign enders.

The free step p1 -> p2 is deliberate. Without it a full channel also blocks
its machine from moving on, some combinations of machine places and channel
occupancy are unreachable, and the model misses the requested 10,000 (the
first draft had 9,073 states). With it the state space is the product of the
five machine places and the binary channels, and every error marking is
reached in every campaign.

### The measurement

Ten-minute wall-clock budget per campaign; the static analysis runs before
the deadline starts. Ants on, T-suffix 4, `satisfiedTargets = Ignore` (so a
satisfied goal does not reshape the graph under the different run sizes),
`maxRuns` effectively unbounded, `--max-events` 50, 200, 800 and 3200. Three
seeds. The four campaigns of one seed ran in parallel, one per core; the
seeds rotated the core assignment (50/200/800/3200 on cores 0/1/2/3, then
3/2/1/0, then 1/0/3/2), so each run size used each core once and the means
do not inherit a fast core.

    events/run   runs    events    states    edges    edges/event
       50         266    13,283     4,897    12,198      0.918
      200          66    13,043     5,049    12,527      0.960
      800          18    13,726     5,251    13,254      0.966
     3200           4    12,134     4,877    11,798      0.972

    per-seed edges (seed 1 / 2 / 3)
       50: 10,847 / 12,380 / 13,368
      200:  9,851 / 13,774 / 13,955
      800: 12,168 / 12,371 / 15,222
     3200: 11,735 / 11,513 / 12,145

Every campaign stopped on `time budget reached` after 600.1-600.2 s, and all
four error markings were found in every campaign. The run sizes describe the
intended shapes: a mean of 266 runs of 50 events up to 4 runs of 3200.

### What it says

- **Many short runs are not better.** The 50-event configuration made about
  266 runs, four to sixty-six times as many as the others, and covered
  12,198 edges on average — below the 200- and 800-event means, and never
  first in a seed. The extra run boundaries do not buy coverage on this
  model.
- **Very few long runs are not better either.** 3200 events per run made
  four runs per campaign and had the lowest mean, 11,798. It was last on two
  seeds and second on the other. Four runs are too few to let the campaign
  cover much ground, even though each run is long.
- **The middle was best on average, but the spread is small.** 800 events
  per run had the best mean (13,254), winning seeds 1 and 3; 200 won seed 2.
  The best-to-worst mean difference is 12 %, and the seed-to-seed spread
  within one size reaches 41 % (200: 9,851 to 13,955), so the ordering is a
  tendency, not a law.
- **The reliable part is the per-event rate.** Edges per event rose with the
  run length: 0.918 at 50, 0.960 at 200, 0.966 at 800, 0.972 at 3200. A
  short run re-tests the initial region on every restart, so a slightly
  larger share of its events lands on already-covered edges. The campaign
  processes roughly the same number of events in ten minutes at every size
  (12.1k-13.7k on average), so the totals stay within a few percent of each
  other.

The practical answer for this shape: run size is a weak coverage lever. The
tool's 10,000-event default is a safety cap, not a coverage target. Choose
`--max-events` for what a run boundary really controls: how much of a bad
run has to be repeated, how often the SUT setup is paid, and how much of the
model one run can reach before restarting. If coverage is the only goal, a
middle range of a few hundred to a few thousand events per run is a
reasonable place to start; neither extreme is measurably better.

### The test

`tests/tst_comm10k.cpp` builds the model and the synchronous SUT. Its CTest
form is a quick structural check, deliberately without a wall clock: it pins
the 10,000 states and 64,000 edges, then runs three 10-event runs and one
30-event run and checks the run-limit semantics (3 runs versus 1, the
per-run cap, and that the exits are normal). The long form is opt-in:

    ./build/tst_comm10k --long
    ./build/tst_comm10k --run-events 800 --budget 600 --seed 1

`--long` runs the four campaigns one after another with a 600-second budget
each; `--run-events` runs a single configuration, which is how the three
seeds above were measured (one process per core). `--no-ants` and
`--tsuffix` are there for a control run. The benchmark is a new CTest entry;
the suite now stands at 19 CTest entries, 18 unit suites (298 unit tests)
and 41 CLI scenarios.

## 33. Update: 21 November 2026 (DeepSeek V4.1 Flash)

### The question

`deadlock` and `livelock` each denoted a static graph property and a run
outcome without the manual separating them. Egge asked for a thorough
explanation of specification deadlock, SUT deadlock, specification livelock
and SUT livelock, and of the consequences of strongly connected components
with or without milestones and targets. No engine code changed;
`userdoc.md` §6.12 is the explanation, and this section records the
semantics it relies on.

### Model finding vs run outcome

- **Specification deadlock**: an expanded reachability-graph state with no
  enabled transition and kind `Normal` (`ReachabilityGraph::isDeadlock`).
  Target and error states are excluded; if a marking matches both, error
  wins (`Semantics.cpp`). A state cut off by the state limit is not expanded,
  so it is never a deadlock.
- **The `deadlock` outcome**: `BeliefTracker::handleQuiescence` first
  narrows the belief to states with no enabled output or internal
  transition. It reports `StepStatus::Deadlock` only if every remaining
  state is `Normal` and has no enabled transition at all. If every possible
  state owes output or an internal step, the same silence is a `deviation`;
  with no quiescence report the session aborts on `--idle-timeout`. A SUT
  that hangs elsewhere therefore usually appears as `deviation` or
  `aborted`, not as `deadlock`.
- **Specification livelock**: `analyzeLivelocks` reports a strongly
  connected component that contains a cycle, has no outgoing edge, and has
  no `StateKind::Target` member. It is a property of the explored graph
  only; a component is certain when no member is incomplete and no firing
  was dropped by a place bound. Edges into unexpanded states count as
  exits. `TargetBoard` keeps only certain terminal findings as livelock
  targets.
- **SUT livelock**: the `BeliefTracker` watch, enabled by
  `livelockTimeoutMs > 0`. At least `livelockMinEvents` observed events
  without progress and the timeout since the last progress produce
  `StepStatus::Livelock`. Progress is a sent stimulus, a state not seen
  earlier in the run, `possiblyTarget()`, or quiescence. The watch has no
  milestone category and does not require a terminal cycle in the model.

### SCCs, targets, milestones

- A cyclic SCC with an exit is not terminal and produces no livelock
  finding, even if the SUT spins in it. The run-time watch can still flag
  that refusal to leave when it is enabled.
- A terminal cyclic SCC without a target marking is a specification
  livelock. By default the campaign seeks each certain one once
  (`--livelock-policy once`) and then forbids its states.
- A target marking in the terminal SCC legitimizes the whole component,
  plain or intentional. The static test asks only for `StateKind::Target`;
  the intentional flag affects the run (campaign end), not the livelock
  finding. Target states are never deadlocks, and the run-time watch counts
  a target only when it is in the current belief, not merely somewhere in
  the SCC.
- A milestone is a transition annotation (`Transition::milestone`), not a
  marking, so `LivelockAnalysis` cannot see it. A targetless terminal cycle
  with a milestone is still a specification livelock; the milestone's
  1.5 × rank attract deposit does not override the livelock target's
  forbidden state class. In a non-terminal cycle the milestone is an
  ordinary reward on the way.
- Error states are not targets for this test; a terminal cycle that
  contains one can still be a targetless terminal cycle, while the error
  state keeps its own error class on the target board.

The new user-manual table, the target/milestone consequences and the
report-reading checklist are §6.12. Existing §3.2, §5, §6.5, §6.6 and
§12.6 now point to it. While writing it, the `--max-events` description was
corrected in the manual: reaching the per-run limit ends the run normally
(`Completed`, "run length limit", §12.10), it is not an event-queue overflow
abort.

## 34. Update: 22 November 2026 (DeepSeek V4.1 Flash)

### The question

The carry-over (§16) merges the pheromone teaching of every restored run and
tones each deposit down by `--restored-pheromones`, but it never ages the
merge: a three-round-old lesson enters exactly as strong as a one-round-old
one. Egge asked for the missing alternative:

> When merging several runs, there shall be an option to allow for
> evaporation except for "matured" targets, errors, and such, which have
> reached their "visited N times" criterion.

The switch is opt-in. Without it the historical merge is byte for byte what
it was.

### What "matured" means

The code already has the phase. `TargetBoard` assigns `TargetPhase::Satisfied`
when a target's hits reach its policy count, and `StateClass::Forbidden` to
its states. That is the "visited N times" criterion:

- an error marking: `errorTarget(i)`, from `once` (1), `avoid` (0), or
  `until_covered`/`target_hits` (the explicit count or the coverage
  threshold);
- the single deadlock target: `TargetPolicies::deadlock`, `deadlockHits`;
- each certain terminal livelock: `TargetPolicies::livelock`,
  `livelockHits`;
- an intentional target marking: first certain visit.

A milestone is a transition reward, not a marking with a visit count, so it
does not mature. Its attract ages normally -- unless the milestone's
destination state is itself matured, in which case the destination rule
spares the edge: the exception is keyed on the edge's target, not on where
the deposit came from. A crash experiment still `seeking` is not `satisfied`
either; its site keeps the `Seeking` class through the board's last override
and ages until the experiment resolves. A plain, non-intentional target
marking is an idle state for the livelock analysis, not a campaign target,
so it is not spared.

### The merge boundary

`Session::restore` ages **before** replaying the next recording, not after:

    restore run 1: replay, rebuild
    restore run 2: age the rebuilt teaching, replay, rebuild
    restore run 3: age the rebuilt teaching, replay, rebuild
    ...

So run *i*'s deposits fade by one step for every later restored run, and the
newest restored run enters at full rebuilt strength — the merge is
chronological. The step is the ordinary `AntColony::age()` step: attract
loses `evaporationAttract` (10 %), repel `evaporationRepel` (2 %), a
saturated edge loses its attract, and the pair store ages at the same rates
and is pruned below 1e-4. `--restored-pheromones` still scales every rebuilt
deposit first; the merge aging is on top.

The exception is implemented where the channels live. `AntColony::age(bool
spareMatureTargets = false)` refreshes its `TargetBoard`; when the flag is
set, every `Forbidden` state is spared:

- an edge whose **target** is spared keeps both channels (only the
  saturation rule still wipes a saturated attract);
- a `(marking, T-suffix)` pair whose **state** is spared is not aged or
  pruned.

The back-propagated traces *behind* the direct way in are not spared: the
rebuild has no provenance that would attribute them to the matured finding,
and freezing whole predecessor cones would also freeze unrelated knowledge.
The direct edge into the target carries the full deposit; the decayed
predecessors age as usual. This is the documented approximation, in the same
spirit as the rebuild's belief-span approximation (§16).

### Wiring

- `SessionConfig::evaporateRestoredPheromones` (default `false`).
- `Session::restore`: when the switch is on and the campaign's static
  analysis is enabled, `analyzeModel()` runs before the first restored run,
  so terminal livelocks already have target findings. It then calls
  `ants_.age(true)` before every replay once a run exists; otherwise nothing
  changes.
- CLI `--evaporate-restored-pheromones` (a flag), console Guidance
  `restored pheromones: N%; merge evaporation: off | between restored runs,
  matured targets spared`, campaign JSON `evaporate_restored_pheromones`.
- `--evaporate-between-runs` remains the live campaign's aging boundary and
  is independent. `report` keeps replaying and rebuilding without the merge
  switch; its `--dump-pheromone` still shows the un-aged carry-over.

### Tests

- `tests/tst_ants.cpp` (`maturedTargetsAreSparedByMergeEvaporation`): a chain
  ends in an error marking with `targetHits` 2, two paths mature it, and
  `age(true)` leaves the matured way in and its pair at 9.0 while an ordinary
  state's edge falls to 6.3 × 0.98 and its pair to 9.0 × 0.98; `age(false)`
  then ages the matured way in too. `tst_ants` is now 24 tests.
- `tests/tst_session.cpp` (`restoredPheromonesAgeBetweenRuns`): restoring a
  crash first matures a `once` error; two later milestone runs age the rank-2
  milestone attract (3.0, then 3.0 × 0.90 + 3.0) and its pair while the
  error's 3.75 repel and pair survive; the switch off gives 3.0 + 3.0 = 6.0.
  `tst_session` is now 59 tests.
- `cli_test.sh` (`evaporate-restored-pheromones`): the console line and the
  JSON key, plus the default `false` in the existing campaign report. The CLI
  suite is now 42 scenarios.

The full suite is still **19 CTest entries**, 18 unit suites (300 tests) and
42 CLI scenarios, all green. `eggembt version` is `v0.0.5`, matching the git
branch of the same name (`main` is the one exception: its name is not a
version). The design record itself was renamed `doku.md` → `journal.md` in
this batch.

## 35. Open item: temporal logic over places (23 November 2026, DeepSeek V4.1 Flash)

Not implemented. Recorded as the next open item, at Egge's suggestion:

> How about adding temporal logic to the specification, where the places act as predicates? E.g. You could express things like p1 IMPLIES EVENTUALLY p2 AND ALWAYS p3.

### Why it fits

A marking is already a valuation of every place: a place is either marked
(one or more tokens) or not. The reachability graph is therefore a Kripke
structure whose states are markings and whose edges are transition firings,
and the existing target board already reads state predicates (`StateKind`,
error markings, target markings, deadlocks, terminal cycles). What is missing
is a way to say how those predicates relate **over time**: an ordering, a
response obligation, an invariant, a recurrence.

The places-as-predicates reading gives a small, natural language:

- `p` — place `p` holds at least one token in the current marking;
- `p >= n` — later, if token counts are wanted;
- `AND`, `OR`, `NOT`, `IMPLIES` — the usual Boolean connectives;
- `ALWAYS φ` (`G φ`), `EVENTUALLY φ` (`F φ`), `NEXT φ` (`X φ`),
  `φ UNTIL ψ` (`φ U ψ`).

Egge's formula should be written with parentheses, because its two readings
differ: `p1 -> (F p2 AND G p3)` says "whenever `p1` holds, `p2` must come
later and `p3` must hold forever", while `(p1 -> F p2) AND G p3` makes
`ALWAYS p3` unconditional. A model format should accept both spellings
(`IMPLIES`/`->`, `EVENTUALLY`/`F`, `ALWAYS`/`G`) and print the parsed form
back, as the rest of the loader does.

The patterns users actually ask for map onto this directly:

| pattern | formula | meaning |
| ------- | ------- | ------- |
| invariant | `G NOT bad` | `bad` is never marked |
| reachability | `F done` | some run reaches `done` |
| response | `G (request IMPLIES F grant)` | every request is eventually granted |
| recurrence | `G (F tick)` | `tick` happens again and again |
| until | `pending U done` | `pending` holds until `done` |

### Semantics that must be fixed before code

1. **Universal or existential?** A requirement ("every run must grant every
   request") is universal; a test goal ("find a run that reaches `done`") is
   existential. The same syntax should not silently mean both. A `kind` field
   (`require`, `forbid`, `seek`) can choose the reading.
2. **Finite runs.** LTL is usually defined over infinite paths. A test run
   ends, so the choice is finite-trace LTL (`LTLf`) or infinite LTL with the
   final state stuttered forever. The difference matters for `F` and `G` at
   the end of a run and must be documented once, not per property.
3. **Per run or per campaign?** "Eventually" across a campaign that restarts
   the SUT is a different claim from "eventually within one run". The default
   should be per run; a campaign-scoped variant can be added deliberately.
4. **Ambiguous belief.** The tracker often has several possible markings.
   Evaluation should be three-valued: true if true in every possible state,
   false if false in every one, unknown otherwise. Only a definite false
   should fail a `require`/`forbid` property; `unknown` is reported, not
   invented into a bug.
5. **Fairness.** `G (F tick)` over an infinite model path needs a fairness
   assumption (which transitions may be ignored forever). Without it the
   checker reports schedules no real tester would choose. Finite-run
   monitoring avoids the question but cannot prove the property.
6. **Relationship to today's targets.** An intentional target marking is
   roughly `F target`; an invariant is roughly an error marking with zero
   permitted hits. Temporal properties should complement the existing
   target/error/milestone machinery, not silently replace its policies. A
   formula can become a goal (`seek`), a veto (`forbid`), or a pure report.

### Two implementation levels

**Runtime monitor (the small first step).** Evaluate the formula on the
observed events and the belief after every step. A definite violation ends
the run with a new outcome (`property_violated`, distinct from `deviation`),
names the property and the trace prefix, and appears in the console, the run
log and JSON. This needs no product automaton and fits the existing
BeliefTracker; it catches safety violations and missed responses on the
executed path, but it cannot prove that an unexecuted path would violate.

**Static model checking (the expensive step).** Build the product of the
reachability graph with a Büchi automaton for the negated formula (or use CTL
fixed points for the universal/existential fragment), search for a violating
path or a witness, and report the counterexample as markings and transitions.
The graph is lazy and can be cut by `--max-states`, so the result must say
"no violation found within the explored graph", not "proved". Liveness adds
the fairness assumptions above.

**Guidance (the payoff).** Once a property is a goal, the suggester can steer
toward its obligations: `F p` becomes a target, `G NOT p` becomes a
forbidden state, and `request IMPLIES F grant` becomes a response obligation
(do not keep a grant owed forever). This reuses the target board, the
pheromone deposits and the milestone reward rather than inventing a second
guidance system.

### A possible first increment

- Places only, with `p` meaning "marked"; token-count atoms later.
- `G`, `F`, `X`, `U`, `AND`, `OR`, `NOT`, `IMPLIES`, with parentheses.
- `kind: require | forbid | seek`; per-run semantics; three-valued
  evaluation over the belief.
- Runtime monitor first, with counterexample prefix and a new outcome; a
  static search for `require`/`forbid` only when the formula is a safety
  property (no `F` under `G`), so no fairness question arises.
- A model key such as:

```json
{"properties": [
  {"name": "grant", "kind": "require", "formula": "G (request IMPLIES F grant)"},
  {"name": "no-overflow", "kind": "forbid", "formula": "G NOT overflow"},
  {"name": "served-once", "kind": "seek", "formula": "F served"}
]}
```

Open questions for the next design pass: operator precedence and printing,
whether formulas may mention transitions as well as places, bounded versions
(`F<=N p`) for test campaigns, whether a violated `require` exits 1 like a
deviation or gets its own exit code, and how a temporal goal interacts with
a still-sought error when both call for the same next input.

The point of the item is not to replace the model: it is to let the model say
*when* a place fact matters, so a specification can express a protocol
obligation directly instead of encoding it as a hand-built target marking.

## 36. Rename: `nontarget` → `off-target` (23 November 2026, DeepSeek V4.1 Flash)

Egge asked for the term to be replaced everywhere:

> Replace nontarget by off-target everywhere in code and documentation. What we mean is off-target and off-target region.

### What changed

The concept, its computation and every number are unchanged; only the name moved. In code:

- `AntConfig::nontargetRepel` → `AntConfig::offTargetRepel` (still 1.0, `0` = off);
- `AntColony::refreshNontargetRegionIfGrown` → `AntColony::refreshOffTargetRegionIfGrown`
  (called from `iterate()` exactly as before);
- comments, doc comments and the two test comments in `tests/tst_ants.cpp` and
  `tests/tst_pheromone.cpp` now say **off-target region**;
- the query API `AntColony::region(EdgeId)`, the `region_` array, the CLI, the
  model format and every output string keep their names and text — the field
  was never exposed as a model key or a command-line switch.

In the living documentation, `userdoc.md` §5 (the target complement), §7.1
(the weight formula) and the glossary now say **off-target region**, and
`README.md`'s region section names `offTargetRepel` and the **off-target
region**. Egge's original wording in the quoted §12 item — *"states from which
no final marking is reachable as a further repellent region"* — is a quotation
and stays verbatim; the sentence introducing it now says the concept is spelled
"off-target".

Historical journal sections keep the old spelling by the append-only rule.
The identifier chain is now:

    noFinalRepel → nontargetRepel → offTargetRepel
    refreshNoFinalRegionIfGrown → refreshNontargetRegionIfGrown → refreshOffTargetRegionIfGrown

so §16 remains readable as the record of the first rename without pretending
it used today's word.

### Verification

The full suite is unchanged: **19 CTest entries**, 18 unit suites (300 tests)
and 42 CLI scenarios, all green. Nothing in the wire protocol, the run log or
the campaign JSON changed, so `v0.0.6` stays the product version.

## 37. Publication: a formal account of the nets and the colony (23 November 2026, DeepSeek V4.1 Flash)

Egge asked for a publication, preferably in LaTeX, with mathematical
definitions of the project's Petri nets and ant-colony approach:

> Create a new directory and file for a publication, preferably written in LaTeX. Can you obtain a mathematical definition of our kind of Petri nets and our ACO approach?

### What was added

A new tracked directory `publication/`:

- `main.tex` — a self-contained article, *A Formal Account of Petri-Net
  Model-Based Testing with an Off-Target-Aware Ant Colony*.  It defines the
  net `N = (P, T, F, I, w, β, ℓ, κ)` with weighted ordinary and inhibitor
  arcs, place bounds, typed and labelled transitions, several named initial
  markings, target and error markings, milestones, rare transitions and crash
  predictions; the firing relation with bound violations excluded from the
  graph; the full and lazily explored reachability graph; coverage,
  saturation and novelty; target phases and board classes; the off-target
  region as the complement of the backward-reachable set of its seeds, with a
  proof that it is reward-inaccessible and closed under successors; the three
  edge channels and the choice weight
  `(a + a⁺ + 0.1)^α · ν^β · exp(−λ(r + g + r⁺))`; deposits,
  back-propagation, evaporation and the matured-target exception; the
  T-suffix recurrence `H_k(m) = mix(H_{k−1}(m−1), s_m)`; the suggester's
  effective score; a table of defaults and a symbol-to-code table.
- `references.bib` — Dijkstra (1959), Dorigo–Maniezzo–Colorni (1996),
  Dorigo–Stützle (2004), Murata (1989), Utting–Legeard (2007) and
  Utting–Pretschner–Legeard (2012).
- `README.md` — the build command and a short table of contents.
- `.gitignore` — the generated auxiliaries and `main.pdf`.

The article is a definition-and-correspondence paper, not an empirical
evaluation.  Section 6 says explicitly that the code is the ground truth if a
definition ever drifts from it.

### Build

Verified with TeX Live 2025: `pdflatex main.tex`, `bibtex main`, then
`pdflatex main.tex` twice.  The result is 16 pages, all citations and
cross-references resolve, and the only remaining box warnings are underfull
table cells.  The generated PDF is ignored; the sources are the deliverable.
No C++ code changed in this step, so the suite and the version are untouched
(`v0.0.6`).
