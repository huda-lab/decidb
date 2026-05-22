# DECIQL Expressivity Reference

This folder documents the expressive power of the DECIQL language — the SQL extension at the heart of DecidB. Each keyword/construct is a **subfolder** containing:

- `done.md` — What is implemented today, with syntax, examples, and code pointers
- `todo.md` — What remains to be built, with design rationale and implementation suggestions

---

## Folders

| Folder | done.md covers | todo.md covers |
|---|---|---|
| [problem_types/](problem_types/) | LP, ILP, MILP, QP, MIQP, QCQP, bilinear, feasibility — problem class taxonomy, solver support matrix, structural properties | Negative domains, explicit bounds, SOCP |
| [decide/](decide/) | IS BOOLEAN, IS INTEGER, IS REAL, multiple vars, row-scoped/table-scoped, linearity | *(no planned features)* |
| [such_that/](such_that/) | Comparisons (`=`,`<`,`<=`,`>`,`>=`,`<>`), BETWEEN, IN (columns + dec. vars), AND, subqueries (uncorrelated + correlated), WHEN, PER, quadratic (`POWER(expr,2)`) | *(no planned features)* |
| [maximize_minimize/](maximize_minimize/) | SUM, multi-var, column arithmetic objectives; cross-refs to sql_functions, problem_types, when, per | *(no planned features)* |
| [when/](when/) | Full implementation (constraints + objectives + PER composition + aggregate-local filters) | *(no planned features)* |
| [per/](per/) | PER on constraints (single + multi-column), PER on objective (nested aggregates), WHEN+PER composition, row_group_ids architecture | Row-varying RHS |
| [sql_functions/](sql_functions/) | SUM, AVG, MIN/MAX, ABS, `<>`, IN (dec. vars), arithmetic, comparisons, BETWEEN, NULL | division |
| [bilinear/](bilinear/) | Bool×anything (McCormick), non-convex (Q matrix), bilinear constraints, data coefficients, WHEN composition | *(no planned features)* |

---

## Keyword Status Matrix

| Keyword / Feature | Implemented | Todo File |
|---|---|---|
| `DECIDE x IS BOOLEAN` | Yes | — |
| `DECIDE x IS INTEGER` | Yes | — |
| `DECIDE x IS REAL` | Yes | — |
| `DECIDE x` (default INTEGER) | Yes | — |
| Multiple variables: `DECIDE x, y` | Yes | — |
| `DECIDE Table.var` (table-scoped) | Yes (entity-keyed, mixed with row-scoped) | — |
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

### Problem Classification

For a complete taxonomy of what mathematical optimization problem classes DecidB can express (LP, ILP, MILP, QP, MIQP, feasibility), see [problem_types/done.md](problem_types/done.md).

---

## Development Priorities

All previously planned priorities are **done**:
1. ~~**IS REAL variables**~~ — done
2. ~~**AVG() aggregate**~~ — done (coefficient scaling)
3. ~~**ABS()**~~ — done (linearized via auxiliary variables)
4. ~~**`<>` and `IN` on decision variables**~~ — done (Big-M / auxiliary binary indicators)
5. ~~**MIN() / MAX()**~~ — done (per-row + Big-M linearization)

7. ~~**Multi-column PER**~~ — done (`PER (col1, col2, ...)` with composite keys)

8. ~~**PER on objective**~~ — done (nested aggregate syntax with two-level auxiliary formulation)
9. ~~**Correlated subqueries**~~ — done (delegated to DuckDB's standard decorrelation; per-row constraints + scalar-RHS aggregate constraints)

**Remaining**:
- **Row-varying RHS with PER** — see [per/todo.md](per/todo.md)

---

## Background

DECIQL extends SQL with constrained optimization. The key structure:

```sql
SELECT select_list
FROM table_expression
[WHERE ...]
DECIDE [Table.]variable_name [IS type] [, ...]
SUCH THAT
    constraint [AND constraint ...]
[MAXIMIZE | MINIMIZE] objective_expression
```

See `context/descriptions/00_project_overview/syntax_reference.md` for the full implemented syntax reference.
