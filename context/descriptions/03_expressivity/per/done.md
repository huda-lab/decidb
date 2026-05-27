# PER Keyword — Implemented Features

## Motivation

Many data manipulation tasks require **one constraint per data-driven group** — but the groups are not known when the query is written. Without `PER`, users must write one explicit constraint per group, which is impractical when groups are determined by the data.

**Example — per-employee workload cap**: Each employee must work at most 40 hours/week. Without `PER`:

```sql
-- Impractical without PER:
SUCH THAT
    SUM(new_hours) <= 40 WHEN empID = 'E001' AND
    SUM(new_hours) <= 40 WHEN empID = 'E002' AND
    ...   -- one per employee (unknown count)
```

With `PER`:

```sql
SUCH THAT
    SUM(new_hours) <= 40 PER empID
```

---

## Core PER on Constraints

PER groups aggregate constraints by distinct column values. One ILP constraint is generated per distinct value of the PER column.

### Syntax

```sql
-- Constraint: single-column PER
constraint_expression [WHEN condition] PER column

-- Constraint: multi-column PER
constraint_expression [WHEN condition] PER (column1, column2, ...)

-- Objective: nested aggregate PER
MAXIMIZE OUTER(INNER(...)) PER column
MINIMIZE OUTER(INNER(...)) PER (column1, column2, ...)
```

Empty groups (groups where WHEN excludes every row) are skipped — no constraint is emitted.

**Examples**:

```sql
-- One constraint per distinct empID
SUCH THAT SUM(x * hours) <= 40 PER empID

-- One constraint per distinct (empID, department) combination
SUCH THAT SUM(x * hours) <= 40 PER (empID, department)

-- Qualified column references — useful in JOIN queries
SUCH THAT SUM(x * hours) <= 40 PER e.empID
SUCH THAT SUM(x * hours) <= 40 PER (e.empID, d.dept_id)
```

`PER` accepts both bare identifiers and `table.column` (or `alias.column`) qualified references. The qualifier is purely syntactic — qualified and unqualified PER produce identical solutions when the unqualified form is unambiguous. Mixed forms (`PER (empID, d.dept_id)`) are accepted. The grammar uses `columnref_opt_indirection` (the same production SELECT/WHERE/GROUP BY use), so any column-reference shape valid in those clauses is valid in PER.

### Semantics

`PER column` (or `PER (col1, col2, ...)`) causes the system to:
1. Find all distinct values (or composite key combinations) of the PER column(s) in the input relation (after any `WHEN` filter).
2. Generate one copy of the constraint for each distinct value/combination, applying it as an implicit filter.

The constraint `SUM(new_hours) <= 40 PER empID` is semantically equivalent to:

```sql
SUM(new_hours) <= 40 WHEN empID = 'E001' AND
SUM(new_hours) <= 40 WHEN empID = 'E002' AND
SUM(new_hours) <= 40 WHEN empID = 'E003' AND
...
```

### WHEN + PER Composition

Expression-level WHEN filters rows first, then PER groups the remaining rows:

```sql
-- Per-employee constraint, but only for Directors
SUCH THAT SUM(x * hours) <= 30 WHEN title = 'Director' PER empID
```

**Execution order**: WHEN (filter) → PER (partition → generate one constraint per group).

Aggregate-local WHEN also composes with PER. Local filters are applied per aggregate term, then PER groups rows that participate in at least one local term:

```sql
SUCH THAT
    SUM(x * hours) WHEN weekday + SUM(x * overtime) WHEN weekend <= 40 PER empID
```

### PER on Objective — Nested Aggregate Syntax

PER on objectives is fully implemented using nested aggregate syntax: `OUTER(INNER(expr)) PER col`, where `OUTER` and `INNER` are each one of `SUM`, `MIN`, `MAX`, or `AVG`. `AVG` as outer is equivalent to `SUM` for optimization (divides by constant G). `AVG` as inner scales each row's coefficient by `1/n_g` (the group size), which is meaningful when groups have different sizes.

```sql
MINIMIZE SUM(MAX(x * cost)) PER department     -- minimize sum of per-dept max costs
MAXIMIZE MIN(SUM(x * profit)) PER region       -- maximize the worst-performing region
MINIMIZE MAX(SUM(x * hours)) PER empID          -- minimize the peak workload
MINIMIZE SUM(AVG(x * cost)) PER department     -- minimize sum of per-dept average costs
MINIMIZE MAX(AVG(x * hours)) PER empID          -- minimize worst per-employee average
```

**Flat aggregate + PER behavior**:
- `SUM(expr) PER col` or `AVG(expr) PER col`: Accepted as a no-op (global sum of per-group sums equals the global sum).
- `MIN(expr) PER col` or `MAX(expr) PER col` (flat, no nested aggregate): **Error** — ambiguous semantics. The nested form is required.

The formulation uses two levels of auxiliary variables: inner (per-group) and outer (across-group), each with easy/hard classification. See [../maximize_minimize/done.md](../maximize_minimize/done.md) for the full formulation details.

