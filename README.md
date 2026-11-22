# Egge's Model-Based Tester (eggembt)

`eggembt` tests a system against a **Petri-net specification** of its behaviour.
The model says what inputs may be sent, what outputs must come back, and what
constitutes an error; the tool then drives the system (or a simulation of it)
through test campaigns, watching every event against the model.

```
eggembt check    MODEL           check a model for errors: deadlocks, livelocks, ...
eggembt explore  MODEL           explore the reachability graph (--dot for Graphviz)
eggembt run      MODEL --simulate | --agent "CMD" | --connect H:P | --listen | --stdio
eggembt report   MODEL --log RUNS.jsonl [--runs | --trace N] [--dot FILE]
eggembt agent-sim MODEL           the built-in simulated SUT, speaking the plug-in protocol
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
    src/cli      the `eggembt` command line tool (main.cpp sits next to it)
    tests        unit tests of every layer, plus shared test helpers
                 (ElevatorModel.h is the model most tests use)
    examples     sample models: elevator-door, drink-machine,
                 heartbeat-livelock, atm (added: GLM-5.3-Flash)
    cli_test.sh  end-to-end tests of the tool: real processes, pipes, sockets
    INSTALL.md   prerequisites, build, tests, installation, troubleshooting

## Build and test

Requires CMake ≥ 3.16 and a C++17 compiler. No third-party dependencies.

    cmake -S . -B build -G Ninja
    cmake --build build
    ctest --test-dir build

`cmake --install build --prefix "$HOME/.local"` installs `eggembt` and
`ref-agent` into `$PREFIX/bin`; `INSTALL.md` has the prerequisites, the test
commands, the platform notes and troubleshooting.

The 19 tests are 18 unit test suites (engine, coverage, belief tracking,
livelock analysis, suggester, ant colony, async event queue, session, remote
plug-in, transports, model loader, PNML import, crash experiments, coverage
overlap, run log, pheromone benchmark, lifecycle inputs, run-size benchmark)
plus `cli_test.sh`, which runs the `eggembt` binary end to end against the
examples.

## Model format and plug-in protocol

Models are JSON; see `examples/elevator-door.json` for places, transitions
(`controllable` / `observable` / `internal`), arcs (including inhibitor arcs)
and the initial, target and error markings. A system under test takes part over
the plug-in protocol — one JSON object per line, exchanged over standard
streams, a pipe or TCP (`--stdio`, `--agent`, `--connect`, `--listen`); the
`eggembt agent-sim` shim turns any plug-in into an agent, and the built-in
simulated SUT behaves exactly as its model says.

---

## Changes

### GLM-5.3-Flash — switch the ant colony off

The ant-colony guidance can be switched off per campaign:

    eggembt run MODEL --simulate --no-ants

- `--no-ants` on the `eggembt run` command line (engine counterpart:
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
- `docs/PLUGIN_PROTOCOL.md` written: the full wire protocol between `eggembt run`
  and an agent — field tables for all ten message types, an example run, a
  checklist for agent authors, and where the code lives. Closes the reference
  in §8 of `journal.md` and in `Protocol.h` / `AgentServer.h`.
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
- `cli_test.sh` scenario `state-limit`: `eggembt explore --max-states 4` reports
  "4 states, 3 edges" with the "(incomplete: state limit or place bounds)"
  marker, and `eggembt run --max-states 4` warns and still finishes with
  "coverage complete".

### GLM-5.3-Flash — seeded campaigns and the `traced` key

**`--seed N` for `eggembt run`** (new, default 0): the input choice had one
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
  used by the belief tracker, the simulated SUT and `eggembt check`'s kind counts.
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
- new command **`eggembt compare MODEL LOG_A LOG_B [--csv FILE] [--json FILE]`**:
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
  instead of fewer; documented in `journal.md` §13.)

Try it:

    eggembt check examples/atm.json
    eggembt run examples/atm.json --simulate --sim-seed 5 --max-runs 45 --quiescence 40

Tested in `cli_test.sh` (scenario `atm-visit`). The suite stands at 14 CTest
entries — 13 unit suites plus `cli_test.sh` (27 scenarios) — all green.

### GLM-5.3-Flash — milestones, offered-vs-taken pheromones, PNML import, rebuild, reference agent

Five things in one batch — the **milestone label** plus §12 items 3, 4, 6 and
7 of the design doc (each documented with the item quoted verbatim in
`journal.md` §14):

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
JSON loader's issue reporting. The eggembt conventions: a
`<toolspecific tool="eggembt" kind="controllable|observable|internal"
label="...">` per transition, a milestone as a second toolspecific
(`kind="milestone" rank="N"`), the `?`/`!` id-prefix fallback for
controllable/observable, unmarked transitions internal and **silent**, and the
per-place initial markings unioned into one marking named after the net. No
bounds or target/error markings come out of PNML (warned about). `eggembt check`
and `eggembt run` accept PNML files (recognised by the leading `<`) next to JSON.
Shipped example `examples/coffee-pnml.pnml` — try
`eggembt check examples/coffee-pnml.pnml` and
`eggembt run examples/coffee-pnml.pnml --simulate`. Tested in the new
`tests/tst_pnml.cpp` (21 tests) and `cli_test.sh` (`pnml-model`).

**The reference agent shim** (§12 item 7). `examples/agents/ref_agent.cpp` is
a hand-written C++ SUT (a coffee machine) wrapped in `ISutPlugin` +
`AgentServer` over standard streams, served by `eggembt run
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
`journal.md` §15):

