# A formal account of the Petri nets and the ant colony

`main.tex` is a self-contained LaTeX article that defines, in mathematical
terms, the Petri nets, reachability graph, coverage and target machinery, the
off-target region, the ant-colony equations (including the T-suffix layer) and
the suggester score used by `eggembt` at version `v0.0.6`.  The definitions
follow the implementation; Section 6 maps every symbol to its source file.

## Build

A TeX distribution with `pdflatex` and `bibtex` is enough:

```sh
cd publication
pdflatex main.tex
bibtex main
pdflatex main.tex
pdflatex main.tex
```

The result is `main.pdf`.  With `latexmk` installed, one command does the same:

```sh
latexmk -pdf main.tex
```

The generated files (`main.aux`, `main.bbl`, `main.blg`, `main.log`,
`main.out`, `main.toc`, `main.pdf`) are ignored by git; only the sources are
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
- a table of default parameters and a symbol-to-code table.

The engineering history behind the definitions, including the rename to
`off-target`, is in `journal.md` sections 16 and 36.
