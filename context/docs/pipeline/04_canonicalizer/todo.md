# Stage 04 — Canonicalizer: open work

Open work in the canonical forms and the stages that read them. Finished work is in [done.md](done.md); a rewrite
belongs in this stage only when it meets [done.md](done.md) § What a rule must be. How to work and the checks:
[`../../direct_solve/framework/todo.md`](../../direct_solve/framework/todo.md) § How to work any task in these todo
files.

## CANON-1 — A SUM or AVG keeps its data addend where its route puts it

- **Problem.** The two routes of a reducer put a data addend in different places, so the integer-step check has two
  sites and two answers. `sum(x + 0.5) > 4` solves (the aggregate row moves `0.5 * n` to the bound), while `sum(x +
  0.5) + x > 4` is refused (`'>' compares 'sum(x + 0.5)', which can take fractional values`) because the value
  route's helper holds the constant. Stage 04 checks the canonical left side (`DecideShapeCheck::CheckIntegerStep`);
  stage 05 keeps its own check for the helper (`RefuseFractionalStep`, `decide_rewrite_scoped.cpp`). MIN/MAX already
  moves a constant addend out (`DecomposeExtremumAddends`).
- **Change.** Stage 04 splits a SUM or AVG body's data addend out by linearity, constant or per-row, with the
  reducer's `when` and `by`: `sum(x + 0.5)` is `sum(x) + sum(0.5)`, `avg(x + d)` is `avg(x) + avg(d)`. Both routes then
  read one canonical form, the stage-04 check holds for both, and `RefuseFractionalStep` goes. It does not fit
  `DecomposeExtremumAddends` as it stands: that moves only foldable addends, which cannot be NULL, while a per-row
  addend has to carry its NULL tests (`SkipNullData` runs after canonicalization) and the moved data reducer enters the
  bound-reducer machinery (`RewriteRhsAggregates`).
- **Ruling (provisional, god for the owner, 2026-11-21).** No query that solves today changes its answer; model rows
  move only for bodies with a data addend; an empty group or a NULL addend keeps its result. No list of addend shapes.
- **Done when.** `sum(x + 0.5) + x > 4` agrees with the oracle; `grep -rn RefuseFractionalStep src` finds nothing;
  `./test/decide/run_tests.sh -n 2 -k "integer or fraction or canonical or scoped"` passes and the golden diff moves
  only bodies with a data addend.