**Backtraces in `eggembt report`** (§12 item 2). The GUI half of the item was
scratched by decision — *"a textual report is fine"* — so the coverage-and-
backtraces view ships as text: `eggembt report` prints, per definitely covered
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

Three things in one batch, documented in full in `journal.md` §16:

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

**Crash predictions** (documented in full in `journal.md` §17). A
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

**`--credits`** (`eggembt credits`, `eggembt --credits`): prints the project's
credits as Egge specified them (the one output that carries a signature, by
explicit request), except the first line, which the later rename changed to the
new product name:

    Egge's Model-Based Tester
        Designer: Esa Kettunen
        Copyright (c) Esa Kettunen 2026
        Software written by artificial intelligence: Claude Sonnet 5.5 Medium
        Software written by artificial intelligence: GLM-5.3-Flash
        Software written by artificial intelligence: DeepSeek V4.1 Flash

The suite stands at **16 CTest entries** — 15 unit suites (new: `tst_crash`;
`tst_session` 41, `tst_jsonnetloader` 35) plus `cli_test.sh` (32 scenarios) —
all green.

### GLM-5.3-Flash — command hooks

**Command hooks** (documented in full in `journal.md` §18; user manual §9). A
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
exit). Commands run through `sh -c` in eggembt's working directory with
`EGGEMBT_TRANSITION`, `EGGEMBT_TRANSITION_LABEL`, `EGGEMBT_RUN` and `EGGEMBT_LOG` beside
the inherited environment; model data is never interpolated into the
command line. Hook records round-trip through the run log and are adopted
on restore **without re-executing** the commands; the report prints a
`Command hooks` table, the Guidance line says
`commands: N transition(s), timeout S s`, and the `--json` always carries
`hook_timeout`/`hook_runs`.

The suite stands at **16 CTest entries** — 15 unit suites (`tst_session`
49 tests, `tst_jsonnetloader` 38; `tst_runlogio` gained the hook-records
round-trip, now 8) plus `cli_test.sh` (33 scenarios) — all green.

### DeepSeek V4.1 Flash — the rename, and synchronized events

