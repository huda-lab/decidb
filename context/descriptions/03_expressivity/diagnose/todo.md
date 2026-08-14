# DIAGNOSE Prefix — Planned Feature

*(No `done.md` in this folder: nothing of this construct is implemented yet. Today's
diagnosis surface is the `diagnose_decide` session setting and the
`decide_diagnostics()` table function — see `../../07_query_diagnostics/`.)*

---

## `DIAGNOSE <query>` — statement prefix that runs a decision query and reports on the run

**Priority: High — paper-facing. Named in the CIDR'27 draft (§1 and §5); the grammar does
not accept it today. Blocked on one semantic decision (below) before it is picked up.**

The paper introduces the prefix as the interface for triggering diagnosis:

> The interface for triggering diagnosis is a prefix to the query: `DIAGNOSE`. It is
> analogous to `EXPLAIN ANALYZE` for a standard query in that it runs the query and reports
> on the run rather than returning rows. Without the prefix, a failed query just reports
> its status.

Nothing of this exists in the code. `DIAGNOSE` is not a keyword in
`third_party/libpg_query/grammar/`, and diagnosis is instead triggered by the solve outcome:
`PRAGMA diagnose_decide` defaults to `'auto'`, so any failed solve is diagnosed
automatically and its detail is read afterwards from `decide_diagnostics()`
(`src/decidb/utility/decide_diagnostic.cpp:200`, modes `auto` / `off`).

**Decision needed — what the prefix does to the existing auto path.** The draft's last
sentence ("without the prefix, a failed query just reports its status") describes opt-in
diagnosis, which is the opposite of today's default. Three readings, not yet chosen:

1. **Prefix replaces auto.** `diagnose_decide` drops to `off` by default; only a
   `DIAGNOSE`-prefixed query is diagnosed. Matches the draft literally. Cost: a user who
   does not already know the keyword never learns why their query failed — the argument
   `07_query_diagnostics/README.md` makes for on-by-default is given up.
2. **Prefix is an escalation on top of auto.** A bare failed query still gets its one-line
   diagnosed error (today's behavior); `DIAGNOSE` additionally *returns the diagnosis as a
   relation* instead of raising, which is what makes it `EXPLAIN ANALYZE`-like and removes
   the need to call `decide_diagnostics()` as a second statement. `diagnose_decide` stays
   the master mute.
3. **Prefix also diagnoses a successful solve.** `DIAGNOSE` on a query that solves reports
   the run (status, solve time, model size, gap) rather than the rows — the closest reading
   of the `EXPLAIN ANALYZE` analogy, and the only one where the prefix is useful on a query
   that did not fail.

Readings 2 and 3 compose; reading 1 excludes both. The choice changes §5 of the paper as
much as it changes the code, so settle it in the draft first.

**Also open, once the shape is fixed:**
- Does `DIAGNOSE` return a relation (composable: `SELECT * FROM (DIAGNOSE ...)`) or print a
  report? `EXPLAIN` returns a two-column relation; `decide_diagnostics()` already returns a
  typed relation, so a relation is the DuckDB-shaped answer.
- What it does on a query with no `DECIDE` clause — reject, or fall through to `EXPLAIN
  ANALYZE` semantics.
- Whether the prefix takes options (`DIAGNOSE (VERBOSE) ...`) the way `EXPLAIN` does.

**Implementation sketch.** Mirror `EXPLAIN` end to end rather than inventing a path:
- Grammar: a `DiagnoseStmt` rule alongside `ExplainStmt`
  (`third_party/libpg_query/grammar/statements/explain.y:8`), reusing `ExplainableStmt`;
  new keyword in the keyword lists. Requires `make grammar-build`.
- Parser node + statement: mirror `src/parser/statement/explain_statement.cpp`.
- Binder: mirror `src/planner/binder/statement/bind_explain.cpp` — bind the inner
  statement, wrap its plan, and declare the output names/types (the
  `decide_diagnostics()` schema is the natural column set).
- Execution: the diagnosis engines already exist and run inside the physical decide
  operator; the prefix only changes *who consumes the result* — the retained diagnosis is
  emitted as rows instead of being folded into an error. See
  `../../07_query_diagnostics/router/done.md` for the terminal that currently makes that
  decision.

**Test**: `test/decide/tests/` — a `DIAGNOSE`-prefixed infeasible query returns the repair
rows and does *not* raise; the same query unprefixed keeps whatever behavior the decision
above fixes; `DIAGNOSE` under `diagnose_decide='off'` behaves as decided.

**Done file**: create `done.md` in this folder when it ships, and update the interface
description in `../../07_query_diagnostics/README.md` plus the syntax reference
(`../../00_project_overview/syntax_reference.md`).

*Logged 2026-08-02, during paper §5 review (confirmed by the user as intended design that
the code has yet to catch up to).*
