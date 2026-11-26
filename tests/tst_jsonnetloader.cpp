#include "io/JsonNetLoader.h"
#include "io/JsonNetWriter.h"  // DeepSeek V4.1 Flash: the annotation path (doku section 40)

#include "MiniTest.h"

#include <cmath>
#include <string>

using pn::JsonNetLoader;
using pn::LoadIssue;
using pn::LoadResult;

namespace {

// GLM-5.3-Flash: for the crash prediction's parsed probabilities.
bool near(double a, double b, double eps = 1e-9)
{
    return std::fabs(a - b) <= eps;
}

// A small valid net; individual tests replace one section to break it.
const char *kPlaces = R"([{"id":"p","bound":3},{"id":"q"}])";
const char *kTransitions =
    R"([{"id":"t","label":"go","kind":"controllable"},{"id":"u","kind":"internal"}])";
const char *kArcs =
    R"([{"from":"p","to":"t"},{"from":"t","to":"q"},{"from":"q","to":"u"},{"from":"u","to":"p"}])";
const char *kMarkings =
    R"({"initial":[{"name":"s","tokens":{"p":1}}],"target":[{"name":"f","tokens":{"p":1}}]})";

std::string makeDoc(const char *places = kPlaces, const char *transitions = kTransitions,
                   const char *arcs = kArcs, const char *markings = kMarkings)
{
    return std::string("{\"places\":") + places + ",\"transitions\":" + transitions
           + ",\"arcs\":" + arcs + ",\"markings\":" + markings + "}";
}

bool hasIssue(const LoadResult &r, LoadIssue::Severity sev, const std::string &path,
              const std::string &textPart = std::string())
{
    for (const LoadIssue &i : r.issues) {
        if (i.severity == sev && i.path == path && minitest::contains(i.message, textPart))
            return true;
    }
    return false;
}

// DeepSeek V4.1 Flash: semantic comparison for the writer's round-trip tests.
// The written document may omit default-valued keys, so the text differs from
// the input; the loaded net must not.
bool sameTokens(const pn::SparseMarking &a, const pn::SparseMarking &b)
{
    if (a.size() != b.size())
        return false;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (a[i].place != b[i].place || a[i].tokens != b[i].tokens)
            return false;
    return true;
}

bool sameArcs(const std::vector<pn::WeightedArc> &a, const std::vector<pn::WeightedArc> &b)
{
    if (a.size() != b.size())
        return false;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (a[i].place != b[i].place || a[i].weight != b[i].weight)
            return false;
    return true;
}

bool sameCrash(const std::optional<pn::CrashPrediction> &a,
               const std::optional<pn::CrashPrediction> &b)
{
    if (a.has_value() != b.has_value())
        return false;
    if (!a)
        return true;
    return a->crashClass == b->crashClass && a->p == b->p && a->attempts == b->attempts
           && a->crashDump == b->crashDump && a->maxDumps == b->maxDumps;
}

bool sameMarking(const pn::NamedMarking &a, const pn::NamedMarking &b)
{
    return a.name == b.name && sameTokens(a.tokens, b.tokens) && a.match == b.match
           && a.intentional == b.intentional && a.policy == b.policy
           && a.targetHits == b.targetHits && a.crashDump == b.crashDump
           && a.maxDumps == b.maxDumps && sameCrash(a.crash, b.crash);
}

bool sameTransition(const pn::Transition &a, const pn::Transition &b)
{
    if (a.id != b.id || a.label != b.label || a.kind != b.kind || a.traced != b.traced
        || a.synchronized != b.synchronized || a.unpredictable != b.unpredictable
        || a.error != b.error || a.crashDump != b.crashDump || a.maxDumps != b.maxDumps
        || a.milestone != b.milestone || a.milestoneRank != b.milestoneRank
        || !sameCrash(a.crashPrediction, b.crashPrediction))
        return false;
    if (a.command.has_value() != b.command.has_value())
        return false;
    if (a.command && (a.command->run != b.command->run || a.command->mode != b.command->mode
                      || a.command->times != b.command->times || a.command->when != b.command->when))
        return false;
    if (a.rare.has_value() != b.rare.has_value())
        return false;
    if (a.rare && (a.rare->p != b.rare->p || a.rare->attempts != b.rare->attempts
                   || a.rare->waitForMs != b.rare->waitForMs))
        return false;
    return sameArcs(a.pre, b.pre) && sameArcs(a.post, b.post) && sameArcs(a.inhibit, b.inhibit);
}

// Writes the net, loads the document again, and compares everything the
// loader and the writer know about.
void checkWriterRoundTrip(const LoadResult &first)
{
    T_VERIFY2(first.ok(), minitest::cstr(first.report()));
    if (!first.ok())
        return;
    const std::string doc = pn::toJsonModel(*first.net);
    const LoadResult second = JsonNetLoader::loadJson(doc);
    T_VERIFY2(second.ok(), minitest::cstr(second.report()));
    if (!second.ok())
        return;
    T_VERIFY2(!second.hasWarnings(), minitest::cstr(second.report()));

    const pn::Net &a = *first.net;
    const pn::Net &b = *second.net;
    T_COMPARE(b.name, a.name);
    T_COMPARE(b.strategy.goals, a.strategy.goals);
    T_COMPARE(b.strategy.errors, a.strategy.errors);
    T_COMPARE(b.places.size(), a.places.size());
    for (std::size_t i = 0; i < a.places.size() && i < b.places.size(); ++i) {
        T_COMPARE(b.places[i].id, a.places[i].id);
        T_COMPARE(b.places[i].bound, a.places[i].bound);
    }
    T_COMPARE(b.transitions.size(), a.transitions.size());
    for (std::size_t i = 0; i < a.transitions.size() && i < b.transitions.size(); ++i)
        T_VERIFY2(sameTransition(a.transitions[i], b.transitions[i]),
                  a.transitions[i].id.c_str());

    const auto sameList = [](const std::vector<pn::NamedMarking> &x,
                             const std::vector<pn::NamedMarking> &y) {
        if (x.size() != y.size())
            return false;
        for (std::size_t i = 0; i < x.size(); ++i)
            if (!sameMarking(x[i], y[i]))
                return false;
        return true;
    };
    T_VERIFY(sameList(a.initialMarkings, b.initialMarkings));
    T_VERIFY(sameList(a.targetMarkings, b.targetMarkings));
    T_VERIFY(sameList(a.errorMarkings, b.errorMarkings));
}

}  // namespace

struct TstJsonNetLoader {
    void baselineIsValid()
    {
        const LoadResult r = JsonNetLoader::loadJson(makeDoc());
        T_VERIFY2(r.ok(), minitest::cstr(r.report()));
        T_VERIFY(!r.hasWarnings());
    }

    void loadsExampleFile()
    {
        const LoadResult r =
            JsonNetLoader::loadFile(std::string(EXAMPLES_DIR "/elevator-door.json"));
        T_VERIFY2(r.ok(), minitest::cstr(r.report()));
        T_VERIFY2(r.issues.empty(), minitest::cstr(r.report()));

        const pn::Net &net = *r.net;
        T_COMPARE(net.name, std::string("elevator-door"));
        T_COMPARE(net.places.size(), std::size_t(6));
        T_COMPARE(net.transitions.size(), std::size_t(7));

        const auto closeCmd = net.findTransition("close_cmd");
        T_VERIFY(closeCmd.has_value());
        const pn::Transition &cc = net.transitions[*closeCmd];
        T_COMPARE(cc.kind, pn::TransitionKind::Controllable);
        T_COMPARE(cc.label, std::string("door.close"));
        T_COMPARE(cc.pre.size(), std::size_t(1));
        T_COMPARE(cc.post.size(), std::size_t(1));
        T_COMPARE(cc.inhibit.size(), std::size_t(1));
        T_COMPARE(cc.inhibit[0].place, *net.findPlace("obstructed"));
        T_COMPARE(cc.inhibit[0].weight, pn::TokenCount(1));

        T_COMPARE(net.initialMarkings.size(), std::size_t(1));
        T_COMPARE(net.targetMarkings.size(), std::size_t(1));
        T_COMPARE(net.errorMarkings.size(), std::size_t(1));
        T_COMPARE(net.errorMarkings[0].match, pn::MatchMode::Covers);
        T_COMPARE(net.errorMarkings[0].policy, pn::TargetPolicy::UntilCovered);
        T_VERIFY(net.errorMarkings[0].targetHits.has_value());
        T_COMPARE(*net.errorMarkings[0].targetHits, pn::TokenCount(3));
        T_COMPARE(net.errorMarkings[0].crashDump, pn::DumpMode::Snapshot);
        T_COMPARE(net.targetMarkings[0].match, pn::MatchMode::Exact);
    }

