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

## Notes to revisit

> **Status: both blocked on a triggering test — not implemented.** Neither can be "cleared"
> by building, because the proper fix is the same bigger refactor (a true lexicographic ladder
> replacing the fixed `1 / 1e3 / 1e6` weights) and there is no failing oracle case yet to
> validate it against. Revisit when a scale-mixed case actually misorders the fixes.

- **Slack weights are uniform among editable knobs (`wᵢ = 1`); data-RHS slacks are penalized
  (`DIAGNOSTIC_DATA_SLACK_WEIGHT`).** With mixed units the L1 race can prefer loosening the
  large-scale constraint. Fix is scale-normalized weights (by RHS magnitude / row-coefficient
  norm); deferred until a test exposes the skew.
- **Weighted preference ladder is a fixed-constant stand-in, not lexicographic.** Two weights
  encode the ladder (editable `1` < data `1e3` < removal `1e6`). A true lexicographic ladder
  (drop the weights, run stage 1 in successive passes) is the proper fix for both at once.
  Revisit if a scale-mixed oracle case misorders the fixes.
