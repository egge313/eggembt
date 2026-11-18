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
- The command-line runner (first half of open item 2) is implemented and tested: `mbt run`, `check`, `explore`, `report` and `agent-sim`, with `--json` reports, run-log continuation and agents over standard streams, child processes or TCP.
- Everything compiles and passes: 12 unit test suites plus `cli_test.sh` (22 scenarios) — 13 CTest entries, all green, now built with `-Wall -Wextra` as §10 promises. §10's "not yet verified" no longer applies.

### Changes made this session

- **Stdio exit deadlock fixed.** `StreamTransport`'s reader blocks in `std::getline` over `std::cin`, holding stdin's C stdio lock; `exit()` then waits for that lock forever while flushing the streams, so `mbt run --stdio` and `mbt agent-sim` hung instead of exiting. On POSIX both now use the poll-based, stoppable `FdTransport(0, 1, false)`; `StreamTransport` remains for non-POSIX builds (two sites in `src/cli/Commands.cpp`, marked).
- **`--no-ants`.** The ant colony can be switched off (`SessionConfig::useAnts`, surfaced as `Session::antsEnabled()`). The report says which guidance ran: a `Guidance` block on the console and `ants` / `ant_iterations` / `ant_walks` in the `--json` output, always emitted so the schema does not depend on the switch. Tested in `tests/tst_session.cpp` (`theAntColonyCanBeSwitchedOff`) and `cli_test.sh` (`no-ants`).
- **The remaining guidance switches are surfaced in the report.** `--deadlock-policy`, `--livelock-policy`, `--dump-on-deviation`, `--dump-on-deadlock`, `--dump-on-livelock` and `--threshold` were accepted but invisible afterwards. The `--json` report now always carries `deadlock_policy`, `livelock_policy`, `dump_on_deviation`, `dump_on_deadlock`, `dump_on_livelock` and `coverage_threshold`; the console `Guidance` block lists the same values (`targets: deadlock …, livelock …; coverage threshold N`, `post-mortems: on deviation …, on deadlock …, on livelock …`); `Session::config()` exposes the effective configuration; `toString(TargetPolicy)` in `src/model/Net.h` fixes the one spelling (`until_covered` / `once` / `avoid`). Tested in `cli_test.sh` (`guidance-in-report`).
- **`docs/PLUGIN_PROTOCOL.md` written.** The dangling reference in §8 (and in `src/engine/Protocol.h` / `AgentServer.h`) now resolves: the full wire specification with field tables, an example run, the checklist for agent authors, and a map of the code that implements it.
- **`--seed N`: reproducible campaigns with steered tie-breaks.** Until now the input choice was fully deterministic — among equally scored inputs the more robust one won, then the lower transition index — so every campaign over the same model and settings picked exactly the same inputs; the only seeds were the ant colony (`--ant-seed`) and the simulated SUT (`--sim-seed`). A non-zero `--seed` now randomises the *last resort only*: when several inputs are exactly tied (same effective score and same robustness), the pick is drawn from a seeded `std::mt19937_64` stream (`SessionConfig::selectionSeed` → `SuggesterConfig::tieBreakSeed`; seed 0 keeps the old behaviour byte for byte). One campaign stays reproducible from its seed, different seeds steer different campaigns, and the quality ranking is untouched. The `--json` report names both seeds (`seed`, `ant_seed`) so a campaign can be reproduced from the report alone. The SUT's own indeterminacy is unaffected — a real SUT may still answer differently, which is fine and interesting. Tested in `tests/tst_suggest.cpp` (`seededTieBreaksVaryAndReproduce`), `tests/tst_session.cpp` (`theSeedSteersAndReproducesCampaigns`) and `cli_test.sh` (`seed`, byte-compare on the deterministic counter model).
- **Testing time as a second criterion next to coverage.** Coverage alone does not say what a campaign cost. Every run now measures its wall-clock duration (`RunReport::durationMs`), the campaign its total (`CampaignReport::wallMs`); the `--json` report gains per-run `duration_ms` and a `time` section (`wall_ms`, `mean_run_ms`, `events_per_s` — how many SUT events a second of testing bought), and the console output a `Time` block. This is the number that tells seeds and configurations apart when their coverage is equal: the same coverage in half the time is the better campaign. Wall-clock numbers are, by nature, not reproducible — the `seed` scenario's byte-compare strips the time fields before comparing, because the reproducibility claim is about the *selection*, not about timing. Tested in `tests/tst_session.cpp` (`theCampaignMeasuresItsTime`) and `cli_test.sh` (`time-and-overlap`).
- **Coverage overlap: how much do two test runs have in common?** New `src/engine/OverlapAnalysis.{h,cpp}`: the Jaccard index |A∩B|/|A∪B| of the covered element sets, computed per run, per campaign and pairwise, separately for transitions, states and edges. For exact per-run attribution `CoverageDb` now records, alongside `steps` and `events`, every state a run definitely visited (including those credited without a known edge in ambiguous stretches) and every transition that certainly fired (including the ambiguous-layer credits); a replayed run reconstructs the same sets, so campaigns can be compared after the fact from their run logs. The campaign `--json` report gained an `overlap` section (full pairwise matrices and `mean_pairwise` means; low means mean the steering spread the testing out, 100% would mean every run repeated the same coverage), the console a one-line `Overlap` summary. New command **`mbt compare MODEL LOG_A LOG_B [--csv FILE] [--json FILE]`**: replays both run logs and reports the campaigns' mutual overlap, the diversity within each campaign, and the run-against-run matrix (printed as a percent grid while it fits the terminal; `--csv` writes every pair as `group,i,j,transitions,states,edges` rows for plots). Tested in `tests/tst_overlap.cpp` (new suite: set attribution, the index, matrix symmetry, empty runs count as identical) and `cli_test.sh` (`compare`: a log against itself is 100%, CSV/JSON output).
- **The `"traced"` key for internal transitions.** Internal steps were tagged only implicitly: an internal transition *with* a label was reported like an output ("traced"), one *without* was silent. That had a footgun — adding a label purely for display or the run log silently flipped the step from silent to reported. The loader now accepts an explicit `"traced": true|false` on internal transitions (default unchanged: labeled ⇒ traced, unlabeled ⇒ silent). It is an error elsewhere: `traced: true` without a label has nothing to report, and on controllable/observable transitions the visibility is part of the testing protocol, not a per-model choice. Engine-wide the decision now lives in `Transition::traced` (derived in `NetBuilder::build()`, settable via `NetBuilder::traced()` for hand-built models), used by the belief tracker (label maps, `expectedLabels`, the silent-step crediting), the simulated SUT and the `check` kind counts — so a *named but silent* internal step works everywhere. Tested in `tests/tst_jsonnetloader.cpp` (`tracedKeyDefaultsAndOverrides`, `tracedKeyIsRejectedWhereItDoesNotApply`), `tests/tst_session.cpp` (`aNamedInternalStepIsReportedOnlyWhenTraced`) and `cli_test.sh` (`check-reports-every-model-error`).
- **Truncated reachability graphs tested (state limit).** The question "how does the tester react when `--max-states` cuts the graph off, and does it expand unresolved nodes?" is now pinned by tests (`tests/tst_session.cpp`, marked): the graph is lazy and the limit stops the *creation* of states, not their expansion — during runs the tracker and the suggester expand every state they stand on (plus up to `expansionBudget`, 500, states ahead), so the graph grows on demand until the cap; at the cap, edges into new markings are dropped and the source is marked incomplete. A cut-off frontier is never mistaken for a deadlock or a livelock (deadlock is judged on the marking, not on edges; the static analysis only reports *certain* findings when no involved state is incomplete), the session warns "the model was not explored completely", and when the SUT walks past the region the graph can represent, the run ends in a deviation whose message says "[the explored model may be incomplete here]". Tests: `aStateLimitTruncatesTheGraphWithoutFalseFindings`, `theGraphGrowsLazilyDuringTheCampaign`, `anSutBeyondTheTruncatedModelIsFlaggedAsPossiblyIncomplete`; end to end, `cli_test.sh` scenario `state-limit` (`mbt explore --max-states` and `mbt run --max-states` over a counting model).
- **Missing pieces reconstructed** during assembly: `tests/ElevatorModel.h` (the shared elevator model), `README.md`, `.gitignore`, and the Qt-free `CMakeLists.txt` (`mbt_engine`, `mbt_core`, `mbt_agent`, the `mbt` application, the tests and the `cli_test.sh` wiring).
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

