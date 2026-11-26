// DeepSeek V4.1 Flash: eggembt JSON export. See JsonNetWriter.h.
#include "io/JsonNetWriter.h"

#include "engine/Json.h"

#include <string>

namespace pn {

namespace {

const char *kindName(TransitionKind kind)
{
    switch (kind) {
    case TransitionKind::Controllable: return "controllable";
    case TransitionKind::Observable:   return "observable";
    case TransitionKind::Internal:     return "internal";
    }
    return "internal";
}

const char *matchName(MatchMode match)
{
    return match == MatchMode::Covers ? "covers" : "exact";
}

const char *dumpName(DumpMode mode)
{
    switch (mode) {
    case DumpMode::None:     return "none";
    case DumpMode::Snapshot: return "snapshot";
    case DumpMode::Crash:    return "crash";
    }
    return "none";
}

JsonValue tokensJson(const Net &net, const SparseMarking &tokens)
{
    JsonValue o = JsonValue::object();
    for (const TokenEntry &e : tokens)
        if (e.place < net.places.size())
            o.set(net.places[e.place].id, JsonValue::number(static_cast<double>(e.tokens)));
    return o;
}

JsonValue crashJson(const CrashPrediction &crash)
{
    JsonValue c = JsonValue::object();
    c.set("class", JsonValue::string(toString(crash.crashClass)));
    if (crash.p)
        c.set("p", JsonValue::number(*crash.p));
    if (crash.attempts)
        c.set("attempts", JsonValue::number(static_cast<double>(*crash.attempts)));
    if (crash.crashDump != DumpMode::None) {
        c.set("crash_dump", JsonValue::string(dumpName(crash.crashDump)));
        c.set("max_dumps", JsonValue::number(static_cast<double>(crash.maxDumps)));
    }
    return c;
}

JsonValue markingJson(const Net &net, const NamedMarking &m, bool initial)
{
    JsonValue o = JsonValue::object();
    o.set("name", JsonValue::string(m.name));
    o.set("tokens", tokensJson(net, m.tokens));
    if (!initial) {
        if (m.match == MatchMode::Covers)
            o.set("match", JsonValue::string(matchName(m.match)));
        if (m.intentional)
            o.set("intentional", JsonValue::boolean(true));
        if (m.policy != TargetPolicy::UntilCovered)
            o.set("policy", JsonValue::string(toString(m.policy)));
        if (m.targetHits)
            o.set("target_hits", JsonValue::number(static_cast<double>(*m.targetHits)));
        if (m.crashDump != DumpMode::None) {
            o.set("crash_dump", JsonValue::string(dumpName(m.crashDump)));
            if (m.maxDumps != 1)
                o.set("max_dumps", JsonValue::number(static_cast<double>(m.maxDumps)));
        }
    }
    if (m.crash)
        o.set("crash", crashJson(*m.crash));
    return o;
}

JsonValue transitionJson(const Net &net, const Transition &t)
{
    JsonValue o = JsonValue::object();
    o.set("id", JsonValue::string(t.id));
    o.set("kind", JsonValue::string(kindName(t.kind)));
    if (!t.label.empty())
        o.set("label", JsonValue::string(t.label));
    // "traced" is meaningful on internal transitions only, and its default is
    // "a label means traced". Emit it only when the model overrides that.
    if (t.kind == TransitionKind::Internal && !t.label.empty() && !t.traced)
        o.set("traced", JsonValue::boolean(false));
    if (t.unpredictable)
        o.set("unpredictable", JsonValue::boolean(true));
    if (t.error) {
        o.set("error", JsonValue::boolean(true));
        if (t.crashDump != DumpMode::None) {
            o.set("crash_dump", JsonValue::string(dumpName(t.crashDump)));
            if (t.maxDumps != 1)
                o.set("max_dumps", JsonValue::number(static_cast<double>(t.maxDumps)));
        }
    }
    if (t.milestone)
        o.set("milestone", JsonValue::number(static_cast<double>(t.milestoneRank)));
    if (t.synchronized)
        o.set("sync", JsonValue::boolean(true));
    if (t.crashPrediction)
        o.set("crash", crashJson(*t.crashPrediction));
    if (t.command) {
        JsonValue c = JsonValue::object();
        c.set("run", JsonValue::string(t.command->run));
        c.set("mode", JsonValue::string(toString(t.command->mode)));
        if (t.command->times)
            c.set("times", JsonValue::number(static_cast<double>(*t.command->times)));
        if (t.command->when != kNoPlace && t.command->when < net.places.size())
            c.set("when", JsonValue::string(net.places[t.command->when].id));
        o.set("command", std::move(c));
    }
    if (t.rare) {
        JsonValue r = JsonValue::object();
        r.set("p", JsonValue::number(t.rare->p));
        r.set("attempts", JsonValue::number(static_cast<double>(t.rare->attempts)));
        r.set("wait-for", JsonValue::number(static_cast<double>(t.rare->waitForMs)));
        o.set("rare", std::move(r));
    }
    return o;
}

void prettyTo(const JsonValue &v, std::string &out, int indent)
{
    const auto spaces = [&](int n) {
        for (int i = 0; i < n; ++i)
            out += ' ';
    };
    if (v.isObject()) {
        if (v.members().empty()) {
            out += "{}";
            return;
        }
        out += "{\n";
        for (std::size_t i = 0; i < v.members().size(); ++i) {
            spaces(indent + 2);
            out += JsonValue::string(v.members()[i].first).dump();
            out += ": ";
            prettyTo(v.members()[i].second, out, indent + 2);
            if (i + 1 < v.members().size())
                out += ',';
            out += '\n';
        }
        spaces(indent);
        out += '}';
        return;
    }
    if (v.isArray()) {
        if (v.items().empty()) {
            out += "[]";
            return;
        }
        out += "[\n";
        for (std::size_t i = 0; i < v.items().size(); ++i) {
            spaces(indent + 2);
            prettyTo(v.items()[i], out, indent + 2);
            if (i + 1 < v.items().size())
                out += ',';
            out += '\n';
        }
        spaces(indent);
        out += ']';
        return;
    }
    out += v.dump();
}

}  // namespace

std::string toJsonModel(const Net &net)
{
    JsonValue root = JsonValue::object();
    root.set("version", JsonValue::number(1));
    if (!net.name.empty())
        root.set("name", JsonValue::string(net.name));

    JsonValue places = JsonValue::array();
    for (const Place &p : net.places) {
        JsonValue o = JsonValue::object();
        o.set("id", JsonValue::string(p.id));
        if (p.bound)
            o.set("bound", JsonValue::number(static_cast<double>(*p.bound)));
        places.push(std::move(o));
    }
    root.set("places", std::move(places));

    JsonValue transitions = JsonValue::array();
    for (const Transition &t : net.transitions)
        transitions.push(transitionJson(net, t));
    root.set("transitions", std::move(transitions));

    JsonValue arcs = JsonValue::array();
    const auto arc = [&](PlaceIndex place, const std::string &transition, bool intoPlace, bool inhibitor,
                         TokenCount weight) {
        JsonValue o = JsonValue::object();
        o.set("from", JsonValue::string(intoPlace ? transition : net.places[place].id));
        o.set("to", JsonValue::string(intoPlace ? net.places[place].id : transition));
        if (inhibitor)
            o.set("type", JsonValue::string("inhibitor"));
        if (weight != 1)
            o.set("weight", JsonValue::number(static_cast<double>(weight)));
        arcs.push(std::move(o));
    };
    for (const Transition &t : net.transitions) {
        for (const WeightedArc &a : t.pre)
            arc(a.place, t.id, /*intoPlace=*/false, false, a.weight);
        for (const WeightedArc &a : t.post)
            arc(a.place, t.id, /*intoPlace=*/true, false, a.weight);
        for (const WeightedArc &a : t.inhibit)
            arc(a.place, t.id, /*intoPlace=*/false, true, a.weight);
    }
    root.set("arcs", std::move(arcs));

    JsonValue markings = JsonValue::object();
    JsonValue initial = JsonValue::array();
    for (const NamedMarking &m : net.initialMarkings)
        initial.push(markingJson(net, m, /*initial=*/true));
    markings.set("initial", std::move(initial));
    if (!net.targetMarkings.empty()) {
        JsonValue target = JsonValue::array();
        for (const NamedMarking &m : net.targetMarkings)
            target.push(markingJson(net, m, /*initial=*/false));
        markings.set("target", std::move(target));
    }
    if (!net.errorMarkings.empty()) {
        JsonValue error = JsonValue::array();
        for (const NamedMarking &m : net.errorMarkings)
            error.push(markingJson(net, m, /*initial=*/false));
        markings.set("error", std::move(error));
    }
    root.set("markings", std::move(markings));

    if (net.strategy.goals != GoalPriority::Balanced || net.strategy.errors != ErrorPriority::Balanced) {
        JsonValue strategy = JsonValue::object();
        strategy.set("goals", JsonValue::string(toString(net.strategy.goals)));
        strategy.set("errors", JsonValue::string(toString(net.strategy.errors)));
        root.set("strategy", std::move(strategy));
    }

    std::string out;
    prettyTo(root, out, 0);
    out += '\n';
    return out;
}

}  // namespace pn
