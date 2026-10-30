# Known bugs

- **BUG-42 — A `CASE` in an aggregate clause's bound gets a message that does not say `CASE`.** `CASE` is refused
  everywhere in DECIDE (`language/guide.md` § Data operands), with a message that names it and the alternatives
  (`DecideCaseUnsupportedMessage`). In the bound of an aggregate clause the refusal comes instead from the bound's
  allow-list (`IsAllowedDecisionFreeBoundExpression`), whose `default: return false` drops the reason: "SUM cannot be
  compared to an expression that is not a scalar or aggregate without DECIDE variables", even for `CASE WHEN true`.
  The refusal is written once per binder path (constraints, reducer argument, objective), so the path that reaches a
  `CASE` first decides the text. Repro: `SELECT id, x FROM t DECIDE x(BOOL) SUCH THAT SUM(x) <= CASE WHEN s = 'F'
  THEN 1 ELSE 2 END MAXIMIZE SUM(p*x)` over `t(id, s VARCHAR, p DOUBLE)`; `x <= CASE ...` names `CASE`. Found
  2026-10-30 (lane AV, PERF-20).

## Limitations (not bugs)

- **A repair exactly at the tolerance on HiGHS's QP path.** An edit whose rows miss a summed bound by exactly the
  feasibility tolerance (1e-6) passes the certificate's LP on both backends, and Gurobi solves the edited query; HiGHS's
  QP path rejects it. HiGHS judges the same violation differently in its LP and QP paths.
- **Gurobi's barrier QP at stage 2.** On a 9,000-row QP repair Gurobi returns a point that breaks a row by 5.7e-5 and an
  objective value 24 above the optimum HiGHS finds (225000010024 against 225000010000). Inside Gurobi.
- **HiGHS on the edited 9,000-row QP.** It reaches its 300 s limit where Gurobi solves it.

Resolved behavior is documented by its owning `done.md`.
