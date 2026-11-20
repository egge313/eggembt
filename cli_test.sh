#!/bin/sh
# End-to-end tests of the eggembt tool: real processes, pipes and sockets.
#   EGGEMBT=/path/to/eggembt EXAMPLES=/path/to/examples sh tests/cli_test.sh
EGGEMBT=${EGGEMBT:?set EGGEMBT to the eggembt binary}
EXAMPLES=${EXAMPLES:?set EXAMPLES to the examples directory}
ELEVATOR=$EXAMPLES/elevator-door.json
DRINKS=$EXAMPLES/drink-machine.json
HEARTBEAT=$EXAMPLES/heartbeat-livelock.json
# DeepSeek V4.1 Flash: the model of the inspection scenario (doku section 21); its
# campaign leaves both a definite path and an ambiguous stretch behind.
ATM=$EXAMPLES/atm.json
# GLM-5.3-Flash: the PNML import example and the reference agent shim
# (doku section 12, items 6 and 7). REF_AGENT is set by the CMake test
# invocation; manual runs without it skip the reference-agent scenario.
PNML=$EXAMPLES/coffee-pnml.pnml
AGENTMODEL=$EXAMPLES/agents/coffee.json

TMP=$(mktemp -d "${TMPDIR:-/tmp}/eggembt-cli-test.XXXXXX") || exit 99
BG_PIDS=""
cleanup() {
    for p in $BG_PIDS; do kill "$p" 2>/dev/null; done
    rm -rf "$TMP"
}
trap cleanup EXIT
trap 'exit 130' INT TERM

TESTS=0
FAILS=0
CURRENT=""

begin() { CURRENT=$1; TESTS=$((TESTS + 1)); }
fail() { FAILS=$((FAILS + 1)); echo "FAIL: $CURRENT: $*"; }

# run <expected exit code> <command...>   (stdout+stderr in $TMP/out)
run() {
    expected=$1; shift
    "$@" >"$TMP/out" 2>&1
    code=$?
    [ "$code" -eq "$expected" ] || { fail "expected exit $expected, got $code from: $*"; sed 's/^/    | /' "$TMP/out" | head -15; return 1; }
}
has() { grep -q -- "$1" "$TMP/out" || { fail "output lacks \"$1\""; sed 's/^/    | /' "$TMP/out" | head -15; return 1; }; }
hasnt() { ! grep -q -- "$1" "$TMP/out" || fail "output unexpectedly has \"$1\""; }

begin version
# DeepSeek V4.1 Flash: the version follows the git branch (doku section 20).
run 0 "$EGGEMBT" version && has "^eggembt v0[.]0[.]4$"

# GLM-5.3-Flash: the credits, byte for byte as Egge asked for them.
begin credits
run 0 "$EGGEMBT" credits && has "^Egge's Model-Based Tester$" \
    && has "^    Designer: Esa Kettunen$" \
    && has "^    Copyright (c) Esa Kettunen 2026$" \
    && has "^    Software written by artificial intelligence: Claude Sonnet 5.5 Medium$" \
    && has "^    Software written by artificial intelligence: DeepSeek V4.1 Flash$" \
    && has "^    Software written by artificial intelligence: DeepSeek V4[.]1 Flash$"
run 0 "$EGGEMBT" --credits && has "Designer: Esa Kettunen"

begin help
run 0 "$EGGEMBT" help && has "Commands:" && has "agent-sim" && has "export"
run 0 "$EGGEMBT" help run && has "eggembt run MODEL" && has "--simulate" && has "--budget"
run 0 "$EGGEMBT" help export && has "standard PNML"
run 0 "$EGGEMBT" run --help && has "--livelock-timeout"

begin usage-errors
run 2 "$EGGEMBT" && has "Usage:"
run 2 "$EGGEMBT" frobnicate && has "unknown command"
run 2 "$EGGEMBT" check && has "expected exactly one model file"
run 2 "$EGGEMBT" check "$ELEVATOR" --no-such-option && has "unknown option"
run 2 "$EGGEMBT" run "$DRINKS" && has "choose exactly one of"
run 2 "$EGGEMBT" run "$DRINKS" --simulate --max-runs banana && has "whole number"
run 2 "$EGGEMBT" run "$DRINKS" --simulate --dump-on-deviation sometimes && has "none, snapshot or crash"
run 2 "$EGGEMBT" run "$DRINKS" --simulate --sim-weight nonsense=1 && has "no transition"

begin check
run 0 "$EGGEMBT" check "$ELEVATOR" && has "elevator-door" && has "7 transitions" && has "deadlocks: 0"
run 0 "$EGGEMBT" check "$ELEVATOR" --no-explore && hasnt "Reachability graph"
run 0 "$EGGEMBT" check "$HEARTBEAT" && has "livelocks): 1" && has "initial marking lies inside"
run 2 "$EGGEMBT" check "$HEARTBEAT" --strict && has "--strict"
run 2 "$EGGEMBT" check "$TMP/does-not-exist.json" && has "cannot open"

# GLM-5.3-Flash: PNML import (doku section 12, item 6). The tool recognises
# the file by its first byte, imports the eggembt toolspecific conventions and
# warns that PNML cannot declare target markings (so the switch-off deadlock
# is the model's one target).
begin pnml-model
run 0 "$EGGEMBT" check "$PNML" && has "coffee-pnml" && has "5 transitions" && has "deadlocks: 1"
run 0 "$EGGEMBT" check "$PNML" --strict && has "coffee-pnml"
run 0 "$EGGEMBT" run "$PNML" --simulate --quiescence 40 --no-color \
    && has "coverage complete" && has "transitions  5 of 5" && has "milestone transitions 1"

begin export
# DeepSeek V4.1 Flash: PNML export (doku section 27). Without --output the
# document is the only thing on standard output; with it, a summary names the
# file. The standard part re-imports; the eggembt extras travel as comments
# and one machine-readable toolspecific block.
run 0 "$EGGEMBT" export "$DRINKS" && has "<pnml" && has "eggembt annotations" \
    && has "<toolspecific tool=\"eggembt\" version="
run 0 "$EGGEMBT" export "$DRINKS" --output "$TMP/drinks.pnml" && has "PNML written to"
grep -q '<place id="idle">' "$TMP/drinks.pnml" || fail "the exported PNML lacks a place"
grep -q '<arc id="a1" source="idle" target="insert_coin">' "$TMP/drinks.pnml" \
    || fail "the exported PNML has a wrong arc direction"
