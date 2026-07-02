# Known Bugs — Open

Bugs discovered but not yet fixed. Each entry: symptom, reproduction, what is known about the cause, what has been ruled out, and where to look next.

Resolved bugs are moved to `done.md`.

---

## SUCH THAT rejects composite (multi-column) PER

- **Location**: `src/planner/expression_binder/decide_constraints_binder.cpp` (PER binding)
- **Discovered**: 2026-07-01 while adding PER-group identity to infeasible diagnostics
- **Symptom**: `SUCH THAT SUM(x) >= 1 PER region, yr` fails to bind with `Binder Error: SUCH THAT clause does not support 'yr'(ExpressionClass::COLUMN_REF)`, even for a feasible query. Single-column PER works.
- **Reproduction**:
  ```sql
  SELECT id, x FROM (VALUES (1,'EU',2024),(2,'US',2025)) t(id, region, yr)
  DECIDE x IS BOOLEAN
  SUCH THAT SUM(x) >= 1 PER region, yr
  MAXIMIZE SUM(x);
  ```
- **Cause**: The constraint binder only consumes the first PER column. (The infeasible-diagnosis group-label path already renders composite keys as a `, `-joined tuple, so this is purely a binder gap.)
- **Where to look next**: Decide whether composite PER in `SUCH THAT` is in scope; if so, extend the constraints binder to accept the trailing PER columns the way the objective/symbolic path does.

---

## Unbounded characterization ignores join-partner columns (entity-scoped)

- **Location**: unbounded escaping-instance characterization (`src/decidb/utility/decide_diagnostic.cpp` categorical-group logic + the key-column harvest in `physical_decide.cpp`)
- **Discovered**: 2026-07-02 while stress-testing diagnostics on real TPC-H joins
- **Symptom**: For an entity-scoped decision variable whose escaping slice is perfectly characterized by a column from a **joined** table, `decide_diagnostics()` falls back to the bare count (`5 of 100 entities`) instead of naming the slice (`... where n_name = 'GERMANY'`). Row-scoped variables characterized by their *own* table's column work fine (case A: `36 of 36 rows where l_shipmode = 'AIR'`).
- **Reproduction**:
  ```sql
  PRAGMA diagnose_decide='auto';
  SELECT s.s_suppkey, keep
  FROM supplier s JOIN nation n ON s.s_nationkey = n.n_nationkey
  DECIDE s.keep IS REAL
  SUCH THAT keep <= 50 WHEN n.n_name <> 'GERMANY'
  MAXIMIZE SUM(keep);           -- GERMANY suppliers escape; n_name is never named
  SELECT * FROM decide_diagnostics();
  ```
- **Ruled out**: not the categorical-ratio guard — raising `PRAGMA diagnose_decide_categorical_ratio=0.3` (n_name is 25 values / 100 entities = 0.25) still does not surface `n_name`. Entity characterization only harvests columns from the entity's *own* source table.
- **Why it matters**: on real schemas the discriminating attribute usually lives on a dimension table reached by a join (nation, part, orders…), so the most useful characterization is exactly the one currently dropped.
- **Where to look next**: extend the entity key-column harvest to include join-partner columns referenced in WHEN/objective, or map the join predicate back to the entity's own FK column for characterization.

---

## Infeasible least-change can report a degenerate "require nothing" edit

- **Location**: elastic engine slack weighting (`src/decidb/utility/decide_diagnostic_engines.cpp`, stage-1 `min Σ wᵢ sᵢ`, uniform `wᵢ = 1`)
- **Discovered**: 2026-07-02 while stress-testing diagnostics on real TPC-H data
- **Symptom**: with two constraints in **incomparable units**, the engine loosens the small-magnitude one to a degenerate bound rather than the constraint that is actually tight. A tight dollar budget vs. a count floor yields `loosen SUM(buy) >= 30 to SUM(buy) >= 0` with `achievable_objective = 0` — i.e. "select nothing," which is feasible but useless advice.
- **Reproduction**:
  ```sql
  PRAGMA diagnose_decide='auto';
  SELECT l_orderkey, buy FROM lineitem WHERE l_orderkey <= 100
  DECIDE buy IS BOOLEAN
  SUCH THAT SUM(buy) >= 30 AND SUM(buy * l_extendedprice) <= 100
  MAXIMIZE SUM(buy);
  SELECT * FROM decide_diagnostics();
  ```
- **Cause**: uniform slack weights make total loosening scale-dependent — 30 count-units looks "cheaper" than the thousands of dollar-units needed to relax the budget, so the count floor is gutted. The two slacks are summed despite incomparable units.
- **Why it matters**: violates the "actionable least-change" promise — the user is told to drop their real requirement to zero instead of being pointed at the genuinely binding budget.
- **Where to look next**: normalize slack weights by constraint scale (e.g. per-row coefficient magnitude / RHS), and/or surface the alternative edit that preserves a nonzero objective rather than only the min-total-slack one.