**The mbt conventions.** A transition's `<toolspecific tool="mbt" kind="controllable|observable|internal" label="...">` sets kind and SUT label (other tools' toolspecifics are ignored silently). A milestone is tagged as a *second* toolspecific so it composes with the kind in any order: `<toolspecific tool="mbt" kind="milestone" rank="2"/>` (rank ≥ 1, default 1). Without any toolspecific, the transition id's first byte decides as a fallback: `?` → controllable, `!` → observable, anything else → internal **silent** (the id is never used as a label by accident). A controllable or observable transition without a label falls back to its id, stripped of a prefix. Per-place `<initialMarking>`s are **unioned into one initial marking** named after the net; `<capacity>` and place toolspecifics draw warnings, because PNML here yields **no bounds and no target or error markings** — declare those in the JSON form if needed.

The CLI recognises the format by the first non-space byte of the file (`<` means XML, i.e. PNML; anything else goes to the JSON loader as before), `mbt check --help` says so, and a pre-existing duplicated `--strict` help line was removed while touching that text. `examples/coffee-pnml.pnml` is the shipped example: five transitions with the conventions above, a rank-2 milestone, an inhibitor arc, the `?`-prefix fallback — and a `switch_off` transition that consumes the brewing state and the lamp token, because with no target markings to declare, a net that is one big cycle would have every move vetoed as a livelock of the specification.