grep -qF '&quot;target&quot;' "$TMP/drinks.pnml" || fail "the exported PNML lacks the target annotation"
run 0 "$EGGEMBT" check "$TMP/drinks.pnml" --no-explore && has "drink-machine" && has "6 transitions"
run 0 "$EGGEMBT" export "$DRINKS" --strict --output "$TMP/strict.pnml" && has "PNML written"
run 2 "$EGGEMBT" export && has "expected exactly one model file"
run 2 "$EGGEMBT" export "$TMP/does-not-exist.json" && has "cannot open"
run 2 "$EGGEMBT" export "$DRINKS" --output && has "needs a value"

begin check-reports-every-model-error
cat >"$TMP/broken.json" <<'EOF'
{"places":[{"id":"p"}],
 "transitions":[{"id":"t","kind":"observable"}],
 "arcs":[{"from":"p","to":"nowhere"},{"from":"t","to":"p","type":"inhibitor"}],
 "markings":{"initial":[{"name":"s","tokens":{"zzz":1}}]}}
EOF
run 2 "$EGGEMBT" check "$TMP/broken.json"
has '\$\.transitions\[0\]' && has '\$\.arcs\[0\]\.to' && has '\$\.arcs\[1\]\.type' && has 'unknown place'
echo '{"places": [,]}' >"$TMP/syntax.json"
run 2 "$EGGEMBT" check "$TMP/syntax.json" && has "line 1, column"

# GLM-5.3-Flash: the "traced" key is checked: only labeled internal
# transitions may carry it.
cat >"$TMP/traced-bad.json" <<'EOF'
{"places":[{"id":"p"}],
 "transitions":[{"id":"u","kind":"internal","traced":true},
                {"id":"v","kind":"internal","label":"work","traced":false},
                {"id":"w","label":"go","kind":"observable","traced":false}],
 "arcs":[{"from":"p","to":"u"},{"from":"u","to":"p"},
         {"from":"p","to":"v"},{"from":"v","to":"p"},
         {"from":"w","to":"p"}],
 "markings":{"initial":[{"name":"s","tokens":{"p":1}}]}}
EOF
run 2 "$EGGEMBT" check "$TMP/traced-bad.json" \
    && has 'transitions\[0\]\.traced' && has 'transitions\[2\]\.traced' \
    && hasnt 'transitions\[1\]'
# GLM-5.3-Flash: a named but silent internal step changes the kind counts.
cat >"$TMP/traced-good.json" <<'EOF'
{"version":1,"name":"traced-good",
 "places":[{"id":"p"}],
 "transitions":[{"id":"u","kind":"internal","label":"work","traced":false},
                {"id":"v","kind":"internal","label":"busy"}],
 "arcs":[{"from":"p","to":"u"},{"from":"u","to":"p"},{"from":"v","to":"p"}],
 "markings":{"initial":[{"name":"s","tokens":{"p":1}}],"target":[{"name":"f","tokens":{"p":1}}]}}
EOF
run 0 "$EGGEMBT" check "$TMP/traced-good.json" && has "1 traced and 1 silent internal"

begin explore
run 0 "$EGGEMBT" explore "$ELEVATOR" --dot "$TMP/e.dot" && has "6 states, 7 edges" && has "Deadlocks  none"
grep -q "^digraph" "$TMP/e.dot" && grep -q "open_cmd" "$TMP/e.dot" || fail "dot file is wrong"
run 0 "$EGGEMBT" explore "$ELEVATOR" --internal-cycles && has "internal cycle"
run 0 "$EGGEMBT" explore "$HEARTBEAT" && has "terminal cycle" && has "tick:1"
run 2 "$EGGEMBT" explore "$ELEVATOR" --list x && has "whole number"

begin simulated-campaign
run 0 "$EGGEMBT" run "$DRINKS" --simulate --quiescence 40 --no-color && has "coverage complete" && has "transitions  6 of 6"
run 0 "$EGGEMBT" run "$DRINKS" --simulate --quiescence 40 --quiet --json "$TMP/r.json" && hasnt "run 1"
grep -q '"complete":true' "$TMP/r.json" && grep -q '"model":"drink-machine"' "$TMP/r.json" || fail "json report is wrong"

begin no-ants
# GLM-5.3-Flash: the ant colony can be switched off, and the report says which guidance ran.
run 0 "$EGGEMBT" run "$DRINKS" --simulate --quiescence 40 --no-ants --no-color && has "coverage complete" && has "the ant colony is switched off"
run 0 "$EGGEMBT" run "$DRINKS" --simulate --quiescence 40 --no-color && has "ant colony:"
run 0 "$EGGEMBT" run "$DRINKS" --simulate --quiescence 40 --no-ants --quiet --json "$TMP/noants.json" && has "Guidance"
grep -q '"ants":false' "$TMP/noants.json" && grep -q '"ant_iterations":0' "$TMP/noants.json" || fail "json does not report the switched-off colony"
grep -q '"ants":true' "$TMP/r.json" && grep -q '"ant_iterations":[1-9]' "$TMP/r.json" || fail "the default campaign json does not report the ant colony"

begin guidance-in-report
# GLM-5.3-Flash: the remaining guidance switches are reported on the console
# and in the --json file, both when set and when left at their defaults.
run 0 "$EGGEMBT" run "$DRINKS" --simulate --quiescence 40 --no-color --deadlock-policy until_covered \
    --livelock-policy until_covered --dump-on-livelock snapshot --threshold 2 --json "$TMP/guid.json" \
    && has "targets: deadlock until_covered" && has "on livelock snapshot" && has "coverage threshold 2"
grep -q '"deadlock_policy":"until_covered"' "$TMP/guid.json" \
    && grep -q '"livelock_policy":"until_covered"' "$TMP/guid.json" \
    && grep -q '"dump_on_livelock":"snapshot"' "$TMP/guid.json" \
    && grep -q '"coverage_threshold":2' "$TMP/guid.json" \
    || fail "json does not report the non-default guidance switches"
grep -q '"deadlock_policy":"once"' "$TMP/r.json" && grep -q '"dump_on_deviation":"none"' "$TMP/r.json" \
    && grep -q '"coverage_threshold":1' "$TMP/r.json" \
    || fail "json does not report the default guidance switches"

