# Stage 03 — Logical plan: open work

## 2. Columns print by their bare name over a join

**Today.** EXPLAIN and DIAGNOSE print a clause's columns by their bare name, so over a join
`when S.cap > 12: abs(x - D.cap) <= S.cap` echoes as `when cap > 12: abs(x - cap) <= cap`. Only a `per` or `by` key
keeps its relation. A suggested edit pasted back over a join can then be ambiguous or bind to the other relation's
column. The written layer is rendered without relation aliases (`InitializeConstraintSourceInfo`); messages, which
quote through `QuoteMessagesWith`, already qualify a column over a join.

**Ruling (owner, 2026-10-27).** The echo qualifies a column by its relation when the query reads more than one relation;
a query over one relation keeps the bare name (D45). Doing so changes every multi-relation echo and the pins of `test_explain.py`,
`test_query_diagnostics_relation.py` and `test_scope_explain_diagnose.py`.

**Test.** The clause above in EXPLAIN and DIAGNOSE over a join of two relations sharing a column name; each suggested
change pasted back runs and gives the repaired answer.

**Update.** `../../language/explain.md`, `../../language/guide.md` §9, `../../diagnostics/infeasible/done.md`,
`../../language/decisions.md` D45.
