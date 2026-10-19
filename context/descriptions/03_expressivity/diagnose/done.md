# `DIAGNOSE` Prefix (how it works)

This is the paper's §5 interface:

> The interface for triggering diagnosis is a prefix to the query: `DIAGNOSE`. It is
> analogous to `EXPLAIN ANALYZE` for a standard query in that it runs the query and reports
> on the run rather than returning rows. Without the prefix, a failed query just reports
> its status.

The syntax and output schema are summarized in the code-grounded
`../../00_project_overview/syntax_reference.md` §8; this file records the
semantics and where the implementation lives.

## The semantic question, answered

Of the three readings of "without the prefix, a failed query just reports its status",
DeciDB implements reading **1 — the prefix replaces automatic diagnosis**, with
reading 2's relation output folded in:

- **`DIAGNOSE` is the only trigger.** There is no automatic path. A query that is never
  prefixed never pays for a diagnostic solve.
- **A bare failed query reports its state and stops.** `DECIDE optimization is
  infeasible.` / `… is unbounded.` No clause name, no repair, no second statement.
  Naming the clause *is* the elastic solve, and that only runs when asked. The error does
  point at the prefix, so a user who does not know the keyword still learns it — the cost
  of reading 1 is paid in one sentence.
- **It returns a relation, not a report.** One row per finding, real columns and real
  types. That matches `EXPLAIN` / `DESCRIBE` / `SUMMARIZE`, the CLI renders it as a box
  for free, and it composes: `SELECT clause FROM (DIAGNOSE …) WHERE amount > 1000`.
- **A feasible query returns one row saying so.** Not the richer reading 3 (solve time,
  model size, gap as a true `EXPLAIN ANALYZE` analogue) — that is unscheduled. One row,
  `state = 'feasible'`, everything else NULL, so there is no separate output path for a
  query that worked.
- **A query with no `DECIDE` clause is rejected**, at bind time, rather than falling
  through to `EXPLAIN ANALYZE` semantics. `DIAGNOSE` reports on an optimization run.
- **The bound plan must contain exactly one `DECIDE`.** A DECIDE inside an ordinary
  subquery is accepted when it is the only optimization in the plan. An outer DECIDE
  with a nested DECIDE, or sibling DECIDE subqueries, is rejected with the number found;
  one diagnosis relation cannot identify which of several solves it describes.
- **No options syntax.** There is no `DIAGNOSE (VERBOSE) …`.

## Scoped clauses in the relation

- **`clause` and `suggested_change`** quote a clause in the plan style EXPLAIN prints:
  lower-case reducers and keywords, `when c per K:` in front, a reducer's `by (G)` after
  it, the colon form as written, and no `per row:` or `by ()` (`../explain/done.md` §2).
  The labels of rewrite rows follow the same style (`max(ship)` for an extremum column,
  `sum(ship) by (D.region) <> 20` for a `<>` binary).
- **`group`** holds the `per` key value of an expanded finding — one row per class
  (`edit_source = expanded_group`); a NULL key value renders as the text `NULL`.
- **`scope`** says `row`, or `entity` for a keyed (`per K:`) decision's escape count; a
  query-wide (`per ()`) decision has one instance and reports no count.
- **The data checks still raise.** A `when` that keeps no row, a `per` key that does not
  determine its clause, and a reducer group its own `when` empties are errors before
  solving, so `DIAGNOSE` reports them as the bare query does (`../scope/done.md` §5).

## Where it lives

The prefix mirrors `SUMMARIZE`, not `EXPLAIN`: `EXPLAIN` carries its own statement node and
buys an options syntax we do not need; `SUMMARIZE` is one grammar alternative plus a flag.

| layer | what it does |
| --- | --- |
| grammar | `DIAGNOSE SelectStmt` alternative on `VariableShowStmt` (`variable_show.y`), `is_diagnose` on `PGVariableShowSelectStmt`, `DIAGNOSE` added to `reserved_keywords.list`. Requires `make grammar-build`. |
| parser | `TransformShowSelect` produces a `ShowRef` with `ShowType::DIAGNOSE`. `select_with_parens`' existing `'(' VariableShowStmt ')'` production gives `FROM (DIAGNOSE …)` for free. |
| binder | `Binder::BindDiagnose` (`bind_showref.cpp`) binds the inner query unchanged, collects every `LogicalDecide`, requires exactly one, sets `diagnose = true`, and wraps the plan in `LogicalDecideDiagnose`. |
| logical plan | `LogicalDecide::diagnose` — a property of the statement, never read back out of a session setting. `LogicalDecideDiagnose` owns the output schema. |
| stage 08 | `PhysicalDecide::diagnose` arms the engines and reports findings instead of raising; `PhysicalDecideDiagnose` sinks the query's rows and emits the findings. |

The prefix decides who starts the engine and how its answer is shaped; the engine itself
is described in `../../07_query_diagnostics/`.

## Tests

- `test/decide/tests/test_diagnose_trigger.py` — the prefix is the only trigger; the
  deleted pragmas are unknown settings; the tuning pragmas still work; a query with no
  DECIDE clause is rejected.
- `test/decide/tests/test_query_diagnostics_relation.py::TestDiagnoseRelationShape` — the
  schema and its types, the `feasible` row, composability as a subquery, and the bare
  query's state-only error.
