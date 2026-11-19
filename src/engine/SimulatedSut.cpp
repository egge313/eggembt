#include "engine/SimulatedSut.h"

#include <algorithm>  // DeepSeek V4.1 Flash: isSyncLabel

namespace pn {

SimulatedSut::SimulatedSut(const Net &net, SimulatedSutOptions options) : net_(net), opt_(std::move(options)) {}

SimulatedSut::~SimulatedSut()
{
    halt();
}

PluginCapabilities SimulatedSut::capabilities() const
{
    PluginCapabilities caps;  // DeepSeek V4.1 Flash: eventAcks comes from the options
    caps.snapshot = opt_.snapshot;
    caps.crash = opt_.crash;
    caps.restartMs = 100;
    caps.eventAcks = opt_.eventAcks;
    return caps;
}

bool SimulatedSut::startRun(const std::string &initial, const PluginRunOptions &options, EventQueue &events,
                            std::string *error)
{
    halt();
    if (opt_.failStart) {
        if (error)
            *error = "simulated start failure";
        return false;
    }
    std::lock_guard<std::mutex> lock(m_);
    marking_.clear();
    for (const NamedMarking &m : net_.initialMarkings)
        if (m.name == initial)
            marking_ = toDense(net_, m.tokens);
    if (marking_.empty()) {
        if (error)
            *error = "unknown initial marking \"" + initial + "\"";
        return false;
    }
    initialMarking_ = marking_;  // DeepSeek V4.1 Flash: the lifecycle landing
    events_ = &events;
    quiescenceMs_ = options.quiescenceTimeoutMs;
    // DeepSeek V4.1 Flash: only an agent that can ack is asked to hold; the
    // Session sends an empty list otherwise.
    syncLabels_ = options.syncLabels;
    waitingAck_.reset();
    acks_ = 0;
    dumpSawHeld_ = false;
    rng_.seed(opt_.seed + generation_);
    ++generation_;
    crashed_ = false;
    quiescentSent_ = false;
    eventsPushed_ = 0;
    deviated_ = false;
    clock_ = 0;
    start_ = std::chrono::steady_clock::now();
    lastActivity_ = start_;
    pendingStim_.clear();
    // DeepSeek V4.1 Flash: the synthetic burst is scaffolding, never held (nothing
    // could ack before the run has even been acknowledged).
    for (int i = 0; i < opt_.burstAtStart; ++i)
        push(EventKind::Observed, i % 2 == 0 ? "ping" : "pong", /*hold=*/false);
    stop_ = false;
    worker_ = std::thread([this] { loop(); });
    return true;
}

bool SimulatedSut::stimulate(const std::string &label, std::string *)
{
    std::lock_guard<std::mutex> lock(m_);
    stimuli_.push_back(label);
    if (opt_.silent || crashed_)
        return true;  // swallowed
    if (opt_.ackDelayMs > 0) {
        pendingStim_.push_back({std::chrono::steady_clock::now() + std::chrono::milliseconds(opt_.ackDelayMs), label});
        return true;
    }
    applyStimulus(label);
    return true;
}

PostMortem SimulatedSut::postMortem(DumpMode mode)
{
    std::lock_guard<std::mutex> lock(m_);
    PostMortem pm;
    // DeepSeek V4.1 Flash: is the SUT still held at a synchronized event? Then the
    // dump really sees the stack the event left behind (doku section 19), and
    // the test can check that the ack comes after the dump, not before.
    dumpSawHeld_ = waitingAck_.has_value();
    // GLM-5.3-Flash: a SUT that died on its own cannot be asked for a
    // post-mortem anymore; the crash prediction's dump request runs into
    // this and the session reports the failure.
    if (crashed_) {
        pm.ok = false;
        pm.error = "the simulated SUT is already down";
        return pm;
    }
    ++dumps_;
    pm.ok = true;
    pm.files = {"dump-" + std::to_string(dumps_) + (mode == DumpMode::Crash ? ".core" : ".snapshot")};
    pm.stackText = "frame #0 simulated_sut_main()";
    if (mode == DumpMode::Crash)
        crashed_ = true;  // no more events
    modes_.push_back(mode);
    return pm;
}

void SimulatedSut::stopRun(StopReason)
{
    halt();
    ++stopCalls_;
}

void SimulatedSut::acknowledgeEvent(std::uint64_t queueSeq)
{
    // DeepSeek V4.1 Flash: the tracker handled the event: let the SUT go on. An ack
    // for anything else (a late one, a non-synchronized event) is ignored.
    std::lock_guard<std::mutex> lock(m_);
    if (waitingAck_ && *waitingAck_ == queueSeq) {
        waitingAck_.reset();
        ++acks_;
        ackCv_.notify_all();
    }
}

std::uint64_t SimulatedSut::acknowledgements() const
{
    std::lock_guard<std::mutex> lock(m_);
    return acks_;
}

bool SimulatedSut::holding() const
{
    std::lock_guard<std::mutex> lock(m_);
    return waitingAck_.has_value();
}

bool SimulatedSut::dumpSawHeldEvent() const
{
    std::lock_guard<std::mutex> lock(m_);
    return dumpSawHeld_;
}

std::string SimulatedSut::pollFault()
{
    std::lock_guard<std::mutex> lock(m_);
    return fault_;
}

void SimulatedSut::setFault(const std::string &text)
{
    std::lock_guard<std::mutex> lock(m_);
    fault_ = text;
}

std::vector<std::string> SimulatedSut::stimuliReceived() const
{
    std::lock_guard<std::mutex> lock(m_);
    return stimuli_;
}

std::vector<DumpMode> SimulatedSut::dumpModes() const
{
    std::lock_guard<std::mutex> lock(m_);
    return modes_;
}

bool SimulatedSut::crashed() const
{
    std::lock_guard<std::mutex> lock(m_);
    return crashed_;
}

void SimulatedSut::halt()
{
    {
        std::lock_guard<std::mutex> lock(m_);
        stop_ = true;
    }
    ackCv_.notify_all();  // DeepSeek V4.1 Flash: a held SUT must not block the end of the run
    if (worker_.joinable())
        worker_.join();
}

std::int64_t SimulatedSut::stamp()
{
    if (opt_.clockStepMs > 0) {
        clock_ += opt_.clockStepMs;
        return clock_;
    }
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start_).count();
}

