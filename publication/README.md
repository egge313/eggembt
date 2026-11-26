# A formal account of the Petri nets and the ant colony

`kettunen2026.tex` is a self-contained LaTeX article that defines, in mathematical
terms, the Petri nets, reachability graph, coverage and target machinery, the
off-target region, the ant-colony equations (including the T-suffix layer) and
the suggester score used by `eggembt` at version `v0.0.8`.  The definitions
follow the implementation; the final correspondence section maps every symbol
to its source file.

## Build

A TeX distribution with `pdflatex` and `bibtex` is enough:

```sh
cd publication
pdflatex kettunen2026.tex
bibtex kettunen2026
pdflatex kettunen2026.tex
pdflatex kettunen2026.tex
```

The result is `kettunen2026.pdf`.  With `latexmk` installed, one command does the same:

```sh
latexmk -pdf kettunen2026.tex
```

The generated files (`kettunen2026.aux`, `kettunen2026.bbl`, `kettunen2026.blg`,
`kettunen2026.log`, `kettunen2026.out`, `kettunen2026.toc`,
`kettunen2026.pdf`) are ignored by git; only the sources are
tracked.

## What the article covers

- the net `N = (P, T, F, I, w, beta, ell, kappa)`: weighted ordinary and
  inhibitor arcs, place bounds, transition kinds and labels, initial, target
  and error markings, milestones, rare transitions and crash predictions;
- the firing and bound-violation rules, and the full and lazily explored
  reachability graph, which is the state space of the net;
- coverage, saturation and novelty;
- target phases, board classes and the off-target region, including the proof
  that the region is reward-inaccessible and closed under successors;
- the ant colony: attract, repel and off-target channels, the choice weight,
  deposits, back-propagation, evaporation, the T-suffix recurrence and the
  pair channels;
- the suggester's effective score and its coupling to the colony;
- abstract algorithms for lazy graph generation, one ant's walk and choice,
  pheromone deposition and evaporation, and event selection with and without
  the colony;
- an application chapter built from the journal's campaigns: the model ladder,
  the ATM campaign, the fork benchmark, the evaporation and run-length
  experiments, the rare hunt and the limits of the evidence;
- a table of default parameters and a symbol-to-code table.

The engineering history behind the definitions, including the rename to
`off-target`, is in `journal.md` sections 16, 36 and 38; the application
chapter follows the campaigns recorded in sections 13, 25, 27, 30 and 32, and
the import and initial-marking work is in section 41.