    void inhibitorWeightIsParsed()
    {
        const char *arcs =
            R"([{"from":"p","to":"t"},{"from":"t","to":"q"},{"from":"q","to":"u"},)"
            R"({"from":"u","to":"p"},{"from":"q","to":"t","type":"inhibitor","weight":2}])";
        const LoadResult r = JsonNetLoader::loadJson(makeDoc(kPlaces, kTransitions, arcs));
        T_VERIFY2(r.ok(), minitest::cstr(r.report()));
        const pn::Transition &t = r.net->transitions[*r.net->findTransition("t")];
        T_COMPARE(t.inhibit.size(), std::size_t(1));
        T_COMPARE(t.inhibit[0].weight, pn::TokenCount(2));
    }

    void rejectsInhibitorFromTransitionToPlace()
    {
        const char *arcs = R"([{"from":"t","to":"q","type":"inhibitor"}])";
        const LoadResult r = JsonNetLoader::loadJson(makeDoc(kPlaces, kTransitions, arcs));
        T_VERIFY(!r.ok());
        T_VERIFY2(hasIssue(r, LoadIssue::Severity::Error, "$.arcs[0].type", "place to a transition"),
                 minitest::cstr(r.report()));
    }

    void rejectsPlaceToPlaceArc()
    {
        const char *arcs = R"([{"from":"p","to":"q"}])";
        const LoadResult r = JsonNetLoader::loadJson(makeDoc(kPlaces, kTransitions, arcs));
        T_VERIFY(!r.ok());
        T_VERIFY2(hasIssue(r, LoadIssue::Severity::Error, "$.arcs[0]", "place and a transition"),
                 minitest::cstr(r.report()));
    }

    void rejectsUnknownNode()
    {
        const char *arcs = R"([{"from":"p","to":"nope"}])";
        const LoadResult r = JsonNetLoader::loadJson(makeDoc(kPlaces, kTransitions, arcs));
        T_VERIFY(!r.ok());
        T_VERIFY2(hasIssue(r, LoadIssue::Severity::Error, "$.arcs[0].to", "unknown node"),
                 minitest::cstr(r.report()));
    }

    void rejectsDuplicateArcAndZeroWeight()
    {
        const char *arcs =
            R"([{"from":"p","to":"t"},{"from":"p","to":"t"},{"from":"t","to":"q","weight":0}])";
        const LoadResult r = JsonNetLoader::loadJson(makeDoc(kPlaces, kTransitions, arcs));
        T_VERIFY(!r.ok());
        T_VERIFY2(hasIssue(r, LoadIssue::Severity::Error, "$.arcs[1]", "duplicate arc"),
                 minitest::cstr(r.report()));
        T_VERIFY2(hasIssue(r, LoadIssue::Severity::Error, "$.arcs[2].weight"),
                 minitest::cstr(r.report()));
    }

    void normalAndInhibitorArcOnSamePairAreDistinct()
    {
        const char *arcs =
            R"([{"from":"p","to":"t"},{"from":"p","to":"t","type":"inhibitor","weight":3},)"
            R"({"from":"t","to":"q"},{"from":"q","to":"u"},{"from":"u","to":"p"}])";
        const LoadResult r = JsonNetLoader::loadJson(makeDoc(kPlaces, kTransitions, arcs));
        T_VERIFY2(r.ok(), minitest::cstr(r.report()));
    }

    void idsAreSharedBetweenPlacesAndTransitions()
    {
        const char *transitions = R"([{"id":"p","kind":"internal"}])";
        const LoadResult r = JsonNetLoader::loadJson(makeDoc(kPlaces, transitions));
        T_VERIFY(!r.ok());
        T_VERIFY2(hasIssue(r, LoadIssue::Severity::Error, "$.transitions[0].id", "duplicate id"),
                 minitest::cstr(r.report()));
    }

    void visibleTransitionsNeedLabel()
    {
        const char *transitions =
            R"([{"id":"t","kind":"observable"},{"id":"u","kind":"controllable","label":""}])";
        const LoadResult r = JsonNetLoader::loadJson(makeDoc(kPlaces, transitions));
        T_VERIFY(!r.ok());
        T_VERIFY(hasIssue(r, LoadIssue::Severity::Error, "$.transitions[0]", "label"));
        T_VERIFY(hasIssue(r, LoadIssue::Severity::Error, "$.transitions[1]", "label"));
    }

    void invalidKindIsReported()
    {
        const char *transitions = R"([{"id":"t","kind":"magic"}])";
        const LoadResult r = JsonNetLoader::loadJson(makeDoc(kPlaces, transitions));
        T_VERIFY(!r.ok());
        T_VERIFY(hasIssue(r, LoadIssue::Severity::Error, "$.transitions[0].kind"));
    }

    // GLM-5.3-Flash: the explicit "traced" key on internal transitions.
    // Default: a labeled internal step is traced, an unlabeled one is silent.
    // The key makes a NAMED internal step silent ("traced": false), so a
    // label can exist for display without making the step visible to the
    // tester; "traced": true on a labeled internal is the (allowed) no-op.
    void tracedKeyDefaultsAndOverrides()
    {
        const char *transitions =
            R"([{"id":"t","label":"go","kind":"controllable"},)"
            R"({"id":"u","kind":"internal"},)"
            R"({"id":"v","kind":"internal","label":"work"},)"
            R"({"id":"w","kind":"internal","label":"work","traced":false},)"
            R"({"id":"x","kind":"internal","label":"busy","traced":true}])";
        const LoadResult r = JsonNetLoader::loadJson(makeDoc(kPlaces, transitions));
        T_VERIFY2(r.ok(), minitest::cstr(r.report()));

        const pn::Net &net = *r.net;
        T_VERIFY(!net.transitions[*net.findTransition("u")].traced);   // unlabeled: silent
        T_VERIFY(net.transitions[*net.findTransition("v")].traced);   // labeled: traced
        T_VERIFY(!net.transitions[*net.findTransition("w")].traced);  // named but silent
        T_VERIFY(net.transitions[*net.findTransition("x")].traced);   // explicit no-op
        T_VERIFY(net.transitions[*net.findTransition("t")].traced);   // controllable: always
    }

    void tracedKeyIsRejectedWhereItDoesNotApply()
    {
        // true without a label: there is nothing the SUT could report.
        const char *unlabeled = R"([{"id":"u","kind":"internal","traced":true}])";
        const LoadResult r1 = JsonNetLoader::loadJson(makeDoc(kPlaces, unlabeled));
        T_VERIFY(!r1.ok());
        T_VERIFY(hasIssue(r1, LoadIssue::Severity::Error, "$.transitions[0].traced", "label"));

        // The visibility of controllable/observable transitions is fixed.
        const char *visibleKind = R"([{"id":"t","label":"go","kind":"observable","traced":false}])";
        const LoadResult r2 = JsonNetLoader::loadJson(makeDoc(kPlaces, visibleKind));
        T_VERIFY(!r2.ok());
        T_VERIFY(hasIssue(r2, LoadIssue::Severity::Error, "$.transitions[0].traced", "internal"));

        // Not a boolean.
        const char *notBool = R"([{"id":"v","kind":"internal","label":"work","traced":"yes"}])";
        const LoadResult r3 = JsonNetLoader::loadJson(makeDoc(kPlaces, notBool));
        T_VERIFY(!r3.ok());
        T_VERIFY(hasIssue(r3, LoadIssue::Severity::Error, "$.transitions[0].traced", "true or false"));
    }

    void initialMarkingMustRespectBounds()
    {
        const char *markings = R"({"initial":[{"name":"s","tokens":{"p":4}}]})";
        const LoadResult r =
            JsonNetLoader::loadJson(makeDoc(kPlaces, kTransitions, kArcs, markings));
        T_VERIFY(!r.ok());
        T_VERIFY2(hasIssue(r, LoadIssue::Severity::Error, "$.markings.initial[0].tokens.p", "bound"),
                 minitest::cstr(r.report()));
    }

