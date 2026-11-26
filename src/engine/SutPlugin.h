// What a plug-in has to offer so that a Session can test a system under test.
//
// The plug-in sits next to the SUT (in its process, or as an agent talking to
// it) and does four things: it starts the SUT in a given initial state, it
// sends the inputs the Session suggests, it takes post-mortems on request, and
// it reports everything that happens as EVENTS.
//
// EVENTS ARE PUSHED, ASYNCHRONOUSLY. The plug-in puts every event into the
// EventQueue it was given in startRun(), from whatever thread observes it, at
// the moment of capture and with a capture timestamp (see EventQueue). It never
// waits for the Session. In particular:
//   * an input the Session asked for is acknowledged by a Stimulated event,
//     which may arrive at any time after stimulate() returned (before, if the
//     plug-in is in-process and fast; much later, if the agent is remote);
//   * if the SUT stays silent for the quiescence timeout of the run options,
//     the plug-in pushes a Quiescent event (the tracker has no clock);
//   * events must be pushed in the order they were captured: the order IS the
//     truth the tracker works with.
//
// THREADS. The Session calls the methods below from its own thread, one at a
// time. The plug-in may push events from any number of threads.
//
// POST-MORTEMS. A Snapshot is a non-destructive dump (gcore, MiniDumpWriteDump,
// ...): the SUT keeps running and its events keep arriving. A Crash ends the
// SUT in a controlled way to obtain a genuine crash dump. A plug-in that cannot
// do one or the other says so in its capabilities, and the Session will not ask.
#pragma once

#include "engine/EventQueue.h"
#include "model/Net.h"

#include <cstdint>
#include <string>
#include <vector>

namespace pn {

struct PluginCapabilities {
    bool snapshot = false;
    bool crash = false;
    std::int64_t restartMs = 0;  // typical time to restart the SUT (informational)

    // DeepSeek V4.1 Flash: the plug-in can HOLD the SUT at a synchronized event and
    // wait for acknowledgeEvent() (doku section 19). Agents advertise this in
    // their hello; a run that needs it but cannot get it says so in a note.
    bool eventAcks = false;

    // DeepSeek V4.1 Flash: the initial markings the plug-in can actually
    // realise (doku section 41). startRun() already receives the marking's
    // name; this capability lets the Session choose among the ones the SUT
    // supports instead of discovering the mismatch mid-campaign. Empty means
    // "not advertised": the Session tries every marking, as before.
    std::vector<std::string> initialMarkings;
};

struct PluginRunOptions {
    // Silence for this long, with no event from the SUT, makes the plug-in push
    // a Quiescent event.
    std::int64_t quiescenceTimeoutMs = 1000;

    // DeepSeek V4.1 Flash: the labels of the model's synchronized transitions (doku
    // section 19). After pushing an Observed event with one of these labels the
    // plug-in must hold the SUT at that point until the tracker calls
    // acknowledgeEvent(). Empty: no event is held.
    std::vector<std::string> syncLabels;
};

struct PostMortem {
    bool ok = false;
    std::string error;                // when !ok
    std::vector<std::string> files;   // core file, minidump, stack file, ...
    std::string stackText;            // the SUT's stack, if the plug-in can give it as text
};

enum class StopReason { Finished, Aborted };

class ISutPlugin {
public:
    virtual ~ISutPlugin() = default;

    virtual PluginCapabilities capabilities() const = 0;

    // Starts (or restarts) the SUT in the state described by the named initial
    // marking of the model. From now on events go into `events`. Must not wait
    // for the SUT to do anything. Returns false, with `error` set, if the SUT
    // could not be started.
    virtual bool startRun(const std::string &initialMarking, const PluginRunOptions &options,
                          EventQueue &events, std::string *error) = 0;

    // Hands an input to the SUT. Returns false if it could not be handed over.
    // The matching Stimulated event is pushed when the input is actually sent.
    virtual bool stimulate(const std::string &label, std::string *error) = 0;

    // DeepSeek V4.1 Flash: the tracker handled the event with this EventQueue
    // sequence number and the SUT may continue (doku section 19). Only
    // meaningful for an event pushed with the ack flag; a plug-in without the
    // eventAcks capability keeps the default no-op.
    virtual void acknowledgeEvent(std::uint64_t queueSeq) { (void)queueSeq; }

    // Takes a post-mortem; may take a while. After Crash the SUT is gone.
    virtual PostMortem postMortem(DumpMode mode) = 0;

    // Ends the run: stops (or cleans up after) the SUT. When it returns, the
    // plug-in must not touch the event queue any more (no push in progress, none
    // to come): the Session may destroy the queue right away.
    virtual void stopRun(StopReason reason) = 0;

    // Non-empty if the plug-in found an unrecoverable problem of its own (lost
    // connection, SUT refused an input, ...). The Session polls this and aborts
    // the run.
    virtual std::string pollFault() { return {}; }
};

}  // namespace pn
