# Direct Solve Research Notes

This file preserves the technical conclusions behind the reader-facing catalogue. It
is not part of the default reading path and is not an implementation specification.

The research establishes mathematical constructions and motivates future optimizer
work. No direct-rewrite rule is implemented in current DeciDB.

## Correctness contract

For an input relation `R`, a DECIDE query defines a feasible set `F(R)` and objective
`f(R,x)`. A direct translation is correct when it preserves all observable outcomes
inside the class it admits.

### Feasible bounded optimization

The relational plan must return an assignment `x_direct` such that:

- `x_direct` belongs to `F(R)`; and
- its primary objective is optimal under DECIDE's numeric policy.

The assignment does not need to equal the vector returned by a solver when several
assignments tie. Stable relational tie-breaking is allowed if it does not change the
primary optimum.

### Feasibility-only queries

The plan may return any feasible assignment. It must not manufacture an assignment when
the original DECIDE problem is infeasible.

### Relational behavior

The plan must preserve:

- result schema and types;
- source-row cardinality;
- row, entity, and scalar decision identity;
- mapping from one computed decision value to every corresponding source row; and
- the surrounding SQL projection, join, filter, and ordering behavior.

### Outcomes and fallback

Unsupported syntax, unknown facts, or a failed proof are detector misses, not
infeasibility. They leave the query on the existing solver path.

An admitted plan must preserve applicable behavior for:

- NULL and nonfinite solver inputs;
- empty input;
- inconsistent bounds;
- infeasibility and unboundedness;
- numeric conversion and tolerance;
- interruption and error propagation; and
- diagnostics and forced-solver controls.

## Admission evidence

Semantic eligibility may use only exact information:

- binder-resolved normalized DECIDE expressions;
- declared decision domains and scopes;
- literals and exact algebraic consequences;
- exact catalog constraints interpreted with SQL NULL semantics; and
- keys or functional dependencies proved to survive the child logical plan.

The following are useful for costing but cannot prove correctness:

- estimated cardinality;
- selectivity;
- sampled distinct counts;
- approximate column statistics; and
- a property that merely happens to hold in the current rows.

For example, computing `COUNT(DISTINCT key)` inside a generated plan does not make
uniqueness known when the optimizer chooses the rewrite.

## Shared semantic requirements

These requirements apply across the catalogue even when the main translation cards do
not repeat their explanations. Every applicable admission condition is nevertheless
listed in that rule's **Valid when** section, so a rule can be audited in one place.

### Decision identity

- A row decision is keyed by stable internal child-row identity.
- An entity decision is keyed by its complete entity tuple.
- A scalar decision has one query-wide identity.
- A user-provided `id` column is illustrative, not automatically unique.

Objective and constraint coefficients must be collected per actual decision identity.
Join duplicates do not automatically create independent decisions.

### `PER` and `WHEN`

DECIDE rows whose `PER` key is NULL bypass that factor. SQL `PARTITION BY NULL` would
instead create a NULL group, so generated plans need an explicit bypass branch.

A `WHEN` filter changes factor membership. Two factors may be decomposed or nested only
when their masks are proved compatible.

### Data-valued right-hand sides

For an aggregate constraint, DECIDE reduces row-varying right-hand sides by direction:

- `expression >= rhs` uses the group's `MAX(rhs)`;
- `expression <= rhs` uses the group's `MIN(rhs)`; and
- equality requires both directions to agree.

A ranked row's local RHS value is not automatically the group's effective bound.

### Empty input

A zero-row child instantiates no decision or aggregate-factor row. Generated SQL must
not manufacture a scalar row through an aggregate and thereby change the source-level
result.

### Numeric domains

- Effective INT intervals are `[CEIL(lower), FLOOR(upper)]`.
- REAL constructions need tolerance-safe feasibility and residual checks.
- BOOL, INT, and REAL rules are not interchangeable merely because they share sorting
  or prefix operators.

## Validation evidence

The experimental campaigns validate mathematical kernels, not optimizer integration.
Timeouts, solver limits, unsupported capabilities, and skipped cases were not counted
as successful oracle comparisons.

The scratch validators listed below were temporary and are not currently present in
the repository. Their recorded totals are historical evidence, not independently
reproducible acceptance evidence. Permanent tests must reproduce every claimed family
before implementation is enabled.