begin tsuffix-and-carryover
# GLM-5.3-Flash: the T-suffix layer (doku section 16) and the carry-over factor
# of the inherited pheromones are guidance switches too, reported the same
# way. --tsuffix N turns the (marking, T-suffix) pairs on, --restored-pheromones
# N sets how much of the earlier rounds' pheromone teaching a continued
# campaign starts with (default 50).
grep -q '"tsuffix":0' "$TMP/r.json" && grep -q '"restored_pheromones":50' "$TMP/r.json" \
    || fail "json does not report the default tsuffix and carry-over switches"
run 0 "$EGGEMBT" run "$DRINKS" --simulate --quiescence 40 --tsuffix 4 --restored-pheromones 25 --no-color \
    && has "coverage complete" && has "transitions  6 of 6"
run 0 "$EGGEMBT" run "$DRINKS" --simulate --quiescence 40 --tsuffix 4 --restored-pheromones 25 --quiet \
    --json "$TMP/tl.json" && has "Guidance"
grep -q '"tsuffix":4' "$TMP/tl.json" && grep -q '"restored_pheromones":25' "$TMP/tl.json" \
    || fail "json does not report the tsuffix and carry-over switches"
run 2 "$EGGEMBT" run "$DRINKS" --simulate --tsuffix 9 && has "whole number"
run 2 "$EGGEMBT" run "$DRINKS" --simulate --tsuffix && has "needs a value"
run 2 "$EGGEMBT" run "$DRINKS" --simulate --restored-pheromones 101 && has "whole number"

# GLM-5.3-Flash: the catastrophe-pheromone pricing is a guidance switch too,
# reported the same way; --flat-catastrophe turns it off.
run 0 "$EGGEMBT" run "$DRINKS" --simulate --quiescence 40 --flat-catastrophe --no-color \
    && has "catastrophes: flat (--flat-catastrophe)" && has "unpredictable transitions 0"
run 0 "$EGGEMBT" run "$DRINKS" --simulate --quiescence 40 --flat-catastrophe --quiet --json "$TMP/flat.json"
grep -q '"catastrophe_time_pricing":false' "$TMP/flat.json" \
    || fail "json does not report the switched-off catastrophe pricing"
grep -q '"catastrophe_time_pricing":true' "$TMP/r.json" \
    || fail "json does not report the default catastrophe pricing"

begin strategy
# DeepSeek V4.1 Flash: the testing strategy (doku section 27). The model
# declares it, `check` and the campaign report name it, the JSON carries it,
# and each half can be overridden on the command line.
cat >"$TMP/strategy.json" <<'EOF'
{"version":1,"name":"strategy",
 "places":[{"id":"s0"},{"id":"s1"}],
 "transitions":[{"id":"go","kind":"controllable","label":"go"},
                {"id":"back","kind":"controllable","label":"back"}],
 "arcs":[{"from":"s0","to":"go"},{"from":"go","to":"s1"},
         {"from":"s1","to":"back"},{"from":"back","to":"s0"}],
 "markings":{"initial":[{"name":"start","tokens":{"s0":1}}],
             "target":[{"name":"done","tokens":{"s1":1}}]},
 "strategy":{"goals":"coverage","errors":"avoid"}}
EOF
run 0 "$EGGEMBT" check "$TMP/strategy.json" --no-explore \
    && has "strategy: goals coverage, errors avoid"
run 0 "$EGGEMBT" run "$TMP/strategy.json" --simulate --quiescence 40 --max-runs 2 --no-color \
    && has "strategy: goals coverage, errors avoid" && has "evaporation: per suggestion"
run 0 "$EGGEMBT" run "$TMP/strategy.json" --simulate --quiescence 40 --max-runs 2 --quiet \
    --json "$TMP/strategy.json.out"
grep -qF '"strategy":{"goals":"coverage","errors":"avoid"}' "$TMP/strategy.json.out" \
    || fail "json does not report the model's strategy"
run 0 "$EGGEMBT" run "$TMP/strategy.json" --simulate --quiescence 40 --max-runs 2 --quiet \
    --goal-priority targets --error-priority seek --json "$TMP/strategy-cli.json"
grep -qF '"strategy":{"goals":"targets","errors":"seek"}' "$TMP/strategy-cli.json" \
    || fail "the command line does not override both strategy halves"
grep -qF '"evaporate_between_runs":false' "$TMP/strategy-cli.json" \
    || fail "json does not report the default evaporation boundary"
grep -qF '"time_budget_ms":0' "$TMP/strategy-cli.json" \
    || fail "json does not report the default (unlimited) budget"
run 2 "$EGGEMBT" run "$TMP/strategy.json" --simulate --goal-priority fast \
    && has "balanced, targets or coverage"
run 2 "$EGGEMBT" run "$TMP/strategy.json" --simulate --error-priority ignore \
    && has "balanced, seek or avoid"

begin satisfied-target-policy
# DeepSeek V4.1 Flash: the Seeking -> Forbidden veto is optional (doku
# section 28). The default stays veto; the report and JSON name the policy.
run 0 "$EGGEMBT" run "$DRINKS" --simulate --quiescence 40 --no-color \
    && has "satisfied targets: veto"
run 0 "$EGGEMBT" run "$DRINKS" --simulate --quiescence 40 --satisfied-target-policy penalise --no-color \
    && has "satisfied targets: penalise"
run 0 "$EGGEMBT" run "$DRINKS" --simulate --quiescence 40 --satisfied-target-policy ignore --quiet \
    --json "$TMP/satisfied.json"
grep -qF '"satisfied_target_policy":"ignore"' "$TMP/satisfied.json" \
    || fail "json does not report the satisfied-target policy"
run 2 "$EGGEMBT" run "$DRINKS" --simulate --satisfied-target-policy maybe \
    && has "veto, penalise or ignore"

begin budget
# DeepSeek V4.1 Flash: the wall-clock budget (doku section 27). A tiny budget
# stops the campaign at a normal run boundary; the exit code stays 0 and the
# report says why. A generous one changes nothing. Bad durations are usage
# errors.
run 0 "$EGGEMBT" run "$DRINKS" --simulate --quiescence 40 --max-runs 50 --budget 1ms --no-color \
    && has "time budget reached"
run 0 "$EGGEMBT" run "$DRINKS" --simulate --quiescence 40 --max-runs 50 --budget 1ms --quiet \
    --json "$TMP/budget.json"
