// The product version, in one place.
//
// DeepSeek V4.1 Flash: the rename batch left a hard-coded "1.0.0" in the CMake
// project, which was premature -- what exists is the tree on the git branch
// `v0.0.1`. Egge: "Retract the version to the current git branch". The number
// now lives here, follows the branch, and is no longer a CMake definition:
// `eggembt version` and the usage header print it as it stands (doku §20).
//
// v0.0.2 (19 November 2026): PNML export, the testing-strategy options (goal
// and error priorities, a wall-clock campaign budget), pheromone evaporation
// between runs, the optional satisfied-target phase veto, and the T-list ->
// T-suffix rename (doku sections 27 and 28).
#pragma once

#define EGGEMBT_VERSION "v0.0.2"
