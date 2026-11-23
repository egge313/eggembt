# Egge's Model-Based Tester (eggembt) — The User Manual

* Author: **Esa Kettunen** alias **Egge**
* Affiliation: **Egge Collective**, currently comprising one (1) person
* Contact: esa.kettunen@gmx.com

This file has been mostly written by two AIs, GLM-5.3-Flash and
DeepSeek V4.1 Flash. The original draft and a preliminary
implementation for the eggembt program was written by yet another Ai,
Claude Sonnet, and later worked on by GLM and DeepSeek, all of based
on Egge's ideas and prompts. The software and documentation have been
reviewed by Egge.

First version of this manual was written on 17 November 2026.

This manual is a recompilation of the daily records
documented in `journal.md` §16. Read `README.md` for a short overview and
`journal.md` for the design record. Two conventions:*

- *Every blockquote (`>`) in this manual is a text Egge actually sent, quoted
  byte for byte. Nothing is invented; the explanations around the quotes are
  this manual's own words.*
- *Fenced blocks show real tool output, captured from the examples shipped in
  `examples/`.*

---

## 1. What model-based testing is

When you test a piece of software the traditional way, you write **test
cases** by hand: step 1, press this; step 2, expect that. Two problems come
with that. First, you only ever test what you happened to think of — the
paths you did not think of stay untested forever. Second, every change to the
system makes your pile of test scripts older and wronger.

**Model-based testing** turns this around. Before testing, you write down how
the system should behave — once, precisely, as a small machine. This
description is called the **model**. A tool then does two jobs with it:

1. It **analyzes the model itself**, finding dead ends and terminal cycles in
   your description before you ever touch the real system.
2. It **generates and executes tests against your real system** (or a
   simulation of it), driving it through sequence after sequence of
   interactions and checking *every event* against the model. Anything the
   system does that the model cannot explain is reported as a candidate bug;
   everything the model and system agree on is accumulated as **coverage** —
   a precise answer to "what have we actually tested?"

Your effort goes into one model instead of hundreds of scripts, and the tool
explores systematically instead of following your hunches.

A model alone is not yet a strategy. The simplest way to explore one is a
blind walk: take an enabled move at random, record what happened, restart,
repeat. That walk is blindfolded with respect to coverage — it carries no
memory of where it has been, so it keeps returning to the same easy states
and edges while a deep branch waits. eggembt's baseline is one step better:
it records coverage per state, edge and transition and plans toward the
nearest uncovered item. The ant colony adds memory — attractive pheromone
marks routes that led to something new or to a declared goal, repellent
pheromone marks catastrophes, and evaporation lets stale lessons fade.
Targets, error states and milestones add intent: they say what "worth
reaching" means, not only "not seen yet". The rest of this manual separates
those layers: §5 defines the targets, §7 the colony, §8.5 the strategy, and
§12 where the heuristics stop helping.

In eggembt the model is a **Petri net** (§2), the tool is the command
`eggembt`, and the "system under test" — SUT for short — can be your real
program, or the built-in simulator that lets you try everything without any
system at all.

---

## 2. The model: a Petri net

### 2.1 The pieces

A Petri net has three kinds of parts:

- **Places** — situations or facts ("a coin has been inserted", "the door is
  open"). Drawn as circles.
- **Tokens** — small markers sitting in places. Which places hold how many
  tokens right now is the **marking** — the model's current situation. Think
  of the marking as "where we are".
- **Transitions** — actions that move tokens ("insert coin", "serve coffee").
  Drawn as bars between places. A transition may fire when its input places
  hold the required tokens; firing takes tokens from some places and puts
  tokens into others.

Runs start in an **initial marking**, and every firing moves the marking
somewhere else. That is the whole machine.

### 2.2 The three kinds of transitions

In a test, the tester and the SUT take turns. The model says who owns which
action:

- **controllable** — an *input*: the tester sends this to the SUT (a key
  press, a command, a request). The run decides when to send it.
- **observable** — an *output*: the SUT produces this and the run must see it
  (a message, a served coffee, a closed-door event). The run waits for it.
- **internal** — a step with no counterpart in the SUT: the model's own
  bookkeeping (an obstruction happening on its own, a timer the model
  abstracts). Internal transitions are **silent** by default — they fire in
  the model but appear in no trace. An internal transition with
  `"traced": true` is visible: it needs a `label` that the SUT reports the
  step as.

Transitions carry a **label** — the word actually exchanged with the SUT on
the wire, like `coin.insert` or `coffee.served`. The `id` is for the model
file; the label is for the SUT.

### 2.3 The annotations that make it a *test* model

Beyond the plain machine, a model can declare what is good, what is bad, and
what is worth noting:

- **Target markings** (`markings.target`) — situations the campaign seeks
  (§5).
- **Error markings** (`markings.error`) — situations the SUT must never
  reach (§6). Each can carry options:

  ```json
  {"name": "jammed", "match": "covers", "tokens": {"fault": 1},
   "policy": "until_covered", "target_hits": 3, "crash_dump": "snapshot"}
  ```

  `match` is `"exact"` (default: the marking must be *exactly* these tokens;
  unlisted places hold 0) or `"covers"` (at least these tokens; other places
  are free). `policy` is `"until_covered"` (default: an error until it has
  been covered `target_hits` times) or `"once"` (an error on every arrival).
  `crash_dump` collects a post-mortem: `"none"` (default), `"snapshot"` (ask
  the SUT's agent for a state dump) or `"crash"` (crash the SUT on purpose
  for a post-mortem); `max_dumps` limits the dumps over the whole campaign
  (default 1).
- **Error transitions** — a transition can itself be forbidden:
  `"error": true`, optionally with `"crash_dump"` and `"max_dumps"` (§6).
- **Milestones** — a transition the campaign wants to step on during the way:
  `"milestone": true` or a rank `"milestone": 2`. Reaching one is a success
  worth advertising, but it does *not* end the run (§7).
- **Rare transitions** — a move the SUT makes only rarely. Mark it with
  `"rare": true` (the defaults) or with an object:

  ```json
  {"id": "glitch", "kind": "observable", "label": "glitch",
   "rare": {"p": 0.001, "attempts": 100, "wait-for": 60000}}
  ```

  `p` (default 0.001) is descriptive only — the tool cannot verify the SUT's
  real odds, so it is reported but never used for scheduling. `attempts`
  (default 100) is the campaign's budget of attempts to see the move;
  `wait-for` (default 0 ms) is how long to dwell at the enabling state.
  `"rare": false` means no marker. The transition must be visible: an
  untraced internal step cannot be rare because the tester would never see
  it. The marker does nothing unless the campaign is run with `--hunt-rare`
  (§8.5).
- **Crash predictions** — a transition or a target/error marking can predict
  a *nondeterministic* SUT crash: `"crash": "sometimes"` (a class word:
  `random`, `rare`, `sometimes`, `often`, `always`), a bare number
  (`"crash": 0.7`, an explicit probability) or an object
  `{"class": "rare", "p": 0.3, "attempts": 7, "crash_dump": "snapshot"}`.
  A predicted crash is a *target* the campaign tries to reproduce until the
  experiment behind it is certain it is deterministic (§6.11).
- **The testing strategy** — a root-level `"strategy"` object says what the
  campaign should weigh more, without changing the SUT: `{"goals":
  "balanced" | "targets" | "coverage", "errors": "balanced" | "seek" |
  "avoid"}`. It is advice, not part of the system; the command line can
  override each half (§8.5).
- **Command hooks** — a transition can carry a shell command that runs when
  its step is certainly fired: `"command": "collect.sh"` (a synchronous
  command) or an object
  `{"run": "verify.sh", "mode": "async", "times": 5, "when": "ready"}`.
  `sync` (default) blocks the run while the command runs (`--hook-timeout`
  bounds it), `async` is fire-and-forget; `times` caps the executions over
  the whole campaign; `when` names a place that must hold a token in every
  believed state after the step. The step must be *visible* (an untraced
  internal transition cannot carry a command). What the command prints and
  how it ends is recorded in the run and reported (§9); a failing command
  is a note, the run continues (§9).
- **Synchronized events** — `"sync": true` on a transition of the SUT's own
  moves (observable, or traced internal) holds the SUT there until the
  tracker has handled the event and acknowledged it. It needs the campaign
  switch `--sync-events` (default off) and a plug-in that can acknowledge;
  without the capability the run goes on unsynchronized and says why. Slower,
  but a post-mortem then sees the stack the event left behind (§8.2, §11).
- **Lifecycle inputs** — a *controllable* transition whose label is exactly
  `start`, `restart`, `stop`, `pause` or `resume` is reserved: the tester
  still sends the label, and the SUT's own control does the work (the reason
  the feature exists: an SUT that is started by hand cannot be restarted from
  the command line). The engine adds the lifecycle meaning on top:

  ```json
  {"id": "restart", "kind": "controllable", "label": "restart"}
  ```

  `start` and `restart` *without post arcs* land the run at its initial
  marking — the SUT is back at its beginning — and are covered as a
  transition (they have no edge of their own); with post arcs the arcs say
  where the run lands. `resume` without post arcs continues where the `pause`
  suspended the run. `stop` and `pause` only close the gate. While paused only
  `resume`, `stop` and `restart` are offered; while stopped only `start` and
  `restart`; silence is expected (no deadlock, no idle abort). A model with an
  arc-less `start` or `restart` begins every run with the gate down, so the
  campaign presses that control before it tests anything. The words are
  case-sensitive and reserved only on controllable transitions: on an
  observable or internal transition `"start"` stays an ordinary label.
- **`"unpredictable": true`** — this move belongs to a human operator, not to
  the tester and not to the machine. The tool cannot say when it happens
  (§6, catastrophe pricing).
- Places can carry `"bound"` (a token limit), and arcs can be
  `"type": "inhibitor"` (the transition may fire only while that place is
  **empty**).

### 2.4 A worked example

`examples/drink-machine.json`, in full:

```json
{
  "version": 1,
  "name": "drink-machine",

  "places": [
    {"id": "idle"},
    {"id": "paid"},
    {"id": "brewing_coffee"},
    {"id": "brewing_tea"}
  ],

  "transitions": [
    {"id": "insert_coin",  "label": "coin.insert",    "kind": "controllable"},
    {"id": "press_coffee", "label": "button.coffee",  "kind": "controllable"},
    {"id": "press_tea",    "label": "button.tea",     "kind": "controllable"},
    {"id": "press_refund", "label": "button.refund",  "kind": "controllable"},
    {"id": "serve_coffee", "label": "coffee.served",  "kind": "observable"},
    {"id": "serve_tea",    "label": "tea.served",     "kind": "observable"}
  ],

  "arcs": [
    {"from": "idle", "to": "insert_coin"},       {"from": "insert_coin", "to": "paid"},
    {"from": "paid", "to": "press_coffee"},      {"from": "press_coffee", "to": "brewing_coffee"},
    {"from": "paid", "to": "press_tea"},         {"from": "press_tea", "to": "brewing_tea"},
    {"from": "paid", "to": "press_refund"},      {"from": "press_refund", "to": "idle"},
    {"from": "brewing_coffee", "to": "serve_coffee"}, {"from": "serve_coffee", "to": "idle"},
    {"from": "brewing_tea", "to": "serve_tea"},  {"from": "serve_tea", "to": "idle"}
  ],

  "markings": {
    "initial": [{"name": "start", "tokens": {"idle": 1}}],
    "target":   [{"name": "idle", "tokens": {"idle": 1}}]
  }
}
```

Read it as a story: the machine starts **idle**; a coin pays it (**paid**);
then the user presses coffee or tea (the machine brews), presses refund
(back to **idle**), or presses nothing; a brewed drink ends with the drink
served and the machine **idle** again.

`eggembt check` validates the model and describes it:

```
Model  drink-machine
  4 places, 6 transitions (4 inputs, 2 outputs, 0 traced and 0 silent internal)
  initial markings: start
  target markings:  idle
  error markings:   none

Reachability graph
  4 states, 6 edges (explored completely)
  deadlocks: 0
  terminal cycles without a target marking (livelocks): 0
```

`examples/elevator-door.json` shows the richer annotations (bounds, inhibitor
arcs, a covers-matched error marking with a snapshot post-mortem), and
`examples/atm.json` a larger model with several initial markings and a crash
post-mortem — its check prints the error marking's options in one line:

```
  error markings:   card-swallowed
    card-swallowed: until_covered, 1 hits, post-mortem crash (at most 1)
```