grep -qF '"stop_reason":"time budget reached"' "$TMP/budget.json" \
    || fail "json does not report the budget stop"
grep -qF '"time_budget_ms":1' "$TMP/budget.json" || fail "json does not report the budget"
grep -qF '"budget_reached":' "$TMP/budget.json" || fail "json lacks per-run budget_reached"
run 0 "$EGGEMBT" run "$DRINKS" --simulate --quiescence 40 --max-runs 2 --budget 5s --no-color \
    && has "coverage complete" && hasnt "time budget reached"
run 0 "$EGGEMBT" run "$DRINKS" --simulate --quiescence 40 --max-runs 1 --budget 1500ms --quiet
run 2 "$EGGEMBT" run "$DRINKS" --simulate --budget 0 && has "positive duration"
run 2 "$EGGEMBT" run "$DRINKS" --simulate --budget banana && has "positive duration"
run 2 "$EGGEMBT" run "$DRINKS" --simulate --budget 2d && has "positive duration"

begin evaporation-between-runs
# DeepSeek V4.1 Flash: aging at run boundaries instead of every suggestion
# (doku section 27). The ants still walk; the console and the JSON name the
# boundary. The measured effect is pinned in tst_pheromone.
run 0 "$EGGEMBT" run "$DRINKS" --simulate --quiescence 40 --evaporate-between-runs --no-color \
    && has "evaporation: between runs (--evaporate-between-runs)" && has "coverage complete"
run 0 "$EGGEMBT" run "$DRINKS" --simulate --quiescence 40 --evaporate-between-runs --quiet \
    --json "$TMP/evaporation.json"
grep -qF '"evaporate_between_runs":true' "$TMP/evaporation.json" \
    || fail "json does not report the run-boundary aging"

begin good-target
# GLM-5.3-Flash: an intentional target marking is a good target state: a normal
# target whose certain reach ends the campaign. The report names it per run.
cat >"$TMP/goodtarget.json" <<'EOF'
{"version":1,"name":"good-target",
 "places":[{"id":"s0"},{"id":"s1"}],
 "transitions":[{"id":"go","kind":"controllable","label":"go"},
                {"id":"back","kind":"controllable","label":"back"}],
 "arcs":[{"from":"s0","to":"go"},{"from":"go","to":"s1"},
         {"from":"s1","to":"back"},{"from":"back","to":"s0"}],
 "markings":{"initial":[{"name":"start","tokens":{"s0":1}}],
             "target":[{"name":"reached","tokens":{"s1":1},"intentional":true}]}}
EOF
run 0 "$EGGEMBT" run "$TMP/goodtarget.json" --simulate --quiescence 40 --no-color --json "$TMP/gf.json" \
    && has "the good target marking 'reached' was reached"
grep -q '"good_target":"reached"' "$TMP/gf.json" \
    && grep -q "the good target marking 'reached' was reached" "$TMP/gf.json" \
    || fail "json does not report the good target state"

begin crash-predictions
# GLM-5.3-Flash: the crash predictions (doku section 17). The model predicts a
# nondeterministic crash at a stimulus; the simulated SUT is seeded to die
# there with the prediction's prior mean (--sim-seed reproduces it). The
# experiment seeks the crash until it is certainly deterministic, then the way
# in is marked as repellent and the campaign completes. In the --json file the
# two knobs and every entry's experiment state are reported.
cat >"$TMP/crashy.json" <<'EOF'
{"version":1,"name":"crashy",
 "places":[{"id":"ready"},{"id":"over"}],
 "transitions":[{"id":"strike","label":"strike","kind":"controllable",
                 "crash":{"class":"always","crash_dump":"snapshot","max_dumps":1}},
                {"id":"stop","label":"stop","kind":"controllable"}],
 "arcs":[{"from":"ready","to":"strike"},{"from":"strike","to":"ready"},
         {"from":"ready","to":"stop"},{"from":"stop","to":"over"}],
 "markings":{"initial":[{"name":"start","tokens":{"ready":1}}],
             "target":[{"name":"idle","tokens":{"ready":1}}]}}
EOF
run 0 "$EGGEMBT" run "$TMP/crashy.json" --simulate --seed 7 --sim-seed 3 --max-runs 12 --no-color \
    && has "the crash was attributed to t:strike (deterministic: 1 attempt(s), 1 crash(es))" \
    && has "Crash predictions" && has "t:strike    always      deterministic  1         1        0 / 1"
run 0 "$EGGEMBT" run "$TMP/crashy.json" --simulate --seed 7 --sim-seed 3 --max-runs 12 --quiet --json "$TMP/cr.json"
grep -q '"crash_level":0.94' "$TMP/cr.json" && grep -q '"crash_attempts":6' "$TMP/cr.json" \
    && grep -q '"prediction":"always"' "$TMP/cr.json" && grep -q '"status":"deterministic"' "$TMP/cr.json" \
    || fail "json does not report the crash experiment"

# GLM-5.3-Flash: a death nobody predicted gets an implicit entry at the last
# credited step. --sim-crash-after N kills the simulated SUT mid-run wherever
# it is; the experiment counts one attempt per run and gives up at its budget.
cat >"$TMP/plain.json" <<'EOF'
{"version":1,"name":"plain",
 "places":[{"id":"a"},{"id":"b"}],
 "transitions":[{"id":"go","label":"go","kind":"controllable"},
                {"id":"back","label":"back","kind":"controllable"}],
 "arcs":[{"from":"a","to":"go"},{"from":"go","to":"b"},
         {"from":"b","to":"back"},{"from":"back","to":"a"}],
 "markings":{"initial":[{"name":"start","tokens":{"a":1}}],
             "target":[{"name":"visited","tokens":{"b":1}}]}}
EOF
run 0 "$EGGEMBT" run "$TMP/plain.json" --simulate --sim-seed 5 --sim-crash-after 2 --max-runs 8 --no-color \
    && has "the crash was attributed to t:back (exhausted: 6 attempt(s), 5 crash(es))" \
    && has "the ant colony marked the crash site as repellent" \
    && has "t:back      implicit    exhausted  6         5        -"

