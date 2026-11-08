# A2 (single-facility location) — open work

A2 picks one shared value for many rows: one `per ():` or `per K:` decision placed at the best center of the rows'
observations. The class is in [definition.md](definition.md). How to work and the checks are in
`../../framework/todo.md` § How to work any task; a new rule follows `../../architecture/rules.md` § Adding a rule.

## A2-12 — The test grids reach the same branches many times

- **Problem.** A2 has 1,172 tests; N1 has 143 and A1 196. The absolute loss alone has 530. The grids are full cross
  products: both centered losses times 10 row sets times 6 intervals, every spelling on three row sets, every weight
  set in every interval. `test_a2_a_weighted_class_answers_the_ruled_point` and
  `test_a2_an_absolute_class_answers_the_ruled_point` run every case of the oracle tables a second time to check the
  tie point. The repository's rule is that a grid case reaches a branch no other case reaches
  (`.claude/CLAUDE.md` § Testing).
- **Change.** For each grid, name the branches of the plan it is for (the lower and the upper median, a clamp up, down
  or none, the nearest-0 tie, no kept row, a NULL value, a point interval; each spelling once for the IR reading) and
  keep one case per branch, named for it. Check the ruled point inside the oracle test, on the same run, and delete
  the separate ruled-point tests.
- **Needs.** H-48, which changes the branches.
- **Done when.** Every remaining case names the branch it reaches, removing that branch fails it, and the matrix
  passes.
