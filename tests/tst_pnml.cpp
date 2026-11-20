// GLM-5.3-Flash: tests of the PNML import (doku section 12, item 6): the XML
// reader, the eggembt toolspecific conventions (kinds, labels, milestones), the
// union of per-place initial markings, inhibitor arcs and weights, and the
// issue reporting for broken files. Also drives a session on the shipped
// example to prove a PNML model is usable end to end.
#include "io/PnmlLoader.h"

#include "engine/Json.h"       // DeepSeek V4.1 Flash: the export's annotations are JSON
#include "engine/Session.h"
#include "io/PnmlWriter.h"     // DeepSeek V4.1 Flash
#include "model/NetBuilder.h"  // DeepSeek V4.1 Flash
#include "FakeSut.h"
#include "MiniTest.h"

#include <algorithm>
#include <string>

using pn::JsonNetLoader;
using pn::JsonValue;  // DeepSeek V4.1 Flash: export annotations
using pn::LoadIssue;
using pn::LoadResult;
using pn::PnmlLoader;

namespace {

bool hasIssue(const LoadResult &r, LoadIssue::Severity sev, const std::string &path,
              const std::string &textPart = std::string())
{
    for (const LoadIssue &i : r.issues) {
        if (i.severity == sev && i.path == path && minitest::contains(i.message, textPart))
            return true;
    }
    return false;
}

// DeepSeek V4.1 Flash: XML entities decoded, the reverse of the writer's
// escaping, so an annotation's <text> can be parsed as JSON.
bool xmlUnescape(const std::string &raw, std::string &out)
{
    out.clear();
    for (std::size_t i = 0; i < raw.size(); ++i) {
        if (raw[i] != '&') {
            out += raw[i];
            continue;
        }
        const std::size_t end = raw.find(';', i);
        if (end == std::string::npos)
            return false;
        const std::string entity = raw.substr(i + 1, end - i - 1);
        if (entity == "lt")       out += '<';
        else if (entity == "gt")  out += '>';
        else if (entity == "amp") out += '&';
        else if (entity == "quot") out += '"';
        else if (entity == "apos") out += '\'';
        else return false;
        i = end;
    }
    return true;
}

// The machine-readable annotation block (the last toolspecific with a <text>).
bool annotationJson(const std::string &doc, JsonValue &out)
{
    const std::size_t at = doc.find("<toolspecific tool=\"eggembt\" version=");
    if (at == std::string::npos)
        return false;
    const std::size_t open = doc.find("<text>", at);
    const std::size_t close = open == std::string::npos ? std::string::npos : doc.find("</text>", open);
    if (open == std::string::npos || close == std::string::npos)
        return false;
    std::string json;
    return xmlUnescape(doc.substr(open + 6, close - open - 6), json) && JsonValue::parse(json, out);
}

// A small valid net; individual tests replace one section to break it.
const char *kNet = R"(<?xml version="1.0"?>
<pnml>
  <net id="n" type="P/T net">
    <name><text>tiny</text></name>
    <page id="p1">
      <place id="a"><initialMarking><text>2</text></initialMarking></place>
      <place id="b"/>
      <transition id="go"><toolspecific tool="eggembt" kind="controllable" label="go!"/></transition>
      <transition id="done"><toolspecific tool="eggembt" kind="observable" label="done"/></transition>
      <arc source="a" target="go"/>
      <arc source="go" target="b"/>
      <arc source="b" target="done"/>
      <arc source="done" target="a"/>
    </page>
  </net>
</pnml>
)";

}  // namespace

struct TstPnml {
    void baselineIsValid()
    {
        const LoadResult r = PnmlLoader::loadText(kNet, "tiny.pnml");
        T_VERIFY2(r.ok(), minitest::cstr(r.report()));
        T_VERIFY(!r.hasWarnings());

        const pn::Net &net = *r.net;
        T_COMPARE(net.name, std::string("tiny"));
        T_COMPARE(net.places.size(), std::size_t(2));
        T_COMPARE(net.transitions.size(), std::size_t(2));
        T_COMPARE(net.initialMarkings.size(), std::size_t(1));
        T_VERIFY(net.targetMarkings.empty());
        T_VERIFY(net.errorMarkings.empty());

        // Initial tokens: the union of the per-place markings.
        T_VERIFY(net.initialMarkings[0].tokens.size() == 1);
        T_COMPARE(net.initialMarkings[0].tokens[0].place, *net.findPlace("a"));
        T_COMPARE(net.initialMarkings[0].tokens[0].tokens, pn::TokenCount(2));

        // Kinds and labels come from the toolspecific elements; both are
        // traced by default (controllable/observable always are).
        const auto go = net.findTransition("go");
        T_VERIFY(go.has_value());
        T_COMPARE(net.transitions[*go].kind, pn::TransitionKind::Controllable);
        T_COMPARE(net.transitions[*go].label, std::string("go!"));
        T_VERIFY(net.transitions[*go].traced);
        const auto done = net.findTransition("done");
        T_VERIFY(done.has_value());
        T_COMPARE(net.transitions[*done].kind, pn::TransitionKind::Observable);
        T_COMPARE(net.transitions[*done].label, std::string("done"));
    }

