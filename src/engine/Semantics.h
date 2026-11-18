// Token-game semantics for pn::Net: enabling (with inhibitor arcs), firing,
// marking classification and hashing. Plain C++17, no Qt.
//
// Markings here are dense: one TokenCount per place, indexed by PlaceIndex.
// Functions take raw pointers so callers can pass slices of a state arena
// without copying.
#pragma once

#include "model/Net.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace pn {

using Marking = std::vector<TokenCount>;

enum class StateKind : std::uint8_t {
    Normal,
    Target,  // matches a target marking (and no error marking)
    Error   // matches an error marking; error wins over target
};

struct Classification {
    StateKind kind = StateKind::Normal;
    int markingIndex = -1;  // index into net.errorMarkings / net.targetMarkings, -1 for Normal
};

enum class FireStatus {
    Ok,
    BoundExceeded,  // a produced token count exceeds the place's declared bound
    Overflow        // a token count would not fit into 32 bits (unbounded place)
};

struct FireResult {
    FireStatus status = FireStatus::Ok;
    PlaceIndex place = 0;  // offending place when status != Ok
};

// Sparse -> dense. The result has net.places.size() entries.
Marking toDense(const Net &net, const SparseMarking &sparse);

// Enabled iff every normal input arc has m(p) >= w and every inhibitor arc
// has m(p) < w. Bounds are not considered: a bound violation is reported by
// fire(), not by disabling the transition.
bool isEnabled(const Transition &t, const TokenCount *marking);
inline bool isEnabled(const Transition &t, const Marking &m) { return isEnabled(t, m.data()); }

// Fires `t` from `from` into `to` (both net.places.size() long, must not
// overlap). Precondition: t is enabled in `from`. Tokens are consumed first,
// then produced, so a transition that reads and writes the same place works.
// On a non-Ok status the contents of `to` are unspecified.
FireResult fire(const Net &net, const Transition &t, const TokenCount *from, TokenCount *to);

// Does `marking` satisfy the named marking (exact or covers)?
bool matches(const NamedMarking &spec, const TokenCount *marking, std::size_t numPlaces);

// Error markings are checked first, then target markings.
Classification classify(const Net &net, const TokenCount *marking);

// All transitions enabled in `marking`, in index order.
std::vector<TransitionIndex> enabledTransitions(const Net &net, const TokenCount *marking);

std::size_t hashTokens(const TokenCount *tokens, std::size_t count);

}  // namespace pn