PNML is accepted too: `eggembt check examples/coffee-pnml.pnml` and
`eggembt run examples/coffee-pnml.pnml --simulate` work beside the JSON files
(Egge's open item, quoted verbatim):

> PNML import.

---

## 3. The reachability graph

This is one of the concepts to be careful with, so take it slowly.

### 3.1 What it is

The **reachability graph** is the answer to the question: *everything that
can happen in this model, and every way to get there — what does that look
like?* The tool computes it by playing out the model on paper:

1. Start from every initial marking.
2. In the current marking, find every transition that may fire.
3. Fire it, write down the marking you land in, and draw an arrow from where
   you were to where you landed.
4. Repeat for every marking you have written down, until no new marking
   appears.

The result is a graph:

- one **node** per reachable marking ("reachable" = can happen, starting
  from the start, by *some* sequence of moves — hence the name); a marking is
  the Petri-net word for a state of the model;
- one **edge** per possible firing, labelled with the transition that fires.

The reachability graph is the complete map of the model's behaviour. In
Petri-net terminology it is also called the model's **state space**: the two
names mean the same graph, whose nodes are the reachable markings. Every
question of the form "can the model end up in situation X?" has a definite
answer in it.

### 3.2 What the analysis of the graph reports

`eggembt check` and `eggembt explore` walk the graph and report:

- **Deadlocks** — states where *nothing* can fire any more. The machine is
  stuck by definition. In `drink-machine` there are none; in a model where a
  place can deadlock (say, a card swallowed with no way out) the count is
  the number of such dead ends.
- **Terminal cycles without a target marking (livelocks)** — a *cycle* of
  states that the model can enter but never legitimately leave: a closed loop
  the specification itself can spin in forever. `examples/heartbeat-livelock.json`
  contains exactly one, and `check` warns about it:

  ```
  Model  heartbeat-livelock
    2 places, 2 transitions (0 inputs, 2 outputs, 0 traced and 0 silent internal)
    initial markings: start
    target markings:  none
    error markings:   none

  Reachability graph
    2 states, 2 edges (explored completely)
    deadlocks: 0
    terminal cycles without a target marking (livelocks): 1
  warning 1 terminal cycle(s) without a target marking found: by definition livelocks of the specification
  warning an initial marking lies inside such a cycle: declare a target marking (e.g. the idle state), otherwise every move counts as entering a livelock and is vetoed
  warning examples/heartbeat-livelock.json: $.markings.target: no target markings: every deadlock will be reported as an error
  ```

  These are findings about the **model**, found before any SUT is touched.
  Fixing them is model work: declare a target marking, break the cycle, add
  the missing output. §6.12 explains how these static findings relate to the
  run outcomes `deadlock` and `livelock`.

`eggembt explore MODEL --dot` writes the graph for Graphviz, so you can look at
it.

### 3.3 How the graph is built during a campaign

During a campaign the graph is grown **lazily**: states the runs actually
reach get expanded as needed, so a campaign can start before the whole graph
exists. `--max-states N` (default 1000000) bounds it; when the bound is hit,
the graph is **truncated** — the tool marks which parts are incomplete and
never reports a finding from an incomplete part, so truncation costs
completeness, not correctness.

---

## 4. Trace, run, campaign, round

Four words the rest of this manual leans on.

**Trace.** A run's **trace** is the ordered list of its **steps** — each step
one transition fired: an input sent, an output observed, an internal step the
model took. The trace is the run's story. It is what gets recorded in the run
log, what a **backtrace** replays backwards
(`start > coin.insert > button.coffee > coffee.served`), and what the T-suffixes
of §7 hash.

**Run.** A **run** is one encounter with the SUT: start in an initial
marking, interact — send inputs, watch outputs — and end. Every end has a
name (§6, §10): `completed`, `deviation`, `deadlock`, `livelock`,
`error_reached`, `sut_crashed`, `aborted`.

**Campaign.** A **campaign** is everything one `eggembt run` invocation does: a
sequence of runs (by default up to `--max-runs 20`) until the coverage is
complete or a limit is hit. Each run starts from an initial marking and is
steered — the colony and the suggester decide which input to send next.

**Round.** A **round** is one invocation of the campaign. `eggembt run` with
`--log FILE` *continues* an earlier round: the log holds every run of every
round, the new invocation replays them, and the pheromone teaching of the
earlier rounds is rebuilt, toned down (§7.3). One warning about the word:
the console line `ant colony: 9 round(s), 180 ant walk(s)` counts the
*colony's own iterations* inside this campaign — a different sense of
"round" than the carry-over's rounds (the campaign rounds of successive
invocations).

**Certain versus ambiguous — read this once.** The tool watches *events*, it
cannot see inside the SUT. Usually it knows exactly which edge the run took
(it sent the input, or exactly one edge explains the event): a **definite
step**, credited exactly, and named in backtraces. Sometimes several edges
explain the same event: an **ambiguous stretch** — the run certainly went on,
but the exact edge is not certain; coverage is credited transition-wise, and
backtraces render the stretch as `...`. Milestones, error transitions and the
T-suffixes (§7.1) act on *definitely credited* steps.

---

## 5. Targets

A **target marking** is a situation the campaign *seeks*: the ant colony
steers runs toward it (§7), and reaching it counts as covering it after
`--threshold` visits (default 1). The name replaced an older, misleading one
("final marking") in the batch of 17 November 2026 — a final marking reads
like net-theory's "final state", a legitimacy property, but the concept here
is the tester's *target*: a marking the campaign steers into.

Two kinds, with different jobs:

- A **plain target marking** — like `idle` in `drink-machine` — is a normal
  situation the campaign should make sure it can reach and revisit. It
  **never ends a run**: runs end on coverage, limits, or errors.
- An **intentional target marking** (`"intentional": true`), a "good target
  state", is the campaign's *purpose*: the moment the SUT is certainly in
  one, the campaign ends successfully — the stop reason names it ("the good
  target marking 'X' was reached").

Targets also give the tester a vocabulary of "good" situations, and the
absence of them has a consequence you should know (it is one of the warnings
in §3.2): with no target markings declared, *every* deadlock of the model is
reported as an error, because there is nothing left that counts as a
legitimate end.

Targets have a complement. Egge's open item said (quoted in its original
wording; the concept is now spelled "off-target" after the rename):

> 5. Optionally, states from which no final marking is reachable as a further repellent region.

That is the **off-target region**: states from which no target marking is
reachable any more. Whatever wanders in there can never come out to a
legitimate end — a run that enters has to be restarted. The colony sets a
modest repellent pheromone on every edge into the region (it is *set*, never
accumulated, and never evaporates — static knowledge about the explored
model), so runs learn to keep out, while still-sought states (an error
marking, a deadlock, a livelock the board is deliberately heading for) and
states that can still reach unknown territory stay excused. A target marking
also decides whether a terminal cycle is a legitimate steady state or a
specification livelock; §6.12 works that through.

---

## 6. The different kinds of errors

"Error" is not one thing here. There are several kinds, from different
sources, with different consequences — a beginner should keep them apart.
The outcome names below are the words the report uses; §10 shows where they
appear.

### 6.1 Findings about the model, before any testing

The loader and the static analysis (§3.2) report problems of the model
itself: unknown or misused keys, no target markings, deadlocks, terminal
cycles, an initial marking inside a livelock. These are not SUT faults — fix
the model or declare the markings that express your intent.

### 6.2 Error markings — `error_reached`

The SUT arrives at a situation the model declares forbidden (`markings.error`
or, matching it, a state the marking covers). The run fails there, the
outcome is `error_reached`, and the detail names the marking — e.g. the
elevator's `jammed`, or the ATM's `card-swallowed`. Per marking you choose
(§2.3): how it matches (`exact`/`covers`), its policy (`until_covered` — an
error until covered `target_hits` times — or `once` — an error every time),
and whether reaching it collects a **post-mortem**: a `snapshot` (the agent
is asked for a state dump) or a `crash` (the SUT is crashed on purpose so its
state can be examined). `max_dumps` (default 1) bounds the dumps across the
whole campaign, so the same error is not collected twenty times.

An important shift of view: an error marking is a *declared* error — the
campaign seeks such situations on purpose (the ants steer toward them) until
they are covered, because finding them is the point of testing. Once its
policy says it is covered, it becomes a **satisfied target** and its states
are forbidden; by default the suggester then refuses to enter them again
(§8.5 shows the `--satisfied-target-policy` relaxation).

### 6.3 Error transitions

The same idea one level smaller: a single transition can be forbidden
(`"error": true` on the transition, optionally with `"crash_dump"`). It is
caught when the step is *certainly* taken, exactly like an error marking;
the detail names the transition. (A `policy` key on a transition currently
has no effect and draws a warning.)

### 6.4 Deviations — `deviation`

The classic model-based bug: the SUT does something the model cannot explain.
The run observes an event no enabled transition can produce — the SUT is
contradicting the specification. The run ends immediately. From the real
output of a campaign against `drink-machine` with a simulator that misbehaves
after two events:

```
  run 1    deviation  3 events, 2 inputs, +3 states +2 edges  observed "printer.jam", which the model does not know (expected: coffee.served)
  run 2    deviation  3 events, 2 inputs, +1 states +1 edges  observed "printer.jam", which the model does not know (expected: tea.served)
```

Read it as a dialogue: the tester waited for `coffee.served`, the SUT said
`printer.jam`. Either the SUT is wrong or the model is incomplete — the
report gives you both halves of the question. `--dump-on-deviation` can
collect a post-mortem at the moment of the contradiction.

### 6.5 Run-time deadlocks — `deadlock`

The SUT goes silent — and silence is meaningful, because the tool reports
**quiescence**: after `--quiescence` milliseconds without any event (default
500), the agent says "nothing more will come". If the reached markings are
deadlocks of the model, the run is stranded exactly where the model says
nothing can happen: a run-time deadlock. The outcome is `deadlock`;
`--dump-on-deadlock` collects a post-mortem. A silent SUT where the model
expects output is *not* this outcome — §6.12 sorts the four terms out.

### 6.6 Run-time livelocks — `livelock`

The SUT keeps *talking* but nothing progresses: no new state, no stimulus, no
target marking, event after event. With `--livelock-timeout MS` (default 0 =
off) and at least `--livelock-min-events` events (default 3) in the window,
the run ends as a livelock. Real output from `heartbeat-livelock`, whose SUT
can only tick and tock:

```
  run 1    livelock  315 events, 0 inputs, +2 states +2 edges  314 events over 400 ms without progress (no new state, no stimulus, no target marking)
```

This is a run-time heuristic. The static terminal-cycle finding and the role
of targets and milestones in it are §6.12.

### 6.7 SUT crashes — `sut_crashed`

The SUT dies, or the connection to it breaks. The run ends as `sut_crashed`.
The same outcome occurs *on purpose* when a post-mortem policy says `crash`
(§6.2): the tester crashes the SUT at the error to collect its state. Either
way the way in is taught to the colony as a catastrophe (§6.10) — unless the
crash is still being *reproduced*: a crash is treated as a target until the
experiment behind it is certain it is deterministic, and while it is, the way
in is marked attractive instead (§6.11).

### 6.8 Aborts — `aborted`

Infrastructure problems, not (necessarily) the SUT's fault. The run aborts
when the event queue overflows (events were lost), when a sent input is never
acknowledged (`--ack-timeout`, default 5000 ms), when the SUT stays silent
while it owes output (`--idle-timeout`, default 10000 ms), or when the agent
cannot start or faults mid-run. The outcome is `aborted`; a campaign that
aborts too many runs in a row stops itself ("too many aborted runs in a
row") and the tool exits with the infrastructure code 3. Reaching
`--max-events` is not an abort: the run ends normally with the detail "run
length limit" (§12.10).

### 6.9 "Not complete" — a status, not a fault

A campaign that ends without covering everything it explored is not a fault
of the SUT — it is a coverage status. Normally the tool exits 0; with
`--require-complete` you can enforce completeness (for a CI pipeline) and the
tool exits 4 instead.

### 6.10 Catastrophe pheromones — what a found error teaches

Every fault found in the SUT (a crash, a run stranded in a deadlock) marks
the **way in** as repellent on the reachability graph: the base deposit is
3.0, *priced by the way in* — it grows by 0.25 per input the run spent
getting there, counted up to 8 inputs, all capped at the pheromone ceiling
`tauMax` 10. A crash one input away is marked 3.0 × (1 + 0.25) = 3.75; a
crash eight inputs deep is marked 3.0 × (1 + 0.25 × 8) = 9.0. The reasoning
is the wall clock: reaching a deeper error state costs more campaign time,
so its way in deserves harder repel. `--flat-catastrophe` keeps the deposit
at 3.0 always, and a way in that crosses an `"unpredictable"` (human)
transition is flat too — a human's reaction time says nothing reproducible
about the SUT. The same lessons also land in the (marking, T-suffix) pairs of
§7.1 and are rebuilt, toned down, from the run log (§7.3). One exception
exists since the crash predictions: a crash that is still being *reproduced*
teaches the opposite while it seeks — the way in is marked attractive,
because the campaign must come back and reproduce the crash (§6.11).

### 6.11 Crash predictions — a crash is a target until it is certain

Sometimes an SUT crash is *nondeterministic*: it happens randomly, rarely,
sometimes, or often — but not always. Any of these indicate a bug. So the
model can **predict** such a crash, with its probable class, and the campaign
treats the crash as a **target**: it keeps coming back to the place, counts
how often the crash actually happens, and only stops when the experiment
behind the prediction is *certain* the crash is deterministic — or when
trying further is pointless.

**Where you declare one.** On a transition (a crash while handling that
stimulus) or on a target or error marking (a crash while the SUT is certainly
in that situation):

```json
{"id": "strike", "kind": "controllable", "label": "strike",
 "crash": "sometimes"}
```

The value is a **class word** — `random`, `rare`, `sometimes`, `often`,
`always` — a bare number (`"crash": 0.7` is shorthand for `{"p": 0.7}`, your
own estimate of the probability), or an object with everything at once:

```json
"crash": {"class": "rare", "p": 0.3, "attempts": 7,
          "crash_dump": "snapshot", "max_dumps": 2}
```

`crash_dump` and `max_dumps` are the post-mortem machinery of §6.2: each dump
a prediction takes counts against its own budget. The step must be **visible**
to the tester (an untraced internal step can never be attributed), an error
transition cannot carry one (it fails certainly, not probably), and neither
can an initial marking (nothing is observed before the campaign starts the
SUT there) or a policy `avoid` marking (what the tester avoids is never
sought).

**The experiment.** While the prediction is unresolved, every definite visit
of the predicted place is one **attempt**, and every death attributed to the
prediction is one **crash** — including the visit the dying step itself made,
counted exactly once. The tool re-evaluates the prediction after every count.
Behind the numbers sits a small Bayesian estimate of the crash probability —
Egge's wording: "Credibility level on a binomial distribution." — with the
class word as the prior. It reaches one of three ends:

- **deterministic** — every attempt so far crashed (a deterministic crash has
  no misses) *and* the credibility is high enough (`--crash-level`, 0.95 by
  default) that the crash probability is at least 0.90 in the future. The
  crash is now treated like any other found fault: the way in is marked
  repellent (§6.10) and the campaign moves on.
- **exhausted** — the give-up budget ran out without certainty. The class
  word sets the budget, a stronger claim needing fewer reproductions:
  `always` 2, `often` 4, `sometimes` 8, `rare` 20, `random` 20; `"attempts"`
  in the prediction overrides it. With at least one crash the bug is real:
  treated like a crash. With none it never showed: **unobserved** — the
  campaign stops seeking and teaches nothing.
- **seeking** — still a target: the way in is marked *attractive*, because
  the campaign must come back and reproduce the crash.

A crash **nobody predicted** — the SUT simply died somewhere — gets an
**implicit** entry at the last step the run certainly credited: the neutral
class, the budget of `--crash-attempts` (default 6). It goes through the same
experiment. Exhaustion can land in the middle of a run (the status is
re-evaluated after every count), so a campaign that dies on every run reads
"6 attempt(s), 5 crash(es)" at its budget: the crash that resolved the
experiment went uncounted.

**Reading the report.** The console (and `eggembt report`) prints a `Crash
predictions` table after the run lines — per crash site its prediction, the
status of its experiment (`seeking` / `deterministic` / `exhausted` /
`unobserved`), its attempts, crashes and dumps. Every attributed crash also
leaves a run note ("the crash was attributed to t:strike (deterministic: 1
attempt(s), 1 crash(es))"). The two knobs are `--crash-level L` (the
credibility, 0.5–1.0; 1.0 would make certainty unreachable) and
`--crash-attempts N` (the budget for crashes nobody predicted). The
experiment state travels with the run log, so a continued campaign picks the
experiments up where they stood.

