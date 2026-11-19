#include "engine/EventQueue.h"

#include <utility>

namespace pn {

EventQueue::EventQueue(std::size_t capacity) : capacity_(capacity == 0 ? 1 : capacity) {}

EventQueue::PushResult EventQueue::push(SutEvent event, std::uint64_t *seq, bool needsAck)
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (closed_)
            return PushResult::Closed;
        if (queue_.size() >= capacity_) {
            overflowed_ = true;
            return PushResult::Full;
        }
        SequencedEvent se;
        se.seq = nextSeq_++;
        se.event = std::move(event);
        se.needsAck = needsAck;  // DeepSeek V4.1 Flash: the producer holds the SUT
        if (seq)
            *seq = se.seq;
        queue_.push_back(std::move(se));
    }
    available_.notify_one();
    return PushResult::Ok;
}

bool EventQueue::tryPop(SequencedEvent &out)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (queue_.empty())
        return false;
    out = std::move(queue_.front());
    queue_.pop_front();
    return true;
}

bool EventQueue::waitForEvents(std::chrono::milliseconds timeout)
{
    std::unique_lock<std::mutex> lock(mutex_);
    available_.wait_for(lock, timeout, [this] { return !queue_.empty() || closed_; });
    return !queue_.empty();
}

void EventQueue::close()
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        closed_ = true;
    }
    available_.notify_all();
}

bool EventQueue::closed() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return closed_;
}

bool EventQueue::overflowed() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return overflowed_;
}

std::size_t EventQueue::size() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return queue_.size();
}

std::uint64_t EventQueue::pushed() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return nextSeq_;
}

void EventQueue::reset()
{
    std::lock_guard<std::mutex> lock(mutex_);
    queue_.clear();
    nextSeq_ = 0;
    closed_ = false;
    overflowed_ = false;
}

PumpResult pumpEvents(EventQueue &queue, BeliefTracker &tracker, std::size_t max)
{
    PumpResult result;
    SequencedEvent e;
    while (result.processed < max && queue.tryPop(e)) {
        result.last = tracker.onEvent(e.event);
        result.lastSeq = e.seq;
        result.lastKind = e.event.kind;    // GLM-5.3-Flash
        result.lastLabel = e.event.label;  // GLM-5.3-Flash
        // DeepSeek V4.1 Flash: one event at a time and the producer is waiting: the
        // caller must react and acknowledge before anything else is pumped.
        result.lastNeedsAck = e.needsAck;
        result.lastAckSeq = e.seq;
        ++result.processed;
        if (e.event.kind == EventKind::Stimulated)
            ++result.stimulated;
        const StepStatus st = result.last.status;
        if (st == StepStatus::Deviation || st == StepStatus::Deadlock || st == StepStatus::Livelock) {
            result.runEnded = true;
            break;
        }
    }
    return result;
}

}  // namespace pn
