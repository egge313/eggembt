#include "engine/Scc.h"

#include <algorithm>

namespace pn {

Sccs computeScc(const ReachabilityGraph &g, const std::function<bool(const Edge &)> &keep)
{
    constexpr std::uint32_t kUnvisited = 0xFFFFFFFFu;
    const std::size_t n = g.numStates();

    std::vector<std::uint32_t> index(n, kUnvisited);
    std::vector<std::uint32_t> low(n, 0);
    std::vector<char> onStack(n, 0);
    std::vector<StateId> stack;

    Sccs out;
    out.componentOf.assign(n, kUnvisited);

    struct Frame {
        StateId v;
        std::size_t next;  // position in v's successor list
    };
    std::vector<Frame> call;
    std::uint32_t counter = 0;

    for (StateId root = 0; root < n; ++root) {
        if (index[root] != kUnvisited)
            continue;
        index[root] = low[root] = counter++;
        stack.push_back(root);
        onStack[root] = 1;
        call.push_back({root, 0});

        while (!call.empty()) {
            const StateId v = call.back().v;
            const EdgeRange edges = g.isExpanded(v) ? g.successors(v) : EdgeRange{};

            bool descended = false;
            while (call.back().next < edges.size()) {
                const Edge &e = edges.first[call.back().next++];
                if (keep && !keep(e))
                    continue;
                const StateId w = e.target;
                if (index[w] == kUnvisited) {
                    index[w] = low[w] = counter++;
                    stack.push_back(w);
                    onStack[w] = 1;
                    call.push_back({w, 0});
                    descended = true;
                    break;
                }
                if (onStack[w])
                    low[v] = std::min(low[v], index[w]);
            }
            if (descended)
                continue;

            if (low[v] == index[v]) {
                const auto id = static_cast<std::uint32_t>(out.members.size());
                std::vector<StateId> component;
                StateId w;
                do {
                    w = stack.back();
                    stack.pop_back();
                    onStack[w] = 0;
                    out.componentOf[w] = id;
                    component.push_back(w);
                } while (w != v);
                std::sort(component.begin(), component.end());
                out.members.push_back(std::move(component));
            }
            call.pop_back();
            if (!call.empty()) {
                const StateId parent = call.back().v;
                low[parent] = std::min(low[parent], low[v]);
            }
        }
    }
    return out;
}

}  // namespace pn
