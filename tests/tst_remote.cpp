// Tests for the JSON codec, the protocol messages, and a Session that drives a
// SUT through a RemotePlugin and an in-memory loopback to a simulated agent.
#include "ElevatorModel.h"
#include "FakeSut.h"
#include "engine/AgentServer.h"
#include "engine/Json.h"
#include "engine/Protocol.h"
#include "engine/RemotePlugin.h"
#include "engine/Session.h"
#include "model/NetBuilder.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace pn;
using testsut::FakeSut;
using testsut::FakeSutOptions;

static int g_failures = 0;

#define CHECK(cond)                                                                       \
    do {                                                                                  \
        if (!(cond)) {                                                                    \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                                                 \
        }                                                                                 \
    } while (0)

static bool contains(const std::string &text, const char *part)
{
    return text.find(part) != std::string::npos;
}

// ===========================================================================
// JSON
// ===========================================================================

static JsonValue parseOk(const std::string &text)
{
    JsonValue v;
    std::string error;
    const bool ok = JsonValue::parse(text, v, &error);
    CHECK(ok);
    if (!ok)
        std::fprintf(stderr, "  parse error for %s: %s\n", text.c_str(), error.c_str());
    return v;
}

static bool parseFails(const std::string &text)
{
    JsonValue v;
    std::string error;
    return !JsonValue::parse(text, v, &error) && !error.empty();
}

static void testJsonBasics()
{
    const JsonValue v = parseOk(R"( { "a" : [1, 2.5, -3e2, true, false, null, "x"], "b": {"c": "d"} } )");
    CHECK(v.isObject());
    const JsonValue *a = v.find("a");
    CHECK(a && a->isArray() && a->items().size() == 7);
    CHECK(a->items()[0].asNumber() == 1.0 && a->items()[1].asNumber() == 2.5 && a->items()[2].asNumber() == -300.0);
    CHECK(a->items()[3].asBool() && !a->items()[4].asBool(true) && a->items()[5].isNull());
    CHECK(a->items()[6].asString() == "x");
    CHECK(v.find("b")->find("c")->asString() == "d");
    CHECK(v.find("zzz") == nullptr);
    CHECK(JsonValue::number(1).find("a") == nullptr);          // not an object
    CHECK(JsonValue::number(1).asString().empty());            // lenient accessors
    CHECK(JsonValue::string("s").asNumber(7) == 7);

    // Building, replacing, ordering, compact output.
    JsonValue o = JsonValue::object();
    o.set("z", JsonValue::number(1)).set("a", JsonValue::string("x"));
    o.set("z", JsonValue::number(2));
    CHECK(o.dump() == R"({"z":2,"a":"x"})");
    JsonValue arr = JsonValue::array();
    arr.push(JsonValue::boolean(true)).push(JsonValue());
    CHECK(arr.dump() == "[true,null]");

    // Duplicate keys: the last one wins.
    CHECK(parseOk(R"({"k":1,"k":2})").find("k")->asNumber() == 2.0);
}

static void testJsonStringsAndUnicode()
{
    CHECK(parseOk(R"("a\"b\\c\/d\b\f\n\r\t")").asString() == "a\"b\\c/d\b\f\n\r\t");
    CHECK(parseOk(R"("\u00e4\u20ac")").asString() == "\xc3\xa4\xe2\x82\xac");       // ä €
    CHECK(parseOk(R"("\ud83d\ude00")").asString() == "\xf0\x9f\x98\x80");            // a surrogate pair: U+1F600
    CHECK(parseOk("\"\xc3\xa4\"").asString() == "\xc3\xa4");                         // raw UTF-8 passes through

    // The serializer never emits a raw control character or newline.
    const std::string nasty = std::string("line1\nline2\t\"q\" back\\slash \x01 end \xc3\xa4");
    const std::string dumped = JsonValue::string(nasty).dump();
    CHECK(dumped.find('\n') == std::string::npos && dumped.find('\t') == std::string::npos);
    CHECK(dumped.find('\x01') == std::string::npos);
    CHECK(dumped.find("\\u0001") != std::string::npos);
    CHECK(parseOk(dumped).asString() == nasty);
}