### 6.12 Specification vs SUT: deadlock, livelock, and the cycles in between

`deadlock` and `livelock` are the oldest words in the tool and the easiest to
misread, because each names two different things: a shape in the **model**
(the specification and its reachability graph), and a behaviour of the **SUT**
in one run. The report uses each word for only some of those combinations.
This section separates them, then follows the consequences through the
strongly connected components of the graph.

| term | level | meaning in one line |
|------|-------|---------------------|
| **specification deadlock** | model | a reachable ordinary marking with no enabled transition |
| **SUT deadlock** | run | the real system stops while the model still expects progress |
| **specification livelock** | model | a terminal cycle without a target marking — the model may spin forever |
| **SUT livelock** | run | the SUT keeps sending events, but a run makes no progress for the timeout |

The outcome `deadlock` is the narrow overlap of the first two: the SUT is
silent *and* the model agrees that nothing can happen. The outcome `livelock`
is the fourth, measured against a timeout. The static findings appear in
`check` and `explore`, not as run outcomes.

**Specification deadlock.** A specification deadlock is a reachable marking
of kind `Normal` in which no transition is enabled. "Normal" is the technical
part: a marking that matches a target marking, or an error marking, is not
ordinary. It has a declared meaning even when nothing can fire from it, so it
is not a deadlock. If a marking matches both a target and an error, the error
wins. The graph reports a deadlock only for a state that was actually
expanded; a cut-off frontier from `--max-states` is never mistaken for one
(§3.3).

`check` prints the count (the `deadlocks: 0` line in §3.2 is this line for a
clean model), `explore` lists the states, and both do this before any SUT is
touched. A static finding is model advice, not a run failure: `check`
returns 0. The campaign turns **all** deadlocks of the graph into **one**
target, because a dump and a restart are expensive whichever dead end is
reached.
`--deadlock-policy` defaults to `once`: the campaign steers into a deadlock
once to confirm the SUT can reach it, then marks its states forbidden and
plans around it (§8.5). Only if the SUT is quiescent exactly in such an
ordinary dead end does the run end as `deadlock`, with the detail "the SUT is
quiescent in a state where the model has a deadlock". That outcome is a
failure exit (1); `--dump-on-deadlock` collects a post-mortem.

**SUT deadlock.** This is the everyday phrase, not a report status: the real
system stops making progress. What the tool reports depends on what the model
expected at that moment:

- **The model is deadlocked too.** The quiescence report narrows the belief
  to ordinary dead ends, so the run ends as `deadlock` — the model agrees.
- **Every possible state requires output or an internal step.** Silence is a
  contradiction, not a deadlock: the run ends as `deviation` with "no output
  arrived, but the model requires one in every possible state". This is the
  usual face of a hung SUT.
- **No quiescence report arrives at all.** After `--idle-timeout` (default
  10000 ms) the run ends as `aborted` with "no event from the SUT for ... ms
  although one was expected". Infrastructure problem by the tool's
  classification, not a specification deadlock.
- **The model waits for an input.** Silence is normal here; the SUT is
  waiting for the tester. The campaign sends the next input. If the SUT never
  acknowledges it, `--ack-timeout` ends the run as `aborted`.
- **The belief is ambiguous.** The `deadlock` verdict requires every state
  still possible after the quiescence report to be an ordinary dead end. One
  remaining target state, one remaining error state, or one enabled
  controllable transition there prevents it.

Read `deadlock` as "the SUT stopped exactly where the model said nothing could
happen", not as "the SUT stopped".

**Specification livelock.** The static form is a **terminal cycle**: a
strongly connected component (SCC) that

1. contains a cycle — more than one state, or a single state with a self-loop;
2. cannot be left — no edge, controllable or not, goes out of it; and
3. contains no target marking.

A **strongly connected component** is a maximal set of states in which every
state can reach every other. Contract each SCC to one node and the graph
becomes a DAG; a *terminal* component is a sink of that DAG. A cycle never
crosses components, so it always lies inside one SCC. `explore` reports the
counts and the findings:

```
Reachability graph
  2 states, 2 edges, 2 expanded
  0 target, 0 error states; 1 strongly connected components, 1 with a cycle

Deadlocks  none

Livelocks  1
  terminal cycle, 2 state(s)
    tick:1
    tock:1
```

This is `examples/heartbeat-livelock.json`: the model can tick and tock
forever and has no target marking. An ordinary single state with no enabled
transition is a deadlock, not a livelock; a single state with a self-loop
*is* a cycle and can be one. A non-cyclic component is never reported.

The finding means "the specification permits endless behaviour that never
reaches a declared legitimate end". That can be a modelling mistake or a
deliberate design. The campaign warns only about **certain** findings: the
analysis works on the explored graph, and a component is uncertain when one
of its states is incomplete or had a firing dropped by a place bound. An edge
into an unexpanded state counts as an exit, so a component next to unexplored
territory is not called terminal. `check` counts every finding; `explore`
marks the uncertain ones "not certain: exploration was incomplete".

For the campaign, each certain terminal cycle is a **livelock target** of its
own. `--livelock-policy` defaults to `once`: seek a definite visit, then
forbid the states. This is coverage of a known terminal cycle, not a verdict about the
SUT; a run that enters the cycle and follows it is still doing exactly what
the model allows. If the reachable graph is one strongly connected component
and declares no target at all, the whole graph is one terminal cycle (the
exception is a single dead state, which is the deadlock case) — which is why
that warning is so common. An initial marking inside such a cycle gets its
own warning: every run starts in the cycle and cannot avoid it.

**SUT livelock.** The run-time form is a judgement about one run: the SUT
keeps producing events, but nothing in the run progresses. It is off by
default because periodic behaviour can be legitimate. With
`--livelock-timeout MS` greater than zero, and at least
`--livelock-min-events` observed events (default 3) since the last progress,
the run ends as `livelock`:

```
  run 1    livelock  315 events, 0 inputs, +2 states +2 edges  314 events over 400 ms without progress (no new state, no stimulus, no target marking)
```

Progress is one of four things: a stimulus was sent, the belief contains a
state not seen earlier in the run, the belief may be in a target marking, or
the SUT fell quiet. Only observed events count towards the window; a
stimulus or a quiescence report resets it. The outcome is a failure exit (1),
and `--dump-on-livelock` collects a post-mortem.

A SUT livelock does **not** require a specification livelock. The SUT can
refuse to take an exit that the model offers, spinning in a perfectly
non-terminal SCC; the static analysis will say nothing about it, while the
timeout notices. Conversely, a specification livelock does not force a
`livelock` outcome: with the timeout off, every event can be legal, the run
ends for some other reason — a run limit (`--max-events`), a stop, a
deviation elsewhere — and the report never says `livelock`. The static
finding and the run-time outcome answer different questions.

The run-time test is a heuristic. It deliberately treats "the belief may be
in a target marking" as progress, because a target is where the campaign
wants to be — but only a target state in the *current belief* counts, not a
target somewhere in the same SCC. A SUT looping among non-target states of a
target-containing SCC can therefore still be flagged if it never actually
reaches the target. And a slow but honest SUT can be declared stuck; choose
the timeout longer than any legitimate autonomous activity (§12.6).

**What an SCC means for the campaign.** Three questions decide everything,
and they are independent: *is the component cyclic?*, *can it be left?*, and
*does it contain a target marking?* A milestone never enters this test. In
one sentence: a cyclic SCC is a livelock only when it is terminal and has no
target marking; a target legitimizes it, a milestone does not, and a
non-terminal SCC is never a livelock.

| SCC | target inside? | static finding | consequence |
|-----|----------------|----------------|-------------|
| cyclic, has an exit | either | none — not terminal | an ordinary loop; the run-time watch may still flag a SUT that refuses the exit |
| cyclic, terminal | no | specification livelock | warning; a certain one is a livelock target, sought once and then forbidden |
| cyclic, terminal | yes | none — legitimate steady state | not a deadlock; a plain target does not end a run, an intentional one ends the campaign |
| one state, no edge, nothing enabled | no, `Normal` | specification deadlock | deadlock target; a quiescent SUT there ends the run as `deadlock` |
| one state, no edge, nothing enabled | target or error | none | a target is a legitimate end; an error is `error_reached`; neither is a deadlock |
| one state, only a self-loop | no | specification livelock | not a deadlock (the self-loop is enabled); same livelock-target handling |

**Targets.** A target marking inside a terminal cycle legitimizes the whole
component: the model says being there is the point, so the cycle is a
legitimate steady state and the static analysis stays silent. This is true
for a plain target as well as an intentional one. The difference is only what
happens in a run: a plain target never ends a run, while an intentional
target ends the whole campaign when the SUT is *certainly* in it (§5). The
static test does not care which; it asks whether a legitimate marking exists,
not whether the campaign will stop there. The test is local: only a target
state *inside* the component counts. A target elsewhere does not legitimize
a terminal SCC, and if an edge leads there the component has an exit and is
not terminal in the first place.

Some consequences are easy to miss. First, a target state is never a
deadlock, even with no outgoing edge, because `isDeadlock` requires kind
`Normal`. Second, the run-time watch only sees a target that is possible
*now*: being somewhere in a target-containing SCC does not suspend it. One
edge case: if a marking matches both a target and an error, the error wins,
so it does not legitimize anything.

**Milestones.** A milestone is a **transition** annotation, not a marking:
`"milestone": true` or a rank. A certain step through it deposits positive
pheromone (1.5 × rank) around the belief, is listed in the run and coverage,
and the run continues. It is a success on the way, never an end.

Because the terminal-cycle test only asks for `StateKind::Target` among the
component's states, a milestone **cannot** legitimize a terminal cycle. A
targetless terminal cycle with a milestone in it is still a specification
livelock: the warning stays, the campaign still treats it as a livelock
target, and the milestone's attract deposit competes with that lesson. Once
the livelock target is satisfied, its states are forbidden; by default the
suggester will not plan into them, and an ant that arrives there dies, so
the milestone's positive deposit does not win. If the loop is legitimate,
declare a target marking in it — a milestone is not a substitute.

A milestone in a **non-terminal** cycle is harmless in this respect: the
component has an exit, so there is no livelock finding, and the milestone is
an ordinary reward on the way. The run-time watch has no milestone category
either: the first step into a new state counts as progress because the state
is new, but looping over the same milestone states forever does not.

**Internal cycles.** One more SCC consequence is optional. The
`explore --internal-cycles` option also reports cycles made only of internal
transitions (divergence: the system can spin without any visible progress).
It is off by default because environment-like internal behaviour that
toggles back and forth is legitimate in many models. These findings are not
campaign livelock targets; they are a diagnostic. The ordinary terminal-cycle
test still counts internal edges like any other edge.

**Reading a report.** The short version:

- `check` says "deadlocks: N": the model has ordinary states with no move. If
  such a state is a legitimate end, declare a target marking there; otherwise
  add the missing transition.
- The `check` line "terminal cycles without a target marking (livelocks)"
  counts above zero: decide whether the loop is a legitimate steady state.
  If yes, put a **target marking** in it — a milestone is not enough. If no,
  break the cycle or add the missing output or exit. An initial marking
  inside it is urgent.
- Run ends `deviation` with "no output arrived": the SUT stopped where the
  model expected output. That is an SUT deadlock in the everyday sense, but
  the report calls it a deviation.
- Run ends `aborted` with "no event from the SUT": silence outlasted the idle
  timeout before any quiescence verdict; infrastructure by classification.
- Run ends `deadlock`: the SUT stopped exactly at a model deadlock; the model
  agrees, and the run is failed there.
- Run ends `livelock`: the SUT kept talking without progress long enough.
  Check the timeout first — it is a heuristic. A static terminal cycle is a
  likely place for it, but neither necessary nor sufficient.

---

## 7. The ant colony

### 7.1 The idea, and what it does

