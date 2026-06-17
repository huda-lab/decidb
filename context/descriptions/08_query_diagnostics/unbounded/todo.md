# Query Diagnostics — Unbounded (remaining)

The objective improves without limit — the infeasible machine run backwards:
region too *open* in the improving direction. The *fix* is forced (the user must
add a bound or fix a sign error — you can't relax your way out), but the
*diagnosis* can be rich: name the exact variables escaping to infinity.

## Output schema (current)

The unbounded diagnosis surfaces through `decide_diagnostics()` as a
**variable-centric** relation (one row = one escaping variable; see
`foundations/done.md` · F5):

    query_id | state | variable | direction | group_label | suggested_bound

`variable` + `direction` are populated; `query_id` ties the rows of one solve
together; `group_label` + `suggested_bound` are reserved (NULL) — see Residual below.

## Landed (see `done.md`)

- **U3 · Ray→SQL naming (full output)** — names the escaping variables (user vars;
  aux→expression as defensive infrastructure) and reports the escape direction,
  through the variable-centric `decide_diagnostics()` relation. Landed with **F6**.

## Remaining (non-v1 enrichments — the unbounded state is functionally complete)

- [ ] **`group_label` — per-group (`PER`) naming.** With `PER`, one variable name
  becomes one instance per group; when only some groups escape, name which (e.g.
  `region=ASIA`) instead of leaving it ambiguous. Two pieces of work: **(a)** the
  current dedup-by-name in `BuildUnboundedDiagnostic` collapses per-group instances
  into a single row — switch to dedup by `(name, group)` when grouped; **(b)** plumb
  the `PER`/`WHEN` group's display value down to the diagnosis site (the group→name
  plumbing, shared with the infeasible engine). Until then `group_label` reads NULL.
  With no `PER` the column is *genuinely* NULL (one variable = one thing, nothing to
  disambiguate) — this is the common case (e.g. the `run.sh` demo).
- [ ] **`suggested_bound` — example cap value.** Compute a candidate (e.g. the
  largest RHS in scope) to fill the column. Deliberately deferred: a wrong number
  anchors the user, and the right cap is domain knowledge. The code is small; the
  hold-up is the design call, not the work.
- [ ] **Clause-aware line (optional).** Name the clauses an escaping variable appears
  in. **Hard limit (finding, load-bearing):** the ray identifies a *missing* bound,
  so it can name the runaway variable but **cannot** finger a single guilty clause —
  a flipped-sign constraint is mathematically indistinguishable from an absent one.
  So this can only ever be *context* ("`x` appears in clauses 2, 5; none cap it"),
  never blame. Needs the same clause-text plumbing as the infeasible engine.

This file tracks remaining unbounded work; landed unbounded notes live in `done.md`.
