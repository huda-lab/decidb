# Known bugs

## Open

- **BUG-43 — `EXPLAIN` prints internal text for a group bound read from data.** A `per k:` reducer clause whose bound is
  a column gets a `↳` line with the rewritten bound as an expression tree, including a positional reference:
  `↳ per k: sum(x) by (k) <= CASE  WHEN ((k IS NULL)) THEN (NULL) ELSE #3 END`. `language/guide.md` § 9 says an auxiliary
  prints as the expression it stands for, never an internal name, and a bound derived from data prints as `M`. The same
  clause with a literal bound prints no such line. It appears in the box and JSON forms of `EXPLAIN`, and on both of the paper's
  Figure 1 reducer clauses, so the paper cannot quote that output as it stands.
  Repro: `EXPLAIN (FORMAT JSON) SELECT id, x DECIDE x(INT) FROM t SUCH THAT x <= 5 AND per k: SUM(x) by (k) <= cap
  MAXIMIZE SUM(x)` over `t(id INT, k VARCHAR, cap INT)`. Found 2026-11-02.
- **BUG-44 — A missing comma between declarations is reported as a missing `SUCH THAT`.** `DECIDE x(INT) y(INT) FROM t
  SUCH THAT ...` fails with `Parser Error: DECIDE requires a SUCH THAT clause; add SUCH THAT with at least one
  constraint`, though the query has that clause, and the caret points at the declaration rather than the second
  variable. With `x(INT), y(INT)` the query runs. The message comes from `select.y`. The paper's Figure 1 prints its
  declarations without the comma, so a reader copying it hits this message. Repro: `SELECT id, x, y DECIDE
  x(INT) y(INT) FROM t SUCH THAT x <= 5 AND y <= 5 MAXIMIZE SUM(x + y)`. Found 2026-11-02.
- **BUG-45 — A data-derived bound multiplied by a decision is repaired as if it were a literal.** `DIAGNOSE` should relax
  a user's literal before a bound derived from data. It does so for `x <= cap`, but `x <= cap * o` is repaired ahead of
  the literal, whichever clause is written first. Canonicalization moves `cap * o` to the left side, which leaves a
  literal 0 as the right side, so the row looks like a literal and joins the literal tier
  (`diagnostics/decide_diagnostic_engines.cpp`, `is_data_offset`). The finding still reports `virtual_offset`, the kind
  for a data-backed bound. A bound with a column in it should repair in the data tier, with or without a decision.
  The paper's Section 6 states that rule. Its Figure 3 should keep its repair, since 25 on the capacity bound is still
  less than 50 on demand; check this when fixing. Repro over `t(id INT, cap INT)` holding `(1, 60)`: `DIAGNOSE SELECT id,
  x, o DECIDE x(INT), o(BOOL) FROM t SUCH THAT x <= cap * o AND SUM(x) >= 100 MINIMIZE SUM(x)` suggests
  `x <= cap * o + 40`; with `x <= cap` it suggests `sum(x) >= 60`. Found 2026-11-02.

## Limitations (not bugs)

- **A repair exactly at the tolerance on HiGHS's QP path.** An edit whose rows miss a summed bound by exactly the
  feasibility tolerance (1e-6) passes the certificate's LP on both backends, and Gurobi solves the edited query; HiGHS's
  QP path rejects it. HiGHS judges the same violation differently in its LP and QP paths.
- **Gurobi's barrier QP at stage 2.** On a 9,000-row QP repair Gurobi returns a point that breaks a row by 5.7e-5 and an
  objective value 24 above the optimum HiGHS finds (225000010024 against 225000010000). Inside Gurobi.
- **HiGHS on the edited 9,000-row QP.** It reaches its 300 s limit where Gurobi solves it.

Resolved behavior is documented by its owning `done.md`.
