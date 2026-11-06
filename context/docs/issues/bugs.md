# Known bugs

## Open

- **BUG-51 — A bad value under `ABS` is worded as a bound the user never wrote.** On the solver path, a NaN value in
  `SUM(ABS(c - v))` raises `DECIDE: the bound -v is NaN on some rows. Skip those rows with 'when NOT isnan(-v):'.`,
  and an infinite one raises `DECIDE optimization is infeasible.` Both come from the Big-M envelope `RewriteAbs`
  builds (`decide_rewrite_abs.cpp`): the value becomes a bound on the helper, and the bound check words it. Every other
  value error names the value as written (`v is NaN or infinite on some rows`, `decide_value_messages.hpp`), which is
  what direct solve's A2 raises for the same queries. Repro: `FROM (VALUES (1, 2.0::DOUBLE), (2, 'NaN'::DOUBLE)) t(id,
  v) DECIDE x(REAL) SUCH THAT x BETWEEN 0 AND 10 MINIMIZE SUM(ABS(x - v))` under `off`; `'Infinity'` for the second.
  Found 2026-11-07 (A2-5). Fix: check the value where the term is read, before the rewrite turns it into a bound.
- **BUG-52 — A sum's own `when` does not keep `ABS` from reading a dropped row.** `MINIMIZE SUM(when f: ABS(c - v))`
  with `v = NaN` on a row `f` drops raises BUG-51's error under `off`, though nothing is computed on a row a `when`
  drops (syntax reference §6.3); the objective-level `MINIMIZE when f: SUM(ABS(c - v))` answers. `MAX(when f:
  ABS(c - v))` does the same. Repro: `FROM (VALUES (1, 2.0::DOUBLE, true), (2, 'NaN'::DOUBLE, false)) t(id, v, f)
  DECIDE per (): c(REAL) SUCH THAT c BETWEEN 0 AND 10 MINIMIZE SUM(when f: ABS(c - v))` under `off`. Direct solve's A2
  answers it. Found 2026-11-07 (A2-5). Fix: the envelope's bounds carry the reducer's own `when`, as its objective
  term does.

## Limitations (not bugs)

- **Gurobi's non-convex tolerance.** On `SUM(POWER(x, 2)) BY (k) <= 25 MAXIMIZE SUM(POWER(x, 2))`, with optimum 50,
  Gurobi returns 50.0000001 and reports it optimal at tolerance 0. Inside Gurobi's spatial branching.
- **A repair exactly at the tolerance on HiGHS's QP path.** An edit whose rows miss a summed bound by exactly the
  feasibility tolerance (1e-6) passes the certificate's LP on both backends, and Gurobi solves the edited query; HiGHS's
  QP path rejects it. HiGHS judges the same violation differently in its LP and QP paths.
- **Gurobi's barrier QP at stage 2.** On a 9,000-row QP repair Gurobi returns a point that breaks a row by 5.7e-5 and an
  objective value 24 above the optimum HiGHS finds (225000010024 against 225000010000). Inside Gurobi.
- **HiGHS on the edited 9,000-row QP.** It reaches its 300 s limit where Gurobi solves it.
- **Gurobi's barrier on many limits on squares.** With a redundant bound on the decision, a feasible, well-scaled query
  with one `SUM(POWER(x, 2))` limit per group can stop with Gurobi's numerical-trouble status and no solution, at
  Gurobi's default settings. With `x <= 50` and a limit of 25, 7 of 20 random inputs fail on 2,000 rows in 100 groups
  and 9 of 20 on 20,000 rows in 1,000 groups. No Gurobi setting tried recovers them. The query raises `DECIDE
  optimization failed: Gurobi could not solve this model (numerical trouble).` Inside Gurobi.
- **Gurobi at an objective constant of about 1e200 and above.** `SELECT id, x FROM (VALUES (1, 1e100)) t(id, v) DECIDE
  x(REAL) SUCH THAT x BETWEEN 0 AND 2e100 MINIMIZE SUM(POWER(x - v, 2))` prints the number-range warning, then
  `DECIDE optimization is unbounded`, on a model whose every decision is bounded. Gurobi returns an unbounded status
  for the expanded square's constant `v^2`; HiGHS refuses to load it ("numbers too far apart in size"). Inside the
  backends; direct solve answers `x = v`.

Resolved behavior is documented by its owning `done.md`.
