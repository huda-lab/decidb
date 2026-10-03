# Correctness — completed work

VAL-00 is the **pre-feature, current-syntax solver baseline** in
[baseline.md](baseline.md). Both Gurobi and HiGHS were available for its
representative cases. The temporary boundary probe and its limits are in
[design/experiments.md](../00_design/experiments.md).

The permanent `test/decide/tests/test_direct_solve.py` checks direct path
selection, independent enumeration of small optima, exact capacities, all 17
documented structural near misses, unknown fact wrappers, late runtime errors,
parent/materialized-CTE contexts, serializer round trips, logical and physical
explanation, profiling, prepared selection and real rebind, forced solver
policy, nested/correlated decisions, and direct/HiGHS/Gurobi primary-objective
agreement on separated scores and tiny-score backend gaps. The latest full
DECIDE run passed 1,856 tests; the serializer-verification run passed the same
1,856 tests. Direct solve is on by default (`auto`), so that run exercises it:
`test_direct_solve_is_on_by_default` issues no `SET`, and every other test that
needs a specific path pins `require` (assert a hit) or `off` (the solver reference).
The [built-in optimizer audit](../00_design/optimizer_audit.md) records the
pass-by-pass boundary argument and discriminating parent tests.
The latest forced-HiGHS suite passed 1,855 tests and retained its one unrelated
`test_norm_combined_l1_l2_objective` failure: that MIQP objective needs Gurobi.
The DECIDE C++ suite passed 698 assertions in 20 cases, including direct
fact-adapter and S1 proof-contract cases.

Global lower, equality, and paired interval regressions compare complete
assignments with independent enumeration and solver-path objective values. The
matrix also checks intersection of several compatible count clauses,
unit-contribution products such as `SUM(1*x)`,
nonempty infeasibility through an outer aggregate and limit, the
current solver's empty-input outcome, reversed comparison spelling, and
source-clause attribution. The 5M-row performance sweep agrees with Gurobi
on selected count and primary objective in every completed paired run.

Scoped regressions independently enumerate `PER` groups, including multiple
keys and NULL-key bypass, and top-level `WHEN` membership. They compare both
objective senses, lower/exact/paired bounds, and unconstrained decisions with
the solver. Mismatched paired scopes miss and leave the solver path intact.
They also check the solver's empty-aggregate error when no scoped row is
eligible, including its precedence over an invalid score.
Serializer verification covers an admitted scoped plan. The 5M-row grouped
interval sweep agrees with Gurobi on selected count and objective in every
paired run.

Strict, fractional, negative, crossed, and consistently foldable scalar bounds
are checked against inclusive count normalization, exhaustive small optima,
both solvers, and nonempty infeasibility. Empty input retains the solver's
no-row outcome.

Fixed-decision regressions independently enumerate all assignments on a small
source and compare the primary objective with both solvers. They check per-row
zero/one pins, multiple pins, both objective senses, residual group bounds,
contradictory pins, infeasible counts, `WHEN` bypass (including NULL), NULL
`PER` keys, empty input, late errors under an outer limit, and error
precedence. Serializer verification covers an admitted pin
plan.
Finite constant objective offsets are checked under both senses against
independent enumeration and both solvers; they shift the objective value
without changing the optimal assignment.
Signed additive objective terms are checked against independent enumeration
and both solvers under both senses. The tests include three terms, unary
negation, grouped bounds with a fixed row, and NULL/NaN/overflow in a late row
under `LIMIT`. A potentially throwing coefficient in a multi-term score must
still miss and retain the solver path.
Source-valued bound fallback regressions check that NULL on a bypassed `WHEN`
or NULL-key `PER` row still errors, while an empty scoped aggregate errors
before an invalid bound or score.
For admitted numeric source-valued bounds, small exhaustive oracles and
both solver backends check global/grouped upper, lower, strict, and equality
forms, matching constant bounds, pins, both objective senses, and infeasible
groups. Boundary tests cover fractional limits, infinities, NaN, BIGINT above
`2^53`, and equality after solver-style DOUBLE conversion. Direct-path tests
also cover varying equality values, NULL/NaN on bypassed rows, a late NULL
under `LIMIT`, and bound-before-score error order.
Multiple dynamic clauses share one window stage; independent enumeration and
both solver backends cover paired upper/lower bounds, intersecting same-side
bounds, strict bounds, three global clauses, pins, and clause-ordered errors.
`COALESCE` source bounds are checked against an independent optimum and both
solvers, including nullable columns and two expression bounds. `TRY_CAST`
failures and NaN results on bypassed rows retain the solver's error.
Aggregate-local `WHEN` on `SUM(x)` is checked against independent enumeration
and both solvers for grouped constant and source-valued intervals, including
NULL-key bypass and empty-aggregate error order. A restrictive RHS value on a
filtered-out row proves the distinct RHS membership. Mixed top-level and
aggregate-local source bounds, equality variation in an inactive-only group,
and NULL on a bypassed row exercise the separate bound partitions.

