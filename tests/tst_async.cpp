// Tests for the asynchronous hand-over of events: ordering, multiple
// producers, bounded capacity, closing, and the pump that feeds the tracker.
#include "engine/BeliefTracker.h"
#include "engine/CoverageDb.h"
#include "engine/EventQueue.h"
#include "engine/ReachabilityGraph.h"
#include "model/NetBuilder.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <functional>
#include <string>
#include <thread>
#include <vector>

using namespace pn;

static std::atomic<int> g_failures{0};

#define CHECK(cond)                                                                       \
    do {                                                                                  \
        if (!(cond)) {                                                                    \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                                                 \
        }                                                                                 \
    } while (0)

static SutEvent ev(EventKind kind, const std::string &label, std::int64_t ms = 0)
{
    return SutEvent{kind, label, ms};
}

// a --ping--> b --pong--> a (both observable), and a controllable self-loop "kick" in a.
static Net makePingPongNet()
{
    NetBuilder b;
    const auto a = b.place("a");
    const auto pb = b.place("b");
    const auto ping = b.transition("ping", TransitionKind::Observable, "ping");
    const auto pong = b.transition("pong", TransitionKind::Observable, "pong");
    b.input(a, ping).output(ping, pb);
    b.input(pb, pong).output(pong, a);
    b.initial("s", {{a, 1}});
    b.targetMarking("idle", {{a, 1}});
    return b.build();
}

// ---------------------------------------------------------------------------

static void testFifoAndSequenceNumbers()
{
    EventQueue q(16);
    std::uint64_t seq = 99;
    CHECK(q.push(ev(EventKind::Observed, "a"), &seq) == EventQueue::PushResult::Ok && seq == 0);
    CHECK(q.push(ev(EventKind::Stimulated, "b"), &seq) == EventQueue::PushResult::Ok && seq == 1);
    CHECK(q.push(ev(EventKind::Quiescent, ""), &seq) == EventQueue::PushResult::Ok && seq == 2);
    CHECK(q.size() == 3 && q.pushed() == 3);

    SequencedEvent e;
    CHECK(q.tryPop(e) && e.seq == 0 && e.event.label == "a");
    CHECK(q.tryPop(e) && e.seq == 1 && e.event.kind == EventKind::Stimulated);
    CHECK(q.tryPop(e) && e.seq == 2 && e.event.kind == EventKind::Quiescent);
    CHECK(!q.tryPop(e));
}

static void testTimestampsTravelWithTheEvent()
{
    EventQueue q;
    q.push(ev(EventKind::Observed, "x", 12345));
    SequencedEvent e;
    CHECK(q.tryPop(e) && e.event.timestampMs == 12345);
}

static void testBoundedQueueNeverDropsSilently()
{
    EventQueue q(3);
    for (int i = 0; i < 3; ++i)
        CHECK(q.push(ev(EventKind::Observed, "e")) == EventQueue::PushResult::Ok);
    CHECK(!q.overflowed());
    CHECK(q.push(ev(EventKind::Observed, "e")) == EventQueue::PushResult::Full);
    CHECK(q.overflowed());  // the gap is recorded
    CHECK(q.size() == 3);   // and nothing queued was lost

    SequencedEvent e;
    CHECK(q.tryPop(e));
    CHECK(q.push(ev(EventKind::Observed, "e")) == EventQueue::PushResult::Ok);  // room again
    CHECK(q.overflowed());                                                      // sticky

    q.reset();
    CHECK(!q.overflowed() && q.size() == 0 && q.pushed() == 0);
    std::uint64_t seq = 7;
    CHECK(q.push(ev(EventKind::Observed, "e"), &seq) == EventQueue::PushResult::Ok && seq == 0);
}

