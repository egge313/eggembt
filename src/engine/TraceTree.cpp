#include "engine/TraceTree.h"

#include <algorithm>

namespace pn {

TraceTree::TraceTree()
{
    nodes_.push_back(Node{kNoNode, kNoEdge, kNoState, 0, 0});  // the root
}

NodeId TraceTree::child(NodeId parent, EdgeId via, StateId state)
{
    const Key key{parent, via, state};
    const auto it = index_.find(key);
    if (it != index_.end()) {
        ++nodes_[it->second].visits;
        return it->second;
    }
    const auto id = static_cast<NodeId>(nodes_.size());
    const std::uint32_t depth = nodes_[parent].depth + 1;
    nodes_.push_back(Node{parent, via, state, depth, 1});
    index_.emplace(key, id);
    return id;
}

std::vector<TraceStep> TraceTree::path(NodeId n, std::size_t maxSteps) const
{
    std::vector<TraceStep> out;
    while (n != kRootNode && n != kNoNode && out.size() < maxSteps) {
        const Node &node = nodes_[n];
        out.push_back({node.state, node.via});
        n = node.parent;
    }
    std::reverse(out.begin(), out.end());
    return out;
}

}  // namespace pn