    void milestoneTag()
    {
        // true means rank 1; a number is the rank; a milestone needs visibility.
        const std::string doc = R"(<?xml version="1.0"?>
<pnml><net id="n"><place id="a"><initialMarking><text>1</text></initialMarking></place>
<transition id="t"><toolspecific tool="eggembt" kind="controllable" label="t"/>
<toolspecific tool="eggembt" kind="milestone" rank="3"/></transition>
<arc source="a" target="t"/><arc source="t" target="a"/></net></pnml>)";
        const LoadResult r = PnmlLoader::loadText(doc, "m.pnml");
        T_VERIFY2(r.ok(), minitest::cstr(r.report()));
        T_VERIFY(r.net->transitions[0].milestone);
        T_COMPARE(r.net->transitions[0].milestoneRank, pn::TransitionIndex(3));
    }

    void milestoneDefaultsToRankOne()
    {
        const std::string doc = R"(<?xml version="1.0"?>
<pnml><net id="n"><place id="a"><initialMarking><text>1</text></initialMarking></place>
<transition id="t"><toolspecific tool="eggembt" kind="observable" label="t"/>
<toolspecific tool="eggembt" kind="milestone"/></transition>
<arc source="a" target="t"/><arc source="t" target="a"/></net></pnml>)";
        const LoadResult r = PnmlLoader::loadText(doc, "m.pnml");
        T_VERIFY2(r.ok(), minitest::cstr(r.report()));
        T_VERIFY(r.net->transitions[0].milestone);
        T_COMPARE(r.net->transitions[0].milestoneRank, pn::TransitionIndex(1));
    }

    void milestoneNeedsVisibility()
    {
        // An internal transition without a label is silent: no milestone.
        const std::string doc = R"(<?xml version="1.0"?>
<pnml><net id="n"><place id="a"><initialMarking><text>1</text></initialMarking></place>
<transition id="t"><toolspecific tool="eggembt" kind="milestone" rank="2"/></transition>
<arc source="a" target="t"/><arc source="t" target="a"/></net></pnml>)";
        const LoadResult r = PnmlLoader::loadText(doc, "m.pnml");
        T_VERIFY(!r.ok());
        T_VERIFY(hasIssue(r, LoadIssue::Severity::Error, "transition 0 toolspecific", "visible"));
    }

    void prefixFallback()
    {
        const std::string doc = R"(<?xml version="1.0"?>
<pnml><net id="n"><place id="a"><initialMarking><text>1</text></initialMarking></place>
<place id="b"/>
<transition id="?press"/>
<transition id="!beep"/>
<transition id="tick"/>
<arc source="a" target="?press"/><arc source="?press" target="b"/>
<arc source="b" target="!beep"/><arc source="!beep" target="a"/>
<arc source="a" target="tick"/><arc source="tick" target="a"/></net></pnml>)";
        const LoadResult r = PnmlLoader::loadText(doc, "p.pnml");
        T_VERIFY2(r.ok(), minitest::cstr(r.report()));

        // '?press': controllable, label without the prefix.
        const auto press = r.net->findTransition("?press");
        T_VERIFY(press.has_value());
        T_COMPARE(r.net->transitions[*press].kind, pn::TransitionKind::Controllable);
        T_COMPARE(r.net->transitions[*press].label, std::string("press"));
        // '!beep': observable, label without the prefix.
        const auto beep = r.net->findTransition("!beep");
        T_VERIFY(beep.has_value());
        T_COMPARE(r.net->transitions[*beep].kind, pn::TransitionKind::Observable);
        T_COMPARE(r.net->transitions[*beep].label, std::string("beep"));
        // 'tick': internal and SILENT -- the id is never a label by accident.
        const auto tick = r.net->findTransition("tick");
        T_VERIFY(tick.has_value());
        T_COMPARE(r.net->transitions[*tick].kind, pn::TransitionKind::Internal);
        T_COMPARE(r.net->transitions[*tick].label, std::string());
        T_VERIFY(!r.net->transitions[*tick].traced);
    }

    void explicitLabelMakesInternalTraced()
    {
        const std::string doc = R"(<?xml version="1.0"?>
<pnml><net id="n"><place id="a"><initialMarking><text>1</text></initialMarking></place>
<transition id="tick"><toolspecific tool="eggembt" kind="internal" label="tick.tock"/></transition>
<arc source="a" target="tick"/><arc source="tick" target="a"/></net></pnml>)";
        const LoadResult r = PnmlLoader::loadText(doc, "p.pnml");
        T_VERIFY2(r.ok(), minitest::cstr(r.report()));
        T_COMPARE(r.net->transitions[0].kind, pn::TransitionKind::Internal);
        T_COMPARE(r.net->transitions[0].label, std::string("tick.tock"));
        T_VERIFY(r.net->transitions[0].traced);
    }

    void controllableWithoutLabelFallsBackToId()
    {
        const std::string doc = R"(<?xml version="1.0"?>
<pnml><net id="n"><place id="a"><initialMarking><text>1</text></initialMarking></place>
<transition id="press"><toolspecific tool="eggembt" kind="controllable"/></transition>
<arc source="a" target="press"/><arc source="press" target="a"/></net></pnml>)";
        const LoadResult r = PnmlLoader::loadText(doc, "p.pnml");
        T_VERIFY2(r.ok(), minitest::cstr(r.report()));
        T_COMPARE(r.net->transitions[0].label, std::string("press"));
    }

    void unknownKindIsAnError()
    {
        const std::string doc = R"(<?xml version="1.0"?>
<pnml><net id="n"><place id="a"><initialMarking><text>1</text></initialMarking></place>
<transition id="t"><toolspecific tool="eggembt" kind="weird"/></transition>
<arc source="a" target="t"/><arc source="t" target="a"/></net></pnml>)";
        const LoadResult r = PnmlLoader::loadText(doc, "p.pnml");
        T_VERIFY(!r.ok());
        T_VERIFY(hasIssue(r, LoadIssue::Severity::Error, "transition 0 toolspecific", "controllable"));
    }

    void otherToolsSpecificsAreIgnored()
    {
        const std::string doc = R"(<?xml version="1.0"?>
<pnml><net id="n"><place id="a"><initialMarking><text>1</text></initialMarking></place>
<transition id="t"><toolspecific tool="someOtherTool" version="2"/></transition>
<arc source="a" target="t"/><arc source="t" target="a"/></net></pnml>)";
        const LoadResult r = PnmlLoader::loadText(doc, "p.pnml");
        T_VERIFY2(r.ok(), minitest::cstr(r.report()));
        T_VERIFY(r.issues.empty());  // not ours: silently ignored
        T_COMPARE(r.net->transitions[0].kind, pn::TransitionKind::Internal);
    }

    void inhibitorArcs()
    {
        const std::string doc = R"(<?xml version="1.0"?>
<pnml><net id="n"><place id="a"><initialMarking><text>1</text></initialMarking></place>
<transition id="t"><toolspecific tool="eggembt" kind="controllable" label="t"/></transition>
<arc source="a" target="t"><type value="inhibitor"/></arc>
<arc source="t" target="a"/></net></pnml>)";
        const LoadResult r = PnmlLoader::loadText(doc, "p.pnml");
        T_VERIFY2(r.ok(), minitest::cstr(r.report()));
        T_VERIFY(r.net->transitions[0].inhibit.size() == 1);
        T_COMPARE(r.net->transitions[0].inhibit[0].place, *r.net->findPlace("a"));
        T_VERIFY(r.net->transitions[0].pre.empty());
    }

    void weights()
    {
        const std::string doc = R"(<?xml version="1.0"?>
<pnml><net id="n"><place id="a"><initialMarking><text>4</text></initialMarking></place>
<transition id="t"><toolspecific tool="eggembt" kind="controllable" label="t"/></transition>
<arc source="a" target="t"><inscription><text>3</text></inscription></arc>
<arc source="t" target="a" weight="2"/></net></pnml>)";
        const LoadResult r = PnmlLoader::loadText(doc, "p.pnml");
        T_VERIFY2(r.ok(), minitest::cstr(r.report()));
        T_COMPARE(r.net->transitions[0].pre[0].weight, pn::TokenCount(3));
        T_COMPARE(r.net->transitions[0].post[0].weight, pn::TokenCount(2));
    }

    void unionOfInitialMarkings()
    {
        const std::string doc = R"(<?xml version="1.0"?>
<pnml><net id="n"><place id="a"><initialMarking><text>1</text></initialMarking></place>
<place id="b"><initialMarking><text>2</text></initialMarking></place>
<transition id="t"><toolspecific tool="eggembt" kind="controllable" label="t"/></transition>
<arc source="a" target="t"/><arc source="t" target="a"/></net></pnml>)";
        const LoadResult r = PnmlLoader::loadText(doc, "p.pnml");
        T_VERIFY2(r.ok(), minitest::cstr(r.report()));
        // ONE initial marking: a tokens in place a, 2 tokens in place b.
        T_COMPARE(r.net->initialMarkings.size(), std::size_t(1));
        T_VERIFY(r.net->initialMarkings[0].tokens.size() == 2);
        T_COMPARE(r.net->initialMarkings[0].tokens[0].tokens, pn::TokenCount(1));
        T_COMPARE(r.net->initialMarkings[0].tokens[1].tokens, pn::TokenCount(2));
    }

    void duplicateIdsAndCollisions()
    {
        const std::string dup = R"(<?xml version="1.0"?>
<pnml><net id="n"><place id="a"><initialMarking><text>1</text></initialMarking></place>
<place id="a"/><transition id="t"><toolspecific tool="eggembt" kind="controllable" label="t"/></transition>
<arc source="a" target="t"/><arc source="t" target="a"/></net></pnml>)";
        const LoadResult r1 = PnmlLoader::loadText(dup, "p.pnml");
        T_VERIFY(!r1.ok());
        T_VERIFY(hasIssue(r1, LoadIssue::Severity::Error, "place 1", "duplicate"));

        const std::string collision = R"(<?xml version="1.0"?>
