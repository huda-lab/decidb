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
