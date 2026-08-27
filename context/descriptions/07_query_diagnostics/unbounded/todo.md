# Query Diagnostics — Unbounded (remaining work)

The unbounded engine is shipped end to end: it reaches a clean `UNBOUNDED` state,
extracts a portable recession ray, names the escaping variables, reports direction, and
characterizes affected rows/entities with categorical rules. See `done.md` for how it
currently works. The engine's actionable core — name the runaway variable, prescribe the
bound — is complete; categorical slices are returned as separate typed findings, and
SELECT-only columns can be named from their user-written projection aliases. What remains
are two pending tasks plus a deferred item blocked on unrelated expressivity work.

---

## Pending

- **Dedupe equivalent categorical findings on narrow inputs.** `CharacterizeEscape`
  (`decide_diagnostic.cpp`) emits *every* group clearing the escape-rate threshold as its own
  finding. On a small input this floods: the candidate filter is
  `2 ≤ distinct ≤ max(min_categories, ratio × N)` with `min_categories` = 20
  (`physical_decide.cpp`), so on an N-row table with N ≲ 20 *every* column is categorical,
  and every singleton group holding an escaper scores rate 1.0. Measured on the paper's Figure 1
  data (3 joined rows, 9 columns, 2 escaping rows): **12 findings**, including
  `capacity = '300'`, `routeID = 'T2'`, `unit_cost = '6'`. That breaks the concision half of the
  user-facing-output principle — the actionable rule is buried in coincidences.
  **Change:** drop rules whose escaping-instance set is identical to an already-emitted rule's
  set, so perfectly-correlated columns collapse to one representative. Do **not** cap or truncate
  the relation: Batch H deliberately removed headline truncation, and every distinct finding
  remains queryable.
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

## Deferred

- **Downward escape (`edit_source='runaway_-inf'`).** Blocked until signed/free variables
  land (`03_expressivity/decide/todo.md`); today all user variables are non-negative, so
  downward escape is unreachable end to end. When they ship: open the lower bound in the
  ray-fallback box (`BuildUnboundedRayFallbackModel`, `diagnostic_solves.cpp`), replace the
  engine/renderer's mismatched direction strings with one shared representation (also tracked
  in `../../06_issues/code_quality/todo.md`), and assert an oracle-confirmed negative-ray
  finding.