std::optional<std::uint64_t> SimulatedSut::push(EventKind kind, const std::string &label, bool hold)
{
    if (!events_)
        return std::nullopt;
    // DeepSeek V4.1 Flash: a synchronized event is the SUT's own visible move and
    // the run is synchronized: the producer (this thread) holds the SUT until
    // the tracker acknowledges. The flag travels with the queue entry, so the
    // Session knows to stop pumping and ack exactly this event.
    const bool sync = hold && kind == EventKind::Observed && isSyncLabel(label) && !crashed_;
    std::uint64_t seq = 0;
    events_->push(SutEvent{kind, label, stamp()}, &seq, sync);
    ++eventsPushed_;
    quiescentSent_ = false;
    lastActivity_ = std::chrono::steady_clock::now();
    // GLM-5.3-Flash: a death nobody predicted, after N events wherever the
    // SUT is. Checked at the one choke point every event goes through, so
    // the Died event always follows the event that reached the count.
    const bool willDie = !crashed_ && opt_.crashAfterEvents >= 0 && eventsPushed_ >= opt_.crashAfterEvents;
    if (willDie) {
        crashed_ = true;  // set first: the recursive push must not re-trigger
        push(EventKind::Died, "spontaneous failure");
    }
    if (sync) {
        waitingAck_ = seq;
        return seq;
    }
    return std::nullopt;
}

bool SimulatedSut::isSyncLabel(const std::string &label) const
{
    return std::find(syncLabels_.begin(), syncLabels_.end(), label) != syncLabels_.end();
}

void SimulatedSut::waitForAck(std::unique_lock<std::mutex> &lock, std::uint64_t seq)
{
    ackCv_.wait(lock, [&] { return stop_ || !waitingAck_ || *waitingAck_ != seq; });
    if (waitingAck_ && *waitingAck_ == seq)
        waitingAck_.reset();  // stopped while held
}

