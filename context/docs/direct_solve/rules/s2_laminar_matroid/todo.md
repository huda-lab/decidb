# S2 (laminar-matroid optimization) — open work

S2 selects the best `BOOL` items under upper count quotas whose groups nest (departments inside divisions inside one
global cap). The class is in `definition.md`. How to work is in `../../framework/todo.md` § How to work any task; the
standard tests are in `../../architecture/rules.md` § The standard tests every rule ships. The exit gate of
`../s1_uniform_matroid/todo.md` holds here too.

## S2-4 — Caps read from the data

- **Problem.** S2 declines a quota whose cap is read from the data (`SUM(x) by (g) <= MAX(cap)`, a source column):
  the group-bound reading S1 uses (the bound's aggregates in W1, its values and messages in PL, the rounding per row)
  is wired inside S1 (`BoundValue`, `SourceLimit`, its verdict checks), and the solver path answers such queries.
- **Change.** Move S1's group-bound cap reading into shared code beside `DirectInclusiveRowsExpr`, read once per level
  by S2 (each level registers its bounds with the builder) and by S1. Answers, error texts and S1's plans stay as they
  are.
- **Done when.** Oracle tests for a data cap at the finer level, at the coarser level and at both, with NaN and NULL
  caps; `./test/decide/run_tests.sh -n 2 -k "direct_s1 or direct_s2"`; `build/release/test/unittest "[decidb]"`.