    void errorMarkingAboveBoundIsOnlyAWarning()
    {
        const char *markings =
            R"({"initial":[{"name":"s","tokens":{"p":1}}],)"
            R"("target":[{"name":"f","tokens":{"p":1}}],)"
            R"("error":[{"name":"e","tokens":{"p":9}}]})";
        const LoadResult r =
            JsonNetLoader::loadJson(makeDoc(kPlaces, kTransitions, kArcs, markings));
        T_VERIFY2(r.ok(), minitest::cstr(r.report()));
        T_VERIFY(hasIssue(r, LoadIssue::Severity::Warning, "$.markings.error[0].tokens.p", "never"));
    }

    void markingMatchModes()
    {
        const char *markings =
            R"({"initial":[{"name":"s","tokens":{"p":1}}],)"
            R"("target":[{"name":"f","tokens":{"p":1}}],)"
            R"("error":[{"name":"e1","match":"covers","tokens":{"q":2}},)"
            R"({"name":"e2","tokens":{"q":0,"p":1}},{"name":"e3","match":"fuzzy","tokens":{}}]})";
        const LoadResult r =
            JsonNetLoader::loadJson(makeDoc(kPlaces, kTransitions, kArcs, markings));
        T_VERIFY(!r.ok());
        T_VERIFY(hasIssue(r, LoadIssue::Severity::Error, "$.markings.error[2].match"));

        // Without the bad entry the others load, zero entries are dropped.
        const char *good =
            R"({"initial":[{"name":"s","tokens":{"p":1}}],)"
            R"("target":[{"name":"f","tokens":{"p":1}}],)"
            R"("error":[{"name":"e1","match":"covers","tokens":{"q":2}},)"
            R"({"name":"e2","tokens":{"q":0,"p":1}}]})";
        const LoadResult g = JsonNetLoader::loadJson(makeDoc(kPlaces, kTransitions, kArcs, good));
        T_VERIFY2(g.ok(), minitest::cstr(g.report()));
        T_COMPARE(g.net->errorMarkings[0].match, pn::MatchMode::Covers);
        T_COMPARE(g.net->errorMarkings[1].match, pn::MatchMode::Exact);
        T_COMPARE(g.net->errorMarkings[1].tokens.size(), std::size_t(1));
    }

    void initialMarkingIsRequired()
    {
        const char *markings = R"({"target":[{"name":"f","tokens":{"p":1}}]})";
        const LoadResult r =
            JsonNetLoader::loadJson(makeDoc(kPlaces, kTransitions, kArcs, markings));
        T_VERIFY(!r.ok());
        T_VERIFY(hasIssue(r, LoadIssue::Severity::Error, "$.markings", "initial"));
    }

    void missingTargetMarkingsWarns()
    {
        const char *markings = R"({"initial":[{"name":"s","tokens":{"p":1}}]})";
        const LoadResult r =
            JsonNetLoader::loadJson(makeDoc(kPlaces, kTransitions, kArcs, markings));
        T_VERIFY2(r.ok(), minitest::cstr(r.report()));
        T_VERIFY(hasIssue(r, LoadIssue::Severity::Warning, "$.markings.target", "deadlock"));
    }

    // DeepSeek V4.1 Flash: the model's testing strategy (doku section 27).
    void strategyIsParsed()
    {
        const LoadResult plain = JsonNetLoader::loadJson(makeDoc());
        T_VERIFY2(plain.ok(), minitest::cstr(plain.report()));
        T_COMPARE(plain.net->strategy.goals, pn::GoalPriority::Balanced);
        T_COMPARE(plain.net->strategy.errors, pn::ErrorPriority::Balanced);

        const LoadResult r = JsonNetLoader::loadJson(
            "{\"strategy\":{\"goals\":\"coverage\",\"errors\":\"seek\"}," + makeDoc().substr(1));
        T_VERIFY2(r.ok(), minitest::cstr(r.report()));
        T_VERIFY(!r.hasWarnings());
        T_COMPARE(r.net->strategy.goals, pn::GoalPriority::Coverage);
        T_COMPARE(r.net->strategy.errors, pn::ErrorPriority::Seek);
    }

    void strategyValuesAreValidated()
    {
        const LoadResult bad =
            JsonNetLoader::loadJson("{\"strategy\":{\"goals\":\"fast\"}," + makeDoc().substr(1));
        T_VERIFY(!bad.ok());
        T_VERIFY(hasIssue(bad, LoadIssue::Severity::Error, "$.strategy.goals", "balanced"));

        const LoadResult badErrors =
            JsonNetLoader::loadJson("{\"strategy\":{\"errors\":\"ignore\"}," + makeDoc().substr(1));
        T_VERIFY(!badErrors.ok());
        T_VERIFY(hasIssue(badErrors, LoadIssue::Severity::Error, "$.strategy.errors", "seek"));

        const LoadResult shape = JsonNetLoader::loadJson("{\"strategy\":[]," + makeDoc().substr(1));
        T_VERIFY(!shape.ok());
        T_VERIFY(hasIssue(shape, LoadIssue::Severity::Error, "$.strategy", "object"));

        const LoadResult typo =
            JsonNetLoader::loadJson("{\"strategy\":{\"goal\":\"targets\"}," + makeDoc().substr(1));
        T_VERIFY2(typo.ok(), minitest::cstr(typo.report()));
        T_VERIFY(hasIssue(typo, LoadIssue::Severity::Warning, "$.strategy.goal", "unknown key"));
    }

    void unknownKeysAreWarnings()
    {
        const char *arcs =
            R"([{"from":"p","to":"t","wieght":2},{"from":"t","to":"q"},)"
            R"({"from":"q","to":"u"},{"from":"u","to":"p"}])";
        const LoadResult r = JsonNetLoader::loadJson(makeDoc(kPlaces, kTransitions, arcs));
        T_VERIFY2(r.ok(), minitest::cstr(r.report()));
        T_VERIFY(hasIssue(r, LoadIssue::Severity::Warning, "$.arcs[0].wieght"));
    }

    // DeepSeek V4.1 Flash: the reserved lifecycle labels (doku section 26) are
    // recognized on controllable transitions only; an arc-less lifecycle input
    // is legitimate and does not get the "no arcs" warning.
    void reservedLifecycleLabelsAreParsed()
    {
        const char *transitions =
            R"([{"id":"r","label":"restart","kind":"controllable"},)"
            R"({"id":"o","label":"start","kind":"observable"}])";
        const LoadResult r =
            JsonNetLoader::loadJson(makeDoc(kPlaces, transitions, "[]",
                                            R"({"initial":[{"name":"s","tokens":{"p":1}}]})"));
        T_VERIFY2(r.ok(), minitest::cstr(r.report()));
        const pn::Net &net = *r.net;
        T_COMPARE(net.transitions[0].lifecycle, pn::LifecycleAction::Restart);
        T_COMPARE(net.transitions[1].lifecycle, pn::LifecycleAction::None);
        T_VERIFY(hasIssue(r, LoadIssue::Severity::Warning, "$.transitions[0]", "lifecycle"));
        T_VERIFY(hasIssue(r, LoadIssue::Severity::Warning, "$.transitions[1]", "reserved"));
        T_VERIFY(!hasIssue(r, LoadIssue::Severity::Warning, "$.transitions[0]", "no arcs"));
    }

    void arclessTransitionWarns()
    {
        const char *transitions =
            R"([{"id":"t","label":"go","kind":"controllable"},{"id":"u","kind":"internal"},)"
            R"({"id":"lonely","kind":"internal"}])";
        const LoadResult r = JsonNetLoader::loadJson(makeDoc(kPlaces, transitions));
        T_VERIFY2(r.ok(), minitest::cstr(r.report()));
        T_VERIFY(hasIssue(r, LoadIssue::Severity::Warning, "$.transitions[2]", "no arcs"));
    }

    void collectsSeveralErrorsInOnePass()
    {
        const char *places = R"([{"id":"p","bound":-1},{"id":""}])";
        const char *arcs = R"([{"from":"x","to":"y"}])";
        const LoadResult r = JsonNetLoader::loadJson(makeDoc(places, kTransitions, arcs));
        T_VERIFY(!r.ok());
        T_VERIFY(hasIssue(r, LoadIssue::Severity::Error, "$.places[0].bound"));
        T_VERIFY(hasIssue(r, LoadIssue::Severity::Error, "$.places[1].id"));
        T_VERIFY(hasIssue(r, LoadIssue::Severity::Error, "$.arcs[0].from"));
        T_VERIFY(hasIssue(r, LoadIssue::Severity::Error, "$.arcs[0].to"));
    }

