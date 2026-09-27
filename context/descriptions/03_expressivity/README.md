# DECIQL Expressivity Reference

This folder documents the expressive power of the DECIQL language — the SQL extension at the heart of DeciDB. Each keyword/construct is a **subfolder** containing:

- `done.md` — What is implemented today: semantics, implementation notes, and code pointers (the canonical *syntax* spec is `../00_project_overview/syntax_reference.md`)
- `todo.md` — What remains to be built, with design rationale and implementation suggestions

The language is the clean redesign of `../00_project_overview/deciql_language_spec.md`
(implemented 2026-09-26): generation (`PER K:`), aggregation (`BY (Γ)`) and
navigation (frames) are separate constructs, and a constraint reads as
`[WHEN θ] [PER K] [IF b]: body`.

---

## Folders

| Folder | done.md covers | todo.md covers |
|---|---|---|
| [problem_types/](problem_types/) | LP, ILP, MILP, QP, MIQP, QCQP, bilinear, feasibility — problem class taxonomy, solver support matrix, structural properties | SOCP |
| [decide/](decide/) | `INT`/`BOOL`/`REAL`/`SEMIINT`/`SEMIREAL`/`TEXT IN [...]`, row / `PER K:` / `PER ():` generation, declaration bounds, both clause orders, keyed reducers | *(no planned features)* |
| [such_that/](such_that/) | Comparisons (`=`,`<`,`<=`,`>`,`>=`,`<>`), BETWEEN, IN (columns + dec. vars), AND, subqueries, the `WHEN`/`PER`/`IF` prefixes, quadratic (`POWER(expr,2)`), NULL policy | *(no planned features)* |
| [maximize_minimize/](maximize_minimize/) | SUM, multi-var, column arithmetic objectives, `THEN` stages, `SATISFY` | *(no planned features)* |
| [when/](when/) | `WHEN θ` as a row filter on constraints, objectives and inside reducers; `IF b` as a decision guard | *(no planned features)* |
| [per/](per/) | `PER K:` generation, the functional-dependency rule, `BY (Γ)` aggregation, nested `MAX(PER k: SUM(e) BY (k))` objectives | *(no planned features)* |
| [sql_functions/](sql_functions/) | SUM, AVG, MIN/MAX, ABS, norm, `<>`, IN (dec. vars), arithmetic including division, comparisons, BETWEEN, NULL, frames (`AT` / `SUM(FROM .. TO ..) OVER`) | `MIN`/`MAX`/`AVG` range frames |
| [bilinear/](bilinear/) | Bool×anything (McCormick), non-convex (Q matrix), bilinear constraints, data coefficients, WHEN composition | *(no planned features)* |
| [explain/](explain/) | `EXPLAIN` / `EXPLAIN ANALYZE` / `EXPLAIN (FORMAT JSON)` on a DECIDE query: node structure, prefix-first clause rendering, layered as-written → canonical → rewritten rendering | *(no planned features)* |
| [diagnose/](diagnose/) | `DIAGNOSE <query>` statement prefix — the only trigger for the diagnostics engine, returning its findings as a relation | — |

---

## Keyword Status Matrix

| Keyword / Feature | Implemented | Notes |
|---|---|---|
| `DECIDE x(INT)` / `x(BOOL)` / `x(REAL)` | Yes | row-scoped by default |
| `x(SEMIINT) BETWEEN lo AND hi` / `SEMIREAL` | Yes | `0 OR lo <= x <= hi`; hidden BOOL switch |
| `x(TEXT IN ['a', 'b'])` | Yes | one-hot indicators, VARCHAR readback; `=`, `<>`, `IN` only |
| `PER K: x(D)` (keyed decision, any column list or relation) | Yes | replaces `T.x(TYPE)` |
| `PER (): x(D)` (query-wide decision) | Yes | replaces `scalar x(TYPE)` |
| Declaration bounds `BETWEEN lo AND hi` / `<= hi` / `>= lo` | Yes | constants or key-determined columns |
| `SUCH THAT` with `=`, `<`, `<=`, `>`, `>=`, `<>`, BETWEEN, IN | Yes | |
| `WHEN θ:` filter prefix | Yes | known data only |
| `PER K:` generation prefix (`PER ()` global) | Yes | functional-dependency rule, reject policy |
| `IF b:` guard prefix | Yes | BOOL / TEXT / integer linear comparison guards; linear bodies; indicator (Gurobi) or Big-M (HiGHS) |
| `agg(WHEN θ PER K: e) BY (Γ)` reducers | Yes | `BY ()` = whole input |
| Nested `MAX(PER k: SUM(e) BY (k))` objective | Yes | all 9 outer/inner combinations |
| Frames `AT(sel [ELSE v]: e) OVER (key [DESC] [CYCLIC] [WITHIN P])` | Yes | constraints only |
| Frames `SUM(FROM a TO b [EVERY d] [ELSE v \| ALL]: e) OVER (...)` | Yes | `MIN`/`MAX`/`AVG` ranges not yet |
| `MAXIMIZE a THEN MINIMIZE b ...` | Yes | staged solves; later stages linear |
| `SATISFY` / omitted objective | Yes | |
| Quadratic / bilinear objectives and constraints | Yes | Gurobi for non-convex and QCQP |
| `ABS()`, `norm(e, p)`, `MIN()`/`MAX()` | Yes | `norm(WHEN c: e, p)` takes the filter prefix |
| `DIAGNOSE <query>` prefix | Yes | labels quote the prefixes first |
| Uncorrelated / correlated scalar subqueries, nested DECIDE | Yes | |
| Variant Signature (spec §7.2) | **No** | the FD rule rejects instead |

### Problem Classification

For a complete taxonomy of what mathematical optimization problem classes DeciDB can express (LP, ILP, MILP, QP, MIQP, feasibility), see [problem_types/done.md](problem_types/done.md).

---

## Development Priorities

Open language-surface work: [SOCP](problem_types/todo.md), and range frames that
reduce with `MIN`/`MAX`/`AVG` ([sql_functions/todo.md](sql_functions/todo.md)).

---

## Background

DECIQL extends SQL with constrained optimization. The key structure:

```sql
SELECT select_list
DECIDE [PER scope:] name(domain) [bounds] [, ...]
FROM table_expression
[WHERE ...]
SUCH THAT
    [WHEN θ] [PER K] [IF b]: body [AND ...]
[MAXIMIZE | MINIMIZE expr [THEN ...] | SATISFY]
```

The declaration may equally sit after `WHERE`, immediately before `SUCH THAT`.
Both orders are accepted and produce the same plan.

See `context/descriptions/00_project_overview/syntax_reference.md` for the full implemented syntax reference.
