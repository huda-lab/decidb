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
  that the solver would evaluate at runtime. A plan-time constant objective
  offset is ignorable for assignment only after its lack of runtime obligation
  is proved.
- No `PER`, `WHEN`, reducer qualifier, frame, nested reducer, extra constraint,
  or `DIAGNOSE`. The matcher examines the entire canonical trees, not a
  promising fragment or original SQL text.
- A finite, plan-time, non-negative integer capacity exactly representable in
  DOUBLE and the window rank type. The exact range is closed by
  [DES-06](../00_design/todo.md); fractional, data-valued, negative, and
  non-finite bounds remain solver cases in this slice.
- A numeric, decision-free per-row coefficient. Data-only casts retain their
  SQL meaning; the value used for rank and sign is the value converted to the
  solver's DOUBLE domain. Volatile or side-effecting expressions miss. The
  policy for deterministic expressions that can throw belongs to
  [DES-02](../00_design/todo.md).

No source-row uniqueness claim is needed. A row-scoped decision belongs to each
input row even when two rows have identical data. Exact identity and fan-out
must be preserved by the result contract.

## Proposed construction

```text
DECIDE input
  -> evaluate coefficient in DOUBLE domain
  -> mandatory all-relevant-row validation
  -> global ROW_NUMBER in improving coefficient order
  -> project every source row plus an INTEGER 0/1 assignment
  -> shared result boundary
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
when all coefficients worsen the objective. The exact guard and optimizer
placement are design gates, not assumptions supplied by this plan sketch.
