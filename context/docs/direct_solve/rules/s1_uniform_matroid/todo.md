# S1 (uniform-matroid optimization) — open work

What S1 does today is in `done.md`; the class is in `definition.md`. How to work and the checks are in
`../../framework/todo.md` § How to work any task. Every widening also meets the exit gate at the end of this file.

## Exit gate for any S1 widening

An independent proof, behavior tests (the standard tests in `../../architecture/rules.md`), and end-to-end measurement that
does not count timeouts, solver limits or unsupported cases as wins. Finding a relational expression in the catalogue is
not enough.

## BLOAT-21 — S1 grid cases that share one plan

- **Problem.** Grouping S1's 497 oracle cases by identical `EXPLAIN` plan (under `require`, same dataset) gives 384
  distinct plans: about 40 groups of 2 to 16 cases. The largest are the `x_*_when` and `x_*_everywhere` row-bound
  grids, the `no_objective_*` fraction, whole and `gt` shapes per scope, and the `offset_-1_0_*` operator-by-delta
  grid. One plan does not prove one branch (`x_ne_0_when` and `x_eq_1_when` share a plan through different `allows`
  paths), and the near-miss shapes `x>1`, `x>1.5`, `x>2` run through shared code (`DecideWrittenBox::ContradictsDomain`,
  `DecideFoldSingletonRow`).
- **Change.** From a coverage run, delete each case that reaches no arm another case reaches (`.claude/CLAUDE.md`
  § Testing).
- **Done when.** `coverage_report.py compare` shows no arm lost, and the matrix passes.
