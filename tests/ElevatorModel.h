// The elevator door model used by most engine tests, built in code.
//
// It is the same model as examples/elevator-door.json: a door that opens,
// may obstruct, closes, and may jam. The marking order of the six places is
//   closed, opening, open, closing, obstructed, fault
// and the seven transitions are
//   open_cmd  (controllable, door.open)   closed -> opening
//   opened    (observable,  door.opened)  opening -> open
//   close_cmd (controllable, door.close)  open -> closing, inhibited while obstructed
//   closed_ev (observable,  door.closed)  closing -> closed
//   obstruct  (internal, silent)          open -> open + obstructed
//   clear     (internal, silent)          obstructed -> nothing
//   jam       (internal, silent)          closing -> fault
//
// `error` configures the "jammed" error marking; default: until_covered at the
// global coverage threshold, no post-mortem.
#pragma once

#include "model/Net.h"
#include "model/NetBuilder.h"

namespace testmodels {

inline pn::Net makeElevatorNet(pn::ErrorOptions error = {})
{
    pn::NetBuilder b("elevator-door");

    const auto closed = b.place("closed");
    const auto opening = b.place("opening");
    const auto open = b.place("open");
    const auto closing = b.place("closing");
    const auto obstructed = b.place("obstructed", 1);
    const auto fault = b.place("fault", 1);

    const auto openCmd = b.transition("open_cmd", pn::TransitionKind::Controllable, "door.open");
    const auto opened = b.transition("opened", pn::TransitionKind::Observable, "door.opened");
    const auto closeCmd = b.transition("close_cmd", pn::TransitionKind::Controllable, "door.close");
    const auto closedEv = b.transition("closed_ev", pn::TransitionKind::Observable, "door.closed");
    const auto obstruct = b.transition("obstruct");
    const auto clear = b.transition("clear");
    const auto jam = b.transition("jam");

    b.input(closed, openCmd).output(openCmd, opening);
    b.input(opening, opened).output(opened, open);
    b.input(open, closeCmd).output(closeCmd, closing).inhibitor(obstructed, closeCmd);
    b.input(closing, closedEv).output(closedEv, closed);

    b.input(open, obstruct).output(obstruct, open).output(obstruct, obstructed);
    b.inhibitor(obstructed, obstruct);
    b.input(obstructed, clear);

    b.input(closing, jam).output(jam, fault);

    b.initial("start", {{closed, 1}});
    b.targetMarking("idle_closed", {{closed, 1}});
    b.errorMarking("jammed", {{fault, 1}}, pn::MatchMode::Covers, error);

    return b.build();
}

}  // namespace testmodels
