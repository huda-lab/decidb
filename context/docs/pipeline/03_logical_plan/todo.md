# Stage 03 — Logical plan: open work

## 1. One key table

**Today.** `LogicalDecide` holds a grouping key in three forms. `entity_scopes`
(`EntityScopeInfo`) holds column bindings, one entry per set of key columns, for a
declaration's `per K:`, a reducer's own `per K'` and the colon form `SUM(D: e)`.
`group_keys` (`DecideGroupKey`) holds bound expressions for every `by (G)`, since a `by`
key may be an expression and an `EntityScopeInfo` cannot hold one. A constraint's
`per K:` key and the two-level objective's key are the trailing children of their
wrappers (`PER_CLASS_TAG`, `PER_CONSTRAINT_TAG`). Execution follows with two grouping
paths: `BuildEntityMappings` for `entity_scopes`, and `LookupOrBuildPerGroupIds`, one
cache, for `by`, class and scope-check keys; NULL is a value in both, and an empty key is
one group.

**Work.** One key table on `LogicalDecide`: `EntityScopeInfo` generalised from column
bindings to a list of bound key expressions, found or created by one function that
sorts and de-duplicates, so a declaration's key, a reducer's key, a `by` key and a
wrapper's key with the same expressions share one entry. The wrappers would then name
their entry instead of carrying columns. Execution's half — one cache turning any key
into row → class ids — is `../08_execution/todo.md`.

The two paths number groups differently: entity ids are first-seen over all rows, group
ids are dense in first-kept-row order. **Ruling (owner, 2026-10-27).** Merge them into one numbering,
the one the design calls for, and re-capture the golden dumps that print it (entries 22,
23, 38, 39, 56, 61, 62, 65, 70 and 91–94 are known to). Keeping a numbering to spare the
goldens is not a reason. Chosen (owner, 2026-10-28): ids ordered by the collated key value, each clause reading
its kept classes as a subsequence, so no message or dump depends on row order. Do it with
the NULL-key change (`../08_execution/todo.md` §2) and collation (`../../issues/bugs.md`),
which touch the same grouping code.

**Test.** `test/common/test_decidb_plan_serialization.cpp` round-trips the merged table
(and `DECIDB_VERIFY_SERIALIZER=1` over the suite); the golden corpus is identical on
Gurobi and HiGHS, or re-captured on the decision above; `test_scope_reducer_by.py` and
`test_scope_generation.py` keep their answers.

**Update.** `done.md` §4 and §5 (the fields and the serialized structs),
`../02_binder/done.md` §1 (key scopes), `../code_structure.md` §4.

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
