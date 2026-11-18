// GLM-5.3-Flash: tests of the PNML import (doku section 12, item 6): the XML
// reader, the mbt toolspecific conventions (kinds, labels, milestones), the
// union of per-place initial markings, inhibitor arcs and weights, and the
// issue reporting for broken files. Also drives a session on the shipped
// example to prove a PNML model is usable end to end.
#include "io/PnmlLoader.h"

#include "engine/Session.h"
#include "FakeSut.h"
#include "MiniTest.h"

#include <algorithm>
#include <string>

using pn::JsonNetLoader;
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

// A small valid net; individual tests replace one section to break it.
const char *kNet = R"(<?xml version="1.0"?>
<pnml>
  <net id="n" type="P/T net">
    <name><text>tiny</text></name>
    <page id="p1">
      <place id="a"><initialMarking><text>2</text></initialMarking></place>
      <place id="b"/>
      <transition id="go"><toolspecific tool="mbt" kind="controllable" label="go!"/></transition>
      <transition id="done"><toolspecific tool="mbt" kind="observable" label="done"/></transition>
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
<transition id="t"><toolspecific tool="mbt" kind="controllable" label="t"/>
<toolspecific tool="mbt" kind="milestone" rank="3"/></transition>
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
<transition id="t"><toolspecific tool="mbt" kind="observable" label="t"/>
<toolspecific tool="mbt" kind="milestone"/></transition>
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
<transition id="t"><toolspecific tool="mbt" kind="milestone" rank="2"/></transition>
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
<transition id="tick"><toolspecific tool="mbt" kind="internal" label="tick.tock"/></transition>
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
<transition id="press"><toolspecific tool="mbt" kind="controllable"/></transition>
<arc source="a" target="press"/><arc source="press" target="a"/></net></pnml>)";
        const LoadResult r = PnmlLoader::loadText(doc, "p.pnml");
        T_VERIFY2(r.ok(), minitest::cstr(r.report()));
        T_COMPARE(r.net->transitions[0].label, std::string("press"));
    }

    void unknownKindIsAnError()
    {
        const std::string doc = R"(<?xml version="1.0"?>
<pnml><net id="n"><place id="a"><initialMarking><text>1</text></initialMarking></place>
<transition id="t"><toolspecific tool="mbt" kind="weird"/></transition>
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
<transition id="t"><toolspecific tool="mbt" kind="controllable" label="t"/></transition>
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
<transition id="t"><toolspecific tool="mbt" kind="controllable" label="t"/></transition>
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
<transition id="t"><toolspecific tool="mbt" kind="controllable" label="t"/></transition>
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
<place id="a"/><transition id="t"><toolspecific tool="mbt" kind="controllable" label="t"/></transition>
<arc source="a" target="t"/><arc source="t" target="a"/></net></pnml>)";
        const LoadResult r1 = PnmlLoader::loadText(dup, "p.pnml");
        T_VERIFY(!r1.ok());
        T_VERIFY(hasIssue(r1, LoadIssue::Severity::Error, "place 1", "duplicate"));

        const std::string collision = R"(<?xml version="1.0"?>
<pnml><net id="n"><place id="a"><initialMarking><text>1</text></initialMarking></place>
<transition id="a"><toolspecific tool="mbt" kind="controllable" label="t"/></transition>
<arc source="a" target="a"/></net></pnml>)";
        const LoadResult r2 = PnmlLoader::loadText(collision, "p.pnml");
        T_VERIFY(!r2.ok());
        T_VERIFY(hasIssue(r2, LoadIssue::Severity::Error, "transition 0", "duplicate"));
    }

    void arcProblems()
    {
        const std::string unknown = R"(<?xml version="1.0"?>
<pnml><net id="n"><place id="a"><initialMarking><text>1</text></initialMarking></place>
<transition id="t"><toolspecific tool="mbt" kind="controllable" label="t"/></transition>
<arc source="a" target="nope"/></net></pnml>)";
        const LoadResult r1 = PnmlLoader::loadText(unknown, "p.pnml");
        T_VERIFY(!r1.ok());
        T_VERIFY(hasIssue(r1, LoadIssue::Severity::Error, "arc 0", "unknown arc endpoint"));

        const std::string placeToPlace = R"(<?xml version="1.0"?>
<pnml><net id="n"><place id="a"><initialMarking><text>1</text></initialMarking></place>
<place id="b"/><transition id="t"><toolspecific tool="mbt" kind="controllable" label="t"/></transition>
<arc source="a" target="b"/></net></pnml>)";
        const LoadResult r2 = PnmlLoader::loadText(placeToPlace, "p.pnml");
        T_VERIFY(!r2.ok());
        T_VERIFY(hasIssue(r2, LoadIssue::Severity::Error, "arc 0", "place and a transition"));

        const std::string badType = R"(<?xml version="1.0"?>
<pnml><net id="n"><place id="a"><initialMarking><text>1</text></initialMarking></place>
<transition id="t"><toolspecific tool="mbt" kind="controllable" label="t"/></transition>
<arc source="a" target="t"><type value="reset"/></arc></net></pnml>)";
        const LoadResult r3 = PnmlLoader::loadText(badType, "p.pnml");
        T_VERIFY(!r3.ok());
        T_VERIFY(hasIssue(r3, LoadIssue::Severity::Error, "arc 0", "unknown arc type"));
    }

    void noTokensIsAnError()
    {
        const std::string doc = R"(<?xml version="1.0"?>
<pnml><net id="n"><place id="a"/><transition id="t"><toolspecific tool="mbt" kind="controllable" label="t"/></transition>
<arc source="a" target="t"/><arc source="t" target="a"/></net></pnml>)";
        const LoadResult r = PnmlLoader::loadText(doc, "p.pnml");
        T_VERIFY(!r.ok());
        T_VERIFY(hasIssue(r, LoadIssue::Severity::Error, "$", "initial marking"));
    }

    void placeToolspecificWarns()
    {
        const std::string doc = R"(<?xml version="1.0"?>
<pnml><net id="n"><place id="a"><initialMarking><text>1</text></initialMarking>
<toolspecific tool="mbt" kind="final"/></place>
<transition id="t"><toolspecific tool="mbt" kind="controllable" label="t"/></transition>
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
<transition id="t"><toolspecific tool="mbt" kind="observable" label="raw &lt;label&gt;"/></transition>
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
    return T_SUMMARY();
}