void SimulatedSut::applyStimulus(const std::string &label)
{
    // GLM-5.3-Flash: handling this stimulus may kill the SUT (a predicted
    // crash, seeded). The acknowledgement goes out first -- the tester did
    // send the input, and the step it started is the one in flight when the
    // SUT dies -- then the single Died event, and the SUT stays down.
    const auto crash = opt_.crashAt.find(label);
    if (crash != opt_.crashAt.end() && crash->second > 0.0
        && static_cast<double>(rng_() >> 11) / 9007199254740992.0 < crash->second) {
        push(EventKind::Stimulated, label);
        if (!crashed_) {  // the event-count death may have fired with the acknowledgement
            crashed_ = true;  // set before the push: the death check must not re-trigger
            push(EventKind::Died, "crashed while handling stimulus '" + label + "'");
        }
        return;
    }
    for (std::size_t i = 0; i < net_.transitions.size(); ++i) {
        const Transition &t = net_.transitions[i];
        if (t.kind == TransitionKind::Controllable && t.label == label && isEnabled(t, marking_)) {
            Marking next(marking_.size());
            if (fire(net_, t, marking_.data(), next.data()).status == FireStatus::Ok)
                marking_ = next;
            // DeepSeek V4.1 Flash: an engine-landed start/restart (no post arcs,
            // doku section 26) brings the simulated SUT itself back to the run's
            // initial marking -- the model says the SUT's own control did it, so
            // the simulation must not keep its old marking.
            if ((t.lifecycle == LifecycleAction::Start || t.lifecycle == LifecycleAction::Restart)
                && t.post.empty())
                marking_ = initialMarking_;
            break;
        }
    }
    push(EventKind::Stimulated, label);
}

double SimulatedSut::weightOf(const Transition &t) const
{
    const auto it = opt_.weight.find(t.id);
    return it == opt_.weight.end() ? 1.0 : it->second;
}

void SimulatedSut::loop()
{
    for (;;) {
        std::this_thread::sleep_for(std::chrono::milliseconds(opt_.stepDelayMs));
        std::unique_lock<std::mutex> lock(m_);
        if (stop_ || crashed_)
            return;
        if (opt_.silent)
            continue;

        const auto now = std::chrono::steady_clock::now();
        while (!pendingStim_.empty() && pendingStim_.front().due <= now) {
            const std::string label = pendingStim_.front().label;
            pendingStim_.erase(pendingStim_.begin());
            applyStimulus(label);
        }

        if (!deviated_ && opt_.deviateAfterEvents >= 0 && eventsPushed_ >= opt_.deviateAfterEvents) {
            deviated_ = true;
            push(EventKind::Observed, opt_.deviationLabel);
            continue;
        }

        // The SUT's own move: an enabled output or internal step.
        std::vector<std::size_t> enabled;
        double total = 0.0;
        for (std::size_t i = 0; i < net_.transitions.size(); ++i) {
            const Transition &t = net_.transitions[i];
            if (t.kind == TransitionKind::Controllable || !isEnabled(t, marking_))
                continue;
            const double w = weightOf(t);
            if (w <= 0.0)
                continue;
            enabled.push_back(i);
            total += w;
        }
        if (enabled.empty()) {
            const auto quietFor = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastActivity_).count();
            if (!quiescentSent_ && quietFor >= quiescenceMs_ && pendingStim_.empty()) {
                push(EventKind::Quiescent, "");
                quiescentSent_ = true;
            }
            continue;
        }
        double pick = (static_cast<double>(rng_() >> 11) / 9007199254740992.0) * total;
        std::size_t chosen = enabled.back();
        for (std::size_t i : enabled) {
            pick -= weightOf(net_.transitions[i]);
            if (pick < 0.0) {
                chosen = i;
                break;
            }
        }
        const Transition &t = net_.transitions[chosen];
        Marking next(marking_.size());
        if (fire(net_, t, marking_.data(), next.data()).status != FireStatus::Ok)
            continue;  // a firing the model's bounds forbid: the SUT does not do it
        marking_ = next;
        lastActivity_ = std::chrono::steady_clock::now();
        quiescentSent_ = false;
        const bool visible = t.kind == TransitionKind::Observable
                             || (t.kind == TransitionKind::Internal && t.traced);  // GLM-5.3-Flash
        if (visible) {
            // DeepSeek V4.1 Flash: a synchronized event freezes the SUT here, after
            // the move and before the next one, until the tracker acked.
            const std::optional<std::uint64_t> held = push(EventKind::Observed, t.label);
            if (held)
                waitForAck(lock, *held);
        }
    }
}

}  // namespace pn