| Campaign | Coverage | Result |
|---|---|---|
| Mixed analytic/Gurobi | 17,900 cases across algebraic, selection, allocation, norm, and ordered rules | All assertions passed |
| Discrete exhaustive | 30,095 Boolean, integer, quota, prefix, and transport cases | All assertions passed |
| Current DECIDE/Gurobi versus SQL | 160 continuous cases | Maximum relative difference `5.51e-7` |
| Supported forced-HiGHS subset | 110 continuous cases | Maximum relative difference `1.78e-15` |
| Current SQL smoke file | 15 representative DECIDE/SQL sections | Primary objectives or formula parameters matched |

### Per-family evidence

| Rules | Main evidence | Important remaining gap |
|---|---|---|
| D1 | Product-set proof and independent leaf campaigns | Negative tests for overlapping identities and factors |
| A1 | 3,000 BOOL/INT/REAL comparisons | Objective-direction and lattice-neighbor regressions |
| A2 | 1,900 analytic/exhaustive checks and 30 DECIDE comparisons | L0 forbidden-gap and zero-weight regressions |
| A3 | 240 random multi-affine boxes | Final dimension and plan-size cap |
| A4 | 1,120 bounded regressions | Singular and ill-conditioned numeric policy |
| A5 | 900 exhaustive bounded-integer models | Complete live infeasibility guard |
| A6 | 960 dimensions 1--12 | Mixed-scope and zero-coefficient mapping |
| N1 | L1/L2 projection and support comparisons | Signed/nonnegative variants and tied thresholds |
| N2 | 720 diagonal ellipsoid comparisons | Numeric QCQP boundary and accepted syntax |
| S1 | 720 exhaustive cardinality cases | NULL groups, varying RHS, and entity fan-out |
| S2 | 16,000 nested-quota cases and 60 current comparisons | Deeper hierarchies and NULL bypass |
| S3 | 5,220 signed-cost exhaustive cases | Complete empty and infeasible outcomes |
| S4 | 900 exhaustive anchor cases | `k=0`, varying parameters, and mapping |
| R1 | 800 linear and 40 piecewise backend cases | General segment extraction and residual repair |
| R2 | 3,600 exhaustive integer allocations | Upper-inequality sign cutoff, expansion cost, and overflow |
| R3 | 960 event-scan and 120 backend comparisons | Equal breakpoints and final residual repair |
| R4 | 40 backend comparisons | Repeated-entity collapse and empty outcomes |
| O1 | 1,800 exhaustive and 90 targeted cases | Pair-support provenance and full-output cost |
| O2 | 2,560 local, 995 integer-mass theorem checks, and 40 targeted cases | Balance provenance, residuals, and output cost |

Every admitted rule still needs permanent positive and negative plan-selection tests,
complete assignment validation, edge semantics, serialization/prepared-statement
coverage, interruption behavior, and an assertion that no solver model is instantiated.

## Performance motivation

Existing pipeline measurements show potential rather than integrated direct-path
speedups. Model construction, backend loading, and solving are the maximum removable
work; the generated relational plan still has its own execution and mapping cost.

Representative existing medians:

| Workload | Gurobi query | HiGHS query | Relevant class |
|---|---:|---:|---|
| Bench-P1 independent linear | `0.626 s` | `0.806 s` | A1/A5 |
| Bench-P2 separable quadratic | `0.243 s` | `300.275 s` solver limit | A1 |
| Bench-P3 coupled quadratic | `1.004 s` | `300.329 s` solver limit | R3 |
| Bench-P4 one per group | `0.548 s` | `82.499 s` | S1 |
| Q11 at 136K rows | `0.235 s` | `0.936 s` | R1 |
| Q9 at 15K rows | `0.204 s` | `19.659 s` | S4 |

Solver-limit rows are incomplete evidence and are not timings of a proven optimum.

The large campaign also identified upper bounds on removable solver-pipeline work:

- P4/HiGHS: `82.319 s`;
- Q9/HiGHS: `19.648 s`; and
- P1/Gurobi: `0.530 s`.

Recognition must occur before solver-neutral model construction to avoid that work.

## Future implementation notes

These notes preserve the researched architecture without making it part of the main
translation explanation.

### Analysis boundary

Direct analysis belongs at the logical DECIDE boundary, before:

- backend selection;
- solver-specific reformulation;
- prepared-model construction; and
- input materialization for the solver.

Matching inside the solver facade is too late because much of the target work has
already happened.

### Rule interface

Each future rule needs six responsibilities:

| Step | Responsibility |
|---|---|
| Match | Identify the normalized objective, constraints, domains, and interaction shape |
| Prove | Establish every validity condition from exact facts |
| Rewrite | Construct a logical subtree from existing DuckDB operators |
| Map | Attach every decision value to the correct output rows |
| Cost | Compare already-correct direct and solver plans |
| Explain | Report the selected rule and proof conditions |

