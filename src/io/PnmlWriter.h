// DeepSeek V4.1 Flash: PNML export (doku section 27). The counterpiece of
// PnmlLoader: writes a model as standard Petri Net Markup Language
// (ISO/IEC 15909-2), so another Petri-net tool can read the net.
//
// WHAT STANDARD PNML CARRIES
//   places, transitions, normal and inhibitor arcs with their weights, and
//   ONE initial marking (the model's first; PNML cannot say "one of several
//   initial markings"). The eggembt kind/label/milestone conventions are
//   written as <toolspecific> children, the same ones PnmlLoader reads, so a
//   round trip keeps the transition kinds, labels and milestone ranks.
//
// WHAT PNML CANNOT CARRY
//   target and error markings, their policies and crash predictions, place
//   bounds, multiple initial markings and the model's testing strategy have no
//   standard PNML place. They are written twice: as XML comments beside the
//   elements (for a human reading the file) and as one machine-readable JSON
//   block inside
//       <toolspecific tool="eggembt" version="..."><text>...</text></toolspecific>
//   (for a script). PnmlLoader ignores that block -- a PNML model has none of
//   these by definition -- so these annotations do NOT survive an import.
//   This is stated in the file's header comment.
#pragma once

#include "model/Net.h"

#include <string>

namespace pn {

// The whole model as one PNML document. Always succeeds; every name is
// XML-escaped and every id made PNML-safe (an unsafe id is replaced by pN/tN
// and its original kept in the annotations).
std::string toPnml(const Net &net);

}  // namespace pn