static void testJsonNumbers()
{
    CHECK(JsonValue::number(42).dump() == "42");
    CHECK(JsonValue::number(-7).dump() == "-7");
    CHECK(JsonValue::number(0.5).dump() == "0.5");
    CHECK(parseOk(JsonValue::number(0.1).dump()).asNumber() == 0.1);       // %.17g round-trips
    CHECK(parseOk("1e3").asNumber() == 1000.0 && parseOk("-0.5E-1").asNumber() == -0.05);
    CHECK(parseOk("0").asNumber() == 0.0);
    CHECK(JsonValue::number(1e300).dump().find("e+") != std::string::npos);
}

static void testJsonErrors()
{
    CHECK(parseFails(""));
    CHECK(parseFails("{"));
    CHECK(parseFails(R"({"a":1,})"));          // trailing comma
    CHECK(parseFails("[1,2,]"));
    CHECK(parseFails(R"({"a" 1})"));
    CHECK(parseFails(R"({a:1})"));
    CHECK(parseFails(R"("unterminated)"));
    CHECK(parseFails(R"("bad \x escape")"));
    CHECK(parseFails("\"raw\x01control\""));
    CHECK(parseFails("01"));                    // leading zero
    CHECK(parseFails("1."));
    CHECK(parseFails("-"));
    CHECK(parseFails(".5"));
    CHECK(parseFails("tru"));
    CHECK(parseFails("[1] x"));                 // trailing garbage
    CHECK(parseFails(R"("\ud83d")"));           // lone high surrogate
    CHECK(parseFails(R"("\ude00")"));           // lone low surrogate
    CHECK(parseFails(R"("\u12")"));
    CHECK(parseFails(R"("\u12zz")"));

    JsonValue v;
    std::string error;
    CHECK(!JsonValue::parse("[1, ?]", v, &error) && contains(error, "offset"));

    // Nesting: 64 levels are fine, 65 are refused (no stack overflow on hostile input).
    CHECK(!parseFails(std::string(64, '[') + std::string(64, ']')));
    CHECK(parseFails(std::string(65, '[') + std::string(65, ']')));
    CHECK(parseFails(std::string(100000, '[')));
}

static void testJsonLeavesTheOutputAloneOnFailure()
{
    JsonValue v = JsonValue::string("keep me");
    CHECK(!JsonValue::parse("{", v));
    CHECK(v.asString() == "keep me");
}

// ===========================================================================
// Protocol messages
// ===========================================================================

static ProtocolMessage roundTrip(const ProtocolMessage &m)
{
    const std::string line = encodeMessage(m);
    CHECK(line.find('\n') == std::string::npos);
    ProtocolMessage back;
    std::string error;
    const bool ok = decodeMessage(line, back, &error);
    CHECK(ok);
    if (!ok)
        std::fprintf(stderr, "  decode error for %s: %s\n", line.c_str(), error.c_str());
    CHECK(back.type == m.type);
    return back;
}

static void testProtocolRoundTrips()
{
    using T = ProtocolMessage::Type;
    {
        ProtocolMessage m;
        m.type = T::Hello;
        m.name = "elevator-agent \xc3\xa4";
        m.snapshot = true;
        m.crash = false;
        m.restartMs = 2500;
        const ProtocolMessage b = roundTrip(m);
        CHECK(b.name == m.name && b.snapshot && !b.crash && b.restartMs == 2500 && b.protocol == kProtocolVersion);
    }
    {
        ProtocolMessage m;
        m.type = T::StartRun;
        m.run = 7;
        m.initial = "start";
        m.quiescenceMs = 1500;
        const ProtocolMessage b = roundTrip(m);
        CHECK(b.run == 7 && b.initial == "start" && b.quiescenceMs == 1500);
    }
    {
        ProtocolMessage m;
        m.type = T::RunStarted;
        m.ok = false;
        m.error = "no such marking: \"x\"";
        const ProtocolMessage b = roundTrip(m);
        CHECK(!b.ok && b.error == m.error);
    }
    {
        ProtocolMessage m;
        m.type = T::Event;
        m.run = 3;
        m.kind = EventKind::Observed;
        m.label = "door.opened";
        m.timestampMs = 1234567;
        const ProtocolMessage b = roundTrip(m);
        CHECK(b.run == 3 && b.kind == EventKind::Observed && b.label == "door.opened" && b.timestampMs == 1234567);
    }
    {
        ProtocolMessage m;
        m.type = T::Event;
        m.kind = EventKind::Quiescent;  // no label needed
        m.timestampMs = 99;
        const ProtocolMessage b = roundTrip(m);
        CHECK(b.kind == EventKind::Quiescent && b.label.empty() && b.timestampMs == 99 && b.run == 0);
    }
    {
        ProtocolMessage m;
        m.type = T::Stimulate;
        m.label = "door.open";
        CHECK(roundTrip(m).label == "door.open");
    }
    {
        ProtocolMessage m;
        m.type = T::PostMortem;
        m.id = 12;
        m.mode = DumpMode::Crash;
        const ProtocolMessage b = roundTrip(m);
        CHECK(b.id == 12 && b.mode == DumpMode::Crash);
    }
    {
        ProtocolMessage m;
        m.type = T::PostMortemResult;
        m.id = 12;
        m.ok = true;
        m.files = {"/tmp/core.1", "/tmp/stack.txt"};
        m.stack = "#0 jam()\n#1 main() \"quoted\" \xc3\xa4";
        const ProtocolMessage b = roundTrip(m);
        CHECK(b.id == 12 && b.ok && b.files == m.files && b.stack == m.stack);
    }
    {
        ProtocolMessage m;
        m.type = T::Fault;
        m.error = "the SUT refused the input";
        CHECK(roundTrip(m).error == m.error);
    }
    {
        ProtocolMessage m;
        m.type = T::StopRun;
        m.reason = "aborted";
        CHECK(roundTrip(m).reason == "aborted");
        m.reason.clear();
        CHECK(roundTrip(m).reason == "finished");  // the default
    }
    // DeepSeek V4.1 Flash: the synchronized-event fields (doku section 19).
    {
        ProtocolMessage m;
        m.type = T::Hello;
        m.name = "sync-agent";
        m.syncCapable = true;
        const ProtocolMessage b = roundTrip(m);
        CHECK(b.syncCapable && b.name == "sync-agent");
    }
    {
        ProtocolMessage m;
        m.type = T::StartRun;
        m.run = 4;
        m.initial = "start";
        m.syncLabels = {"out", "tick"};
        const ProtocolMessage b = roundTrip(m);
        CHECK(b.run == 4 && b.syncLabels == m.syncLabels);
    }
    {
        ProtocolMessage m;
        m.type = T::Event;
        m.run = 4;
        m.kind = EventKind::Observed;
        m.label = "out";
        m.seq = 9;
        m.syncEvent = true;
        const ProtocolMessage b = roundTrip(m);
        CHECK(b.run == 4 && b.seq == 9 && b.syncEvent && b.label == "out");
    }
    {
        ProtocolMessage m;
        m.type = T::Ack;
        m.run = 4;
        m.seq = 9;
        const ProtocolMessage b = roundTrip(m);
        CHECK(b.run == 4 && b.seq == 9);
    }
    {
        ProtocolMessage m;
        m.type = T::Shutdown;
        roundTrip(m);
    }
}

static bool decodeFails(const std::string &line, const char *errorPart = nullptr)
{
    ProtocolMessage m;
    std::string error;
    if (decodeMessage(line, m, &error))
        return false;
    return !errorPart || contains(error, errorPart);
}

static void testProtocolRejectsBadMessages()
{
    CHECK(decodeFails("not json", "malformed"));
    CHECK(decodeFails("[1,2]", "object"));
    CHECK(decodeFails("{}", "type"));
    CHECK(decodeFails(R"({"type":"warp_drive"})", "unknown message type"));
    CHECK(decodeFails(R"({"type":"event","kind":"observed"})", "label"));
    CHECK(decodeFails(R"({"type":"event","kind":"exploded","label":"x"})", "unknown event kind"));
    CHECK(decodeFails(R"({"type":"stimulate"})", "label"));
    CHECK(decodeFails(R"({"type":"post_mortem","id":1,"mode":"sideways"})", "unknown post-mortem mode"));
    CHECK(decodeFails(R"({"type":"post_mortem","mode":"crash"})", "id"));
    CHECK(decodeFails(R"({"type":"start_run"})", "initial"));
    CHECK(decodeFails(R"({"type":"run_started"})", "ok"));
    CHECK(decodeFails(R"({"type":"hello","protocol":2})", "unsupported protocol version"));
    CHECK(decodeFails(R"({"type":"hello"})", "protocol"));
    CHECK(decodeFails(R"({"type":"fault"})", "message"));
}

static void testProtocolIgnoresUnknownFields()
{
    ProtocolMessage m;
    std::string error;
    CHECK(decodeMessage(R"({"type":"event","kind":"observed","label":"x","t":5,"future":{"a":[1]},"run":2})", m, &error));
    CHECK(m.label == "x" && m.timestampMs == 5 && m.run == 2);
    CHECK(decodeMessage(R"({"type":"hello","protocol":1,"extra":true})", m, &error));
}

// ===========================================================================
// RemotePlugin over a loopback
// ===========================================================================

// One end of an in-memory connection. Lines are delivered synchronously on the sender's thread.
class LoopbackEnd : public ILineTransport {
public:
    void connect(LoopbackEnd *peer) { peer_ = peer; }
    void cut() { cut_ = true; }

    bool sendLine(const std::string &line) override
    {
        if (cut_ || !peer_)
            return false;
        peer_->deliver(line);
        return true;
    }
    void setLineHandler(std::function<void(const std::string &)> handler) override
    {
        std::lock_guard<std::mutex> lock(m_);
        handler_ = std::move(handler);
    }

private:
    void deliver(const std::string &line)
    {
        std::function<void(const std::string &)> h;
        {
            std::lock_guard<std::mutex> lock(m_);
            h = handler_;
        }
        if (h)
            h(line);
    }

    LoopbackEnd *peer_ = nullptr;
    std::atomic<bool> cut_{false};
    std::mutex m_;
    std::function<void(const std::string &)> handler_;
};

static SessionConfig quick()
{
    SessionConfig cfg;
    cfg.poll = std::chrono::milliseconds(2);
    cfg.plugin.quiescenceTimeoutMs = 30;
    cfg.idleTimeout = std::chrono::milliseconds(3000);
    cfg.stimulusAckTimeout = std::chrono::milliseconds(3000);
    cfg.maxRuns = 3;
    return cfg;
}

static Net makeThreeBranchNet()
{
    NetBuilder b;
    const auto s0 = b.place("s0");
    std::vector<PlaceIndex> mid;
    for (const char *name : {"s1", "s2", "s3"})
        mid.push_back(b.place(name));
    const char *inputs[] = {"a", "b", "c"};
    const char *outputs[] = {"oa", "ob", "oc"};
    for (std::size_t i = 0; i < 3; ++i) {
        const auto in = b.transition(inputs[i], TransitionKind::Controllable, inputs[i]);
        const auto out = b.transition(outputs[i], TransitionKind::Observable, outputs[i]);
        b.input(s0, in).output(in, mid[i]);
        b.input(mid[i], out).output(out, s0);
    }
    b.initial("start", {{s0, 1}});
    b.targetMarking("idle", {{s0, 1}});
    return b.build();
}

// Connects a RemotePlugin to an AgentServer wrapping a simulated SUT with `options` on `net`.
struct Rig {
    LoopbackEnd trackerEnd, agentEnd;
    FakeSut sut;
    std::unique_ptr<RemotePlugin> plugin;
    std::unique_ptr<AgentServer> agent;

    Rig(const Net &net, FakeSutOptions options, RemoteTimeouts timeouts = {}) : sut(net, options)
    {
        trackerEnd.connect(&agentEnd);
        agentEnd.connect(&trackerEnd);
        plugin = std::make_unique<RemotePlugin>(trackerEnd, timeouts);
        agent = std::make_unique<AgentServer>(sut, agentEnd, "fake-agent");
        agent->start();
    }
    ~Rig()
    {
        plugin->shutdown();
        agent.reset();
    }
};

// DeepSeek V4.1 Flash: a synchronized observable step (doku section 19).
static Net makeSyncNet()
{
    NetBuilder b;
    const auto s0 = b.place("s0");
    const auto s1 = b.place("s1");
    const auto s2 = b.place("s2");
    const auto go = b.transition("go", TransitionKind::Controllable, "go");
    const auto out = b.transition("out", TransitionKind::Observable, "out");
    b.input(s0, go).output(go, s1);
    b.input(s1, out).output(out, s2);
    b.initial("start", {{s0, 1}});
    b.targetMarking("done", {{s2, 1}});
    b.sync(out);
    return b.build();
}

// DeepSeek V4.1 Flash: the ack must be on the wire when runOnce() returns. The
// plug-in waits for its own mapping (RemotePlugin::acknowledgeEvent) instead
// of dropping a lookup that the pushing thread had not registered yet; this
// rig delivers synchronously, so the SUT is released before the run ends.
static void testSynchronizedEventsTravelOverTheProtocol()
{
    const Net net = makeSyncNet();
    Rig rig(net, FakeSutOptions{});
    CHECK(rig.plugin->waitForHello(std::chrono::milliseconds(1000)));
    CHECK(rig.plugin->capabilities().eventAcks);  // hello advertised it

    SessionConfig cfg = quick();
    cfg.syncEvents = true;
    cfg.maxRuns = 1;
    Session session(net, cfg);
    const RunReport r = session.runOnce(*rig.plugin);
    CHECK(r.outcome == RunOutcome::Completed);
    CHECK(rig.sut.acknowledgements() == 1);  // the wire ack reached the SUT
    CHECK(!rig.sut.holding());
}

static void testAnAgentWithoutTheAckCapabilityIsNoted()
{
    const Net net = makeSyncNet();
    FakeSutOptions opt;
    opt.eventAcks = false;  // hello will say "sync": false
    Rig rig(net, opt);
    CHECK(rig.plugin->waitForHello(std::chrono::milliseconds(1000)));
    CHECK(!rig.plugin->capabilities().eventAcks);

    SessionConfig cfg = quick();
    cfg.syncEvents = true;
    cfg.maxRuns = 1;
    Session session(net, cfg);
    const RunReport r = session.runOnce(*rig.plugin);
    CHECK(r.outcome == RunOutcome::Completed);
    CHECK(rig.sut.acknowledgements() == 0);
    bool noted = false;
    for (const std::string &n : r.notes)
        if (contains(n, "cannot acknowledge events"))
            noted = true;
    CHECK(noted);
}

static void testHelloCarriesTheCapabilities()
{
    const Net net = makeThreeBranchNet();
    FakeSutOptions opt;
    opt.snapshot = true;
    opt.crash = false;
    Rig rig(net, opt);
    CHECK(rig.plugin->waitForHello(std::chrono::milliseconds(1000)));
    CHECK(rig.plugin->agentName() == "fake-agent");
    const PluginCapabilities caps = rig.plugin->capabilities();
    CHECK(caps.snapshot && !caps.crash && caps.restartMs == 100);
}

static void testACampaignOverTheProtocolReachesFullCoverage()
{
    const Net net = makeThreeBranchNet();
    Rig rig(net, FakeSutOptions{});
    CHECK(rig.plugin->waitForHello(std::chrono::milliseconds(1000)));

    std::vector<PersistedRun> saved;
    SessionConfig cfg = quick();
    cfg.runSink = [&saved](const PersistedRun &r) { saved.push_back(r); };
    Session session(net, cfg);

    const CampaignReport rep = session.runCampaign(*rig.plugin);
    CHECK(rep.complete && rep.runs.size() == 1);
    CHECK(rep.runs[0].stimuli == 3 && rep.runs[0].events == 6);
    CHECK(rep.summary.transitionsCovered == 6);
    CHECK(rig.sut.stimuliReceived().size() == 3);
    CHECK(saved.size() == 1 && saved[0].events.size() == 6);
}

static void testAPostMortemTravelsOverTheProtocol()
{
    ErrorOptions jammed;
    jammed.crashDump = DumpMode::Snapshot;
    const Net net = testmodels::makeElevatorNet(jammed);
    FakeSutOptions opt;
    opt.weight = {{"obstruct", 0}, {"clear", 0}, {"closed_ev", 0}};
    Rig rig(net, opt);
    CHECK(rig.plugin->waitForHello(std::chrono::milliseconds(1000)));

    std::vector<PersistedRun> saved;
    SessionConfig cfg = quick();
    cfg.maxRuns = 1;
    cfg.runSink = [&saved](const PersistedRun &r) { saved.push_back(r); };
    Session session(net, cfg);

    const RunReport r = session.runOnce(*rig.plugin);
    CHECK(r.outcome == RunOutcome::ErrorReached && contains(r.detail, "jammed"));
    CHECK(r.dumps == 1);
    CHECK(saved.size() == 1 && saved[0].dumps.size() == 1);
    CHECK(saved[0].dumps[0].files == std::vector<std::string>{"dump-1.snapshot"});
    CHECK(saved[0].dumps[0].stackText == "frame #0 simulated_sut_main()");
}

static void testAnAgentThatCannotStartTheSutIsReported()
{
    const Net net = makeThreeBranchNet();
    FakeSutOptions opt;
    opt.failStart = true;
    Rig rig(net, opt);
    CHECK(rig.plugin->waitForHello(std::chrono::milliseconds(1000)));

    EventQueue q;
    std::string err;
    PluginRunOptions po;
    CHECK(!rig.plugin->startRun("start", po, q, &err));
    CHECK(contains(err, "simulated start failure"));

    Session session(net, quick());
    const RunReport r = session.runOnce(*rig.plugin);
    CHECK(r.outcome == RunOutcome::Aborted && contains(r.detail, "simulated start failure"));
}

static void testASilentAgentTimesOut()
{
    LoopbackEnd trackerEnd, agentEnd;  // nobody listens on the agent end
    trackerEnd.connect(&agentEnd);
    agentEnd.connect(&trackerEnd);
    RemoteTimeouts t;
    t.runStart = std::chrono::milliseconds(60);
    t.postMortem = std::chrono::milliseconds(60);
    RemotePlugin plugin(trackerEnd, t);

    CHECK(!plugin.waitForHello(std::chrono::milliseconds(30)));
    EventQueue q;
    std::string err;
    CHECK(!plugin.startRun("start", PluginRunOptions{}, q, &err) && contains(err, "did not answer"));
    const PostMortem pm = plugin.postMortem(DumpMode::Snapshot);
    CHECK(!pm.ok && contains(pm.error, "in time"));
}

static void testALostConnectionIsReported()
{
    const Net net = makeThreeBranchNet();
    Rig rig(net, FakeSutOptions{});
    CHECK(rig.plugin->waitForHello(std::chrono::milliseconds(1000)));
    EventQueue q;
    std::string err;
    CHECK(rig.plugin->startRun("start", PluginRunOptions{}, q, &err));

    rig.trackerEnd.cut();
    CHECK(!rig.plugin->stimulate("a", &err) && contains(err, "connection"));
    CHECK(!rig.plugin->postMortem(DumpMode::Snapshot).ok);
    rig.plugin->stopRun(StopReason::Aborted);
}

static void testFaultsAndGarbageFromTheAgent()
{
    {
        const Net net = makeThreeBranchNet();
        Rig rig(net, FakeSutOptions{});
        CHECK(rig.plugin->pollFault().empty());
        ProtocolMessage m;
        m.type = ProtocolMessage::Type::Fault;
        m.error = "the SUT refused the input";
        rig.agentEnd.sendLine(encodeMessage(m));
        CHECK(rig.plugin->pollFault() == "the SUT refused the input");

        Session session(net, quick());
        const RunReport r = session.runOnce(*rig.plugin);
        CHECK(r.outcome == RunOutcome::Aborted && contains(r.detail, "refused the input"));
    }
    {
        const Net net = makeThreeBranchNet();
        Rig rig(net, FakeSutOptions{});
        rig.agentEnd.sendLine("this is not json");
        CHECK(contains(rig.plugin->pollFault(), "bad message from the agent"));
    }
}

static void testTheQueueMayBeDestroyedRightAfterStopRun()
{
    // The agent keeps forwarding events from its own thread. After stopRun() returns
    // the queue is freed at once; a push still in progress would be a use-after-free
    // (caught by the address and thread sanitizers).
    const Net net = makeThreeBranchNet();
    FakeSutOptions opt;
    opt.weight = {{"oa", 1}};
    Rig rig(net, opt);
    CHECK(rig.plugin->waitForHello(std::chrono::milliseconds(1000)));

    for (int i = 0; i < 40; ++i) {
        auto queue = std::make_unique<EventQueue>();
        std::string err;
        CHECK(rig.plugin->startRun("start", PluginRunOptions{}, *queue, &err));
        rig.plugin->stimulate("a", nullptr);  // keeps the agent busy producing events
        std::this_thread::sleep_for(std::chrono::microseconds(300 * (i % 5)));
        rig.plugin->stopRun(StopReason::Finished);
        queue.reset();  // freed immediately
    }
}

static void testLateEventsOfAnEarlierRunAreDropped()
{
    const Net net = makeThreeBranchNet();
    LoopbackEnd trackerEnd, agentEnd;
    trackerEnd.connect(&agentEnd);
    agentEnd.connect(&trackerEnd);
    RemotePlugin plugin(trackerEnd);

    // A scripted agent: answers start_run, then we inject events by hand.
    std::int64_t runNumber = 0;
    agentEnd.setLineHandler([&](const std::string &line) {
        ProtocolMessage m;
        if (!decodeMessage(line, m) || m.type != ProtocolMessage::Type::StartRun)
            return;
        runNumber = m.run;
        ProtocolMessage reply;
        reply.type = ProtocolMessage::Type::RunStarted;
        reply.ok = true;
        agentEnd.sendLine(encodeMessage(reply));
    });
    const auto event = [&](std::int64_t run, const char *label) {
        ProtocolMessage m;
        m.type = ProtocolMessage::Type::Event;
        m.run = run;
        m.kind = EventKind::Observed;
        m.label = label;
        agentEnd.sendLine(encodeMessage(m));
    };

    EventQueue q;
    std::string err;
    CHECK(plugin.startRun("start", PluginRunOptions{}, q, &err));
    const std::int64_t first = runNumber;
    event(first, "one");
    plugin.stopRun(StopReason::Finished);
    event(first, "after-stop");  // the run is over: dropped
    CHECK(q.size() == 1);

    q.reset();
    CHECK(plugin.startRun("start", PluginRunOptions{}, q, &err));
    CHECK(runNumber == first + 1);
    event(first, "stale");          // a late event of the previous run: dropped
    event(runNumber, "current");    // kept
    ProtocolMessage unnumbered;     // an agent that does not echo the run number: kept
    unnumbered.type = ProtocolMessage::Type::Event;
    unnumbered.kind = EventKind::Observed;
    unnumbered.label = "unnumbered";
    agentEnd.sendLine(encodeMessage(unnumbered));

    CHECK(q.size() == 2);
    SequencedEvent e;
    CHECK(q.tryPop(e) && e.event.label == "current");
    CHECK(q.tryPop(e) && e.event.label == "unnumbered");
    (void)net;
}

// ---------------------------------------------------------------------------

int main()
{
    const std::vector<std::pair<const char *, std::function<void()>>> tests = {
        {"jsonBasics", testJsonBasics},
        {"jsonStringsAndUnicode", testJsonStringsAndUnicode},
        {"jsonNumbers", testJsonNumbers},
        {"jsonErrors", testJsonErrors},
        {"jsonLeavesTheOutputAloneOnFailure", testJsonLeavesTheOutputAloneOnFailure},
        {"protocolRoundTrips", testProtocolRoundTrips},
        {"protocolRejectsBadMessages", testProtocolRejectsBadMessages},
        {"protocolIgnoresUnknownFields", testProtocolIgnoresUnknownFields},
        {"helloCarriesTheCapabilities", testHelloCarriesTheCapabilities},
        {"aCampaignOverTheProtocolReachesFullCoverage", testACampaignOverTheProtocolReachesFullCoverage},
        {"aPostMortemTravelsOverTheProtocol", testAPostMortemTravelsOverTheProtocol},
        {"anAgentThatCannotStartTheSutIsReported", testAnAgentThatCannotStartTheSutIsReported},
        {"aSilentAgentTimesOut", testASilentAgentTimesOut},
        {"aLostConnectionIsReported", testALostConnectionIsReported},
        {"faultsAndGarbageFromTheAgent", testFaultsAndGarbageFromTheAgent},
        {"theQueueMayBeDestroyedRightAfterStopRun", testTheQueueMayBeDestroyedRightAfterStopRun},
        {"lateEventsOfAnEarlierRunAreDropped", testLateEventsOfAnEarlierRunAreDropped},
        {"synchronizedEventsTravelOverTheProtocol", testSynchronizedEventsTravelOverTheProtocol},  // DeepSeek V4.1 Flash
        {"anAgentWithoutTheAckCapabilityIsNoted", testAnAgentWithoutTheAckCapabilityIsNoted},  // DeepSeek V4.1 Flash
    };
    for (const auto &t : tests) {
        const int before = g_failures;
        t.second();
        std::printf("%s %s\n", g_failures == before ? "PASS" : "FAIL", t.first);
    }
    std::printf("%d failure(s)\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