The wide-output regressions prove that an outer aggregate can drop an unused
stored payload from a scan or inner comparison join while retaining the rank,
result bindings, and late score guard. They compare direct with the solver
when an unused computed payload calls `error()`: both paths must raise.
Projected aliases through filters and joins are covered, and the pruned plans
pass serializer verification.

VAL-01: bound-plan C++ cases check exact and unknown facts, source-clause
attribution, a complete S1 proof, and proof rejection for missing attribution,
unrepresentable bounds, and a volatile coefficient. SQL cases check typed output
bindings, an unchanged solver plan on `auto` misses, forced modes, and absence
of model work on a direct hit.

VAL-02: the first-rule matrix covers both objective senses, zero/one/oversized
capacity, empty input, mixed signs, signed zero, subnormal scores, duplicate
rows, ties, the `2^53` bound boundary, and structural near misses. Small
positive cases are checked against independent exhaustive enumeration.

VAL-03: late NULL, NaN, infinity, and throwing-cast fixtures check that a
consumed DECIDE result validates all relevant input rows when a parent uses
`LIMIT 1`, `COUNT(*)`, projection, filter, join, or a materialized CTE. An
input `WHERE` removes an invalid row; outer `LIMIT 0` can skip execution. The
parent join fixture has a higher-scoring row and a late invalid row outside the
join result, making a pushed join filter observably wrong.

VAL-03 also pins the wording: for NULL, NaN, and infinite scores the direct error equals the
solver's text (a NULL score names its column; the solver's "at row N" is the one difference),
so a user sees one message whichever path ran. A computed score such as `(a + b) * x` keeps the
solver's generic NULL wording because the plan has no failing row to inspect.

VAL-04: optimized logical and default physical plans and profiling expose the
selected rule. Serializer-verification and prepared-rebind cases preserve
binding, type, and selection behavior. A direct hit has a window and ordinary
physical projection, without `PhysicalDecide`; a model-dump hook is untouched.

VAL-05: the permanent tiny-score fixture checks direct against the exact
finite-DOUBLE optimum at `5e-324`, `1e-12`, `1e-9`, `1e-8`, `1e-7`, and `1e-6`.
On this machine, both backends returned zero at the first two magnitudes;
HiGHS also returned zero through `1e-7`, while Gurobi selected the positive
row from `1e-9` upward. The largest measured absolute primary-objective gap
was `1e-7`; the fixture permits that gap for backend comparison while requiring
the direct result to attain the exact optimum. Relative gaps can be 100% near
zero. The well-separated differential cases and independent enumeration remain
the primary semantic checks.

The historical scratch validators remain
research evidence only.

VAL-06: `test_direct_solve_oracle.py` checks 13 direct results against the project's
independent ILP oracle (`oracle_solver`), which builds its model straight from the rows.
The cases cover global upper, lower, fractional, and interval bounds, exact and
`PER`-grouped bounds, `WHEN` membership, `PER` with `WHEN`, source-valued bounds (the
oracle adds one constraint per row, so it never computes the group minimum or
maximum the direct rule uses), and per-row pins with group and source-valued bounds,
under both objective senses. Each compares schema, row count, primary objective, and
the decision vector (alternate optima are accepted). One more case checks that an
infeasible group is infeasible for the oracle and raises DECIDE infeasibility on the
direct path. A mutation check (forcing the SQL sense to MAXIMIZE) made exactly the three
MINIMIZE cases fail. The oracle cache keys on the test function's source only, so editing
the case table or model builder does not invalidate it; the file's docstring says how to
clear the entries.

VAL-07: eight seeded fuzz tests in the same file generate random small S1 queries (NULL
keys and bounds, ties, `PER`, top-level and aggregate-local `WHEN`, constant,
fractional, and source-valued bounds, pins, three objectives, both senses) and run each
under `require` (direct) and `off` (solver path). A shape the matcher does not prove is
skipped as a miss. Otherwise both paths must succeed or fail together, failures must have
the same class (infeasible, empty aggregate, NULL bound, and so on), and successes must
agree on row count and primary objective. The selected count is not compared, because a
tied zero-contribution row can differ. Each seed must reach the direct path on at least
70% of its queries and compare at least 20% successfully, and a generated parser error
fails the test. The last guard was added because the review-time fuzz wrote `PER g WHEN
flag`, which the grammar rejects (`WHEN flag PER g` is correct), so every PER-with-WHEN
query was a parser error on both paths and silently matched.
