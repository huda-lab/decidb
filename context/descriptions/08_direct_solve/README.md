# Direct solve

Direct solve answers some DECIDE problems without a solver. When a problem matches a class it can prove, the optimizer
replaces the DECIDE node with ordinary DuckDB operators (sort, rank, filter) that compute the optimal assignment. Any other
problem takes the existing solver path, unchanged. It is **on by default** (`decide_direct_solve = 'auto'`); `off` forces
the solver and `require` raises an error saying why a query was not proved.

Today one problem class works: **S1**, "choose K rows by score" (count bounds on one `BOOL` variable, optionally per
group). On 5M-row sources it is about 15 to 100 times faster than Gurobi on those shapes. The other 18 classes in the
catalogue are not started. The working branch is `direct-solve-prototype`.

## Where to look

| I want to... | Go to |
|---|---|
| Understand how it works | [architecture/overview.md](architecture/overview.md), then the other files there |
| Add a new problem class | [architecture/rules.md](architecture/rules.md), then a new folder under `rules/` |
| See what S1 covers, how it is tested, how fast it is | [rules/s1/done.md](rules/s1/done.md) |
| See what is open for S1 | [rules/s1/todo.md](rules/s1/todo.md) |
| Work on the shared engine, a second rule, error wording | [rules/_harness/](rules/_harness/) |
| See a class that is not built yet | `rules/<class>/todo.md` (table below) |
| Know why `auto` is the default, or what a user can notice | [architecture/policy.md](architecture/policy.md) |
| Read the definition and correctness argument of a class | `rules/<class>/definition.md` |
| Check current DECIDE syntax | `../00_project_overview/syntax_reference.md` |

## Status by class

| Class | Name | Catalogue status | Direct solve |
|---|---|---|---|
| [A1](rules/a1/) | Independent bounded decisions | Exact | Next (second rule) |
| [A2](rules/a2/) | One shared mean, median, midpoint, or mode | Exact | Not started |
| [A3](rules/a3/) | Fixed-small multi-affine box | Exact | Not started |
| [A4](rules/a4/) | Bounded two-parameter least squares | Prototype | Not started |
| [A5](rules/a5/) | Objective-aligned monotone corner | Exact | Not started |
| [A6](rules/a6/) | One squared affine residual over a box | Exact | Not started |
| [N1](rules/n1/) | Projection or linear support under one sum-of-squares bound | Exact | Not started |
| [N2](rules/n2/) | Linear support over a diagonal ellipsoid | Exact | Not started |
| [S1](rules/s1/) | Top-k and cardinality intervals | Exact | **Works**, on by default |
| [S2](rules/s2/) | Nested upper quotas | Exact | Not started |
| [S3](rules/s3/) | Maximum item count under one budget | Exact | Not started |
| [S4](rules/s4/) | Exact count and budget, maximizing the best selected item | Exact | Not started |
| [R1](rules/r1/) | Continuous piecewise-linear allocation | Exact | Not started |
| [R2](rules/r2/) | Bounded integer marginal units | Exact | Not started |
| [R3](rules/r3/) | Strictly convex quadratic allocation | Prototype | Not started |
| [R4](rules/r4/) | Proportional max-min fairness | Exact | Not started |
| [O1](rules/o1/) | Ordered complete assignment | Prototype | Not started |
| [O2](rules/o2/) | Balanced one-dimensional Monge transport | Prototype | Not started |
| [D1](rules/d1/) | Exact keyed decomposition | Exact | Not started |

The shared engine (facts, coordinator, builders, result boundary) is in [rules/_harness/](rules/_harness/).

## How these docs are kept

- Each folder under `rules/` has a `todo.md` (open work, atomic tasks) and, once something ships, a `done.md` (what is true
  now, in present tense, with the tests that verify it and a summary of measurements).
- A finished task leaves `todo.md` and becomes a line in `done.md`. Nothing appears in both.
- Each `done.md` starts with a stamp: the commit, date and command last used to verify it. Re-run and update the stamp when
  you change the behavior it describes.
- These docs are a high-level map. For detail, read the code in `src/optimizer/decide/direct/`.
- Each folder under `rules/` also has a `definition.md`: the class as the catalogue defines it (worked example,
  recognition, construction, correctness, eligibility). It is the only copy; the Word catalogue it came from was removed
  on 2026-10-05 and is in git history at `721d49ba43`. Edit it when the code proves something different.
