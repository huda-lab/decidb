# Query Diagnostics — Infeasible (remaining work)

The elastic engine is fully shipped (I1–I5, plus aggregate `<>` removal; see `done.md`). One
deferred shape remains.

## I2 follow-up · uncorrelated scalar-subquery RHS

A per-row bound whose RHS is an uncorrelated scalar subquery (`x <= (SELECT 5)`) is solved
correctly by the main solver, but infeasible diagnosis treats the RHS as per-row data — so it
reports a data conflict instead of an editable shared cap (`Loosen x <= 5 to x <= 10`).

```sql
SELECT id, x
FROM (VALUES (1,10)) t(id, lo)
DECIDE x IS REAL
SUCH THAT x <= (SELECT 5) AND x >= lo
MAXIMIZE SUM(x);
```

**Cause.** Shared-literal classification uses `Expression::IsFoldable()` in the
`physical_decide.cpp` RHS-evaluation block, which is false for a subquery. The optimizer
flattens `(SELECT 5)` into a join, so the RHS arrives as a column reference, structurally
indistinguishable from row data. Blindly tagging it shared would be wrong for correlated
subqueries (legitimately row-varying).

**Fix.** Detect row-invariant flattened subquery RHS by structure (uncorrelated by
construction) and classify as `SHARED_LITERAL` / a shared-scalar shape; keep correlated
subqueries as `PER_ROW_DATA`.

## Notes to revisit

- **Slack weights are uniform among editable knobs (`wᵢ = 1`); data-RHS slacks are penalized
  (`DIAGNOSTIC_DATA_SLACK_WEIGHT`).** With mixed units the L1 race can prefer loosening the
  large-scale constraint. Fix is scale-normalized weights (by RHS magnitude / row-coefficient
  norm); deferred until a test exposes the skew.
- **Weighted preference ladder is a fixed-constant stand-in, not lexicographic.** Two weights
  encode the ladder (editable `1` < data `1e3` < removal `1e6`). A true lexicographic ladder
  (drop the weights, run stage 1 in successive passes) is the proper fix for both at once.
  Revisit if a scale-mixed oracle case misorders the fixes.
