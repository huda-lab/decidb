# DECIQL Expressivity Reference

This folder documents the expressive power of the DECIQL language — the SQL extension at the heart of DeciDB. Each keyword/construct is a **subfolder** containing:

- `done.md` — What is implemented today: semantics, implementation notes, and code pointers (the canonical *syntax* spec is `../00_project_overview/syntax_reference.md`)
- `todo.md` — What remains to be built, with design rationale and implementation suggestions

---

## Folders

| Folder | done.md covers | todo.md covers |
|---|---|---|
| [problem_types/](problem_types/) | LP, ILP, MILP, QP, MIQP, QCQP, bilinear, feasibility — problem class taxonomy, solver support matrix, structural properties | SOCP |
| [decide/](decide/) | BOOL, INT, REAL, per-row / keyed (`per K:`) / query-wide (`per ():`) declarations, signed variables, both clause orders, the colon form `SUM(D: e)` | No open work |
| [such_that/](such_that/) | Comparisons (`=`,`<`,`<=`,`>`,`>=`,`<>`), BETWEEN, IN (columns + dec. vars), AND, subqueries (uncorrelated + correlated), composed MIN/MAX, factors on reducers, quadratic (`POWER(expr,2)`), NULL policy | No open work |
| [maximize_minimize/](maximize_minimize/) | SUM, multi-var, column arithmetic objectives, composed MIN/MAX, nested objectives; cross-refs to sql_functions, problem_types, scope | No open work |
| [scope/](scope/) | `per`, `when` and `by`: keyed declarations, `[when c] [per K]:` in front of constraints and the objective, `agg([when c] [per K]: e) by (G)`, defaults, variant signatures, NULL and empty sets, how a clause becomes model rows, the two-level objective | The `entity` → `keyed` rename |
| [sql_functions/](sql_functions/) | SUM, AVG, MIN/MAX, ABS, norm, `<>`, IN (dec. vars), arithmetic including division, comparisons, BETWEEN, NULL | No open work |
| [bilinear/](bilinear/) | Bool×anything (McCormick), non-convex (Q matrix), bilinear constraints, data coefficients, composition with scopes | — |
| [explain/](explain/) | `EXPLAIN` / `EXPLAIN ANALYZE` / `EXPLAIN (FORMAT JSON)` on a DECIDE query: node structure, the shared renderer and its scope echo, cardinality, layered as-written → canonical → rewritten rendering | No open work |
| [diagnose/](diagnose/) | `DIAGNOSE <query>` statement prefix — the only trigger for the diagnostics engine, returning its findings as a relation | — |

---

## Keyword Status Matrix

