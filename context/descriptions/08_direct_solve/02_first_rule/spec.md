# First Rule: Prototype Contract

This is the *implementation admission* for a small part of S1, not a second
definition or proof of the class. The formal class catalogue is the
[Word document](../decidb_direct_relational_rewrites.docx). If any condition
below is unproved, the rule misses and DECIDE follows the solver path.

## Admitted shape

- Exactly one user decision variable, row-scoped and declared `BOOL`; no
  auxiliary, entity-scoped, or scalar decision. Its SQL output is `INTEGER` 0/1.
- One canonical global upper-cardinality constraint and one additive linear
  objective in that variable. There are no other decision or data-only terms
  that the solver would evaluate at runtime. The first build rejects a nonzero
  objective offset, even a constant one; a later proof may admit it without
  changing the harness.
- No `PER`, `WHEN`, reducer qualifier, frame, nested reducer, extra constraint,
  or `DIAGNOSE`. The matcher examines the entire canonical trees, not a
  promising fragment or original SQL text.
- An immutable bound constant capacity whose value is an integer from 0
  through `2^53`, exactly representable in DOUBLE and `BIGINT`. Fractional,
  data-valued, negative, non-finite, volatile, and unproved foldable bounds
  remain solver cases in this slice.
- A numeric, decision-free per-row coefficient. Data-only casts retain their
  SQL meaning; the value used for rank and sign is the value converted to the
  solver's DOUBLE domain. Volatile or side-effecting expressions miss.
  Deterministic expressions that throw are allowed only when the generated
  plan retains their ordinary DuckDB error on every relevant input row.

No source-row uniqueness claim is needed. A row-scoped decision belongs to each
input row even when two rows have identical data. Exact identity and fan-out
must be preserved by the result contract.

## Proposed construction

```text
DECIDE input (including its own WHERE)
  -> evaluate typed coefficient, then convert it to DOUBLE as score
  -> truth-or-error guard for score NULL / non-finite
  -> global ROW_NUMBER in improving score order
  -> project every source row plus an INTEGER 0/1 assignment
  -> shared result boundary with explicit output-slot map
```

For maximization, select positive coefficients among the first `K` descending
ranks; for minimization, select negative coefficients among the first `K`
ascending ranks. Zero and objective-worsening coefficients receive 0. The
upper-bound case remains feasible on empty input and when fewer than `K` rows
improve the objective. Boundary ties may choose different rows with the same
primary optimum. A filtering `ORDER BY ... LIMIT` result is invalid because it
drops the unchosen source rows.

The rule returns one typed value per row and preserves the original result
schema and row count, even when the parent does not read the decision column.
The mandatory guard checks relevant coefficients even at capacity zero and
when all coefficients worsen the objective. The rank or a separate blocking
validator must remain live in those cases; the [experiment](../00_design/experiments.md)
showed that a guard alone can miss a late invalid row under an outer limit.
The implementation must prove the complete zero-capacity assignment, because
the temporary bound-plan probe tested validation there with a fixed rank
threshold of one.
