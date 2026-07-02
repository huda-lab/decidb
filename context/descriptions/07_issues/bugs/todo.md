# Known Bugs — Open

Bugs discovered but not yet fixed. Each entry: symptom, reproduction, what is known about the cause, what has been ruled out, and where to look next.

Resolved bugs are moved to `done.md`.

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
