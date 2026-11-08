# Stage 05 — Optimizer: open work

Open work in the optimizer passes. Finished work is in [done.md](done.md).

## BUG-51 — A bad value under `ABS` is worded as a bound the user never wrote

- **Problem.** On the solver path, a NaN value in `SUM(ABS(c - v))` raises `DECIDE: the bound -v is NaN on some rows.
  Skip those rows with 'when NOT isnan(-v):'.`, and an infinite one raises `DECIDE optimization is infeasible.` Both
  come from the Big-M envelope `RewriteAbs` builds (`decide_rewrite_abs.cpp`): the value becomes a bound on the helper,
  and the bound check words it. Every other value error names the value as written (`v is NaN or infinite on some
  rows`, `decide_value_messages.hpp`), which is what direct solve's A2 raises for the same queries. Repro:
  `FROM (VALUES (1, 2.0::DOUBLE), (2, 'NaN'::DOUBLE)) t(id, v) DECIDE x(REAL) SUCH THAT x BETWEEN 0 AND 10
  MINIMIZE SUM(ABS(x - v))` under `decide_direct_solve='off'`; `'Infinity'` for the second.
- **Change.** Check the value where the term is read, before the rewrite turns it into a bound.
- **Done when.** Both repros raise the value error as written on both backends, and a test pins each text.

## BUG-52 — A sum's own `when` does not keep `ABS` from reading a dropped row

- **Problem.** `MINIMIZE SUM(when f: ABS(c - v))` with `v = NaN` on a row `f` drops raises BUG-51's error under
  `decide_direct_solve='off'`, though nothing is computed on a row a `when` drops (syntax reference §6.3); the
  objective-level `MINIMIZE when f: SUM(ABS(c - v))` answers. `MAX(when f: ABS(c - v))` does the same. Repro:
  `FROM (VALUES (1, 2.0::DOUBLE, true), (2, 'NaN'::DOUBLE, false)) t(id, v, f) DECIDE per (): c(REAL) SUCH THAT c
  BETWEEN 0 AND 10 MINIMIZE SUM(when f: ABS(c - v))`. Direct solve's A2 answers it.
- **Change.** The envelope's bounds carry the reducer's own `when`, as its objective term does.
- **Done when.** The repro and its `MAX` form answer on both backends with the answer direct solve gives, and the A2
  cases that run on direct solve alone (`ABS_DIRECT_ONLY_CASES`) move to the oracle tables.
