# R3 — Strictly convex quadratic allocation

Catalogue status: Prototype.

> Any assignment with the same objective value is acceptable. Wherever this text describes a stable or deterministic
> tie order, it describes the catalogue's construction, not a requirement on the result.

Exact mathematical translation, but still a prototype because breakpoint ordering and final optimality checks need a complete numeric specification.

## Worked example

R3 distributes one resource among continuous decisions with strictly convex quadratic penalties.

| **Item** | **Target** | **Allowed range** |
|:--------:|------------|-------------------|
|    A     | 4          | 0 to 10           |
|    B     | 8          | 0 to 10           |

Without the shared limit, the decisions would be 4 and 8. A total limit of 10 requires both values to move down. The optimal result is 3 and 7.

```sql
SELECT id, amount
FROM items
DECIDE amount(REAL)
SUCH THAT amount BETWEEN 0 AND 10
AND SUM(amount) <= 10
MINIMIZE SUM(POWER(amount-target, 2));
```

For this interior example, both decisions receive the same reduction:

```sql
WITH state AS (
SELECT
GREATEST(0.0, (SUM(target)-10.0)/COUNT(*)) AS reduction
FROM items
)
SELECT
id,
LEAST(10.0, GREATEST(0.0, target-reduction)) AS amount
FROM items
CROSS JOIN state;
```

The general case uses one shared multiplier but allows different quadratic curvature, resource coefficients, and active bounds.

## Recognition

After normalization, each independent component has the form:

minimize SUM(0.5\*d_i\*x_i² - y_i\*x_i)

subject to lo_i \<= x_i \<= hi_i

L \<= SUM(a_i\*x_i) \<= U

with strictly positive d_i and a_i.

The objective must be separable and diagonal. A cross-term such as x_i\*x_j, a second shared resource, an integer decision, or nonpositive curvature does not belong to R3.

## Supported variants

| **Variant** | **Handling** |
|----|----|
| Resource constraint inactive | Use each decision's independent clamped quadratic minimum. |
| Upper resource bound active | Increase the shared multiplier until resource use reaches the upper bound. |
| Lower resource bound active | Decrease the multiplier until resource use reaches the lower bound. |
| Exact resource equality | Solve directly for the required shared resource total. |
| Decision reaches a bound | Keep it clamped while the remaining decisions continue responding. |
| All decisions at upper or lower bounds | Emit that bound corner directly. |
| Different curvature or resource coefficients | Each decision responds at its own rate to the same multiplier. |
| Coincident breakpoints | Group their changes before scanning the next multiplier interval. |
| Independent groups | Find one multiplier separately for each proved-disjoint component. |

For a chosen multiplier lambda, every decision has the direct formula:

x_i = CLAMP((y_i-lambda\*a_i)/d_i, lo_i, hi_i)

## Relational construction

SQL:

> 1\. computes the minimum and maximum reachable resource totals;
>
> 2\. checks whether the independent clamped minimizer already satisfies the resource interval;
>
> 3\. selects the violated resource boundary as the target total;
>
> 4\. emits the multiplier value at which each decision enters or leaves its bounds;
>
> 5\. sorts and groups equal multiplier breakpoints;
>
> 6\. scans the intervals where total resource use is an affine function of the multiplier;
>
> 7\. solves for the multiplier inside the interval containing the target total;
>
> 8\. broadcasts that multiplier, clamps every decision, and validates the result.

The final assignment is ordinary SQL arithmetic:

```sql
SELECT
decision_id,
LEAST(hi, GREATEST(lo, (y-lambda*a)/d)) AS x
FROM normalized_items
CROSS JOIN chosen_multiplier;
```

## Correctness

For any fixed multiplier, each decision independently minimizes its quadratic penalty plus the resource price. Increasing the multiplier continuously reduces total resource use. Bound hits divide this response into affine intervals.

The breakpoint scan covers every interval, so it finds a multiplier whose assignment reaches the required feasible boundary. The clamps preserve every decision bound.

The resulting assignment satisfies the stationarity, feasibility, bound, multiplier, and complementary-slackness conditions for a convex problem. These conditions prove global optimality, and strict convexity makes the decision assignment unique.

## Eligibility conditions

> • Every decision is REAL with finite, nonempty bounds.
>
> • The objective is exactly a separable diagonal quadratic with strictly positive curvature and no cross-term or secondary objective.
>
> • There is exactly one resource lower bound, upper bound, equality, or interval with finite, strictly positive coefficients.
>
> • The required resource interval intersects the resource range reachable from the decision boxes.
>
> • Equal breakpoints are grouped before their combined change is applied.
>
> • Empty boxes or unreachable resource requirements produce DECIDE infeasibility.
>
> • WHEN, PER, NULL, decision-identity, and output-mapping semantics are preserved.
>
> • Breakpoint ordering, arithmetic error, final resource use, and all optimality residuals are certified under one DECIDE-compatible numeric policy.

## Prototype limitations

The multiplier construction is mathematically complete, but the numeric policy is not yet fully specified. Before enabling the rewrite, DeciDB must reliably order nearly equal breakpoints and certify the final resource, stationarity, multiplier-sign, and complementary-slackness residuals. If those checks cannot certify the assignment, the query must continue through the general solver.
