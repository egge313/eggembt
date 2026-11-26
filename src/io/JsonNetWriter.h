// DeepSeek V4.1 Flash: writes a model back as an eggembt JSON document.
//
// This is the counterpiece of JsonNetLoader and the second half of the PNML
// import path: PnmlLoader reads PNML into pn::Net, and this writer turns that
// net into the JSON form a modeller can annotate (targets, errors, bounds,
// crash predictions, the testing strategy, ...). The output is a complete
// model, not a lossy sketch: every meaningful annotation the loader
// understands is written unless it has its default value, and the JSON is
// pretty-printed so it can be edited by hand.
//
// PNML itself cannot carry the testing annotations. An import from PNML
// therefore produces a JSON model with the standard net and the transition
// kinds/labels/milestones that the eggembt toolspecific block carried; the
// remaining annotations are what the modeller adds next.
#pragma once

#include "model/Net.h"

#include <string>

namespace pn {

// The whole model as one pretty-printed JSON document. Always succeeds.
std::string toJsonModel(const Net &net);

}  // namespace pn
