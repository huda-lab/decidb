# Quadratic Programming Test Coverage — Done

Tests live in:
- `test/decide/tests/test_quadratic.py` — QP/MIQP objectives
- `test/decide/tests/test_quadratic_constraints.py` — QCQP (quadratic constraints)

Covers `POWER(expr, 2)` via Q matrix construction. Convex forms run on both
solvers; non-convex and MIQP require Gurobi. Three syntax forms supported:
`POWER(expr, 2)`, `expr ** 2`, `(expr) * (expr)` self-product, plus negated
variants.

## Scenarios covered (objectives)

All oracle-verified; gurobi-gated cases noted:

- **Convex/concave core** (`test_quadratic.py`): `MINIMIZE SUM(POWER(x - target, 2))` (Q=PSD), `MAXIMIZE SUM(-POWER(x - target, 2))` (Q=NSD), simple squared variable, various coefficient forms, all three syntax forms (`POWER`, `** 2`, self-product), multiple variables, TPC-H data QP, QP objective over a keyed decision.
- **Non-convex** `MAXIMIZE SUM(POWER(x, 2))` — gurobi-gated.
- **MIQP** (integer vars + convex QP, `test_maximize_convex_power_integer_case_b`) — gurobi-gated.
- **Mixed linear + quadratic**: same SUM (`SUM(POWER(x-t,2) + c*x)`), sibling SUMs, and negated quadratic + linear.
- **Composition**: QP objective under a `when`; QP objective beside a `per` constraint (`test_per_interactions.py`).
- **Nested objectives**: `sum(per grp: sum(power(x - t, 2)) by (grp))` (binding per-group cap, unconstrained, and with no constant term), `sum(per grp: avg(power(x - t, 2)) by (grp))` with unequal groups (`test_quadratic.py`), each written as the objective's own sum (D75); an inner `max(power(...))` / `min` is refused by name (`test_per_objective.py`, D74).

## Scenarios covered (constraints — QCQP)

All oracle-verified, in `test_quadratic_constraints.py`:

- **Core shapes**: per-row `POWER(expr, 2) <= K` (gurobi-gated), aggregate `SUM(POWER(expr, 2)) <= K`, zero budget (exact match), binding vs non-binding constraints, multi-variable inner expression, negated `-POWER(expr, 2)`, scaled `K * POWER(expr, 2)`, data-dependent coefficients.
- **Composition**: quadratic constraint under a `when`, under `per` (one quadratic row per group), under `when` + `per` (the `when` filters before the classes form); multiple quadratic constraints per query; QCQP (quadratic objective + quadratic constraint); mixed linear + quadratic constraints; a keyed decision + QP; REAL and INT variables; bilinear + self-product mixed.
- Infeasible quadratic constraint (negative budget) — error test, not oracle-verified.

## Backend coverage

Every test above runs on the unforced fixture, which prefers Gurobi wherever it is
linked — so on a developer machine they only ever exercise the Gurobi path.
`TestHighsQuadratic` (`test_quadratic.py`) closes that gap for the one quadratic
class HiGHS accepts, a convex continuous QP: it pins the reported repro, a mixed
linear + quadratic objective, and a two-variable `POWER` group, each against
`oracle_solver` through `decidb_cli_highs`. Before it, nothing in the suite solved
a quadratic on HiGHS at all, which is how a HiGHS-only Hessian-convention defect
survived — it returned wrong answers with no error for as long as the backend has
existed.

A multi-variable `POWER` group is refused on HiGHS rather than solved, so the
cross-term case is a *rejection* test (`TestHighsRejection`) rather than a correctness
one. It is paired with `test_gurobi_solves_what_highs_refuses`, which runs the same SQL
through `decidb_cli_gurobi` and oracle-checks it: without that pair, a gate that was too
broad — refusing a class Gurobi also mishandles — would look identical from the HiGHS
side alone.

## Error cases

- Exponent rejections (`test_quadratic.py`): `POWER(x, 3)`, variable exponents.
- Multiple POWER groups in one objective rejected (`test_quadratic.py`).
- Degree > 2 rejections — self-product of POWER, product of two POWERs, variable × POWER — each tested in both objective (`test_quadratic.py`) and constraint (`test_quadratic_constraints.py`) positions.
- HiGHS rejects non-convex QP, MIQP, and a `POWER` group coupling two decision variables (`test_quadratic.py::TestHighsRejection` + `_expect_gurobi` pattern) and quadratic constraints (`test_quadratic_constraints.py`). Each is checked to refuse at plan time, with no model built.
- Infeasible quadratic constraint with tight `match=` (`test_infeasible_negative_budget`).

## Feature interactions covered

| Feature A | Feature B | Tested |
|-----------|-----------|--------|
| QP objective | `when` | ✓ |
| QP constraint | `when` | ✓ |
| QP constraint | `per` | ✓ |
| QP constraint | `when` + `per` | ✓ |
| QP objective | multiple variables | ✓ |
| QP constraint | multiple variables | ✓ |
| QP | BOOL (MIQP) | ✓ (gurobi-gated) |
| QP | INT (MIQP) | ✓ (gurobi-gated) |
| QP | REAL | ✓ |
| QP | keyed decision | ✓ |
| QP constraint | bilinear (mixed) | ✓ |
| QP objective | `per` constraint | ✓ |
| QP objective | linear terms in same SUM | ✓ |
| QP objective | linear terms in sibling SUM | ✓ |
| Negated QP objective | linear terms | ✓ |
| QP objective | nested, outer SUM over SUM/AVG | ✓ |
| QP objective | nested, inner MIN or MAX | refused (D74) |
