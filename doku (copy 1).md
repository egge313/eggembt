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
  - `final`: legitimate end states.
  - `error`: states that are targets of testing. Each can match `exact` (default) or `covers` (at least these tokens), and carries `policy` (`until_covered`, `once`, `avoid`), `target_hits`, `crash_dump` (`none`, `snapshot`, `crash`) and `max_dumps`.

Example fragment:

```json
"markings": {
  "initial": [{"name": "start", "tokens": {"closed": 1}}],
  "final":   [{"name": "idle_closed", "tokens": {"closed": 1}}],
  "error":   [{"name": "jammed", "match": "covers", "tokens": {"fault": 1},
               "policy": "until_covered", "target_hits": 3, "crash_dump": "snapshot"}]
}
```

The loader never stops at the first problem. It reports every error and warning with a path such as `$.arcs[3].to`, and produces a net only if there are no errors. Ids must be unique across places and transitions; inhibitor arcs must run from a place to a transition; an initial marking above a place bound is an error, a final or error marking above it only a warning. Warnings cover unknown keys (typos), transitions without arcs, and a model without final markings.

## 3. Semantics and the reachability graph

- **Enabling and firing.** A transition is enabled if every normal input place holds at least the arc weight and every inhibitor place holds fewer tokens than its weight. Firing consumes first, then produces.
- **Bounds.** A firing that would exceed a place bound (or overflow 32 bits) is not turned into an edge; it is recorded as a bound violation.
- **Lazy graph.** States are interned markings with dense ids; a state is expanded (its successors computed) only when asked. A state limit (default one million) stops the creation of new states; dropped edges mark the source state incomplete and the graph truncated, so a cut-off frontier is never mistaken for a deadlock. Edges have stable global ids.
- **Deadlock.** A state that was expanded, has nothing enabled, and is neither final nor an error marking.
- **Error versus final.** If a marking matches both, error wins.

### Livelocks: two kinds

1. **Static, in the specification's RG.** A *terminal cycle* is a strongly connected component that contains a cycle, cannot be left by any edge, and contains no final marking. The analysis works on whatever has been explored: edges into unexpanded states count as exits, and a finding is only *certain* if no state of the component is incomplete and none had a firing dropped for a bound. Optionally, an *internal cycle* (a cycle of internal transitions only) is reported as divergence; it is off by default because legitimate environment-like behaviour (an obstruction that appears and clears) forms such cycles.
2. **Run-time, with a settable timeout.** With `livelockTimeoutMs` greater than zero, the tracker declares a livelock when at least `livelockMinEvents` (default 3) observed events in a row bring no progress and the time since the last progress reaches the timeout. Progress is: a stimulus was sent, the belief contains a state not seen earlier in the run, the belief may be in a final marking, or the SUT fell quiet. This is a heuristic: choose the timeout longer than any legitimate autonomous activity of the SUT. Time comes from event timestamps, so replays are deterministic.

Note that a cyclic model without any final marking is, by this definition, one big terminal cycle. The session warns about this and tells you to declare the idle state final.

## 4. Following a live SUT

Not everything the SUT does is visible, so the tracker keeps a **belief set**: all RG states the SUT may be in, closed under silent internal steps.

| Event | Effect |
| --- | --- |
| Observed(label) | Keep the successors, via observable or traced internal transitions with that label. Nothing possible anywhere: **Deviation**. |
| Stimulated(label) | Same with controllable transitions. Not enabled in any possible state: **Preempted** (a silent step got there first; the model says nothing about the reaction; the belief is unchanged and the run goes on). |
| Quiescent | The SUT was silent for the quiescence timeout. Allowed in states with no enabled output or internal step; the belief narrows to those. None: Deviation (missing output). If all remaining states are non-final, non-error dead ends: **Deadlock**. |

The plug-in decides when silence has lasted long enough and sends the Quiescent event, because the tracker has no clock. A deviation names the labels that would have been allowed and says if the explored model may be incomplete there.

### Coverage under ambiguity

The tracker keeps the layers of belief sets. When the belief collapses to one state, and at the end of a run, it builds the graph of all paths consistent with the observations and computes dominators:

- elements on **every** consistent path are credited as definite visits;
- elements on **some** but not all are credited as ambiguous;
- elements on no complete consistent path (ruled out by later events) are not credited.

Definiteness is monotone, so crediting at a collapse point is final. A transition that certainly fired, but from an ambiguous source, is still credited at transition level. Backtraces skip ambiguous stretches; such a trace step has no edge although its depth is greater than one.

## 5. Coverage and run logs

The coverage database lives beside the graph and merges any number of runs. It keeps, per state and edge, definite and ambiguous counts and the first and last visit (run plus trace-tree node); per transition and place, hit counts; per error marking, hits and dumps taken; and a saturation threshold. All runs share one prefix tree of paths, so a backtrace costs one node per distinct prefix.

Runs are persisted as JSON lines by transition id (path runs) or as the live events with capture timestamps (event runs), never by internal state or edge ids, because those depend on expansion order. The log header holds a fingerprint of the graph (independent of declaration order, labels, kinds and final/error markings). Replaying a log into a fresh session rebuilds coverage, backtraces, target phases and dump budgets, and rejects, without recording anything, a run that no longer fits the model.

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

- A livelock is a terminal cycle without a final marking; a cyclic protocol must declare its idle state final.
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
5. Optionally, states from which no final marking is reachable as a further repellent region.
6. PNML import.
7. A reference agent shim for C++ SUTs.
