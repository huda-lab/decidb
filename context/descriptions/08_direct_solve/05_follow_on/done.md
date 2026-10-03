# Follow-on Work — completed work

NEXT-02: the first S1 rule now admits inclusive upper, lower, equality, or
intersecting count clauses when each bound has an exact finite proof. It uses
one rank per proved group and adds a full-partition count only for a
positive lower bound. Small independent enumeration, solver differential,
nonempty infeasibility, empty-input, outer-limit, and C++ fact-provenance
cases pass. The [five-million-row interval
measurements](../04_performance/s1_api_phase.md#global-cardinality-intervals)
show that the count guard retained the large narrow-row benefit on the tested
stored source. Other fixed-decision and wider scoped forms remain open.

NEXT-07 progress: the same rule now also admits strict comparisons and finite
consistently foldable fractional or negative bounds when their DOUBLE values
and inclusive integer limits are exact. It rounds upper limits down and lower
limits up, with the strict one-step adjustments. A negative upper limit,
fractional equality, or crossed interval raises DECIDE infeasibility for a
nonempty eligible group while empty input stays empty. Source-dependent,
non-finite, unrepresentable, and inconsistent foldable bounds still miss. Pure scalar
arithmetic such as `SUM(x)<=1+1` now has the same exact conversion proof.
Several compatible count clauses intersect into one interval. A finite
constant objective offset is allowed because it does not change the optimal
assignment. Exact unit products such as `SUM(1*x)` now share the same count
proof; data-valued or offset sum bodies still miss.
Numeric source-valued count bounds now convert to the solver's DOUBLE domain
and reduce by group: MIN for upper, MAX for lower, and equal MIN/MAX for
equality. Multiple clauses share one window stage, validate every input row
for NULL/NaN in clause order, and intersect inclusive limits with `least` and
`greatest`. Validation includes rows whose `WHEN` is false or whose `PER` key
is NULL; scoped empty-aggregate errors precede bound checks. Oracle and
both-backend differential cases cover the admitted form. Deterministic,
nonthrowing source-only numeric expressions such as `COALESCE` and `TRY_CAST`
use the same reduction and validation. Throwing source expressions remain open.
Aggregate-local `WHEN` on unit `SUM(x)` now reuses the ranking proof. Its
source-valued RHS reduces over all group rows, including filtered-out rows,
while the count and rank use filtered membership. The bound proof records that
choice per clause; mixed top-level and aggregate-local clauses keep their
respective RHS memberships.
Signed additive objective terms now use the same proof and assignment plan.
The rule converts each source-only coefficient to DOUBLE, adds signed terms
in the solver's order, and guards the resulting score on every row. Small
independent enumeration and both solver backends cover positive, negative,
and three-term scores, grouped pins, and NULL/NaN/overflow hidden by an outer
`LIMIT`. Multiple terms with a potentially throwing coefficient still miss.
The [five-million-row additive-score sweep](../04_performance/s1_api_phase.md#additive-linear-scores-at-scale)
checks grouped bounds at large scale.

The first NEXT-03 slice admits `PER` on one or more source columns and an
optional top-level deterministic source-only `WHEN`. Exact fact extraction
preserves both on each attributed constraint; the proof requires identical
membership for a paired interval. The rank and lower-bound count partition by
eligible group. NULL `PER` keys and false aggregate `WHEN` rows bypass that
bound but retain score validation and any per-row pin. Independent exhaustive
enumeration, solver differential, NULL-key, mismatch-fallback, and serializer
cases cover the admitted form. The [five-million-row grouped
sweep](../04_performance/s1_api_phase.md#grouped-cardinality-intervals)
measures 100 groups on a stored source. A later differential fixture exposed
the solver's empty-aggregate error when no row belongs to any scoped group.
The corrected direct plan counts eligible rows globally before score
evaluation and raises the same error on a nonempty input; the revised
five-million-row timing includes that check. NEXT-03 remains open for other
keyed decision shapes.

NEXT-06 progress: source-only per-row pins at exact zero or one, with optional
top-level `WHEN`, now participate in S1. The plan ranks free rows first, counts
fixed selected and free rows per eligible group, and checks the residual
interval. Active conflicting pins and unreachable group counts raise DECIDE
infeasibility. Exhaustive small assignments and both solver backends cover
global and grouped bounds, `WHEN` bypass, NULL `PER` keys, and both objective
senses. Other ways to fix a Boolean remain outside this admission.
The [five-million-row fixed-pin sweep](../04_performance/s1_api_phase.md#fixed-boolean-pins-at-scale)
matches Gurobi's selected count and objective in every run and measures
0.326–0.330-second direct versus 36.256–37.517-second Gurobi
query/collection medians on stored grouped sources.

No second rule, ANR adapter, or cost-based direct selection is implemented.
