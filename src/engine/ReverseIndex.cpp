#include "engine/ReverseIndex.h"

namespace pn {

void ReverseIndex::sync()
{
    head_.resize(g_.numStates(), kEnd);
    for (; syncedExpansions_ < g_.numExpanded(); ++syncedExpansions_) {
        const StateId s = g_.expandedState(syncedExpansions_);
        EdgeId e = g_.firstEdge(s);
        for (const Edge &edge : g_.successors(s)) {
            rev_.push_back({s, e, head_[edge.target]});
            head_[edge.target] = static_cast<std::uint32_t>(rev_.size() - 1);
            ++e;
        }
    }
}

}  // namespace pn
