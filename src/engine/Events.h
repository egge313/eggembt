// Events the plug-in reports about the live SUT.
//
//  Observed    the SUT did something visible: an output (observable
//              transition) or a traced internal step (an internal transition
//              that carries a label).
//  Stimulated  the tester sent an input (controllable transition).
//  Quiescent   the quiescence timeout elapsed without any output.
//  Died        the SUT died on its own (it crashed, its process exited, the
//              connection broke): the plug-in pushes exactly one Died event,
//              with whatever reason it knows in the label, and no further
//              events for the run. GLM-5.3-Flash: the missing surface for
//              spontaneous SUT deaths -- until now only the tester-induced
//              post-mortem crash could end a SUT.
//
// Internal transitions WITHOUT a label are silent: they are never reported
// and the tracker has to reason about them (see BeliefTracker).
#pragma once

#include <cstdint>
#include <string>

namespace pn {

enum class EventKind { Observed, Stimulated, Quiescent, Died };  // GLM-5.3-Flash: Died

struct SutEvent {
    EventKind kind = EventKind::Observed;
    std::string label;  // empty for Quiescent; for Died, the death reason if known
    // When it happened, in milliseconds on any monotonic clock (typically since
    // the start of the run). Only differences matter; they drive the run-time
    // livelock timeout. Recorded in the run log so replays are deterministic.
    std::int64_t timestampMs = 0;
};

inline const char *toString(EventKind k)
{
    switch (k) {
    case EventKind::Observed:   return "observed";
    case EventKind::Stimulated: return "stimulated";
    case EventKind::Quiescent:  return "quiescent";
    case EventKind::Died:       return "died";
    }
    return "observed";
}

inline bool parseEventKind(const std::string &s, EventKind &out)
{
    for (EventKind k : {EventKind::Observed, EventKind::Stimulated, EventKind::Quiescent,
                        EventKind::Died}) {
        if (s == toString(k)) {
            out = k;
            return true;
        }
    }
    return false;
}

}  // namespace pn
