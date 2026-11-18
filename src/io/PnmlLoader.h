// GLM-5.3-Flash: loads a Petri net from PNML (Petri Net Markup Language,
// ISO/IEC 15909-2), doku section 12 item 6. A minimal, dependency-free XML
// reader; enough for PNML, nothing more.
//
// WHAT IS IMPORTED
//   places, transitions, arcs (normal and inhibitor, with weights), the
//   per-place <initialMarking>s -- they are UNIONED into ONE initial marking
//   of the model -- and the net's <name>.
//
// THE MBT CONVENTION for kinds and labels: a <toolspecific> child of a
// transition with tool="mbt" (or no tool attribute):
//     <toolspecific tool="mbt" kind="controllable|observable|internal" label="..."/>
//   kind sets TransitionKind, label sets the SUT action/event. Without a
//   toolspecific, the transition id's first byte decides as a fallback:
//   '?' -> controllable, '!' -> observable, anything else -> internal SILENT
//   (the id is not used as a label, so a named internal step never becomes a
//   traced one by accident). A milestone is tagged the same way, as a second
//   toolspecific so it composes with the kind:
//     <toolspecific tool="mbt" kind="milestone" rank="2"/>
//   (rank >= 1, default 1; see Transition::milestone).
//
// WHAT IS NOT IMPORTED (reported as warnings): place bounds (<capacity>) and
// any place toolspecific -- PNML has no mbt target or error markings, so a
// PNML model has none; declare those in the JSON form if needed.
//
// Like the JSON loader, this loader never stops at the first problem: it
// collects every error and warning, and a net is only produced when there are
// no errors.
#pragma once

#include "io/JsonNetLoader.h"

#include <string>

namespace pn {

class PnmlLoader {
public:
    static LoadResult loadFile(const std::string &fileName);
    static LoadResult loadText(const std::string &xml, const std::string &sourceName);
};

}  // namespace pn