static void testCloseWakesTheConsumerAndRefusesProducers()
{
    EventQueue q;
    std::atomic<bool> woke{false};
    std::atomic<bool> gotEvent{true};
    std::thread consumer([&] {
        gotEvent = q.waitForEvents(std::chrono::milliseconds(5000));
        woke = true;
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    CHECK(!woke);
    q.close();
    consumer.join();
    CHECK(woke && !gotEvent);  // woken by close, not by the 5 s timeout, and with nothing to read
    CHECK(q.closed());
    CHECK(q.push(ev(EventKind::Observed, "late")) == EventQueue::PushResult::Closed);

    // What was queued before closing can still be drained.
    EventQueue q2;
    q2.push(ev(EventKind::Observed, "kept"));
    q2.close();
    CHECK(q2.waitForEvents(std::chrono::milliseconds(10)));
    SequencedEvent e;
    CHECK(q2.tryPop(e) && e.event.label == "kept");
    CHECK(!q2.waitForEvents(std::chrono::milliseconds(10)));
}

static void testWaitWakesOnPushFromAnotherThread()
{
    EventQueue q;
    std::thread producer([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        q.push(ev(EventKind::Observed, "hello"));
    });
    const auto start = std::chrono::steady_clock::now();
    CHECK(q.waitForEvents(std::chrono::milliseconds(5000)));
    const auto waited = std::chrono::steady_clock::now() - start;
    producer.join();
    CHECK(waited < std::chrono::milliseconds(2000));
}

static void testManyProducersKeepASingleOrderAndTheirOwn()
{
    constexpr int kProducers = 4;
    constexpr int kPerProducer = 2000;
    EventQueue q(kProducers * kPerProducer);

    std::vector<std::thread> producers;
    for (int p = 0; p < kProducers; ++p) {
        producers.emplace_back([&q, p] {
            for (int i = 0; i < kPerProducer; ++i)
                q.push(ev(EventKind::Observed, std::to_string(p) + ":" + std::to_string(i)));
        });
    }
    for (std::thread &t : producers)
        t.join();

    CHECK(q.pushed() == static_cast<std::uint64_t>(kProducers * kPerProducer));
    std::vector<int> next(kProducers, 0);
    std::uint64_t expectedSeq = 0;
    SequencedEvent e;
    bool ordered = true;
    while (q.tryPop(e)) {
        ordered = ordered && e.seq == expectedSeq++;
        const std::size_t colon = e.event.label.find(':');
        const int p = std::stoi(e.event.label.substr(0, colon));
        const int i = std::stoi(e.event.label.substr(colon + 1));
        ordered = ordered && i == next[static_cast<std::size_t>(p)]++;  // each producer's own order is kept
    }
    CHECK(ordered);
    CHECK(expectedSeq == static_cast<std::uint64_t>(kProducers * kPerProducer));
}

static void testPumpStopsAtAnEndingEventAndKeepsTheRest()
{
    const Net net = makePingPongNet();
    ReachabilityGraph rg(net);
    BeliefTracker tr(rg);
    tr.beginRun(0);

    EventQueue q;
    q.push(ev(EventKind::Observed, "ping"));
    q.push(ev(EventKind::Observed, "ping"));  // not possible in b: a deviation
    q.push(ev(EventKind::Observed, "pong"));

    const PumpResult r = pumpEvents(q, tr);
    CHECK(r.processed == 2);
    CHECK(r.runEnded && r.last.status == StepStatus::Deviation);
    CHECK(r.lastSeq == 1);
    CHECK(q.size() == 1);  // the event behind the deviation is still queued
    tr.endRun(RunOutcome::Deviation, r.last.message);
}

static void testPumpHonoursMax()
{
    const Net net = makePingPongNet();
    ReachabilityGraph rg(net);
    BeliefTracker tr(rg);
    tr.beginRun(0);
    EventQueue q;
    for (int i = 0; i < 10; ++i)
        q.push(ev(EventKind::Observed, i % 2 == 0 ? "ping" : "pong"));
    const PumpResult r = pumpEvents(q, tr, 4);
    CHECK(r.processed == 4 && !r.runEnded && q.size() == 6);
    tr.endRun(RunOutcome::Completed);
}

static void testAFastSutOutrunsTheTrackerWithoutLosingAnything()
{
    // A producer thread fires 20000 events as fast as it can into a small
    // queue (retrying when full, which is the backpressure) while the tracker
    // consumes on this thread.
    constexpr int kEvents = 20000;
    const Net net = makePingPongNet();
    ReachabilityGraph rg(net);
    CoverageDb db(rg);
    BeliefTracker tr(rg, &db);
    tr.beginRun(0);

    EventQueue q(64);
    std::atomic<int> refused{0};
    std::thread sut([&] {
        for (int i = 0; i < kEvents; ++i) {
            const SutEvent e = ev(EventKind::Observed, i % 2 == 0 ? "ping" : "pong", i);
            while (q.push(e) == EventQueue::PushResult::Full) {
                ++refused;
                std::this_thread::yield();
            }
        }
        q.close();
    });

    std::size_t processed = 0;
    bool ended = false;
    while (!ended && (q.waitForEvents(std::chrono::milliseconds(2000)) || !q.closed())) {
        const PumpResult r = pumpEvents(q, tr);
        processed += r.processed;
        ended = r.runEnded;
        if (r.processed == 0 && q.closed() && q.size() == 0)
            break;
    }
    sut.join();

    CHECK(!ended);
    CHECK(processed == static_cast<std::size_t>(kEvents));  // every event, in order
    CHECK(tr.isDefinite() && tr.possiblyTarget());           // an even number of ping/pong: back in a target marking
    tr.endRun(RunOutcome::Completed);
    CHECK(db.run(0).events.size() == static_cast<std::size_t>(kEvents));
    // Whether the producer was ever held back depends on scheduling, so it is not asserted;
    // what matters is that holding it back (Full + retry) lost nothing.
    (void)refused;
}

static void testARaceBetweenAnOutputAndASuggestedInputIsPreserved()
{
    // s0 offers the input "x" (to s1) and the SUT's own output "o" (to s2),
    // competing for the same token. The suggestion is x, but the fast SUT
    // answers with o first; the plug-in's "I sent x" arrives behind it.
    NetBuilder b;
    const auto s0 = b.place("s0");
    const auto s1 = b.place("s1");
    const auto s2 = b.place("s2");
    const auto x = b.transition("x", TransitionKind::Controllable, "x");
    const auto o = b.transition("o", TransitionKind::Observable, "o");
    b.input(s0, x).output(x, s1);
    b.input(s0, o).output(o, s2);
    b.initial("start", {{s0, 1}});
    const Net net = b.build();

    ReachabilityGraph rg(net);
    BeliefTracker tr(rg);
    tr.beginRun(0);
    CHECK(tr.enabledControllable().size() == 1);  // x looks fine at suggestion time

    EventQueue q;
    q.push(ev(EventKind::Observed, "o"));       // the SUT was faster
    q.push(ev(EventKind::Stimulated, "x"));     // the plug-in's report, in capture order
    const PumpResult r = pumpEvents(q, tr);
    CHECK(r.processed == 2);
    CHECK(r.last.status == StepStatus::Preempted);  // not a deviation: the input simply lost the race
    CHECK(!r.runEnded);
    CHECK(tr.belief().size() == 1 && rg.marking(tr.belief()[0]) == (Marking{0, 0, 1}));  // the SUT went to s2
    tr.endRun(RunOutcome::Completed);
}

// ---------------------------------------------------------------------------

int main()
{
    const std::vector<std::pair<const char *, std::function<void()>>> tests = {
        {"fifoAndSequenceNumbers", testFifoAndSequenceNumbers},
        {"timestampsTravelWithTheEvent", testTimestampsTravelWithTheEvent},
        {"boundedQueueNeverDropsSilently", testBoundedQueueNeverDropsSilently},
        {"closeWakesTheConsumerAndRefusesProducers", testCloseWakesTheConsumerAndRefusesProducers},
        {"waitWakesOnPushFromAnotherThread", testWaitWakesOnPushFromAnotherThread},
        {"manyProducersKeepASingleOrderAndTheirOwn", testManyProducersKeepASingleOrderAndTheirOwn},
        {"pumpStopsAtAnEndingEventAndKeepsTheRest", testPumpStopsAtAnEndingEventAndKeepsTheRest},
        {"pumpHonoursMax", testPumpHonoursMax},
        {"aFastSutOutrunsTheTrackerWithoutLosingAnything", testAFastSutOutrunsTheTrackerWithoutLosingAnything},
        {"aRaceBetweenAnOutputAndASuggestedInputIsPreserved", testARaceBetweenAnOutputAndASuggestedInputIsPreserved},
    };
    for (const auto &t : tests) {
        const int before = g_failures;
        t.second();
        std::printf("%s %s\n", g_failures == before ? "PASS" : "FAIL", t.first);
    }
    std::printf("%d failure(s)\n", g_failures.load());
    return g_failures == 0 ? 0 : 1;
}
