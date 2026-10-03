# First Rule: Prototype Contract

This is the *implementation admission* for a small part of S1, not a second
definition or proof of the class. The formal class catalogue is the
[Word document](../decidb_direct_relational_rewrites.docx). If any condition
below is unproved, the rule misses and DECIDE follows the solver path.

## Admitted shape

- Exactly one user decision variable, row-scoped and declared `BOOL`; no
  auxiliary, entity-scoped, or scalar decision. Its SQL output is `INTEGER` 0/1.
- One or more cardinality clauses `SUM(x) <= U`, `< U`, `>= L`, `> L`, or
  `SUM(x) = K` over unqualified unit-contribution sums and identical
  membership. `SUM(1*x)` is equivalent; weighted or offset sum bodies miss.
  Their inclusive limits intersect into one interval. The default lower bound
  is zero and a lower-only shape has no explicit upper bound. There are no
  other decision terms. Bounds may be numeric source-only expressions whose
  evaluation cannot throw, or foldable constants.
  A finite folded constant may shift the objective without changing which
  assignment is optimal. The objective may contain one or more signed,
  unfiltered `SUM(coefficient*x)` terms. Other objective forms remain solver
  cases.
- A bound can be global, keyed by one or more source-column `PER` keys, or
  restricted by a top-level deterministic source-only `WHEN` predicate. A
  bound may instead use aggregate-local `WHEN` on `SUM(x)`. All
  cardinality clauses must have structurally identical `PER` keys and `WHEN`
  membership. NULL `PER` keys and rows outside the aggregate's
  `WHEN` bypass that bound but still obey any per-row pin. A nonempty source
  with no eligible row for a scoped aggregate raises DECIDE's empty-aggregate
  error. Per-row Boolean pins `x=0`, `x<=0`,
  `x<1`, `x=1`, `x>=1`, or `x>0` may also appear, with an optional deterministic,
  source-only top-level `WHEN` and no `PER`. Any number of pins may overlap;
  contradictory active pins are infeasible. No reducer qualifier, frame,
  nested reducer, other constraint, or `DIAGNOSE`. The matcher
  examines the entire canonical trees, not original SQL text.
- Source-independent numeric bound expressions that DuckDB can fold
  consistently and convert exactly to finite DOUBLE, or deterministic
  source-only numeric expressions whose evaluation cannot throw. `TRY_CAST`
  conversion failures become NULL and are validated like other bound results;
  `COALESCE` may supply a valid value for a nullable column.
  For an integral count, normalize `<= U` with `floor(U)`, `< U` with
  `ceil(U)-1`, `>= L` with `ceil(L)`, and `> L` with `floor(L)+1`.
  Lower limits at or below zero become zero. The admitted nonnegative limits
  are at most `2^53`; larger or inexact folded constants and unproved expressions
  remain solver cases. Source-valued results use the solver's DOUBLE conversion
  before group reduction; fractional values and infinities are admitted. The
  plan checks NULL and NaN on every input row after the scoped empty-aggregate
  check and before any score evaluation.
  An upper bound takes the group minimum; a lower bound takes the group
  maximum. Equality also checks that every eligible value in its group agrees.
  Throwing arithmetic and other unproved expressions still miss. The DECIDE
  binder currently rejects `CASE` as a reduced constraint's bound.
  A source-valued RHS under aggregate-local `WHEN` reduces over all group
  rows, including rows excluded from the count. The proof records that distinct
  RHS membership for its MIN/MAX window; count ranking remains filtered.
  A negative upper limit, fractional equality, or lower limit above upper
  limit is infeasible for a nonempty eligible group and must raise DECIDE's
  infeasibility error.
- Each term has a numeric, decision-free per-row coefficient. Data-only casts
  retain their SQL meaning; each value is converted to the solver's DOUBLE
  domain before signed terms are added in order. Volatile or side-effecting
  expressions miss. Multiple terms require nonthrowing coefficients; a
  throwing coefficient in a single term is allowed only when the generated
  plan retains its ordinary DuckDB error on every relevant input row.

No source-row uniqueness claim is needed. A row-scoped decision belongs to each
input row even when two rows have identical data. Exact identity and fan-out
must be preserved by the result contract.

## Proposed construction

```text
DECIDE input (including its own WHERE)
  -> compute bound eligibility and per-row Boolean pin flags
  -> for a scoped aggregate, count eligible rows globally and report an
     empty-aggregate error if a nonempty source has none
  -> if present, validate each source-valued bound on every row, then reduce
     it by group, check equality variation clause-wide, and intersect limits
     before scoring
  -> evaluate typed coefficients, convert each to DOUBLE, and add signed terms
     into one row score in solver order
  -> truth-or-error guard for score NULL / non-finite
  -> rank free rows first by score within each eligible group; count fixed
     selected rows and, for a positive or source-valued lower bound, free rows
  -> fail on pin conflicts or an infeasible residual interval
  -> apply pins, then choose improving free rows outside bound membership
  -> project every source row plus an INTEGER 0/1 assignment
  -> shared result boundary with explicit output-slot map
```

For maximization, rank coefficients descending; for minimization, ascending.
Within each eligible group, subtract the fixed selected count from `L` and
`U`. For free rows, select the best prefix through the remaining lower limit,
then any improving rows through the remaining upper limit. Fixed zero rows stay
zero, and fixed selected rows stay one. This chooses the best feasible prefix
without a second positive-score count. An upper-only bound remains feasible on empty input and
when fewer than `U` rows improve the
objective. The current solver path returns no rows for empty input even with a
positive lower bound; the relational count guard does the same. A nonempty
eligible group with fewer than `L` rows reports DECIDE infeasibility. Boundary
ties may choose different rows with the same primary optimum. A filtering
`ORDER BY ... LIMIT` result is invalid because it drops the unchosen source rows.

The rule returns one typed value per row and preserves the original result
schema and row count, even when the parent does not read the decision column.
The mandatory guard checks relevant coefficients even at zero upper bound and
when all coefficients worsen the objective. The rank or a separate blocking
validator must remain live in those cases; the [experiment](../00_design/experiments.md)
showed that a guard alone can miss a late invalid row under an outer limit.
The implementation must prove the complete zero-capacity assignment, because
the temporary bound-plan probe tested validation there with a fixed rank
threshold of one.
