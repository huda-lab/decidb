# First S1 Rule — open work

Read the [prototype contract](spec.md) before editing this rule. The Word
catalogue owns the class proof. This TODO owns only the implementation slice.

- [ ] **RULE-01 — Complete matcher.** Depends: DES-07, HAR-01. Recognize the
  admitted bound/canonical shape from semantic facts; reject every extra
  objective/constraint factor, scope, grouping, filter, qualifier, frame, and
  unsupported expression. Return a stable reason for each one-condition-away
  miss. Do not use data samples or estimates.
- [ ] **RULE-02 — Eligibility proof.** Depends: RULE-01, DES-06, HAR-02. Produce
  a proof object containing variable identity/domain, capacity, objective
  direction and coefficient, guard obligations, and output identity. The
  builder cannot run from an incomplete proof.
- [ ] **RULE-03 — Relational assignment.** Depends: RULE-02, HAR-03. Build bound
  DuckDB operators for DOUBLE score evaluation, global ranking, and complete
  `INTEGER` 0/1 output. Handle maximize/minimize, zero/worsening coefficients,
  ties, empty input, and capacity above row count without dropping source rows.
- [ ] **RULE-04 — Runtime guard.** Depends: DES-02, RULE-03. Insert the mandatory
  NULL/non-finite check at the approved location. A direct plan that leaves a
  solver-read value unchecked is not eligible.

**Exit gate:** every positive case has a complete structural proof and ordinary
relational assignment; every near miss falls back before plan mutation. Final
acceptance still depends on [correctness](../03_correctness/todo.md).