<pnml><net id="n"><place id="a"><initialMarking><text>1</text></initialMarking></place>
<transition id="a"><toolspecific tool="eggembt" kind="controllable" label="t"/></transition>
<arc source="a" target="a"/></net></pnml>)";
        const LoadResult r2 = PnmlLoader::loadText(collision, "p.pnml");
        T_VERIFY(!r2.ok());
        T_VERIFY(hasIssue(r2, LoadIssue::Severity::Error, "transition 0", "duplicate"));
    }

    void arcProblems()
    {
        const std::string unknown = R"(<?xml version="1.0"?>
<pnml><net id="n"><place id="a"><initialMarking><text>1</text></initialMarking></place>
<transition id="t"><toolspecific tool="eggembt" kind="controllable" label="t"/></transition>
<arc source="a" target="nope"/></net></pnml>)";
        const LoadResult r1 = PnmlLoader::loadText(unknown, "p.pnml");
        T_VERIFY(!r1.ok());
        T_VERIFY(hasIssue(r1, LoadIssue::Severity::Error, "arc 0", "unknown arc endpoint"));

        const std::string placeToPlace = R"(<?xml version="1.0"?>
<pnml><net id="n"><place id="a"><initialMarking><text>1</text></initialMarking></place>
<place id="b"/><transition id="t"><toolspecific tool="eggembt" kind="controllable" label="t"/></transition>
<arc source="a" target="b"/></net></pnml>)";
        const LoadResult r2 = PnmlLoader::loadText(placeToPlace, "p.pnml");
        T_VERIFY(!r2.ok());
        T_VERIFY(hasIssue(r2, LoadIssue::Severity::Error, "arc 0", "place and a transition"));

        const std::string badType = R"(<?xml version="1.0"?>
