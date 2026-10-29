# Stage 03 — Logical plan: open work

## 1. One key table

**Today.** `LogicalDecide` holds a grouping key in three forms. `entity_scopes`
(`EntityScopeInfo`) holds column bindings, one entry per set of key columns, for a
declaration's `per K:`, a reducer's own `per K'` and the colon form `SUM(D: e)`.
`group_keys` (`DecideGroupKey`) holds bound expressions for every `by (G)` (and a nested
reducer's path key, which the optimizer adds), since a `by` key may be an expression and
an `EntityScopeInfo` cannot hold one. A constraint's `per K:` key is the trailing
children of its `PER_CLASS_TAG` wrapper. Execution follows with two grouping
paths: `BuildEntityMappings` for `entity_scopes`, and `LookupOrBuildPerGroupIds`, one
cache, for `by`, class and scope-check keys; NULL is a value in both, and an empty key is
one group.

**Work.** One key table on `LogicalDecide`: `EntityScopeInfo` generalised from column
bindings to a list of bound key expressions, found or created by one function that
sorts and de-duplicates, so a declaration's key, a reducer's key, a `by` key and a
wrapper's key with the same expressions share one entry. The wrappers would then name
their entry instead of carrying columns. Execution's half — one cache turning any key
into row → class ids — is `../08_execution/todo.md`.

**Decision needed.** The two paths number groups differently: entity ids are first-seen
over all rows, group ids are dense in first-kept-row order. One table and one cache
either keep both numberings or re-capture the golden dumps that print them (entries 22,
23, 38, 39, 56, 61, 62, 65, 70 and 91–94 are known to).

**Test.** `test/common/test_decidb_plan_serialization.cpp` round-trips the merged table
(and `DECIDB_VERIFY_SERIALIZER=1` over the suite); the golden corpus is identical on
Gurobi and HiGHS, or re-captured on the decision above; `test_scope_reducer_by.py` and
`test_scope_generation.py` keep their answers.

**Update.** `done.md` §4 and §5 (the fields and the serialized structs),
`../02_binder/done.md` §1 (key scopes), `../code_structure.md` §4.
