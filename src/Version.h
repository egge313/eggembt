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
// between runs, and the T-list -> T-suffix rename (doku §27).
//
// v0.0.3 (19 November 2026): the optional satisfied-target phase policy
// (--satisfied-target-policy, doku §28).
//
// v0.0.4 (19 November 2026): rare transitions and the hunt (the "rare" model
// key, --hunt-rare, dwells and attempt budgets, doku §30), plus the measured
// evaporation plateau (doku §31).
//
// v0.0.5 (22 November 2026): merge-time evaporation of restored pheromone,
// sparing matured targets and errors (journal §34). The rule for this file:
// the version is the git branch name, and `main` is the exception because its
// name is not a version.
#pragma once

#define EGGEMBT_VERSION "v0.0.5"