<pnml><net id="n"><place id="a"><initialMarking><text>1</text></initialMarking></place>
<transition id="t"><toolspecific tool="eggembt" kind="controllable" label="t"/></transition>
<arc source="a" target="t"><type value="reset"/></arc></net></pnml>)";
        const LoadResult r3 = PnmlLoader::loadText(badType, "p.pnml");
        T_VERIFY(!r3.ok());
        T_VERIFY(hasIssue(r3, LoadIssue::Severity::Error, "arc 0", "unknown arc type"));
    }

    void noTokensIsAnError()
    {
        const std::string doc = R"(<?xml version="1.0"?>
<pnml><net id="n"><place id="a"/><transition id="t"><toolspecific tool="eggembt" kind="controllable" label="t"/></transition>
<arc source="a" target="t"/><arc source="t" target="a"/></net></pnml>)";
        const LoadResult r = PnmlLoader::loadText(doc, "p.pnml");
        T_VERIFY(!r.ok());
        T_VERIFY(hasIssue(r, LoadIssue::Severity::Error, "$", "initial marking"));
    }

    void placeToolspecificWarns()
    {
        const std::string doc = R"(<?xml version="1.0"?>
<pnml><net id="n"><place id="a"><initialMarking><text>1</text></initialMarking>
<toolspecific tool="eggembt" kind="final"/></place>
<transition id="t"><toolspecific tool="eggembt" kind="controllable" label="t"/></transition>
<arc source="a" target="t"/><arc source="t" target="a"/></net></pnml>)";
        const LoadResult r = PnmlLoader::loadText(doc, "p.pnml");
        T_VERIFY2(r.ok(), minitest::cstr(r.report()));  // only a warning
        T_VERIFY(hasIssue(r, LoadIssue::Severity::Warning, "place 0", "not imported from PNML"));
        T_VERIFY(r.net->targetMarkings.empty());
    }

    void xmlNoiseIsTolerated()
    {
        // Comments, a DOCTYPE with an internal subset, CDATA text and
        // entities in content and attribute values must not confuse the
        // loader.
        const std::string doc = R"(<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE pnml SYSTEM "http://pnml.org" [ <!ENTITY stuff "ignored"> ]>
<!-- a comment at the top -->
<pnml><net id="n"><name><text><![CDATA[a <b> & co]]></text></name>
<place id="a"><initialMarking><text>1</text></initialMarking></place>
<transition id="t"><toolspecific tool="eggembt" kind="observable" label="raw &lt;label&gt;"/></transition>
<arc source="a" target="t"/><arc source="t" target="a"/></net></pnml>)";
        const LoadResult r = PnmlLoader::loadText(doc, "p.pnml");
        T_VERIFY2(r.ok(), minitest::cstr(r.report()));
        T_COMPARE(r.net->name, std::string("a <b> & co"));
        T_COMPARE(r.net->transitions[0].label, std::string("raw <label>"));
    }

    void brokenXmlIsReported()
    {
        const LoadResult r1 = PnmlLoader::loadText("<pnml><net><place id='a'", "p.pnml");
        T_VERIFY(!r1.ok());
        T_VERIFY(hasIssue(r1, LoadIssue::Severity::Error, "$", ""));

        // Not PNML at all: any error at the root, no net.
        const LoadResult r2 = PnmlLoader::loadText("{\"places\": []}", "p.json");
        T_VERIFY(!r2.ok());
        T_VERIFY(r2.hasErrors());
        T_VERIFY(!hasIssue(r2, LoadIssue::Severity::Warning, "", ""));
    }

    void loadsShippedExample()
    {
        const LoadResult r = PnmlLoader::loadFile(std::string(EXAMPLES_DIR "/coffee-pnml.pnml"));
        T_VERIFY2(r.ok(), minitest::cstr(r.report()));
        const pn::Net &net = *r.net;
        T_COMPARE(net.name, std::string("coffee-pnml"));
        T_COMPARE(net.places.size(), std::size_t(5));
        T_COMPARE(net.transitions.size(), std::size_t(5));

        const auto serve = net.findTransition("serve");
        T_VERIFY(serve.has_value());
        T_VERIFY(net.transitions[*serve].milestone);
        T_COMPARE(net.transitions[*serve].milestoneRank, pn::TransitionIndex(2));
        T_COMPARE(net.transitions[*serve].kind, pn::TransitionKind::Observable);
        T_COMPARE(net.transitions[*serve].label, std::string("drink.served"));

        const auto flush = net.findTransition("?flush");
        T_VERIFY(flush.has_value());
        T_COMPARE(net.transitions[*flush].kind, pn::TransitionKind::Controllable);
        T_COMPARE(net.transitions[*flush].label, std::string("flush"));
        // The inhibitor arc from 'idle' refuses flush while idle.
        T_VERIFY(net.transitions[*flush].inhibit.size() == 1);
        T_COMPARE(net.transitions[*flush].inhibit[0].place, *net.findPlace("idle"));

        // switch_off consumes brewing AND the lamp: the machine can leave the
        // serving cycle (PNML cannot declare target markings).
        const auto off = net.findTransition("switch_off");
        T_VERIFY(off.has_value());
        T_COMPARE(net.transitions[*off].kind, pn::TransitionKind::Controllable);
        T_COMPARE(net.transitions[*off].label, std::string("power.off"));
        T_VERIFY(net.transitions[*off].pre.size() == 2);

        // The union: idle and lamp carry one token each.
        T_COMPARE(net.initialMarkings.size(), std::size_t(1));
        T_VERIFY(net.initialMarkings[0].tokens.size() == 2);
    }

    void missingFile()
    {
        const LoadResult r = PnmlLoader::loadFile("/no/such/file.pnml");
        T_VERIFY(!r.ok());
        T_VERIFY(hasIssue(r, LoadIssue::Severity::Error, "/no/such/file.pnml", "cannot open"));
    }

    void pnmlModelRunsInASession()
    {
        // The imported model drives a real campaign: the milestone transition
        // is certainly stepped and covered. (The attract pheromone it left is
        // wiped again once the edge saturates -- the exact deposit amounts
        // are tested in tst_session, on the rebuild.)
        const LoadResult r = PnmlLoader::loadFile(std::string(EXAMPLES_DIR "/coffee-pnml.pnml"));
        T_VERIFY2(r.ok(), minitest::cstr(r.report()));

        pn::SessionConfig cfg;
        cfg.maxRuns = 60;
        cfg.analyzeModel = false;
        pn::Session session(*r.net, cfg);
        testsut::FakeSut sut(*r.net);
        const pn::CampaignReport rep = session.runCampaign(sut);
        T_VERIFY(rep.complete);
        T_VERIFY(rep.summary.transitionsCovered > 0);
        T_VERIFY(session.coverage().transitionHits(*r.net->findTransition("serve")) > 0);
    }

    // DeepSeek V4.1 Flash: the export (doku section 27). The standard part
    // must re-import as the same net; the eggembt-only part is annotations.
    void exportRoundTrip()
    {
        pn::NetBuilder b("coffee & tea");
        const pn::PlaceIndex idle = b.place("idle");
        const pn::PlaceIndex busy = b.place("busy", 3);
        const pn::PlaceIndex lamp = b.place("lamp");
        const pn::TransitionIndex press = b.transition("press", pn::TransitionKind::Controllable, "coin<insert>");
        const pn::TransitionIndex serve = b.transition("serve", pn::TransitionKind::Observable, "drink.served");
        const pn::TransitionIndex tick = b.transition("tick", pn::TransitionKind::Internal);
        b.input(idle, press).output(press, busy).output(press, lamp);
        b.input(busy, serve, 2).input(lamp, serve).output(serve, idle);
        b.inhibitor(lamp, tick).input(idle, tick).output(tick, idle);
        b.milestone(serve, 2);
        b.rare(serve, pn::RareSpec{0.002, 9, 7000});  // DeepSeek V4.1 Flash: rare hunt (doku section 30)
        b.initial("start", {{idle, 1}, {lamp, 1}});
        b.initial("late", {{busy, 2}});
        b.targetMarking("served", {{idle, 1}});
        b.errorMarking("stuck", {{busy, 1}}, pn::MatchMode::Exact,
                       pn::ErrorOptions{pn::TargetPolicy::Once, 1, pn::DumpMode::Crash, 2});
        pn::Net net = b.build();
        net.strategy.goals = pn::GoalPriority::Coverage;
        net.strategy.errors = pn::ErrorPriority::Avoid;

        const std::string doc = pn::toPnml(net);
        T_VERIFY(doc.rfind("<?xml", 0) == 0);
        T_VERIFY(doc.find("<pnml") != std::string::npos);
        T_VERIFY(doc.find("target &quot;served&quot;") != std::string::npos);
        T_VERIFY(doc.find("bound 3") != std::string::npos);
        T_VERIFY(doc.find("PNML has one initial marking") != std::string::npos);

        const LoadResult r = PnmlLoader::loadText(doc, "export.pnml");
        T_VERIFY2(r.ok(), minitest::cstr(r.report()));
        T_COMPARE(r.net->name, std::string("coffee & tea"));  // escaping survives
        T_COMPARE(r.net->places.size(), net.places.size());
        T_COMPARE(r.net->transitions.size(), net.transitions.size());

        const auto press2 = r.net->findTransition("press");
        T_VERIFY(press2.has_value());
        T_COMPARE(r.net->transitions[*press2].kind, pn::TransitionKind::Controllable);
        T_COMPARE(r.net->transitions[*press2].label, std::string("coin<insert>"));
        const auto serve2 = r.net->findTransition("serve");
        T_VERIFY(serve2.has_value());
        T_COMPARE(r.net->transitions[*serve2].kind, pn::TransitionKind::Observable);
        T_VERIFY(r.net->transitions[*serve2].milestone);
        T_COMPARE(r.net->transitions[*serve2].milestoneRank, pn::TransitionIndex(2));
        // Weight 2 on busy -> serve survives.
        bool weighted = false;
        for (const pn::WeightedArc &a : r.net->transitions[*serve2].pre)
            if (a.place == *r.net->findPlace("busy") && a.weight == 2)
                weighted = true;
        T_VERIFY(weighted);
        const auto tick2 = r.net->findTransition("tick");
        T_VERIFY(tick2.has_value());
        T_COMPARE(r.net->transitions[*tick2].kind, pn::TransitionKind::Internal);
        T_COMPARE(r.net->transitions[*tick2].label, std::string());
        T_VERIFY(!r.net->transitions[*tick2].traced);
        T_VERIFY(r.net->transitions[*tick2].inhibit.size() == 1);
        T_COMPARE(r.net->transitions[*tick2].inhibit[0].place, *r.net->findPlace("lamp"));

        // Standard PNML carries exactly the FIRST initial marking ...
        T_COMPARE(r.net->initialMarkings.size(), std::size_t(1));
        T_COMPARE(r.net->initialMarkings[0].tokens.size(), std::size_t(2));
        // ... and no bounds or targets, which are annotations only.
        const auto busy2 = r.net->findPlace("busy");
        T_VERIFY(busy2.has_value());
        T_VERIFY(!r.net->places[*busy2].bound.has_value());
        T_VERIFY(r.net->targetMarkings.empty());
        T_VERIFY(r.net->errorMarkings.empty());

        // The machine-readable block keeps everything the round trip lost.
        JsonValue ann;
        T_VERIFY(annotationJson(doc, ann));
        T_COMPARE(ann.find("name")->asString(), std::string("coffee & tea"));
        const JsonValue *strategy = ann.find("strategy");
        T_VERIFY(strategy != nullptr);
        T_COMPARE(strategy->find("goals")->asString(), std::string("coverage"));
        T_COMPARE(strategy->find("errors")->asString(), std::string("avoid"));
        T_COMPARE(ann.find("initial")->items().size(), std::size_t(2));
        T_COMPARE(ann.find("target")->items().size(), std::size_t(1));
        T_COMPARE(ann.find("error")->items().size(), std::size_t(1));
        const JsonValue *error = &ann.find("error")->items()[0];
        T_COMPARE(error->find("name")->asString(), std::string("stuck"));
        T_COMPARE(error->find("policy")->asString(), std::string("once"));
        T_COMPARE(error->find("crash_dump")->asString(), std::string("crash"));
        T_COMPARE(error->find("max_dumps")->asNumber(), 2.0);
        const JsonValue *places = ann.find("places");
        T_VERIFY(places != nullptr);
        T_COMPARE(places->items().size(), std::size_t(1));
        T_COMPARE(places->items()[0].find("bound")->asNumber(), 3.0);

        // DeepSeek V4.1 Flash: the rare hunting contract travels in both the
        // human-readable comment and the machine-readable annotation (doku
        // section 30).
        const JsonValue *transitions = ann.find("transitions");
        T_VERIFY(transitions != nullptr);
        const JsonValue *serveAnn = nullptr;
        for (const JsonValue &t : transitions->items())
            if (const JsonValue *id = t.find("id"); id && id->asString() == "serve")
                serveAnn = &t;
        T_VERIFY(serveAnn != nullptr);
        const JsonValue *rare = serveAnn->find("rare");
        T_VERIFY(rare != nullptr);
        T_VERIFY(rare->find("p")->asNumber() > 0.0019 && rare->find("p")->asNumber() < 0.0021);
        T_COMPARE(rare->find("attempts")->asNumber(), 9.0);
        T_COMPARE(rare->find("wait-for")->asNumber(), 7000.0);
        T_VERIFY(doc.find("rare p") != std::string::npos);
        T_VERIFY(doc.find("wait-for 7000 ms") != std::string::npos);
    }

    // DeepSeek V4.1 Flash: ids that are not XML names cannot be PNML ids. The
    // replacement keeps the document readable; the original stays in <name>
    // and in the annotations.
    void exportUnsafeIdsStillLoad()
    {
        pn::NetBuilder b("bad & <net>");
        const pn::PlaceIndex p = b.place("1 bad place");
        const pn::TransitionIndex t = b.transition("t<x", pn::TransitionKind::Controllable, "do&it");
        b.input(p, t).output(t, p);
        b.initial("go", {{p, 1}});
        const pn::Net net = b.build();

        const std::string doc = pn::toPnml(net);
        T_VERIFY(doc.find("id=\"p0\"") != std::string::npos);
        T_VERIFY(doc.find("id=\"t0\"") != std::string::npos);
        T_VERIFY(doc.find("1 bad place") != std::string::npos);
        T_VERIFY(doc.find("t&lt;x") != std::string::npos);

        const LoadResult r = PnmlLoader::loadText(doc, "unsafe.pnml");
        T_VERIFY2(r.ok(), minitest::cstr(r.report()));
        T_COMPARE(r.net->name, std::string("bad & <net>"));
        T_COMPARE(r.net->transitions[0].label, std::string("do&it"));
        T_COMPARE(r.net->places[0].id, std::string("p0"));
    }

    // DeepSeek V4.1 Flash: the shipped PNML example survives an export and a
    // re-import structurally (ids that are not XML names may change).
    void exportShippedExampleRoundTrips()
    {
        const LoadResult r = PnmlLoader::loadFile(std::string(EXAMPLES_DIR "/coffee-pnml.pnml"));
        T_VERIFY2(r.ok(), minitest::cstr(r.report()));
        const std::string doc = pn::toPnml(*r.net);
        const LoadResult again = PnmlLoader::loadText(doc, "again.pnml");
        T_VERIFY2(again.ok(), minitest::cstr(again.report()));

        T_COMPARE(again.net->places.size(), r.net->places.size());
        T_COMPARE(again.net->transitions.size(), r.net->transitions.size());
        for (std::size_t i = 0; i < r.net->transitions.size(); ++i) {
            const pn::Transition &a = r.net->transitions[i];
            const pn::Transition &b2 = again.net->transitions[i];
            T_COMPARE(b2.kind, a.kind);
            T_COMPARE(b2.label, a.label);
            T_COMPARE(b2.milestone, a.milestone);
            if (a.milestone)
                T_COMPARE(b2.milestoneRank, a.milestoneRank);
            T_COMPARE(b2.pre.size(), a.pre.size());
            T_COMPARE(b2.post.size(), a.post.size());
            T_COMPARE(b2.inhibit.size(), a.inhibit.size());
        }
        T_COMPARE(again.net->initialMarkings.size(), r.net->initialMarkings.size());
        T_COMPARE(again.net->initialMarkings[0].tokens.size(), r.net->initialMarkings[0].tokens.size());
    }
};

