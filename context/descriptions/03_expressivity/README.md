# DECIQL Expressivity Reference

This folder documents the expressive power of the DECIQL language — the SQL extension at the heart of DeciDB. Each keyword/construct is a **subfolder** containing:

- `done.md` — What is implemented today: semantics, implementation notes, and code pointers (the canonical *syntax* spec is `../00_project_overview/syntax_reference.md`)
- `todo.md` — What remains to be built, with design rationale and implementation suggestions

---

## Folders

| Folder | done.md covers | todo.md covers |
|---|---|---|
| [problem_types/](problem_types/) | LP, ILP, MILP, QP, MIQP, QCQP, bilinear, feasibility — problem class taxonomy, solver support matrix, structural properties | Negative domains, explicit bounds, SOCP |
| [decide/](decide/) | IS BOOLEAN, IS INTEGER, IS REAL, multiple vars, row-scoped/table-scoped, linearity | Clause order (`DECIDE` before `FROM`), `(BOOL)`/`(INT)` type form, query-wide `scalar`, relation-qualified reducer `SUM(D: expr)` — all paper §3 |
| [such_that/](such_that/) | Comparisons (`=`,`<`,`<=`,`>`,`>=`,`<>`), BETWEEN, IN (columns + dec. vars), AND, subqueries (uncorrelated + correlated), WHEN, PER, quadratic (`POWER(expr,2)`) | `IS BETWEEN` spelling (paper §3) |
| [maximize_minimize/](maximize_minimize/) | SUM, multi-var, column arithmetic objectives; cross-refs to sql_functions, problem_types, when, per | *(no planned features)* |
| [when/](when/) | Full implementation (constraints + objectives + PER composition + aggregate-local filters) | *(no planned features)* |
| [per/](per/) | PER on constraints (single + multi-column), PER on objective (nested aggregates), WHEN+PER composition, row_group_ids architecture | Row-varying RHS |
| [sql_functions/](sql_functions/) | SUM, AVG, MIN/MAX, ABS, `<>`, IN (dec. vars), arithmetic, comparisons, BETWEEN, NULL | division |
| [bilinear/](bilinear/) | Bool×anything (McCormick), non-convex (Q matrix), bilinear constraints, data coefficients, WHEN composition | *(no planned features)* |
| [diagnose/](diagnose/) | *(nothing implemented — no `done.md`)* | `DIAGNOSE <query>` statement prefix (paper §5); today's surface is `PRAGMA diagnose_decide` + `decide_diagnostics()` |

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
| `DIAGNOSE <query>` prefix | No (paper §5; `PRAGMA diagnose_decide='auto'` today) | [diagnose/todo.md](diagnose/todo.md) |
| Relation-qualified reducer `SUM(D: expr)` | No (paper §3.2; aggregates run over join-result rows today) | [decide/todo.md](decide/todo.md) |
| `DECIDE` before `FROM` (paper clause order) | No (paper Figure 1; `SELECT…FROM…DECIDE` only today — both orders will be accepted) | [decide/todo.md](decide/todo.md) |
| `DECIDE x(BOOL)` / `x(INT)` type form | No (paper §3.1; `IS BOOLEAN` / `IS INTEGER` only today — both forms will be accepted) | [decide/todo.md](decide/todo.md) |
| `DECIDE scalar x(INT)` (query-wide) | No (paper §3.1; row-scoped and table-scoped only today) | [decide/todo.md](decide/todo.md) |
| `IS BETWEEN a AND b` | No (paper Figure 1; bare `BETWEEN` only today — both spellings will be accepted) | [such_that/todo.md](such_that/todo.md) |

### Problem Classification

For a complete taxonomy of what mathematical optimization problem classes DeciDB can express (LP, ILP, MILP, QP, MIQP, feasibility), see [problem_types/done.md](problem_types/done.md).

---

## Development Priorities

All previously planned expressivity priorities are implemented (see the status matrix above).
**Remaining** — the queue is now driven by the submitted CIDR'27 paper; the full
paper-vs-code sweep and its staging order live in [`../todo.md`](../todo.md).

**Stage 1 — grammar batch** (one `make grammar-build` cycle; both orders and both spellings
accepted, nothing deprecated, no existing query migrates):
- **Clause order: `DECIDE` before `FROM`** — largest of the four; watch the `in_decide_clause` lexer flag. [decide/todo.md](decide/todo.md)
- **Parenthesized type form `x(BOOL)` / `x(INT)`** — [decide/todo.md](decide/todo.md)
- **Query-wide `scalar` decisions** — keyword, settled by the draft. [decide/todo.md](decide/todo.md)
- **`IS BETWEEN` spelling** — smallest. [such_that/todo.md](such_that/todo.md)

**After stage 1:**
- **Relation-qualified reducer `SUM(D: expr)`** — paper-facing; small grammar edit, real work is the de-duplication stage and multi-relation keys. [decide/todo.md](decide/todo.md)
- **Row-varying RHS with PER** — with the sibling constraint shapes in `../todo.md` group B. [per/todo.md](per/todo.md)
- **`DIAGNOSE` statement prefix** — deferred to the diagnostics stage; blocked on one semantic decision. [diagnose/todo.md](diagnose/todo.md)

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
