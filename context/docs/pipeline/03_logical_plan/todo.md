# Stage 03 — Logical plan: open work

## LP-2 — Columns print by their bare name over a join

**Today.** EXPLAIN and DIAGNOSE print a clause's columns by their bare name, so over a join
`when S.cap > 12: abs(x - D.cap) <= S.cap` echoes as `when cap > 12: abs(x - cap) <= cap`. Only a `per` or `by` key
keeps its relation. A suggested edit pasted back over a join can then be ambiguous or bind to the other relation's
column. The written layer is rendered without relation aliases (`InitializeConstraintSourceInfo`); messages, which
quote through `QuoteMessagesWith`, already qualify a column over a join.

**Ruling (owner, 2026-10-27).** The echo qualifies a column by its relation when the query reads more than one relation;
a query over one relation keeps the bare name (D45). Doing so changes every multi-relation echo and the pins of `test_explain.py`,
`test_query_diagnostics_relation.py` and `test_scope_explain_diagnose.py`.

**Design (architect review done).** One root under this task and the two bullets of
`../../issues/bugs.md` § BUG-8 (one name form, one key text per clause): display text lives on shared objects
(`EntityScopeInfo.table_alias`, a key reference's `name` set to its SQL spelling by `AddKeyColumn`,
`decide_binder.cpp:390`) instead of on each occurrence. A table-index → alias map on the operator would go stale:
DuckDB renumbers DECIDE's bindings (`ColumnBindingReplacer`, `RemoveUnusedColumns`) and the physical plan has none.
Proposed: the binder gives each data column reference its bare name plus a relation tag pointing into
`source_fragments` (over two or more relations only), `RenderSource` prints `relation.name`, and every
`relations`/`no_relations` parameter goes; a reducer's written key is recorded per occurrence the same way (a new tag
kind; `__source_fragment_` would replace the whole reducer).

**Decision needed.** Four choices, each with a recommendation.
1. Relation text in `source_fragments`, in a new `relation_names` registry (serialized field), or in the tag payload?
   Recommended: `source_fragments`.
2. Tag where each reference is bound, or in one pass after binding? Recommended: where bound.
3. Land the per-occurrence key text with this change, or separately? The one key table (`LogicalDecide::keys`) merges
   keys over the same columns into one entry. Recommended: with this change.
4. Does a key written bare (`per d:`) echo qualified over a join (D45 qualifies columns; keys "keep as written")?
   Recommended: as written.

**Test.** The clause above in EXPLAIN and DIAGNOSE over a join of two relations sharing a column name; each suggested
change pasted back runs and gives the repaired answer.

**Update.** `../../language/explain.md`, `../../language/guide.md` §9, `../../diagnostics/infeasible/done.md`,
`../../language/decisions.md` D45.
