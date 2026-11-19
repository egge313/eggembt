// DeepSeek V4.1 Flash: PNML export (doku section 27). See PnmlWriter.h.
#include "io/PnmlWriter.h"

#include "Version.h"
#include "engine/Json.h"

#include <cctype>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace pn {

namespace {

std::string escapeXml(const std::string &text)
{
    std::string out;
    out.reserve(text.size());
    for (const char c : text) {
        switch (c) {
        case '&':  out += "&amp;";  break;
        case '<':  out += "&lt;";   break;
        case '>':  out += "&gt;";   break;
        case '"':  out += "&quot;"; break;
        case '\'': out += "&apos;"; break;
        default:   out += c;        break;
        }
    }
    return out;
}

// An XML comment may not contain "--" and may not end with '-'.
std::string commentSafe(std::string text)
{
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (i + 1 < text.size() && text[i] == '-' && text[i + 1] == '-') {
            out += "- -";
            ++i;
        } else {
            out += text[i];
        }
    }
    if (!out.empty() && out.back() == '-')
        out += ' ';
    return out;
}

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

bool isNameStart(const char c)
{
    return std::isalpha(static_cast<unsigned char>(c)) || c == '_';
}

bool isNameChar(const char c)
{
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '.' || c == '-';
}

bool isXmlId(const std::string &id)
{
    if (id.empty() || !isNameStart(id.front()))
        return false;
    for (const char c : id)
        if (!isNameChar(c))
            return false;
    return true;
}

// A PNML id must be an XML name. A model id that is not one is replaced by a
// generated id and its original is kept in the annotations; the used set makes
// the replacement collision-free across places and transitions.
std::string pnmlId(const std::string &id, const char *prefix, std::size_t index, std::set<std::string> &used)
{
    if (isXmlId(id) && !used.count(id)) {
        used.insert(id);
        return id;
    }
    const std::string base = std::string(prefix) + std::to_string(index);
    std::string candidate = base;
    for (int n = 1; used.count(candidate); ++n)
        candidate = base + "_" + std::to_string(n);
    used.insert(candidate);
    return candidate;
}

std::string tokensText(const Net &net, const SparseMarking &tokens)
{
    std::string out;
    for (const TokenEntry &e : tokens) {
        if (e.place >= net.places.size())
            continue;
        if (!out.empty())
            out += ", ";
        out += net.places[e.place].id + "=" + std::to_string(e.tokens);
    }
    return out;
}

JsonValue tokensJson(const Net &net, const SparseMarking &tokens)
{
    JsonValue o = JsonValue::object();
    for (const TokenEntry &e : tokens)
        if (e.place < net.places.size())
            o.set(net.places[e.place].id, JsonValue::number(static_cast<double>(e.tokens)));
    return o;
}

std::string markingComment(const Net &net, const NamedMarking &m, const char *role)
{
    std::string out = std::string(role) + " \"" + m.name + "\" (";
    out += matchName(m.match);
    if (std::string(role) == "target" && m.intentional)
        out += ", intentional";
    if (m.policy != TargetPolicy::UntilCovered)
        out += std::string(", policy ") + toString(m.policy);
    if (m.targetHits)
        out += ", target_hits " + std::to_string(*m.targetHits);
    if (m.crashDump != DumpMode::None) {
        out += std::string(", crash_dump ") + dumpName(m.crashDump);
        out += ", max_dumps " + std::to_string(m.maxDumps);
    }
    if (m.crash) {
        out += std::string(", crash ") + toString(m.crash->crashClass);
        if (m.crash->p)
            out += ", p " + std::to_string(*m.crash->p);
        if (m.crash->attempts)
            out += ", attempts " + std::to_string(*m.crash->attempts);
    }
    out += "): " + tokensText(net, m.tokens);
    return out;
}