**Zero-coefficient row pre-filter (PATH B inner MIN/MAX, both easy and hard)**: A row whose every term coefficient is zero contributes a vacuous `z_g op 0` linking row in the easy branch and an unnecessary indicator binary plus Big-M row in the hard branch. The inner formulation now builds a per-group active-rows CSR (mirroring PATH A's flat pre-filter) and emits constraints only for rows that have at least one nonzero coefficient. For groups with no active rows, `z_g`'s bounds are pinned to `[0, 0]` directly — that preserves the original semantics, where the elided constraints combined with the outer optimization direction would have settled `z_g` at `0`. Outer-easy `MIN/MAX` over inner-`SUM` group sums applies the analogous group-level skip: groups with all-zero contribution don't emit a `w op 0` row, and if every group is identically zero, `w` is pinned to `0` directly.

---

## Restrictions

- **Aggregate-only**: PER requires an aggregate constraint, such as `SUM(...)`, `AVG(...)`, or an additive expression of aggregate terms. Per-row constraints produce a binder error.
- **Column references only**: Each PER column must be a simple column reference, not an expression. DECIDE variables are not allowed.
- **Constant RHS**: No row-varying RHS with PER.
- **NULL handling**: NULL in any PER column excludes the row (`INVALID_INDEX`), matching SQL GROUP BY behavior.

### Multi-column PER

Multi-column PER uses parenthesized column lists: `PER (col1, col2, ...)`. Groups are formed by distinct combinations of all PER column values. A composite key is built from the per-row values of all columns, using null-byte separation for collision-free hashing. `PER (col)` with a single column in parens is equivalent to `PER col`.

---

## Architecture: Unified WHEN + PER via `row_group_ids`

PER and WHEN are unified under a single abstraction. Instead of separate `row_mask` (WHEN) and group information (PER), the system uses one field:

```cpp
// In EvaluatedConstraint (solver_input.hpp):
vector<idx_t> row_group_ids;   // Unified WHEN+PER: row→group mapping
idx_t num_groups = 0;           // 0 = ungrouped, >0 = number of groups
```

| Case | `row_group_ids` | `num_groups` | ILP constraints |
|------|-----------------|-------------|-----------------|
| No WHEN, no PER | empty | 0 | 1 (all rows) |
| WHEN only | 0 or INVALID_INDEX | 1 | 1 (matching rows) |
| PER only | 0..K-1 | K | K (one per group) |
| WHEN + PER | 0..K-1 or INVALID_INDEX | K | K (filtered, grouped) |

`SolverModel::Build` uses a group→rows index for O(N)-total constraint generation across all groups.

Aggregate-local WHEN is not represented as a standalone `row_group_ids` wrapper. Each aggregate term carries a filter mask, and row grouping includes rows that pass at least one local term filter.

---

## Use Cases

### Per-Employee / Per-Group Resource Limits (Repair)

```sql
SELECT *
FROM Employees E JOIN WeeklyPlan P ON E.empID = P.empID
DECIDE new_hours IS INTEGER
SUCH THAT
    SUM(new_hours) <= 40 PER P.empID AND
    SUM(new_hours) <= 30 WHEN E.title = 'Director' PER P.empID
MINIMIZE SUM(ABS(new_hours - hours)) PER projectID
```

### Per-Label Coverage (Active Learning / Selection)

```sql
SELECT * FROM Reviews
DECIDE keep IS BOOLEAN
SUCH THAT
    SUM(keep * weak_label) >= 50 PER weak_label AND
    SUM(keep) BETWEEN 200 AND 400
MINIMIZE SUM(keep * confidence)
```

Here `PER weak_label` generates one coverage constraint per distinct label class, ensuring adequate representation.

---

## Scaling Considerations

The number of generated constraints equals `|distinct_values| x |PER_constraints|`. For large relations this can produce O(|D|) constraints (one per tuple in the worst case). This is a key motivation for the optimizer's partition-solve strategy (see [../../04_optimizer/partition_solve/todo.md](../../04_optimizer/partition_solve/todo.md)).

**Mitigation strategies** (future optimizer work):
- **Partition-solve**: When all constraints and objective share the same PER column and there are no global constraints, decompose into K independent ILPs — see [../../04_optimizer/partition_solve/todo.md](../../04_optimizer/partition_solve/todo.md)
- **Constraint-to-bound conversion**: Detect PER constraints that are equivalent to simple variable bounds
- **Skyband pruning**: Eliminate dominated tuples before generating constraints
- **Drop-solve-validate-refine loop**: Generate a subset of constraints, solve, check if dropped constraints are violated, and iteratively refine

---

## Files Modified

- `src/include/duckdb/common/enums/decide.hpp` — `PER_CONSTRAINT_TAG`
- `src/include/duckdb/execution/operator/decide/physical_decide.hpp` — `DecideConstraint::per_columns`
- `src/include/duckdb/decidb/solver_input.hpp` — `row_group_ids` replaces `row_mask`
- `third_party/libpg_query/` — grammar rules, keyword, enum
- `src/parser/transform/expression/transform_operator.cpp` — transformer
- `src/decidb/symbolic/decide_symbolic.cpp` — normalizer passthrough
- `src/planner/expression_binder/decide_constraints_binder.cpp/.hpp` — `BindPerConstraint`
- `src/planner/expression_binder/decide_objective_binder.cpp` — nested aggregate PER objective binding
- `src/planner/binder/query_node/bind_select_node.cpp` — nested aggregate detection for PER objectives
- `src/execution/operator/decide/physical_decide.cpp` — unified WHEN+PER evaluation
- `src/decidb/utility/ilp_model_builder.cpp` — group-aware constraint builder
