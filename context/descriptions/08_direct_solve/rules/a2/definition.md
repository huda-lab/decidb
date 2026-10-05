# A2 — One shared mean, median, midpoint, or mode

Catalogue status: Exact.

> Any assignment with the same objective value is acceptable. Wherever this text describes a stable or deterministic
> tie order, it describes the catalogue's construction, not a requirement on the result.

## Worked example

A2 covers problems where many rows contribute observations, but they all share one decision.

For example, choose one center that best represents these values:

| **Value** |
|-----------|
| 2         |
| 4         |
| 9         |

Under squared error, the best center is their mean: 5.

```sql
SELECT id, center
FROM observations
DECIDE scalar center(REAL)
SUCH THAT center BETWEEN 0 AND 10
MINIMIZE SUM(POWER(center - value, 2));
```

The direct SQL computes the mean, clamps it to the allowed interval, and repeats it on every row:

```sql
WITH answer AS (
SELECT
LEAST(10.0, GREATEST(0.0, AVG(value))) AS center
FROM observations
)
SELECT o.id, a.center
FROM observations o
CROSS JOIN answer a;
```

## Recognition

After normalization:

> • one REAL scalar or entity decision is shared by many rows;
>
> • each row compares that shared decision with an observed value;
>
> • the objective uses one recognized loss;
>
> • constraints on the shared decision reduce to one interval.

The distinction from A1 is simple:

> • A1 gives independent decisions their own answers.
>
> • A2 combines many observations to produce one shared answer.

Independent row decisions do not belong to A2. Applying AVG(value) to them would incorrectly force every row to use the same value.

## Supported variants

| **Loss** | **Best shared value** |
|----|----|
| Squared distance | Weighted mean |
| Absolute distance | Weighted median |
| Maximum absolute distance | Midpoint of the smallest and largest observations |
| L0 distance | Most frequent tolerance-safe value, with safe gaps also considered |

The decision can be:

> • scalar, giving one center for the entire input;
>
> • entity-scoped, giving one center per entity or group.

## Relational construction

For each shared decision, SQL:

> 1\. intersects its bounds using MAX(lower) and MIN(upper);
>
> 2\. aggregates the required statistics;
>
> 3\. computes the mean, median, midpoint, or L0 candidate;
>
> 4\. clamps the result to the legal interval;
>
> 5\. joins the chosen value back to every corresponding source row.

The translation uses aggregates, ordered window functions, grouping, clamping, and joins.

## Correctness

Each supported loss has a known one-dimensional optimum:

> • squared distance is minimized by the mean;
>
> • absolute distance is minimized by a median;
>
> • maximum distance is minimized by the midpoint of the extremes;
>
> • L0 distance is minimized by comparing exact target values and the safe gaps between them.

Bounds are handled by projecting the unconstrained answer onto the legal interval. The resulting value is then repeated for every row sharing that decision.

## Eligibility conditions

> • Each component contains exactly one shared REAL scalar or entity decision.
>
> • The objective is exactly one supported MINIMIZE loss, with no additional decision-dependent term.
>
> • All constraints on the decision reduce to one finite interval.
>
> • L1 and L2 weights are finite and nonnegative, with an explicit zero-total-weight rule.
>
> • The L0 variant uses DECIDE's exact tolerance and examines every tolerance-safe target and gap.
>
> • Values, weights, and bounds are finite and non-NULL.
>
> • Empty input, infeasible bounds, ties, numeric behavior, decision identity, and output mapping preserve DECIDE semantics.
