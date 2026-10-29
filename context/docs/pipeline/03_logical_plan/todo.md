# Stage 03 — Logical plan: open work

## LP-2 — Columns print by their bare name over a join

**Today.** EXPLAIN and DIAGNOSE print a clause's columns by their bare name, so over a join
`when S.cap > 12: abs(x - D.cap) <= S.cap` echoes as `when cap > 12: abs(x - cap) <= cap`. Only a `per` or `by` key
keeps its relation. A suggested edit pasted back over a join can then be ambiguous or bind to the other relation's
column. The written layer is rendered without relation aliases (`InitializeConstraintSourceInfo`); messages, which
quote through `QuoteMessagesWith`, already qualify a column over a join.

**Root.** One root under this task and the two bullets of `../../issues/bugs.md` § BUG-8 (one name form, one key text
per clause): display text lives on shared objects (`EntityScopeInfo.table_alias`, a key reference's `name` set to its
SQL spelling by `AddKeyColumn`, `decide_binder.cpp:390`) instead of on each occurrence. A table-index → alias map on
the operator would go stale: DuckDB renumbers DECIDE's bindings (`ColumnBindingReplacer`, `RemoveUnusedColumns`) and
the physical plan has none.

**Ruling (owner, 2026-10-29); architect review done.** D45 changes: every
column prints as written, over one relation or many. A column written `S.cap` keeps `S.`; a bare `cap` that bound has
one meaning (DuckDB refuses an ambiguous bare name), so it pastes back unchanged. No relation name is inserted. The
problem above was that re-rendering dropped the written `S.`; BUG-4's spans (`../../issues/bugs.md` § BUG-4) keep it.
- Where `DecideBinder` binds a data column, it tags the bound reference with a `__source_fragment_` entry holding its
  spelling as written. `RenderSource` already prints such fragments, the tag survives DuckDB's renumbering, and it is
  serialized with `source_fragments`. No new registry and no new tag kind.
- Tagged where bound, the one place that knows both the spelling and the binding.
- The per-occurrence key text lands with this change: each clause's key text is cut from its own `per`/`by` span.
- A key echoes as written.
- Every `relations`/`no_relations` parameter goes (`RenderContext`, `DecidePlanNames`, `DecideRelationNames`,
  `QuoteMessagesWith`), including the 4-argument overload in `decide_linear_form.cpp`.

**Test.** The clause above in EXPLAIN and DIAGNOSE over a join of two relations sharing a column name prints as
written; each suggested change pasted back runs and gives the repaired answer. A stage-05 refusal over a join quotes
the column as written (`../../issues/bugs.md` § BUG-5).

**Update.** `../../language/explain.md`, `../../language/guide.md` §9, `../../diagnostics/infeasible/done.md`,
`../../language/decisions.md` D45.