| Keyword / Feature | Implemented | Todo File |
|---|---|---|
| `DECIDE x(BOOL)` | Yes | — |
| `DECIDE x(INT)` | Yes | — |
| `DECIDE x(REAL)` | Yes | — |
| Multiple variables: `DECIDE x(INT), y(BOOL)` | Yes | — |
| Keyed declaration `DECIDE per K: x(TYPE)` (`K` columns or relations) | Yes (one decision per class of `K`; NULL is a class) | [scope/todo.md](scope/todo.md) |
| Query-wide declaration `DECIDE per (): x(TYPE)` | Yes (one decision for the query) | — |
| `DECIDE` before `FROM` (paper clause order) | Yes (both orders accepted) | — |
| `SUCH THAT` with `=`, `<`, `<=`, `>`, `>=` | Yes | — |
| `<>` (not-equal) | Yes (Big-M disjunction, or an indicator constraint on Gurobi) | — |
| `AND` constraint separator | Yes (`OR` and commas between constraints are refused) | — |
| `BETWEEN ... AND ...` | Yes | — |
| `IN (...)` on table columns | Yes | — |
| `IN (...)` on decision variables | Yes (auxiliary binary indicators) | — |
| Uncorrelated scalar subqueries | Yes | — |
| Correlated scalar subqueries | Yes (a per-row bound; beside a reducer, the tightest value per group; as a factor on a reducer in a constraint, a helper decision) | — |
| Nested DECIDE in a scalar RHS subquery | Yes (inner solve supplies the outer bound) | — |
| Linear constraints | Yes | — |
| Quadratic objective: `MINIMIZE SUM(POWER(expr, 2))` | Yes (convex QP, syntax-enforced) | — |
| Bilinear objectives (`b * x`, `x * y`) | Yes (McCormick / Q matrix) | — |
| Bilinear constraints (`b * x`, `x * y`) | Yes (McCormick / `GRBaddqconstr`) | — |
| Quadratic constraints: `POWER(expr, 2)` in SUCH THAT | Yes (QCQP, Gurobi only) | — |
| Feasibility (no MAXIMIZE/MINIMIZE) | Yes (both solvers) | — |
| `when c:` in front of a constraint | Yes (kept rows only; a `when` that keeps no row is an error) | — |
| `per K:` in front of a constraint | Yes (one constraint per class; `per ()` one; NULL is a class) | — |
| `MAXIMIZE when c: E` / `MINIMIZE when c: E` | Yes (produced once; `per ()` is the only `per` allowed) | — |
| `agg(e) by (G)` | Yes (`SUM`, `AVG`, `MIN`, `MAX`, `count(*)`, `norm`; either side) | — |
| `agg(when c per K: e)` | Yes (filters and de-duplicates that reducer only) | — |
| Two-level objective `outer(per k: inner(e) by (k))` | Yes (all nine of `SUM`/`MIN`/`MAX`) | — |
| Determination of a `per` key | Yes (checked on the data before solving) | — |
| `MAXIMIZE SUM(...)` | Yes | — |
| `MINIMIZE SUM(...)` | Yes | — |
| Bare query-wide decision objective (`MINIMIZE cap`) | Yes | — |
| `SUM()` over decision variables | Yes | — |
| `AVG()` over decision variables | Yes (coefficient scaling) | — |
| `ABS()` | Yes (linearized) | — |
| `MIN()` / `MAX()` over dec. vars | Yes (per-row bound / extremum column) | — |
| Colon form `SUM(D: expr)` / `SUM(D, T: expr)` | Yes (means `sum(per D: expr)`; paper §3.2.2) | — |
| `DIAGNOSE <query>` prefix | Yes | [diagnose/done.md](diagnose/done.md) |
| `IS BETWEEN a AND b` | **No** (bare `BETWEEN` is the supported syntax) | — |

### Problem Classification

For a complete taxonomy of what mathematical optimization problem classes DeciDB can express (LP, ILP, MILP, QP, MIQP, feasibility), see [problem_types/done.md](problem_types/done.md).

---

## Development Priorities

The deferred language-surface extension recorded here is
[SOCP](problem_types/todo.md); [scope/todo.md](scope/todo.md) holds the rename of the
keyed scope. The other feature-specific `todo.md` files have no open work.

`DIAGNOSE <query>` is implemented — [diagnose/done.md](diagnose/done.md).

---

## Background

DECIQL extends SQL with constrained optimization. The key structure:

```sql
SELECT select_list
DECIDE [per key:] variable_name(type) [, ...]
FROM table_expression
[WHERE ...]
SUCH THAT
    [[when condition] [per key]:] constraint [AND ...]
[MAXIMIZE | MINIMIZE] [when condition:] objective_expression
```

A reducer inside a constraint or the objective may carry its own scope and the rows it
reads: `agg([[when condition] [per key]:] expression) [by (key)]`.

The declaration may equally sit after `WHERE`, immediately before `SUCH THAT`
(`... FROM t WHERE ... DECIDE x(INT) SUCH THAT ...`). Both orders are accepted
and produce the same plan.

See `context/descriptions/00_project_overview/syntax_reference.md` for the full implemented syntax reference.
