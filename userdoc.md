# Egge's Model-Based Tester (eggembt) — the user manual

*GLM-5.3-Flash, 17 November 2026. This manual is new — the whole file is the
addition of the batch documented in `doku.md` §16. Read `README.md` for a
short overview and `doku.md` for the design record. Two conventions:*

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

1. It **analyzes the model itself**, finding dead ends and traps in your
   description before you ever touch the real system.
2. It **generates and executes tests against your real system** (or a
   simulation of it), driving it through sequence after sequence of
   interactions and checking *every event* against the model. Anything the
   system does that the model cannot explain is reported as a candidate bug;
   everything the model and system agree on is accumulated as **coverage** —
   a precise answer to "what have we actually tested?"

Your effort goes into one model instead of hundreds of scripts, and the tool
explores systematically instead of following your hunches.

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
- **Crash predictions** — a transition or a target/error marking can predict
  a *nondeterministic* SUT crash: `"crash": "sometimes"` (a class word:
  `random`, `rare`, `sometimes`, `often`, `always`), a bare number
  (`"crash": 0.7`, an explicit probability) or an object
  `{"class": "rare", "p": 0.3, "attempts": 7, "crash_dump": "snapshot"}`.
  A predicted crash is a *target* the campaign tries to reproduce until the
  experiment behind it is certain it is deterministic (§6.11).
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

- one **state** per reachable marking ("reachable" = can happen, starting
  from the start, by *some* sequence of moves — hence the name);
- one **edge** per possible firing, labelled with the transition that fires.

The reachability graph is the complete map of the model's behaviour. Every
question of the form "can the model end up in situation X?" has a definite
answer in it.

### 3.2 What the analysis of the graph reports

`eggembt check` and `eggembt explore` walk the graph and report:

- **Deadlocks** — states where *nothing* can fire any more. The machine is
  stuck by definition. In `drink-machine` there are none; in a model where a
  place can deadlock (say, a card swallowed with no way out) the count is
  the number of such traps.
- **Terminal cycles without a target marking (livelocks)** — a *cycle* of
  states that the model can enter but never legitimately leave: a trap the
  specification itself can spin in forever. `examples/heartbeat-livelock.json`
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
  the missing output.

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
(`start > coin.insert > button.coffee > coffee.served`), and what the T-lists
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
T-lists (§7.1) act on *definitely credited* steps.

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
wording; the concept is now spelled "nontarget" after the rename):

> 5. Optionally, states from which no final marking is reachable as a further repellent region.

That is the **nontarget region**: states from which no target marking is
reachable any more. Whatever wanders in there can never come out to a
legitimate end — a run that enters has to be restarted. The colony sets a
modest repellent pheromone on every edge into the region (it is *set*, never
accumulated, and never evaporates — static knowledge about the explored
model), so runs learn to keep out, while still-sought states (an error
marking, a deadlock, a livelock the board is deliberately heading for) and
states that can still reach unknown territory stay excused.

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
they are covered, because finding them is the point of testing.

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
`--dump-on-deadlock` collects a post-mortem.

### 6.6 Run-time livelocks — `livelock`

The SUT keeps *talking* but nothing progresses: no new state, no stimulus, no
target marking, event after event. With `--livelock-timeout MS` (default 0 =
off) and at least `--livelock-min-events` events (default 3) in the window,
the run ends as a livelock. Real output from `heartbeat-livelock`, whose SUT
can only tick and tock:

