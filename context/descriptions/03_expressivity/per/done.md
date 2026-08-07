# PER Keyword — Implemented Features

`PER` generates **one constraint per data-driven group** (one per distinct value/combination of the named column(s)) — groups the user can't enumerate when writing the query. `SUM(new_hours) <= 40 PER empID` is semantically equivalent to writing `SUM(new_hours) <= 40 WHEN empID = 'E001' AND … ` once per distinct `empID`.

**Syntax and basic semantics** (single/multi-column form, qualified references, WHEN+PER ordering, nested-aggregate objectives, restrictions): see `../../00_project_overview/syntax_reference.md` §7. This doc covers implementation semantics and architecture.

---

## Semantics beyond the syntax spec

- **Grammar for column refs**: PER uses `columnref_opt_indirection` (the same production SELECT/WHERE/GROUP BY use), so any column-reference shape valid in those clauses is valid in PER. The qualifier is purely syntactic — qualified and unqualified PER produce identical solutions when the unqualified form is unambiguous.
- **Empty groups** (WHEN excludes every row of a group) are skipped — no constraint is emitted. Only when *every* group is empty is the aggregate rejected (see `../when/done.md` → "Empty Row Sets").
- **Aggregate-local WHEN composes with PER**: local filters are applied per aggregate term, then PER groups rows that participate in at least one local term:
  ```sql
  SUCH THAT SUM(x * hours) WHEN weekday + SUM(x * overtime) WHEN weekend <= 40 PER empID
  ```
- **Multi-column PER**: groups are distinct combinations of all PER column values; the composite key is built from per-row values with null-byte separation for collision-free hashing. `PER (col)` ≡ `PER col`.
- **NULL handling**: NULL in any PER column excludes the row (`INVALID_INDEX`), matching SQL GROUP BY behavior.

## PER on Objective — Implementation Notes

The nested-aggregate objective `OUTER(INNER(expr)) PER col` uses two levels of auxiliary variables: inner (per-group) and outer (across-group), each with easy/hard classification — see [../maximize_minimize/done.md](../maximize_minimize/done.md).

**Zero-coefficient row pre-filter (PATH B inner MIN/MAX, both easy and hard)**: A row whose every term coefficient is zero contributes a vacuous `z_g op 0` linking row in the easy branch and an unnecessary indicator binary plus Big-M row in the hard branch. The inner formulation builds a per-group active-rows CSR (mirroring PATH A's flat pre-filter) and emits constraints only for rows with at least one nonzero coefficient. For groups with no active rows, `z_g`'s bounds are pinned to `[0, 0]` directly — preserving the original semantics, where the elided constraints combined with the outer optimization direction would have settled `z_g` at `0`. Outer-easy `MIN/MAX` over inner-`SUM` group sums applies the analogous group-level skip: groups with all-zero contribution don't emit a `w op 0` row, and if every group is identically zero, `w` is pinned to `0` directly.

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

## Use Case Example

```sql
-- Per-employee workload repair with role-specific cap
SELECT *
FROM Employees E JOIN WeeklyPlan P ON E.empID = P.empID
DECIDE new_hours(INT)
SUCH THAT
    SUM(new_hours) <= 40 PER P.empID AND
    SUM(new_hours) <= 30 WHEN E.title = 'Director' PER P.empID
MINIMIZE SUM(ABS(new_hours - hours)) PER projectID
```

---

## Scaling Considerations

The number of generated constraints equals `|distinct_values| x |PER_constraints|` — O(|D|) in the worst case. This motivates the optimizer's matrix-efficiency work on high-cardinality PER columns (see [../../04_optimizer/matrix_efficiency/todo.md](../../04_optimizer/matrix_efficiency/todo.md)): constraint-to-bound conversion, skyband pruning, drop-solve-validate-refine loops.

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
