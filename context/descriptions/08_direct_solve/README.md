# Direct solve

Direct solve answers some DECIDE problems without a solver. When a problem matches a class it can prove, the optimizer
replaces the DECIDE node with ordinary DuckDB operators (sort, rank, filter) that compute the optimal assignment. Any other
problem takes the existing solver path, unchanged. It is **on by default** (`decide_direct_solve = 'auto'`); `off` forces
the solver and `require` raises an error saying why a query was not proved.

Two problem classes work: **S1**, "choose K rows by score" (count bounds on one `BOOL` variable, optionally per
group), and **A1**, "each row gets its best value in its own range" (per-row bounds on `BOOL`, `INT` or `REAL` decisions
with a linear or one-square objective). On 5M-row sources S1 is about 15 to 100 times faster than Gurobi and A1 about 4 to
30 times, on those shapes. The other 17 classes in the catalogue are not started.

## Where to look

| I want to... | Go to |
|---|---|
| Understand how it works | [architecture/overview.md](architecture/overview.md), then the other files there |
| Look up a term, its standard name and the identifiers it covers | [architecture/glossary.md](architecture/glossary.md) |
| Add a new problem class | [architecture/rules.md](architecture/rules.md), then a new folder under `rules/` |
| See what S1 covers, how it is tested, how fast it is | [rules/s1_uniform_matroid/done.md](rules/s1_uniform_matroid/done.md) |
| See what A1 covers, where it differs from the solver, how fast it is | [rules/a1_separable_box/done.md](rules/a1_separable_box/done.md) |
| See what is open for S1 or A1 | [rules/s1_uniform_matroid/todo.md](rules/s1_uniform_matroid/todo.md), [rules/a1_separable_box/todo.md](rules/a1_separable_box/todo.md) |
| Work on the shared engine, a third rule, error wording | [framework/](framework/) |
| See a class that is not built yet | `rules/<id>_<slug>/todo.md` (table below) |
| Know why `auto` is the default, or what a user can notice | [architecture/policy.md](architecture/policy.md) |
| Read the definition and correctness argument of a class | `rules/<id>_<slug>/definition.md` |
| Check current DECIDE syntax | `../00_project_overview/syntax_reference.md` |

## Status by class

| Class | Name | Catalogue status | Direct solve |
|---|---|---|---|
| [A1](rules/a1_separable_box/) | Separable box-constrained problem | Exact | **Works**, on by default |
| [A2](rules/a2_single_facility_location/) | Single-facility location (1-D) | Exact | Not started |
| [A3](rules/a3_multilinear_box/) | Multilinear optimization over a box | Exact | Not started |
| [A4](rules/a4_bounded_least_squares/) | Bounded-variable least squares | Prototype | Not started |
| [A5](rules/a5_monotone/) | Monotone optimization | Exact | Not started |
| [A6](rules/a6_rank_one_least_squares/) | Rank-one bounded least squares | Exact | Not started |
| [N1](rules/n1_euclidean_ball/) | Euclidean-ball projection and support function | Exact | Not started |
| [N2](rules/n2_ellipsoid_support/) | Ellipsoid support function | Exact | Not started |
| [S1](rules/s1_uniform_matroid/) | Uniform-matroid optimization | Exact | **Works**, on by default |
| [S2](rules/s2_laminar_matroid/) | Laminar-matroid optimization | Exact | Not started |
| [S3](rules/s3_max_cardinality_knapsack/) | Maximum-cardinality knapsack | Exact | Not started |
| [S4](rules/s4_bottleneck_knapsack/) | Cardinality-constrained bottleneck knapsack | Exact | Not started |
| [R1](rules/r1_continuous_knapsack/) | Continuous (fractional) knapsack | Exact | **Works**, on by default |
| [R2](rules/r2_discrete_resource_allocation/) | Discrete separable convex resource allocation | Exact | Not started |
| [R3](rules/r3_quadratic_knapsack/) | Continuous quadratic knapsack | Prototype | Not started |
| [R4](rules/r4_max_min_fairness/) | Weighted max-min fair allocation | Exact | Not started |
| [O1](rules/o1_monge_assignment/) | Monge assignment | Prototype | Not started |
| [O2](rules/o2_monge_transportation/) | Monge transportation problem | Prototype | Not started |
| [D1](rules/d1_block_decomposition/) | Block-diagonal decomposition | Exact | Not started |

The shared engine (IR, rule driver, plan fragments, runtime checks, bound reader, result adapter) is in [framework/](framework/).

## How these docs are kept

- Each folder under `rules/` has a `todo.md` (open work, atomic tasks) and, once something ships, a `done.md` (what is true
  now, in present tense, with the tests that verify it and a summary of measurements).
- A finished task leaves `todo.md` and becomes a line in `done.md`. Nothing appears in both.
- A `done.md` states only what is true now, with no history (no "was", "shipped", "fixed on"). A `todo.md` holds only work
  still to do, plus the open decisions that block it.
- Each `done.md` starts with a stamp: the commit, date and command last used to verify it. Re-run and update the stamp when
  you change the behavior it describes.
- These docs are a high-level map. For detail, read the code in `src/optimizer/decide/direct/`.
- Each folder under `rules/` also has a `definition.md`: the class as the catalogue defines it (worked example,
  recognition, construction, correctness, eligibility). It is the only copy. Edit it when the code proves something
  different.