Real ants find food without a plan: they wander, and they lay pheromone on
the ground as they walk. A path travelled by many ants smells stronger; more
ants choose strong-smelling paths; the smell fades over time, so abandoned
paths stop attracting. Out of many blind walks and one simple rule — *follow
the smell, and strengthen what you use* — a colony finds good paths.

eggembt runs such a colony over the reachability graph. Every iteration
(default 20 ants, each up to 40 steps), an ant walks from a start marking and
chooses among the edges by a weight that mixes three things:

- **pheromone** — the attract channel raises an edge's weight; the repel
  channel and the static off-target region lower it;
- **novelty** — *unexplored territory smells interesting*: every edge's
  weight carries its own novelty, `1/(1 + hits)` (0.01 when the edge is
  saturated), and an ant that reaches a state with an uncovered outgoing edge
  — or one that cannot be expanded within the round's budget — deposits 1.0
  there and ends its walk, the deposit back-propagating along its way in;
- **offered versus taken** — one of Egge's open items, quoted verbatim:

  > Record how often each SUT move is offered versus taken, and feed it to the ants and the robustness estimate.

  The tool counts, per label, how often the SUT was *offered* a move (the
  session waited for it) and how often it was actually *taken*; the take
  rate weights the ants' environment moves — an observable transition the
  SUT rarely takes is walked by the ants rarely too.

The choice itself, written out with the defaults in brackets: an ant first
decides between the SUT's own moves and the tester's inputs (probability 0.5
each when both exist), then draws from that pool in proportion to weight.

    weight(edge) = (attract + 0.1)^alpha · novelty^beta · exp(-lambda · (repel + region))
    novelty      = 1 / (1 + hits)        (0.01 when the edge is saturated)
    alpha = beta = lambda = 1            (the defaults)

Environment moves use the observed take rates in place of the formula, and
the (marking, T-suffix) layer (§7.2) adds its pair channels to `attract` and
`repel` for input edges. A round may expand at most 200 new states.

What the ants find changes the landscape the next real run is steered by:

- **sought targets** deposit 2.0;
- **milestones** deposit 1.5 × rank (§2.3), once per definitely credited
  step — a success on the way, not an end;
- **catastrophes** repel, priced by the way in (§6.10);
- the **off-target region** is set to 1.0 on every edge into it (§5);
- **back-propagation** marks the way *in* to any of these — at least the
  previous 5 nodes, fading by 0.7 per step of distance — so the approach to
  a good state is advertised, not just the state itself;
- everything **evaporates** per iteration: attract loses 10%, repel loses
  2%; no deposit exceeds the ceiling `tauMax` 10;
- edges covered to the threshold are **saturated** and lose their attraction
  — the colony moves the campaign on instead of flogging covered ground.

The colony steers through the **suggester**, the part that decides which
input the next run sends; `--seed N` seeds the run's random tie-breaks
(0 keeps the deterministic order; the same seed reproduces a campaign) and
`--ant-seed N` seeds the colony's walks. `--no-ants` switches the colony off,
leaving coverage-only guidance.

### 7.2 The T-suffix — (marking, T-suffix) pairs as a finer pheromone basis

Pheromones keyed by *marking* alone have a blind spot: all arrivals at one
situation are lumped together. A catastrophe suffered *via one route*
condemns that marking for *every* route. The T-suffix is the refinement —
Egge's text, verbatim:

> Yet another feature, which can be optionally on or off: To each marking, add a list of truncated traces, each of which is called a T-list, from which we arrived to the marking. The length of the list is N. By default, N is 4. The Nth element of the T-list is a mini-hash created from the current trace. The (marking, T-list) pairs are now the basis for pheromone generation instead of mere markings.

The quote keeps the word of its time; the feature is called **T-suffix**
since v0.0.2, and this manual uses that name (`journal.md` §27 records the
rename).

In plain words: every marking now remembers *how you got there* — not the
whole trace, but a small list of fingerprints of its recent steps. The
**T-suffix** of an arrival is that list; a **(marking, T-suffix) pair** is "this
situation, arrived at this way". Pheromones can then be kept per pair, so
learning becomes **route-sensitive**.

**The element of the list is the suffix cascade.** A run's trace is its
sequence of steps s_1, …, s_m, each hashed by `stepHashOf()` of the
transition that fired — the transition's label if it has one, else `"I:"` +
transition id for a silent internal step, hashed by FNV-1a (`stepHash`).
With `kTraceSeed` (0x5EED5EED) as the hash of an empty suffix, element k
after m steps is

    H_k(m) = mix(H_{k-1}(m-1), s_m)     for k = 1..N,     H_k(0) = kTraceSeed

where `mix` folds one step into a 32-bit carry: xor with the step, multiply
with 0x9E3779B9, xor-shift 15, multiply with 0x85EBCA6B, xor-shift 13,
multiply with 0xC2B2AE35, xor-shift 16 — a Murmur-style final avalanche,
written out so every platform and replay produces the same values. So
H_1(m) = mix(kTraceSeed, s_m) depends only on the last step, H_2(m) on the
last two, …, and H_N(m) is the mini-hash created from the current trace,
truncated to its last N steps — the Nth element of the T-suffix. Two arrivals
at a marking share the first j elements of their T-suffixes exactly when their
traces' last j steps agree. The recurrence lets one rolling pass per
arriving step maintain the whole list (`h_[i]` holds element i+1):

    carry = kTraceSeed
    for i in 0..len-1:
        older = h_[i]               // h_[i] held H_{i+1}(m-1)
        h_[i] = mix(carry, s_m)     // becomes H_{i+1}(m)
        carry = older               // H_{i+1}(m-1) is H_i(m-1) for the next i

