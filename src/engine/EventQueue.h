// Asynchronous hand-over of SUT events from the plug-in side to the tracker.
//
// WHY ASYNCHRONOUS. A fast SUT can emit several events while the tracker is
// still working on the previous one (expanding the graph, crediting coverage,
// computing a suggestion). The plug-in must never wait for the tracker, so it
// pushes events into this queue and carries on; the tracker drains the queue
// on its own thread.
//
// ORDER. The order of the events IS the order of the queue: push() assigns the
// sequence number under the lock, so the sequence defines the one total order
// in which things happened as far as the tracker is concerned. Producers must
// push at the moment of capture, and stamp the event's timestamp at capture
// too (not when the tracker gets to it): the run-time livelock timeout and
// replay depend on capture times, and the tracker keeps no clock of its own.
//
// RACES ARE NOT A PROBLEM, THEY ARE THE MODEL. If the SUT emits an event
// before the plug-in has sent a suggested input, the queue holds the event
// ahead of the "Stimulated" report; the tracker applies them in that order and
// the stimulus comes out Preempted if the event took its place. Nothing is
// reordered or dropped to hide this.
//
// BACKPRESSURE. The queue is bounded, and it never drops an event silently: a
// lost event would silently corrupt the tracker's belief. When it is full,
// push() refuses (Full) and the queue is marked overflowed. The producer may
// retry (blocking its own thread) or give up; if events were lost, the run must
// be ended as Aborted. Size the queue generously: tracking an event is cheap.
//
// THREADS. Any number of producers; ONE consumer (tryPop / pumpEvents).
// Everything else in the engine (tracker, coverage, suggester, ants) stays
// single-threaded and belongs to the consumer's thread.
#pragma once

#include "engine/BeliefTracker.h"
#include "engine/Events.h"

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>

namespace pn {

struct SequencedEvent {
    std::uint64_t seq = 0;  // 0, 1, 2, ... in queue order
    SutEvent event;
};

class EventQueue {
public:
    enum class PushResult { Ok, Full, Closed };

    explicit EventQueue(std::size_t capacity = 65536);

    EventQueue(const EventQueue &) = delete;
    EventQueue &operator=(const EventQueue &) = delete;

    // Producer side. Thread-safe. `seq`, if given, receives the assigned number.
    PushResult push(SutEvent event, std::uint64_t *seq = nullptr);

    // Consumer side. Non-blocking.
    bool tryPop(SequencedEvent &out);

    // Blocks until an event is available (true), the queue is closed and empty
    // or the timeout elapsed (false).
    bool waitForEvents(std::chrono::milliseconds timeout);

    // No more pushes are accepted; the consumer can still drain what is left.
    void close();
    bool closed() const;

    // True once a push was refused because the queue was full: the event
    // stream has a gap and the run cannot be trusted.
    bool overflowed() const;

    std::size_t size() const;
    std::uint64_t pushed() const;  // events accepted so far

    // Empties the queue and clears the closed/overflow flags and the sequence,
    // for the next run. Call it only while no producer is pushing.
    void reset();

private:
    mutable std::mutex mutex_;
    std::condition_variable available_;
    std::deque<SequencedEvent> queue_;
    std::size_t capacity_;
    std::uint64_t nextSeq_ = 0;
    bool closed_ = false;
    bool overflowed_ = false;
};

struct PumpResult {
    std::size_t processed = 0;
    std::size_t stimulated = 0;  // how many of them were Stimulated events (acknowledged inputs)
    // The last processed event ended the run: Deviation, Deadlock or Livelock.
    // The events behind it stay in the queue.
    bool runEnded = false;
    StepResult last;
    std::uint64_t lastSeq = 0;

    // GLM-5.3-Flash: what the last processed event was, so a caller that
    // pumps one event at a time can bookkeep per event (the session's
    // offered/taken statistics). Meaningful when processed > 0.
    EventKind lastKind = EventKind::Observed;
    std::string lastLabel;
};

// Feeds queued events to the tracker, in order, until the queue is empty,
// `max` events were processed, or an event ends the run. Call it on the
// tracker's thread. A Preempted stimulus does not end a run.
PumpResult pumpEvents(EventQueue &queue, BeliefTracker &tracker,
                      std::size_t max = static_cast<std::size_t>(-1));

}  // namespace pn