**The rename.** The product is now **Egge's Model-Based Tester**, abbreviated
`eggembt` — the binary, the command in every example, and the CMake project.
The `MBT_*` environment prefix became `EGGEMBT_*` (including the hooks'
`EGGEMBT_TRANSITION`, `EGGEMBT_TRANSITION_LABEL`, `EGGEMBT_RUN`, `EGGEMBT_LOG`),
the credits' first line reads `Egge's Model-Based Tester`, and the agent names
and test temp prefix follow. Two format tags moved with backward
compatibility: PNML accepts `tool="eggembt"` and still `tool="mbt"`; run logs
are written as `"format":"eggembt-runlog"` and still read `"mbt-runlog"`. The
internal library targets `mbt_engine`/`mbt_core`/`mbt_agent` and the macro
`MBT_POSIX_IO` keep their names on purpose.

**Synchronized events** (documented in full in `journal.md` §19; user manual
§2.3, §8.3). A model can mark a transition of the SUT's own moves with
`"sync": true` (observable, or traced internal — controllable transitions are
rejected, their stimulus is acknowledged already). With the campaign switch
`--sync-events` (default off) the SUT is then **held at that event** until the
tracker has handled it: the event is pumped, its consequences run — including
a post-mortem, which is the point, because only a frozen SUT gives an accurate
stack — and only then does the ack release it. A run that ends while the SUT is
held sends no ack; `stop_run`, `shutdown` or the closing connection releases
it. The plug-in capability is `eventAcks` (`"sync": true` in hello); the wire
adds `sync_labels` to `start_run`, `seq` and `"sync"` to `event`, and a new
tracker→agent `{"type":"ack","run":N,"seq":M}` — all capability-gated, so the
protocol version stays 1. An agent that cannot acknowledge does not stop
anything: the run goes on unsynchronized with a note saying why, once per
campaign. The Guidance block reports the state
(`sync: N transition(s), held until acknowledged (--sync-events)` or
`not held (no --sync-events)`), and the `--json` carries `sync_events` and
`sync_transitions`.

**The version** now lives in `src/Version.h` and follows the git branch
(`v0.0.1`), instead of the premature `1.0.0` in the CMake project (`eggembt
version` prints `eggembt v0.0.1`). The credits gained a sixth line,
`Software written by artificial intelligence: DeepSeek V4.1 Flash`.

The suite stands at **16 CTest entries** — 15 unit suites (`tst_session`
53 tests, `tst_jsonnetloader` 40, `tst_remote` 19) plus `cli_test.sh`
(34 scenarios) — all green.

### DeepSeek V4.1 Flash — inspecting a campaign afterwards

**Three views of the record, one drawing** (documented in `journal.md` §21; user
manual §8.3, §10.5). `report --runs` lists every run's recorded steps, a long
one keeping its last 20 and saying `[N steps]`; `report --trace N` prints one
run in full with its detail. Both read the log's own steps — an event run shows
its events — so nothing is re-simulated. `--coverage-elements` on `run` or
`report` adds `coverage.states[]` and `coverage.edges[]` to the `--json`
report: per known element its marking (or `from`/`transition`/`label`/`to`),
`definite`, `ambiguous`, `covered` (the `--threshold` rule) and the 1-based
`first_run`/`last_run`. `report --dot FILE` draws the graph for Graphviz
coloured by that coverage — dark green and thick for covered, light green for
visited below the threshold, orange for possibly visited, grey for never
visited, with a legend; `explore --dot` stays plain because it runs before any
campaign.

`--dump-pheromone FILE` (on `run`, or on `report`) writes the colony as JSON:
per edge its `from`, `transition`, `label`, `to`, `attract`, `repel` and
static `region`; per (marking, T-list) pair its `tlist` and both channels;
plus `tlist`, `tau_max`, `iterations`, `ant_walks`. Everything is keyed by
readable names because edge ids are session-local. The new
`AntColony::pairEntries()` view sorts by (state, list) so two dumps of the same
state are identical. The log records the campaign's steps, not its colony
settings, so `report` gained `--tlist N`: give it the campaign's value and a
rebuilt dump shows the same pair rows (four runs of `examples/atm.json` at
`--tlist 4` yield the same three rows live and rebuilt). The rebuild stays the
documented approximation — milestone and catastrophe deposits at
`--restored-pheromones`, hence `iterations`/`ant_walks` 0 on `report`.

The suite stands at **16 CTest entries** — 15 unit suites (`tst_ants` gained
`pairEntriesAreReadableAndSorted`, now 22; `tst_session` 53,
`tst_jsonnetloader` 40, `tst_remote` 19) plus `cli_test.sh` (35 scenarios,
+`inspection`) — all green.

### DeepSeek V4.1 Flash — the colony written out, Jaccard's index, and a column

**A cosmetic fix with a test.** The backtrace table padded transition names
to a fixed 16 characters, so the ATM's `bank.stolen_report` ran into its
chain. The column now follows the longest name printed and gets one separator
space; the `inspection` scenario in `cli_test.sh` pins the space.

**The colony, checked against its code** (user manual §7.1). The choice rule
is now written out instead of described as a "cost":

    weight(edge) = (attract + 0.1)^alpha · novelty^beta · exp(-lambda · (repel + region))
    novelty      = 1 / (1 + hits)      (0.01 when the edge is saturated)
    alpha = beta = lambda = 1

with the two-step draw (SUT moves vs tester inputs at 0.5 each, then by
weight). The novel-territory deposit was also misdescribed: an ant deposits
1.0 **at the state** whose way out is uncovered (or unexpandable within the
round's 200-state budget) and ends its walk — not on the edges into unvisited
states; the per-edge `1/(1 + hits)` factor is separate. Everything else was
verified against `AntConfig` and stands: 20 ants × 40 steps, back-propagation
of at least 5 nodes at 0.7 decay, evaporation 0.10/0.02, `tauMax` 10,
milestone 1.5 × rank, catastrophe 3.0 × (1 + 0.25 · min(stimuli, 8)), static
region repel 1.0, saturation. The manual's command list gained the `credits`
line the help has carried since the rename.

**Jaccard's index, verbatim** (user manual §10.6; `journal.md` §22). For two sets
of covered elements, `J(A, B) = |A ∩ B| / |A ∪ B| = |A ∩ B| / (|A| + |B| −
|A ∩ B|)`, computed separately for transitions, states and edges. The sets are
the elements a run *definitely* touched — states visited (initial state
included), transitions certainly fired (ambiguous credits included), edges
traversed — presence only, never frequency. `J(A,A) = 1`, two empty sets count
as identical (`0/0` is not computable), campaign sets are the unions of their
runs, and `mean_pairwise` averages the `n(n−1)/2` unordered pairs. Worked
example: `{insert_card, open_session, connect_db, reader_check}` against
`{insert_card, open_session, connect_db, atm_fail}` is `3/5 = 60 %`. The ATM
demo's two logs: campaigns 93 / 80 / 80 %, within-campaign 49 / 8 / 6 and
49 / 7 / 6, cross means 58 / 23 / 22 (transitions / states / edges). It is not
a coverage measure, not the overlap coefficient `|A ∩ B| / min(|A|, |B|)`, and
not frequency- or order-aware; the console rounds, `--csv`/`--json` are exact.

### DeepSeek V4.1 Flash — v0.0.2: PNML export, a testing strategy, a budget, and evaporation at the run boundary

**PNML export** (`eggembt export MODEL [--output FILE] [--strict]`; user
manual §8.6, `journal.md` §27). The model is written as standard PNML
(ISO/IEC 15909-2): places, transitions, normal and inhibitor arcs with their
weights, and the **first** initial marking. The eggembt conventions the PNML
loader understands — transition kind, label, milestone rank — are written as
`<toolspecific tool="eggembt" ...>` and survive a round trip (the shipped
`examples/coffee-pnml.pnml` is the pinned case). Everything PNML cannot
express — the other initial markings, target and error markings, policies,
place bounds, transition tags, crash predictions, the strategy — is written
twice: as XML comments for the reader and as one machine-readable JSON
`<toolspecific>` block. Those annotations do not survive an import, and the
file's header says so. Ids that are not XML names are exported as `pN`/`tN`
with the original kept in `<name>` and the annotations. Without `--output`
the document is the only thing on stdout; `check` accepts the result.

**Testing strategy** (user manual §8.5). A root-level model key
`"strategy": {"goals": "balanced|targets|coverage", "errors":
"balanced|seek|avoid"}`, overridable per campaign with `--goal-priority` and
`--error-priority`. `targets` prices fresh transitions 2, edges 4 and
frontier states 8 extra steps and raises target/milestone deposits 1.5×;
`coverage` prices a sought target 4 extra steps while fresh edges cost 0 and
frontier states 1. `seek` sets the suggester's risk/hazard weights to 1/10;
`avoid` to 5/40. The halves are independent. `check`, the campaign's
`Guidance` block and the campaign JSON name the effective values. The
complete JSON remains the authority for what the model says.

**Wall-clock budget.** `--budget DURATION` (bare seconds, or `ms/s/m/h`;
positive, at most 30 days) bounds the whole campaign, not each run. The
deadline is checked at every run boundary and every loop turn of a running
run; the cut-short run is a normal `completed` run with the detail `campaign
time budget reached` and `"budget_reached": true`; the campaign's
`stop_reason` is `time budget reached`, and the exit code is unchanged. A
budget is planning, not a fault, and does not make `"complete"` true.

**Evaporation at the run boundary** (`--evaporate-between-runs`; user manual
§7.4). The ants still walk at every suggestion, but the two pheromone
channels age once per finished run instead of once per suggestion. The
question was whether evaporation can ruin progress and at what stage: yes,
inside a long run. The per-suggestion rates are 10 %/2 %, so the repel
channel's half-life is ~34 suggestions and a run with 200 suggestions keeps
only 1.8 % of what its start taught. On the 1000-state diamond benchmark
(six runs × 200 events), per-suggestion aging crashes the SUT 3 times and
covers 586 edges; boundary aging crashes twice, keeps four runs productive
and covers 777. The mechanical test pins 200 walking iterations without
aging, one `age()` at 98 %, and the old 1.8 % after 200 per-suggestion
agings. The campaign report names the boundary, and the JSON carries
`evaporate_between_runs`.

**The limits of the approach** (user manual §12, `journal.md` §27). The new
chapter documents the measured failure modes: edge-channel saturation and a
sought target outshouting the catastrophe repel (on the benchmark, ants
without the T-suffix pair layer are exactly as good as no ants); the pair
layer's exact-match sparsity (short suffixes merge routes, long ones
fragment); evaporation versus long runs; target and catastrophe being
indistinguishable when the recent history is; truncated graphs; human
unpredictability; and the strategy's deliberate trade-offs.

**T-suffix, not T-list.** The feature and every live name around it were
renamed: `TSuffix.h`, `kMaxTSuffix`, `tsuffixLength`, the `tsuffix` JSON key,
the `--tsuffix` switch, the user manual and `docs/`. Historical changelog
entries (this file, `journal.md` §§16, 21, 25) keep the word as it was written
then, and Egge's own quoted text keeps it with a note. No behaviour changed.

The suite stands at **18 CTest entries** — 17 unit suites (288 tests;
`tst_pnml` 24, `tst_jsonnetloader` 43, `tst_session` 55, `tst_ants` 23,
`tst_pheromone` 2) plus `cli_test.sh` (39 scenarios, +`export`, +`strategy`,
+`budget`, +`evaporation-between-runs`) — all green.

### DeepSeek V4.1 Flash — v0.0.3: the satisfied-target phase policy

`--satisfied-target-policy veto|penalise|ignore` (user manual §8.5,
`journal.md` §28). Every target is `seeking` while it is still a goal and
`satisfied` once its policy count is reached; its states are then forbidden.
The default, `veto`, keeps the historical rule: the suggester never chooses
a move into a satisfied target and plans route around it. `penalise` allows
the move and the plan but keeps the hazard fraction in the score; `ignore`
treats a satisfied target as an ordinary state. The `Seeking -> Satisfied`
transition, the outcomes (`error_reached`, `deadlock`) and the
intentional-target ending are unchanged — the switch only says whether the
suggester may choose the move. It is useful when a satisfied error marking
blocks the only corridor to behaviour that is still uncovered, and the
relaxation is off by default because the price is paid in whole runs.

The console `Guidance` block prints `satisfied targets: <policy>` and the
campaign JSON always carries `satisfied_target_policy`. The suite stands at
**18 CTest entries** — 17 unit suites (290 tests; `tst_pnml` 24,
`tst_jsonnetloader` 43, `tst_session` 55, `tst_ants` 23, `tst_pheromone` 2,
`tst_suggest` 20) plus `cli_test.sh` (40 scenarios, +`export`, +`strategy`,
+`budget`, +`evaporation-between-runs`, +`satisfied-target-policy`) — all
green. `eggembt version` prints `v0.0.3`.

### DeepSeek V4.1 Flash — v0.0.4: rare transitions and the hunt

Some SUT moves happen only rarely. A transition can now say so:

```json
{"id": "glitch", "kind": "observable", "label": "glitch",
 "rare": {"p": 0.001, "attempts": 100, "wait-for": 60000}}