Tested in `tests/tst_pnml.cpp` (new suite, 21 tests: XML noise, conventions, fallbacks, inhibitor arcs and weights, the marking union, every reported problem, the shipped example, and a session campaign driven by an imported model) and `cli_test.sh` (`pnml-model`: `check` names the model and its 5 transitions, `--strict` passes, `run --simulate` reaches `coverage complete` with `transitions 5 of 5` and `milestone transitions 1`).

### Item 7: the reference agent shim

> 7. A reference agent shim for C++ SUTs.

`examples/agents/ref_agent.cpp` shows the whole path for a hand-written C++ SUT: a plain `CoffeeMachine` state machine (it knows nothing about Petri nets or the protocol), an `ISutPlugin` wrapper that drives it (`RefSutPlugin`), and `AgentServer` over `FdTransport`/stdio as the agent `mbt-ref-agent`. Its header comment doubles as the checklist for wrapper authors: map each named initial marking of the model to a real SUT state (`startRun`), acknowledge exactly the inputs handed over (`stimulate` → `Stimulated`), report every model-relevant SUT move in capture order with millisecond timestamps (`Observed`), push **one** `Quiescent` per quiet stretch from a clock-bearing thread, be honest in `capabilities()` about what `postMortem` can do, and stop pushing events after `stopRun`. The model it serves is `examples/agents/coffee.json`; CMake builds it as the `ref-agent` target and hands it to `cli_test.sh` as `REF_AGENT`, whose `reference-agent` scenario runs `mbt run examples/agents/coffee.json --agent "$REF_AGENT" --quiescence 40` to coverage complete (skipped when the variable is unset, for manual runs).

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

The GUI half of item 2 was this batch's open question — a GUI in a Qt-free, zero-dependency tree — and the decision was Egge's: *"Scratch the idea of a GUI. A textual report is fine."* So the view the item asks for — coverage **and backtraces** — ships as text: `mbt report` keeps its coverage sections and gains a **Backtraces** section.

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
- **The report.** After the run lines, a `Crash predictions` block names the credibility level and the implicit budget and lists every entry (site, prediction word — with `p=`/`attempts=` when explicit —, status, attempts, crashes, dumps). The campaign `--json` always carries `crash_level`, `crash_attempts` and `crash_predictions` (key, prediction, status, attempts, crashes, dumps) so the schema does not depend on the model. `mbt report` prints the same block from a run log.
- **The CLI**: `--crash-level L` (0.5..1.0, default 0.95) and `--crash-attempts N` (1..1000000, default 6) — the two knobs of the experiment; `--sim-crash-after N` (0 = never) on the simulator.

