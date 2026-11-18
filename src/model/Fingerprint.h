#pragma once

#include "model/Net.h"

#include <string>

namespace pn {

// Identity of the net's *graph*: places (id, bound), transitions (id and arcs
// by place id) and the named initial markings. It is independent of
// declaration order, labels, kinds, and target/error markings, because none of
// those change which markings are reachable or which transitions fire. A run
// log recorded against one fingerprint can be replayed on any net with the
// same one. Returns 16 lowercase hex digits (64-bit FNV-1a).
std::string graphFingerprint(const Net &net);

}  // namespace pn