begin command-hooks
# GLM-5.3-Flash: the command hooks (doku section 18). A synchronous hook's
# output is captured into the run record, an asynchronous one is launched and
# left alone, the guidance line names the model's hooks and their bound, and
# the executions travel in the run log (the report shows them after a restore;
# the commands are not run again).
cat >"$TMP/hooky.json" <<'EOF'
{"version":1,"name":"hooky",
 "places":[{"id":"a"},{"id":"b"}],
 "transitions":[{"id":"go","label":"go","kind":"controllable",
                 "command":"printf 'hooked %s\\n' \"$EGGEMBT_TRANSITION\""},
                {"id":"back","label":"back","kind":"controllable",
                 "command":{"run":"true","mode":"async"}}],
 "arcs":[{"from":"a","to":"go"},{"from":"go","to":"b"},
         {"from":"b","to":"back"},{"from":"back","to":"a"}],
 "markings":{"initial":[{"name":"start","tokens":{"a":1}}],
             "target":[{"name":"visited","tokens":{"b":1}}]}}
EOF
run 0 "$EGGEMBT" run "$TMP/hooky.json" --simulate --quiescence 40 --no-color \
    && has "Command hooks" && has "hooked go" && has "launched" \
    && has "commands: 2 transition(s), timeout 60 s"
run 0 "$EGGEMBT" run "$TMP/hooky.json" --simulate --quiescence 40 --quiet --json "$TMP/hooks.json"
grep -q '"hook_timeout":60' "$TMP/hooks.json" && grep -q '"hook_runs":\[' "$TMP/hooks.json" \
    && grep -q '"status":"ok"' "$TMP/hooks.json" && grep -q '"status":"launched"' "$TMP/hooks.json" \
    || fail "json does not report the hook runs"
run 2 "$EGGEMBT" run "$TMP/hooky.json" --simulate --hook-timeout 0 && has "needs a whole number from 1 to 3600"
run 0 "$EGGEMBT" run "$TMP/hooky.json" --simulate --quiescence 40 --hook-timeout 2 --no-color --log "$TMP/h.log" \
    && has "commands: 2 transition(s), timeout 2 s"
run 0 "$EGGEMBT" report "$TMP/hooky.json" --log "$TMP/h.log" --no-color \
    && has "Command hooks" && has "hooked go" && has "launched"

begin atm-visit
# GLM-5.3-Flash: the worked ATM example exercises the whole feature set on one
# model: three initial markings (several ATMs, differing cash cassettes, so
# the round-robin rotation is visible in the run log), an error TRANSITION per
# failing component (session, database, ATM -- each caught with a crash-dump
# post-mortem), a stolen-card branch announced by a traced internal, a
# swallowed card as an error MARKING target, an unpredictable human-typed PIN
# step, and an intentional good target that needs a real withdrawal and ends
# the campaign. With --sim-seed 5 the story is deterministic: 37 runs.
run 0 "$EGGEMBT" check "$EXAMPLES/atm.json" && has "atm" && has "initial markings: atm-lobby, atm-drivein, atm-branch" \
    && has "target markings:  visit-done, visit-aborted" && has "error markings:   card-swallowed"
rm -f "$TMP/atm.jsonl"
run 0 "$EGGEMBT" run "$EXAMPLES/atm.json" --simulate --sim-seed 5 --max-runs 45 --quiescence 40 \
    --log "$TMP/atm.jsonl" --quiet --json "$TMP/atm.json" \
    && has "the good target marking 'visit-done' was reached"
grep -q '"stop_reason":"the good target marking '"'"'visit-done'"'"' was reached"' "$TMP/atm.json" \
    || fail "json does not name the good-target stop reason"
grep -q '"good_target":"visit-done"' "$TMP/atm.json" || fail "json does not name the reached good target"
for t in confiscate session_fail db_fail atm_fail; do
    grep -q "\"id\":\"$t\"" "$TMP/atm.json" || fail "json does not report error transition $t"
done
grep -q '"name":"card-swallowed".*"satisfied":true' "$TMP/atm.json" \
    && grep -q '"dumps":1' "$TMP/atm.json" \
    || fail "the swallowed-card error marking was not hit and dumped"
grep -q '"catastrophe_time_pricing":true' "$TMP/atm.json" || fail "json does not report the catastrophe pricing"
# the rotation over the three ATMs is visible in the run log
for a in atm-lobby atm-drivein atm-branch; do
    grep -q "\"initial\":\"$a\"" "$TMP/atm.jsonl" || fail "the run log never started at $a"
done
# and the crash post-mortems are on the console
run 0 "$EGGEMBT" run "$EXAMPLES/atm.json" --simulate --sim-seed 5 --max-runs 45 --quiescence 40 --no-color \
    && has "Post-mortems" && has "dump-" && has "unpredictable transitions 1" \
    && has "priced by the way in"

begin state-limit
# GLM-5.3-Flash: a graph cut off by --max-states is announced, never mistaken
# for a deadlock, and the campaign still covers everything it can reach.
cat >"$TMP/counter.json" <<'EOF'
{"version":1,"name":"counter",
 "places":[{"id":"tokens","bound":20}],
 "transitions":[{"id":"add","kind":"controllable","label":"add"}],
 "arcs":[{"from":"tokens","to":"add"},{"from":"add","to":"tokens","weight":2}],
 "markings":{"initial":[{"name":"start","tokens":{"tokens":1}}]}}
EOF
run 0 "$EGGEMBT" explore "$TMP/counter.json" --max-states 4 && has "4 states, 3 edges" && has "incomplete: state limit"
run 0 "$EGGEMBT" run "$TMP/counter.json" --simulate --quiescence 40 --max-states 4 --no-color \
    && has "not explored completely" && has "coverage complete"

begin seed
# GLM-5.3-Flash: --seed steers the tie-breaks of the input choice. A bad seed
# is a usage error; the same seed reproduces a campaign byte for byte in the
# JSON report; the report names both seeds (selection and ants). The byte
# compare uses the counter model, whose single input and lack of autonomous
# SUT moves make the report deterministic (the drink machine's simulated
# outputs race the poller, which may legitimately change the campaign).
run 2 "$EGGEMBT" run "$DRINKS" --simulate --seed banana && has "whole number"
# GLM-5.3-Flash: the report now also carries wall-clock times, which may
# (legitimately) differ between two identical campaigns; the reproducibility
# claim is about the SELECTION, so the comparison strips the time fields.
norm() { sed -E 's/"(duration_ms|wall_ms|mean_run_ms|events_per_s)":[0-9.]+/"\1":0/g' "$1"; }
run 0 "$EGGEMBT" run "$TMP/counter.json" --simulate --quiescence 40 --seed 7 --quiet --json "$TMP/seed-a.json" >/dev/null
run 0 "$EGGEMBT" run "$TMP/counter.json" --simulate --quiescence 40 --seed 7 --quiet --json "$TMP/seed-b.json" >/dev/null
norm "$TMP/seed-a.json" >"$TMP/seed-a.norm" && norm "$TMP/seed-b.json" >"$TMP/seed-b.norm"
cmp -s "$TMP/seed-a.norm" "$TMP/seed-b.norm" || fail "the same seed produced different reports"
grep -q '"seed":7' "$TMP/seed-a.json" || fail "json does not report the selection seed"
grep -q '"ant_seed":1' "$TMP/seed-a.json" || fail "json does not report the ant seed"