### `--credits`

Between the crash design and the hooks, Egge asked for a credits command:

> Feature: Add a new command line parameter --credits. It should tell the following: "Egge Model Based Tester\n Designer: Esa Kettunen\n Copyright (c) Esa Kettunen 2026\n Software written by artificial intelligence: Claude Sonnet 5.5 Medium and GLM-5.3-Flash\n"

followed by:

> Indent the --credit lines with 4 spaces instead of one. Show GLM credits on it own line, starting with "    Software written by artificial intelligence:"

`mbt credits` and `mbt --credits` both print, byte for byte:

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

An **asynchronous** command is fire-and-forget: mbt does not wait for it and never kills it; its output goes to /dev/null and its record says `launched` with no exit. At session destruction the children that already ended are reaped (no zombie lingers in the process table); the ones still running are orphaned to init, which reaps them.

The record's status words: `ok` (exit 0), `failed` (a non-zero exit; or killed by a signal of its own; or the fork/pipe failed before any child existed), `timeout` (killed at the bound), `launched` (async, no exit known — the record's exit holds −1, `kHookNoExit`). Every failure is a note and nothing else — Egge's "Note only, run continues" (the notes appear under the run line in the console; the hook records themselves survive a restore, the notes of a live run do not round-trip):

    the command of '<id>' failed (exit N): the run continues
    the command of '<id>' failed: the run continues
    the command of '<id>' timed out after N s: the run continues
    the command of '<id>' could not be launched: <strerror>

### The environment of a command

The command line runs through `sh -c` in mbt's working directory, with the inherited environment plus four variables — the model's data is never interpolated into the command line itself:

| variable | value |
|----------|-------|
| `MBT_TRANSITION` | the transition's id |
| `MBT_TRANSITION_LABEL` | its label (may be empty) |
| `MBT_RUN` | the run's id within the campaign (0 for the first run) |
| `MBT_LOG` | the open run log's path (`--log`); absent when there is none |

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
- `tests/tst_session.cpp` (+8, now 49): `commandHooksRunSyncAndReportFailures` (stdout and stderr captured, exit 3 is a `failed` record plus the byte-exact note, the run completes), `commandHookBudgetLimitsExecutions` (a self-loop under a 12-event cap: exactly two executions in run 1, none in run 2 — the budget is the campaign's), `commandHookGuardReadsTheBeliefAfterTheStep` (two guarded hooks share one command that appends its own transition id; only the hook whose guard holds after its step ran), `anAsyncCommandHookIsLaunchedNotWaited` (`launched`, no exit, the child still writes its marker after the campaign returned), `aTimedOutSyncHookIsKilledAndNoted` (`--hook-timeout 1` against `sleep 30`: `timeout`, the note, the run completes), `aRuledOutSuspectCommandIsReported` (an ambiguous stretch resolved against the hooked transition: no execution, the ruled-out note), `restoredRunsAdoptHookRecordsWithoutReexecuting` (the marker file gains exactly one line live; the restore re-adopts the record without a second line), `commandHooksReceiveTheirContext` (the four environment variables, `MBT_LOG` only when a log is open).
- `tests/tst_runlogio.cpp` (+1, now 8): `hookRunsRoundTrip` (three records — `ok` with captured output, `timeout` at exit −1, `launched` async — round-trip through the log unchanged).
- `cli_test.sh` (+1 scenario, now 33): `command-hooks` (a sync `printf` hook's output in the table, an async `true` hook as `launched`, the Guidance line `commands: 2 transition(s), timeout 60 s`, the json keys, `--hook-timeout 0` rejected with the bounds in the message, `--hook-timeout 2` reflected in the Guidance line, and `mbt report` over the log showing the same table).

The suite stands at **16 CTest entries** — 15 unit suites (`tst_session` 49 tests, `tst_jsonnetloader` 38, `tst_runlogio` 8 with the hook-records round-trip) plus `cli_test.sh` (33 scenarios) — all green. Build and check with:

    cmake -S . -B build -G Ninja && cmake --build build && ctest --test-dir build
