# A1 — Separable box-constrained problem

Catalogue status: Exact.

> Any assignment with the same objective value is acceptable. Wherever this text describes a stable or deterministic
> tie order, it describes the catalogue's construction, not a requirement on the result.

## Worked example

A1 covers problems where every decision can be optimized independently inside its own interval.

For example, choose x as close as possible to a target while respecting 0 ≤ x ≤ capacity:

| **Target** | **Capacity** | **Best x** |
|:----------:|--------------|------------|
|     8      | 12           | 8          |
|     8      | 5            | 5          |
|     -3     | 10           | 0          |

```sql
SELECT id, x
FROM items
DECIDE x(REAL)
SUCH THAT x BETWEEN 0 AND capacity
MINIMIZE SUM(POWER(x - target, 2));
```

Because rows do not interact, SQL can clamp each target directly:

```sql
SELECT
id,
LEAST(capacity, GREATEST(0.0, target)) AS x
FROM items;
```

## Recognition

After normalization, each decision must have:

> • an objective of the form a\*x² + b\*x + constant;
>
> • its own lower and upper bounds;
>
> • no constraint or objective term involving another decision.

A shared constraint such as x + y \<= 10 is not A1 because the choice of x affects the choices available for y.

## Supported variants

| **Variant** | **Direct rule** |
|----|----|
| Linear objective | Compare the two endpoints. |
| Quadratic objective | Compare the endpoints and, when relevant, the turning point -b/(2a). |
| REAL | Use the clipped turning point directly. |
| INT | Check the integers immediately below and above the turning point. |
| BOOL | Compare 0 and 1. |
| MINIMIZE or MAXIMIZE | Rank the candidates in the required direction. |

## Relational construction

The catalogue builds candidates with `UNION ALL`, ranks them with `ROW_NUMBER` and joins the winner back. Because each row
holds its own decisions, none of that is needed: the plan is a chain of projections over the source, each reading only
the one before it.

> 1\. every coefficient, bound and `when` is evaluated on every row its `when` keeps, and nothing on a row it drops;
>
> 2\. each bound clause becomes a lower and an upper bound on its decision, and the objective becomes `a*x² + b*x` per
> decision;
>
> 3\. the bounds are combined into the decision's box, and the box is rounded inward for `INT` and `BOOL`;
>
> 4\. the answer is the clamped turning point, or the better endpoint, computed from `a` and `b`;
>
> 5\. every row is checked before one is released (infeasible, unbounded, invalid data), and the answer is cast to the
> decision's output type.

The translation uses projections, `LEAST`, `GREATEST`, `CASE` and one counting window that holds the rows back. See
[done.md](done.md) for the exact behavior.

## Correctness

The objective is a sum of independent terms. Improving one decision cannot affect the feasibility or objective contribution of another decision. Therefore, combining the best value for every decision gives a global optimum.

For one bounded linear or quadratic function, the optimum can only occur at an endpoint or at the quadratic turning point. The generated candidate set therefore contains an optimum.

## Eligibility conditions

> • Every component contains exactly one independent BOOL, INT, or REAL decision. A query may hold several such
> decisions; each is solved on its own.
>
> • The objective reduces exactly to a\*x² + b\*x + constant.
>
> • Every constraint reduces to an individual bound, with no shared constraints or cross terms.
>
> • Coefficients are finite and non-NULL, and bounds are non-NULL and not NaN. An infinite bound is no bound on that
> side, as on the solver. Integer or strict bounds have been converted to legal endpoints.
>
> • Empty intervals produce DECIDE's infeasible result, while empty input remains empty.
>
> • `when`, `per`, `by`, NULL, numeric, tie, decision-identity, and output-mapping semantics are preserved.

## As built

What the implementation settles beyond the catalogue text:

> • **Default box.** `BOOL` is [0, 1], `INT` and `REAL` are [0, ∞). An unconditional constant lower bound the solver
> folds replaces the default 0, in any spelling of a bound on one decision (`x >= -3`, `2*x >= -6`, D41); every other
> lower bound intersects with it.
>
> • **Bounds.** A clause may carry a coefficient and an offset on the decision, a `when`, and a value that is a
> constant, a numeric or Boolean source column, or any deterministic source expression, including one that can raise.
>
> • **Objective.** `MINIMIZE`, `MAXIMIZE` or none; signed `SUM` parts with constant factors, optional `when`, at most one
> squared term whose inner weight and offset may be row-valued.
>
> • **Declined bound.** A bound the solver rejects before it reads any row (an upper bound below the default floor of 0,
> or a `BOOL` lower bound above 1) is declined, so the solver reports it, on an empty source too.
>
> • **Errors.** An empty box on any row is infeasible; an improving direction with no finite limit on any row is unbounded;
> an `INT` answer beyond 2^53 cannot be returned; NULL, NaN or infinite values raise on every row that reads them, and
> nothing is computed on a row a `when` drops; a term whose `when` keeps no row is dropped, as on the solver (D68).
>
> • **Ties** go to the smaller value, and a decision with no objective term takes the lowest value its box allows.