    void errorPolicyFieldsAreParsed()
    {
        const char *markings =
            R"({"initial":[{"name":"s","tokens":{"p":1}}],)"
            R"("target":[{"name":"f","tokens":{"p":1}}],"error":[)"
            R"({"name":"e1","match":"covers","tokens":{"q":1},"policy":"until_covered",)"
            R"("target_hits":3,"crash_dump":"snapshot","max_dumps":2},)"
            R"({"name":"e2","tokens":{"q":2},"policy":"once","crash_dump":"crash"},)"
            R"({"name":"e3","tokens":{"q":3},"policy":"avoid"},)"
            R"({"name":"e4","tokens":{"q":4}}]})";
        const LoadResult r =
            JsonNetLoader::loadJson(makeDoc(kPlaces, kTransitions, kArcs, markings));
        T_VERIFY2(r.ok(), minitest::cstr(r.report()));
        T_VERIFY2(r.issues.empty(), minitest::cstr(r.report()));

        const auto &e = r.net->errorMarkings;
        T_COMPARE(e.size(), std::size_t(4));

        T_COMPARE(e[0].policy, pn::TargetPolicy::UntilCovered);
        T_COMPARE(*e[0].targetHits, pn::TokenCount(3));
        T_COMPARE(e[0].crashDump, pn::DumpMode::Snapshot);
        T_COMPARE(e[0].maxDumps, std::uint32_t(2));
        T_COMPARE(pn::effectiveTargetHits(e[0], 7), std::uint32_t(3));

        T_COMPARE(e[1].policy, pn::TargetPolicy::Once);
        T_COMPARE(e[1].crashDump, pn::DumpMode::Crash);
        T_COMPARE(e[1].maxDumps, std::uint32_t(1));  // default
        T_COMPARE(pn::effectiveTargetHits(e[1], 7), std::uint32_t(1));

        T_COMPARE(e[2].policy, pn::TargetPolicy::Avoid);
        T_COMPARE(pn::effectiveTargetHits(e[2], 7), std::uint32_t(0));

        // Defaults: seek until the global threshold, spec fault (no dump).
        T_COMPARE(e[3].policy, pn::TargetPolicy::UntilCovered);
        T_VERIFY(!e[3].targetHits.has_value());
        T_COMPARE(e[3].crashDump, pn::DumpMode::None);
        T_COMPARE(pn::effectiveTargetHits(e[3], 7), std::uint32_t(7));
    }

    void invalidErrorPolicyFieldsAreRejected()
    {
        const char *markings =
            R"({"initial":[{"name":"s","tokens":{"p":1}}],"error":[)"
            R"({"name":"a","tokens":{"q":1},"policy":"sometimes"},)"
            R"({"name":"b","tokens":{"q":2},"target_hits":0},)"
            R"({"name":"c","tokens":{"q":3},"crash_dump":"explode"},)"
            R"({"name":"d","tokens":{"q":4},"max_dumps":-1}]})";
        const LoadResult r =
            JsonNetLoader::loadJson(makeDoc(kPlaces, kTransitions, kArcs, markings));
        T_VERIFY(!r.ok());
        T_VERIFY2(hasIssue(r, LoadIssue::Severity::Error, "$.markings.error[0].policy"),
                 minitest::cstr(r.report()));
        T_VERIFY(hasIssue(r, LoadIssue::Severity::Error, "$.markings.error[1].target_hits"));
        T_VERIFY(hasIssue(r, LoadIssue::Severity::Error, "$.markings.error[2].crash_dump"));
        T_VERIFY(hasIssue(r, LoadIssue::Severity::Error, "$.markings.error[3].max_dumps"));
    }

    void ineffectiveErrorSettingsWarn()
    {
        const char *markings =
            R"({"initial":[{"name":"s","tokens":{"p":1}}],)"
            R"("target":[{"name":"f","tokens":{"p":1},"policy":"once"}],"error":[)"
            R"({"name":"a","tokens":{"q":1},"policy":"once","target_hits":5},)"
            R"({"name":"b","tokens":{"q":2},"max_dumps":3}]})";
        const LoadResult r =
            JsonNetLoader::loadJson(makeDoc(kPlaces, kTransitions, kArcs, markings));
        T_VERIFY2(r.ok(), minitest::cstr(r.report()));
        T_VERIFY(hasIssue(r, LoadIssue::Severity::Warning, "$.markings.target[0].policy", "error markings"));
        T_VERIFY(hasIssue(r, LoadIssue::Severity::Warning, "$.markings.error[0].target_hits", "fixes"));
        T_VERIFY(hasIssue(r, LoadIssue::Severity::Warning, "$.markings.error[1].max_dumps", "crash_dump"));
    }

    // GLM-5.3-Flash: "intentional" marks a target marking as a good target state.
    void intentionalTargetMarkingsAreParsed()
    {
        const char *markings =
            R"({"initial":[{"name":"s","tokens":{"p":1}}],)"
            R"("target":[{"name":"f","tokens":{"p":1},"intentional":true},)"
            R"({"name":"g","tokens":{"q":1}}]})";
        const LoadResult r =
            JsonNetLoader::loadJson(makeDoc(kPlaces, kTransitions, kArcs, markings));
        T_VERIFY2(r.ok(), minitest::cstr(r.report()));
        T_VERIFY2(r.issues.empty(), minitest::cstr(r.report()));
        T_VERIFY(r.net->targetMarkings[0].intentional);
        T_VERIFY(!r.net->targetMarkings[1].intentional);  // the default

        // On other roles the tag is meaningless and warns.
        const char *warnMarkings =
            R"({"initial":[{"name":"s","tokens":{"p":1},"intentional":true}],)"
            R"("error":[{"name":"e","tokens":{"q":1},"intentional":true}]})";
        const LoadResult w =
            JsonNetLoader::loadJson(makeDoc(kPlaces, kTransitions, kArcs, warnMarkings));
        T_VERIFY2(w.ok(), minitest::cstr(w.report()));
        T_VERIFY(hasIssue(w, LoadIssue::Severity::Warning, "$.markings.initial[0].intentional", "target markings"));
        T_VERIFY(hasIssue(w, LoadIssue::Severity::Warning, "$.markings.error[0].intentional", "target markings"));
    }

    // GLM-5.3-Flash: the transition tags unpredictable / error / crash_dump /
    // max_dumps.
    void transitionTagsAreParsed()
    {
        const char *transitions =
            R"([{"id":"t","label":"go","kind":"controllable"},)"
            R"({"id":"u","kind":"internal"},)"
            R"({"id":"v","label":"boom","kind":"observable","unpredictable":true,"error":true,)"
            R"("crash_dump":"crash","max_dumps":4}])";
        const char *arcs =
            R"([{"from":"p","to":"t"},{"from":"t","to":"q"},{"from":"q","to":"u"},{"from":"u","to":"p"},)"
            R"({"from":"q","to":"v"},{"from":"v","to":"p"}])";
        const LoadResult r = JsonNetLoader::loadJson(makeDoc(kPlaces, transitions, arcs));
        T_VERIFY2(r.ok(), minitest::cstr(r.report()));
        T_VERIFY2(r.issues.empty(), minitest::cstr(r.report()));

        const pn::Transition &v = r.net->transitions[*r.net->findTransition("v")];
        T_VERIFY(v.unpredictable);
        T_VERIFY(v.error);
        T_COMPARE(v.crashDump, pn::DumpMode::Crash);
        T_COMPARE(v.maxDumps, std::uint32_t(4));

        const pn::Transition &t = r.net->transitions[*r.net->findTransition("t")];
        T_VERIFY(!t.unpredictable && !t.error);  // the defaults
        T_COMPARE(t.crashDump, pn::DumpMode::None);
        T_COMPARE(t.maxDumps, std::uint32_t(1));
    }

    // GLM-5.3-Flash: settings that cannot have an effect on a transition warn.
    void ineffectiveTransitionSettingsWarn()
    {
        const char *transitions =
            R"([{"id":"t","label":"go","kind":"controllable","policy":"once","target_hits":2},)"
            R"({"id":"u","kind":"internal"},)"
            R"({"id":"v","label":"boom","kind":"observable","crash_dump":"snapshot"},)"
            R"({"id":"w","label":"zap","kind":"observable","error":true,"max_dumps":3}])";
        const LoadResult r = JsonNetLoader::loadJson(makeDoc(kPlaces, transitions, kArcs));
        T_VERIFY2(r.ok(), minitest::cstr(r.report()));
        T_VERIFY(hasIssue(r, LoadIssue::Severity::Warning, "$.transitions[0].policy", "no effect"));
        T_VERIFY(hasIssue(r, LoadIssue::Severity::Warning, "$.transitions[0].target_hits", "no effect"));
        T_VERIFY(hasIssue(r, LoadIssue::Severity::Warning, "$.transitions[2].crash_dump", "\"error\": true"));
        T_VERIFY(hasIssue(r, LoadIssue::Severity::Warning, "$.transitions[3].max_dumps", "crash_dump"));
    }

