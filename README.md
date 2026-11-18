# Eggetest — a model-based tester

`mbt` tests a system against a **Petri-net specification** of its behaviour.
The model says what inputs may be sent, what outputs must come back, and what
constitutes an error; the tool then drives the system (or a simulation of it)
through test campaigns, watching every event against the model.

```
mbt check    MODEL           check a model for errors: deadlocks, livelocks, ...
mbt explore  MODEL           explore the reachability graph (--dot for Graphviz)
mbt run      MODEL --simulate | --agent "CMD" | --connect H:P | --listen | --stdio
mbt report   MODEL --log RUNS.jsonl
mbt agent-sim MODEL           the built-in simulated SUT, speaking the plug-in protocol
```

A campaign runs repeatedly until the model's transitions, edges, states and
places are covered and every target (error markings, deadlocks, livelocks) is
satisfied or deliberately avoided. Events that the model does not allow are
deviations; a system gone silent at the wrong moment is a quiescence failure;
cycles that make no progress are livelocks, found statically and at run time.
Every run is recorded as JSON lines and can be replayed into a later session,
so coverage continues where a previous session stopped.

## Layout

    src/model    the Petri-net model and its fingerprint
    src/engine   semantics, reachability graph, coverage database, belief
                 tracker (what the SUT's state can still be), target phases,
                 suggester, ant colony, event queue, session, plug-in
                 protocol, remote plug-in, run log — plain C++17
    src/io       the JSON model loader, the JSON-lines run log, and the
                 line transports (standard streams, POSIX pipes, child
                 processes, TCP)
    src/cli      the `mbt` command line tool (main.cpp sits next to it)
    tests        unit tests of every layer, plus shared test helpers
                 (ElevatorModel.h is the model most tests use)
    examples     sample models: elevator-door, drink-machine,
                 heartbeat-livelock, atm (added: GLM-5.3-Flash)
    cli_test.sh  end-to-end tests of the tool: real processes, pipes, sockets

## Build and test

Requires CMake ≥ 3.16 and a C++17 compiler. No third-party dependencies.

    cmake -S . -B build -G Ninja
    cmake --build build
    ctest --test-dir build

The 13 tests are 12 unit test suites (engine, coverage, belief tracking,
livelock analysis, suggester, ant colony, async event queue, session, remote
plug-in, transports, model loader, run log) plus `cli_test.sh`, which runs the
`mbt` binary end to end against the examples.

## Model format and plug-in protocol

Models are JSON; see `examples/elevator-door.json` for places, transitions
(`controllable` / `observable` / `internal`), arcs (including inhibitor arcs)
and the initial, target and error markings. A system under test takes part over
the plug-in protocol — one JSON object per line, exchanged over standard
streams, a pipe or TCP (`--stdio`, `--agent`, `--connect`, `--listen`); the
`mbt agent-sim` shim turns any plug-in into an agent, and the built-in
simulated SUT behaves exactly as its model says.

---

## Changes

### GLM-5.3-Flash — switch the ant colony off

The ant-colony guidance can be switched off per campaign:

    mbt run MODEL --simulate --no-ants

- `--no-ants` on the `mbt run` command line (engine counterpart:
  `SessionConfig::useAnts = false`). The colony then neither walks the model
  nor leaves pheromone; the suggester ranks inputs on its own
  (targets, new transitions, uncovered edges, frontier, robustness, hazard).
- The campaign report says which guidance ran: a `Guidance` line in the
  console output, and `ants` / `ant_iterations` / `ant_walks` fields in the
  `--json` report. `Session::antsEnabled()` exposes the setting to the API.
- Tested in `tests/tst_session.cpp`
  (`theAntColonyCanBeSwitchedOff`) and `cli_test.sh` (`no-ants`).

### GLM-5.3-Flash — report every guidance switch; plug-in protocol documented

- The campaign report now shows the whole steering configuration, not just the
  ant counters: `deadlock_policy`, `livelock_policy`, `dump_on_deviation`,
  `dump_on_deadlock`, `dump_on_livelock` and `coverage_threshold` in the
  `--json` output (always emitted, defaults included), and the same values in
  the console `Guidance` block. The engine side gained
  `Session::config()` and a canonical `toString(TargetPolicy)` in
  `src/model/Net.h`. Tested in `cli_test.sh` (`guidance-in-report`).
- `docs/PLUGIN_PROTOCOL.md` written: the full wire protocol between `mbt run`
  and an agent — field tables for all ten message types, an example run, a
  checklist for agent authors, and where the code lives. Closes the reference
  in §8 of `doku.md` and in `Protocol.h` / `AgentServer.h`.
- Build hardening: warnings enabled (`-Wall -Wextra`) as the design doc
  promises; the tree compiles clean with them.

### GLM-5.3-Flash — tests for truncated reachability graphs

New tests pin what the tester does when `--max-states` cuts the reachability
graph off (the limit stops *creating* states, not expanding them):

- `tests/tst_session.cpp`: `aStateLimitTruncatesTheGraphWithoutFalseFindings`
  (truncated chain, the "not explored completely" warning, no false livelock,
  the campaign still completes coverage of what it can reach),
  `theGraphGrowsLazilyDuringTheCampaign` (nothing explored up front: the runs
  grow the graph on demand from 0 states to the cap),
  `anSutBeyondTheTruncatedModelIsFlaggedAsPossiblyIncomplete` (a scripted SUT
  walks past the truncated region: the run ends in a deviation whose message
  says the explored model may be incomplete there — a dropped-edge stimulus is
  only Preempted, and the frontier is never called a deadlock).
- `cli_test.sh` scenario `state-limit`: `mbt explore --max-states 4` reports
  "4 states, 3 edges" with the "(incomplete: state limit or place bounds)"
  marker, and `mbt run --max-states 4` warns and still finishes with
  "coverage complete".

### GLM-5.3-Flash — seeded campaigns and the `traced` key

**`--seed N` for `mbt run`** (new, default 0): the input choice had one
arbitrary last resort — among exactly tied inputs the lowest transition index
won — which made every campaign over the same model pick the same inputs. A
non-zero seed breaks those ties at random, reproducibly:

- the same seed reproduces the same campaign; different seeds steer different
  campaigns (the quality ranking — score, robustness, hazard, pheromones — is
  untouched; only the arbitrary last resort is randomised);
- seed 0 keeps the old, fully deterministic behaviour byte for byte;
- the `--json` report records `seed` and `ant_seed` (the ant colony keeps its
  own `--ant-seed`; the simulated SUT its own `--sim-seed`), so a campaign can
  be reproduced from its report;
- the SUT's own indeterminacy is unaffected: a real SUT may still answer
  differently run to run, which is exactly what the tester wants to see.
- Tested in `tests/tst_suggest.cpp` (`seededTieBreaksVaryAndReproduce`),
  `tests/tst_session.cpp` (`theSeedSteersAndReproducesCampaigns`) and
  `cli_test.sh` (`seed`, byte-compare of two `--seed 7` reports over the
  deterministic counter model).

**The `"traced"` key for internal transitions** (new, optional): an internal
step is reported like an output ("traced") when it carries a label, and silent
when it does not — but the label did double duty, so adding one for display
silently flipped the step from silent to reported. Internal transitions now
accept an explicit `"traced": true|false`:

    {"id": "step", "kind": "internal", "label": "work", "traced": false}

- default unchanged (labeled ⇒ traced, unlabeled ⇒ silent); `"traced": false`
  makes a *named but silent* step — the label stays available for display and
  the run log without changing what the tester sees;
- it is an error elsewhere: `traced: true` without a label has nothing to
  report, and on controllable/observable transitions visibility is part of the
  testing protocol, not a per-model choice;
- engine-wide the decision now lives in `Transition::traced` (derived by
  `NetBuilder::build()`, overridable per transition via `NetBuilder::traced()`),
  used by the belief tracker, the simulated SUT and `mbt check`'s kind counts.
- Tested in `tests/tst_jsonnetloader.cpp`
  (`tracedKeyDefaultsAndOverrides`, `tracedKeyIsRejectedWhereItDoesNotApply`),
  `tests/tst_session.cpp` (`aNamedInternalStepIsReportedOnlyWhenTraced`) and
  `cli_test.sh` (negative cases in `check-reports-every-model-error`).

### GLM-5.3-Flash — testing time and coverage overlap

Two more criteria for judging a campaign, next to coverage:

**Testing time.** Every run measures its wall-clock duration, the campaign its
total:

- per run, `duration_ms` in the `--json` report; the console shows a `Time`
  block: `campaign 0.20 s, mean run 68 ms (longest 73 ms), 195.1 events/s`;
- the campaign report carries a `time` section: `wall_ms`, `mean_run_ms` and
  `events_per_s` — how many SUT events a second of testing bought. This is
  the number that tells seeds and configurations apart when their coverage is
  equal: the same coverage in half the time is the better campaign;
- wall-clock numbers are not reproducible by nature; the `seed` scenario's
  byte-compare strips them before comparing (the reproducibility claim is
  about the selection, not the timing).
- Tested in `tests/tst_session.cpp` (`theCampaignMeasuresItsTime`) and
  `cli_test.sh` (`time-and-overlap`).

**Coverage overlap.** How much do two test runs (or two whole campaigns) have
in common? `src/engine/OverlapAnalysis.{h,cpp}` computes the Jaccard index
|A∩B|/|A∪B| of the covered sets — per run, per campaign and pairwise, for
transitions, states and edges:

- `CoverageDb` attributes every covered element to its run (states visited,
  transitions certainly fired, including the ambiguous-stretch credits), and a
  replayed run reconstructs exactly the same sets — so campaigns can be
  compared after the fact from their run logs;
- the campaign `--json` report gained an `overlap` section: the full pairwise
  matrices and `mean_pairwise` means. Low means say the steering spread the
  testing out; 100% would say every run repeated the same coverage. The
  console shows the one-line `Overlap` summary;
- new command **`mbt compare MODEL LOG_A LOG_B [--csv FILE] [--json FILE]`**:
  the campaigns' mutual overlap, the diversity within each campaign, and the
  run-against-run matrix (percent grid, while it fits the terminal).
  `--csv` writes every pair as `group,i,j,transitions,states,edges` rows for
  plots; `--json` the whole analysis. Run a campaign twice — once with
  `--seed 1`, once with `--seed 2` — and compare the logs;
- tested in `tests/tst_overlap.cpp` (new suite) and `cli_test.sh`
  (`compare`: a log against itself is 100%).

### GLM-5.3-Flash — the ants defend the campaign after a catastrophe

When the SUT crashes, finding the root cause takes wall-clock time; the
campaign should spend that time testing somewhere else, not re-finding the
crash. Two scenario campaigns in `tests/tst_session.cpp` pin the mechanism —
the Session tells the colony at the end of a run that really suffered a
catastrophe, via the colony's back-propagating deposit hook for "callers with
their own knowledge":

- **A crash** (`afterACrashTheAntsKeepTheCampaignProductive`): a region model
  with a one-input way into a `boom` error marking that demands two visits and
  may crash the SUT for a post-mortem each time, next to two safe corridors.
  With the colony, run 1 finds the crash (one input, one dump, the way in is
  marked repellent), run 2 — the research window — covers both corridors and
  only then delivers the second, policy-mandated hit. Without the colony, run
  2 walks straight back into the crash region: no new coverage, dump #2 of the
  same bug before anything else was tested.
- **A deadlock** (`afterADeadlockCrashTheAntsDoTheSame`): a true sink deadlock
  never produces the run-time Deadlock outcome — the suggester runs out of
  work while the SUT hangs, so the run ends *stranded* (`rep.stuck`) and no
  post-mortem is taken; the visit itself credits the deadlock target. A
  stranded run teaches the colony the same lesson ("the way into the deadlock
  is repellent"), and the colony's campaign finishes in 2 runs where the
  colony-less one needs 3 (its run 2 is a second hang of the same kind).

The scenario is tuned so the catastrophe sits exactly one input deep: once
covered-and-still-sought it *ties* with the fresh corridor edges, and only the
pheromone decides — the scenario sets `pheromoneRepelWeight = 12`, inside the
window where fresh corridors beat the marked way in, yet the catastrophe wins
again as soon as the corridors are covered.

### GLM-5.3-Flash — bad pheromones priced by the way in

An error state — deadlock, livelock, SUT crash — deserves worse pheromones the
more time it takes to reach it. A repeat visit pays the way in again, and for
a given SUT the wall-clock time of that way grows with the number of inputs on
it: the quantity a run can measure reproducibly (the run's wall-clock duration
itself is not reproducible, so it must not steer). The Session's real-run
catastrophe deposits now use `AntConfig::catastropheDeposit(stimuli)`:

    depositCatastrophe * (1 + catastropheTimeWeight * min(stimuli, catastropheTimeCap))

- `catastropheTimeWeight = 0.25`: a quarter of the base deposit per input the
  run spent getting there; a crash one input in deposits 3.75, four inputs in
  6.0, from eight inputs on 9.0;
- `catastropheTimeCap = 8`: inputs counted at most — however long the run was,
  one catastrophe cannot out-shout everything else (`tauMax` stays the
  absolute ceiling);
- `catastropheTimeWeight = 0` restores the old flat deposit;
- the colony's own deposits — ants that die in a Forbidden state — stay flat:
  an ant's walk through the model costs the campaign nothing.

Tested in `tests/tst_ants.cpp` (`catastropheDepositGrowsWithTheWayIn`) and
`tests/tst_session.cpp` (`theLongerTheWayInTheWorseTheCatastrophePheromone`:
one crashing run each on chains of 1, 4, 10 and 12 inputs — the strongest
repel left behind grows 3.75 → 6.0 → 9.0 and then stays). The suite stands at
14 CTest entries — 13 unit suites plus `cli_test.sh` — all green.

### GLM-5.3-Flash — good target states, error transitions, and the wall-clock criterion under two switches

**Target states, two ways.** Target markings were always part of the model
format (they are the idle states the livelock analysis needs), but reaching
one never ended anything. A target marking can now be declared *intentional* —
a "good target state":

    "target":[{"name":"reached","tokens":{"s1":1},"intentional":true}]

Such a marking is a normal target: the guidance steers into it like into any
other sought goal. But the moment the tester knows the SUT is *certainly* in
it, the run — and the whole campaign — end: that marking is what the campaign
came for. The report says so per run (`good_target`) and for the campaign
(`stop_reason: the good target marking 'reached' was reached`). Without the
tag, a target marking keeps its old meaning: an idle state, never an end.

**Error transitions.** Error *places* could be tagged from the start; now
*transitions* can be too, with their own post-mortem settings:

    {"id":"c1","kind":"controllable","label":"c1","error":true,
     "crash_dump":"crash","max_dumps":1}

A step of an error transition is reacted to only when it is *certain* — every
possible state of the belief advanced by the very same transition, nothing
lost to the exploration limits; an ambiguous step stays ordinary coverage.
The reaction is the one error markings get: recorded in `errors_reached`, its
own `max_dumps` budget, `SutCrashed` on a `crash` request, the run continues
on `none`. The loader rejects the tag on steps the tester cannot see
(untraced internal transitions) and warns that `policy`/`target_hits` have no
effect on transitions yet (steering stays ordinary transition coverage).

**The wall-clock bad pheromones under two switches.** The time-priced
catastrophe pheromones reason "deeper error ⇒ longer way in ⇒ worse repel" in
wall-clock terms. Two things can void that reasoning:

- `--flat-catastrophe` switches the feature off entirely: every real-run
  deposit stays at the base level, however deep the crash was
  (`catastrophe_time_pricing: false` in the report, console line
  `catastrophes: flat (--flat-catastrophe)`);
- `"unpredictable": true` tags a transition as temporally unpredictable — a
  human operator drives it, so the wall clock measured the human, not the
  model. A run whose way in crossed such a step keeps its deposit flat even
  with the feature on. The console Guidance line also counts the tagged
  transitions ("unpredictable transitions N").

Tested in `tests/tst_jsonnetloader.cpp` (`intentionalTargetMarkingsAreParsed`,
`transitionTagsAreParsed`, `ineffectiveTransitionSettingsWarn`,
`errorTransitionMustBeVisible`), `tests/tst_session.cpp`
(`aGoodTargetMarkingEndsTheCampaign`,
`anErrorTransitionIsCaughtLikeAnErrorMarking`,
`anErrorTransitionWithoutADumpLetsTheRunContinue`,
`theCatastrophePricingCanBeSwitchedOff`, `anUnpredictableStepKeepsTheDepositFlat`)
and `cli_test.sh` (`good-target`, `guidance-in-report`). The suite stands at 14
CTest entries — 13 unit suites plus `cli_test.sh` (26 scenarios) — all green.

### GLM-5.3-Flash — internal transitions without a counterpart in the SUT

An *unlabeled* internal transition is the "no counterpart" case: it only
furthers the specification from one external behaviour to the next, and the
SUT has no action for it. The tooling already treated it that way everywhere:

- the tester can never stimulate it (only controllable transitions are
  inputs) and never waits for a report of it (only observable and
  traced-internal steps appear in `expectedLabels`);
- the belief tracker follows it as a silent step: the belief set is closed
  under it after every event, and `creditSegment` credits a silent edge
  definitely when every consistent path crosses it (a silent detour that may
  or may not have happened stays ambiguous, a ruled-out one is not credited);
- the simulated SUT takes such moves by itself between external ones and
  pushes no event for them.

That behaviour was pinned at unit level in `tests/tst_belief.cpp`, but there
was no end-to-end session test for the bridge case. Two new tests in
`tests/tst_session.cpp` over `s0 -a-> s1 -advance-> s2 -oa-> s0`, where the
unlabeled internal step is the *only* connection between the input and the
output:

- `anInternalStepWithoutACounterpartIsFollowedAndCovered`: the run records
  exactly two events (`Stimulated a`, `Observed oa`) and never anything for
  the internal step, the SUT was never asked for it, and yet the silent edge
  is definitely covered (`transitionHits == 1`) — the campaign completes in
  one run with all three transitions covered.
- `anInternalOnlyStallIsAMissingOutput`: a SUT that never takes the step and
  goes silent is a *missing output* — the run ends `Deviation` ("no output
  arrived … (expected: oa)"), and the silent stretch stays ambiguous and
  uncredited, because the tester could not know whether it fired.

This complements `aNamedInternalStepIsReportedOnlyWhenTraced` (the other
internal case: a *named* step whose visibility the model chooses with
`"traced"`). Still 14 CTest suites, all green.

### GLM-5.3-Flash — the ATM example

`examples/atm.json` is a worked example that exercises the whole recent feature
set on one coherent model — the classic ATM case: insert a card, enter a PIN
(three invalid PINs swallow the card), a stolen-card branch that confiscates,
withdraw cash subject to a funds check, several ATMs, a per-visit bank session
and its database connection, an acknowledgement per operation, and a failure
path for each of the three components (ATM, session, database).

- **Error transitions with crash dumps**: `session_fail` (`session.down`) and
  `db_fail` (`db.down`) compete with the cash answer at the funds window,
  `atm_fail` (`atm.down`) with the ejection after a withdrawal, `confiscate`
  (`card.confiscated`) with the normal readout after a traced
  `bank.stolen_report` announcement — each caught *certainly* (single-state
  belief, visible competitors only) and dumped once.
- **An error marking as a target**: the third invalid PIN takes
  `reject_last → card.kept` and the `card-swallowed` marking
  (`match: covers`, one hit, one crash dump) is hunted and satisfied.
- **A human in the loop**: `pin.enter` is `"unpredictable": true` — runs
  through it keep the catastrophe deposit flat.
- **Several ATMs, visibly**: three initial markings (lobby/drive-in/branch)
  differing in their cash cassette (2/1/0) — the round-robin rotation is
  visible in the run log, and the empty branch ATM can never complete a
  withdrawal.
- **A good target that means it**: `visit-done` needs `visit_done ≥ 1` *and*
  `cash_taken ≥ 1`; the plain card-return exit is a non-intentional target, so
  the campaign keeps testing until a real withdrawal happened, then stops with
  "the good target marking 'visit-done' was reached".
- **A deterministic story per sim-seed**: with `--sim-seed 5 --max-runs 45`,
  37 runs, 5 crash post-mortems, all four error transitions dumped, the
  swallowed card satisfied, the good target at run 37, the ATMs rotate
  13/12/12. (`SimulatedSut` reseeds per run with consecutive seeds, whose
  early draws skew slightly — everything is still found, just in ~37–58 runs
  instead of fewer; documented in `doku.md` §13.)

Try it:

    mbt check examples/atm.json
    mbt run examples/atm.json --simulate --sim-seed 5 --max-runs 45 --quiescence 40

Tested in `cli_test.sh` (scenario `atm-visit`). The suite stands at 14 CTest
entries — 13 unit suites plus `cli_test.sh` (27 scenarios) — all green.

### GLM-5.3-Flash — milestones, offered-vs-taken pheromones, PNML import, rebuild, reference agent

Five things in one batch — the **milestone label** plus §12 items 3, 4, 6 and
7 of the design doc (each documented with the item quoted verbatim in
`doku.md` §14):

**Milestones.** A transition can be tagged `"milestone": true` or, ranked,
`"milestone": 2` (`Transition::milestone` / `milestoneRank`,
`NetBuilder::milestone()`; the loader rejects the tag on untraced internal
transitions, same visibility rule as `"error"`). When the tester knows the SUT
*certainly* stepped one (`BeliefTracker::definiteMilestoneTransition()`, the
mirror of the error-transition rule), the session deposits positive pheromones
at every state the belief spans — `depositMilestone = 1.5` per rank, so a
rank-1 milestone sits just under the colony's target deposit (2.0) — once per
step, before the good-target check, and the run **continues**: a milestone is a
success on the way, not an end. The way in is not priced; `tauMax` caps.
Reported per run (`milestones_reached`), per model (`milestones` with
`id`/`label`/`rank`/`hits`) and on the console ("milestones: deposit 1.5 per
rank; milestone transitions N"). Tested in `tests/tst_session.cpp`
(`aMilestoneStepIsRewarded`).

**Offered versus taken** (§12 item 3). `CoverageDb` counts per label how often
each SUT move was offered (the session waiting for it) and taken (a model-known
observed step), `takeRate = taken/offered` clamped to 1.0; never-offered moves
rate 1.0, so silent internal steps stay neutral. Two feeds: the ants'
environment moves are weighted by the take rate of their label
(`AntConfig::environmentWeight`, wired by the `Session` constructor), and the
suggester's robustness estimate weights the environment moves it reasons about
the same way. Tested in `tests/tst_session.cpp`
(`theEnvironmentFeedCountsOffersAndTakes`, deterministic one-corridor timing)
and via the read-only `AntColony::config()`; the weighted robustness figure
itself is pinned in `tests/tst_suggest.cpp`
(`robustnessWeightsTheEnvironmentByTakeRate`).

**Pheromones rebuilt from the run log** (§12 item 4). `Session::restore` now
rebuilds what the live run taught the colony (`rebuildPheromones`): recorded
milestone steps deposit `milestoneDeposit(rank)` at their recorded targets
(no evaporation), and a recorded crash — or a run stranded in a recorded
deadlock — marks the way in repellent, priced by the recorded way in exactly
like the live deposits (flat under `--flat-catastrophe` or for an
`"unpredictable"` way in). The colony's own walk deposits are transient state,
not SUT knowledge, and are not rebuilt. Exact amounts pinned in
`tests/tst_session.cpp` (`theRebuildReplaysWhatTheRunTaught`: rank 2 → 3.0, a
one-input crash → 3.75, flat variants → 3.0).

**PNML import** (§12 item 6). `src/io/PnmlLoader.{h,cpp}` reads PNML
(Petri Net Markup Language) with a small dependency-free XML reader and the
JSON loader's issue reporting. The mbt conventions: a
`<toolspecific tool="mbt" kind="controllable|observable|internal"
label="...">` per transition, a milestone as a second toolspecific
(`kind="milestone" rank="N"`), the `?`/`!` id-prefix fallback for
controllable/observable, unmarked transitions internal and **silent**, and the
per-place initial markings unioned into one marking named after the net. No
bounds or target/error markings come out of PNML (warned about). `mbt check`
and `mbt run` accept PNML files (recognised by the leading `<`) next to JSON.
Shipped example `examples/coffee-pnml.pnml` — try
`mbt check examples/coffee-pnml.pnml` and
`mbt run examples/coffee-pnml.pnml --simulate`. Tested in the new
`tests/tst_pnml.cpp` (21 tests) and `cli_test.sh` (`pnml-model`).

**The reference agent shim** (§12 item 7). `examples/agents/ref_agent.cpp` is
a hand-written C++ SUT (a coffee machine) wrapped in `ISutPlugin` +
`AgentServer` over standard streams, served by `mbt run
examples/agents/coffee.json --agent <ref-agent>`; its header comment is the
checklist for writing your own wrapper (marking mapping, acknowledgements,
ordered events, quiescence, post-mortem honesty). Built as the `ref-agent`
target and exercised in `cli_test.sh` (`reference-agent`), which caught and
fixed a real wrapper bug: the SUT must land exactly where the model says the
reported step leaves it.

The suite stands at **15 CTest entries** — 14 unit suites plus `cli_test.sh`
(29 scenarios) — all green.

### GLM-5.3-Flash — backtraces in the report, the nontarget region

Two closures that end §12 (both documented with the item quoted verbatim in
`doku.md` §15):

**Backtraces in `mbt report`** (§12 item 2). The GUI half of the item was
scratched by decision — *"a textual report is fine"* — so the coverage-and-
backtraces view ships as text: `mbt report` prints, per definitely covered
transition, the chain of moves that led into its first definite visit, oldest
first (`start > coin.insert > button.coffee > coffee.served`). An ambiguous
stretch — the run certainly went on, but the exact edge is not certain —
renders as `...` with a legend; transition-level-only coverage says so instead
of guessing a chain. Tested in `cli_test.sh`
(`run-log-continues-and-reports`).

**The nontarget region** (§12 item 5). States from which no target marking is
reachable — a run that wanders in can never legitimately leave — now form a
further repellent region in the ant colony, alongside the catastrophes. The
colony recomputes it whenever the graph grows (reverse walk from target
markings, still-sought targets and unknown territory; everything else is the
region) and **sets** `nontargetRepel` (1.0, 0 = off) on every edge into it —
never accumulated, never evaporated, deliberately modest so uncovered
behaviour inside the region can still win. The colony's input weight and the
suggester's repel term both carry it; with the ants switched off it does
nothing. Two excuses keep it honest: states the board is still seeking (the
campaign goes to its targets on purpose — the existing crash-region campaign
caught the first version getting this wrong) and states that can still reach
unknown territory (a target marking might lie beyond the frontier). Tested in
`tests/tst_ants.cpp` (`statesWithoutATargetMarkingRepel`,
`theRegionSparesUnknownTerritory`, `theRegionSteersTheSuggester`).

With this batch §12 is closed. The suite stands at **15 CTest entries** —
14 unit suites (`tst_ants` now 16 tests) plus `cli_test.sh` (29 scenarios) —
all green.

### GLM-5.3-Flash — `target` instead of `final`, T-list pairs, and toned-down carry-over

Three things in one batch, documented in full in `doku.md` §16:

**The rename.** "Final marking" reads like net-theory's final state, but the
concept is the tester's *target* — so `final`/`nofinal` became
`target`/`nontarget` everywhere: identifiers (`targetMarkings`,
`StateKind::Target`, `goodTarget`, `nontargetRepel`, …), the model-JSON key
`$.markings.target` (files with the old `"final"` key load with a warning and
no target markings), the output ("the good target marking 'X' was reached",
"N target,"), the docs, and the test names. Kept on purpose: the help line
"only the final report", the two C++ `final` class specifiers in
`tests/tst_session.cpp`, and the foreign PNML `kind="final"` fixture.

**The T-list** (`--tlist N`, 0..8, default 0 = off). To each marking a list of
truncated traces — T-lists — of the recent trace history it was reached by;
the (marking, T-list) pairs become a finer pheromone basis beside the edge
channels. Each list element is a mini-hash of the trace's last k steps
(element k), built by one rolling pass per arriving step: element 1 depends
only on the last step, element N on the last N. Two arrivals at a marking
share exactly as many elements as their traces' last steps agree — so a
catastrophe suffered via one route no longer condemns the same marking reached
via another. Milestone and catastrophe deposits land in the pair store under
the arrival pair (flat, `tauMax`-capped, same amounts as the edge deposits);
ants and suggester weigh an edge under the pair its target would create; the
rebuild replays the recorded steps over its own cascade, so restored pair keys
match the live ones. Environment moves and `arrive()` are untouched.

**The carry-over** (`--restored-pheromones N`, 0..100, default 50). The run
log already saves every round's runs and merges their rebuilt teaching; now
that teaching enters toned down: every rebuilt deposit — milestones,
catastrophes, and the T-list pairs — is scaled by N/100. `100` keeps the old
full strength, `0` ignores the inherited teaching. The factor is applied per
deposit before `tauMax` caps and does not compound across rounds (the log
stores recordings, not pheromone state — every continuation re-derives the
knowledge at log strength times the factor).

The suite stands at **15 CTest entries** — 14 unit suites (`tst_ants` now
21 tests, `tst_session` 35) plus `cli_test.sh` (30 scenarios) — all green.

### GLM-5.3-Flash — crash predictions, and the `--credits` command

**Crash predictions** (documented in full in `doku.md` §17). A
*nondeterministic* SUT crash — random, rare, sometimes, often, but not always
— is a bug, so the campaign now treats a crash as a **target**: it reproduces
it, counting attempts (definite visits of the predicted place) and crashes
(attributed deaths), until the experiment is certain the crash is
deterministic — Egge's rule: "Credibility level on a binomial distribution."
(k = n and I_θ(a0+n, b0) ≤ 1−γ on a Beta prior, γ = `--crash-level` 0.95,
θ = 0.90) — or its give-up budget runs out (exhausted with crashes: a real
bug; without any: unobserved, teaching nothing). Predictions are declared in
the model with `"crash"` on a transition or a target/error marking — a class
word (`always`/`often`/`sometimes`/`rare`/`random`, prior means 0.98/0.80/0.50/0.10/0.50,
budgets 2/4/8/20/20), a bare number (shorthand for `{"p": …}`), or a full
object with `p`, `attempts`, `crash_dump`, `max_dumps`. While an entry seeks,
its place is a goal and the way in is marked *attractive*; once it resolves,
the catastrophe teaching takes over. A death nobody predicted gets an
implicit entry at the run's last credited step (the neutral prior, budget
`--crash-attempts` 6). The engine gained `EventKind::Died` (a plug-in reports
the SUT died spontaneously), the per-entry experiment state round-trips
through the run log (attribution + attempt trail) and rebuilds exactly on
restore; the report prints a `Crash predictions` table and the `--json`
carries `crash_level`/`crash_attempts`/`crash_predictions`. The simulated SUT
seeds predicted crashes on controllable transitions (`--sim-seed`
reproducible) and `--sim-crash-after N` dies wherever it is. Known gap,
documented: dumps taken for a prediction are not re-counted on restore (the
persisted dump record does not name its crash entry).

**`--credits`** (`mbt credits`, `mbt --credits`): prints the project's
credits, byte for byte as Egge specified (the one output that carries a
signature, by explicit request):

    Egge Model Based Tester
        Designer: Esa Kettunen
        Copyright (c) Esa Kettunen 2026
        Software written by artificial intelligence: Claude Sonnet 5.5 Medium
        Software written by artificial intelligence: GLM-5.3-Flash

The suite stands at **16 CTest entries** — 15 unit suites (new: `tst_crash`;
`tst_session` 41, `tst_jsonnetloader` 35) plus `cli_test.sh` (32 scenarios) —
all green.

### GLM-5.3-Flash — command hooks

**Command hooks** (documented in full in `doku.md` §18; user manual §9). A
transition can carry a shell command that runs when its step is certainly
fired — `"command": "collect.sh"`, or an object with `run`, `mode`
(`"sync"`/`"async"`), `times` (a whole-campaign budget, `1` = once) and
`when` (a guard place). Egge answered the five design questions: certain
steps only — "suspect steps which do not execute need to be reported" (a
hooked transition ruled out of a resolved stretch gets its note), always
with `times` as the bound, the guard reads the belief *certainly marked
after* the step, a failing command is "note only, run continues", and the
crash batch (above) came first. A synchronous command blocks the run loop,
captures stdout+stderr (cut at 4096 bytes) and is bounded by
`--hook-timeout S` (1–3600, default 60 — a child past its deadline is
killed and counts as `timeout`); an asynchronous one is fire-and-forget
(`launched`, output to /dev/null, children reaped — never killed — at
exit). Commands run through `sh -c` in mbt's working directory with
`MBT_TRANSITION`, `MBT_TRANSITION_LABEL`, `MBT_RUN` and `MBT_LOG` beside
the inherited environment; model data is never interpolated into the
command line. Hook records round-trip through the run log and are adopted
on restore **without re-executing** the commands; the report prints a
`Command hooks` table, the Guidance line says
`commands: N transition(s), timeout S s`, and the `--json` always carries
`hook_timeout`/`hook_runs`.

The suite stands at **16 CTest entries** — 15 unit suites (`tst_session`
49 tests, `tst_jsonnetloader` 38; `tst_runlogio` gained the hook-records
round-trip, now 8) plus `cli_test.sh` (33 scenarios) — all green.
