# The DIAGNOSE prefix

`DIAGNOSE <query>` runs a DECIDE query and returns a relation describing the run instead of the query's rows. The
output schema and settings are in [guide.md §8](guide.md#8-diagnose). This file covers the semantics and where the
prefix is implemented. The engines that compute the findings are in `../diagnostics/`.

## Semantics

- **The prefix is the only trigger.** There is no automatic diagnosis, so an unprefixed query never pays for a
  diagnostic solve.
- **A bare failed query reports its state and stops**: `DECIDE optimization is infeasible.` or `… is unbounded.`,
  with a pointer to the prefix. Naming the clause takes the diagnostic (elastic) solve, which runs only when asked.
- **The result is a relation**: one row per finding, real columns and types, like `EXPLAIN`, `DESCRIBE` and
  `SUMMARIZE`. It composes: `SELECT clause FROM (DIAGNOSE …) WHERE amount > 1000`.
- **A feasible query returns one row**, `state = 'feasible'`, all else NULL, so there is no second output path.
- **The query must contain exactly one DECIDE**, possibly inside a subquery. No DECIDE, a nested DECIDE, or sibling
  DECIDE subqueries are refused at bind time with the number found: one relation cannot say which solve it describes.
- **There is no options syntax** (`DIAGNOSE (VERBOSE) …`).
- **Scoped clauses.** `clause` and `suggested_change` quote clauses in the EXPLAIN style
  ([explain.md](explain.md#rendering)). `group` holds the `per` key value of an expanded finding, one row per class
  (`edit_source = expanded_group`); a NULL key value prints as `NULL`. `scope` is `row`, or `keyed` for a keyed
  decision's escape count; a query-wide decision has one instance and reports no count.
- **Data checks still raise.** A `when` that keeps no row, a key that does not determine its clause, and an empty
  reducer group fail before solving, under DIAGNOSE as without it ([guide.md §7](guide.md#7-determination-null-and-empty-sets)).

## Implementation

The prefix follows `SUMMARIZE` (one grammar alternative and a flag) rather than `EXPLAIN` (its own statement node and
an options syntax DIAGNOSE does not need).

| Layer | What it does |
|---|---|
| Grammar | `DIAGNOSE SelectStmt` on `VariableShowStmt` (`variable_show.y`), `is_diagnose` on `PGVariableShowSelectStmt`, `DIAGNOSE` in `reserved_keywords.list` |
| Parser | `TransformShowSelect` makes a `ShowRef` with `ShowType::DIAGNOSE`; the existing `'(' VariableShowStmt ')'` production gives `FROM (DIAGNOSE …)` |
| Binder | `Binder::BindDiagnose` (`bind_showref.cpp`) binds the inner query, requires exactly one `LogicalDecide`, sets `diagnose`, wraps the plan in `LogicalDecideDiagnose` |
| Logical plan | `LogicalDecide::diagnose` is a property of the statement, never read from a session setting; `LogicalDecideDiagnose` owns the schema |
| Execution | `PhysicalDecide::diagnose` arms the engines and reports findings instead of raising; `PhysicalDecideDiagnose` emits them |

Tests: `test_diagnose_trigger.py`, `test_query_diagnostics_relation.py::TestDiagnoseRelationShape`.