    // GLM-5.3-Flash: an error tag on a step the tester cannot see is rejected:
    // its steps would never be reported, so the tester could never know it
    // certainly fired.
    void errorTransitionMustBeVisible()
    {
        const char *transitions =
            R"([{"id":"t","label":"go","kind":"controllable"},)"
            R"({"id":"u","kind":"internal","error":true}])";
        const LoadResult r = JsonNetLoader::loadJson(makeDoc(kPlaces, transitions, kArcs));
        T_VERIFY(!r.ok());
        T_VERIFY2(hasIssue(r, LoadIssue::Severity::Error, "$.transitions[1].error", "visible"),
                  minitest::cstr(r.report()));
    }

    // GLM-5.3-Flash: the "crash" key predicts a nondeterministic SUT crash
    // (doku section 17). Three spellings on a transition -- a class word, a
    // number (shorthand for {"p": ...}), a full object -- and the same value
    // on a target or error marking.
    void crashKeyFormsAreParsed()
    {
        {   // the class word
            const char *transitions =
                R"([{"id":"t","label":"go","kind":"controllable","crash":"sometimes"},)"
                R"({"id":"u","kind":"internal"}])";
            const LoadResult r = JsonNetLoader::loadJson(makeDoc(kPlaces, transitions, kArcs));
            T_VERIFY(r.ok());
            const auto &cp = r.net->transitions[0].crashPrediction;
            T_VERIFY(cp.has_value());
            T_COMPARE(toString(cp->crashClass), std::string("sometimes"));
            T_VERIFY(!cp->p.has_value());
            T_VERIFY(!cp->attempts.has_value());
            T_COMPARE(cp->crashDump, pn::DumpMode::None);
            T_COMPARE(cp->maxDumps, 1u);
            T_VERIFY(!r.net->transitions[1].crashPrediction.has_value());
        }
        {   // the number shorthand: {"p": 0.7} on the neutral class "random"
            const char *transitions =
                R"([{"id":"t","label":"go","kind":"controllable","crash":0.7},)"
                R"({"id":"u","kind":"internal"}])";
            const LoadResult r = JsonNetLoader::loadJson(makeDoc(kPlaces, transitions, kArcs));
            T_VERIFY(r.ok());
            const auto &cp = r.net->transitions[0].crashPrediction;
            T_VERIFY(cp.has_value());
            T_COMPARE(toString(cp->crashClass), std::string("random"));
            T_VERIFY(cp->p.has_value() && near(*cp->p, 0.7));
        }
        {   // the full object
            const char *transitions =
                R"([{"id":"t","label":"go","kind":"controllable",)"
                R"("crash":{"class":"rare","p":0.3,"attempts":7,"crash_dump":"snapshot","max_dumps":2}},)"
                R"({"id":"u","kind":"internal"}])";
            const LoadResult r = JsonNetLoader::loadJson(makeDoc(kPlaces, transitions, kArcs));
            T_VERIFY(r.ok());
            const auto &cp = r.net->transitions[0].crashPrediction;
            T_VERIFY(cp.has_value());
            T_COMPARE(toString(cp->crashClass), std::string("rare"));
            T_VERIFY(cp->p.has_value() && near(*cp->p, 0.3));
            T_VERIFY(cp->attempts.has_value() && *cp->attempts == 7);
            T_COMPARE(cp->crashDump, pn::DumpMode::Snapshot);
            T_COMPARE(cp->maxDumps, 2u);
        }
        {   // on a target marking
            const char *markings =
                R"({"initial":[{"name":"s","tokens":{"p":1}}],)"
                R"("target":[{"name":"f","tokens":{"p":1},"crash":"often"}]})";
            const LoadResult r = JsonNetLoader::loadJson(makeDoc(kPlaces, kTransitions, kArcs, markings));
            T_VERIFY(r.ok());
            T_VERIFY(r.net->targetMarkings[0].crash.has_value());
            T_COMPARE(toString(r.net->targetMarkings[0].crash->crashClass), std::string("often"));
        }
    }

    // Where the prediction cannot sit: an invisible step (its crash could
    // never be attributed), an error transition (a certain reaction, not a
    // probable crash), an initial marking (nothing is observed before the
    // campaign starts the SUT there) and a policy "avoid" marking (the
    // tester avoids it from the start).
    void crashKeyIsRejectedWhereItDoesNotApply()
    {
        {   // an untraced internal step is never visible
            const char *transitions =
                R"([{"id":"t","label":"go","kind":"controllable"},)"
                R"({"id":"u","kind":"internal","crash":"rare"}])";
            const LoadResult r = JsonNetLoader::loadJson(makeDoc(kPlaces, transitions, kArcs));
            T_VERIFY(!r.ok());
            T_VERIFY2(hasIssue(r, LoadIssue::Severity::Error, "$.transitions[1].crash", "visible"),
                      minitest::cstr(r.report()));
        }
        {   // an error transition fails certainly
            const char *transitions =
                R"([{"id":"t","label":"go","kind":"controllable","error":true,"crash":"rare"},)"
                R"({"id":"u","kind":"internal"}])";
            const LoadResult r = JsonNetLoader::loadJson(makeDoc(kPlaces, transitions, kArcs));
            T_VERIFY(!r.ok());
            T_VERIFY2(hasIssue(r, LoadIssue::Severity::Error, "$.transitions[0].crash", "certainly"),
                      minitest::cstr(r.report()));
        }
        {   // an initial marking: the campaign starts the SUT there
            const char *markings =
                R"({"initial":[{"name":"s","tokens":{"p":1},"crash":"often"}],)"
                R"("target":[{"name":"f","tokens":{"p":1}}]})";
            const LoadResult r = JsonNetLoader::loadJson(makeDoc(kPlaces, kTransitions, kArcs, markings));
            T_VERIFY(!r.ok());
            T_VERIFY2(hasIssue(r, LoadIssue::Severity::Error, "$.markings.initial[0].crash", "initial marking"),
                      minitest::cstr(r.report()));
        }
        {   // a policy "avoid" marking is not sought
            const char *markings =
                R"({"initial":[{"name":"s","tokens":{"p":1}}],)"
                R"("target":[{"name":"f","tokens":{"p":1}}],)"
                R"("error":[{"name":"e","tokens":{"p":1},"policy":"avoid","crash":"often"}]})";
            const LoadResult r = JsonNetLoader::loadJson(makeDoc(kPlaces, kTransitions, kArcs, markings));
            T_VERIFY(!r.ok());
            T_VERIFY2(hasIssue(r, LoadIssue::Severity::Error, "$.markings.error[0].crash", "avoid"),
                      minitest::cstr(r.report()));
        }
    }

    // The prediction's numbers must mean something: a probability strictly
    // inside (0,1) (a crash that never or always happens is not a
    // prediction), a known class word, a give-up budget of at least 1.
    void crashPredictionValuesAreValidated()
    {
        {   // the number must lie strictly inside (0,1)
            for (const char *bad : {"0", "1", "1.5", "-0.2"}) {
                const std::string transitions =
                    std::string(R"([{"id":"t","label":"go","kind":"controllable","crash":)") + bad
                    + R"(},{"id":"u","kind":"internal"}])";
                const LoadResult r = JsonNetLoader::loadJson(makeDoc(kPlaces, transitions.c_str(), kArcs));
                T_VERIFY(!r.ok());
                T_VERIFY2(hasIssue(r, LoadIssue::Severity::Error, "$.transitions[0].crash", "between"),
                          minitest::cstr(r.report()));
            }
        }
        {   // an unknown class word
            const char *transitions =
                R"([{"id":"t","label":"go","kind":"controllable","crash":"occasionally"},)"
                R"({"id":"u","kind":"internal"}])";
            const LoadResult r = JsonNetLoader::loadJson(makeDoc(kPlaces, transitions, kArcs));
            T_VERIFY(!r.ok());
            T_VERIFY(hasIssue(r, LoadIssue::Severity::Error, "$.transitions[0].crash", "unknown crash class"));
        }
        {   // a give-up budget of 0
            const char *transitions =
                R"([{"id":"t","label":"go","kind":"controllable","crash":{"attempts":0}},)"
                R"({"id":"u","kind":"internal"}])";
            const LoadResult r = JsonNetLoader::loadJson(makeDoc(kPlaces, transitions, kArcs));
            T_VERIFY(!r.ok());
            T_VERIFY(hasIssue(r, LoadIssue::Severity::Error, "$.transitions[0].crash.attempts"));
        }
        {   // an explicit "p" outside (0,1)
            const char *transitions =
                R"([{"id":"t","label":"go","kind":"controllable","crash":{"p":1.5}},)"
                R"({"id":"u","kind":"internal"}])";
            const LoadResult r = JsonNetLoader::loadJson(makeDoc(kPlaces, transitions, kArcs));
            T_VERIFY(!r.ok());
            T_VERIFY(hasIssue(r, LoadIssue::Severity::Error, "$.transitions[0].crash.p", "between"));
        }
        {   // neither word, number nor object
            const char *transitions =
                R"([{"id":"t","label":"go","kind":"controllable","crash":true},)"
                R"({"id":"u","kind":"internal"}])";
            const LoadResult r = JsonNetLoader::loadJson(makeDoc(kPlaces, transitions, kArcs));
            T_VERIFY(!r.ok());
            T_VERIFY(hasIssue(r, LoadIssue::Severity::Error, "$.transitions[0].crash", "object"));
        }
    }