begin time-and-overlap
# GLM-5.3-Flash: testing time next to coverage, and how much the runs of a
# campaign overlap.
run 0 "$EGGEMBT" run "$ELEVATOR" --simulate --quiescence 40 --max-runs 3 --no-color \
    && has "Time  campaign" && has "events/s" && has "mean pairwise Jaccard"
run 0 "$EGGEMBT" run "$ELEVATOR" --simulate --quiescence 40 --max-runs 3 --quiet --json "$TMP/time.json"
grep -q '"duration_ms":[0-9]' "$TMP/time.json" && grep -q '"wall_ms":[0-9]' "$TMP/time.json" \
    && grep -q '"events_per_s":[0-9]' "$TMP/time.json" && grep -q '"mean_pairwise"' "$TMP/time.json" \
    || fail "json lacks the time and overlap criteria"

begin compare
# GLM-5.3-Flash: the mutual overlap of two campaigns' coverage (Jaccard
# index): the same log against itself is 100%, two logs compare per run pair,
# and --csv writes every pair for plots.
run 0 "$EGGEMBT" run "$ELEVATOR" --simulate --quiescence 40 --max-runs 3 --seed 1 --quiet --log "$TMP/cmp-a.jsonl" >/dev/null
run 0 "$EGGEMBT" compare "$ELEVATOR" "$TMP/cmp-a.jsonl" "$TMP/cmp-a.jsonl" --no-color \
    && has "campaigns as a whole: transitions 100%"
run 0 "$EGGEMBT" run "$ELEVATOR" --simulate --quiescence 40 --max-runs 3 --seed 2 --quiet --log "$TMP/cmp-b.jsonl" >/dev/null
run 0 "$EGGEMBT" compare "$ELEVATOR" "$TMP/cmp-a.jsonl" "$TMP/cmp-b.jsonl" --no-color --csv "$TMP/ov.csv" \
    && has "Overlap" && has "run against run" && has "transition overlap"
grep -q '^group,i,j,transitions,states,edges' "$TMP/ov.csv" || fail "csv lacks its header"
grep -q '^cross,0,0,' "$TMP/ov.csv" || fail "csv lacks cross rows"
grep -q '^within-a,0,1,' "$TMP/ov.csv" || fail "csv lacks within rows"

begin require-complete
run 4 "$EGGEMBT" run "$ELEVATOR" --simulate --quiescence 40 --max-runs 1 --require-complete --sim-weight jam=0 --sim-weight obstruct=0 --sim-weight clear=0 && has "transitions not definitely covered\|Transitions not definitely covered"

begin deviation-is-a-failure
run 1 "$EGGEMBT" run "$DRINKS" --simulate --quiescence 40 --sim-deviate-after 2 --max-runs 1 && has "deviation" && has "Findings"

begin snapshot-dump-of-a-jam
run 0 "$EGGEMBT" run "$ELEVATOR" --simulate --quiescence 40 --max-runs 2 --sim-weight obstruct=0 --sim-weight clear=0 --sim-weight closed_ev=0 && has "post-mortem" && has "Post-mortems" && has "dump-1.snapshot"
run 0 "$EGGEMBT" run "$ELEVATOR" --simulate --quiescence 40 --max-runs 1 --sim-weight obstruct=0 --sim-weight clear=0 --sim-weight closed_ev=0 --sim-no-snapshot && has "cannot snapshot"

begin run-log-continues-and-reports
rm -f "$TMP/runs.jsonl"
run 0 "$EGGEMBT" run "$DRINKS" --simulate --quiescence 40 --log "$TMP/runs.jsonl" && has "coverage complete"
[ -s "$TMP/runs.jsonl" ] || fail "no run log was written"
run 0 "$EGGEMBT" run "$DRINKS" --simulate --quiescence 40 --log "$TMP/runs.jsonl" && has "earlier run(s) restored" && has "coverage complete"
# GLM-5.3-Flash: the report also traces each covered transition back to its
# first definite visit (doku section 12, item 2 -- the GUI half of the item
# was closed as a textual report: coverage and backtraces, no GUI).
run 0 "$EGGEMBT" report "$DRINKS" --log "$TMP/runs.jsonl" \
    && has "2 of 2 runs replayed" && has "transitions  6 of 6" \
    && has "Backtraces" && has "start > coin.insert"
run 2 "$EGGEMBT" run "$ELEVATOR" --simulate --log "$TMP/runs.jsonl" && has "different model"
run 2 "$EGGEMBT" report "$ELEVATOR" --log "$TMP/runs.jsonl" && has "different model"
run 2 "$EGGEMBT" report "$DRINKS" --log "$TMP/missing.jsonl" && has "cannot open"

begin agent-as-child-process
run 0 "$EGGEMBT" run "$DRINKS" --agent "$EGGEMBT agent-sim $DRINKS" --quiescence 40 && has "eggembt-agent-sim" && has "coverage complete" && has "transitions  6 of 6"

# GLM-5.3-Flash: the reference agent shim (doku section 12, item 7): a
# hand-written C++ SUT wrapped in ISutPlugin + AgentServer, spawned by
# 'run --agent'. Skipped when the binary was not built (manual runs).
begin reference-agent
if [ -n "$REF_AGENT" ] && [ -x "$REF_AGENT" ]; then
    run 0 "$EGGEMBT" run "$AGENTMODEL" --agent "$REF_AGENT" --quiescence 40 \
        && has "eggembt-ref-agent" && has "coverage complete" && has "transitions  6 of 6"
else
    echo "skipped: REF_AGENT not set"
fi

begin agent-that-cannot-be-started
run 3 "$EGGEMBT" run "$DRINKS" --agent "/no/such/agent" && has "cannot run"

