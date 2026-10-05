# A4 — Bounded two-parameter least squares

Catalogue status: Prototype.

> Any assignment with the same objective value is acceptable. Wherever this text describes a stable or deterministic
> tie order, it describes the catalogue's construction, not a requirement on the result.

Exact mathematical translation, but still a prototype because its numeric checks need a complete specification.

## Worked example

A4 fits a straight line:

``` math
y\  = \ intercept\  + \ slope\  \times \ x
```

Both parameters are shared across all observations and have their own bounds.

| **x** | **y** |
|-------|-------|
| 0     | 1     |
| 1     | 3     |
| 2     | 5     |

The best line is y = 1 + 2x.

```sql
SELECT id, intercept, slope
FROM observations
DECIDE scalar intercept(REAL), scalar slope(REAL)
SUCH THAT intercept BETWEEN 0 AND 2
AND slope BETWEEN 0 AND 3
MINIMIZE SUM(POWER(y - (intercept + slope*x), 2));
```

For this example, the unconstrained fit lies inside the bounds:

```sql
WITH fit AS (
SELECT
REGR_INTERCEPT(y, x) AS intercept,
REGR_SLOPE(y, x) AS slope
FROM observations
)
SELECT o.id, f.intercept, f.slope
FROM observations o
CROSS JOIN fit f;
```

The general bounded translation needs additional candidates for the rectangle's edges and corners.

## Recognition

After normalization:

> • there are exactly two shared REAL decisions;
>
> • they represent an intercept and slope;
>
> • the objective is weighted or unweighted squared regression error;
>
> • each parameter has independent finite bounds;
>
> • there are no other decision-dependent terms or constraints.

Unlike A1, the two decisions cannot be optimized separately. Changing the intercept can change the best slope.

Unlike A3, the squared terms allow the optimum to lie inside the rectangle rather than at a corner.

## Supported variants

| **Variant** | **How SQL handles it** |
|----|----|
| Interior optimum | Solve the ordinary least-squares equations and keep the result if both parameters satisfy their bounds. |
| Edge optimum | Fix one parameter at one of its bounds, then solve the remaining one-dimensional problem. There are four edges. |
| Corner optimum | Evaluate the four combinations of parameter bounds. |
| Weighted regression | Use nonnegative weights in all aggregates. The candidate structure stays the same. |
| One fit per entity | Compute and rank candidates separately for each entity or group. |
| Singular data | If the data cannot uniquely determine both parameters, the objective is flat in some direction. SQL uses an explicit edge or corner candidate with the same objective value. |
| Zero total weight | The objective is constant, so SQL returns a canonical legal corner. |

The important distinction is where the optimum lies:

inside rectangle ordinary regression solution

on an edge one parameter is fixed at a bound

at a corner both parameters are fixed at bounds

Simply clamping the unconstrained intercept and slope independently is not valid because the two parameters are correlated.

## Relational construction

For each regression component, SQL:

> 1\. aggregates the regression statistics, such as SUM(weight), SUM(weight\*x), SUM(weight\*y), SUM(weight\*x\*x), and SUM(weight\*x\*y);
>
> 2\. generates the interior regression solution when it exists;
>
> 3\. generates the optimum on each of the four edges;
>
> 4\. adds the four corners;
>
> 5\. evaluates the squared error for every candidate;
>
> 6\. selects the minimum and repeats it across the corresponding source rows.

This uses aggregates, arithmetic expressions, UNION ALL, ranking, and joins.

## Correctness

With nonnegative weights, squared regression error is a convex function over the bounded rectangle.

A minimum must occur:

> • at the stationary point inside the rectangle;
>
> • at a stationary point along one of its edges;
>
> • or at a corner.

The generated candidates cover all three possibilities. Each candidate is either checked against the bounds or constructed directly from them, so every selected candidate is feasible.

## Eligibility conditions

> • Each component has exactly two shared REAL decisions representing intercept and slope.
>
> • The objective is exactly a MINIMIZE weighted least-squares objective with finite, nonnegative weights.
>
> • The only constraints are independent finite bounds on the two parameters.
>
> • SQL evaluates the valid interior candidate, all four edges, and all four corners.
>
> • Zero-weight and singular cases have explicit rules.
>
> • Empty input, inconsistent bounds, ties, decision identity, and output mapping preserve DECIDE semantics.
>
> • Accumulation, near-singular determinant checks, division, containment tests, and objective comparisons follow a DECIDE-compatible numeric policy.

## Prototype limitations

The mathematical candidate set is complete, but the numeric policy is not yet fully specified. Before enabling the rewrite, DeciDB must reliably classify nearly singular systems, decide whether computed candidates lie inside the bounds, and compare nearly equal objective values in a way that matches existing DECIDE semantics. Until those checks can certify the result, the query must continue through the general solver.