    // GLM-5.3-Flash: the "command" key (doku section 18) in both spellings.
    void commandKeyFormsAreParsed()
    {
        {   // the string shorthand: a synchronous hook without a budget or guard
            const char *transitions =
                R"([{"id":"t","label":"go","kind":"controllable","command":"collect.sh"},)"
                R"({"id":"u","kind":"internal"}])";
            const LoadResult r = JsonNetLoader::loadJson(makeDoc(kPlaces, transitions, kArcs));
            T_VERIFY2(r.ok(), minitest::cstr(r.report()));
            T_VERIFY(!r.hasWarnings());
            const pn::CommandHook &h = *r.net->transitions[*r.net->findTransition("t")].command;
            T_COMPARE(h.run, std::string("collect.sh"));
            T_COMPARE(h.mode, pn::HookMode::Sync);
            T_VERIFY(!h.times.has_value());
            T_COMPARE(h.when, pn::kNoPlace);
        }
        {   // the object form with every field
            const char *transitions =
                R"([{"id":"t","label":"go","kind":"controllable",)"
                R"("command":{"run":"verify.sh","mode":"async","times":3,"when":"q"}},)"
                R"({"id":"u","kind":"internal"}])";
            const LoadResult r = JsonNetLoader::loadJson(makeDoc(kPlaces, transitions, kArcs));
            T_VERIFY2(r.ok(), minitest::cstr(r.report()));
            T_VERIFY(!r.hasWarnings());
            const pn::CommandHook &h = *r.net->transitions[*r.net->findTransition("t")].command;
            T_COMPARE(h.run, std::string("verify.sh"));
            T_COMPARE(h.mode, pn::HookMode::Async);
            T_VERIFY(h.times.has_value());
            T_COMPARE(*h.times, pn::TokenCount(3));
            T_COMPARE(h.when, *r.net->findPlace("q"));
        }
        {   // "once" is spelled times 1; sync is the explicit default
            const char *transitions =
                R"([{"id":"t","label":"go","kind":"controllable",)"
                R"("command":{"run":"x.sh","times":1}},)"
                R"({"id":"u","kind":"internal"}])";
            const LoadResult r = JsonNetLoader::loadJson(makeDoc(kPlaces, transitions, kArcs));
            T_VERIFY2(r.ok(), minitest::cstr(r.report()));
            const pn::CommandHook &h = *r.net->transitions[*r.net->findTransition("t")].command;
            T_VERIFY(h.times.has_value());
            T_COMPARE(*h.times, pn::TokenCount(1));
            T_COMPARE(h.mode, pn::HookMode::Sync);
        }
    }

    // GLM-5.3-Flash: the "command" key's values are validated.
    void commandValuesAreValidated()
    {
        {   // an unknown mode word
            const char *transitions =
                R"([{"id":"t","label":"go","kind":"controllable","command":{"run":"x.sh","mode":"later"}},)"
                R"({"id":"u","kind":"internal"}])";
            const LoadResult r = JsonNetLoader::loadJson(makeDoc(kPlaces, transitions, kArcs));
            T_VERIFY(!r.ok());
            T_VERIFY2(hasIssue(r, LoadIssue::Severity::Error, "$.transitions[0].command.mode",
                               "must be \"sync\" or \"async\""),
                     minitest::cstr(r.report()));
        }
        {   // a budget of 0
            const char *transitions =
                R"([{"id":"t","label":"go","kind":"controllable","command":{"run":"x.sh","times":0}},)"
                R"({"id":"u","kind":"internal"}])";
            const LoadResult r = JsonNetLoader::loadJson(makeDoc(kPlaces, transitions, kArcs));
            T_VERIFY(!r.ok());
            T_VERIFY(hasIssue(r, LoadIssue::Severity::Error, "$.transitions[0].command.times"));
        }
        {   // a guard naming no place of the net
            const char *transitions =
                R"([{"id":"t","label":"go","kind":"controllable","command":{"run":"x.sh","when":"zz"}},)"
                R"({"id":"u","kind":"internal"}])";
            const LoadResult r = JsonNetLoader::loadJson(makeDoc(kPlaces, transitions, kArcs));
            T_VERIFY(!r.ok());
            T_VERIFY(hasIssue(r, LoadIssue::Severity::Error, "$.transitions[0].command.when", "unknown place"));
        }
        {   // an empty command line, in both spellings (the object form names "run")
            const char *transitions =
                R"([{"id":"t","label":"go","kind":"controllable","command":""},)"
                R"({"id":"u","kind":"internal"}])";
            const LoadResult r = JsonNetLoader::loadJson(makeDoc(kPlaces, transitions, kArcs));
            T_VERIFY(!r.ok());
            T_VERIFY(hasIssue(r, LoadIssue::Severity::Error, "$.transitions[0].command",
                              "must not be empty"));
            const char *objectForm =
                R"([{"id":"t","label":"go","kind":"controllable","command":{"run":""}},)"
                R"({"id":"u","kind":"internal"}])";
            const LoadResult r2 = JsonNetLoader::loadJson(makeDoc(kPlaces, objectForm, kArcs));
            T_VERIFY(!r2.ok());
            T_VERIFY(hasIssue(r2, LoadIssue::Severity::Error, "$.transitions[0].command.run",
                              "must not be empty"));
        }
        {   // a missing "run" in the object form
            const char *transitions =
                R"([{"id":"t","label":"go","kind":"controllable","command":{"mode":"async"}},)"
                R"({"id":"u","kind":"internal"}])";
            const LoadResult r = JsonNetLoader::loadJson(makeDoc(kPlaces, transitions, kArcs));
            T_VERIFY(!r.ok());
            T_VERIFY(hasIssue(r, LoadIssue::Severity::Error, "$.transitions[0].command", "missing required key"));
        }
        {   // neither string nor object
            const char *transitions =
                R"([{"id":"t","label":"go","kind":"controllable","command":42},)"
                R"({"id":"u","kind":"internal"}])";
            const LoadResult r = JsonNetLoader::loadJson(makeDoc(kPlaces, transitions, kArcs));
            T_VERIFY(!r.ok());
            T_VERIFY(hasIssue(r, LoadIssue::Severity::Error, "$.transitions[0].command",
                              "must be a command line string or an object"));
        }
        {   // unknown keys inside the object are warned about
            const char *transitions =
                R"([{"id":"t","label":"go","kind":"controllable","command":{"run":"x.sh","exit":0}},)"
                R"({"id":"u","kind":"internal"}])";
            const LoadResult r = JsonNetLoader::loadJson(makeDoc(kPlaces, transitions, kArcs));
            T_VERIFY2(r.ok(), minitest::cstr(r.report()));
            T_VERIFY(hasIssue(r, LoadIssue::Severity::Warning, "$.transitions[0].command.exit", "unknown key"));
        }
    }