One arriving step updates all N elements in O(N); pricing a candidate edge
extends the list by the edge's step — the same pass on a copy, O(N) per
candidate. At reset (a run's begin) all N elements are kTraceSeed, and a
trace shorter than an element's length hashes deterministically: with m
steps taken, element i+1 has been updated min(i+1, m) times and holds the
hash of the whole trace so far — the missing deeper history contributes
kTraceSeed, the hash of nothing. Entries at or beyond the configured length
stay kTraceSeed, so a pair key is determined by the length alone.

A concrete example from `drink-machine`. The marking `idle` can be reached
by two routes:

- trace A: `coin.insert`, `button.coffee`, `coffee.served`
- trace B: `coin.insert`, `button.tea`, `tea.served`

Element 1 hashes only the last step — `coffee.served` for A, `tea.served`
for B — so the two arrivals have *different* pairs even though the marking
is the same. Two arrivals whose traces end in the same steps share exactly
those elements of their lists.

What it changes mechanically: with the layer on (`--tsuffix N`, 0..8; 0 = off,
the default — Egge's "By default, N is 4" is `--tsuffix 4`), the session's
milestone and catastrophe deposits also land in a per-pair store under the
arrival pair — flat, capped at `tauMax`, the same amounts as the edge
deposits, evaporating with them, pruned when both channels fall to nothing.
The ants and the suggester weigh an edge under the pair of the arrival
context it would create, so a catastrophe suffered via one route repels only
that route. The edge channels stay exactly as they were: the pairs are a
finer layer *beside* them. And a continued campaign replays the recorded
steps over the same cascade, so the rebuilt pair keys match the live ones.

### 7.3 The carry-over — several rounds saved, merged, toned down, optionally aged

The feature as Egge phrased it (it arrived as a "Feature:" line; the
sentence itself is verbatim):

> Pheromones from several rounds are saved and merged (and toned down in the next one).

Three claims, each already true of a part of the tool, now true together:

- **Saved.** Every run of every round is appended to the run log (`--log
  FILE`) — not as pheromone state, but as *recordings*: the initial marking,
  the steps taken (or the live events), the outcome, the dumps. This closes
  an open item, quoted verbatim:

  > Persist pheromone, or rebuild it from the run log.

- **Merged.** A continued campaign replays all earlier rounds' runs. Their
  coverage merges immediately, and each recording's pheromone teaching is
  *rebuilt* onto the same edges and pairs — deposits accumulate, exactly as
  they did when the runs were live. Several rounds merge; nothing is
  averaged away.

- **Toned down.** Every deposit the rebuild lays — milestones,
  catastrophes, and the (marking, T-suffix) pairs alike — is scaled by
  `--restored-pheromones N` (0..100, default **50**). The reasoning:
  inherited knowledge is second-hand and possibly stale — the SUT may have
  changed since the earlier round — so a fresh round should weigh what it
  learns itself higher and keep exploring. `100` keeps the old full
  strength; `0` ignores the inherited teaching entirely. The factor applies
  *per deposit, before the ceiling*, so a much-marked edge still ends capped
  at the same ceiling while the single-visit marks around it fade. And it
  does **not compound** across rounds: the log stores recordings, not
  pheromone state, so every continuation re-derives the knowledge at log
  strength times the factor — a three-round-old run contributes exactly the
  same toned-down deposit as a one-round-old one.

Example with the pinned numbers: a rank-2 milestone deposits 1.5 × 2 = 3.0
live; rebuilt into a continued campaign at the default 50 % it deposits
1.5. Two restored runs of the same milestone merge to 1.5 + 1.5 = 3.0 —
merged as before, each toned down.

**Aging the merge.** The toned-down merge above freezes every deposit at its
rebuilt strength. If the earlier runs are many, or the SUT has moved on, the
old teaching should fade instead — but not everything should fade equally.
`--evaporate-restored-pheromones` (default **off**) lets the rebuilt
pheromone age once at every boundary between restored runs:

- The order is chronological. Before the second restored run is replayed,
  the teaching rebuilt from the first run ages once; before the third, the
  teaching of the first two ages once; and so on. The newest restored run
  therefore enters at full rebuilt strength, and an older run has faded by
  one step for every later restored run.
- The step is the colony's ordinary aging step: attract loses 10 %, repel
  2 %, and a saturated edge loses its attract. `--restored-pheromones N`
  still applies to every rebuilt deposit first; the merge aging is on top of
  that factor.
- **Matured findings are spared.** A target, error marking, deadlock or
  livelock whose phase is `satisfied` (§5) has reached its "visited N
  times" criterion. The pheromone deposits that lead into such a state —
  the incoming edge channels and the `(marking, T-suffix)` pair entries —
  do not evaporate. The direct way in keeps its lesson for later runs,
  while the weaker back-propagated traces behind it still age. Saturation
  is not evaporation: a saturated edge's attract is still wiped.
- **What is not matured.** A milestone is a transition reward, not a target
  with a visit count, so its attract ages normally unless it lies on the
  way into a state that is itself matured. A crash experiment
  that is still `seeking` is not `satisfied` either: its way in ages until
  the experiment resolves. An error marking, deadlock, livelock or
  intentional target marking is matured by its own policy count —
  `once`, `until_covered`/`target_hits`, or `avoid` (count 0). Plain,
  non-intentional target markings are idle states for the livelock
  analysis, not campaign targets, and are not spared.

With the switch off, the historical merge is unchanged: no aging happens
between restored runs. The switch concerns only the merge. It does not
change the live campaign's aging (`--evaporate-between-runs`, §7.4), and
`report` keeps rebuilding the log without it. If the model's static
analysis is enabled (`--no-analyze` is not given), a merge with this switch
runs it before the restored runs, so terminal livelocks already have their
target findings when maturation is checked.

### 7.4 Evaporation — per suggestion or at the run boundary

The pheromone is not permanent. Every suggestion the tool makes ages the
colony once: the attract channel loses 10 %, the repel channel 2 %, and an
edge that is already saturated loses its attract outright. That is the
historical setting and it stays the default.

The repel channel's 2 % is the one that matters for a lesson learned the
hard way: a catastrophe. Its half-life is about 34 suggestions. In a short
run that is plenty — the next run starts with the lesson still loud. In a
long run it is not: a run that makes 200 suggestions keeps only
0.98^200 ≈ 1.8 % of what the beginning of the run taught. The benchmark in
`tst_pheromone` (§12.1 repeats its numbers) makes this concrete: on the
1000-state diamond, six runs of 200 events crash the SUT **three** times and
cover 586 edges when the colony ages per suggestion; with
`--evaporate-between-runs` they crash **twice**, keep four runs productive
and cover **777** edges.

A sweep of the rate itself (`journal.md` §31) finds the same outcome from 0.005
to 0.10 per suggestion on this benchmark: the fork is reached only about
five suggestions after the lesson is laid, so the short interval survives
even a fast decay, and the pair layer plus the safe route's attract protect
the choice twice. The per-suggestion default is on a plateau for this shape,
not on a cliff; §12.3 explains the timing and what would move it.

`--evaporate-between-runs` changes *when* the aging happens, not whether the
ants walk: at every suggestion the ants still walk and still inspect the
pheromone, but neither channel ages. Once a run has finished — its final
settle included, so every deposit has landed — the colony ages exactly
once. A run, however long, then costs the lesson one 2 % step instead of
one per suggestion.

Which one to use? The per-suggestion aging is right when the campaign is
made of many short runs: a lesson from run *k* reaches run *k+1* almost
undiminished anyway, and the quick decay keeps the colony responsive to a
changing SUT. The boundary aging is right when runs are long and the lesson
must survive from one run to the next — which is exactly the case the
benchmark measures. The automatic test
(`agingCanWaitForTheRunBoundary`, `tst_ants`) pins the mechanics: 200
walking iterations without aging leave a deposit at exactly its value, one
`age()` leaves exactly 98 % of it, and 200 per-suggestion agings leave
1.8 % — the arithmetic behind the difference above.

The flag is a campaign switch, not a model property, and `report` is not
affected: a rebuilt campaign lays the recorded deposits and does not
simulate the aging history of the live one (it never did).

---

## 8. Using the tool

This chapter assumes `eggembt` is on your `PATH`. How to build, test and
install it — prerequisites, `cmake --install`, platform notes, troubleshooting
— is the subject of `INSTALL.md`.

### 8.1 The commands

```
Usage: eggembt <command> [options]

Commands:
  check MODEL      validate a model and show what it describes
  explore MODEL    explore the reachability graph: deadlocks, livelocks, Graphviz output
  run MODEL        test a system (an agent, or the built-in simulator) against the model
  report MODEL     replay a run log and show the coverage reached so far
  compare MODEL    mutual overlap of two run logs' coverage (Jaccard, CSV for plots)
  export MODEL     write the model as standard PNML, with the eggembt extras as comments
  agent-sim MODEL  serve a simulated SUT over standard input/output (an agent for 'run')
  version          print the version
  credits          print the credits
  help [COMMAND]   this text, or help for one command
```

Exit codes (the help states them; they are exact):

| code | meaning |
|------|---------|
| 0 | ok |
| 1 | faults found — a run ended `deviation`, `deadlock` or `livelock` |
| 2 | usage or model error (bad arguments, a model that does not load) |
| 3 | infrastructure problems (agent, connection) |
| 4 | coverage incomplete, with `--require-complete` |

Note the split: `error_reached` and `sut_crashed` do **not** exit 1 by
themselves — those are situations the campaign *seeks* (declared errors,
post-mortems). The faults that fail the process are the ones where the SUT
contradicted the model or hung.

### 8.2 Choosing the SUT

`eggembt run MODEL` needs exactly one of:

- `--simulate` — the built-in simulated SUT, which conforms to the model.
  Start here: it proves the model end to end with no system at all.
- `--agent "CMD ARGS"` — start a program that speaks the protocol on
  stdin/stdout (a wrapper around your SUT; `examples/agents/ref_agent.cpp`
  is a worked C++ example — Egge's item, verbatim: "A reference agent shim
  for C++ SUTs." — and `eggembt agent-sim MODEL` serves the simulated SUT as
  such an agent).
- `--connect HOST:PORT` — connect to an agent that listens.
- `--listen [ADDR:]PORT` — wait for an agent to connect
  (`--accept-timeout SEC`, default 60).
- `--stdio` — speak the protocol on the tool's own stdin/stdout.

A plug-in may also offer **synchronized events**: it advertises the capability
in its hello (`"sync": true`), and when a run is synchronized it holds the SUT
at a marked event until the tracker acknowledges it. `--simulate` and
`eggembt agent-sim` can do this; an agent that cannot keeps the run
unsynchronized, and the run notes why. The model marks the events with
`"sync": true` and the campaign opts in with `--sync-events` (§2.3, §8.3).

**What a plug-in must contain.** An agent is a process that speaks the line
protocol in `docs/PLUGIN_PROTOCOL.md`: it sends `hello`; answers `start_run`
with `run_started`; acknowledges every `stimulate` with a `stimulated`
event; reports everything the SUT does that the model names as `observed`
events, in capture order; sends one `quiescent` event after the run's
silence timeout; sends exactly one `Died` if the SUT dies on its own; answers
`post_mortem`; and cleans up on `stop_run`/`shutdown`.
`examples/agents/ref_agent.cpp` is a complete worked wrapper around a
hand-written coffee machine (built on the generic `AgentServer`), and
`eggembt agent-sim MODEL` serves the built-in simulator behind the same
protocol. In-process plug-ins implement `ISutPlugin`
(`src/engine/SutPlugin.h`) instead. The protocol document's §5 is the
author's checklist.

**Missing or extra events.** The tracker treats a missing report and an
unexpected report differently. The full table is in
`docs/PLUGIN_PROTOCOL.md` §3, under "Missing or extra messages"; in short:

| the plug-in ... | the tracker ... |
|---|---|
| never answers `start_run` | aborts the run ("did not answer start_run in time") |
| never acknowledges a `stimulate` | aborts after `--ack-timeout` (default 5 s) |
| reports neither output nor quiescence while output is due | aborts after `--idle-timeout` (default 10 s) |
| reports quiescence although every possible state requires output | ends as `deviation`: "no output arrived ..." |
| reports an unknown or disabled observed event | ends as `deviation`, naming the expected labels |
| reports an unsolicited or duplicate stimulated event | ignores it as preempted; the belief is unchanged and the run continues |
| reports an extra quiescent event | narrows the belief to the quiet states; if they are all dead ends, the run is a `deadlock` |
| lets the SUT die without a `Died` event | a closed transport is a fault; otherwise the silence trips the idle timeout |
| never answers `post_mortem` | waits 60 s, records the failure; a snapshot run continues without a dump, a crash request still ends the run |
| reports events of an older run or after `stop_run` | drops them |
| floods the event queue | aborts the run ("event queue overflow: events were lost") |
| sends unknown fields | ignores them (the protocol may grow) |
| sends an unknown message type | treats it as a fault and aborts |

### 8.3 The options, with their defaults

Campaign shape:

| option | default | meaning |
|--------|---------|---------|
| `--max-runs N` | 20 | runs per campaign |
| `--max-events N` | 10000 | events per run; reaching it ends the run normally ("run length limit", §12.10) |
| `--threshold N` | 1 | visits after which a state/edge counts as covered |
| `--initial NAME` | all | start only from this initial marking; repeatable |
| `--seed N` | 0 | seed for random tie-breaks; 0 = deterministic order; same seed reproduces a campaign |
| `--no-analyze` | off | skip the static analysis of the model |
| `--max-states N` | 1000000 | limit of the reachability graph (§3.3) |
| `--budget DURATION` | 0 (no limit) | wall-clock budget for the whole campaign: a bare number is seconds, or use `ms`, `s`, `m`, `h` (`500ms`, `90s`, `10m`, `2h`); the campaign stops there as a normal stop (§8.5) |
| `--sync-events` | off | hold the SUT at the model's `"sync"` events until the tracker has handled and acknowledged them (§2.3); needs a plug-in that can acknowledge |
| `--hunt-rare` | off | hunt the transitions the model marks `"rare"`: they outrank every other goal, an enabled rare move gets its `wait-for` as a dwell, and the campaign stays incomplete while one is unseen with attempts left (§8.5) |

Timing:

| option | default | meaning |
|--------|---------|---------|
| `--quiescence MS` | 500 | silence before the agent reports quiescence (§6.5) |
| `--poll MS` | 20 | how long to wait for events per loop |
| `--idle-timeout MS` | 10000 | abort if the SUT is silent while it owes output |
| `--ack-timeout MS` | 5000 | abort if an input is never acknowledged |
| `--livelock-timeout MS` | 0 (off) | run-time livelock detection window (§6.6) |
| `--livelock-min-events N` | 3 | events required inside that window |
| `--hook-timeout S` | 60 | seconds a synchronous command hook may run before it is killed (§9); 1–3600 |

Policies and post-mortems:

| option | default | meaning |
|--------|---------|---------|
| `--deadlock-policy P` | once | `until_covered` \| `once` \| `avoid` — how a static deadlock is treated: seek it until covered / seek it once / never seek it and treat it as a catastrophe from the start |
| `--livelock-policy P` | once | same for static livelocks |
| `--satisfied-target-policy P` | veto | what a satisfied target means to the suggester: `veto` refuses moves into it and plans around it, `penalise` allows it but keeps the hazard penalty, `ignore` treats it as an ordinary state (§8.5) |
| `--dump-on-deviation M` | none | post-mortem on a deviation: `none` \| `snapshot` \| `crash` |
| `--dump-on-deadlock M` | none | … on a run-time deadlock |
| `--dump-on-livelock M` | none | … on a run-time livelock |
| `--crash-level L` | 0.95 | credibility for "certain deterministic" crash predictions (§6.11); 0.5–1.0 |
| `--crash-attempts N` | 6 | give-up budget of the crash experiment for crashes nobody predicted (§6.11) |

Guidance (the ant colony, §7):

| option | default | meaning |
|--------|---------|---------|
| `--no-ants` | off | switch the colony off |
| `--ant-seed N` | 1 | seed of the colony's walks |
| `--flat-catastrophe` | off | catastrophe deposits stay flat instead of growing with the way in |
| `--tsuffix N` | 0 (off) | the (marking, T-suffix) pair layer; list length 0..8 (§7.2) |
| `--restored-pheromones N` | 50 | percent of the earlier rounds' pheromone strength a continued campaign starts with (§7.3) |
| `--evaporate-restored-pheromones` | off | age the rebuilt teaching once between restored runs; deposits into matured targets and errors are spared (§7.3) |
| `--goal-priority P` | the model's `strategy.goals`, else balanced | `balanced` \| `targets` \| `coverage` — whether targets and milestones or fresh coverage should win (§8.5) |
| `--error-priority P` | the model's `strategy.errors`, else balanced | `balanced` \| `seek` \| `avoid` — how much risk on the way into an error is accepted and how hard a satisfied error is avoided (§8.5) |
| `--evaporate-between-runs` | off | the ants still walk at every suggestion, but the pheromone ages once per finished run instead of at every suggestion (§7.4) |

Output:

| option | meaning |
|--------|---------|
| `--log FILE` | append every run to FILE; if it exists, continue from it (§4) |
| `--json FILE` | write a machine-readable report |
| `--coverage-elements` | add every known state and edge with its hit counts to `--json` (§10.5) |
| `--dump-pheromone FILE` | write the colony's pheromone — per edge and per (marking, T-suffix) pair — as JSON (§10.5) |
| `--require-complete` | exit 4 if coverage is not complete |
| `--quiet` | only the final report |
| `--no-color` | plain text |

Inspection (`report` only; §10.5):

| option | meaning |
|--------|---------|
| `--runs` | print every run's recorded steps (a long run keeps its last 20) |
| `--trace N` | print run N's recorded steps in full; N counts from 1 |
| `--dot FILE` | write the graph for Graphviz, coloured by the coverage reached |
| `--tsuffix N` | rebuild the pair store at the campaign's `--tsuffix`, so `--dump-pheromone` can show it |

`report` also accepts `--coverage-elements` and `--dump-pheromone` from the
output table above — the same switches, applied to the rebuilt campaign.

Simulator options (`--simulate` only): `--sim-seed N` (default 1), 
`--sim-weight ID=W` (weight of the SUT's own transition ID, 0 = never),
`--sim-ack-delay MS`, `--sim-step-delay MS` (default 1),
`--sim-deviate-after N` with `--sim-deviation-label L` (make the simulated
SUT misbehave — §6.4's sample used exactly this), `--sim-crash-after N`
(the simulated SUT dies after N events, wherever it is — a crash nobody
predicted, §6.11; predicted stimuli crash it as the model says),
`--sim-no-snapshot`, `--sim-no-crash`.

### 8.4 A workflow that works

1. **Check the model.** `eggembt check examples/drink-machine.json` — fix the
   warnings before testing (§3.2, §6.1).
2. **Prove it against the simulator.** `eggembt run examples/drink-machine.json
   --simulate --quiescence 40` — a model that cannot complete against its
   own simulation will not complete against the real SUT.
3. **Point at the real system.** Wrap it in an agent (or connect to one) and
   swap `--simulate` for `--agent`/`--connect`/`--listen`. Keep the same
   model.
4. **Keep a run log.** `--log runs.log` saves every run; the next
   invocation continues from it (merged and toned down, §7.3). Inspect it
   with `eggembt report`, and compare two logs' coverage with `eggembt compare`
   (Jaccard overlap, §10.6; `--csv` for plots).
5. **Look at what happened.** `report --runs` lists every run's steps,
   `--trace N` one run in full, `--dot` draws the graph coloured by the
   coverage, and `--dump-pheromone` shows what the ants learned (§10.5).

`eggembt report MODEL --log FILE` replays a log and shows coverage plus
**backtraces** (§10.3); `eggembt compare MODEL LOG_A LOG_B` shows how much two
campaigns covered the same things.

### 8.5 Declaring what matters: the strategy and the budget

A campaign is always a compromise: chase the still-sought error, or cover
the fresh edge next door? Re-test the crash (the report matters), or stay
away from it (the SUT is gone)? The model can state its preference, and the
command line can overrule it.

**The strategy in the model.** A root-level object, beside `"places"` and
`"markings"`:

```json
"strategy": {"goals": "targets", "errors": "seek"}
```

Both keys are optional; a missing key means `balanced`. `check` prints the
model's values (`strategy: goals targets, errors seek`), the campaign's
`Guidance` block names the effective ones, and the JSON carries them (§10.2).

`goals` decides what the suggester treats as the goal, and what the ants
deposit for:

| value | what changes |
|-------|--------------|
| `balanced` | the historical weights: targets and fresh coverage close together |
| `targets` | targets and milestones matter more than fresh coverage: a fresh transition costs 2 extra steps, a fresh edge 4, an unexplored state 8; target and milestone deposits are 1.5 × louder |
| `coverage` | fresh behaviour before targets: a still-sought target costs 4 extra steps, while a fresh edge costs nothing extra and an unexplored state 1 |

`errors` decides how much risk on the way is accepted, and how hard an
already satisfied error is avoided:

| value | what changes |
|-------|--------------|
| `balanced` | the historical caution: risk weight 2, hazard weight 20 |
| `seek` | reach the error first; accept a less robust input (risk weight 1) and a known hazard (hazard weight 10) |
| `avoid` | demand robustness (risk weight 5) and punish a known way into a satisfied error much harder (hazard weight 40) |

The two halves are independent: `"goals": "coverage", "errors": "avoid"` is
"cover new behaviour, but do not get near a known error". The command line
overrides one half at a time, so on such a model `--error-priority seek`
changes the errors and keeps the coverage goal priority, while
`--goal-priority targets` changes the goals and keeps the avoidance. Both
can be given:

```
eggembt run model.json --simulate --goal-priority targets --error-priority seek
```

The strategy is advice about *how* to test, not a property of the SUT. It is
not part of the PNML standard; the export writes it into the annotation
block (§8.6).

**The phase veto.** Every target has two phases (§5): `seeking` until it
has been reached often enough, then `satisfied`. A satisfied target's states
are `forbidden` in the reachability graph: the suggester normally never
chooses a move into one, plans never pass through one, and a candidate that
leads there from only some of the possible states pays the `hazard` fraction
in its score. `--satisfied-target-policy` changes only that reaction:

| value | what the suggester does |
|-------|-------------------------|
| `veto` (default) | refuses every move whose every possible effect enters a satisfied target; plans route around them |
| `penalise` | allows the move and the plan, but still counts the entry as hazard (20 steps at `balanced`, 10 at `seek`, 40 at `avoid`) |
| `ignore` | allows the move and the plan, and does not count the hazard |

This is useful when a satisfied error marking blocks the only corridor to
behaviour that is still uncovered: `penalise` lets the campaign pay a run
for the passage instead of giving up on what lies behind it. The phase
transition itself is unchanged, and so are the outcomes: a run that reaches
a satisfied error marking still ends as `error_reached`, a deadlock still
ends as `deadlock`, and an intentional target still ends the campaign. The
switch says whether the suggester may *choose* the move, not what happens
afterwards. The default is the historical, safest behaviour.

**The rare hunt.** Some SUT behaviour happens only rarely, and random
testing would need an unreasonable number of runs to see it. The model can
mark such a transition `"rare"` (§2.3), and `--hunt-rare` makes the campaign
spend itself on it:

```
eggembt run model.json --agent "..." --hunt-rare
```

The marker takes three numbers: `p` (reported, not used), `attempts` (the
campaign's attempt budget) and `wait-for` (milliseconds). `"rare": true` uses
0.001, 100 and 0; `"rare": false` is no marker; an object may set any subset.
A rare transition must be visible, so it cannot be an untraced internal
step.

With the switch on, an unseen rare transition with attempts left is a goal
above every other goal. The *enabling state* is the goal, not the edge: for a
move of the SUT the tester cannot take the edge, so being where the move is
enabled is what gives it a chance. When the suggester says `dwell`, the
campaign stays at that state for `wait-for` and leaves the SUT alone. Any
real event (`Observed` or `Died`) ends the dwell early because the state
moved on; a `Quiescent` report is not an event and does not end it. The idle
timeout is suspended during the dwell — the silence is deliberate — but the
wall-clock budget and a stop request still apply. A controllable rare input
is handled like any other input, except that it is sent only after the
`wait-for` has elapsed.

Every dwell is one attempt, even several dwells inside one run. A
controllable rare input sent without a dwell (`wait-for` 0) is one attempt
when it is sent. A rare move of the SUT with `wait-for` 0 produces a plain
wait — stay put and listen — which spends no attempt and is bounded by the
idle timeout and the run limits. The attempt budget is campaign-scoped and
is not written to the run log: a campaign continued from a log restores
which transitions were seen, but starts the attempt budget fresh.

Rare goals outrank coverage, so the campaign does not call itself complete
while a rare transition is unseen and still has attempts. The hard limits
remain hard: `--max-runs`, `--max-events`, `--max-states` and `--budget`.
When one of the first two cuts the hunt short, the stop reason says so:

```
run limit reached; rare transitions still unseen
time budget reached; rare transitions still unseen
```

When the attempt budget is spent and the transition is still unseen, the
hunt gives up as the model allowed and the campaign may end `coverage
complete`. `--require-complete` treats a still-pending rare transition as
incomplete. The console `Guidance` block and the JSON report show the
counts and whether hunting is on (§10.1, §10.2).

**The wall-clock budget.** `--budget DURATION` bounds the whole campaign in
wall-clock time, not per run:

```
eggembt run model.json --agent "..." --budget 10m
```

A bare number is seconds; `ms`, `s`, `m` and `h` are accepted. The value must
be positive and at most 30 days. The deadline is checked between runs and at
every turn of a running run, so a long run stops soon after the deadline
rather than after its event budget. The run that meets the deadline ends as
an ordinary
`completed` run with the detail `campaign time budget reached` and
`"budget_reached": true` in the JSON; the campaign's `stop_reason` becomes
`time budget reached`.

A budget is planning, not a fault: the exit code stays the one the campaign
would otherwise have (in particular 0, unless `--require-complete` finds the
coverage incomplete). And a budget does not pretend the campaign finished:
`"complete"` stays false unless the campaign really covered everything. The
default is 0 — no limit — so a campaign still stops on `--max-runs`,
`--max-events` or completeness.

### 8.6 Exporting to standard PNML

```
eggembt export MODEL [--output FILE] [--strict]
```

`export` converts a model to Petri Net Markup Language (ISO/IEC 15909-2), so
another Petri-net tool can draw, analyse or simulate the net. Without
`--output` the document goes to standard output (warnings still go to
standard error); with it, the file is written and a one-line summary names
the place and transition counts. `--strict` turns loader warnings into
errors, as everywhere else.

What is standard PNML, and survives every reader:

- places, transitions, and their names;
- normal arcs and inhibitor arcs, with their weights;
- **one** initial marking — the model's *first* one. PNML has no way to say
  "one of several initial markings"; the others are annotations.

The eggembt conventions that are PNML extensions are written the way the
tool's own PNML loader reads them: a `<toolspecific tool="eggembt"
kind="..." label="..." rank="..."/>` on each transition keeps the kind, the
label and the milestone. A net exported by eggembt and imported again
therefore keeps its input/output/internal character, the wire labels and the
milestones — the shipped `examples/coffee-pnml.pnml` survives that round
trip, and so does a model with weights and inhibitor arcs.

Everything PNML cannot express is carried as **annotations**, twice:

- as XML comments beside the elements, for a human reading the file
  (`<!-- transition "press": controllable, label "coin.insert" -->`,
  `<!-- place "busy": bound 3 -->`, a note that PNML keeps only the first
  initial marking);
- as one machine-readable block at the end,
  `<toolspecific tool="eggembt" version="..."><text>{...}</text></toolspecific>`,
  whose JSON holds the full name, the testing strategy, every initial,
  target and error marking with its policy and crash settings, the place
  bounds and the transition tags.

The annotations do **not** survive a PNML import: a PNML model has no target
markings, error markings, bounds or strategy by definition, and the tool's
loader ignores the eggembt block. The JSON model remains the complete
specification; the PNML file is the standard-format view of its net. The
export says so in its header comment.

One technical detail: PNML ids must be XML names. A model whose place or
transition id is not (say, `1 bad place`) is exported with a generated id
(`p0`, `t0`); the original id stays in `<name>` and in the annotation block,
so nothing is lost, but the imported net uses the generated id.

---

## 9. Command hooks

A Petri net covers the concurrent/distributed aspect of computing. Sometimes
the test needs the rest — a data check, a collection, a side effect — and it
should happen *during* the run, when the model says the SUT is in a certain
situation. Egge's assignment, verbatim:

> Feature: Run a shell command synchronously or asynchronously when a transition fires. Once or always. (Okay it can be controlled by a certain place.) After all, a Petri net model only covers the concurrent/distributed aspect of computing. We may want invoke e.g. data oriented testing, during a run, too.

### 9.1 The `command` key

A command sits on a transition (§2.3):

```json
{"id": "coin.insert", "label": "coin.insert", "kind": "controllable",
 "command": "collect.sh"}
{"id": "coffee.served", "label": "coffee.served", "kind": "observable",
 "command": {"run": "verify.sh", "mode": "async", "times": 5, "when": "ready"}}
```

`mode` is `"sync"` (default: the run waits for the command) or `"async"`
(fire-and-forget); `times` caps the executions over the whole campaign
(`1` = once); `when` names a place — the guard (§9.3). The command must be
non-empty, and the transition's step must be *visible*: an untraced internal
transition cannot carry a command, because the tester never sees its step.

### 9.2 When the command runs

The command runs when a step of its transition is **certain** — one
execution per certainly fired step, in counting order. An ambiguous stretch
(a "maybe" step, §4) runs nothing; but when the stretch is later resolved
and a hooked transition was *ruled out* of it, the run notes that the
command did not run:

```
note the command of 'x2' did not run: its suspect step was ruled out
```

The `times` budget is the whole campaign's: run 1 may spend it, run 2 gets
nothing. A guard that fails, and a spent budget, run nothing and are not
failures — the campaign carries on silently.

A synchronous command that fails — a non-zero exit, a timeout, launch
trouble — is a note under the run line, and nothing else:

```
note the command of 'tb' failed (exit 3): the run continues
note the command of 'tb' timed out after 60 s: the run continues
note the command of 'tb' could not be launched: No such file or directory
```

### 9.3 The guard: `when`

Egge's answer: **"Certainly marked after."** The guard names a place, and
the command runs only when that place holds a token in **every** state the
tester believes possible **after** the certain step. The guard is read-only:
a condition on the believed marking, never a way for the command to steer.

### 9.4 Sync, async, and the bound

A **synchronous** command blocks the run while it runs — the SUT's own
events queue up meanwhile and nothing is lost. Its stdout+stderr is captured
into the run, cut at 4096 bytes. `--hook-timeout S` (1–3600, default 60)
kills a command that runs past its deadline; its record says `timeout`.

An **asynchronous** command is fire-and-forget: the tool does not wait for
it and never kills it. At eggembt's exit the children that already ended are
reaped; the ones still running keep running.

Every execution leaves one **hook record** in the run: transition, label,
sync or async, the status word (`ok`, `failed`, `timeout`, `launched`), the
exit code (unknown = −1) and the captured output. The records travel in the
run log and are adopted on a restore — **the commands are not run again**.

### 9.5 What a command can see

The command line runs through `sh -c` in eggembt's working directory, with the
inherited environment plus four variables (the model's data is never
interpolated into the command line itself):

| variable | value |
|----------|-------|
| `EGGEMBT_TRANSITION` | the transition's id |
| `EGGEMBT_TRANSITION_LABEL` | its label (may be empty) |
| `EGGEMBT_RUN` | the run's id within the campaign (0 for the first) |
| `EGGEMBT_LOG` | the open run log's path (`--log`); absent without one |

### 9.6 Where the hooks are reported

- The console prints a `Command hooks` table after the run lines (run, mode,
  status, transition, exit, first line of the output) whenever any run
  executed a hook (§10.1), and the Guidance block gains the line
  `commands: N transition(s), timeout S s`.
- The `--json` report always carries `hook_timeout` and `hook_runs` (§10.2).
- `eggembt report` over a run log shows the same table after a restore (§10.3).

---

## 10. Reading the output

### 10.1 The console report, line by line

A real campaign against `drink-machine`:

```
Testing drink-machine against the built-in simulated SUT
  run 1    completed       8 events, 6 inputs, +4 states +6 edges  coverage complete: every explored edge is covered and no target is left to seek

Campaign  coverage complete; 1 run(s) this session

Coverage
  transitions  6 of 6  (100%)
  places       4 of 4  (100%)
  states       4 of 4 known  (100%)
  edges        6 of 6 known  (100%)

Runs  1 recorded  (1 completed)

Guidance  ant colony: 9 round(s), 180 ant walk(s)
  targets: deadlock once, livelock once; coverage threshold 1
  satisfied targets: veto
  post-mortems: on deviation none, on deadlock none, on livelock none
  catastrophes: priced by the way in; unpredictable transitions 0
  milestones: deposit 1.5 per rank; milestone transitions 0

Time  campaign 0.02 s, mean run 24 ms (longest 24 ms), 333.3 events/s
```

- The **run lines** are the campaign's log: outcome name, event and input
  counts, how much the run grew the graph, and the run's own detail (§6
  showed the deviation and livelock forms).
- The **Campaign** line names the **stop reason**: "coverage complete", "run
  limit reached" (the `--max-runs` default 20 hit — §6.4's sample campaign
  ended this way), "the good target marking 'X' was reached" (an
  intentional target, §5), "too many aborted runs in a row" (§6.8), or an
  external stop request.
- **Coverage** counts transitions/places exactly, and states/edges as
  *known* (the graph may still be growing, §3.3). Uncovered transitions are
  listed under "Transitions not definitely covered".
- **Guidance** reports which steering ran and its switches — the colony's
  iterations and walks, the policies, the post-mortem modes, the catastrophe
  pricing, the milestones. With the T-suffix and carry-over switches on, the
  `--json` report carries them too (§10.2).
- **Rare transitions** — when the model marks any, a `rare:` line follows
  the other guidance lines: how many there are, how many have been seen, how
  many attempts the dwells have used, and whether `--hunt-rare` is on. It is
  printed whether hunting is on or off, so a model with markers and a
  campaign without the switch is visible at once (§8.5).
- **Crash predictions** — when the model declares crash predictions (or a
  death was attributed to an implicit entry), this table follows the run
  lines: per crash site its prediction, the status of its experiment, its
  attempts, crashes and dumps (§6.11).
- **Command hooks** — when the model declares `command` hooks and a run
  executed one, a table of the executions follows the run lines (§9).
- **Traces** — with `report --runs` or `--trace N`, the recorded steps follow
  the post-mortems: one `run N outcome start > step > step` line per run,
  oldest first (§10.5).
- **Time** is the wall-clock goodness criterion: total, mean, longest run,
  events per second — this is what tells seeds apart.

### 10.2 The JSON report

`--json FILE` writes the same campaign for machines. The top-level keys:
`model`, `complete`, `stop_reason`, `runs`, `coverage`, `warnings`, `seed`,
`ant_seed`, `ants`, `ant_walks`, `ant_iterations`,
`catastrophe_time_pricing`, `coverage_threshold`, `deadlock_policy`,
`livelock_policy`, `satisfied_target_policy`, `dump_on_deviation`, `dump_on_deadlock`,
`dump_on_livelock`, `tsuffix`, `restored_pheromones`, `crash_level`,
`crash_attempts`, `crash_predictions`, `hook_timeout`, `hook_runs`,
`strategy`, `evaporate_between_runs`, `evaporate_restored_pheromones`,
`time_budget_ms`, `hunt_rare`,
`rare_transitions`, `overlap`, `time`.
The `strategy` object names the effective priorities (`{"goals": "...",
"errors": "..."}`); `evaporate_between_runs` says where the pheromone aged
(§7.4); `evaporate_restored_pheromones` says whether restored teaching aged
between the restored runs and spared matured targets (§7.3);
`time_budget_ms` is the configured budget (0 = none);
`satisfied_target_policy` names the phase policy (§8.5); and every run
carries `"budget_reached"` — true for the run the wall-clock budget cut
short (§8.5). `hunt_rare` is the boolean campaign switch and
`rare_transitions` is always present (possibly empty), one object per marked
transition: `{transition, label?, p, attempts, wait_for_ms, seen,
attempts_used}`. `seen` is definite transition coverage, so an ambiguous
"may have happened" does not count (§8.5).
The two guidance switches of the 17 November batch are always present,
e.g. `"tsuffix": 0, "restored_pheromones": 50`, and so are the three crash keys
(§6.11): the credibility level, the implicit give-up budget, and per entry
its site, prediction, status, attempts, crashes and dumps. The hooks keys (§9) are
always present too: the synchronous bound `hook_timeout`, and `hook_runs`
with, per run that executed one, its hook records. With
`--coverage-elements` the `coverage` object also carries `states[]` and
`edges[]`, one object per known element (§10.5).

### 10.3 Backtraces — the textual report

One of Egge's open items asked for:

> A command-line runner (model, run log, agent address) and a GUI for coverage and backtraces.

The runner, the run log and the agent address all exist; the GUI half was
scratched by decision — the sentence, verbatim:

> Scratch the idea of a GUI. A textual report is fine.

So the coverage-and-backtraces view ships as text: `eggembt report` prints, per
definitely covered transition, the chain of moves that led into its first
definite visit, oldest first:

```
Backtraces  (the way into the first definite visit of each covered transition, oldest first)
  coin.insert     start > coin.insert
  button.coffee   start > coin.insert > button.coffee
  button.tea      start > coin.insert > button.coffee > coffee.served > coin.insert > button.tea
```

An ambiguous stretch — the run certainly went on, but the exact edge is not
certain (§4) — renders as `...` with a legend, instead of guessing a chain.

### 10.4 The outcome names, collected

| outcome | what ended the run | exit-code effect |
|---------|--------------------|------------------|
| `completed` | coverage complete, or nothing left to seek | — |
| `deviation` | the SUT did what the model does not know (§6.4) | 1 |
| `deadlock` | stranded where the model cannot move (§6.5) | 1 |
| `livelock` | busy without progress (§6.6) | 1 |
| `error_reached` | a declared error marking / error transition (§6.2, §6.3) | — (sought) |
| `sut_crashed` | the SUT died or was crashed for a post-mortem (§6.7) | — (a catastrophe to learn from) |
| `aborted` | infrastructure: timeouts, overflow, agent faults (§6.8) | 3 if the campaign stops for it |
| `open` | the run is still being written (internal/log state) | — |

### 10.5 Inspecting a campaign afterwards

The log is the record, so the inspection views read it: they never re-run the
SUT and never change the campaign.

**The traces.** `report --runs` prints every run's steps, oldest first:

```
Traces  (the recorded steps of every run, oldest first)
  run 1   sut_crashed   atm-lobby > card.insert > card.read > pin.enter > pin.accepted > withdraw.request > db.down
  run 2   sut_crashed   atm-drivein > card.insert > bank.stolen_report > card.confiscated
  run 3   completed     atm-branch > card.insert > card.read > ... > pin.accepted > card.return > card.ejected
```

A long run keeps its last 20 steps and says so (`... > pin.accepted ...
[N steps]`). `report --trace N` prints one run in full, with its detail in
parentheses. The steps come from the log, so an event run shows its events
instead of a path. Asking for a run the log does not have prints
`the log has 4 run(s), no run 99` and is not an error.

**Coverage per element.** `run --coverage-elements --json FILE` or
`report --coverage-elements --json FILE` add two arrays to the report:

```
"coverage": {
  "states": [{"marking": "idle:1, cassette:2, tries:2", "definite": 2, "ambiguous": 0,
              "covered": true, "first_run": 1, "last_run": 4}],
  "edges":  [{"from": "idle:1, cassette:2, tries:2", "transition": "insert_card",
              "label": "card.insert", "to": "inserting:1, cassette:2, tries:2",
              "definite": 2, "ambiguous": 0, "covered": true,
              "first_run": 1, "last_run": 4}]
}
```

`covered` follows `--threshold`; `ambiguous` is the "may have visited" count
from an ambiguous stretch (§4); the run numbers are 1-based, matching
`--runs`. Leave the switch off for a compact report — a large graph makes
this big.

**The drawing.** `report --dot FILE` writes the graph for Graphviz, coloured
by coverage: dark green and thick for covered (`definite >= threshold`), light
green for visited below the threshold, orange for possibly visited only, grey
for known but never visited. The drawing includes the legend.
`explore --dot` stays plain: it runs before any campaign.

**The pheromone.** `--dump-pheromone FILE` (on `run`, or on `report`) writes
what steered the ants:

```
{"model": "atm", "tsuffix": 4, "tau_max": 10, "iterations": 37, "ant_walks": 740,
 "edges": [{"from": "...", "transition": "insert_card", "label": "card.insert",
            "to": "...", "attract": 0, "repel": 0.5007, "region": 0}],
 "pairs": [{"state": "confiscated:1, cassette:1, tries:2",
            "tsuffix": [2597307718, 78764499, ...], "attract": 0, "repel": 2.6599}]}
```

`edges[]` is the per-edge layer (§7), `pairs[]` the (marking, T-suffix) store
with the key in full (§7.2), empty when the layer is off. Names replace ids on
purpose: edge ids only mean something inside the session that made them.

`report` rebuilds the colony from the log, which records steps but not its
settings — so give it the campaign's `--tsuffix N`, or the pair store comes out
empty. The rebuild is the documented approximation of §7.3: the milestone and
catastrophe deposits, toned down by `--restored-pheromones` (default 50). Its
`iterations` and `ant_walks` are 0 because the log does not record how many
rounds ran; the channels show what was learned, not how long it took.

### 10.6 The overlap index, Jaccard's, verbatim

`compare` and the campaign report's `Overlap` line measure **coverage
overlap** — not coverage. For two sets A and B of covered elements the
Jaccard index is

    J(A, B) = |A ∩ B| / |A ∪ B|

with `|X|` the number of elements of X: of all elements either side covered,
the fraction both covered. `1` means the same set, `0` means nothing in
common. It is computed **three times**, once per element kind — transitions,
states, edges — because a campaign can revisit a few transitions over a wide
state space, or the other way round; one number would hide that.

The sets are the ones a run *definitely* touched:

- **states** — every state the run definitely visited, the initial state
  included;
- **transitions** — every transition that certainly fired, including the
  credits of an ambiguous stretch (a transition seen to fire, even where the
  exact edge is not certain);
- **edges** — every edge the run definitely traversed.

A set counts **presence, not frequency**: visiting a state once or ten times
is one element, so the index does not drift with run length or with the
number of visits. A run that covered nothing contributes an empty set.

The conventions the tool pins (`src/engine/OverlapAnalysis.*`):

    J(A, A) = 1                      (every element is in both)
    J(∅, ∅) = 1                      (0/0 is not computable; nothing = nothing)
    J(∅, B) = 0  for non-empty B
    campaign set = union of its runs' sets; the campaign index is J of those unions
    mean_pairwise = arithmetic mean over the n(n-1)/2 unordered pairs,
                    0 when fewer than two runs

The union count is `|A ∪ B| = |A| + |B| − |A ∩ B|`, so the index needs only
the sizes and the common elements — which is why the matrices are cheap to
compute from a run log and why a replay reproduces the live numbers exactly.

**Worked example, transitions.** Run A covered `{insert_card, open_session,
connect_db, reader_check}`, run B `{insert_card, open_session, connect_db,
atm_fail}`. Then `|A ∩ B| = 3`, `|A ∪ B| = 5`, and `J = 3/5 = 0.60`: 60 %.

**A real one.** The ATM demo of §10.5, a four-run log against a six-run log
of the same model:

```
  campaigns as a whole: transitions 93%, states 80%, edges 80%
  within the four-run log: transitions 49%, states 8%, edges 6%
  within the six-run log:  transitions 49%, states 7%, edges 6%
  run against run (4 x 6 pairs), cross means: transitions 58%, states 23%, edges 22%
```

Reading it: within one campaign the runs share about half their transitions
but only a few percent of their states and edges — the steering spreads the
testing out instead of repeating the same path; across the two logs the
campaigns agree closely on transitions (93 %) but differ in which states and
edges they reached (80 %), which is what a different run count and steering
history does.

What the number is **not**:

- not a coverage measure — that is the `Coverage` block (how much of the
  model is tested); the index says only how much two coverages coincide;
- not the overlap coefficient `|A ∩ B| / min(|A|, |B|)`, which would call a
  small set inside a large one 100 % — Jaccard punishes the difference in
  size, which is what "how similar are these two campaigns?" needs;
- not frequency-aware and not order-aware (by design: it compares *what* was
  tested, not how often or in which order);
- printed rounded to whole percent; `--csv` and `--json` carry the exact
  values.

---

## 11. Glossary

| term | meaning |
|------|---------|
| **SUT** | system under test — the thing being tested |
| **model** | the Petri-net description of the SUT's behaviour |
| **place / token** | a situation / a marker in it; which places hold how many tokens is the **marking** — the model's current situation |
| **transition** | an action that moves tokens; **controllable** (tester input), **observable** (SUT output), **internal** (model bookkeeping; silent unless traced) |
| **label** | the word exchanged with the SUT on the wire |
| **marking** | which places hold how many tokens; **initial**, **target** (§5), **error** (§6) are named kinds of it |
| **reachability graph** | every state the model can be in and every way to get there (§3) |
| **state / edge** | one reachable marking / one possible firing in that graph |
| **deadlock** | a state where nothing can fire — stuck by definition; the `deadlock` outcome needs the SUT to be silent at such an ordinary state (§6.5, §6.12) |
| **livelock** | a cycle without a legitimate exit — static (a terminal cycle) or run-time (busy without progress); the `livelock` outcome is the run-time heuristic (§6.6, §6.12) |
| **strongly connected component (SCC)** | a maximal set of graph states in which every state reaches every other; a terminal one has no edge leaving it (§6.12) |
| **trace** | the ordered steps of one run (§4) |
| **definite step** | a step the tool knows exactly (certain); ambiguous stretches are credited transition-wise only |
| **run / campaign / round** | one encounter with the SUT / all runs of one invocation / one invocation (§4) |
| **run log** | the file every run is appended to; continuing replays it (§4, §7.3) |
| **coverage** | what has been tested so far, counted per transition/place/state/edge; complete after `--threshold` visits |
| **coverage overlap (Jaccard)** | how much two coverages coincide: `|A ∩ B| / |A ∪ B|` per transition, state and edge set, presence only (§10.6) |
| **novelty** | the colony's "unexplored smells interesting": each edge's weight carries `1/(1 + hits)`, and an ant at a state with an uncovered way out deposits 1.0 and ends its walk (§7.1) |
| **target marking** | a situation the campaign seeks; **intentional** ones end the campaign successfully (§5) |
| **target phase** | `seeking` while a target is still a goal; `satisfied` (matured, meaning its "visited N times" criterion is reached) once its policy count is reached, after which its states are forbidden (§8.5) |
| **phase veto** | the suggester's default refusal to move into a satisfied target; `--satisfied-target-policy` relaxes it (§8.5) |
| **off-target region** | states from which no target marking is reachable — a repellent region (§5) |
| **error marking / error transition** | a declared forbidden situation / step (§6.2, §6.3) |
| **deviation** | an event the model cannot explain (§6.4) |
| **quiescence** | "nothing more will come" — reported after `--quiescence` ms of silence |
| **post-mortem** | a snapshot or a deliberate crash at an error, to examine the SUT's state |
| **aborted** | a run ended by infrastructure trouble, not by the SUT (§6.8) |
| **ant colony** | the steering heuristic: virtual ants walk the graph and lay pheromone (§7.1) |
| **pheromone** | attract/repel deposits on edges that steer later choices; evaporate; capped at `tauMax` |
| **pheromone dump** | the colony written as JSON (`--dump-pheromone`), keyed by markings, transition ids and T-suffix elements — never by session-local edge ids (§10.5) |
| **milestone** | a transition worth stepping on: deposits 1.5 × rank, never ends a run, does not legitimize a terminal cycle (§6.12) |
| **catastrophe** | the pheromone lesson of a found fault, priced by the way in (§6.10) |
| **T-suffix** | the list of truncated traces (mini-hashes of the recent trace) an arrival carries; the (marking, T-suffix) pairs are a finer pheromone basis (§7.2) |
| **suffix cascade** | the recurrence the T-suffix elements follow — element k hashes the trace's last k steps (§7.2) |
| **carry-over** | the pheromone teaching of earlier rounds, rebuilt merged and toned down into a continued campaign, optionally aged between the restored runs with matured findings spared (§7.3) |
| **crash prediction** | a nondeterministic SUT crash declared in the model with its probable class; the campaign reproduces it until it is certain (§6.11) |
| **attempt / crash (of the experiment)** | one definite visit of the predicted place while it seeks / one death attributed to it (§6.11) |
| **credibility level** | how certainly the experiment may conclude "deterministic" — `--crash-level`, 0.95 (§6.11) |
| **implicit crash** | a crash nobody predicted: an entry at the run's last credited step, the neutral class (§6.11) |
| **command hook** | a shell command attached to a transition (`"command"`); runs when its step is certainly fired, synchronously or asynchronously, once or always, optionally guarded by a place (§9) |
| **hook record** | the record of one hook execution (status, exit code, captured output); it travels in the run log, the command is not run again on a restore (§9) |
| **synchronized event** | a step of the SUT's own (`"sync": true`) at which the SUT is held until the tracker acknowledged the event; opt-in per transition and per campaign (`--sync-events`), capability-gated on the plug-in (§2.3) |
| **ack** | the tracker's "handled, go on" for a synchronized event; it comes after the event's consequences, including a post-mortem, never for an ordinary event |
| **lifecycle input** | a controllable transition whose label is one of the reserved words `start`, `restart`, `stop`, `pause`, `resume`: the tester sends it, the SUT's own control does the work, and the engine supplies the landing or closes the gate (§2.3) |
| **lifecycle gate** | what the campaign may send next: **Running** (everything), **Paused** (`resume`, `stop`, `restart`), **Down** (`start`, `restart`); silence is expected while not Running (§2.3) |

---

## 12. The limits of the approach

The tool is a heuristic tester, and every heuristic has a shape of problem it
does not fit. This chapter collects the limits that were measured or
reasoned through while building it, so a user can recognise them instead of
blaming the model. The pheromone's failure modes are the first four; the
last ones concern the graph, the model's time and the strategy.

### 12.1 The edge channel saturates, and a sought target outshouts it

The colony caps every edge channel at `tauMax` (10.0). A catastrophe's
deposit is 3.0 at the shortest way in and grows by 0.25 × 3.0 per input on
the way (up to 8 inputs), i.e. up to 9.0. A still-sought target deposits
2.0, and every ant that reaches it deposits again. Two consequences:

- **More walking is not more learning.** Once the edges into a target are
  saturated, further deposits change nothing; the channel cannot say
  "twice as bad".
- **A sought target can cancel the catastrophe.** While a crash prediction
  or an error is still to be reproduced, TargetBoard keeps it a goal — the
  campaign must go back. Its attract lands on the same edges as the
  catastrophe's repel (the same marking, the same prefix), and attract
  enters the choice weight multiplicatively while repel enters in a decaying
  exponential. If the history does not separate the two ways in, the attract
  wins.

This is not theory. The benchmark in `tst_pheromone` (1008 states, 2010
edges; six runs of 200 events; one crashing shortcut and one safe two-step
way into the region) gives, seed after seed:

```
no ants            crashes 5  long runs 1  covered 201
ants, no T-suffix  crashes 5  long runs 1  covered 201
ants, T-suffix 4   crashes 3  long runs 3  covered 586
```

The edge-only colony is *exactly* as good as no colony on this model. Only
the (marking, T-suffix) layer can say "the crash is the way in *via the
shortcut*, not the way in via the safe route" and keep both lessons alive.
So: **pheromones guided only by markings do not work when the good and the
bad route share their recent history** — which is precisely the interesting
case for a crash near the start of a run.

### 12.2 The pair layer matches exactly, so it is only as good as the repetition

The pair key is the marking plus the **complete** T-suffix list, element for
element. There is no similarity, no wildcard, no generalisation:

- Two arrivals whose last *j* steps agree but whose earlier steps differ are
  different pairs. A lesson at one does not transfer to the other.
- A short list (`--tsuffix 1` or 2) merges many routes and the crash lesson
  spills onto safe ones; a long list (7 or 8) fragments the arrivals so
  finely that the same route is rarely met twice and no lesson accumulates.
  The default off (0) means the layer does not exist; 4 is the useful
  middle.
- The store's size follows the number of distinct (marking, suffix)
  combinations actually visited. It is pruned only when both channels have
  evaporated below 1e-4, so a long campaign with a long suffix can carry a
  large store.

Consequently, pheromones do not help when **the same mistake is never
repeated along the same recent history** — a genuinely new route each time,
or an SUT whose step timing differs on every visit. There is nothing to
learn from; the campaign is then a random walk with coverage bookkeeping.
The exact cascade is also the reason a *near* repetition does not count:
the last step alone (`--tsuffix 1`) distinguishes less than the user might
expect, because the first element already erases everything before it.

### 12.3 Evaporation and long runs

Per suggestion, the attract loses 10 % and the repel 2 %. The repel
half-life is about 34 suggestions; after 200 suggestions, 1.8 % is left
(§7.4). So:

- **Within one long run**, a lesson from its beginning is essentially gone
  by its end unless `--evaporate-between-runs` is used. This is the stage at
  which evaporation ruins progress: not at the first suggestion, and not
  between short runs, but *inside a run that makes many suggestions*.
- **With boundary aging**, the opposite risk appears: one very long run
  ages nothing at all until it ends. A wrong lesson laid early can steer
  the whole run, and a lesson from the previous run survives at 98 % even
  if the SUT has changed in between. There is no adaptive rate.

The campaign repeats runs exactly because a run is short enough to be
recoverable; evaporation is the second line of defence, not the first.

The sweep behind the default (`journal.md` §31) moves the per-suggestion repel
rate from 0.005 to 0.10: every rate gives the same outcome on the benchmark
— 3 crashes, 3 productive runs, about 586 covered edges — even where the
nominal retention over a run falls to nothing. The reason is the timing:
the fork is reached about five suggestions after the run starts, so only a
short interval has to survive, and the pair repel plus the safe route's
attract protect it twice. Aging between runs is still visibly better (2
crashes, 4 productive runs, 777 edges), and the same rate × run length gives
the same profile. The default is therefore on a plateau for this shape, not
on a cliff; a model whose fork lies much deeper into a run is the case where
the rate should be lowered or the boundary aging used.

### 12.4 When the target and the catastrophe are indistinguishable

Even the pair layer cannot separate two routes whose recent histories are
identical. If the crash and the safe continuation are enabled from the same
state and the model offers no observable difference before the choice, then
"arrive at the choice via route A" and "via route B" are the same pair. The
campaign can only alternate at random, and no amount of pheromone makes it
certain. The same holds if the crash happens on the *first* step after an
unmodelled initial action: with no prefix to hash, there is no pair at all.

The practical answer is model design, not more pheromone: make the
difference observable (a place set by an earlier output, a milestone, a
sync event), or accept that this crash is only reachable by repetition.

### 12.5 The graph is finite, and the pheromone only knows the graph

`--max-states` (default 1,000,000) and the lazy expansion budgets (500
states ahead per suggestion, 200 per ant iteration) bound what the tool
knows. Pheromone, distances and targets all live *inside* the explored
graph:

- A target or error beyond the truncation is invisible: the suggester can
  report `Stuck` (goals exist but none is reachable from here) although the
  SUT could reach them. The run then ends and a new run restarts; the
  campaign may still never get there.
- The frontier tier is the only signal toward unknown territory (an ant
  deposits 1.0 and ends its walk there). That is a weak signal compared to a
  target's 2.0, and it disappears once the expansion budget is exhausted.
- A model whose state space is genuinely huge will spend its budget on the
  graph before it tests, or will test only the explored part. `explore`
  reports the truncation; the campaign JSON and the console say when states
  were cut off.

### 12.6 Human and timing unpredictability

A transition marked `"unpredictable": true` is stepped by a human operator.
Two things follow: the model cannot say *when*, and the tool cannot price
the wall-clock way in (the delay measured a human, not the SUT), so
catastrophe deposits stay flat. The ants' plans assume the environment moves
by the model's rules; a human action that is not pinned by an observable
output can move the SUT to a marking the pair layer did not predict, and
the learned suffix then belongs to the wrong situation. Run-time livelock
detection (`--livelock-timeout`) and the idle timeout are wall-clock
judgements too: a slow but honest SUT can be declared stuck. §6.12 separates
that run-time judgement from the static terminal-cycle finding.

### 12.7 The model must be right, and the strategy is a trade

Everything above assumes the model describes the SUT. A wrong model is the
worst case for a pheromone-driven tester: the colony learns confidently
about a system that does not exist, the suggester vetoes inputs that lead to
"forbidden" states the real SUT would not enter, and a deviation is
reported late. `check` and `explore` are the cheap cures; a first
`--simulate` campaign is the second.

The strategy options are deliberately a trade, not a free lunch.
`--goal-priority targets` can starve coverage of fresh behaviour (the fresh
edge keeps losing to the target); `--goal-priority coverage` can postpone a
sought error indefinitely on a large model; `--error-priority seek` accepts
crashes and dumps that cost wall-clock time, and `--error-priority avoid`
may keep the campaign away from the very error it was asked to reproduce.
The wall-clock budget then truncates the campaign wherever it happens to
be — before the target, if the target was late. None of these is a bug;
they are the reason the strategy is declared per model and overridable per
campaign.

### 12.8 The phase veto is a hard rule only at its default

`--satisfied-target-policy penalise` and `ignore` remove the suggester's
only absolute rule. The hazard term is a penalty in *steps*, so a goal that
is close enough behind a satisfied target can still win: at `balanced`, a
certain entry costs 20 steps, which is a lot but not infinite; `ignore`
removes even that. The run that enters the target still ends with the
target's outcome — `error_reached`, `deadlock`, or a completed campaign for
an intentional target — so the price is paid in whole runs, not just in
suggestion steps. On a model where the satisfied target is the only
corridor, that is the point; on a model where it is a shortcut to a place
you can reach safely another way, the runs are simply lost. `veto` remains
the default for that reason.

### 12.9 The rare hunt spends coverage by design

`--hunt-rare` does exactly what it says: the rare transitions outrank
everything else, including fresh coverage, and the campaign refuses to call
itself complete while one is still unseen and still has attempts. That is
the point of the switch, but it is also a real cost:

- **`p` is not a measurement.** It is reported in `check`, the console and
  the JSON, but the engine cannot verify the SUT's real odds and does not
  use it. Marking a common move as rare spends the campaign on it.
- **An attempt is a dwell, not a trigger.** The budget bounds how often the
  campaign waits. `{"attempts": 100, "wait-for": 60000}` can mean a hundred
  minutes of deliberate silence, each one a suggestion not spent on fresh
  coverage. A `wait-for` that is too short can miss a slow SUT; one that is
  too long can eat the budget.
- **A rare move of the SUT with `wait-for: 0` spends no attempt.** It
  produces the ordinary stay-put-and-listen wait, bounded by the idle
  timeout and the run limits instead.
- **An unreachable rare transition keeps the campaign alive.** If its
  enabling place never holds a token in the explored graph, no dwell can
  happen and no attempt can be spent, but the transition stays pending; the
  campaign runs to `--max-runs`, `--max-events`, `--max-states` or
  `--budget`, and the stop reason names the rare hunt.
- **The idle timeout is suspended during a dwell.** A SUT that hangs inside
  one is noticed only when the dwell ends; a stop request and the budget are
  still honoured.
- **Attempt budgets do not survive a run log.** A continued campaign starts
  the budget fresh; only definite transition hits are restored. A rare
  transition can therefore be hunted again after every restart of the tool.
- **The hunt cannot outrun a wrong model.** A rare move that the SUT makes
  only under conditions the model does not describe cannot be hunted by
  waiting at a state the model believes in; that is a modelling problem,
  like the indistinguishable target and catastrophe of §12.4.

`--require-complete` treats a still-pending rare transition as incomplete,
which is consistent with the hunt's own notion of completeness: the campaign
has not done what it was asked to do yet.

### 12.10 How long a run should be

`--max-events` bounds one run, not the campaign. A campaign with a
`--budget` can therefore spend the same time on many short runs or on a few
long ones, and every run starts again at the model's initial marking while
the coverage and the pheromone stay with the campaign. The natural question
is which shape buys more coverage. The benchmark in `tests/tst_comm10k.cpp`
was built to answer it: four communicating machines over a ring of four
binary channels, 24 places, 36 transitions, exactly 10,000 states and
64,000 edges, four goals and four error states, run for ten minutes at 50,
200, 800 and 3200 events per run on three seeds (`journal.md` §32).

The answer is that run size is a weak lever: neither many tiny runs nor a
few huge ones is better. On the mean of the three seeds:

```
events per run   edges covered   edge per event
     50             12,198           0.918
    200             12,527           0.960
    800             13,254           0.966
   3200             11,798           0.972
```

The best-to-worst mean spread is 12 %, and the seed-to-seed spread within
one size reaches 41 % (200 events: 9,851 to 13,955), so there is no sharp
optimum. Two trends are reliable. First, a shorter run re-tests its initial
region on every restart: per event it covered 4-6 % fewer new edges than a
run of 200 or more. Second, both extremes were weak on the mean; four runs
of 3200 events per campaign was the weakest, and 50 events per run was not
better than the middle. The 800-event middle was best on average, but it won
only two of the three seeds.

The practical reading:

- The default `--max-events 10000` is a safety cap, not a recommendation.
  It protects the campaign from one runaway run; it does not make coverage
  better or worse by itself.
- Choose `--max-events` for what the run boundary really controls: how much
  of a bad run has to be repeated, how often the SUT's setup is paid, and
  how deep one run goes before the campaign restarts. A long run can reach
  behaviour near the end of a deep path; a short run restarts from the
  initial marking more often.
- If coverage is the only goal, start in the middle (a few hundred to a few
  thousand events) rather than at either extreme, and measure your own
  model: the comparison is cheap to repeat with `--budget` and two values of
  `--max-events`.
