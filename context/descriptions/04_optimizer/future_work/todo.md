# Future Work — Todo

Secondary and research-stage optimization ideas. These are lower priority than the two core areas (matrix efficiency, rewrite passes) but represent valuable directions for scaling DeciDB to larger datasets and more complex queries.

---

## 1. Skyband Indexing

**Priority**: Medium (critical for 100K+ row datasets)

Precompute the set of "potentially optimal" tuples — those not dominated on all relevant dimensions. A tuple `t` is dominated by `t'` if `t'` is at least as good on every objective dimension and at least as tight on every constraint dimension. The skyband (set of non-dominated tuples) is often much smaller than the full dataset.

**Benefit**: On million-tuple datasets, skyband computation takes ~12ms and can reduce solver input from millions of rows to thousands. The solver then works on a much smaller problem.

**Approach**: Build a multi-dimensional index over (objective coefficients, constraint coefficients), compute the skyline, pass only skyband tuples to the solver. Verify the solution against the full dataset for edge cases where dominated tuples matter.

---

## 2. Progressive Shading

**Priority**: Medium (depends on Skyband Indexing)

Layer-by-layer evaluation: Layer 0 = skyline, Layer 1 = skyline of remaining tuples after removing Layer 0, etc. Solve using Layer 0 only; if Layer 1 could improve the solution, add it; continue until optimal or time limit reached.

**Benefit**: Layer 0 alone often contains the optimal solution, avoiding full dataset materialization. Provides an anytime algorithm — return the best solution found so far at any point.

---

## 3. Layered Grouping Structure (LGS)

**Priority**: Low (depends on PER + Skyband)

A data structure that groups tuples by constraint participation for efficient incremental evaluation. Pre-groups tuples so constraint coefficient vectors are computed by lookup rather than full-relation scan. Specifically motivated by PER-generated constraints — without PER, the overhead isn't justified.

**Essential when**: PER on high-cardinality columns (1000+ distinct values) with large base tables.

---

## 4. LP Relaxation + Rounding

**Priority**: Low

Solve the continuous relaxation of the ILP (drop integrality constraints), then round the solution to obtain a feasible integer solution. Provides: (a) fast bounds on the optimal value, (b) an approximate feasible solution.

**Use cases**: Large problems where exact ILP solving is too slow, tight LP relaxations where rounding yields near-optimal solutions, warm-starting branch-and-bound.

Both HiGHS and Gurobi support LP solving natively — this requires building the same model but requesting continuous rather than integer solve.

---

## 5. Constraint Tightening (Chvatal-Gomory Cuts)

**Priority**: Low

Strengthen constraints to reduce the LP feasible region closer to the integer hull. Example: `2x + 3y <= 7` with integer x, y can be tightened to `x + y <= 2`.

**Note**: Modern solvers (Gurobi, HiGHS) already generate cutting planes internally. DeciDB-level cuts would only help if we can exploit problem structure the solver can't see (e.g., DECIDE-specific semantics).

---

## 6. Symmetry Breaking

**Priority**: Low

Add constraints that eliminate equivalent solutions when decision variables are interchangeable. Example: selecting 5 items from an identical pool → add ordering constraints `x_1 >= x_2 >= ...` to break symmetry.

**Benefit**: Reduces symmetric branches in the solver's search tree, potentially dramatic speedup for highly symmetric problems.

---

## 7. Constraint Softening (Slack Variables)

**Priority**: Medium

Replace hard constraints `SUM(x * w) <= K` with soft constraints `SUM(x * w) <= K + epsilon` where epsilon is a non-negative slack variable penalized in the objective. Instead of returning INFEASIBLE, return the best approximate solution with minimal constraint violation.

**Use case**: Interactive exploration where users are iterating on constraints and want feedback even when the problem is infeasible.

---

## 8. Objective Hardening (Sparsity Promotion)

**Priority**: Low

For repair/explanation tasks where diffuse solutions are admitted but sparse solutions are preferred. Convert continuous deviations to binary "did this row change?" indicators and minimize the number of changed rows.

**Use case**: "What's the smallest set of changes to make this plan feasible?"

---

## 9. Incremental Reasoning (Solution Maintenance)

**Priority**: Low (depends on Skyband Indexing)

When data or constraints change incrementally: determine if the last solution still holds, can be updated incrementally, or requires full re-solve. Uses skyband index and LGS for efficient determination.

**Dependency**: Requires Skyband Indexing and LGS to be implemented first.

---

## 10. Cost-Based Solver/Strategy Selection

**Priority**: Low

Currently solver selection is static (Gurobi > HiGHS). A cost-based approach would choose execution strategy based on estimated problem characteristics:

| Strategy | Best For | Trade-off |
|----------|----------|-----------|
| Direct ILP (Gurobi) | Small-medium (< 10K rows) | Exact optimal, may be slow for large |
| Direct ILP (HiGHS) | When Gurobi unavailable | Slower but always available |
| LP relaxation + rounding | Large, near-optimality OK | Fast, approximate |
| Progressive Shading + LGS | Very large (100K+ rows) | Requires index, amortized over queries |

**Approach**: Estimate problem size (matrix dimensions, non-zeros), estimate solver time using a simple model, choose strategy meeting target time budget.

---

## 11. Bound Tightening

**Status**: A first version is **implemented** at execution time —
`DecidePropagateImpliedBounds` derives finite upper bounds from non-negative
`<=`/`=` constraints (the knapsack/budget pattern) and feeds them into the
Big-M / McCormick formulations. See `../matrix_efficiency/done.md`. The
remaining work below generalizes it.

Derive tighter variable bounds from constraint interactions. If constraints collectively force a variable into a narrower range than its declared type allows, pass the tighter bounds to the solver.

**Example**: If `x(INT)` (default bounds [0, +inf)) but constraints imply `x <= 5`, set the upper bound to 5. Tighter bounds improve solver performance and Big-M quality.

**Remaining**: handle negative coefficients / general sign patterns (the current pass requires all-non-negative terms), iterate to a fixpoint (chained implications), and migrate the analysis into the optimizer (binder-to-optimizer migration) so it composes with other rewrites.

---

## Dependency Graph

```
Skyband Indexing ──→ Progressive Shading
       │                    │
       ├──→ LGS ───────────┤
       │                    │
       └──→ Incremental Reasoning
            (also depends on LGS)

Binder Migration ──→ Bound Tightening
```
