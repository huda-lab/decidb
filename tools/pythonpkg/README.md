# DeciDB

**Optimization, Native in SQL**

DeciDB extends SQL with the `DECIDE` clause for declarative in-database optimization. Express Integer Linear Programming (ILP) problems directly in SQL — no external solvers, no data export, no context switching.

A research project by [HUDA Lab](https://huda-lab.github.io/) at NYU Abu Dhabi.

## Quick Example

**Traditional approach** — export data, build a model in Python, solve, map results back:

```python
import duckdb, pulp

items = conn.execute("SELECT id, value, weight FROM Items").fetchall()
prob = pulp.LpProblem("knapsack", pulp.LpMaximize)
x = [pulp.LpVariable(f"x_{i}", cat='Binary') for i in range(len(items))]
prob += pulp.lpSum(x[i] * items[i][2] for i in range(len(items))) <= 50
prob += pulp.lpSum(x[i] * items[i][1] for i in range(len(items)))
prob.solve()
# ... then map results back to database
```

**With DeciDB** — one SQL query:

```sql
SELECT id, value, weight, x
FROM Items
WHERE category = 'electronics'
DECIDE x IS BOOLEAN
SUCH THAT
    SUM(x * weight) <= 50
MAXIMIZE SUM(x * value);
```

Same result, a fraction of the code.

## Installation

```bash
pip install decidb
```

## Usage

```python
import decidb

# Connect (in-memory or file-based)
conn = decidb.connect()

# Create data
conn.execute("""
    CREATE TABLE Items (id INTEGER, value INTEGER, weight INTEGER);
    INSERT INTO Items VALUES (1, 100, 20), (2, 60, 10), (3, 120, 30);
""")

# Run an optimization query
result = conn.execute("""
    SELECT id, value, weight, x
    FROM Items
    DECIDE x IS BOOLEAN
    SUCH THAT
        SUM(x * weight) <= 50
    MAXIMIZE SUM(x * value)
""").fetchall()

print(result)
```

## The DECIDE Clause

DeciDB adds the `DECIDE` clause to standard SQL:

```sql
SELECT columns
FROM table
DECIDE variable [IS type], ...
SUCH THAT
    constraint AND ...
MAXIMIZE | MINIMIZE SUM(objective)
```

### Decision Variable Types

| Type | Domain | Example |
|------|--------|---------|
| `IS BOOLEAN` | {0, 1} | Select/skip an item |
| `IS INTEGER` | {0, 1, 2, ...} | Quantity to assign |

`IS INTEGER` is the default if no type is specified.

### Constraints

- Arithmetic: `SUM(x * weight) <= 50`
- Multiple constraints separated by `AND`
- Conditional constraints: `SUM(x * weight) <= 50 WHEN category = 'A'`
- Supported operators: `=`, `<>`, `<`, `<=`, `>`, `>=`

### Objective

```sql
MAXIMIZE SUM(x * value)
MINIMIZE SUM(x * cost)
```

All expressions involving decision variables must be **linear**.

## Use Cases

- **Knapsack / inventory selection** — maximize value under weight or budget constraints
- **Portfolio optimization** — select assets maximizing return under risk limits
- **Resource allocation** — assign resources to tasks within capacity constraints
- **Meal planning** — choose items optimizing nutrition within calorie budgets

## Key Features

- **Native SQL** — no context switching between query and optimization languages
- **Zero data movement** — solve directly on database buffers
- **Declarative** — define what to optimize, not how
- **Embedded solver** — powered by [HiGHS](https://highs.dev/), a high-performance LP/MIP solver

## Documentation

- [Getting Started](https://huda-lab.github.io/decidb/getting-started.html)
- [Syntax Reference](https://huda-lab.github.io/decidb/documentation.html)
- [Examples](https://huda-lab.github.io/decidb/examples.html)

## Research

DeciDB builds on a decade of research into **in-database constrained optimization**:

- *Scalable Package Queries in Relational Database Systems* — Brucato, Abouzied, Meliou (VLDB 2016)
- *Scalable Computation of High-Order Optimization Queries* — Brucato, Abouzied, Meliou (CACM 2019)
- *Scaling Package Queries to a Billion Tuples* — Mai, Wang, Abouzied, Brucato, Haas, Meliou (VLDB 2024)

## License

MIT License. See [LICENSE](https://github.com/huda-lab/decidb/blob/main/LICENSE) for details.

## Links

- [Website](https://huda-lab.github.io/decidb)
- [GitHub](https://github.com/huda-lab/decidb)
- [Issue Tracker](https://github.com/huda-lab/decidb/issues)
- [HUDA Lab](https://huda-lab.github.io/)
