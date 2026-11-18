// Overlap of coverages: how much do two test runs (or two whole campaigns)
// have in common?
//
// THE METRIC. For two sets of covered elements A and B the Jaccard index
// |A∩B| / |A∪B| says how much they overlap, independent of how large the
// model is: 1 = identical sets, 0 = nothing in common. It is computed
// separately for transitions, edges and states, because a campaign can
// revisit the same few transitions over wide state space or the other way
// round.
//
// WHAT A RUN COVERS. Every run records the states it definitely visited and
// the transitions that certainly fired (CoverageDb does the attribution);
// this file only sorts and de-duplicates those into sets. A replayed run
// yields the same sets as the live run, so campaigns can be compared after
// the fact from run logs (`mbt compare`).
//
// Runs (and campaigns) with nothing covered at all count as identical
// (index 1): two runs that covered nothing have the same coverage.
//
// Not thread-safe (it only reads the database).
#pragma once

#include "engine/CoverageDb.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace pn {

// The elements one run definitely touched, sorted and unique. The initial
// state is included.
struct RunCoverage {
    std::vector<std::uint32_t> states;
    std::vector<std::uint32_t> edges;
    std::vector<std::uint32_t> transitions;
};

// The union over all finished runs of the database.
struct CampaignCoverage {
    std::vector<std::uint32_t> states;
    std::vector<std::uint32_t> edges;
    std::vector<std::uint32_t> transitions;
};

RunCoverage runCoverage(const CoverageDb &db, RunId id);
CampaignCoverage campaignCoverage(const CoverageDb &db);

// |A∩B| / |A∪B| for two sorted, unique sets; 1 for two empty sets.
double jaccardIndex(const std::vector<std::uint32_t> &a, const std::vector<std::uint32_t> &b);

// All pairwise indices between the runs of one database, row-major n*n
// (symmetric, diagonal 1).
struct OverlapMatrix {
    std::size_t n = 0;
    std::vector<double> transitions;
    std::vector<double> states;
    std::vector<double> edges;

    double at(const std::vector<double> &m, std::size_t i, std::size_t j) const
    {
        return m[static_cast<std::size_t>(i) * n + j];
    }
    // Mean over the pairs i < j; 0 when there are fewer than two runs.
    double meanPairwise(const std::vector<double> &m) const;
};

OverlapMatrix runOverlapMatrix(const CoverageDb &db);

}  // namespace pn