```

`"rare": true` takes the defaults (`p` 0.001, `attempts` 100, `wait-for` 0);
`"rare": false` is no marker; an object may set any subset. `p` is
descriptive only — the tool cannot verify the SUT's real odds — while
`attempts` is the campaign's budget and `wait-for` the dwell at the enabling
state. The transition must be visible, so an untraced internal step cannot
be rare. `check` prints the markers and their contracts.

`--hunt-rare` (default off) makes an unseen rare transition a goal above
every other goal: targets, fresh transitions, edges and the frontier. The
enabling state is the goal, because the tester cannot take a move of the SUT
itself. An enabled rare move is given its `wait-for` as a deliberate dwell;
any real event ends the dwell early and a withheld controllable input is
dropped when the state moves on. The idle timeout is suspended during the
dwell, but the wall-clock budget and a stop request still apply. Every dwell
is one attempt, even within one run; a controllable rare input sent without
a dwell is one attempt too. The counts are campaign-scoped and are not
written to the run log.

Rare goals outrank coverage, so the campaign does not declare itself
complete while one is unseen with attempts left. The hard limits
(`--max-runs`, `--max-events`, `--max-states`, `--budget`) still hold, and a
campaign cut short says `run limit reached; rare transitions still unseen`
or `time budget reached; rare transitions still unseen`. When the attempt
budget is spent and the transition is still unseen, the hunt gives up and
the campaign may complete normally.

The console `Guidance` block prints `rare: N transition(s), S seen, A
attempt(s) used; hunting (--hunt-rare)` (or `not hunted`), and the campaign
JSON always carries `hunt_rare` and `rare_transitions` with `transition`,
`label`, `p`, `attempts`, `wait_for_ms`, `seen` and `attempts_used`. The
PNML export keeps the contract in the transition comment and annotation.

The third pheromone benchmark now sweeps the evaporation rates (`journal.md`
§31): the per-suggestion repel rate from 0.005 to 0.10 gives the same
outcome on the 1000-state diamond (3 crashes, 3 productive runs, about 586
edges), because the fork is reached about five suggestions after the
deposit. The test pins that plateau instead of manufacturing a cliff, and
the between-runs contrast (2 crashes, 4 productive runs, 777 edges) remains.

**Many short runs or a few long ones?** A ten-minute campaign can spend its
budget on 266 runs of 50 events or 4 runs of 3200. The new `tst_comm10k`
benchmark builds a 10,000-state model of four communicating machines (24
places, 36 transitions, 64,000 edges, four goals, four errors) and measures
both ends. On three seeds the mean covered edges were 12,198 (50), 12,527
(200), 13,254 (800) and 11,798 (3200). The best-to-worst mean spread is
12 %, smaller than the seed-to-seed spread within a size, and the ordering
changed between seeds: many tiny runs were not better, nor were four huge
ones. The only monotonic signal was per event — 0.918 new edges at 50 events
per run up to 0.972 at 3200 — because every short run re-tests its initial
region. A middle range (a few hundred to a few thousand events per run) is a
sensible place to start; `--max-events 10000` is a safety cap, not a
coverage target. `journal.md` §32 has the table and `tst_comm10k --long` runs
the four campaigns.

The suite stands at **19 CTest entries** — 18 unit suites (298 tests;
`tst_pnml` 24, `tst_jsonnetloader` 45, `tst_session` 58, `tst_suggest` 21,
`tst_ants` 23, `tst_pheromone` 3, `tst_comm10k` 1) plus `cli_test.sh` (41
scenarios, +`rare`) — all green. `eggembt version` prints `v0.0.4`.

### DeepSeek V4.1 Flash — v0.0.5: merge-time evaporation for restored runs

When a campaign continues from a run log, the earlier runs' pheromone
teaching is rebuilt and merged (§16). It normally enters frozen at
`--restored-pheromones` percent. `--evaporate-restored-pheromones` (default
off) lets it age instead: before each restored run after the first, the
teaching rebuilt so far ages once with the colony's ordinary rates (attract
10 %, repel 2 %, saturated attract wiped). The newest restored run therefore
enters at full rebuilt strength, and older runs fade by one step for every
later restored run.

The exception is a **matured finding**. A target, error marking, deadlock or
livelock whose phase is `satisfied` (its `once` / `until_covered` /
`target_hits` criterion is reached; `avoid` makes the count 0) is spared:
the edge channels into that state and its `(marking, T-suffix)` pair entries
do not evaporate. Milestones are transition rewards without a visit count
and do not mature; their attract ages normally unless the milestone's
destination state is itself matured, in which case the edge is spared by the
destination rule. A crash experiment still `seeking` is not matured.
Saturation remains a coverage rule, not evaporation: a saturated edge still
loses its attract. With `--no-analyze` the static livelock findings are
absent; otherwise the analysis runs before the restored runs so terminal
livelocks are known when maturation is checked.

The console `Guidance` block prints `restored pheromones: N%; merge
evaporation: off` or `between restored runs, matured targets spared`, and
the campaign JSON always carries `evaporate_restored_pheromones`. The exact
merge arithmetic is pinned in `tst_session` (`restoredPheromonesAgeBetweenRuns`)
and the sparing in `tst_ants` (`maturedTargetsAreSparedByMergeEvaporation`).

The suite stands at **19 CTest entries** — 18 unit suites (300 tests;
`tst_pnml` 24, `tst_jsonnetloader` 45, `tst_session` 59, `tst_suggest` 21,
`tst_ants` 24, `tst_pheromone` 3, `tst_comm10k` 1) plus `cli_test.sh` (42
scenarios, +`evaporate-restored-pheromones`) — all green. `eggembt version`
prints `v0.0.5`.
