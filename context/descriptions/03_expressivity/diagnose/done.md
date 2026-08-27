# `DIAGNOSE` Prefix (how it works)

Shipped in batch H (2026-08-26). The paper's §5 interface exists:

> The interface for triggering diagnosis is a prefix to the query: `DIAGNOSE`. It is
> analogous to `EXPLAIN ANALYZE` for a standard query in that it runs the query and reports
> on the run rather than returning rows. Without the prefix, a failed query just reports
> its status.

The syntax and the output schema are in `../../00_project_overview/syntax_reference.md`
§8, which is the canonical spec; this file records the semantics that were decided and
where the code lives.

## The semantic question, answered

The design note this file replaced listed three readings of "without the prefix, a failed
query just reports its status". Reading **1 — prefix replaces auto** was chosen, with
reading 2's relation output folded in:

- **`DIAGNOSE` is the only trigger.** There is no automatic path. A query that is never
  prefixed never pays for a diagnostic solve.
- **A bare failed query reports its state and stops.** `DECIDE optimization is
  infeasible.` / `… is unbounded.` No clause name, no repair, no second statement.
  Naming the clause *is* the elastic solve, and that only runs when asked. The error does
  point at the prefix, so a user who does not know the keyword still learns it — the cost
  reading 1 was originally charged with is paid in one sentence.
- **It returns a relation, not a report.** One row per finding, real columns and real
  types. That matches `EXPLAIN` / `DESCRIBE` / `SUMMARIZE`, the CLI renders it as a box
  for free, and it composes: `SELECT clause FROM (DIAGNOSE …) WHERE amount > 1000`.
- **A feasible query returns one row saying so.** Not the richer reading 3 (solve time,
  model size, gap as a true `EXPLAIN ANALYZE` analogue) — that is unscheduled. One row,
  `state = 'feasible'`, everything else NULL, so there is no separate output path for a
  query that worked.
- **A query with no `DECIDE` clause is rejected**, at bind time, rather than falling
  through to `EXPLAIN ANALYZE` semantics. `DIAGNOSE` reports on an optimization run.
- **No options syntax.** There is no `DIAGNOSE (VERBOSE) …`.

## Where it lives

Batch H mirrored `SUMMARIZE`, not `EXPLAIN`: `EXPLAIN` carries its own statement node and
buys an options syntax we do not need; `SUMMARIZE` is one grammar alternative plus a flag.

| layer | what it does |
| --- | --- |
| grammar | `DIAGNOSE SelectStmt` alternative on `VariableShowStmt` (`variable_show.y`), `is_diagnose` on `PGVariableShowSelectStmt`, `DIAGNOSE` added to `reserved_keywords.list`. Requires `make grammar-build`. |
| parser | `TransformShowSelect` produces a `ShowRef` with `ShowType::DIAGNOSE`. `select_with_parens`' existing `'(' VariableShowStmt ')'` production gives `FROM (DIAGNOSE …)` for free. |
| binder | `Binder::BindDiagnose` (`bind_showref.cpp`) binds the inner query unchanged, finds the plan's one `LogicalDecide`, sets `diagnose = true`, and wraps the plan in `LogicalDecideDiagnose`. |
| logical plan | `LogicalDecide::diagnose` — a property of the statement, never read back out of a session setting. `LogicalDecideDiagnose` owns the output schema. |
| stage 08 | `PhysicalDecide::diagnose` arms the engines and reports findings instead of raising; `PhysicalDecideDiagnose` sinks the query's rows and emits the findings. |

The engine itself is unchanged. What batch H changed is who starts it and how its answer
is shaped — see `../../07_query_diagnostics/`.

## Tests

- `test/decide/tests/test_diagnose_trigger.py` — the prefix is the only trigger; the
  deleted pragmas are unknown settings; the tuning pragmas still work; a query with no
  DECIDE clause is rejected.
- `test/decide/tests/test_query_diagnostics_relation.py::TestDiagnoseRelationShape` — the
  schema and its types, the `feasible` row, composability as a subquery, and the bare
  query's state-only error.
