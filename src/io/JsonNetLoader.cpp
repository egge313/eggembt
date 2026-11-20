#include "io/JsonNetLoader.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <initializer_list>
#include <map>
#include <set>
#include <sstream>

namespace pn {

// ---------------------------------------------------------------------------
// LoadResult
// ---------------------------------------------------------------------------

bool LoadResult::hasErrors() const
{
    return std::any_of(issues.begin(), issues.end(),
                       [](const LoadIssue &i) { return i.severity == LoadIssue::Severity::Error; });
}

bool LoadResult::hasWarnings() const
{
    return std::any_of(issues.begin(), issues.end(),
                       [](const LoadIssue &i) { return i.severity == LoadIssue::Severity::Warning; });
}

std::string LoadResult::report() const
{
    std::string out;
    for (const LoadIssue &i : issues) {
        if (!out.empty())
            out += '\n';
        out += (i.severity == LoadIssue::Severity::Error ? "error " : "warning ") + i.path + ": " + i.message;
    }
    return out;
}

// ---------------------------------------------------------------------------
// Helpers (file-local)
// ---------------------------------------------------------------------------

namespace {

using Severity = LoadIssue::Severity;

class Diag {
public:
    void error(const std::string &path, const std::string &message)
    {
        m_issues.push_back({Severity::Error, path, message});
        ++m_errors;
    }
    void warning(const std::string &path, const std::string &message)
    {
        m_issues.push_back({Severity::Warning, path, message});
    }
    bool hasErrors() const { return m_errors > 0; }
    std::vector<LoadIssue> take() { return std::move(m_issues); }

private:
    std::vector<LoadIssue> m_issues;
    int m_errors = 0;
};

enum class Role { Initial, Target, Error };

std::string at(const std::string &base, std::size_t index)
{
    return base + "[" + std::to_string(index) + "]";
}

std::string sub(const std::string &base, const std::string &key)
{
    return base + "." + key;
}

std::string quoted(const std::string &s)
{
    return "\"" + s + "\"";
}

void warnUnknownKeys(const JsonValue &o, std::initializer_list<const char *> allowed, const std::string &path,
                     Diag &d)
{
    for (const auto &m : o.members()) {
        bool known = false;
        for (const char *a : allowed) {
            if (m.first == a) {
                known = true;
                break;
            }
        }
        if (!known)
            d.warning(sub(path, m.first), "unknown key ignored (typo?)");
    }
}

// Reads a string member. Reports a missing key only when `required`.
bool getString(const JsonValue &o, const char *key, const std::string &path, Diag &d, bool required,
               std::string &out)
{
    const JsonValue *v = o.find(key);
    if (!v) {
        if (required)
            d.error(path, "missing required key " + quoted(key));
        return false;
    }
    if (!v->isString()) {
        d.error(sub(path, key), "must be a string");
        return false;
    }
    out = v->asString();
    return true;
}

const JsonValue *getArray(const JsonValue &o, const char *key, const std::string &path, Diag &d)
{
    const JsonValue *v = o.find(key);
    if (!v) {
        d.error(path, "missing required array " + quoted(key));
        return nullptr;
    }
    if (!v->isArray()) {
        d.error(sub(path, key), "must be an array");
        return nullptr;
    }
    return v;
}

// JSON numbers arrive as double; accept only exact non-negative 32-bit integers.
bool getUInt(const JsonValue &v, const std::string &path, Diag &d, std::uint32_t minValue, std::uint32_t &out)
{
    const std::string msg = "must be an integer >= " + std::to_string(minValue) + " that fits in 32 bits";
    if (!v.isNumber()) {
        d.error(path, msg);
        return false;
    }
    const double x = v.asNumber();
    if (x != std::floor(x) || x < static_cast<double>(minValue) || x > 4294967295.0) {
        d.error(path, msg);
        return false;
    }
    out = static_cast<std::uint32_t>(x);
    return true;
}

// GLM-5.3-Flash: the value of a "crash" key (on a transition or on a named
// marking), one of:
//   "sometimes"  a class word: the class calibration stands as it is
//   0.7          shorthand for {"p": 0.7}
//   an object    {"class", "p", "attempts", "crash_dump", "max_dumps"},
//                defaulting to the neutral class "random"
// The class calibrates the reproduction experiment (engine/CrashCredibility.h);
// "p" overrides its prior mean, "attempts" its give-up budget. A probability
// must lie strictly between 0 and 1: a crash that never or always happens is
// not a prediction, and the Beta prior behind the experiment cannot express
// either.
void parseCrashPrediction(const JsonValue &v, const std::string &path, Diag &d,
                          std::optional<CrashPrediction> &out)
{
    const char *kMsg = "must be \"always\", \"often\", \"sometimes\", \"rare\", \"random\", "
                       "a number in (0,1), or an object";

    const auto parseWord = [&](const JsonValue &wv, CrashClass &c) {
        if (!wv.isString()) {
            d.error(path, kMsg);
            return false;
        }
        const std::string word = wv.asString();
        for (CrashClass k : {CrashClass::Always, CrashClass::Often, CrashClass::Sometimes,
                             CrashClass::Rare, CrashClass::Random}) {
            if (word == toString(k)) {
                c = k;
                return true;
            }
        }
        d.error(path, "unknown crash class " + quoted(word)
                      + ": must be \"always\", \"often\", \"sometimes\", \"rare\" or \"random\"");
        return false;
    };

    CrashPrediction cp;
    if (v.isString()) {
        if (parseWord(v, cp.crashClass))
            out = cp;
        return;
    }
    if (v.isNumber()) {
        const double x = v.asNumber();
        if (!(x > 0.0 && x < 1.0)) {
            d.error(path, "must lie strictly between 0 and 1 (a crash that never or always "
                          "happens is not a prediction)");
            return;
        }
        cp.p = x;  // the neutral class "random" keeps its table's give-up budget
        out = cp;
        return;
    }
    if (!v.isObject()) {
        d.error(path, kMsg);
        return;
    }

    warnUnknownKeys(v, {"class", "p", "attempts", "crash_dump", "max_dumps"}, path, d);
    if (const JsonValue *cl = v.find("class"))
        parseWord(*cl, cp.crashClass);
    if (const JsonValue *pv = v.find("p")) {
        if (!pv->isNumber() || !(pv->asNumber() > 0.0 && pv->asNumber() < 1.0))
            d.error(sub(path, "p"), "must lie strictly between 0 and 1");
        else
            cp.p = pv->asNumber();
    }
    if (const JsonValue *at = v.find("attempts")) {
        std::uint32_t n = 0;
        if (getUInt(*at, sub(path, "attempts"), d, 1, n))
            cp.attempts = n;
    }
    if (const JsonValue *cs = v.find("crash_dump")) {
        std::string word;
        if (cs->isString()) {
            word = cs->asString();
            if (word == "none")
                cp.crashDump = DumpMode::None;
            else if (word == "snapshot")
                cp.crashDump = DumpMode::Snapshot;
            else if (word == "crash")
                cp.crashDump = DumpMode::Crash;
            else
                d.error(sub(path, "crash_dump"), "must be \"none\", \"snapshot\" or \"crash\"");
        } else {
            d.error(sub(path, "crash_dump"), "must be a string");
        }
    }
    if (const JsonValue *md = v.find("max_dumps")) {
        std::uint32_t n = 0;
        if (getUInt(*md, sub(path, "max_dumps"), d, 0, n))
            cp.maxDumps = n;
    }
    out = cp;
}

// DeepSeek V4.1 Flash: the value of a transition's "rare" key:
//   true                       the documented defaults (p 0.001, 100 attempts,
//                              no dwell)
//   {"p", "attempts", "wait-for"}
//                              an explicit hunting contract; omitted fields take
//                              the same defaults
//   false                      no marker (the transition is ordinary)
// The probability must lie strictly between 0 and 1 for the same reason as a
// crash prediction's: the engine cannot verify the SUT's real odds, and "never"
// or "always" would not describe a rare transition. "wait-for" is a dwell at
// the enabling state in milliseconds, 0 = no dwell. The visibility rule (an
// untraced internal step is never seen, so it cannot be hunted) is checked by
// the caller, mirroring "error", "milestone" and "crash".
void parseRareSpec(const JsonValue &v, const std::string &path, Diag &d, std::optional<RareSpec> &out)
{
    if (v.isBool()) {
        if (v.asBool())
            out = RareSpec{};
        return;
    }
    if (!v.isObject()) {
        d.error(path, "must be true or an object with \"p\", \"attempts\" and \"wait-for\"");
        return;
    }

    RareSpec spec;
    warnUnknownKeys(v, {"p", "attempts", "wait-for"}, path, d);
    if (const JsonValue *pv = v.find("p")) {
        if (!pv->isNumber() || !(pv->asNumber() > 0.0 && pv->asNumber() < 1.0))
            d.error(sub(path, "p"), "must lie strictly between 0 and 1");
        else
            spec.p = pv->asNumber();
    }
    if (const JsonValue *av = v.find("attempts")) {
        std::uint32_t n = 0;
        if (getUInt(*av, sub(path, "attempts"), d, 1, n))
            spec.attempts = n;
    }
    if (const JsonValue *wv = v.find("wait-for")) {
        std::uint32_t n = 0;
        if (getUInt(*wv, sub(path, "wait-for"), d, 0, n))
            spec.waitForMs = n;
    }
    out = spec;
}

// GLM-5.3-Flash: the value of a transition's "command" key (Egge: "Run a
// shell command synchronously or asynchronously when a transition fires."),
// one of:
//   "collect-data.sh"     shorthand for {"run": "..."} (synchronous)
//   an object             {"run", "mode", "times", "when"}
// The command runs when a step of the transition is *certain* (the same
// certainty rule as an error transition or a milestone) and, with "when",
// only while the named place holds a token in every state the belief spans
// after the step (the guard reads, it never consumes). "times" caps the
// executions over the whole campaign. The command line must be non-empty
// and "mode" must be "sync" or "async"; the visibility rule (an untraced
// internal step is never seen, so its command could never fire) is checked
// by the caller, mirroring "error", "milestone" and "crash".
void parseCommandHook(const JsonValue &v, const std::string &path, const Net &net, Diag &d,
                      std::optional<CommandHook> &out)
{
    CommandHook hook;
    if (v.isString()) {
        hook.run = v.asString();
        if (hook.run.empty()) {
            d.error(path, "the command line must not be empty");
            return;
        }
        out = hook;
        return;
    }
    if (!v.isObject()) {
        d.error(path, "must be a command line string or an object");
        return;
    }

    warnUnknownKeys(v, {"run", "mode", "times", "when"}, path, d);
    if (!getString(v, "run", path, d, true, hook.run))
        return;  // no command line, no hook
    if (hook.run.empty()) {
        d.error(sub(path, "run"), "the command line must not be empty");
        return;
    }
    if (const JsonValue *mv = v.find("mode")) {
        if (!mv->isString()) {
            d.error(sub(path, "mode"), "must be \"sync\" or \"async\"");
        } else {
            const std::string word = mv->asString();
            if (word == "sync")
                hook.mode = HookMode::Sync;
            else if (word == "async")
                hook.mode = HookMode::Async;
            else
                d.error(sub(path, "mode"), "must be \"sync\" or \"async\"");
        }
    }
    if (const JsonValue *tv = v.find("times")) {
        std::uint32_t n = 0;
        if (getUInt(*tv, sub(path, "times"), d, 1, n))
            hook.times = n;  // "once" = times 1
    }
    if (const JsonValue *wv = v.find("when")) {
        if (!wv->isString()) {
            d.error(sub(path, "when"), "must be a place id");
        } else {
            const std::string name = wv->asString();
            if (const std::optional<PlaceIndex> place = net.findPlace(name))
                hook.when = *place;
            else
                d.error(sub(path, "when"), "unknown place " + quoted(name));
        }
    }
    out = hook;
}

bool parseKind(const std::string &s, TransitionKind &kind)
{
    if (s == "controllable") { kind = TransitionKind::Controllable; return true; }
    if (s == "observable")   { kind = TransitionKind::Observable;   return true; }
    if (s == "internal")     { kind = TransitionKind::Internal;     return true; }
    return false;
}

// ---------------------------------------------------------------------------
// Sections
// ---------------------------------------------------------------------------

void parsePlaces(const JsonValue &root, Net &net, Diag &d)
{
    const std::string base = "$.places";
    const JsonValue *arr = getArray(root, "places", "$", d);
    if (!arr)
        return;
    if (arr->items().empty())
        d.error(base, "a net needs at least one place");

    for (std::size_t i = 0; i < arr->items().size(); ++i) {
        const std::string p = at(base, i);
        const JsonValue &o = arr->items()[i];
        if (!o.isObject()) {
            d.error(p, "must be an object");
            continue;
        }
        warnUnknownKeys(o, {"id", "bound"}, p, d);

        std::string id;
        if (!getString(o, "id", p, d, true, id))
            continue;
        if (id.empty()) {
            d.error(sub(p, "id"), "must not be empty");
            continue;
        }
        if (net.placeIndex.count(id) || net.transitionIndex.count(id)) {
            d.error(sub(p, "id"), "duplicate id " + quoted(id) + " (ids are shared by places and transitions)");
            continue;
        }

        Place place;
        place.id = id;
        if (const JsonValue *b = o.find("bound")) {
            std::uint32_t bound = 0;
            if (getUInt(*b, sub(p, "bound"), d, 1, bound))
                place.bound = bound;
        }
        net.placeIndex.emplace(id, static_cast<PlaceIndex>(net.places.size()));
        net.places.push_back(std::move(place));
    }
}

void parseTransitions(const JsonValue &root, Net &net, Diag &d)
{
    const std::string base = "$.transitions";
    const JsonValue *arr = getArray(root, "transitions", "$", d);
    if (!arr)
        return;

    for (std::size_t i = 0; i < arr->items().size(); ++i) {
        const std::string p = at(base, i);
        const JsonValue &o = arr->items()[i];
        if (!o.isObject()) {
            d.error(p, "must be an object");
            continue;
        }
        // GLM-5.3-Flash: the new transition tags. "policy"/"target_hits" are
        // listed so they get a dedicated "no effect" warning instead of the
        // generic unknown-key one. "milestone" is the new milestone label.
        warnUnknownKeys(o, {"id", "label", "kind", "traced",  // GLM-5.3-Flash: "traced"
                            "unpredictable", "error", "crash_dump", "max_dumps",
                            "milestone", "crash",  // GLM-5.3-Flash: "crash"
                            "command",  // GLM-5.3-Flash: shell command hook
                            "sync",     // DeepSeek V4.1 Flash: synchronized event
                            "rare",     // DeepSeek V4.1 Flash: the rare hunt
                            "policy", "target_hits"},
                        p, d);

        std::string id;
        if (!getString(o, "id", p, d, true, id))
            continue;
        if (id.empty()) {
            d.error(sub(p, "id"), "must not be empty");
            continue;
        }
        if (net.placeIndex.count(id) || net.transitionIndex.count(id)) {
            d.error(sub(p, "id"), "duplicate id " + quoted(id) + " (ids are shared by places and transitions)");
            continue;
        }

        Transition t;
        t.id = id;

        std::string kindStr;
        bool haveKind = getString(o, "kind", p, d, true, kindStr);
        if (haveKind && !parseKind(kindStr, t.kind)) {
            d.error(sub(p, "kind"), "must be \"controllable\", \"observable\" or \"internal\"");
            haveKind = false;
        }

        const bool hasLabelKey = o.find("label") != nullptr;
        std::string label;
        const bool haveLabel = hasLabelKey && getString(o, "label", p, d, false, label);
        if (haveLabel)
            t.label = label;
        if (haveKind && t.kind != TransitionKind::Internal
            && (!hasLabelKey || (haveLabel && label.empty()))) {
            d.error(p, "controllable and observable transitions need a non-empty \"label\" that maps to a SUT action");
        }

        // GLM-5.3-Flash: the tracing default. Observable steps are always
        // reported; internal ones are traced exactly when they carry a label.
        t.traced = t.kind != TransitionKind::Internal || !t.label.empty();

        // DeepSeek V4.1 Flash: the reserved lifecycle labels (doku section 26) are
        // recognized on controllable transitions only. A warning, not an error:
        // the model is valid, but the word now carries engine meaning.
        if (t.kind == TransitionKind::Controllable) {
            t.lifecycle = lifecycleOf(t.label);
            if (t.lifecycle != LifecycleAction::None)
                d.warning(p, "the reserved lifecycle label " + quoted(t.label)
                                 + " makes this a lifecycle input: the engine sends it to the SUT and applies its "
                                   "lifecycle meaning (doku section 26)");
        } else if (lifecycleOf(t.label) != LifecycleAction::None) {
            d.warning(p, "the label " + quoted(t.label)
                             + " is reserved for lifecycle control; on an observable or internal transition "
                               "it stays an ordinary label");
        }

        // GLM-5.3-Flash: an internal transition is traced (reported like an
        // output) when it carries a label. The explicit "traced" key overrides
        // that default, so a named internal step can be silent: useful for
        // labels that are only meant for display or for the run log. It is an
        // error everywhere else: an unlabeled internal step has nothing to
        // report, and the visibility of controllable/observable transitions is
        // part of the testing protocol, not a per-model choice.
        if (const JsonValue *tr = o.find("traced")) {
            if (haveKind && t.kind != TransitionKind::Internal) {
                d.error(sub(p, "traced"), "\"traced\" only applies to internal transitions");
            } else if (!tr->isBool()) {
                d.error(sub(p, "traced"), "must be true or false");
            } else if (tr->asBool() && t.label.empty()) {
                d.error(sub(p, "traced"), "a traced internal step needs a \"label\" the SUT reports the step as");
            } else {
                t.traced = tr->asBool();
            }
        }

        // GLM-5.3-Flash: "unpredictable" marks a transition a human operator
        // drives, so the wall clock says nothing about it. Runs that step it
        // keep the catastrophe pheromones flat (see Session). Allowed on any
        // kind of transition.
        if (const JsonValue *up = o.find("unpredictable")) {
            if (!up->isBool())
                d.error(sub(p, "unpredictable"), "must be true or false");
            else
                t.unpredictable = up->asBool();
        }

        // GLM-5.3-Flash: "error" makes the transition itself an error state.
        // The step must be *visible* for the tester to know it happened, so an
        // untraced internal transition cannot carry the tag.
        bool errorTag = false;
        if (const JsonValue *er = o.find("error")) {
            if (!er->isBool()) {
                d.error(sub(p, "error"), "must be true or false");
            } else {
                errorTag = er->asBool();
                t.error = errorTag;
                if (errorTag && !t.traced)
                    d.error(sub(p, "error"),
                            "an error transition must be visible to the tester: "
                            "give it a label and \"traced\": true (or make it controllable/observable)");
            }
        }

        // GLM-5.3-Flash: post-mortem request for error transitions, parsed
        // like the error-marking keys.
        if (o.find("crash_dump")) {
            std::string cs;
            if (getString(o, "crash_dump", p, d, false, cs)) {
                if (cs == "none")
                    t.crashDump = DumpMode::None;
                else if (cs == "snapshot")
                    t.crashDump = DumpMode::Snapshot;
                else if (cs == "crash")
                    t.crashDump = DumpMode::Crash;
                else
                    d.error(sub(p, "crash_dump"), "must be \"none\", \"snapshot\" or \"crash\"");
                if (t.crashDump != DumpMode::None && !errorTag)
                    d.warning(sub(p, "crash_dump"), "has no effect without \"error\": true");
            }
        }
        if (const JsonValue *md = o.find("max_dumps")) {
            std::uint32_t maxDumps = 0;
            if (getUInt(*md, sub(p, "max_dumps"), d, 0, maxDumps)) {
                t.maxDumps = maxDumps;
                if (t.crashDump == DumpMode::None)
                    d.warning(sub(p, "max_dumps"), "has no effect without \"crash_dump\"");
            }
        }

        // GLM-5.3-Flash: "milestone" marks a transition the campaign wants to
        // reach: a certainly stepped one deposits positive pheromones, almost
        // as much as a target marking gets. The value is true (rank 1) or an
        // integer rank >= 1. Same visibility rule as "error": the step must be
        // visible, so an untraced internal transition cannot carry the tag.
        if (const JsonValue *ms = o.find("milestone")) {
            if (ms->isBool()) {
                t.milestone = ms->asBool();
                t.milestoneRank = 1;
            } else if (ms->isNumber()) {
                std::uint32_t rank = 0;
                if (getUInt(*ms, sub(p, "milestone"), d, 1, rank)) {
                    t.milestone = true;
                    t.milestoneRank = rank;
                }
            } else {
                d.error(sub(p, "milestone"), "must be true (rank 1) or an integer rank >= 1");
            }
            if (t.milestone && !t.traced)
                d.error(sub(p, "milestone"),
                        "a milestone transition must be visible to the tester: "
                        "give it a label and \"traced\": true (or make it controllable/observable)");
        }

        // GLM-5.3-Flash: "crash" predicts a nondeterministic SUT crash at this
        // transition (engine/CrashCredibility.h): the crash is a target until
        // the reproduction experiment is certain it is deterministic. Two
        // rules mirror "error" and "milestone": the step must be VISIBLE (an
        // untraced internal step is never seen, so its crash could never be
        // attributed), and an error transition cannot carry the tag (a
        // certain reaction, not a probable crash).
        if (const JsonValue *cr = o.find("crash")) {
            std::optional<CrashPrediction> cp;
            parseCrashPrediction(*cr, sub(p, "crash"), d, cp);
            if (cp) {
                if (!t.traced)
                    d.error(sub(p, "crash"),
                            "a crash prediction needs a visible step: give the transition a label "
                            "and \"traced\": true (or make it controllable/observable)");
                else if (errorTag)
                    d.error(sub(p, "crash"),
                            "an error transition already fails certainly: it cannot also predict a crash");
                else
                    t.crashPrediction = cp;
            }
        }

        // GLM-5.3-Flash: "command" attaches a shell command to the transition
        // (Egge: "We may want invoke e.g. data oriented testing, during a
        // run, too."). Same visibility rule as "error", "milestone" and
        // "crash": the step must be VISIBLE, because an untraced internal
        // step is never seen and its command could never fire.
        if (const JsonValue *cv = o.find("command")) {
            std::optional<CommandHook> hook;
            parseCommandHook(*cv, sub(p, "command"), net, d, hook);
            if (hook) {
                if (!t.traced)
                    d.error(sub(p, "command"),
                            "a command needs a visible step: give the transition a label "
                            "and \"traced\": true (or make it controllable/observable)");
                else
                    t.command = hook;
            }
        }

        // DeepSeek V4.1 Flash: "sync" holds the SUT at this event until the tracker
        // has acknowledged that it handled the event (doku section 19). Only
        // the SUT's own moves can carry it: an observable step or a traced
        // internal one. A controllable transition needs no such hold (its
        // stimulus is acknowledged already), and an untraced internal step is
        // never seen, so there is nothing to acknowledge.
        if (const JsonValue *sy = o.find("sync")) {
            if (!sy->isBool()) {
                d.error(sub(p, "sync"), "must be true or false");
            } else if (sy->asBool()) {
                if (haveKind && t.kind == TransitionKind::Controllable)
                    d.error(sub(p, "sync"),
                            "a synchronized event must be the SUT's own move: a controllable "
                            "transition is acknowledged as a stimulus already");
                else if (!t.traced)
                    d.error(sub(p, "sync"),
                            "a synchronized event must be visible to the tester: give it a label "
                            "and \"traced\": true (or make it observable)");
                else
                    t.synchronized = true;
            }
        }

        // DeepSeek V4.1 Flash: "rare" marks a transition that fires only with
        // a very low probability, with its own hunting budget and dwell time
        // (doku section 30). Same visibility rule as "error", "milestone" and
        // "crash": an untraced internal step is never seen, so a hunt could
        // never tell whether it fired.
        if (const JsonValue *rv = o.find("rare")) {
            std::optional<RareSpec> spec;
            parseRareSpec(*rv, sub(p, "rare"), d, spec);
            if (spec) {
                if (!t.traced)
                    d.error(sub(p, "rare"),
                            "a rare transition must be visible to the tester: give it a label "
                            "and \"traced\": true (or make it controllable/observable)");
                else
                    t.rare = spec;
            }
        }

        // GLM-5.3-Flash: steering policies are an error-*marking* concept for
        // now; a transition error is steered by ordinary transition coverage.
        if (o.find("policy"))
            d.warning(sub(p, "policy"), "has no effect on transitions yet; error transitions are covered like ordinary transitions");
        if (o.find("target_hits"))
            d.warning(sub(p, "target_hits"), "has no effect on transitions yet; error transitions are covered like ordinary transitions");

        net.transitionIndex.emplace(id, static_cast<TransitionIndex>(net.transitions.size()));
        net.transitions.push_back(std::move(t));
    }
}

void parseArcs(const JsonValue &root, Net &net, Diag &d)
{
    const std::string base = "$.arcs";
    const JsonValue *arr = getArray(root, "arcs", "$", d);
    if (!arr)
        return;

    std::set<std::string> seen;
    for (std::size_t i = 0; i < arr->items().size(); ++i) {
        const std::string p = at(base, i);
        const JsonValue &o = arr->items()[i];
        if (!o.isObject()) {
            d.error(p, "must be an object");
            continue;
        }
        warnUnknownKeys(o, {"from", "to", "type", "weight"}, p, d);

        std::string from, to;
        const bool haveFrom = getString(o, "from", p, d, true, from);
        const bool haveTo = getString(o, "to", p, d, true, to);

        bool inhibitor = false;
        bool typeOk = true;
        if (o.find("type")) {
            std::string type;
            if (getString(o, "type", p, d, false, type)) {
                if (type == "normal") {
                    inhibitor = false;
                } else if (type == "inhibitor") {
                    inhibitor = true;
                } else {
                    d.error(sub(p, "type"), "must be \"normal\" or \"inhibitor\"");
                    typeOk = false;
                }
            } else {
                typeOk = false;
            }
        }

        std::uint32_t weight = 1;
        bool weightOk = true;
        if (const JsonValue *w = o.find("weight"))
            weightOk = getUInt(*w, sub(p, "weight"), d, 1, weight);

        if (!haveFrom || !haveTo)
            continue;

        const auto fromP = net.findPlace(from);
        const auto fromT = net.findTransition(from);
        const auto toP = net.findPlace(to);
        const auto toT = net.findTransition(to);

        bool resolved = true;
        if (!fromP && !fromT) {
            d.error(sub(p, "from"), "unknown node " + quoted(from));
            resolved = false;
        }
        if (!toP && !toT) {
            d.error(sub(p, "to"), "unknown node " + quoted(to));
            resolved = false;
        }
        if (!resolved)
            continue;

        if ((fromP && toP) || (fromT && toT)) {
            d.error(p, "an arc must connect a place and a transition");
            continue;
        }
        if (!typeOk || !weightOk)
            continue;

        const std::string dupKey = from + '\x1f' + to + '\x1f' + (inhibitor ? "i" : "n");
        if (fromT && inhibitor) {
            d.error(sub(p, "type"), "inhibitor arcs must go from a place to a transition");
            continue;
        }
        if (!seen.insert(dupKey).second) {
            d.error(p, "duplicate arc; use \"weight\" instead of repeating it");
            continue;
        }

        if (fromP) {
            Transition &t = net.transitions[*toT];
            (inhibitor ? t.inhibit : t.pre).push_back({*fromP, weight});
        } else {
            net.transitions[*fromT].post.push_back({*toP, weight});
        }
    }
}

void finishTransitions(Net &net, Diag &d)
{
    const auto byPlace = [](const WeightedArc &a, const WeightedArc &b) { return a.place < b.place; };
    for (std::size_t i = 0; i < net.transitions.size(); ++i) {
        Transition &t = net.transitions[i];
        std::sort(t.pre.begin(), t.pre.end(), byPlace);
        std::sort(t.post.begin(), t.post.end(), byPlace);
        std::sort(t.inhibit.begin(), t.inhibit.end(), byPlace);
        // DeepSeek V4.1 Flash: an arc-less lifecycle input is not a mistake -- the
        // engine supplies its landing (doku section 26), so it neither fires
        // tokens nor needs arcs.
        if (t.lifecycle == LifecycleAction::None && t.pre.empty() && t.post.empty() && t.inhibit.empty())
            d.warning(at("$.transitions", i), "transition " + quoted(t.id) + " has no arcs");
    }
}

void parseMarkingList(const JsonValue &arr, const std::string &base, Role role, const Net &net,
                      std::vector<NamedMarking> &out, Diag &d)
{
    std::set<std::string> names;
    for (std::size_t i = 0; i < arr.items().size(); ++i) {
        const std::string p = at(base, i);
        const JsonValue &o = arr.items()[i];
        if (!o.isObject()) {
            d.error(p, "must be an object");
            continue;
        }
        warnUnknownKeys(o, {"name", "tokens", "match", "policy", "target_hits", "crash_dump", "max_dumps",
                            "intentional", "crash"},
                        p, d);  // GLM-5.3-Flash: "intentional", "crash"

        NamedMarking m;

        std::string name;
        if (getString(o, "name", p, d, true, name)) {
            if (name.empty()) {
                d.error(sub(p, "name"), "must not be empty");
            } else if (!names.insert(name).second) {
                d.error(sub(p, "name"), "duplicate marking name " + quoted(name));
            } else {
                m.name = name;
            }
        }

        // GLM-5.3-Flash: "intentional" turns a target marking into a "good
        // target state": a normal target whose certain reach ends the campaign.
        // It has no meaning for initial or error markings, so it warns there.
        if (o.find("intentional")) {
            if (role != Role::Target) {
                d.warning(sub(p, "intentional"), "only meaningful for target markings; ignored");
            } else if (const JsonValue *iv = o.find("intentional"); !iv->isBool()) {
                d.error(sub(p, "intentional"), "must be true or false");
            } else {
                m.intentional = iv->asBool();
            }
        }

        if (o.find("match")) {
            if (role == Role::Initial) {
                d.warning(sub(p, "match"), "\"match\" is ignored for initial markings");
            } else {
                std::string ms;
                if (getString(o, "match", p, d, false, ms)) {
                    if (ms == "exact")
                        m.match = MatchMode::Exact;
                    else if (ms == "covers")
                        m.match = MatchMode::Covers;
                    else
                        d.error(sub(p, "match"), "must be \"exact\" or \"covers\"");
                }
            }
        }

        // Error-marking settings: how the tester treats this error and how the
        // SUT is asked for a post-mortem when it is reached.
        static const char *const kErrorOnlyKeys[] = {"policy", "target_hits", "crash_dump", "max_dumps"};
        if (role != Role::Error) {
            for (const char *k : kErrorOnlyKeys)
                if (o.find(k))
                    d.warning(sub(p, k), "only meaningful for error markings; ignored");
        } else {
            if (o.find("policy")) {
                std::string ps;
                if (getString(o, "policy", p, d, false, ps)) {
                    if (ps == "until_covered")
                        m.policy = TargetPolicy::UntilCovered;
                    else if (ps == "once")
                        m.policy = TargetPolicy::Once;
                    else if (ps == "avoid")
                        m.policy = TargetPolicy::Avoid;
                    else
                        d.error(sub(p, "policy"), "must be \"until_covered\", \"once\" or \"avoid\"");
                }
            }
            if (const JsonValue *th = o.find("target_hits")) {
                std::uint32_t hits = 0;
                if (getUInt(*th, sub(p, "target_hits"), d, 1, hits)) {
                    m.targetHits = hits;
                    if (m.policy != TargetPolicy::UntilCovered)
                        d.warning(sub(p, "target_hits"), "ignored: this policy fixes the target");
                }
            }
            if (o.find("crash_dump")) {
                std::string cs;
                if (getString(o, "crash_dump", p, d, false, cs)) {
                    if (cs == "none")
                        m.crashDump = DumpMode::None;
                    else if (cs == "snapshot")
                        m.crashDump = DumpMode::Snapshot;
                    else if (cs == "crash")
                        m.crashDump = DumpMode::Crash;
                    else
                        d.error(sub(p, "crash_dump"), "must be \"none\", \"snapshot\" or \"crash\"");
                }
            }
            if (const JsonValue *md = o.find("max_dumps")) {
                std::uint32_t maxDumps = 0;
                if (getUInt(*md, sub(p, "max_dumps"), d, 0, maxDumps)) {
                    m.maxDumps = maxDumps;
                    if (m.crashDump == DumpMode::None)
                        d.warning(sub(p, "max_dumps"), "has no effect without \"crash_dump\"");
                }
            }
        }

        // GLM-5.3-Flash: "crash" predicts a nondeterministic SUT crash while
        // the SUT is certainly in this marking (engine/CrashCredibility.h).
        // It works on the target and error markings -- the two lists the
        // tracker can be "certainly in" -- and cannot sit on an initial
        // marking (the SUT is started there before anything is observed, so a
        // death before the first step cannot be attributed to a prediction)
        // nor on a marking with policy "avoid" (what the tester avoids cannot
        // be sought). The usual reaction to an error marking stays as it is;
        // the prediction only adds the death that may follow.
        if (const JsonValue *cr = o.find("crash")) {
            std::optional<CrashPrediction> cp;
            parseCrashPrediction(*cr, sub(p, "crash"), d, cp);
            if (cp) {
                if (role == Role::Initial)
                    d.error(sub(p, "crash"),
                            "an initial marking cannot predict a crash: the campaign starts the SUT "
                            "there, and a death before any step cannot be attributed to a prediction");
                else if (m.policy == TargetPolicy::Avoid)
                    d.error(sub(p, "crash"),
                            "a marking the tester avoids from the start cannot also be sought "
                            "for a crash");
                else
                    m.crash = cp;
            }
        }

        const JsonValue *tv = o.find("tokens");
        if (!tv) {
            d.error(p, "missing required key \"tokens\"");
            continue;
        }
        if (!tv->isObject()) {
            d.error(sub(p, "tokens"), "must be an object mapping place ids to token counts");
            continue;
        }

        std::map<PlaceIndex, TokenCount> acc;
        const std::string tokensPath = sub(p, "tokens");
        for (const auto &entry : tv->members()) {
            const std::string tp = sub(tokensPath, entry.first);
            const auto pi = net.findPlace(entry.first);
            if (!pi) {
                d.error(tp, "unknown place " + quoted(entry.first));
                continue;
            }
            std::uint32_t n = 0;
            if (!getUInt(entry.second, tp, d, 0, n))
                continue;
            if (n == 0)
                continue;  // zero entries carry no information in a sparse marking

            const Place &place = net.places[*pi];
            if (place.bound && n > *place.bound) {
                const std::string msg = std::to_string(n) + " tokens exceed the bound " + std::to_string(*place.bound)
                                        + " of place " + quoted(entry.first);
                if (role == Role::Initial)
                    d.error(tp, msg);
                else
                    d.warning(tp, msg + " (this marking can never be reached)");
            }
            acc[*pi] = n;
        }

        if (role != Role::Initial && m.match == MatchMode::Covers && acc.empty())
            d.warning(p, "a \"covers\" marking without tokens matches every marking");

        for (const auto &kv : acc)
            m.tokens.push_back({kv.first, kv.second});
        out.push_back(std::move(m));
    }
}

// DeepSeek V4.1 Flash: the model's testing strategy (doku section 27). Optional;
// the command line can override both priorities.
void parseStrategy(const JsonValue &root, Net &net, Diag &d)
{
    const std::string base = "$.strategy";
    const JsonValue *v = root.find("strategy");
    if (!v)
        return;
    if (!v->isObject()) {
        d.error(base, "must be an object");
        return;
    }
    warnUnknownKeys(*v, {"goals", "errors"}, base, d);
    if (const JsonValue *g = v->find("goals")) {
        if (!g->isString())
            d.error(sub(base, "goals"), "must be a string (balanced, targets or coverage)");
        else if (!parseGoalPriority(g->asString(), net.strategy.goals))
            d.error(sub(base, "goals"),
                    "unknown goal priority " + quoted(g->asString()) + " (balanced, targets or coverage)");
    }
    if (const JsonValue *e = v->find("errors")) {
        if (!e->isString())
            d.error(sub(base, "errors"), "must be a string (balanced, seek or avoid)");
        else if (!parseErrorPriority(e->asString(), net.strategy.errors))
            d.error(sub(base, "errors"),
                    "unknown error priority " + quoted(e->asString()) + " (balanced, seek or avoid)");
    }
}

void parseMarkings(const JsonValue &root, Net &net, Diag &d)
{
    const std::string base = "$.markings";
    const JsonValue *mv = root.find("markings");
    if (!mv) {
        d.error("$", "missing required key \"markings\"");
        return;
    }
    if (!mv->isObject()) {
        d.error(base, "must be an object");
        return;
    }
    warnUnknownKeys(*mv, {"initial", "target", "error"}, base, d);

    const auto section = [&](const char *key, Role role, std::vector<NamedMarking> &out, bool required) {
        const std::string p = sub(base, key);
        const JsonValue *v = mv->find(key);
        if (!v) {
            if (required)
                d.error(base, "missing required array " + quoted(key));
            return;
        }
        if (!v->isArray()) {
            d.error(p, "must be an array");
            return;
        }
        if (required && v->items().empty())
            d.error(p, "at least one initial marking is required");
        parseMarkingList(*v, p, role, net, out, d);
    };

    section("initial", Role::Initial, net.initialMarkings, true);
    section("target", Role::Target, net.targetMarkings, false);
    section("error", Role::Error, net.errorMarkings, false);

    if (net.targetMarkings.empty())
        d.warning(sub(base, "target"), "no target markings: every deadlock will be reported as an error");
}

}  // namespace

// ---------------------------------------------------------------------------
// JsonNetLoader
// ---------------------------------------------------------------------------

LoadResult JsonNetLoader::loadValue(const JsonValue &root)
{
    Diag d;
    Net net;
    if (!root.isObject()) {
        d.error("$", "top-level value must be an object");
        LoadResult r;
        r.issues = d.take();
        return r;
    }

    warnUnknownKeys(root, {"$schema", "version", "name", "places", "transitions", "arcs", "markings", "strategy"}, "$",
                    d);  // DeepSeek V4.1 Flash: "strategy"

    if (const JsonValue *version = root.find("version")) {
        std::uint32_t v = 0;
        if (getUInt(*version, "$.version", d, 1, v) && v != 1)
            d.error("$.version", "unsupported format version " + std::to_string(v) + " (this loader reads version 1)");
    }
    if (root.find("name")) {
        std::string n;
        if (getString(root, "name", "$", d, false, n))
            net.name = n;
    }

    parsePlaces(root, net, d);
    parseTransitions(root, net, d);
    parseArcs(root, net, d);
    finishTransitions(net, d);
    parseMarkings(root, net, d);
    parseStrategy(root, net, d);  // DeepSeek V4.1 Flash: doku section 27

    LoadResult result;
    const bool failed = d.hasErrors();
    result.issues = d.take();
    if (!failed)
        result.net = std::move(net);
    return result;
}

LoadResult JsonNetLoader::loadJson(const std::string &json)
{
    JsonValue root;
    std::string error;
    std::size_t offset = 0;

    LoadResult result;
    if (!JsonValue::parse(json, root, &error, &offset)) {
        std::size_t line = 1;
        std::size_t column = 1;
        const std::size_t limit = std::min(offset, json.size());
        for (std::size_t i = 0; i < limit; ++i) {
            if (json[i] == '\n') {
                ++line;
                column = 1;
            } else {
                ++column;
            }
        }
        const std::size_t cut = error.rfind(" at offset ");  // the position is reported as line and column instead
        if (cut != std::string::npos)
            error.erase(cut);
        result.issues.push_back({Severity::Error, "$",
                                 "JSON syntax error at line " + std::to_string(line) + ", column "
                                     + std::to_string(column) + ": " + error});
        return result;
    }
    return loadValue(root);
}

LoadResult JsonNetLoader::loadFile(const std::string &fileName)
{
    std::ifstream in(fileName, std::ios::binary);
    if (!in) {
        LoadResult result;
        result.issues.push_back({Severity::Error, fileName, "cannot open file"});
        return result;
    }
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return loadJson(buffer.str());
}

}  // namespace pn
