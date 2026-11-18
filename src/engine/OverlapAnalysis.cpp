#include "engine/OverlapAnalysis.h"

#include <algorithm>
#include <cassert>

namespace pn {

namespace {

void sortUnique(std::vector<std::uint32_t> &v)
{
    std::sort(v.begin(), v.end());
    v.erase(std::unique(v.begin(), v.end()), v.end());
}

}  // namespace

RunCoverage runCoverage(const CoverageDb &db, RunId id)
{
    RunCoverage out;
    const RunRecord &run = db.run(id);
    out.states = run.states;
    out.transitions = run.transitions;
    out.edges.reserve(run.steps.size());
    for (const RunStep &s : run.steps)
        out.edges.push_back(s.edge);
    sortUnique(out.states);
    sortUnique(out.edges);
    sortUnique(out.transitions);
    return out;
}

CampaignCoverage campaignCoverage(const CoverageDb &db)
{
    CampaignCoverage out;
    for (RunId i = 0; i < db.numRuns(); ++i) {
        const RunRecord &run = db.run(i);
        out.states.insert(out.states.end(), run.states.begin(), run.states.end());
        out.transitions.insert(out.transitions.end(), run.transitions.begin(), run.transitions.end());
        for (const RunStep &s : run.steps)
            out.edges.push_back(s.edge);
    }
    sortUnique(out.states);
    sortUnique(out.edges);
    sortUnique(out.transitions);
    return out;
}

double jaccardIndex(const std::vector<std::uint32_t> &a, const std::vector<std::uint32_t> &b)
{
    if (a.empty() && b.empty())
        return 1.0;  // nothing covered on either side: the same coverage
    std::size_t common = 0;
    std::size_t i = 0, j = 0;
    while (i < a.size() && j < b.size()) {
        if (a[i] < b[j])
            ++i;
        else if (b[j] < a[i])
            ++j;
        else {
            ++common;
            ++i;
            ++j;
        }
    }
    const std::size_t total = a.size() + b.size() - common;
    return total == 0 ? 1.0 : static_cast<double>(common) / static_cast<double>(total);
}

double OverlapMatrix::meanPairwise(const std::vector<double> &m) const
{
    if (n < 2)
        return 0.0;
    double sum = 0.0;
    std::size_t pairs = 0;
    for (std::size_t i = 0; i < n; ++i)
        for (std::size_t j = i + 1; j < n; ++j) {
            sum += m[i * n + j];
            ++pairs;
        }
    assert(pairs > 0);
    return sum / static_cast<double>(pairs);
}

OverlapMatrix runOverlapMatrix(const CoverageDb &db)
{
    OverlapMatrix out;
    out.n = db.numRuns();
    const std::size_t n = out.n;
    if (n == 0)
        return out;

    std::vector<RunCoverage> cov;
    cov.reserve(n);
    for (RunId i = 0; i < n; ++i)
        cov.push_back(runCoverage(db, i));

    out.transitions.assign(n * n, 1.0);
    out.states.assign(n * n, 1.0);
    out.edges.assign(n * n, 1.0);
    for (std::size_t i = 0; i < n; ++i)
        for (std::size_t j = i; j < n; ++j) {
            const double tr = jaccardIndex(cov[i].transitions, cov[j].transitions);
            const double st = jaccardIndex(cov[i].states, cov[j].states);
            const double ed = jaccardIndex(cov[i].edges, cov[j].edges);
            out.transitions[i * n + j] = out.transitions[j * n + i] = tr;
            out.states[i * n + j] = out.states[j * n + i] = st;
            out.edges[i * n + j] = out.edges[j * n + i] = ed;
        }
    return out;
}

}  // namespace pn