    // GLM-5.3-Flash: a command needs a VISIBLE step -- an untraced internal
    // step is never seen, so its command could never fire.
    void commandNeedsVisibleStep()
    {
        {   // the untraced internal transition is rejected
            const char *transitions =
                R"([{"id":"t","label":"go","kind":"controllable"},)"
                R"({"id":"u","kind":"internal","command":"spy.sh"}])";
            const LoadResult r = JsonNetLoader::loadJson(makeDoc(kPlaces, transitions, kArcs));
            T_VERIFY(!r.ok());
            T_VERIFY(hasIssue(r, LoadIssue::Severity::Error, "$.transitions[1].command",
                              "a command needs a visible step"));
        }
        {   // a TRACED internal transition may carry one
            const char *transitions =
                R"([{"id":"t","label":"go","kind":"controllable"},)"
                R"({"id":"u","kind":"internal","label":"audit","traced":true,"command":"spy.sh"}])";
            const LoadResult r = JsonNetLoader::loadJson(makeDoc(kPlaces, transitions, kArcs));
            T_VERIFY2(r.ok(), minitest::cstr(r.report()));
            T_VERIFY(r.net->transitions[*r.net->findTransition("u")].command.has_value());
        }
    }

    // DeepSeek V4.1 Flash: the "sync" key (doku section 19) on the SUT's own moves.
    void syncKeyIsParsed()
    {
        const char *transitions =
            R"([{"id":"t","label":"go","kind":"controllable"},)"
            R"({"id":"u","label":"out","kind":"observable","sync":true},)"
            R"({"id":"v","label":"in","kind":"observable"},)"
            R"({"id":"w","label":"audit","kind":"internal","traced":true,"sync":true}])";
        const LoadResult r = JsonNetLoader::loadJson(makeDoc(kPlaces, transitions, kArcs));
        T_VERIFY2(r.ok(), minitest::cstr(r.report()));
        T_VERIFY(!r.net->transitions[*r.net->findTransition("t")].synchronized);
        T_VERIFY(r.net->transitions[*r.net->findTransition("u")].synchronized);
        T_VERIFY(!r.net->transitions[*r.net->findTransition("v")].synchronized);
        T_VERIFY(r.net->transitions[*r.net->findTransition("w")].synchronized);
    }

    // DeepSeek V4.1 Flash: "sync" holds only visible moves of the SUT.
    void syncKeyIsRejectedWhereItDoesNotApply()
    {
        {   // a controllable transition is acknowledged as a stimulus already
            const char *transitions =
                R"([{"id":"t","label":"go","kind":"controllable","sync":true},)"
                R"({"id":"u","kind":"internal"}])";
            const LoadResult r = JsonNetLoader::loadJson(makeDoc(kPlaces, transitions, kArcs));
            T_VERIFY(!r.ok());
            T_VERIFY(hasIssue(r, LoadIssue::Severity::Error, "$.transitions[0].sync", "own move"));
        }
        {   // an untraced internal step is never seen
            const char *transitions =
                R"([{"id":"t","label":"go","kind":"controllable"},)"
                R"({"id":"u","kind":"internal","sync":true}])";
            const LoadResult r = JsonNetLoader::loadJson(makeDoc(kPlaces, transitions, kArcs));
            T_VERIFY(!r.ok());
            T_VERIFY(hasIssue(r, LoadIssue::Severity::Error, "$.transitions[1].sync", "visible"));
        }
        {   // and the value must be a boolean
            const char *transitions =
                R"([{"id":"t","label":"go","kind":"controllable"},)"
                R"({"id":"u","label":"out","kind":"observable","sync":"yes"}])";
            const LoadResult r = JsonNetLoader::loadJson(makeDoc(kPlaces, transitions, kArcs));
            T_VERIFY(!r.ok());
            T_VERIFY(hasIssue(r, LoadIssue::Severity::Error, "$.transitions[1].sync", "true or false"));
        }
    }

    // DeepSeek V4.1 Flash: the "rare" key (doku section 30). true means the
    // documented defaults; an object overrides them; false is no marker.
    void rareKeyFormsAreParsed()
    {
        const char *transitions =
            R"([{"id":"t","label":"go","kind":"controllable",)"
            R"("rare":{"p":0.002,"attempts":7,"wait-for":250}},)"
            R"({"id":"u","kind":"internal"},)"
            R"({"id":"v","label":"pop","kind":"observable","rare":true},)"
            R"({"id":"w","label":"nix","kind":"observable","rare":false}])";
        const char *arcs =
            R"([{"from":"p","to":"t"},{"from":"t","to":"q"},{"from":"q","to":"u"},{"from":"u","to":"p"},)"
            R"({"from":"q","to":"v"},{"from":"v","to":"p"},{"from":"q","to":"w"},{"from":"w","to":"p"}])";
        const LoadResult r = JsonNetLoader::loadJson(makeDoc(kPlaces, transitions, arcs));
        T_VERIFY2(r.ok(), minitest::cstr(r.report()));
        T_VERIFY2(r.issues.empty(), minitest::cstr(r.report()));

        const pn::Transition &t = r.net->transitions[*r.net->findTransition("t")];
        T_VERIFY(t.rare.has_value());
        T_VERIFY(near(t.rare->p, 0.002));
        T_COMPARE(t.rare->attempts, std::uint32_t(7));
        T_COMPARE(t.rare->waitForMs, std::uint32_t(250));

        const pn::Transition &v = r.net->transitions[*r.net->findTransition("v")];
        T_VERIFY(v.rare.has_value());
        T_VERIFY(near(v.rare->p, 0.001));  // the documented defaults
        T_COMPARE(v.rare->attempts, std::uint32_t(100));
        T_COMPARE(v.rare->waitForMs, std::uint32_t(0));

        T_VERIFY(!r.net->transitions[*r.net->findTransition("w")].rare.has_value());
    }

    // DeepSeek V4.1 Flash: a rare transition must be visible, and its numbers
    // have to describe a hunt that can actually run.
    void rareKeyIsValidatedAndNeedsVisibility()
    {
        {   // an untraced internal step is never seen, so a hunt could never tell
            const char *transitions =
                R"([{"id":"t","label":"go","kind":"controllable"},)"
                R"({"id":"u","kind":"internal","rare":true}])";
            const LoadResult r = JsonNetLoader::loadJson(makeDoc(kPlaces, transitions, kArcs));
            T_VERIFY(!r.ok());
            T_VERIFY(hasIssue(r, LoadIssue::Severity::Error, "$.transitions[1].rare", "visible"));
        }
        {   // p must lie strictly between 0 and 1, attempts >= 1, wait-for >= 0
            const char *transitions =
                R"([{"id":"r1","label":"a","kind":"observable","rare":{"p":0}},)"
                R"({"id":"r2","label":"b","kind":"observable","rare":{"p":1}},)"
                R"({"id":"r3","label":"c","kind":"observable","rare":{"attempts":0}},)"
                R"({"id":"r4","label":"d","kind":"observable","rare":{"wait-for":-1}},)"
                R"({"id":"r5","label":"e","kind":"observable","rare":"yes"}])";
            const char *arcs =
                R"([{"from":"p","to":"r1"},{"from":"r1","to":"q"},{"from":"p","to":"r2"},)"
                R"({"from":"r2","to":"q"},{"from":"p","to":"r3"},{"from":"r3","to":"q"},)"
                R"({"from":"p","to":"r4"},{"from":"r4","to":"q"},{"from":"p","to":"r5"},)"
                R"({"from":"r5","to":"q"}])";
            const LoadResult r = JsonNetLoader::loadJson(makeDoc(kPlaces, transitions, arcs));
            T_VERIFY(!r.ok());
            T_VERIFY(hasIssue(r, LoadIssue::Severity::Error, "$.transitions[0].rare.p", "strictly between"));
            T_VERIFY(hasIssue(r, LoadIssue::Severity::Error, "$.transitions[1].rare.p", "strictly between"));
            T_VERIFY(hasIssue(r, LoadIssue::Severity::Error, "$.transitions[2].rare.attempts", "integer >= 1"));
            T_VERIFY(hasIssue(r, LoadIssue::Severity::Error, "$.transitions[3].rare.wait-for", "integer >= 0"));
            T_VERIFY(hasIssue(r, LoadIssue::Severity::Error, "$.transitions[4].rare", "true or an object"));
        }
    }

    // DeepSeek V4.1 Flash: the writer is the second half of the PNML import
    // path (doku section 40). Every shipped model survives write + reload with
    // the same meaning, and so does a model that uses every annotation the
    // loader understands.
    void writerRoundTripsTheShippedModels()
    {
        for (const char *file : {"drink-machine.json", "elevator-door.json", "atm.json"})
            checkWriterRoundTrip(
                JsonNetLoader::loadFile(std::string(EXAMPLES_DIR "/") + file));
    }

