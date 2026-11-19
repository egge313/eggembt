// The wire protocol between the tracker (the Session side) and an agent that
// sits next to the SUT, possibly in another process, on another machine, in
// another language. One JSON object per line, UTF-8, "\n" terminated.
// See docs/PLUGIN_PROTOCOL.md for the full description with examples.
//
//   agent -> tracker   hello             first message: name, capabilities
//                      run_started       answer to start_run
//                      event             something happened in the SUT
//                      post_mortem_result  answer to post_mortem
//                      fault             the agent has an unrecoverable problem
//   tracker -> agent   start_run         start the SUT in an initial marking
//                      stimulate         send an input
//                      post_mortem       take a snapshot or crash dump
//                      ack               the event is handled, the SUT may go on
//                      stop_run          end the run
//                      shutdown          the session is over
//
// DeepSeek V4.1 Flash: synchronized events (doku section 19) are capability-gated,
// so the version stays 1: an agent that cannot hold its SUT simply does not
// set "sync" in its hello, and a tracker that does not know "ack" never gets
// one (an event with "sync": true is only sent to a tracker that asked for
// synchronization in start_run).
//
// Unknown fields are ignored (so the protocol can grow); unknown message types
// are an error.
#pragma once

#include "engine/Events.h"
#include "model/Net.h"

#include <cstdint>
#include <string>
#include <vector>

namespace pn {

constexpr int kProtocolVersion = 1;

struct ProtocolMessage {
    enum class Type {
        Hello,
        RunStarted,
        Event,
        PostMortemResult,
        Fault,
        StartRun,
        Stimulate,
        PostMortem,
        Ack,  // DeepSeek V4.1 Flash: tracker -> agent, synchronized events
        StopRun,
        Shutdown
    };

    Type type = Type::Shutdown;

    // hello
    int protocol = kProtocolVersion;
    std::string name;
    bool snapshot = false;
    bool crash = false;
    std::int64_t restartMs = 0;
    bool syncCapable = false;  // DeepSeek V4.1 Flash: the agent can hold the SUT at an event

    // start_run (and echoed in every event of that run, so that late events of
    // a previous run can be told apart and dropped)
    std::int64_t run = 0;
    std::string initial;
    std::int64_t quiescenceMs = 0;
    // DeepSeek V4.1 Flash: labels the agent must hold the SUT at (see syncCapable).
    std::vector<std::string> syncLabels;

    // event
    EventKind kind = EventKind::Observed;
    std::string label;          // event, stimulate
    std::int64_t timestampMs = 0;
    // DeepSeek V4.1 Flash: per-run event number (0, 1, 2, ...), and whether the
    // agent is holding the SUT at this event until an ack for this number.
    std::int64_t seq = 0;
    bool syncEvent = false;

    // post_mortem / post_mortem_result
    std::int64_t id = 0;
    DumpMode mode = DumpMode::Snapshot;
    bool ok = false;
    std::vector<std::string> files;
    std::string stack;

    // run_started, post_mortem_result, fault, stop_run
    std::string error;          // error text; for fault the message
    std::string reason;         // stop_run: "finished" or "aborted"
};

// One line, without the trailing newline.
std::string encodeMessage(const ProtocolMessage &message);

// Returns false, with `error` set, for malformed JSON, a missing or unknown
// "type", or a missing mandatory field.
bool decodeMessage(const std::string &line, ProtocolMessage &out, std::string *error = nullptr);

}  // namespace pn
