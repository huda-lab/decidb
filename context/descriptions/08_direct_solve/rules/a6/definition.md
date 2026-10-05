# A6 — One squared affine residual over a box

Catalogue status: Exact.

> Any assignment with the same objective value is acceptable. Wherever this text describes a stable or deterministic
> tie order, it describes the catalogue's construction, not a requirement on the result.

## Worked example

A6 applies when several continuous decisions matter only through one weighted sum inside a squared error.

For example:

```sql
SELECT id, x, y
FROM targets
DECIDE x(REAL), y(REAL)
SUCH THAT x BETWEEN 0 AND 4
AND y BETWEEN 0 AND 3
MINIMIZE SUM(POWER(2*x + y - 7, 2));
```

The objective only cares about the combined value:

``` math
z\  = \ 2x\  + \ y
```

The legal ranges imply:

2x can contribute 0 to 8

y can contribute 0 to 3

z can therefore be anywhere from 0 to 11

The target 7 is reachable. One optimal assignment is x = 3.5 and y = 0.

```sql
WITH chosen_sum AS (
SELECT LEAST(11.0, GREATEST(0.0, 7.0)) AS z
)
SELECT
id,
LEAST(4.0, z / 2.0) AS x,
z - 2*LEAST(4.0, z / 2.0) AS y
FROM targets
CROSS JOIN chosen_sum;
```

This SQL first chooses the best combined value, then distributes it across x and y.

## Recognition

After normalization:

> • all decisions are REAL;
>
> • each decision has independent lower and upper bounds;
>
> • the objective reduces to one expression of the form:

weight \* (a₁x₁ + a₂x₂ + ... + aₙxₙ - target)²

> • the weight is positive;
>
> • no other objective term distinguishes assignments with the same weighted sum.

This last condition is essential. Adding -3\*x would make some assignments with the same 2\*x + y better than others, so an arbitrary distribution would no longer be valid.

## Supported variants

| **Variant** | **How it is handled** |
|----|----|
| Reachable target | Construct decisions whose weighted sum equals the target, giving zero residual. |
| Target below the reachable interval | Use the smallest reachable sum. |
| Target above the reachable interval | Use the largest reachable sum. |
| Positive coefficient | Its contribution increases as the decision increases. |
| Negative coefficient | Its contribution interval is reversed, so SQL uses MIN(a\*lo, a\*hi) and MAX(a\*lo, a\*hi). |
| Zero coefficient | The decision does not affect the objective, so SQL chooses a canonical legal value. |
| Several written squares | Allowed only if algebra combines them into one effective square. |
| Independent groups | Compute a separate reachable interval and assignment for every group. |

For example, two squares involving the same affine expression can sometimes be combined:

(z - 3)² + (z - 5)² = 2(z - 4)² + 2

The effective target is 4. The final constant does not affect the chosen decisions.

A6 requires continuous REAL decisions. Integer decisions may leave gaps in the reachable sums, so summing their interval endpoints would incorrectly suggest that every intermediate value is attainable.

## Relational construction

For each component, SQL:

> 1\. rewrites the objective as weight\*(sum - target)²;
>
> 2\. converts every decision interval into its contribution interval;
>
> 3\. sums those intervals to find the complete reachable range;
>
> 4\. clamps the target to that range;
>
> 5\. uses ordered prefix sums to distribute the chosen total across the decisions;
>
> 6\. converts each contribution back to its decision value.

The translation uses LEAST, GREATEST, grouped SUM, windowed prefix sums, CASE, and joins.

## Correctness

A linear expression maps a continuous box to one complete interval. Therefore, every value between the minimum and maximum weighted sums is reachable.

Squared error is minimized by the target when the target is reachable, or by the closest interval endpoint otherwise. The prefix-fill construction produces decisions whose weighted sum equals that chosen value.

Many assignments may produce the same sum. They are all equally optimal because the objective depends only on that sum.

## Eligibility conditions

> • Every decision is REAL and has finite, nonempty bounds.
>
> • The objective reduces exactly to one squared affine residual with a finite positive weight.
>
> • Multiple squares are used only when they reduce algebraically to one effective square.
>
> • No coupled constraint or additional objective term distinguishes assignments with the same affine sum.
>
> • The target is fixed within each component.
>
> • Zero coefficients use a defined canonical legal value.
>
> • Prefix sums are partitioned and ordered by the correct component and decision identities.
>
> • Empty input, infeasible bounds, numeric behavior, ties, and output mapping preserve DECIDE semantics.