JsonValue markingJson(const Net &net, const NamedMarking &m, bool initial)
{
    JsonValue o = JsonValue::object();
    o.set("name", JsonValue::string(m.name));
    o.set("tokens", tokensJson(net, m.tokens));
    if (!initial) {
        o.set("match", JsonValue::string(matchName(m.match)));
        if (m.intentional)
            o.set("intentional", JsonValue::boolean(true));
        if (m.policy != TargetPolicy::UntilCovered)
            o.set("policy", JsonValue::string(toString(m.policy)));
        if (m.targetHits)
            o.set("target_hits", JsonValue::number(static_cast<double>(*m.targetHits)));
        if (m.crashDump != DumpMode::None) {
            o.set("crash_dump", JsonValue::string(dumpName(m.crashDump)));
            o.set("max_dumps", JsonValue::number(static_cast<double>(m.maxDumps)));
        }
    }
    if (m.crash) {
        JsonValue c = JsonValue::object();
        c.set("class", JsonValue::string(toString(m.crash->crashClass)));
        if (m.crash->p)
            c.set("p", JsonValue::number(*m.crash->p));
        if (m.crash->attempts)
            c.set("attempts", JsonValue::number(static_cast<double>(*m.crash->attempts)));
        if (m.crash->crashDump != DumpMode::None) {
            c.set("crash_dump", JsonValue::string(dumpName(m.crash->crashDump)));
            c.set("max_dumps", JsonValue::number(static_cast<double>(m.crash->maxDumps)));
        }
        o.set("crash", std::move(c));
    }
    return o;
}

std::string transitionComment(const Net &net, const Transition &t)
{
    std::string out = "transition \"" + t.id + "\": " + kindName(t.kind);
    if (!t.label.empty())
        out += ", label \"" + t.label + "\"";
    if (t.traced)
        out += ", traced";
    if (t.lifecycle != LifecycleAction::None)
        out += std::string(", lifecycle ") + toString(t.lifecycle);
    if (t.synchronized)
        out += ", synchronized";
    if (t.unpredictable)
        out += ", human-operated";
    if (t.error)
        out += ", error transition";
    if (t.milestone)
        out += ", milestone rank " + std::to_string(t.milestoneRank);
    if (t.crashDump != DumpMode::None) {
        out += std::string(", crash_dump ") + dumpName(t.crashDump);
        out += ", max_dumps " + std::to_string(t.maxDumps);
    }
    if (t.crashPrediction) {
        out += std::string(", crash ") + toString(t.crashPrediction->crashClass);
        if (t.crashPrediction->p)
            out += ", p " + std::to_string(*t.crashPrediction->p);
        if (t.crashPrediction->attempts)
            out += ", attempts " + std::to_string(*t.crashPrediction->attempts);
    }
    if (t.command) {
        out += ", command (" + std::string(toString(t.command->mode)) + ")";
        if (t.command->times)
            out += ", times " + std::to_string(*t.command->times);
        if (t.command->when != kNoPlace)
            out += ", when " + net.places[t.command->when].id;
    }
    return out;
}

JsonValue transitionJson(const Net &net, const Transition &t)
{
    JsonValue o = JsonValue::object();
    o.set("id", JsonValue::string(t.id));
    o.set("kind", JsonValue::string(kindName(t.kind)));
    if (!t.label.empty())
        o.set("label", JsonValue::string(t.label));
    if (t.traced)
        o.set("traced", JsonValue::boolean(true));
    if (t.lifecycle != LifecycleAction::None)
        o.set("lifecycle", JsonValue::string(toString(t.lifecycle)));
    if (t.synchronized)
        o.set("sync", JsonValue::boolean(true));
    if (t.unpredictable)
        o.set("unpredictable", JsonValue::boolean(true));
    if (t.error)
        o.set("error", JsonValue::boolean(true));
    if (t.crashDump != DumpMode::None) {
        o.set("crash_dump", JsonValue::string(dumpName(t.crashDump)));
        o.set("max_dumps", JsonValue::number(static_cast<double>(t.maxDumps)));
    }
    if (t.milestone)
        o.set("milestone", JsonValue::number(static_cast<double>(t.milestoneRank)));
    if (t.crashPrediction) {
        JsonValue c = JsonValue::object();
        c.set("class", JsonValue::string(toString(t.crashPrediction->crashClass)));
        if (t.crashPrediction->p)
            c.set("p", JsonValue::number(*t.crashPrediction->p));
        if (t.crashPrediction->attempts)
            c.set("attempts", JsonValue::number(static_cast<double>(*t.crashPrediction->attempts)));
        if (t.crashPrediction->crashDump != DumpMode::None) {
            c.set("crash_dump", JsonValue::string(dumpName(t.crashPrediction->crashDump)));
            c.set("max_dumps", JsonValue::number(static_cast<double>(t.crashPrediction->maxDumps)));
        }
        o.set("crash", std::move(c));
    }
    if (t.command) {
        JsonValue c = JsonValue::object();
        c.set("run", JsonValue::string(t.command->run));
        c.set("mode", JsonValue::string(toString(t.command->mode)));
        if (t.command->times)
            c.set("times", JsonValue::number(static_cast<double>(*t.command->times)));
        if (t.command->when != kNoPlace)
            c.set("when", JsonValue::string(net.places[t.command->when].id));
        o.set("command", std::move(c));
    }
    return o;
}

}  // namespace

