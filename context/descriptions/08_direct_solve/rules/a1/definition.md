# A1 — Independent bounded decisions

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

For each decision, SQL:

> 1\. computes the effective lower and upper bounds;
>
> 2\. generates the small set of possible optimal values;
>
> 3\. evaluates and ranks those candidates;
>
> 4\. selects one candidate per decision and joins it back to the source rows.

The translation uses ordinary relational operations such as LEAST, GREATEST, UNION ALL, ROW_NUMBER, and joins.

## Correctness

The objective is a sum of independent terms. Improving one decision cannot affect the feasibility or objective contribution of another decision. Therefore, combining the best value for every decision gives a global optimum.

For one bounded linear or quadratic function, the optimum can only occur at an endpoint or at the quadratic turning point. The generated candidate set therefore contains an optimum.

## Eligibility conditions

> • Every component contains exactly one independent BOOL, INT, or REAL decision.
>
> • The objective reduces exactly to a\*x² + b\*x + constant.
>
> • Every constraint reduces to an individual bound, with no shared constraints or cross terms.
>
> • Bounds and coefficients are finite and non-NULL, and integer or strict bounds have been converted to legal endpoints.
>
> • Empty intervals produce DECIDE's infeasible result, while empty input remains empty.
>
> • WHEN, PER, NULL, numeric, tie, decision-identity, and output-mapping semantics are preserved.