int main()
{
    T_RUN(TstPnml, baselineIsValid);
    T_RUN(TstPnml, milestoneTag);
    T_RUN(TstPnml, milestoneDefaultsToRankOne);
    T_RUN(TstPnml, milestoneNeedsVisibility);
    T_RUN(TstPnml, prefixFallback);
    T_RUN(TstPnml, explicitLabelMakesInternalTraced);
    T_RUN(TstPnml, controllableWithoutLabelFallsBackToId);
    T_RUN(TstPnml, unknownKindIsAnError);
    T_RUN(TstPnml, otherToolsSpecificsAreIgnored);
    T_RUN(TstPnml, inhibitorArcs);
    T_RUN(TstPnml, weights);
    T_RUN(TstPnml, unionOfInitialMarkings);
    T_RUN(TstPnml, duplicateIdsAndCollisions);
    T_RUN(TstPnml, arcProblems);
    T_RUN(TstPnml, noTokensIsAnError);
    T_RUN(TstPnml, placeToolspecificWarns);
    T_RUN(TstPnml, xmlNoiseIsTolerated);
    T_RUN(TstPnml, brokenXmlIsReported);
    T_RUN(TstPnml, loadsShippedExample);
    T_RUN(TstPnml, missingFile);
    T_RUN(TstPnml, pnmlModelRunsInASession);
    // DeepSeek V4.1 Flash: PNML export (doku section 27).
    T_RUN(TstPnml, exportRoundTrip);
    T_RUN(TstPnml, exportUnsafeIdsStillLoad);
    T_RUN(TstPnml, exportShippedExampleRoundTrips);
    return T_SUMMARY();
}
