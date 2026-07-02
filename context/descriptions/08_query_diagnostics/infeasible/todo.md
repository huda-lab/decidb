# Query Diagnostics — Infeasible (remaining work)

The elastic engine is fully shipped (I1–I5, plus aggregate `<>` removal, PER-group
identity, and uncorrelated scalar-subquery RHS classification; see `done.md`).

## Output polish

- **Default stderr wording and labels are still rough.** The engine emits correct
  diagnoses, but some default output is awkward for SQL users, especially grouped/PER
  subjects where the subject text and `group` attribute can duplicate context, and
  multi-edit summaries that read as a flat list of equally minimal alternatives.
  Tighten `BuildInfeasibleDiagnostic` and the label/provenance formatting so the
  default error is concise without losing the richer relation rows.
- **Headline enumerates every failing PER group (no cap).** With dozens of failing
  groups the "grouped clause … for groups …" phrase lists them all inline (59 keys on
  `SUM(x) >= 5 PER l_orderkey` over 400 TPC-H orders). Cap the headline list (first K +
  "and N more"); the relation already carries the full per-group detail. Logged in
  `07_issues/code_quality/todo.md`; `xfail` at
  `test_query_diagnostics_tpch.py::TestKnownGaps::test_D2_many_group_headline_should_be_capped`.
- **`drop` edit duplicated per per-row `<>` expansion.** A single `x <> 1` over an order
  with N line items emits N identical `drop` rows. Dedupe by `(clause_id, indicator label)`.
  Logged in `07_issues/code_quality/todo.md`; `xfail` at
  `test_query_diagnostics_tpch.py::TestKnownGaps::test_G_drop_edit_should_be_deduplicated`.

## Notes to revisit

> **Status: both blocked on a triggering test — not implemented.** Neither can be "cleared"
> by building, because the proper fix is the same bigger refactor (a true lexicographic ladder
> replacing the fixed `1 / 1e3 / 1e6` weights) and there is no failing oracle case yet to
> validate it against. Revisit when a scale-mixed case actually misorders the fixes.
>
> **Update (2026-07-02): a triggering case now exists.** On real TPC-H data, a tight dollar
> budget vs. a count floor (`SUM(buy) >= 30 AND SUM(buy*l_extendedprice) <= 100`) makes the
> uniform-weight L1 race gut the count floor to `SUM(buy) >= 0` — a degenerate "select
> nothing" edit (`achievable_objective = 0`). Captured as an `xfail` in
> `test/decide/tests/test_query_diagnostics_tpch.py::TestKnownGaps::test_E_loosen_should_not_be_degenerate`
> and logged in `07_issues/bugs/todo.md`. This is the scale-mixed case the note was waiting for.

- **Slack weights are uniform among editable knobs (`wᵢ = 1`); data-RHS slacks are penalized
  (`DIAGNOSTIC_DATA_SLACK_WEIGHT`).** With mixed units the L1 race can prefer loosening the
  large-scale constraint. Fix is scale-normalized weights (by RHS magnitude / row-coefficient
  norm); deferred until a test exposes the skew.
- **Weighted preference ladder is a fixed-constant stand-in, not lexicographic.** Two weights
  encode the ladder (editable `1` < data `1e3` < removal `1e6`). A true lexicographic ladder
  (drop the weights, run stage 1 in successive passes) is the proper fix for both at once.
  Revisit if a scale-mixed oracle case misorders the fixes.