begin agent-that-says-nothing
run 3 "$EGGEMBT" run "$DRINKS" --agent "sleep 30" --hello-timeout 300 && has "did not say hello"

begin agent-that-dies-mid-run
# says hello, accepts the start, then exits: the run must abort, not hang
cat >"$TMP/dying-agent.sh" <<'EOF'
#!/bin/sh
echo '{"type":"hello","protocol":1,"name":"dying","snapshot":false,"crash":false,"restart_ms":0}'
read line
echo '{"type":"run_started","ok":true}'
exit 0
EOF
run 3 "$EGGEMBT" run "$DRINKS" --agent "sh $TMP/dying-agent.sh" --max-runs 3 --poll 5 --idle-timeout 3000 && has "connection to the agent" && has "aborted"

begin tcp-tool-listens-agent-connects
"$EGGEMBT" run "$DRINKS" --listen 127.0.0.1:0 --quiescence 40 --accept-timeout 20 >"$TMP/listen.out" 2>&1 &
LISTEN_PID=$!
BG_PIDS="$BG_PIDS $LISTEN_PID"
PORT=""
i=0
while [ -z "$PORT" ] && [ $i -lt 100 ]; do
    PORT=$(sed -n 's/.*Waiting for an agent on 127.0.0.1:\([0-9][0-9]*\).*/\1/p' "$TMP/listen.out" | head -1)
    [ -z "$PORT" ] && sleep 0.1
    i=$((i + 1))
done
if [ -z "$PORT" ]; then fail "the tool never reported its port"; else
    "$EGGEMBT" agent-sim "$DRINKS" --connect "127.0.0.1:$PORT" 2>"$TMP/agent.err" &
    AGENT_PID=$!
    BG_PIDS="$BG_PIDS $AGENT_PID"
    wait "$LISTEN_PID"; code=$?
    [ "$code" -eq 0 ] || { fail "run --listen exited with $code"; sed 's/^/    | /' "$TMP/listen.out" | head; }
    grep -q "coverage complete" "$TMP/listen.out" || fail "run --listen did not complete"
    wait "$AGENT_PID" 2>/dev/null
fi

begin tcp-agent-listens-tool-connects
"$EGGEMBT" agent-sim "$DRINKS" --listen 127.0.0.1:0 --accept-timeout 20 2>"$TMP/agent2.err" &
AGENT_PID=$!
BG_PIDS="$BG_PIDS $AGENT_PID"
PORT=""
i=0
while [ -z "$PORT" ] && [ $i -lt 100 ]; do
    PORT=$(sed -n 's/.*listening on 127.0.0.1:\([0-9][0-9]*\).*/\1/p' "$TMP/agent2.err" | head -1)
    [ -z "$PORT" ] && sleep 0.1
    i=$((i + 1))
done
if [ -z "$PORT" ]; then fail "the agent never reported its port"; else
    run 0 "$EGGEMBT" run "$DRINKS" --connect "127.0.0.1:$PORT" --quiescence 40 && has "coverage complete"
    wait "$AGENT_PID" 2>/dev/null
fi
run 3 "$EGGEMBT" run "$DRINKS" --connect "127.0.0.1:1" && has "cannot connect"

begin stdio-protocol-over-named-pipes
mkfifo "$TMP/to-agent" "$TMP/from-agent" 2>/dev/null
if [ -p "$TMP/to-agent" ]; then
    # Opening a named pipe blocks until the other end is opened: the two sides must open them in opposite
    # orders (the agent its output first, the tool its input first), or both wait forever.
    "$EGGEMBT" agent-sim "$DRINKS" >"$TMP/from-agent" <"$TMP/to-agent" 2>/dev/null &
    BG_PIDS="$BG_PIDS $!"
    "$EGGEMBT" run "$DRINKS" --stdio --quiescence 40 --no-color <"$TMP/from-agent" >"$TMP/to-agent" 2>"$TMP/stdio.out"
    code=$?
    [ "$code" -eq 0 ] || { fail "run --stdio exited with $code"; sed 's/^/    | /' "$TMP/stdio.out" | head; }
    grep -q "coverage complete" "$TMP/stdio.out" || fail "run --stdio did not complete"
else
    echo "skipped: no named pipes"
fi

begin sync-events
# DeepSeek V4.1 Flash: synchronized events (doku section 19): the SUT is held at the
# model's "sync" event until the tracker acknowledged it. The simulated SUT
# can hold itself; without --sync-events the same model runs unheld.
cat >"$TMP/sync.json" <<'EOF'
{
  "places": [{"id":"idle","bound":1},{"id":"busy"},{"id":"done","bound":1}],
  "transitions": [
    {"id":"go","label":"go","kind":"controllable"},
    {"id":"out","label":"out","kind":"observable","sync":true}
  ],
  "arcs": [
    {"from":"idle","to":"go"},{"from":"go","to":"busy"},
    {"from":"busy","to":"out"},{"from":"out","to":"done"}
  ],
  "markings": {"initial":[{"name":"start","tokens":{"idle":1}}],
               "target":[{"name":"finished","tokens":{"done":1}}]}
}
EOF
run 0 "$EGGEMBT" run "$TMP/sync.json" --simulate --quiescence 40 --sync-events \
    && has "sync: 1 transition(s), held until acknowledged (--sync-events)" \
    && has "coverage complete"
run 0 "$EGGEMBT" run "$TMP/sync.json" --simulate --quiescence 40 \
    && has "sync: 1 transition(s), not held (no --sync-events)"
run 0 "$EGGEMBT" run "$TMP/sync.json" --agent "$EGGEMBT agent-sim $TMP/sync.json" --quiescence 40 --sync-events \
    && has "eggembt-agent-sim" && has "coverage complete"
run 0 "$EGGEMBT" run "$TMP/sync.json" --simulate --quiescence 40 --sync-events --json "$TMP/sync-report.json" \
    && has "coverage complete"
grep -q '"sync_events":true' "$TMP/sync-report.json" || fail "the --json report lacks sync_events"
grep -q '"sync_transitions":1' "$TMP/sync-report.json" || fail "the --json report lacks the sync transition count"

