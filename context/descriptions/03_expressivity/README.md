# DECIQL Expressivity Reference

This folder documents the expressive power of the DECIQL language — the SQL extension at the heart of DeciDB. Each keyword/construct is a **subfolder** containing:

- `done.md` — What is implemented today: semantics, implementation notes, and code pointers (the canonical *syntax* spec is `../00_project_overview/syntax_reference.md`)
- `todo.md` — What remains to be built, with design rationale and implementation suggestions

---

## Folders

| Folder | done.md covers | todo.md covers |
|---|---|---|
| [problem_types/](problem_types/) | LP, ILP, MILP, QP, MIQP, QCQP, bilinear, feasibility — problem class taxonomy, solver support matrix, structural properties | Negative domains, explicit bounds, SOCP |
| [decide/](decide/) | BOOL, INT, REAL (type mandatory), multiple vars, row-scoped/table-scoped, both clause orders, linearity | Query-wide `scalar`, relation-qualified reducer `SUM(D: expr)` — paper §3 |
| [such_that/](such_that/) | Comparisons (`=`,`<`,`<=`,`>`,`>=`,`<>`), BETWEEN, IN (columns + dec. vars), AND, subqueries (uncorrelated + correlated), WHEN, PER, quadratic (`POWER(expr,2)`) | `IS BETWEEN` spelling (paper §3) |
| [maximize_minimize/](maximize_minimize/) | SUM, multi-var, column arithmetic objectives; cross-refs to sql_functions, problem_types, when, per | *(no planned features)* |
| [when/](when/) | Full implementation (constraints + objectives + PER composition + aggregate-local filters) | *(no planned features)* |
| [per/](per/) | PER on constraints (single + multi-column), PER on objective (nested aggregates), WHEN+PER composition, row_group_ids architecture | Row-varying RHS |
| [sql_functions/](sql_functions/) | SUM, AVG, MIN/MAX, ABS, `<>`, IN (dec. vars), arithmetic, comparisons, BETWEEN, NULL | division |
| [bilinear/](bilinear/) | Bool×anything (McCormick), non-convex (Q matrix), bilinear constraints, data coefficients, WHEN composition | *(no planned features)* |
| [explain/](explain/) | `EXPLAIN` / `EXPLAIN ANALYZE` / `EXPLAIN (FORMAT JSON)` on a DECIDE query: node structure, the shared `WHEN`/`PER` renderer, cardinality | A live tag-leak bug in the Constraints section; layered as-written → canonical → rewritten rendering |
| [diagnose/](diagnose/) | *(nothing implemented — no `done.md`)* | `DIAGNOSE <query>` statement prefix (paper §5); today's surface is `PRAGMA diagnose_decide` + `decide_diagnostics()` |

---

## Keyword Status Matrix

| Keyword / Feature | Implemented | Todo File |
|---|---|---|
| `DECIDE x(BOOL)` | Yes | — |
| `DECIDE x(INT)` | Yes | — |
| `DECIDE x(REAL)` | Yes | — |
| Multiple variables: `DECIDE x(INT), y(BOOL)` | Yes | — |
| `DECIDE Table.var(TYPE)` (table-scoped) | Yes (entity-keyed, mixed with row-scoped) | — |
| `SUCH THAT` with `=`, `<`, `<=`, `>`, `>=` | Yes | — |
| `<>` (not-equal) | Yes (Big-M disjunction) | — |
| `AND` constraint separator | Yes | — |
| `BETWEEN ... AND ...` | Yes | — |
| `IN (...)` on table columns | Yes | — |
| `IN (...)` on decision variables | Yes (auxiliary binary indicators) | — |
| Uncorrelated scalar subqueries | Yes | — |
| Correlated scalar subqueries | Yes (per-row constraints; aggregate requires scalar RHS) | — |
| Linear constraints | Yes | — |
| Quadratic objective: `MINIMIZE SUM(POWER(expr, 2))` | Yes (convex QP, syntax-enforced) | — |
| Bilinear objectives (`b * x`, `x * y`) | Yes (McCormick / Q matrix) | — |
| Bilinear constraints (`b * x`, `x * y`) | Yes (McCormick / `GRBaddqconstr`) | — |
| Quadratic constraints: `POWER(expr, 2)` in SUCH THAT | Yes (QCQP, Gurobi only) | — |
| Feasibility (no MAXIMIZE/MINIMIZE) | Yes (both solvers) | — |
| `WHEN` on constraints | Yes | — |
| `WHEN` on objective | Yes | — |
| `PER` on constraints | Yes | — |
| `PER` on objective | Yes (nested aggregate syntax) | — |
| `MAXIMIZE SUM(...)` | Yes | — |
| `MINIMIZE SUM(...)` | Yes | — |
| `SUM()` over decision variables | Yes | — |
| `AVG()` over decision variables | Yes (coefficient scaling) | — |
| `ABS()` | Yes (linearized) | — |
| `MIN()` / `MAX()` over dec. vars | Yes (per-row / Big-M) | — |
| `DIAGNOSE <query>` prefix | No (paper §5; `PRAGMA diagnose_decide='auto'` today) | [diagnose/todo.md](diagnose/todo.md) |
| Relation-qualified reducer `SUM(D: expr)` / `SUM(D, T: expr)` | Yes (paper §3.2.2; composite multi-relation keys included) | — |
| `DECIDE` before `FROM` (paper clause order) | Yes (both orders accepted) | — |
| `DECIDE x(BOOL)` / `x(INT)` type form | Yes (both forms accepted) | — |
| `DECIDE scalar x(INT)` (query-wide) | Yes (paper §3.1) | — |
| `IS BETWEEN a AND b` | **No** (paper Figure 1; bare `BETWEEN` only — a known paper-vs-code divergence) | [such_that/todo.md](such_that/todo.md) |

### Problem Classification

For a complete taxonomy of what mathematical optimization problem classes DeciDB can express (LP, ILP, MILP, QP, MIQP, feasibility), see [problem_types/done.md](problem_types/done.md).

---

## Development Priorities

Every expressivity item the paper sweep raised is now implemented, bar two, and the
matrix above is the current state rather than a plan. What is left:

- **`IS BETWEEN` spelling** — not parsed; bare `BETWEEN` is. Paper Figure 1 uses the long
  form, so the figure does not run as printed. A known divergence, not scheduled work.
  [such_that/todo.md](such_that/todo.md)
- **`DIAGNOSE <query>` statement prefix** — deferred to the diagnostics stage and blocked
  on one semantic decision; `PRAGMA diagnose_decide='auto'` is today's route.
  [diagnose/todo.md](diagnose/todo.md)

Both are carried into the paper-vs-code review in [`../todo.md`](../todo.md).

---

## Background

DECIQL extends SQL with constrained optimization. The key structure:

```sql
SELECT select_list
DECIDE [Table.]variable_name(type) [, ...]
FROM table_expression
[WHERE ...]
SUCH THAT
    constraint [AND constraint ...]
[MAXIMIZE | MINIMIZE] objective_expression
```

The declaration may equally sit after `WHERE`, immediately before `SUCH THAT`
(`... FROM t WHERE ... DECIDE x(INT) SUCH THAT ...`). Both orders are accepted
and produce the same plan.

See `context/descriptions/00_project_overview/syntax_reference.md` for the full implemented syntax reference.