std::string toPnml(const Net &net)
{
    std::set<std::string> used;
    std::vector<std::string> placeId;
    placeId.reserve(net.places.size());
    for (std::size_t i = 0; i < net.places.size(); ++i)
        placeId.push_back(pnmlId(net.places[i].id, "p", i, used));
    std::vector<std::string> transId;
    transId.reserve(net.transitions.size());
    for (std::size_t i = 0; i < net.transitions.size(); ++i)
        transId.push_back(pnmlId(net.transitions[i].id, "t", i, used));
    std::set<std::string> netUsed;
    const std::string netId = pnmlId(net.name.empty() ? "net" : net.name, "net", 0, netUsed);

    JsonValue annotations = JsonValue::object();
    annotations.set("tool", JsonValue::string("eggembt"));
    annotations.set("version", JsonValue::string(EGGEMBT_VERSION));
    annotations.set("name", JsonValue::string(net.name));
    {
        JsonValue strategy = JsonValue::object();
        strategy.set("goals", JsonValue::string(toString(net.strategy.goals)));
        strategy.set("errors", JsonValue::string(toString(net.strategy.errors)));
        annotations.set("strategy", std::move(strategy));
    }
    JsonValue places = JsonValue::array();
    for (const Place &p : net.places) {
        if (!p.bound)
            continue;
        JsonValue o = JsonValue::object();
        o.set("id", JsonValue::string(p.id));
        o.set("bound", JsonValue::number(static_cast<double>(*p.bound)));
        places.push(std::move(o));
    }
    if (!places.items().empty())
        annotations.set("places", std::move(places));
    JsonValue transitions = JsonValue::array();
    for (const Transition &t : net.transitions)
        transitions.push(transitionJson(net, t));
    annotations.set("transitions", std::move(transitions));
    if (!net.initialMarkings.empty()) {
        JsonValue initial = JsonValue::array();
        for (const NamedMarking &m : net.initialMarkings)
            initial.push(markingJson(net, m, /*initial=*/true));
        annotations.set("initial", std::move(initial));
    }
    if (!net.targetMarkings.empty()) {
        JsonValue target = JsonValue::array();
        for (const NamedMarking &m : net.targetMarkings)
            target.push(markingJson(net, m, /*initial=*/false));
        annotations.set("target", std::move(target));
    }
    if (!net.errorMarkings.empty()) {
        JsonValue error = JsonValue::array();
        for (const NamedMarking &m : net.errorMarkings)
            error.push(markingJson(net, m, /*initial=*/false));
        annotations.set("error", std::move(error));
    }

    std::ostringstream out;
    out << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        << "<!-- " << escapeXml(commentSafe(
               "eggembt " + std::string(EGGEMBT_VERSION) +
               " PNML export. Standard PNML carries the places, transitions, arcs and the first "
               "initial marking. Everything PNML cannot express -- the other initial markings, "
               "target and error markings, policies, place bounds, transition tags, crash "
               "predictions and the testing strategy -- follows as comments and in the eggembt "
               "toolspecific block below. A PNML import cannot restore those annotations; use the "
               "JSON model for them."))
        << " -->\n"
        << "<pnml xmlns=\"http://www.pnml.org/version-2009/grammar/pnml\">\n"
        << "  <net id=\"" << escapeXml(netId)
        << "\" type=\"http://www.pnml.org/version-2009/grammar/ptnet\">\n"
        << "    <name><text>" << escapeXml(net.name) << "</text></name>\n"
        << "    <page id=\"page1\">\n";

    for (std::size_t i = 0; i < net.places.size(); ++i) {
        const Place &p = net.places[i];
        if (p.bound)
            out << "      <!-- " << escapeXml(commentSafe("place \"" + p.id + "\": bound " +
                                                           std::to_string(*p.bound)))
                << " -->\n";
        out << "      <place id=\"" << escapeXml(placeId[i]) << "\">\n"
            << "        <name><text>" << escapeXml(p.id) << "</text></name>\n";
        // The standard PNML initial marking: the FIRST named initial marking,
        // per place. The annotations carry all of them.
        const NamedMarking *first = net.initialMarkings.empty() ? nullptr : &net.initialMarkings.front();
        if (first) {
            TokenCount tokens = 0;
            for (const TokenEntry &e : first->tokens)
                if (e.place == static_cast<PlaceIndex>(i))
                    tokens = e.tokens;
            if (tokens > 0)
                out << "        <initialMarking><text>" << tokens << "</text></initialMarking>\n";
        }
        out << "      </place>\n";
    }

    for (std::size_t i = 0; i < net.transitions.size(); ++i) {
        const Transition &t = net.transitions[i];
        out << "      <!-- " << escapeXml(commentSafe(transitionComment(net, t))) << " -->\n"
            << "      <transition id=\"" << escapeXml(transId[i]) << "\">\n"
            << "        <name><text>" << escapeXml(t.id) << "</text></name>\n"
            << "        <toolspecific tool=\"eggembt\" kind=\"" << kindName(t.kind) << "\"";
        if (!t.label.empty())
            out << " label=\"" << escapeXml(t.label) << "\"";
        if (t.traced)
            out << " traced=\"true\"";
        out << "/>\n";
        if (t.milestone)
            out << "        <toolspecific tool=\"eggembt\" kind=\"milestone\" rank=\"" << t.milestoneRank
                << "\"/>\n";
        out << "      </transition>\n";
    }

    std::size_t arcNo = 0;
    // `from` is always the arc's place and `to` the transition; `intoPlace`
    // only chooses the direction the PNML source/target attributes need.
    const auto arc = [&](PlaceIndex from, std::size_t to, bool intoPlace, bool inhibitor, TokenCount weight) {
        out << "      <arc id=\"a" << ++arcNo << "\" source=\""
            << escapeXml(intoPlace ? transId[to] : placeId[from]) << "\" target=\""
            << escapeXml(intoPlace ? placeId[from] : transId[to]) << "\">\n";
        if (weight != 1)
            out << "        <inscription><text>" << weight << "</text></inscription>\n";
        if (inhibitor)
            out << "        <type value=\"inhibitor\"/>\n";
        out << "      </arc>\n";
    };
    for (std::size_t ti = 0; ti < net.transitions.size(); ++ti) {
        const Transition &t = net.transitions[ti];
        for (const WeightedArc &a : t.pre)
            arc(a.place, ti, /*intoPlace=*/false, false, a.weight);
        for (const WeightedArc &a : t.post)
            arc(a.place, ti, /*intoPlace=*/true, false, a.weight);
        for (const WeightedArc &a : t.inhibit)
            arc(a.place, ti, /*intoPlace=*/false, true, a.weight);
    }

    out << "      <!-- eggembt annotations (machine-readable; not part of standard PNML) -->\n"
        << "      <toolspecific tool=\"eggembt\" version=\"" << EGGEMBT_VERSION << "\">\n"
        << "        <text>" << escapeXml(annotations.dump()) << "</text>\n"
        << "      </toolspecific>\n";

    if (net.initialMarkings.size() > 1) {
        std::string names;
        for (std::size_t i = 0; i < net.initialMarkings.size(); ++i) {
            if (i)
                names += ", ";
            names += "\"" + net.initialMarkings[i].name + "\"";
        }
        out << "      <!-- PNML has one initial marking; the standard <initialMarking> elements above are "
            << escapeXml(commentSafe("the first one. All " + std::to_string(net.initialMarkings.size()) +
                                     " initial markings (" + names + ") are in the toolspecific block."))
            << " -->\n";
    }
    for (const NamedMarking &m : net.initialMarkings)
        out << "      <!-- " << escapeXml(commentSafe(markingComment(net, m, "initial"))) << " -->\n";
    for (const NamedMarking &m : net.targetMarkings)
        out << "      <!-- " << escapeXml(commentSafe(markingComment(net, m, "target"))) << " -->\n";
    for (const NamedMarking &m : net.errorMarkings)
        out << "      <!-- " << escapeXml(commentSafe(markingComment(net, m, "error"))) << " -->\n";

    out << "    </page>\n"
        << "  </net>\n"
        << "</pnml>\n";
    return out.str();
}

}  // namespace pn
