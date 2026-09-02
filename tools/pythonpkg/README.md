# DeciDB

**Optimization, Native in SQL**

DeciDB extends SQL with a `DECIDE` clause for declarative in-database optimization. Express constrained optimization problems directly in SQL — no external solver, no data export, no context switching.

A research project by [HUDA Lab](https://huda-lab.github.io/) at NYU Abu Dhabi.

## Install

```bash
pip install decidb
```

Wheels are published for Linux x86-64, macOS arm64, and Windows AMD64 on Python 3.11–3.13. DeciDB is a fork of DuckDB, so the Python API is DuckDB's.

## Quick Example

A knapsack: pick the most valuable items that fit in the weight budget.

```python
import decidb

conn = decidb.connect()
conn.execute("""
    CREATE TABLE Items (id INTEGER, value INTEGER, weight INTEGER);
    INSERT INTO Items VALUES (1, 100, 20), (2, 60, 10), (3, 120, 30);
""")

print(conn.execute("""
    SELECT id, value, weight, x
    FROM Items
    DECIDE x(BOOL)
    SUCH THAT SUM(x * weight) <= 50
    MAXIMIZE SUM(x * value)
""").fetchall())
```

`x` comes back as `1` for chosen rows and `0` for the rest. The equivalent with an external modelling library means exporting the table, rebuilding it as variables, solving, and joining the answer back by hand.

Choosing rows is only the simplest case. `INT` and `REAL` decisions assign a *quantity* to every row, which is what most real problems need — here, how many units to produce at each plant to meet demand at the lowest cost:

```sql
SELECT plant, units
FROM Plants
DECIDE units(INT)
SUCH THAT units <= capacity
      AND SUM(units) >= 500
MINIMIZE SUM(units * unit_cost)
```

## The DECIDE Clause

```sql
SELECT select_list
FROM table_expression
[WHERE ...]
DECIDE [Table.]variable_name(TYPE) [, ...]
SUCH THAT constraint [AND constraint ...]
[MAXIMIZE | MINIMIZE] objective_expression
```

The declaration may also sit between `SELECT` and `FROM`; the two orders parse to the same plan. `SUCH THAT` is required.

### Decision Variables

The type is **mandatory** and written in parentheses after the name.

| Declaration | Domain | Typical use |
|---|---|---|
| `x(BOOL)` | {0, 1} | Select or skip a row |
| `x(INT)` | {0, 1, 2, ...} | A whole quantity |
| `x(REAL)` | [0, ∞) | A continuous amount |

Lower bounds default to 0. A variable goes negative only when the query gives it an explicit negative bound, such as `SUCH THAT adj BETWEEN -10 AND 10`.

Three scopes control how many variables one declaration creates:

| Spelling | Scope | Variables |
|---|---|---|
| `x(INT)` | row-scoped (default) | one per result row |
| `T.x(INT)` | table-scoped | one per distinct entity of `T` |
| `scalar x(INT)` | query-wide | exactly one |

Table-scoped variables matter after a join: `DECIDE n.keep(BOOL)` gives each nurse one decision even when they appear in five shift rows.

### Constraints

Operators are `=`, `<>`, `<`, `<=`, `>`, `>=`, plus `BETWEEN` and `IN`. Aggregates over a decision are `SUM`, `AVG`, `MIN`, and `MAX`. To count selected rows use `SUM(x)` — `COUNT(x)` is rejected, because a decision variable is never null and so counting one always returns the row count.

```sql
SUCH THAT SUM(x * weight) <= 50            -- aggregate
SUCH THAT x <= 3                           -- per row
SUCH THAT SUM(x) <= 2 PER category         -- one constraint per group
SUCH THAT SUM(x * cost) <= 100 WHEN region = 'EU'   -- conditional
```

`PER` produces one constraint per distinct group. `WHEN` is a postfix condition that restricts which rows a constraint or objective term applies to.

### Objective

`MAXIMIZE` or `MINIMIZE` over an aggregate expression, or omit it entirely to ask only for a feasible assignment.

### Beyond linear

Products of a decision and a column (`x * weight`) are linear and always supported. Two further forms work:

- **Bilinear** — `x * y`, a product of two decisions. Bundled HiGHS handles Boolean-by-anything pairs; other pairs need Gurobi.
- **Quadratic** — `POWER(expr, 2)`, `expr ** 2`, or `(expr) * (expr)` in an objective, for least-squares style problems. Quadratic *constraints* are Gurobi only.

Triple products such as `x * x * x` are rejected.

## Diagnosing a Query

Prefix any decision query with `DIAGNOSE` to ask why it behaved as it did — which constraints conflict when there is no answer, and what to relax.

```sql
DIAGNOSE SELECT id, x FROM Items
DECIDE x(INT) SUCH THAT x <= 5 AND x >= 8 MAXIMIZE SUM(x);
```

## Solvers

[HiGHS](https://highs.dev/) is embedded — nothing to install. [Gurobi](https://www.gurobi.com/) is detected at runtime when present and unlocks non-convex bilinear objectives and quadratic constraints.

## What You Can Model

Any problem where the answer is a set of values that has to satisfy constraints while optimizing an objective — not only which rows to keep.

| Problem | The decision | Declared as |
|---|---|---|
| Ship what fits in a weight budget | keep or drop each row | `x(BOOL)` |
| Produce how much at each plant | a whole quantity per row | `units(INT)` |
| Blend inputs to hit a spec at least cost | a continuous amount per row | `mix(REAL)` |
| Roster staff across shifts | one decision per person, shared by their rows | `staff.on(BOOL)` |
| Hold a portfolio under a risk limit | continuous weights, quadratic risk constraint | `w(REAL)`, Gurobi |
| Cap the worst shortfall anywhere | a single number for the whole query | `scalar worst(INT)` |

The shape is always the same: declare the decisions, constrain them, name what to optimize.

## Documentation

- [Getting Started](https://huda-lab.github.io/decidb/getting-started.html)
- [Syntax Reference](https://huda-lab.github.io/decidb/documentation.html)
- [Examples](https://huda-lab.github.io/decidb/examples.html)

## Research

DeciDB builds on a decade of research into in-database constrained optimization:

- *Scalable Package Queries in Relational Database Systems* — Brucato, Abouzied, Meliou (VLDB 2016)
- *Scalable Computation of High-Order Optimization Queries* — Brucato, Abouzied, Meliou (CACM 2019)
- *Scaling Package Queries to a Billion Tuples* — Mai, Wang, Abouzied, Brucato, Haas, Meliou (VLDB 2024)

## License

MIT. See [LICENSE](https://github.com/huda-lab/decidb/blob/master/LICENSE).

## Links

- [Website](https://huda-lab.github.io/decidb)
- [GitHub](https://github.com/huda-lab/decidb)
- [Issue Tracker](https://github.com/huda-lab/decidb/issues)
- [HUDA Lab](https://huda-lab.github.io/)
