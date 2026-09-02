### Optimization, Native in SQL

DeciDB extends DuckDB with declarative constrained optimization.  
Assign optimal decisions over data with constraints and objectives — no external solver code required.

  
  



  


## Quick Example

```sql
SELECT item, value, weight, x AS selected
FROM Items
DECIDE x(BOOL)
SUCH THAT
    SUM(x * weight) <= 50
MAXIMIZE SUM(x * value);
```

Decisions are not only which rows to keep. `INT` and `REAL` variables assign a
quantity to every row — here, how much to produce at each plant:

```sql
SELECT plant, units
FROM Plants
DECIDE units(INT)
SUCH THAT units <= capacity
      AND SUM(units) >= 500
MINIMIZE SUM(units * unit_cost);
```

## Why DeciDB?

- **Native SQL** — Express optimization as a SQL extension. No context switching between your database and an external solver.
- **Zero Data Movement** — Solve directly on database buffers. No export/import overhead.
- **Declarative** — Define *what* to optimize, not *how*. The system chooses the solver formulation automatically, from linear through mixed-integer and quadratic.
- **Built on DuckDB** — Columnar storage, vectorized execution, and an embedded [HiGHS](https://highs.dev/) solver. Optional [Gurobi](https://www.gurobi.com/) support for commercial workloads.

## DECIDE Syntax

```sql
SELECT select_list
FROM table_expression
[WHERE ...]
DECIDE [Table.]variable_name(TYPE) [, ...]
SUCH THAT constraint [AND constraint ...]
[MAXIMIZE | MINIMIZE] objective_expression
```

The type is mandatory and written in parentheses. The declaration may also sit
between `SELECT` and `FROM`; both orders parse to the same plan. Omitting
`MAXIMIZE`/`MINIMIZE` asks only for a feasible assignment.


| Feature                  | Details                                                                          |
| ------------------------ | -------------------------------------------------------------------------------- |
| **Variable types**       | `BOOL` (0/1), `INT` (non-negative), `REAL` (continuous) |
| **Constraint operators** | `=`, `<`, `<=`, `>`, `>=`, `<>`, `BETWEEN`, `IN`                                 |
| **Variable scopes**      | `x(T)` per row, `Table.x(T)` per entity, `scalar x(T)` once per query            |
| **Aggregates**           | `SUM()`, `AVG()`, `MIN()`, `MAX()` — `COUNT()` over a decision is rejected; use `SUM(x)` |
| **Beyond linear**        | bilinear `x * y`, quadratic `POWER(expr, 2)` objectives (some forms Gurobi-only) |
| **Conditional**          | `expression WHEN condition` (postfix)                                            |
| **Grouping**             | `SUM(expr) op rhs PER column` — one constraint per distinct group                |


For the full syntax specification, see `[context/descriptions/00_project_overview/syntax_reference.md](context/descriptions/00_project_overview/syntax_reference.md)`.

## Building from Source

DeciDB requires [CMake](https://cmake.org), Python3, and a C++11 compliant compiler.

```bash
make release
```

Build output:

- **CLI**: `build/release/decidb`
- **Library**: `build/release/src/libduckdb.so`

## Running Tests

```bash
make decide-test       # Run DECIDE differential tests
make decide-setup      # Setup test environment only
```

Tests are located in `[test/decide/](test/decide/)`.

## Example Problem Domains

DeciDB can express a wide range of optimization problems directly in SQL:

- **Knapsack / Packing** — Maximize value within weight or budget limits
- **Diet / Nutrition** — Meet nutritional targets while minimizing cost
- **Portfolio Selection** — Maximize return under risk constraints
- **Resource Allocation** — Assign limited resources to maximize output
- **Production Planning** — Optimize production quantities with capacity constraints
- **Assignment** — Assign items to resources optimally
- **Scheduling** — Complex constraint satisfaction over time slots
- **Multi-period Optimization** — Time-indexed decision problems with carry-over constraints

## Documentation

Full documentation lives in `[context/descriptions/](context/descriptions/)`. Start with the [README there](context/descriptions/README.md) for navigation and reading order.

Key areas:

- `[00_project_overview/](context/descriptions/00_project_overview/)` — Syntax specification
- `[01_pipeline/](context/descriptions/01_pipeline/)` — The eight pipeline stages, one folder each
- `[03_expressivity/](context/descriptions/03_expressivity/)` — Feature status (WHEN, PER, aggregates, etc.)
- `[01_pipeline/04_canonicalizer/](context/descriptions/01_pipeline/04_canonicalizer/)` — The constraint/objective shape contract
- `[01_pipeline/05_optimizer/](context/descriptions/01_pipeline/05_optimizer/)` — Optimizer rewrite strategies

## Research

DeciDB is developed by the [HUDA Lab](https://huda-lab.github.io/) (NYU Abu Dhabi), and UMass Amherst.

**Foundation papers:**

- *Scalable Package Queries in Relational Database Systems* (VLDB 2016)
- *Scalable Computation of High-Order Optimization Queries* (ACM Communications, 2019)
- *Scaling Package Queries to a Billion Tuples* (VLDB 2024)

Contact: [huda-lab@nyu.edu](mailto:huda-lab@nyu.edu)

## License

DeciDB is released under the MIT License. See [LICENSE](LICENSE) for details.