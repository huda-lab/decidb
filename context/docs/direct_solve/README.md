# Direct solve

Direct solve answers some DECIDE problems without a solver. When a problem matches a class it can prove, the optimizer
replaces the DECIDE node with ordinary DuckDB operators (sort, rank, filter) that compute the optimal assignment. Any other
problem takes the existing solver path, unchanged. It is **on by default** (`decide_direct_solve = 'auto'`); `off` forces
the solver and `require` raises an error when no rule applies.

Five problem classes work: **S1**, "choose K rows by score" (count bounds on one `BOOL` variable); **A1**, "each row
gets its best value in its own range" (per-row bounds on `BOOL`, `INT` or `REAL` decisions with a linear or one-square
objective); **R1**, "fill one budget by value per unit" (one budget over `REAL` decisions); **N1**, "stay inside one sum
of squares" (one `SUM(POWER(x, 2)) <= B` over a `REAL` decision, per group or over all rows); and **A2**, "one shared
value closest to every row" (one query-wide or keyed `REAL` decision under a squared, absolute or maximum absolute
loss). Each class's `done.md` gives its speed against Gurobi and HiGHS; every class is faster than
each solver that takes it, at every size measured. N1 also answers without a Gurobi license, where the solver path
refuses a quadratic limit. The other 14 classes in the catalogue are not started.

## Where to look

| I want to... | Go to |
|---|---|
| Understand how it works | [architecture/overview.md](architecture/overview.md), then the other files there |
| Look up a term, its standard name and the identifiers it covers | [../glossary.md](../glossary.md) |
| Add a new problem class | [architecture/rules.md](architecture/rules.md), then a new folder under `rules/` |
| See what a working class covers, where it differs from the solver, how it is tested and how fast it is | `rules/<id>_<slug>/done.md`: [S1](rules/s1_uniform_matroid/done.md), [A1](rules/a1_separable_box/done.md), [R1](rules/r1_continuous_knapsack/done.md), [N1](rules/n1_euclidean_ball/done.md), [A2](rules/a2_single_facility_location/done.md) |
| See what is open for a working class | `rules/<id>_<slug>/todo.md`: [S1](rules/s1_uniform_matroid/todo.md), [A1](rules/a1_separable_box/todo.md), [R1](rules/r1_continuous_knapsack/todo.md), [N1](rules/n1_euclidean_ball/todo.md), [A2](rules/a2_single_facility_location/todo.md) |
| Work on the shared engine or error wording | [framework/](framework/) |
| See a class that is not built yet, or start one | [todo.md](todo.md) |
| Time direct solve against Gurobi and HiGHS on TPC-H | [../performance/benchmarking/direct_solve.md](../performance/benchmarking/direct_solve.md) |
| Rerun a measurement a rule's `done.md` cites | [../performance/benchmarking/archive_profilers.md](../performance/benchmarking/archive_profilers.md) |
| Know why `auto` is the default, or what a user can notice | [architecture/policy.md](architecture/policy.md) |
| Read the definition and correctness argument of a class | `rules/<id>_<slug>/definition.md` |
| Check current DECIDE syntax | `../language/guide.md` |

## Status by class

| Class | Name | Catalogue status | Direct solve |
|---|---|---|---|
| [A1](rules/a1_separable_box/) | Separable box-constrained problem | Exact | **Works**, on by default |
| [A2](rules/a2_single_facility_location/) | Single-facility location (1-D) | Exact | **Works**, on by default, including keyed decisions |
| [A3](rules/a3_multilinear_box/) | Multilinear optimization over a box | Exact | Not started |
| [A4](rules/a4_bounded_least_squares/) | Bounded-variable least squares | Prototype | Not started |
| [A5](rules/a5_monotone/) | Monotone optimization | Exact | Not started |
| [A6](rules/a6_rank_one_least_squares/) | Rank-one bounded least squares | Exact | Not started |
| [N1](rules/n1_euclidean_ball/) | Euclidean-ball projection and support function | Exact | **Works**, on by default, including per `by` group |
| [N2](rules/n2_ellipsoid_support/) | Ellipsoid support function | Exact | Not started |
| [S1](rules/s1_uniform_matroid/) | Uniform-matroid optimization | Exact | **Works**, on by default, including per `by` group |
| [S2](rules/s2_laminar_matroid/) | Laminar-matroid optimization | Exact | Not started |
| [S3](rules/s3_max_cardinality_knapsack/) | Maximum-cardinality knapsack | Exact | Not started |
| [S4](rules/s4_bottleneck_knapsack/) | Cardinality-constrained bottleneck knapsack | Exact | Not started |
| [R1](rules/r1_continuous_knapsack/) | Continuous (fractional) knapsack | Exact | **Works**, on by default, including per `by` group |
| [R2](rules/r2_discrete_resource_allocation/) | Discrete separable convex resource allocation | Exact | Not started |
| [R3](rules/r3_quadratic_knapsack/) | Continuous quadratic knapsack | Prototype | Not started |
| [R4](rules/r4_max_min_fairness/) | Weighted max-min fair allocation | Exact | Not started |
| [O1](rules/o1_monge_assignment/) | Monge assignment | Prototype | Not started |
| [O2](rules/o2_monge_transportation/) | Monge transportation problem | Prototype | Not started |
| [D1](rules/d1_block_decomposition/) | Block-diagonal decomposition | Exact | Not started |

The shared engine (IR, rule driver, plan fragments, runtime checks, bound reader, result adapter) is in [framework/](framework/).

## How these docs are kept

The general rules are in [../README.md](../README.md). In this area:

- Each folder under `rules/` has a `definition.md`: the class as the catalogue defines it (worked example, recognition,
  construction, correctness, eligibility). It is the only copy. Edit it when the code proves something different.
- A class that is started also has a `todo.md`, and once something works, a `done.md`. A class not started is a row in
  [todo.md](todo.md).
- Each `done.md` starts with a stamp: the commit and command last used to verify it. Re-run and update the stamp when
  you change the behavior it describes; `test/decide/run_direct_matrix.sh` renews it in one command (about 3 minutes).
- These docs are a map. For detail, read the code in `src/optimizer/decide/direct/`.