    void writerRoundTripsEveryAnnotation()
    {
        const char *doc = R"({
  "version": 1,
  "name": "writer-features",
  "places": [{"id": "p", "bound": 5}, {"id": "q"}, {"id": "r"}],
  "transitions": [
    {"id": "in", "kind": "controllable", "label": "in"},
    {"id": "out", "kind": "observable", "label": "out", "milestone": 2, "sync": true},
    {"id": "work", "kind": "internal", "label": "work", "traced": false},
    {"id": "bad", "kind": "observable", "label": "bad", "error": true,
     "crash_dump": "snapshot", "max_dumps": 3},
    {"id": "rareout", "kind": "observable", "label": "rareout",
     "rare": {"p": 0.01, "attempts": 7, "wait-for": 25}},
    {"id": "cmdout", "kind": "observable", "label": "cmdout",
     "command": {"run": "true", "mode": "async", "times": 2, "when": "p"}},
    {"id": "crashout", "kind": "observable", "label": "crashout", "unpredictable": true,
     "crash": {"class": "often", "p": 0.4, "attempts": 9, "crash_dump": "crash", "max_dumps": 2}}
  ],
  "arcs": [
    {"from": "p", "to": "in"}, {"from": "in", "to": "q"},
    {"from": "q", "to": "out"}, {"from": "out", "to": "p"},
    {"from": "q", "to": "work"}, {"from": "work", "to": "r"},
    {"from": "r", "to": "bad"}, {"from": "bad", "to": "p"},
    {"from": "p", "to": "rareout"}, {"from": "rareout", "to": "q"},
    {"from": "p", "to": "cmdout"}, {"from": "cmdout", "to": "q"},
    {"from": "p", "to": "crashout"}, {"from": "crashout", "to": "q"},
    {"from": "r", "to": "in", "type": "inhibitor", "weight": 2}
  ],
  "markings": {
    "initial": [{"name": "start", "tokens": {"p": 1}}],
    "target": [{"name": "done", "tokens": {"q": 1}, "match": "covers", "intentional": true,
                "crash": {"class": "sometimes", "p": 0.5}}],
    "error": [
      {"name": "oops", "tokens": {"r": 1}, "match": "covers", "policy": "once"},
      {"name": "bad2", "tokens": {"q": 2}, "target_hits": 2, "crash_dump": "crash",
       "max_dumps": 4}
    ]
  },
  "strategy": {"goals": "targets", "errors": "avoid"}
})";
        checkWriterRoundTrip(JsonNetLoader::loadJson(doc));
    }

    void unsupportedVersionIsRejected()
    {
        std::string doc = makeDoc();
        doc.insert(1, "\"version\":2,");
        const LoadResult r = JsonNetLoader::loadJson(doc);
        T_VERIFY(!r.ok());
        T_VERIFY(hasIssue(r, LoadIssue::Severity::Error, "$.version", "unsupported"));
    }

    void syntaxErrorReportsPosition()
    {
        const LoadResult r = JsonNetLoader::loadJson("{\n  \"places\": [,]\n}");
        T_VERIFY(!r.ok());
        T_VERIFY(r.net == std::nullopt);
        T_VERIFY2(hasIssue(r, LoadIssue::Severity::Error, "$", "line 2"), minitest::cstr(r.report()));
    }

    void topLevelMustBeObject()
    {
        const LoadResult r = JsonNetLoader::loadJson("[1,2,3]");
        T_VERIFY(!r.ok());
        T_VERIFY(hasIssue(r, LoadIssue::Severity::Error, "$", "object"));
    }

    void missingFileIsReported()
    {
        const LoadResult r = JsonNetLoader::loadFile(std::string("/no/such/file.json"));
        T_VERIFY(!r.ok());
        T_VERIFY(r.hasErrors());
    }
};

int main()
{
    T_RUN(TstJsonNetLoader, baselineIsValid);
    T_RUN(TstJsonNetLoader, loadsExampleFile);
    T_RUN(TstJsonNetLoader, inhibitorWeightIsParsed);
    T_RUN(TstJsonNetLoader, rejectsInhibitorFromTransitionToPlace);
    T_RUN(TstJsonNetLoader, rejectsPlaceToPlaceArc);
    T_RUN(TstJsonNetLoader, rejectsUnknownNode);
    T_RUN(TstJsonNetLoader, rejectsDuplicateArcAndZeroWeight);
    T_RUN(TstJsonNetLoader, normalAndInhibitorArcOnSamePairAreDistinct);
    T_RUN(TstJsonNetLoader, idsAreSharedBetweenPlacesAndTransitions);
    T_RUN(TstJsonNetLoader, visibleTransitionsNeedLabel);
    T_RUN(TstJsonNetLoader, invalidKindIsReported);
    T_RUN(TstJsonNetLoader, tracedKeyDefaultsAndOverrides);  // GLM-5.3-Flash
    T_RUN(TstJsonNetLoader, tracedKeyIsRejectedWhereItDoesNotApply);  // GLM-5.3-Flash
    T_RUN(TstJsonNetLoader, initialMarkingMustRespectBounds);
    T_RUN(TstJsonNetLoader, errorMarkingAboveBoundIsOnlyAWarning);
    T_RUN(TstJsonNetLoader, markingMatchModes);
    T_RUN(TstJsonNetLoader, initialMarkingIsRequired);
    T_RUN(TstJsonNetLoader, missingTargetMarkingsWarns);
    T_RUN(TstJsonNetLoader, strategyIsParsed);  // DeepSeek V4.1 Flash
    T_RUN(TstJsonNetLoader, strategyValuesAreValidated);  // DeepSeek V4.1 Flash
    T_RUN(TstJsonNetLoader, unknownKeysAreWarnings);
    T_RUN(TstJsonNetLoader, reservedLifecycleLabelsAreParsed);  // DeepSeek V4.1 Flash
    T_RUN(TstJsonNetLoader, arclessTransitionWarns);
    T_RUN(TstJsonNetLoader, collectsSeveralErrorsInOnePass);
    T_RUN(TstJsonNetLoader, errorPolicyFieldsAreParsed);
    T_RUN(TstJsonNetLoader, invalidErrorPolicyFieldsAreRejected);
    T_RUN(TstJsonNetLoader, ineffectiveErrorSettingsWarn);
    T_RUN(TstJsonNetLoader, intentionalTargetMarkingsAreParsed);  // GLM-5.3-Flash
    T_RUN(TstJsonNetLoader, transitionTagsAreParsed);  // GLM-5.3-Flash
    T_RUN(TstJsonNetLoader, ineffectiveTransitionSettingsWarn);  // GLM-5.3-Flash
    T_RUN(TstJsonNetLoader, errorTransitionMustBeVisible);  // GLM-5.3-Flash
    T_RUN(TstJsonNetLoader, crashKeyFormsAreParsed);  // GLM-5.3-Flash
    T_RUN(TstJsonNetLoader, crashKeyIsRejectedWhereItDoesNotApply);  // GLM-5.3-Flash
    T_RUN(TstJsonNetLoader, crashPredictionValuesAreValidated);  // GLM-5.3-Flash
    T_RUN(TstJsonNetLoader, commandKeyFormsAreParsed);  // GLM-5.3-Flash
    T_RUN(TstJsonNetLoader, commandValuesAreValidated);  // GLM-5.3-Flash
    T_RUN(TstJsonNetLoader, commandNeedsVisibleStep);  // GLM-5.3-Flash
    T_RUN(TstJsonNetLoader, syncKeyIsParsed);  // DeepSeek V4.1 Flash
    T_RUN(TstJsonNetLoader, syncKeyIsRejectedWhereItDoesNotApply);  // DeepSeek V4.1 Flash
    T_RUN(TstJsonNetLoader, rareKeyFormsAreParsed);  // DeepSeek V4.1 Flash
    T_RUN(TstJsonNetLoader, rareKeyIsValidatedAndNeedsVisibility);  // DeepSeek V4.1 Flash
    T_RUN(TstJsonNetLoader, writerRoundTripsTheShippedModels);  // DeepSeek V4.1 Flash
    T_RUN(TstJsonNetLoader, writerRoundTripsEveryAnnotation);  // DeepSeek V4.1 Flash
    T_RUN(TstJsonNetLoader, unsupportedVersionIsRejected);
    T_RUN(TstJsonNetLoader, syntaxErrorReportsPosition);
    T_RUN(TstJsonNetLoader, topLevelMustBeObject);
    T_RUN(TstJsonNetLoader, missingFileIsReported);
    return T_SUMMARY();
}
