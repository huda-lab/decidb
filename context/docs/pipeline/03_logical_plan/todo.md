# Stage 03 — Logical plan: open work

## 1. One numbering for the key table

**Today.** Every key is one entry of `LogicalDecide::keys` (`done.md` §4), but execution
numbers its classes two ways: entity ids are first-seen over all rows
(`BuildEntityMappings`), group ids are dense in first-kept-row order
(`LookupOrBuildPerGroupIds`). NULL is a value in both, and an empty key is one group.

**Ruling (owner, 2026-10-27).** Merge them into one numbering, the one the design calls
for, and re-capture the golden dumps that print it (entries 22, 23, 38, 39, 56, 61, 62,
65, 70 and 91–94 are known to). Keeping a numbering to spare the goldens is not a reason.
Chosen (owner, 2026-10-28): ids ordered by the collated key value, each clause reading its
kept classes as a subsequence, so no message or dump depends on row order. Do it with the
NULL-key change (`../08_execution/todo.md` §2) and collation (`../../issues/bugs.md`),
which touch the same grouping code. Execution's half — one cache turning any key into
row → class ids — is `../08_execution/todo.md` §1.

**Test.** The golden corpus re-captured on the decision above, on Gurobi and HiGHS;
`test_scope_reducer_by.py` and `test_scope_generation.py` keep their answers.

**Update.** `../08_execution/done.md` §4 and §5.

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