begin rare
# DeepSeek V4.1 Flash: rare transitions and the hunt (doku section 30). The
# marker is a model property; --hunt-rare makes the campaign dwell at the
# enabling state, spend its attempt and report both on the console and in JSON.
cat >"$TMP/rare.json" <<'EOF'
{"version":1,"name":"rare",
 "places":[{"id":"s0"},{"id":"s1"}],
 "transitions":[{"id":"go","kind":"controllable","label":"go"},
                {"id":"rare_go","kind":"controllable","label":"rare.go",
                 "rare":{"p":0.001,"attempts":2,"wait-for":30}},
                {"id":"back","kind":"controllable","label":"back"}],
 "arcs":[{"from":"s0","to":"go"},{"from":"go","to":"s1"},
         {"from":"s0","to":"rare_go"},{"from":"rare_go","to":"s1"},
         {"from":"s1","to":"back"},{"from":"back","to":"s0"}],
 "markings":{"initial":[{"name":"start","tokens":{"s0":1}}],
             "target":[{"name":"idle","tokens":{"s0":1}}]}}
EOF
run 0 "$EGGEMBT" help run && has "--hunt-rare"
run 0 "$EGGEMBT" check "$TMP/rare.json" --no-explore \
    && has "rare transitions: 1 (rare_go: p 0.001, 2 attempts, wait 30 ms)"
run 0 "$EGGEMBT" run "$TMP/rare.json" --simulate --quiescence 40 --hunt-rare --no-color \
    --json "$TMP/rare-report.json" \
    && has "rare: 1 transition(s), 1 seen, 1 attempt(s) used; hunting (--hunt-rare)" \
    && has "coverage complete"
grep -q '"hunt_rare":true' "$TMP/rare-report.json" || fail "the --json report lacks hunt_rare"
grep -q '"transition":"rare_go"' "$TMP/rare-report.json" || fail "the --json report lacks the rare transition"
grep -q '"attempts_used":1' "$TMP/rare-report.json" || fail "the --json report lacks the rare attempt"
grep -q '"seen":true' "$TMP/rare-report.json" || fail "the --json rare summary does not say seen"
# Without the switch the marker is inert: the input is still sent as ordinary
# coverage, but no dwell and no attempt.
run 0 "$EGGEMBT" run "$TMP/rare.json" --simulate --quiescence 40 --no-color \
    --json "$TMP/rare-off.json" \
    && has "rare: 1 transition(s), 1 seen, 0 attempt(s) used; not hunted (add --hunt-rare)" \
    && has "coverage complete"
grep -q '"hunt_rare":false' "$TMP/rare-off.json" || fail "the --json report does not say hunting is off"
grep -q '"attempts_used":0' "$TMP/rare-off.json" || fail "the inert marker still spent an attempt"

begin inspection
# DeepSeek V4.1 Flash: post-campaign inspection (doku section 21). The views read
# the recording -- the traces and the coverage from the log's own steps, the
# pheromone rebuilt from it -- so inspecting never changes the campaign.
run 0 "$EGGEMBT" run "$ATM" --simulate --max-runs 4 --tsuffix 4 --log "$TMP/inspect.log" \
    --json "$TMP/inspect.json" --coverage-elements --dump-pheromone "$TMP/inspect-pheromone.json" \
    && has "Pheromone written to"
grep -q '"states":\[' "$TMP/inspect.json" || fail "--coverage-elements lacks the states"
grep -q '"covered":true' "$TMP/inspect.json" || fail "--coverage-elements lacks the covered flag"
grep -q '"edges":\[' "$TMP/inspect.json" || fail "--coverage-elements lacks the edges"
grep -q '"transition":"' "$TMP/inspect.json" || fail "--coverage-elements lacks the transitions"
grep -q '"tsuffix":[1-9]' "$TMP/inspect-pheromone.json" || fail "the pheromone dump lacks the T-suffix length"
grep -q '"pairs":\[{' "$TMP/inspect-pheromone.json" || fail "the pheromone dump lacks the pair store"
grep -q '"attract":' "$TMP/inspect-pheromone.json" || fail "the pheromone dump lacks the channels"
grep -q 'GLM-5' "$TMP/inspect-pheromone.json" && fail "the pheromone dump leaks the signature"
# The traces come from the log, not from a re-simulation. The backtrace table
# widens its name column to the longest label, so a long one keeps a space
# before its chain (the ATM's bank.stolen_report is longer than the old 16).
run 0 "$EGGEMBT" report "$ATM" --log "$TMP/inspect.log" --runs \
    && has "the recorded steps of every run" && has "  run 1 " \
    && has "bank.stolen_report atm-drivein > card.insert"
run 0 "$EGGEMBT" report "$ATM" --log "$TMP/inspect.log" --trace 1 \
    && has "run 1, oldest first" && has "card.insert"
run 0 "$EGGEMBT" report "$ATM" --log "$TMP/inspect.log" --trace 99 && has "no run 99"
run 2 "$EGGEMBT" report "$ATM" --log "$TMP/inspect.log" --trace 0 && has "whole number"
run 2 "$EGGEMBT" report "$ATM" --log "$TMP/inspect.log" --trace banana && has "whole number"
# The same log drawn and dumped again: the graph coloured by the coverage and
# the pair store rebuilt at the campaign's --tsuffix.
run 0 "$EGGEMBT" report "$ATM" --log "$TMP/inspect.log" --dot "$TMP/inspect.dot" \
    --coverage-elements --tsuffix 4 --dump-pheromone "$TMP/inspect-pheromone2.json" \
    --json "$TMP/inspect-report.json" \
    && has "Graph written" && has "Pheromone written"
grep -q 'penwidth=3' "$TMP/inspect.dot" || fail "the coverage drawing has no threshold-reached element"
grep -q 'color="#2e7d32"' "$TMP/inspect.dot" || fail "the coverage drawing has no green element"
grep -q 'coverage: green = visited' "$TMP/inspect.dot" || fail "the coverage drawing lacks its legend"
grep -q '"states":\[' "$TMP/inspect-report.json" || fail "report --coverage-elements lacks the states"
grep -q '"pairs":\[{' "$TMP/inspect-pheromone2.json" || fail "the rebuilt pair store is empty"

begin livelock-detection-at-run-time
# the SUT can only tick and tock; with a short livelock timeout the run must end as a livelock
run 1 "$EGGEMBT" run "$HEARTBEAT" --simulate --quiescence 100 --livelock-timeout 150 --max-runs 1 --sim-step-delay 20 && has "livelock"

echo
echo "$TESTS tests, $FAILS failure(s)"
[ "$FAILS" -eq 0 ]
