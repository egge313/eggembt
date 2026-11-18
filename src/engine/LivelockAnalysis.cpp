#include "engine/LivelockAnalysis.h"

#include "engine/Scc.h"

#include <algorithm>

namespace pn {

namespace {

bool hasSelfLoop(const ReachabilityGraph &g, StateId s, const std::function<bool(const Edge &)> &keep)
{
    if (!g.isExpanded(s))
        return false;
    for (const Edge &e : g.successors(s))
        if (e.target == s && (!keep || keep(e)))
            return true;
    return false;
}

}  // namespace

LivelockReport analyzeLivelocks(const ReachabilityGraph &g, const LivelockOptions &options)
{
    LivelockReport report;
    const Net &net = g.net();
    const std::size_t n = g.numStates();

    report.graphComplete = g.numExpanded() == n && !g.truncated() && g.boundViolations().empty();

    std::vector<char> hasDroppedFiring(n, 0);
    for (const BoundViolation &v : g.boundViolations())
        hasDroppedFiring[v.state] = 1;

    // --- terminal cycles --------------------------------------------------------
    {
        const Sccs scc = computeScc(g);
        report.components = scc.members.size();

        for (std::uint32_t c = 0; c < scc.members.size(); ++c) {
            const std::vector<StateId> &members = scc.members[c];
            const bool cyclic = members.size() > 1 || hasSelfLoop(g, members[0], {});
            if (!cyclic)
                continue;
            ++report.cyclicComponents;
            if (!options.terminalCycles)
                continue;

            bool terminal = true;
            bool hasTarget = false;
            bool certain = true;
            for (StateId u : members) {
                if (g.kind(u) == StateKind::Target)
                    hasTarget = true;
                if (g.isExpanded(u)) {
                    if (g.isIncomplete(u) || hasDroppedFiring[u])
                        certain = false;
                    for (const Edge &e : g.successors(u))
                        if (scc.componentOf[e.target] != c)
                            terminal = false;
                }
            }
            if (terminal && !hasTarget)
                report.findings.push_back({LivelockKind::TerminalCycle, members, certain});
        }
    }

    // --- internal cycles (divergence) -----------------------------------------------
    if (options.internalCycles) {
        const auto internalOnly = [&net](const Edge &e) {
            return net.transitions[e.transition].kind == TransitionKind::Internal;
        };
        const Sccs scc = computeScc(g, internalOnly);
        for (const std::vector<StateId> &members : scc.members) {
            if (members.size() > 1 || hasSelfLoop(g, members[0], internalOnly))
                report.findings.push_back({LivelockKind::InternalCycle, members, true});
        }
    }

    std::sort(report.findings.begin(), report.findings.end(),
              [](const LivelockFinding &a, const LivelockFinding &b) {
                  return a.states.front() < b.states.front();
              });
    return report;
}

}  // namespace pn
