# Query Diagnostics — Unbounded (remaining work)

The actionable engine is complete. Two output-quality tasks remain; current
behavior is recorded in `done.md`.

---

## Pending

- **Dedupe equivalent categorical findings on narrow inputs.** `CharacterizeEscape`
  (`decide_diagnostic.cpp`) emits *every* group clearing the escape-rate threshold as its own
  finding. On a small input this floods: the candidate filter is
  `2 ≤ distinct ≤ max(min_categories, ratio × N)` with `min_categories` = 20
  (`physical_decide.cpp`), so on an N-row table with N ≲ 20 *every* column is categorical,
  and every singleton group holding an escaper scores rate 1.0. Re-measured 2026-08-27 on the paper's
  Figure 1 data (3 joined rows, 9 columns, 2 escaping rows): **4 findings** —
  `capacity = '300'` (1/1), `capacity = '350'` (1/1), `priority = 'standard'` (2/2),
  `unit_cost = '3'` (1/1). The original filing recorded 12; the unnamed-column
  suppression added since has narrowed the candidate set to the columns the DECIDE
  clause references, which is also why `routeID` and `demand` no longer appear. That breaks the concision half of the
  user-facing-output principle — the actionable rule is buried in coincidences.
  **Change:** drop rules whose escaping-instance set is identical to an already-emitted rule's
  set, so perfectly-correlated columns collapse to one representative. Do **not** cap or truncate
  the relation; every distinct finding must remain queryable.
  **Decision needed from the user:** which representative wins when several columns partition
  the escaping rows *identically* (on Figure 1, `priority='standard'`, `regionID='R2'` and
  `demand='600'` are indistinguishable). Candidate tie-break: prefer a column the DECIDE clause
  itself references (WHEN / PER / constraint / objective) over one that only rides along in
  `SELECT *`. It does not fully disambiguate — all three of those appear on line 9 — so a
  deterministic fallback is still needed. Do not pick silently.
  **Interaction:** this must land before or with the scoped-repair task below — a scoped repair
  built from an unranked rule list would cap on a coincidental column like `unit_cost = '6'`.
  **Test:** extend `test/decide/tests/test_query_diagnostics_escaping_instances.py` with a
  narrow-table case (≤ 5 rows, many columns) asserting equivalent slices collapse and that the
  query-referenced column survives; keep an existing wide-table case green to show distinct
  findings are not truncated.
  **Done file:** merge into `done.md` → "Escape characterization". Discovered
  2026-08-04 while checking the paper's §4 unbounded example against Figure 1's data.

- **Scope the prescribed bound to the escaping rows.** (Not paper-blocking: §4's unbounded
  example was moved to a *total*-escape edit, where the global repair is already correct. This
  gap bites only when escape is partial, which is the common case on real data.) Today the remedy is a
  global `suggested_change` (`x <= <cap>`) on each finding, while `group` may report that
  only a slice escapes (`priority = 'standard'`, with the escaping count in `amount`). The
  finding is scoped and the repair is not, so the user must turn the characterization into a
  conditional edit themselves — and the global cap also constrains rows that were already bounded.
  **Change:** when the categorical rules cover *every* escaping instance of a variable, render
  the remedy as `x <= <cap> when <col> = '<val>'` (DeciQL text the user pastes as a new
  `such that` conjunct) instead of the bare global form.
  **Decision:** the scoped form is emitted **only at full coverage** — every escaping instance
  falls under a reported rule and the rule's rate is 1.0. Partial coverage keeps the global
  form: capping extra non-escaping rows is safe (over-restriction), but missing an escaper
  outside the rule leaves the program unbounded. Multi-rule unions render as the disjunction
  the rules already form.
  **Stays inside the load-bearing limit:** this is a scoped *addition*, not blame on the
  clause whose `WHEN` excluded those rows — the ray cannot finger a guilty clause (see
  `done.md`, "names a variable, not a guilty clause"). Do not phrase it as "clause N's `WHEN`
  is too narrow."
  **Pointers:** rule computation `CharacterizeEscape`; formatter `BuildUnboundedDiagnostic`
  (`decide_diagnostic.cpp`); the coverage flag has to come out of `CharacterizeEscape`
  alongside the rules, since the finding builder cannot recover it from the flattened fields.
  **Test:** extend `test/decide/tests/test_query_diagnostics_escaping_instances.py` — full
  coverage yields the scoped remedy, a scattered/sub-threshold escape keeps the global one, and
  the scoped repair pasted back into the query makes the solve bounded (oracle-checked).
  **Done file:** merge into `done.md` under "The output relation" + "Escape characterization".
  Discovered 2026-08-04 while checking the paper's §4 unbounded example, whose
  reported repair did not map back to the query.

- **Categorical group names differ by source kind, and can be positional.**
  `BuildRowGrouping` (`physical_decide.cpp`) suppresses rules over columns whose source
  name was never resolved, precisely so a finding never names a positional `colN` the
  user did not type. It still happens. The same logical query gives different findings
  depending on whether the rows come from a `VALUES` list or a real table:

  ```sql
  -- over (VALUES ('a','red',5), …) t(tag, colour, cap)
  buy <= <cap>   1/1   row   col0 = 'c'
  buy <= <cap>   2/2   row   col1 = 'blue'
  buy <= <cap>   2/2   row   cap = '0'

  -- over a CREATE TABLE items(tag, colour, region, cap) with the same rows
  buy <= <cap>   2/2   row   cap = '0'
  ```

  Over `VALUES`, `tag` and `colour` reach the back-fill in
  `plan_decide.cpp:182-186` carrying the derived table's internal `col0` / `col1`
  rather than the user's aliases; over a real table they carry no name at all and are
  dropped. Only `cap`, referenced in the DECIDE clause itself, is named correctly in
  both. **Note for the dedupe/tie-break work above**: the candidate set is in practice
  the DECIDE-clause-referenced columns, so the tie-break originally sketched there
  ("prefer a column the DECIDE clause references") cannot discriminate — every
  candidate already is one.

  **Change**: make the back-fill resolve the user's alias for a derived-table column,
  or drop the column as unnamed, so the two sources agree. Decide which, then apply it
  before ranking rules, since the ranking reads these names.

  **Test**: the same rows over both a `VALUES` list and a table produce the same
  findings, and no finding names a `colN`.
  **Done file:** merge into `done.md` → "Escape characterization".
  **Discovered**: 2026-08-27, while measuring the finding flood on Figure 1 data.
