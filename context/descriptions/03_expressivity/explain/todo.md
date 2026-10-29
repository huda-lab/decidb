# EXPLAIN — open work

## Columns print by their bare name over a join

**Today.** EXPLAIN and DIAGNOSE print a clause's columns by their bare name, so over a
join `when S.cap > 12: abs(x - D.cap) <= S.cap` echoes as
`when cap > 12: abs(x - cap) <= cap`. Only a `per` or `by` key keeps its relation, as
written. A suggested edit pasted back over a join can then be ambiguous, or bind to the
other relation's column. The written layer is rendered without the relation aliases
(`InitializeConstraintSourceInfo`); messages, which quote through
`QuoteMessagesWith`, already qualify a column by its relation over a join.

**Decision needed.** Whether the echo qualifies a column by its relation when the query
reads more than one relation (D45 keeps the bare name). Doing so changes every
multi-relation echo and the pins of `test_explain.py`,
`test_query_diagnostics_relation.py` and `test_scope_explain_diagnose.py`.

**Test.** The clause above in EXPLAIN and DIAGNOSE over a join of two relations that
share a column name; each suggested change pasted back runs and gives the repaired
answer.

**Update.** `done.md` (the rendering layers),
`../../00_project_overview/syntax_reference.md` §9 ("Echo"),
`../../07_query_diagnostics/infeasible/done.md`.

## The variant line of EXPLAIN ANALYZE

Found by the review of 2026-10-29; every answer below is right, only the account is off.

- **A reducer with its own `per` counts over every row it reads, not per `by` group.**
  `per t.h: sum(per t.k: t.c * y) by (t.h) <= 10` prints *"3 values from 2 classes, one
  per distinct (c) within a class"* although each `h` group reads each `k` once: the line
  suggests the D65 double count where none happens (`BuildEntityMappings`, the
  qualified-scope branch). Count the variants per (group, class) for the comparison.
- **Most clause shapes print no line.** Only the rows shape and the helper shape record
  their variants; the aggregate shape (`per t.g: sum(x) by (t.h) <= t.v`), an easy
  MIN/MAX, ABS and `IN` under a `per` whose classes hold several variants do not.

**Test.** Both shapes in `test_anr_variants.py`, beside
`test_explain_analyze_counts_the_variants`.

**Update.** `done.md`, `../../00_project_overview/syntax_reference.md` §9.