```
  run 1    livelock  315 events, 0 inputs, +2 states +2 edges  314 events over 400 ms without progress (no new state, no stimulus, no target marking)
```

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
when the event queue overflows (`--max-events` per run, default 10000), when
a sent input is never acknowledged (`--ack-timeout`, default 5000 ms), when
the SUT stays silent while it owes output (`--idle-timeout`, default
10000 ms), or when the agent cannot start or faults mid-run. The outcome is
`aborted`; a campaign that aborts too many runs in a row stops itself ("too
many aborted runs in a row") and the tool exits with the infrastructure code
3.

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
about the SUT. The same lessons also land in the (marking, T-list) pairs of
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
  channel and the static nontarget region lower it;
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
the (marking, T-list) layer (§7.2) adds its pair channels to `attract` and
`repel` for input edges. A round may expand at most 200 new states.

What the ants find changes the landscape the next real run is steered by:

- **sought targets** deposit 2.0;
- **milestones** deposit 1.5 × rank (§2.3), once per definitely credited
  step — a success on the way, not an end;
- **catastrophes** repel, priced by the way in (§6.10);
- the **nontarget region** is set to 1.0 on every edge into it (§5);
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

### 7.2 The T-list — (marking, T-list) pairs as a finer pheromone basis

Pheromones keyed by *marking* alone have a blind spot: all arrivals at one
situation are lumped together. A catastrophe suffered *via one route*
condemns that marking for *every* route. The T-list is the refinement —
Egge's text, verbatim:

> Yet another feature, which can be optionally on or off: To each marking, add a list of truncated traces, each of which is called a T-list, from which we arrived to the marking. The length of the list is N. By default, N is 4. The Nth element of the T-list is a mini-hash created from the current trace. The (marking, T-list) pairs are now the basis for pheromone generation instead of mere markings.

In plain words: every marking now remembers *how you got there* — not the
whole trace, but a small list of fingerprints of its recent steps. The
**T-list** of an arrival is that list; a **(marking, T-list) pair** is "this
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
truncated to its last N steps — the Nth element of the T-list. Two arrivals
at a marking share the first j elements of their T-lists exactly when their
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

What it changes mechanically: with the layer on (`--tlist N`, 0..8; 0 = off,
the default — Egge's "By default, N is 4" is `--tlist 4`), the session's
milestone and catastrophe deposits also land in a per-pair store under the
arrival pair — flat, capped at `tauMax`, the same amounts as the edge
deposits, evaporating with them, pruned when both channels fall to nothing.
The ants and the suggester weigh an edge under the pair of the arrival
context it would create, so a catastrophe suffered via one route repels only
that route. The edge channels stay exactly as they were: the pairs are a
finer layer *beside* them. And a continued campaign replays the recorded
steps over the same cascade, so the rebuilt pair keys match the live ones.

### 7.3 The carry-over — several rounds saved, merged, toned down

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
  catastrophes, and the (marking, T-list) pairs alike — is scaled by
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

### 8.3 The options, with their defaults

Campaign shape:

| option | default | meaning |
|--------|---------|---------|
| `--max-runs N` | 20 | runs per campaign |
| `--max-events N` | 10000 | events per run (overflow aborts the run) |
| `--threshold N` | 1 | visits after which a state/edge counts as covered |
| `--initial NAME` | all | start only from this initial marking; repeatable |
| `--seed N` | 0 | seed for random tie-breaks; 0 = deterministic order; same seed reproduces a campaign |
| `--no-analyze` | off | skip the static analysis of the model |
| `--max-states N` | 1000000 | limit of the reachability graph (§3.3) |
| `--sync-events` | off | hold the SUT at the model's `"sync"` events until the tracker has handled and acknowledged them (§2.3); needs a plug-in that can acknowledge |

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
| `--tlist N` | 0 (off) | the (marking, T-list) pair layer; list length 0..8 (§7.2) |
| `--restored-pheromones N` | 50 | percent of the earlier rounds' pheromone strength a continued campaign starts with (§7.3) |

Output:

| option | meaning |
|--------|---------|
| `--log FILE` | append every run to FILE; if it exists, continue from it (§4) |
| `--json FILE` | write a machine-readable report |
| `--coverage-elements` | add every known state and edge with its hit counts to `--json` (§10.5) |
| `--dump-pheromone FILE` | write the colony's pheromone — per edge and per (marking, T-list) pair — as JSON (§10.5) |
| `--require-complete` | exit 4 if coverage is not complete |
| `--quiet` | only the final report |
| `--no-color` | plain text |

Inspection (`report` only; §10.5):

| option | meaning |
|--------|---------|
| `--runs` | print every run's recorded steps (a long run keeps its last 20) |
| `--trace N` | print run N's recorded steps in full; N counts from 1 |
| `--dot FILE` | write the graph for Graphviz, coloured by the coverage reached |
| `--tlist N` | rebuild the pair store at the campaign's `--tlist`, so `--dump-pheromone` can show it |

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
  pricing, the milestones. With the T-list and carry-over switches on, the
  `--json` report carries them too (§10.2).
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
`livelock_policy`, `dump_on_deviation`, `dump_on_deadlock`,
`dump_on_livelock`, `tlist`, `restored_pheromones`, `crash_level`,
`crash_attempts`, `crash_predictions`, `hook_timeout`, `hook_runs`,
`overlap`, `time`.
The two guidance switches of the 17 November batch are always present,
e.g. `"tlist": 0, "restored_pheromones": 50`, and so are the three crash keys
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
{"model": "atm", "tlist": 4, "tau_max": 10, "iterations": 37, "ant_walks": 740,
 "edges": [{"from": "...", "transition": "insert_card", "label": "card.insert",
            "to": "...", "attract": 0, "repel": 0.5007, "region": 0}],
 "pairs": [{"state": "confiscated:1, cassette:1, tries:2",
            "tlist": [2597307718, 78764499, ...], "attract": 0, "repel": 2.6599}]}
```

`edges[]` is the per-edge layer (§7), `pairs[]` the (marking, T-list) store
with the key in full (§7.2), empty when the layer is off. Names replace ids on
purpose: edge ids only mean something inside the session that made them.

`report` rebuilds the colony from the log, which records steps but not its
settings — so give it the campaign's `--tlist N`, or the pair store comes out
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
| **deadlock** | a state where nothing can fire — stuck by definition (static: in the graph; run-time: the SUT goes silent there) |
| **livelock** | a cycle without a legitimate exit — static (a terminal cycle) or run-time (busy without progress) |
| **trace** | the ordered steps of one run (§4) |
| **definite step** | a step the tool knows exactly (certain); ambiguous stretches are credited transition-wise only |
| **run / campaign / round** | one encounter with the SUT / all runs of one invocation / one invocation (§4) |
| **run log** | the file every run is appended to; continuing replays it (§4, §7.3) |
| **coverage** | what has been tested so far, counted per transition/place/state/edge; complete after `--threshold` visits |
| **coverage overlap (Jaccard)** | how much two coverages coincide: `|A ∩ B| / |A ∪ B|` per transition, state and edge set, presence only (§10.6) |
| **novelty** | the colony's "unexplored smells interesting": each edge's weight carries `1/(1 + hits)`, and an ant at a state with an uncovered way out deposits 1.0 and ends its walk (§7.1) |
| **target marking** | a situation the campaign seeks; **intentional** ones end the campaign successfully (§5) |
| **nontarget region** | states from which no target marking is reachable — a repellent region (§5) |
| **error marking / error transition** | a declared forbidden situation / step (§6.2, §6.3) |
| **deviation** | an event the model cannot explain (§6.4) |
| **quiescence** | "nothing more will come" — reported after `--quiescence` ms of silence |
| **post-mortem** | a snapshot or a deliberate crash at an error, to examine the SUT's state |
| **aborted** | a run ended by infrastructure trouble, not by the SUT (§6.8) |
| **ant colony** | the steering heuristic: virtual ants walk the graph and lay pheromone (§7.1) |
| **pheromone** | attract/repel deposits on edges that steer later choices; evaporate; capped at `tauMax` |
| **pheromone dump** | the colony written as JSON (`--dump-pheromone`), keyed by markings, transition ids and T-list elements — never by session-local edge ids (§10.5) |
| **milestone** | a transition worth stepping on: deposits 1.5 × rank, never ends a run |
| **catastrophe** | the pheromone lesson of a found fault, priced by the way in (§6.10) |
| **T-list** | the list of truncated traces (mini-hashes of the recent trace) an arrival carries; the (marking, T-list) pairs are a finer pheromone basis (§7.2) |
| **suffix cascade** | the recurrence the T-list elements follow — element k hashes the trace's last k steps (§7.2) |
| **carry-over** | the pheromone teaching of earlier rounds, rebuilt merged and toned down into a continued campaign (§7.3) |
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