Recognition is atomic. A partial match must not mutate the original logical plan.

### Initial policy

- `DIAGNOSE` stays on the solver path because a relational optimum does not retain the
  evidence required for diagnosis.
- `DECIDB_FORCE_SOLVER` disables direct rewrites for differential testing and
  benchmarking.
- Only existing DuckDB relational and scalar operators are in scope.
- No adaptive runtime switch to a solver is assumed after execution begins.

### Common foundation

Before a leaf rule ships, the optimizer needs:

- normalized decision identity and coefficient extraction;
- exact fact tracking;
- D1 component analysis;
- typed assignment relations;
- DECIDE-compatible error and status guards;
- rule-level `EXPLAIN`; and
- a no-solver execution assertion.

## Temporary evidence

The following scratch artifacts were used during the research. They are not repository
tests and may disappear from `/tmp`.

| File | SHA-256 |
|---|---|
| `/tmp/validate_decide_rewrites.py` | `30ca53b79130a2e632e37ea8bece818b6701d8a022463b52d279f5e5bfb37f54` |
| `/tmp/decidb_discrete_rewrite_validation.py` | `28bcd3fb740bc0a97317ed210fae704c8cece0414100214d7fbed691ede4266d` |
| `/tmp/validate_continuous_rewrites.py` | `4d165f4645c62f6aded363f56ab613f5eb4bc01703ed8ff76b2bede8ff815f19` |
| `/tmp/decide_rewrite_smoke.sql` | `b145a96958818a03b4d3aff469f57f32dce130672a55b27351cd81298c9a13eb` |
| `/tmp/validate_doc_snippets.sql` | `25d34182bc9b7b02ba8027549b9a708978010a58a50a9501f497b6cc69876c0a` |
| `/tmp/validate_hierarchical_quotas.py` | `e67866fb01bd48248ad3899d50b71857098c21f492b6c5cf09e136b8d8df7327` |
| `/tmp/validate_monge_transport.py` | `e7fa96ba7001e7a472debe3a9ab7be3525970243fb8a7ad2383cb0d487f252bf` |
| `/tmp/validate_ordered_assignment.py` | `c92af9039097e30d3f10ce745c97279b190d1c72d86b79dfb67f840f9c9f23dc` |

## Sources

### Repository sources

- [`../00_project_overview/syntax_reference.md`](../00_project_overview/syntax_reference.md)
  defines current DECIDE syntax and semantics.
- `src/optimizer/optimizer.cpp` and `src/optimizer/decide/` define the current logical
  optimization boundary.
- `src/execution/physical_plan/plan_decide.cpp` begins prepared solver-model construction.
- `benchmark/decide/results/pipeline_profile_large.csv` contains the large pipeline
  measurements summarized above.

### Algorithm references

- Duchi et al., [Efficient Projections onto the L1-Ball for Learning in High
  Dimensions](https://research.google/pubs/efficient-projections-onto-the-l1-ball-for-learning-in-high-dimensions/).
- Edmonds, [Matroids and the Greedy Algorithm](https://doi.org/10.1007/BF01584082).
- Zipkin, [Simple Ranking Methods for Allocation of One
  Resource](https://pubsonline.informs.org/doi/10.1287/mnsc.26.1.34).
- Klinz and Woeginger, [The Northwest-Corner Rule for Monge Transportation
  Problems](https://www.math.tugraz.at/fosp/pdfs/tugraz_9972.pdf).
- Dowling and Gallier, [Linear-Time Algorithms for Testing the Satisfiability of
  Propositional Horn Formulae](https://www.seas.upenn.edu/~cis5110/Dowling-Gallier-Horn-sat.pdf).
- Aspvall, Plass, and Tarjan, [A Linear-Time Algorithm for Testing the Truth of Certain
  Quantified Boolean Formulas](https://doi.org/10.1016/0020-0190(79)90002-4).

## Remaining uncertainty

- Which key and functional-dependency facts DuckDB preserves strongly enough for the
  proposed recognizers.
- Whether generated error guards can reproduce DECIDE's error class and evaluation
  precedence.
- Floating accumulation, equal-breakpoint grouping, and residual repair at numeric
  limits.
- Integrated performance after full assignment mapping and normal DuckDB optimization.
- Cost limits for corner enumeration, integer-unit expansion, and pair-output rules.

These are reasons to retain the solver fallback and to implement the first wave
conservatively; they do not invalidate the mathematical catalogue.
