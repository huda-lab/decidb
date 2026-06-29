# Query Diagnostics — Infeasible (remaining work)

The elastic engine is fully shipped (I1–I5, plus aggregate `<>` removal; see `done.md`). Two
deferred shapes remain.

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

## I2 follow-up · PER-group identity in the relation (unblocks SUM fold for PER)

An ungrouped `SUM(x) >= K` now renders the clause as `SUM(x)` (`FormatSumLhs`, see `done.md`
"I2.d — SUM"). A **PER-grouped** aggregate (`SUM(x) >= K PER grp`) is deliberately left
expanded (`x + x`, `x + x + x`): folding both groups to `SUM(x)` would make them
indistinguishable in the relation, because today the differing row counts are the *only* thing
that tells one group's edit from another's (`subject` is the only key). The fold is gated on
`provenance.group_key == INVALID` in `FormatLhs` precisely to preserve that.

```sql
SELECT id, x
FROM (VALUES (1,'a'),(2,'a'),(3,'b'),(4,'b'),(5,'b')) t(id, grp)
DECIDE x IS BOOLEAN
SUCH THAT SUM(x) >= 5 PER grp
MAXIMIZE SUM(x);
```

**Cause.** The edit's `subject` carries only the clause text; the group is identified by an
opaque ordinal (`provenance.group_key = g`), and the group's printable key value (`'a'`/`'b'`)
is never threaded out of the physical operator into the model.

**Fix (no change to PER solve logic — purely an additive label channel, mirroring the
`<>` `global_variable_labels` plumbing).** Surface the group value as its own field so the fold
can apply to PER too:
- **`BuildPerGroupMapping`** (`physical_decide.cpp`) already has `BuildGroupIds`'s `&rep_keys`
  out-param available (used for categorical rules at the `BuildRowGrouping` site) but does not
  request it. Capture the representative `Value` per group and store a `group_value` on the
  cache entry. **Subtle:** the post-WHEN remap to consecutive `0..K'` (the `remap` loop) must
  reindex the rep_keys too, or labels misalign with the ordinals.
- Thread `group_key → label` from the grouping through `SolverInput` to provenance (stamped at
  `ilp_model_builder.cpp` where `group_key = g`), parallel to `global_variable_labels`.
- In the diagnosis, emit the value as a new `attribute='group'` EAV row (keeps `subject` = the
  clause text), then drop the `group_key == INVALID` gate in `FormatLhs` so PER aggregates fold
  to `SUM(x)` as well.

**Two more facets this work should close (same root cause — grouped aggregate rows lose their
clause identity):**
- **Lost `SUM(...)` wrapper on a single-row / WHEN group.** `SUM(x) >= 99 WHEN g='a'` where the
  group has one row renders `x >= 99` — both the per-row fan-out the renderer keys on AND the
  `group_key == INVALID` gate are absent, so neither the SUM fold nor the wrapper fires. An
  `is_aggregate` provenance flag (set at the aggregate emission sites in `ilp_model_builder.cpp`,
  distinct from `SHARED_LITERAL`, which a per-row constant bound also carries) lets `FormatLhs`
  wrap the reconstruction in `SUM(...)` regardless of group size. Fold safely once a clause is
  known to contribute a single group to the edit set (no sibling to collide with).
- **WHEN/PER qualifier dropped from the label.** The label never shows the `WHEN g='a'` predicate
  or the `PER` key, so the clause isn't fully recognizable. The qualifier text isn't threaded
  today (labels are reconstructed from the matrix) — it rides the same clause-context channel as
  the group value.

**Edge cases.** Composite keys (`PER region, year`) → `rep_keys` is `vector<vector<Value>>`,
render a tuple (`region=EU, year=2024`); WHEN+PER remap alignment (above); entity-scoped + PER;
serialization (if the label channel rides the logical op / `SolverInput` like
`aux_var_expressions` does).

## Notes to revisit

- **Slack weights are uniform among editable knobs (`wᵢ = 1`); data-RHS slacks are penalized
  (`DIAGNOSTIC_DATA_SLACK_WEIGHT`).** With mixed units the L1 race can prefer loosening the
  large-scale constraint. Fix is scale-normalized weights (by RHS magnitude / row-coefficient
  norm); deferred until a test exposes the skew.
- **Weighted preference ladder is a fixed-constant stand-in, not lexicographic.** Two weights
  encode the ladder (editable `1` < data `1e3` < removal `1e6`). A true lexicographic ladder
  (drop the weights, run stage 1 in successive passes) is the proper fix for both at once.
  Revisit if a scale-mixed oracle case misorders the fixes.
